#pragma once

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace stone::windows::ipc {
inline constexpr wchar_t ServiceName[] = L"StoneService";
constexpr uint32_t Magic = 0x31425353, Version = 2;
enum class Operation : uint32_t {
    Launch = 1,
    Status = 2,
    Stop = 3,
    Register = 4,
    HostState = 5,
    ShellProfile = 6,
    ShellShortcut = 7,
    NotificationIdentity = 8,
    SystemAwake = 9,
    Jobs = 10,
    PackageRemoved = 11,
    RetireClient = 12,
    ClientProfile = 13,
    ClientStarting = 14,
    Notifications = 15,
    NotificationActivate = 16,
    Clipboard = 17,
    OpenWebUrl = 18,
    ApplicationLogs = 19,
    ApplicationIcon = 20,
    Activities = 21
};

struct Header {
    uint32_t magic = Magic, version = Version;
    Operation operation;
    uint32_t length = 0;
};

struct Response {
    uint32_t magic = Magic, version = Version, result = 1, process = 0, length = 0;
};

static_assert(sizeof(Header) == 16 && sizeof(Response) == 20);

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

struct Identity {
    std::wstring pipe, mutex, security;
    explicit Identity(const std::filesystem::path& directory);
};

void Check(bool value, const char* message);
std::wstring Normalize(const std::filesystem::path& path);
std::wstring OwnDirectory();
bool SameSession(DWORD process);
bool ProcessIs(DWORD process, const std::filesystem::path& executable);
bool Transfer(HANDLE pipe, void* bytes, DWORD size, bool write);
bool Request(const Identity& identity, const std::filesystem::path& directory, Operation operation,
             const std::wstring& text, Response& response, std::wstring& output, const char** failure = nullptr);
void RequestWithStart(const std::filesystem::path& directory, Operation operation, const std::wstring& text,
                      Response& response, std::wstring& output, bool start);
std::vector<std::wstring> Split(const std::wstring& command);
std::wstring Pack(const std::vector<std::wstring>& fields);
}
