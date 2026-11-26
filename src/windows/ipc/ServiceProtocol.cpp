#include "ServiceProtocol.h"
#include "../host/CommandLine.h"
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <memory>
#include <stdexcept>

namespace stone::windows::ipc {
namespace fs = std::filesystem;

namespace {
struct ServiceHandle {
    SC_HANDLE value;

    explicit ServiceHandle(SC_HANDLE handle) : value(handle)
    {
    }

    ~ServiceHandle()
    {
        if (value)
            CloseServiceHandle(value);
    }

    ServiceHandle(const ServiceHandle&) = delete;
    ServiceHandle& operator=(const ServiceHandle&) = delete;
};

bool InstalledService(const fs::path& directory)
{
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    Check(manager.value, "Cannot query Windows Service Control Manager");
    ServiceHandle service(OpenServiceW(manager.value, ServiceName, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS));
    if (!service.value) {
        Check(GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST, "Cannot query installed StoneService");
        return false;
    }
    DWORD size = 0;
    QueryServiceConfigW(service.value, nullptr, 0, &size);
    Check(size != 0, "Cannot inspect installed StoneService configuration");
    std::vector<unsigned char> buffer(size);
    auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    Check(QueryServiceConfigW(service.value, config, size, &size), "Cannot read installed StoneService configuration");
    int count = 0;
    auto arguments = CommandLineToArgvW(config->lpBinaryPathName, &count);
    Check(arguments && count, "Invalid installed StoneService command line");
    std::unique_ptr<void, decltype(&LocalFree)> argv(arguments, LocalFree);
    std::error_code error;
    if (!fs::equivalent(arguments[0], directory / L"StoneService.exe", error))
        return false; // An isolated development build has its own user broker.
    SERVICE_STATUS_PROCESS status{};
    Check(QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status),
                               &size),
          "Cannot query installed StoneService status");
    Check(status.dwCurrentState != SERVICE_STOP_PENDING, "StoneService is stopping");
    if (status.dwCurrentState == SERVICE_STOPPED) {
        ServiceHandle starter(OpenServiceW(manager.value, ServiceName, SERVICE_START));
        Check(starter.value, "StoneService is stopped; start it from an elevated Windows service manager");
        Check(StartServiceW(starter.value, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING,
              "Cannot start installed StoneService");
    }
    return true;
}
}

void Check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

std::wstring Normalize(const fs::path& path)
{
    std::error_code error;
    auto normalized = fs::weakly_canonical(fs::absolute(path), error);
    if (error)
        normalized = fs::absolute(path).lexically_normal();
    auto text = normalized.wstring();
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return text;
}

std::wstring OwnDirectory()
{
    std::wstring path(32768, L'\0');
    auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    Check(count && count < path.size(), "Cannot locate Stone service installation");
    return fs::path(path.substr(0, count)).parent_path().wstring();
}

std::wstring UserSid()
{
    HANDLE token = nullptr;
    Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), "Cannot inspect service user");
    Handle owner(token);
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    Check(GetTokenInformation(token, TokenUser, user.data(), size, &size), "Cannot read service user");
    PWSTR text = nullptr;
    Check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &text),
          "Cannot identify service user");
    std::wstring sid(text);
    LocalFree(text);
    return sid;
}

Identity::Identity(const fs::path& directory)
{
    const auto sid = UserSid();
    DWORD session = 0;
    Check(ProcessIdToSessionId(GetCurrentProcessId(), &session), "Cannot identify Windows session");
    auto text = Normalize(directory / L"StoneService.exe") + L"|" + sid + L"|" + std::to_wstring(session);
    std::array<unsigned char, 32> digest{};
    Check(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(text.data()),
                     static_cast<ULONG>(text.size() * sizeof(wchar_t)), digest.data(),
                     static_cast<ULONG>(digest.size())) >= 0,
          "Cannot identify service endpoint");
    std::wstring hash;
    for (auto byte : digest) {
        hash += L"0123456789abcdef"[byte >> 4];
        hash += L"0123456789abcdef"[byte & 15];
    }
    pipe = L"\\\\.\\pipe\\Stone.Service." + hash;
    mutex = L"Local\\Stone.Service." + hash;
    security = L"D:P(A;;GA;;;" + sid + L")";
}

bool Transfer(HANDLE pipe, void* bytes, DWORD size, bool write)
{
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.value)
        return false;
    OVERLAPPED pending{};
    pending.hEvent = event.value;
    DWORD count = 0;
    bool done = (write ? WriteFile(pipe, bytes, size, &count, &pending)
                       : ReadFile(pipe, bytes, size, &count, &pending)) != FALSE;
    if (!done && GetLastError() == ERROR_IO_PENDING) {
        done = WaitForSingleObject(event.value, 5000) == WAIT_OBJECT_0 &&
               GetOverlappedResult(pipe, &pending, &count, FALSE);
        if (!done) {
            CancelIoEx(pipe, &pending);
            GetOverlappedResult(pipe, &pending, &count, TRUE);
        }
    }
    return done && count == size;
}

bool ProcessIs(DWORD pid, const fs::path& executable)
{
    if (!SameSession(pid))
        return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.value)
        return false;
    std::wstring name(32768, L'\0');
    DWORD length = static_cast<DWORD>(name.size());
    if (!QueryFullProcessImageNameW(process.value, 0, name.data(), &length))
        return false;
    std::error_code error;
    return fs::equivalent(fs::path(name.substr(0, length)), executable, error);
}

bool SameSession(DWORD pid)
{
    DWORD own = 0, peer = 0;
    return ProcessIdToSessionId(GetCurrentProcessId(), &own) && ProcessIdToSessionId(pid, &peer) && own == peer;
}

bool Request(const Identity& identity, const fs::path& directory, Operation operation, const std::wstring& text,
             Response& response, std::wstring& output, const char** failure)
{
    if (failure)
        *failure = nullptr;
    auto failed = [failure](const char* stage) {
        if (failure)
            *failure = stage;
        return false;
    };
    Handle pipe;
    const auto deadline = GetTickCount64() + 5000;
    for (;;) {
        pipe.value = CreateFileW(identity.pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe.value != INVALID_HANDLE_VALUE)
            break;
        const auto error = GetLastError();
        pipe.value = nullptr;
        if (error != ERROR_PIPE_BUSY)
            return failed("connect");
        const auto now = GetTickCount64();
        if (now >= deadline)
            return failed("connect timeout");
        // Availability is not a reservation: another caller can win CreateFile
        // after this wait succeeds. Retry only before any request bytes exist.
        if (!WaitNamedPipeW(identity.pipe.c_str(), static_cast<DWORD>(deadline - now)))
            return failed("connect wait");
    }
    ULONG server = 0;
    if (!GetNamedPipeServerProcessId(pipe.value, &server) || !ProcessIs(server, directory / L"StoneService.exe"))
        return failed("server identity");
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr))
        return failed("pipe mode");
    Header header{Magic, Version, operation, static_cast<uint32_t>(text.size())};
    if (!Transfer(pipe.value, &header, sizeof(header), true))
        return failed("write header");
    if (!text.empty() && !Transfer(pipe.value, const_cast<wchar_t*>(text.data()),
                                   static_cast<DWORD>(text.size() * sizeof(wchar_t)), true))
        return failed("write payload");
    if (!Transfer(pipe.value, &response, sizeof(response), false))
        return failed("read response");
    if (response.magic != Magic || response.version != Version || response.length > 32767)
        return failed("invalid response");
    output.resize(response.length);
    if (!output.empty() &&
        !Transfer(pipe.value, output.data(), static_cast<DWORD>(output.size() * sizeof(wchar_t)), false))
        return failed("read payload");
    unsigned char acknowledged = 0xac;
    return Transfer(pipe.value, &acknowledged, 1, true) || failed("acknowledgment");
}

void RequestWithStart(const fs::path& directory, Operation operation, const std::wstring& text, Response& response,
                      std::wstring& output, bool start)
{
    Check(text.size() <= 32767 && text.find(L'\0') == text.npos, "Service request exceeds its text limits");
    Identity identity(directory);
    if (Request(identity, directory, operation, text, response, output))
        return;
    Check(start, "StoneService is not running");
    if (InstalledService(directory)) {
        const auto deadline = GetTickCount64() + 15000;
        do {
            if (Request(identity, directory, operation, text, response, output))
                return;
            Sleep(50);
        } while (GetTickCount64() < deadline);
        throw std::runtime_error("Windows service did not provide a broker for this login session");
    }
    const auto executable = directory / L"StoneService.exe";
    auto command = stone::session::Quote(executable.wstring()) + L" --serve";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         directory.c_str(), &startup, &process),
          "Cannot start service broker");
    CloseHandle(process.hThread);
    Handle server(process.hProcess);
    auto deadline = GetTickCount64() + 10000;
    do {
        if (Request(identity, directory, operation, text, response, output))
            return;
        Sleep(20);
    } while (GetTickCount64() < deadline);
    throw std::runtime_error("Service broker did not become available");
}

std::vector<std::wstring> Split(const std::wstring& command)
{
    Check(command.find(L'\0') == command.npos, "Embedded null in service request");
    int count = 0;
    auto argv = CommandLineToArgvW(command.c_str(), &count);
    Check(argv && count <= 65, "Invalid service request");
    std::vector<std::wstring> result;
    for (int index = 1; index < count; ++index)
        result.emplace_back(argv[index]);
    LocalFree(argv);
    return result;
}

std::wstring Pack(const std::vector<std::wstring>& fields)
{
    std::wstring result = L"Stone";
    for (const auto& field : fields) {
        Check(field.find(L'\0') == field.npos, "Embedded null in service field");
        result += L" " + stone::session::Quote(field);
    }
    Check(result.size() <= 32767, "Service fields exceed the IPC limit");
    return result;
}
}
