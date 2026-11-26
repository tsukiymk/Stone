#include "ArtSettingsConfirmation.h"
#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace stone::windows {
namespace {
struct Handle {
    HANDLE value = nullptr;

    explicit Handle(HANDLE handle = nullptr) : value(handle == INVALID_HANDLE_VALUE ? nullptr : handle)
    {
    }

    ~Handle()
    {
        if (value)
            CloseHandle(value);
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct Configuration {
    std::filesystem::path executable;
    std::wstring arguments;
    uint32_t timeoutMs = 300000;
    std::wstring parentPipe, identifier;
    uint32_t parentId = 0;
    std::shared_ptr<Handle> parent;
};

struct Request {
    Configuration configuration;
    std::string json;
    Handle cancel{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<bool> awaiting{false};
};

std::mutex mutex;
Configuration configuration;
uint64_t nextRequest = 0;
std::unordered_map<uint64_t, std::shared_ptr<Request>> requests;

void Check(bool success, const char* message)
{
    if (!success)
        throw std::runtime_error(message);
}

bool Cancelled(const Request& request)
{
    return WaitForSingleObject(request.cancel.value, 0) != WAIT_TIMEOUT;
}

// The OVERLAPPED and its buffer remain alive until CancelIoEx has completed.
struct Pending {
    HANDLE pipe;
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    OVERLAPPED overlap{};

    explicit Pending(HANDLE file) : pipe(file)
    {
        Check(event.value != nullptr, "could not create pipe event");
        overlap.hEvent = event.value;
    }

    ~Pending()
    {
        CancelIoEx(pipe, &overlap);
        DWORD ignored;
        GetOverlappedResult(pipe, &overlap, &ignored, TRUE);
    }

    DWORD Wait(const Request& request, HANDLE child, ULONGLONG deadline)
    {
        const HANDLE events[]{event.value, request.cancel.value, child};
        const auto now = GetTickCount64();
        Check(now < deadline, "settings confirmation timed out");
        Check(WaitForMultipleObjects(3, events, FALSE, static_cast<DWORD>(deadline - now)) == WAIT_OBJECT_0,
              "settings confirmation cancelled or disconnected");
        DWORD transferred = 0;
        Check(GetOverlappedResult(pipe, &overlap, &transferred, FALSE) != FALSE, "settings pipe operation failed");
        Check(!Cancelled(request), "settings confirmation cancelled");
        return transferred;
    }
};

void Transfer(HANDLE pipe, HANDLE child, const Request& request, ULONGLONG deadline, void* buffer, DWORD size,
              bool write)
{
    auto* bytes = static_cast<unsigned char*>(buffer);
    while (size) {
        Pending pending(pipe);
        DWORD count = 0;
        const auto success = write ? WriteFile(pipe, bytes, size, &count, &pending.overlap)
                                   : ReadFile(pipe, bytes, size, &count, &pending.overlap);
        if (!success) {
            Check(GetLastError() == ERROR_IO_PENDING, "settings pipe disconnected");
            count = pending.Wait(request, child, deadline);
        }
        Check(count > 0 && count <= size && !Cancelled(request), "invalid settings pipe response");
        bytes += count;
        size -= count;
    }
}

struct Child {
    Handle process;
    DWORD id;

    Child(HANDLE handle, DWORD pid) : process(handle), id(pid)
    {
    }

    ~Child()
    {
        if (WaitForSingleObject(process.value, 1000) != WAIT_TIMEOUT)
            return;
        EnumWindows(
            +[](HWND window, LPARAM parameter) -> BOOL {
                DWORD owner = 0;
                GetWindowThreadProcessId(window, &owner);
                if (owner == static_cast<DWORD>(parameter))
                    PostMessageW(window, WM_CLOSE, 0, 0);
                return TRUE;
            },
            static_cast<LPARAM>(id));
        if (WaitForSingleObject(process.value, 1000) == WAIT_TIMEOUT) {
            TerminateProcess(process.value, ERROR_CANCELLED);
            WaitForSingleObject(process.value, 1000);
        }
    }
};

bool RunParent(Request& request)
{
    const auto& config = request.configuration;
    Check(WaitForSingleObject(config.parent->value, 0) == WAIT_TIMEOUT, "settings parent exited");
    Handle pipe(CreateFileW((L"\\\\.\\pipe\\" + config.parentPipe).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                            nullptr));
    Check(pipe.value != nullptr, "could not connect to the settings parent");
    ULONG server = 0;
    Check(GetNamedPipeServerProcessId(pipe.value, &server) && server == config.parentId,
          "unexpected settings parent pipe server");
    std::string identifier;
    for (const auto ch : config.identifier)
        identifier.push_back(static_cast<char>(ch)); // Validated GUID, ASCII only.
    std::string envelope = "{\"format\":1,\"requestId\":\"" + identifier +
                           "\",\"parentProcessId\":" + std::to_string(GetCurrentProcessId()) +
                           ",\"confirmation\":" + request.json + "}";
    Check(envelope.size() <= 65536 && !Cancelled(request), "invalid settings parent request");
    const auto deadline = GetTickCount64() + config.timeoutMs;
    uint32_t size = static_cast<uint32_t>(envelope.size());
    Transfer(pipe.value, config.parent->value, request, deadline, &size, sizeof(size), true);
    Transfer(pipe.value, config.parent->value, request, deadline, envelope.data(), size, true);
    unsigned char result = 0;
    Transfer(pipe.value, config.parent->value, request, deadline, &result, 1, false);
    Check(result <= 1, "invalid settings parent decision");
    unsigned char acknowledgment = 0xac;
    Transfer(pipe.value, config.parent->value, request, deadline, &acknowledgment, 1, true);
    return result == 1 && !Cancelled(request);
}

bool Run(Request& request)
{
    Check(!Cancelled(request), "settings confirmation cancelled");
    if (request.configuration.parent)
        return RunParent(request);
    GUID guid{};
    Check(SUCCEEDED(CoCreateGuid(&guid)), "could not create settings request identity");
    wchar_t guidText[40]{};
    Check(StringFromGUID2(guid, guidText, 40) > 0, "could not format settings request identity");
    const std::wstring identifier(guidText + 1, 36);
    const auto parent = GetCurrentProcessId();
    const auto name = L"Stone.Settings." + std::to_wstring(parent) + L"." + identifier;
    Handle pipe(CreateNamedPipeW(
        (L"\\\\.\\pipe\\" + name).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr));
    Check(pipe.value != nullptr, "could not create settings confirmation pipe");
    Pending connection(pipe.value);
    const bool connected = ConnectNamedPipe(pipe.value, &connection.overlap) != FALSE;
    const auto connectionError = connected ? ERROR_SUCCESS : GetLastError();
    Check(connected || connectionError == ERROR_IO_PENDING || connectionError == ERROR_PIPE_CONNECTED,
          "could not listen for settings");
    std::wstring command = L"\"" + request.configuration.executable.wstring() + L"\" --confirmation-pipe " + name +
                           L" --confirmation-parent " + std::to_wstring(parent) + L" " +
                           request.configuration.arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(request.configuration.executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                         nullptr, &startup, &process) != FALSE,
          "could not launch StoneSettings.exe");
    Child child(process.hProcess, process.dwProcessId);
    Handle thread(process.hThread);
    const auto deadline = GetTickCount64() + request.configuration.timeoutMs;
    if (!connected && connectionError == ERROR_IO_PENDING)
        connection.Wait(request, child.process.value, deadline);
    ULONG client = 0;
    Check(GetNamedPipeClientProcessId(pipe.value, &client) && client == child.id, "unexpected settings pipe client");
    std::string requestId;
    for (const auto ch : identifier)
        requestId.push_back(static_cast<char>(ch)); // GUIDs contain only ASCII.
    std::string envelope = "{\"format\":1,\"requestId\":\"" + requestId +
                           "\",\"parentProcessId\":" + std::to_string(parent) + ",\"confirmation\":" + request.json +
                           "}";
    Check(envelope.size() <= 65536, "settings request too large");
    uint32_t size = static_cast<uint32_t>(envelope.size());
    Transfer(pipe.value, child.process.value, request, deadline, &size, sizeof(size), true);
    Transfer(pipe.value, child.process.value, request, deadline, envelope.data(), size, true);
    unsigned char result = 0;
    Transfer(pipe.value, child.process.value, request, deadline, &result, 1, false);
    Check(result <= 1, "invalid settings confirmation decision");
    unsigned char acknowledgment = 0xac;
    Transfer(pipe.value, child.process.value, request, deadline, &acknowledgment, 1, true);
    return result == 1 && !Cancelled(request);
}
}

void ConfigureArtSettings(const std::filesystem::path& executable, std::wstring arguments, uint32_t timeoutMs)
{
    const std::lock_guard lock(mutex);
    configuration = {executable, std::move(arguments), timeoutMs};
}

void ConfigureArtSettingsParent(const std::filesystem::path& executable, const std::wstring& pipe, uint32_t parent)
{
    const auto prefix = L"Stone.Install." + std::to_wstring(parent) + L".";
    Check(parent && pipe.starts_with(prefix) && pipe.size() == prefix.size() + 36, "invalid settings parent pipe");
    const auto identifier = pipe.substr(prefix.size());
    GUID guid{};
    const auto braced = L"{" + identifier + L"}";
    Check(SUCCEEDED(CLSIDFromString(braced.c_str(), &guid)), "invalid settings parent request identity");
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{sizeof(entry)};
    bool matched = false;
    if (snapshot.value && Process32FirstW(snapshot.value, &entry))
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) {
                matched = entry.th32ParentProcessID == parent;
                break;
            }
        } while (Process32NextW(snapshot.value, &entry));
    Check(matched, "settings confirmation host is not the actual parent");
    auto process =
        std::make_shared<Handle>(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, parent));
    Check(process->value != nullptr, "could not inspect settings parent");
    wchar_t image[32768]{};
    DWORD length = 32768;
    Check(QueryFullProcessImageNameW(process->value, 0, image, &length) &&
              std::filesystem::equivalent(std::filesystem::path(image), executable),
          "settings parent is not the sibling StoneSettings.exe");
    const std::lock_guard lock(mutex);
    configuration = {executable, {}, 300000, pipe, identifier, parent, std::move(process)};
}

bool ArtSettingsEnabled()
{
    const std::lock_guard lock(mutex);
    return !configuration.executable.empty();
}

uint64_t CreateArtSettingsConfirmation(std::string json)
{
    Check(!json.empty() && json.size() <= 64000, "invalid settings confirmation payload");
    auto request = std::make_shared<Request>();
    Check(request->cancel.value != nullptr, "could not create settings cancellation event");
    request->json = std::move(json);
    const std::lock_guard lock(mutex);
    Check(!configuration.executable.empty(), "settings confirmation is not configured");
    request->configuration = configuration;
    const auto id = ++nextRequest;
    requests.emplace(id, std::move(request));
    return id;
}

bool AwaitArtSettingsConfirmation(uint64_t id)
{
    std::shared_ptr<Request> request;
    {
        const std::lock_guard lock(mutex);
        const auto found = requests.find(id);
        if (found == requests.end())
            return false;
        request = found->second;
    }
    if (request->awaiting.exchange(true))
        return false;
    bool accepted = false;
    try {
        accepted = Run(*request);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "[settings] %s\n", error.what());
    }
    {
        const std::lock_guard lock(mutex);
        requests.erase(id);
    }
    return accepted;
}

void CancelArtSettingsConfirmation(uint64_t id)
{
    const std::lock_guard lock(mutex);
    const auto found = requests.find(id);
    if (found != requests.end())
        SetEvent(found->second->cancel.value);
}
}
