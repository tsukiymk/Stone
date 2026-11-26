#include "platform/windows/ArtSettingsConfirmation.h"
#include <windows.h>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace stone::windows;

namespace {
void Check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

void Read(HANDLE pipe, void* buffer, DWORD size)
{
    auto* bytes = static_cast<char*>(buffer);
    while (size) {
        DWORD count = 0;
        Check(ReadFile(pipe, bytes, size, &count, nullptr) && count && count <= size, "peer read failed");
        bytes += count;
        size -= count;
    }
}

std::string Payload()
{
    return "{\"operation\":\"install\",\"packageName\":\"com.trebledot.stone.test\",\"name\":\"Stone "
           "\xe6\xb5\x8b\xe8\xaf\x95 \xf0\x9f\x93\xa6\",\"version\":\"1.0\",\"installer\":\"Stone CIT\",\"verified\":true}";
}

int Peer(int argc, wchar_t** argv)
{
    std::wstring name, mode;
    for (int i = 1; i + 1 < argc; i++) {
        if (std::wstring_view(argv[i]) == L"--confirmation-pipe")
            name = argv[++i];
        else if (std::wstring_view(argv[i]) == L"--peer")
            mode = argv[++i];
    }
    if (mode == L"exit")
        return 0;
    if (mode == L"wrong-client") {
        std::wstring command = L"\"" + std::wstring(argv[0]) + L"\" --confirmation-pipe " + name + L" --peer approve";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        Check(CreateProcessW(argv[0], command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &startup, &process),
              "could not start wrong peer");
        WaitForSingleObject(process.hProcess, 10000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 0;
    }
    HANDLE pipe = CreateFileW((L"\\\\.\\pipe\\" + name).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    Check(pipe != INVALID_HANDLE_VALUE, "peer connect failed");
    if (mode == L"hang") {
        Sleep(INFINITE);
        return 1;
    }
    uint32_t count = 0;
    Read(pipe, &count, sizeof(count));
    Check(count && count <= 65536, "peer frame size");
    std::string json(count, '\0');
    Read(pipe, json.data(), count);
    Check(json.find(Payload()) != std::string::npos, "UTF-8 payload was changed");
    if (mode != L"disconnect") {
        unsigned char result = mode == L"approve" ? 1 : mode == L"cancel" ? 0 : 2;
        DWORD written;
        Check(WriteFile(pipe, &result, 1, &written, nullptr) && written == 1, "peer reply failed");
        if (result <= 1) {
            unsigned char ack = 0;
            Read(pipe, &ack, 1);
            Check(ack == 0xac, "missing peer acknowledgment");
        }
    }
    CloseHandle(pipe);
    return 0;
}

void ParentTransfer(HANDLE pipe, void* buffer, DWORD size, bool write = false)
{
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(event != nullptr, "parent transfer event failed");

    struct Close {
        HANDLE h;

        ~Close()
        {
            CloseHandle(h);
        }
    } cleanup{event};

    auto* bytes = static_cast<unsigned char*>(buffer);
    while (size) {
        OVERLAPPED overlap{};
        overlap.hEvent = event;
        ResetEvent(event);
        DWORD count = 0;
        bool success = (write ? WriteFile(pipe, bytes, size, &count, &overlap)
                              : ReadFile(pipe, bytes, size, &count, &overlap)) != FALSE;
        if (!success && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &overlap);
                GetOverlappedResult(pipe, &overlap, &count, TRUE);
                throw std::runtime_error("parent transfer timeout");
            }
            success = GetOverlappedResult(pipe, &overlap, &count, FALSE) != FALSE;
        }
        Check(success && count && count <= size, "parent transfer failed");
        bytes += count;
        size -= count;
    }
}

void ParentHost(const wchar_t* executable)
{
    for (const std::wstring mode :
         {L"approve", L"cancel", L"malformed", L"disconnect", L"wrong-parent", L"wrong-image"}) {
        const DWORD parent = GetCurrentProcessId() + (mode == L"wrong-parent" ? 1 : 0);
        const auto name = L"Stone.Install." + std::to_wstring(parent) + L".12345678-1234-1234-1234-123456789abc";
        HANDLE pipe = CreateNamedPipeW((L"\\\\.\\pipe\\" + name).c_str(),
                                       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
                                       65536, 0, nullptr);
        Check(pipe != INVALID_HANDLE_VALUE, "parent pipe creation failed");

        struct Close {
            HANDLE h;

            ~Close()
            {
                CloseHandle(h);
            }
        } closePipe{pipe};

        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(event != nullptr, "parent pipe event failed");
        Close closeEvent{event};
        OVERLAPPED connection{};
        connection.hEvent = event;
        Check(!ConnectNamedPipe(pipe, &connection) && GetLastError() == ERROR_IO_PENDING, "parent pipe listen failed");

        struct PendingConnection {
            HANDLE pipe;
            OVERLAPPED& overlap;

            ~PendingConnection()
            {
                CancelIoEx(pipe, &overlap);
                DWORD ignored = 0;
                GetOverlappedResult(pipe, &overlap, &ignored, TRUE);
            }
        } pendingConnection{pipe, connection};

        std::wstring command = L"\"" + std::wstring(executable) + L"\" --parent-peer " + name + L" " +
                               std::to_wstring(parent) + L" " + mode;
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &startup, &child),
              "parent peer launch failed");
        Close childThread{child.hThread};

        struct ChildCleanup {
            HANDLE h;

            ~ChildCleanup()
            {
                if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT)
                    TerminateProcess(h, 99);
                CloseHandle(h);
            }
        } cleanup{child.hProcess};

        HANDLE waits[]{event, child.hProcess};
        const auto outcome = WaitForMultipleObjects(2, waits, FALSE, 5000);
        if (mode == L"wrong-parent" || mode == L"wrong-image") {
            Check(outcome == WAIT_OBJECT_0 + 1, "invalid settings parent connected");
        }
        else {
            Check(outcome == WAIT_OBJECT_0, "settings worker failed to connect");
            DWORD transferred = 0;
            Check(GetOverlappedResult(pipe, &connection, &transferred, FALSE), "parent connection failed");
            ULONG peer = 0;
            Check(GetNamedPipeClientProcessId(pipe, &peer) && peer == child.dwProcessId,
                  "unexpected management worker");
            uint32_t count = 0;
            ParentTransfer(pipe, &count, 4);
            Check(count && count <= 65536, "parent frame length");
            std::string json(count, '\0');
            ParentTransfer(pipe, json.data(), count);
            Check(json.find(Payload()) != json.npos && json.find("12345678-1234-1234-1234-123456789abc") != json.npos,
                  "parent payload identity mismatch");
            if (mode == L"disconnect")
                DisconnectNamedPipe(pipe);
            else {
                unsigned char decision = mode == L"approve" ? 1 : mode == L"cancel" ? 0 : 2;
                ParentTransfer(pipe, &decision, 1, true);
                if (decision <= 1) {
                    unsigned char ack = 0;
                    ParentTransfer(pipe, &ack, 1);
                    Check(ack == 0xac, "parent acknowledgment missing");
                }
            }
        }
        Check(WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0, "parent worker did not exit");
        DWORD result = 99;
        Check(GetExitCodeProcess(child.hProcess, &result) &&
                  result == (mode == L"approve"                                  ? 0u
                             : mode == L"wrong-parent" || mode == L"wrong-image" ? 1u
                                                                                 : 3u),
              "incorrect parent decision");
        std::wcout << L"PASS: existing settings parent " << mode << L'\n';
    }
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc == 5 && std::wstring_view(argv[1]) == L"--parent-peer") {
            auto executable = std::filesystem::absolute(argv[0]);
            if (std::wstring_view(argv[4]) == L"wrong-image")
                executable = executable.parent_path() / L"missing-settings.exe";
            ConfigureArtSettingsParent(executable, argv[2], static_cast<uint32_t>(std::stoul(argv[3])));
            const auto request = CreateArtSettingsConfirmation(Payload());
            const bool accepted = AwaitArtSettingsConfirmation(request);
            Check(!AwaitArtSettingsConfirmation(request), "parent request replay accepted");
            return accepted ? 0 : 3;
        }
        for (int i = 1; i < argc; i++)
            if (std::wstring_view(argv[i]) == L"--peer")
                return Peer(argc, argv);
        if (argc != 1) {
            std::cerr << "Usage: stone-settings-confirmation-tests\n";
            return 2;
        }
        ParentHost(std::filesystem::absolute(argv[0]).c_str());
        for (const auto* mode :
             {L"approve", L"cancel", L"malformed", L"disconnect", L"exit", L"hang", L"wrong-client"}) {
            ConfigureArtSettings(std::filesystem::absolute(argv[0]), std::wstring(L"--peer ") + mode, 2000);
            const auto request = CreateArtSettingsConfirmation(Payload());
            Check(AwaitArtSettingsConfirmation(request) == (std::wstring_view(mode) == L"approve"),
                  "pipe returned an incorrect decision");
            Check(!AwaitArtSettingsConfirmation(request), "request replay was accepted");
            CancelArtSettingsConfirmation(request);
            std::wcout << L"PASS: pipe " << mode << L'\n';
        }
        ConfigureArtSettings(std::filesystem::absolute(argv[0]), L"--peer hang", 60000);
        auto request = CreateArtSettingsConfirmation(Payload());
        auto waiting = std::async(std::launch::async, [request] {
            return AwaitArtSettingsConfirmation(request);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CancelArtSettingsConfirmation(request);
        Check(waiting.wait_for(std::chrono::seconds(5)) == std::future_status::ready && !waiting.get(),
              "cancellation did not unblock pipe");
        request = CreateArtSettingsConfirmation(Payload());
        CancelArtSettingsConfirmation(request);
        Check(!AwaitArtSettingsConfirmation(request), "pre-cancelled confirmation was accepted");
        ConfigureArtSettings(std::filesystem::absolute(argv[0]).parent_path() / "missing-settings.exe");
        Check(!AwaitArtSettingsConfirmation(CreateArtSettingsConfirmation(Payload())),
              "missing settings executable was accepted");
        std::cout << "PASS: active cancellation, pre-cancellation, replay and missing executable\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
