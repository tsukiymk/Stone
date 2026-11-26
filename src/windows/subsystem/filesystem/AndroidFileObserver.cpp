#include "AndroidFileObserver.h"
#include "AndroidSystemFiles.h"
#include "../../host/WindowsPath.h"
#include "platform/windows/GuestPaths.h"
#include <windows.h>
#include <io.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace stone::windows {
namespace {
constexpr int Modify = 2, CloseWrite = 8, MovedFrom = 64, MovedTo = 128, Create = 256, Delete = 512;
constexpr uint32_t Attrib = 4, DeleteSelf = 0x400, MoveSelf = 0x800, Overflow = 0x4000, Ignored = 0x8000,
                   IsDirectory = 0x40000000;
std::atomic<uint32_t> nextCookie{1};

struct Watch {
    HANDLE directory = INVALID_HANDLE_VALUE;
    HANDLE ready = nullptr;
    OVERLAPPED operation{};
    alignas(DWORD) std::array<unsigned char, 16384> buffer{};
    std::filesystem::path path;
    AndroidDirectoryEvent callback;
    std::mutex mutex;
    bool stopped = false;
    uint32_t mask = 0, renameCookie = 0;
    int id = -1;

    ~Watch()
    {
        if (directory != INVALID_HANDLE_VALUE)
            CloseHandle(directory);
        if (ready)
            CloseHandle(ready);
    }

    bool Arm()
    {
        ResetEvent(ready);
        operation = {};
        operation.hEvent = ready;
        return ReadDirectoryChangesExW(directory, buffer.data(), static_cast<DWORD>(buffer.size()), FALSE,
                                       FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                           FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                                           FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY,
                                       nullptr, &operation, nullptr, ReadDirectoryNotifyExtendedInformation) != FALSE;
    }

    void Stop()
    {
        const std::lock_guard lock(mutex);
        stopped = true;
        CancelIoEx(directory, &operation);
    }
};

struct Registry {
    std::mutex mutex;
    std::map<int, std::shared_ptr<Watch>> watches;
    int next = 1;
};

// FileObserver's image-owned daemon lives until process termination.
Registry& Watches()
{
    static auto* registry = new Registry;
    return *registry;
}

void Run(const std::shared_ptr<Watch>& watch)
{
    for (;;) {
        DWORD count = 0;
        // A directory's own rename does not complete ReadDirectoryChangesExW.
        // Track the identity handle as well, so a moved watch cannot keep exposing
        // names from a location outside its original access grant.
        if (WaitForSingleObject(watch->ready, 100) == WAIT_TIMEOUT) {
            bool invalid = false, moved = false;
            {
                std::unique_lock lock(watch->mutex);
                if (watch->stopped) {
                    lock.unlock();
                    DWORD ignored;
                    GetOverlappedResult(watch->directory, &watch->operation, &ignored, TRUE);
                    return;
                }
                std::wstring current(32768, L'\0');
                const auto length = GetFinalPathNameByHandleW(watch->directory, current.data(),
                                                              static_cast<DWORD>(current.size()), FILE_NAME_NORMALIZED);
                if (length && length < current.size()) {
                    current.resize(length);
                    const auto relative = RelativeWindowsGuestPath(current, watch->path);
                    moved = !relative || !relative->empty();
                }
                try {
                    CheckAndroidSharedStorageAccess(watch->path);
                }
                catch (const AndroidSystemPathError&) {
                    invalid = true;
                }
                if (moved || invalid) {
                    watch->stopped = true;
                    CancelIoEx(watch->directory, &watch->operation);
                }
            }
            if (!moved && !invalid)
                continue;
            DWORD ignored;
            GetOverlappedResult(watch->directory, &watch->operation, &ignored, TRUE);
            if (moved && (watch->mask & MoveSelf))
                watch->callback(watch->id, MoveSelf, 0, {});
            watch->callback(watch->id, Ignored, 0, {});
            auto& registry = Watches();
            const std::lock_guard lock(registry.mutex);
            registry.watches.erase(watch->id);
            return;
        }
        const bool completed = GetOverlappedResult(watch->directory, &watch->operation, &count, TRUE) != FALSE;
        const auto error = completed ? ERROR_SUCCESS : GetLastError();

        struct Event {
            uint32_t mask, cookie;
            std::wstring name;
        };

        std::vector<Event> events;
        {
            const std::lock_guard lock(watch->mutex);
            if (watch->stopped)
                return;
            if (!completed && error != ERROR_NOTIFY_ENUM_DIR) {
                watch->stopped = true;
                if (error == ERROR_ACCESS_DENIED || error == ERROR_PATH_NOT_FOUND || error == ERROR_DELETE_PENDING)
                    if (watch->mask & DeleteSelf)
                        events.push_back({DeleteSelf, 0, {}});
                events.push_back({Ignored, 0, {}});
            }
            else if (!count)
                events.push_back({Overflow, 0, {}});
            size_t offset = 0;
            constexpr auto header = offsetof(FILE_NOTIFY_EXTENDED_INFORMATION, FileName);
            while (offset + header <= count) {
                const auto* event =
                    reinterpret_cast<const FILE_NOTIFY_EXTENDED_INFORMATION*>(watch->buffer.data() + offset);
                if (event->FileNameLength % sizeof(wchar_t) || event->FileNameLength > count - offset - header)
                    break;
                int mask = 0;
                switch (event->Action) {
                case FILE_ACTION_ADDED:
                    mask = Create;
                    break;
                case FILE_ACTION_REMOVED:
                    mask = Delete;
                    break;
                case FILE_ACTION_MODIFIED:
                    mask = Modify | Attrib;
                    break;
                case FILE_ACTION_RENAMED_OLD_NAME:
                    mask = MovedFrom;
                    watch->renameCookie = nextCookie.fetch_add(1);
                    if (!watch->renameCookie)
                        watch->renameCookie = nextCookie.fetch_add(1);
                    break;
                case FILE_ACTION_RENAMED_NEW_NAME:
                    mask = MovedTo;
                    break;
                }
                const uint32_t cookie = (mask & (MovedFrom | MovedTo)) ? watch->renameCookie : 0;
                if (mask & watch->mask)
                    events.push_back(
                        {(mask & watch->mask) | ((event->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? IsDirectory : 0),
                         cookie, std::wstring(event->FileName, event->FileNameLength / sizeof(wchar_t))});
                if (mask == MovedTo)
                    watch->renameCookie = 0;
                if (!event->NextEntryOffset || event->NextEntryOffset > count - offset)
                    break;
                offset += event->NextEntryOffset;
            }
            if (!watch->stopped && !watch->Arm()) {
                const auto failure = GetLastError();
                watch->stopped = true;
                if ((watch->mask & DeleteSelf) && (failure == ERROR_ACCESS_DENIED || failure == ERROR_PATH_NOT_FOUND ||
                                                   failure == ERROR_DELETE_PENDING))
                    events.push_back({DeleteSelf, 0, {}});
                events.push_back({Ignored, 0, {}});
            }
        }
        for (const auto& event : events)
            watch->callback(watch->id, event.mask, event.cookie, event.name);
        {
            const std::lock_guard lock(watch->mutex);
            if (!watch->stopped)
                continue;
        }
        auto& registry = Watches();
        const std::lock_guard lock(registry.mutex);
        registry.watches.erase(watch->id);
        return;
    }
}
}

int WatchAndroidDirectory(const std::wstring& guestPath, int mask, AndroidFileEvent callback)
{
    const auto guest = AndroidSystemGuestPath(guestPath);
    if (!guest)
        return -1;
    std::filesystem::path path;
    try {
        path = ResolveAndroidSystemFile(*guest);
    }
    catch (const AndroidSystemPathError&) {
        return -1;
    }
    return WatchAndroidDirectoryPath(
        path, mask, [callback = std::move(callback)](int id, uint32_t event, uint32_t, const std::wstring& name) {
            callback(id, static_cast<int>(event), name);
        });
}

int WatchAndroidDirectoryPath(const std::filesystem::path& path, uint32_t mask, AndroidDirectoryEvent callback)
{
    auto watch = std::make_shared<Watch>();
    watch->path = ExtendedWindowsPath(std::filesystem::canonical(path));
    watch->mask = mask;
    watch->callback = std::move(callback);
    watch->directory =
        CreateFileW(path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (watch->directory == INVALID_HANDLE_VALUE) {
        errno = GetLastError() == ERROR_ACCESS_DENIED ? EACCES : ENOENT;
        return -1;
    }
    watch->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!watch->ready) {
        errno = ENOMEM;
        return -1;
    }
    auto& registry = Watches();
    const std::lock_guard lock(registry.mutex);
    if (registry.watches.size() >= 256 || registry.next == INT_MAX) {
        errno = ENOSPC;
        return -1;
    }
    if (!watch->Arm()) {
        errno = 95;
        return -1;
    }
    const int id = registry.next++;
    watch->id = id;
    registry.watches.emplace(id, watch);
    try {
        std::thread([watch] {
            Run(watch);
        }).detach();
    }
    catch (...) {
        watch->Stop();
        DWORD ignored;
        GetOverlappedResult(watch->directory, &watch->operation, &ignored, TRUE);
        registry.watches.erase(id);
        throw;
    }
    return id;
}

void StopAndroidDirectoryWatch(int id)
{
    auto& registry = Watches();
    std::shared_ptr<Watch> watch;
    {
        const std::lock_guard lock(registry.mutex);
        const auto found = registry.watches.find(id);
        if (found == registry.watches.end())
            return;
        watch = std::move(found->second);
        registry.watches.erase(found);
    }
    watch->Stop();
}

bool AndroidDirectoryWatchAccessible(int id)
{
    auto& registry = Watches();
    std::shared_ptr<Watch> watch;
    {
        const std::lock_guard lock(registry.mutex);
        const auto it = registry.watches.find(id);
        if (it == registry.watches.end())
            return true;
        watch = it->second;
    }
    const std::lock_guard lock(watch->mutex);
    std::wstring path(32768, L'\0');
    const auto length =
        GetFinalPathNameByHandleW(watch->directory, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED);
    try {
        CheckAndroidSharedStorageAccess(watch->path);
        if (length && length < path.size()) {
            path.resize(length);
            CheckAndroidSharedStorageTarget(watch->path, path);
        }
        return true;
    }
    catch (const AndroidSystemPathError&) {
        return false;
    }
}

std::filesystem::path AndroidWritableFileClosing(int fd)
{
    auto& registry = Watches();
    {
        const std::lock_guard lock(registry.mutex);
        if (registry.watches.empty())
            return {};
    }
    const auto raw = _get_osfhandle(fd);
    if (raw == -1 || raw == -2)
        return {};
    HANDLE handle = reinterpret_cast<HANDLE>(raw);
    if (GetFileType(handle) != FILE_TYPE_DISK)
        return {};

    struct AccessInformation {
        ACCESS_MASK flags;
    } access{};

    // Native FileAccessInformation (8) reports the actual access granted to the
    // handle, including descriptors imported without a CRT open-mode record.
    using QueryInformation = LONG(NTAPI*)(HANDLE, void*, void*, ULONG, ULONG);
    const auto query =
        reinterpret_cast<QueryInformation>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile"));

    struct Status {
        union {
            LONG status;
            void* pointer;
        };

        ULONG_PTR information;
    } status{};

    if (!query || query(handle, &status, &access, sizeof(access), 8) < 0 ||
        !(access.flags & (FILE_WRITE_DATA | FILE_APPEND_DATA)))
        return {};
    std::wstring path(32768, L'\0');
    const auto count =
        GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED);
    if (!count || count >= path.size())
        return {};
    path.resize(count);
    return std::filesystem::path(path);
}

void NotifyAndroidFileClosed(const std::filesystem::path& path)
{
    if (path.empty())
        return;
    auto& registry = Watches();
    std::vector<std::shared_ptr<Watch>> matching;
    {
        const std::lock_guard lock(registry.mutex);
        for (const auto& [id, watch] : registry.watches)
            if ((watch->mask & CloseWrite) &&
                CompareStringOrdinal(watch->path.c_str(), -1, path.parent_path().c_str(), -1, TRUE) == CSTR_EQUAL)
                matching.push_back(watch);
    }
    for (const auto& watch : matching) {
        bool stopped;
        {
            const std::lock_guard lock(watch->mutex);
            stopped = watch->stopped;
        }
        if (!stopped)
            watch->callback(watch->id, CloseWrite, 0, path.filename().wstring());
    }
}
}
