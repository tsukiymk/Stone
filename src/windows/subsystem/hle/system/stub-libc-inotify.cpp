#include "stub-libc-inotify.h"
#include "stub-libc.h"
#include "filesystem/AndroidFileObserver.h"
#include "filesystem/AndroidSystemFiles.h"
#include "platform/windows/GuestPaths.h"
#include "../../../host/WindowsPath.h"
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace stone::windows {
namespace {
constexpr int Nonblocking = 0x800, CloseOnExec = 0x80000;
constexpr uint32_t AllEvents = 0xfff, Ignored = 0x8000, Overflow = 0x4000, IsDirectory = 0x40000000;
constexpr uint32_t MaskAdd = 0x20000000, MaskCreate = 0x10000000, OneShot = 0x80000000;
constexpr uint32_t Options = 0x01000000 | 0x02000000 | 0x04000000 | MaskAdd | MaskCreate | OneShot;

struct EventHeader {
    int32_t watch;
    uint32_t mask, cookie, length;
};

static_assert(sizeof(EventHeader) == 16);

struct Event {
    EventHeader header;
    std::string name;

    size_t size() const
    {
        return sizeof(header) + header.length;
    }
};

struct Watch {
    int backend;
    uint32_t mask;
    std::filesystem::path path;
};

struct Instance {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<int, Watch> watches;
    std::deque<Event> events;
    size_t bytes = 0;
    uint64_t generation = 0;
    int flags = 0, nextWatch = 1;
    bool closed = false, overflow = false;

    ~Instance()
    {
        for (const auto& [id, watch] : watches)
            StopAndroidDirectoryWatch(watch.backend);
    }

    void Enqueue(int watch, uint32_t mask, uint32_t cookie, const std::wstring& name)
    {
        if (closed)
            return;
        std::string utf8;
        if (!name.empty()) {
            const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(),
                                                   static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
            if (!count) {
                mask = Overflow;
                watch = -1;
                cookie = 0;
            }
            else {
                utf8.resize(count);
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()),
                                    utf8.data(), count, nullptr, nullptr);
            }
        }
        Event event{{watch, mask, cookie, utf8.empty() ? 0u : static_cast<uint32_t>((utf8.size() + 4) & ~size_t(3))},
                    std::move(utf8)};
        // Keep space for one overflow record; never emit partial records.
        constexpr size_t limit = 1024 * 1024 - sizeof(EventHeader);
        if (events.size() >= 4095 || bytes > limit || event.size() > limit - bytes) {
            if (overflow)
                return;
            event = {{-1, Overflow, 0, 0}, {}};
            overflow = true;
        }
        if (!events.empty()) {
            const auto& last = events.back();
            if (last.header.watch == event.header.watch && last.header.mask == event.header.mask &&
                last.header.cookie == event.header.cookie && last.name == event.name)
                return;
        }
        const auto length = event.size();
        events.push_back(std::move(event));
        bytes += length;
        ++generation;
        changed.notify_all();
    }

    void Revalidate()
    {
        bool revoked = false;
        for (auto it = watches.begin(); it != watches.end();) {
            try {
                CheckAndroidSharedStorageAccess(it->second.path);
                if (!AndroidDirectoryWatchAccessible(it->second.backend))
                    throw AndroidSystemPathError{EACCES};
                ++it;
            }
            catch (const AndroidSystemPathError&) {
                StopAndroidDirectoryWatch(it->second.backend);
                const int id = it->first;
                it = watches.erase(it);
                if (!revoked) {
                    events.clear();
                    bytes = 0;
                    overflow = false;
                    revoked = true;
                }
                Enqueue(id, Ignored, 0, {});
            }
        }
    }
};

struct Descriptor {
    std::shared_ptr<Instance> instance;
    uint64_t id;
    bool closeOnExec;
};

struct Registry {
    std::mutex mutex;
    std::map<int, Descriptor> descriptors;
    uint64_t next = 1;
};

Registry& State()
{
    static auto* state = new Registry;
    return *state;
}

Descriptor Find(int fd)
{
    auto& state = State();
    const std::lock_guard lock(state.mutex);
    const auto it = state.descriptors.find(fd);
    return it == state.descriptors.end() ? Descriptor{} : it->second;
}

void Receive(const std::weak_ptr<Instance>& weak, int id, uint32_t mask, uint32_t cookie, const std::wstring& name)
{
    const auto instance = weak.lock();
    if (!instance)
        return;
    try {
        const std::lock_guard lock(instance->mutex);
        instance->Revalidate();
        const auto it = instance->watches.find(id);
        if (it == instance->watches.end() || instance->closed)
            return;
        const auto selected = mask & it->second.mask & AllEvents;
        if (mask & (Ignored | Overflow))
            instance->Enqueue((mask & Overflow) ? -1 : id, mask, 0, {});
        else if (selected)
            instance->Enqueue(id, selected | (mask & IsDirectory), cookie, name);
        if ((mask & Ignored) || (selected && (it->second.mask & OneShot))) {
            const int backend = it->second.backend;
            instance->watches.erase(it);
            StopAndroidDirectoryWatch(backend);
            if (!(mask & Ignored))
                instance->Enqueue(id, Ignored, 0, {});
        }
    }
    catch (const std::bad_alloc&) {
        // The bounded queue normally prevents this; do not unwind a Windows worker.
    }
}
}

int CreateAndroidInotify(int flags)
{
    if (flags & ~(Nonblocking | CloseOnExec)) {
        errno = EINVAL;
        return -1;
    }
    int fd = -1;
    _sopen_s(&fd, "NUL", _O_RDONLY | _O_BINARY | _O_NOINHERIT, _SH_DENYNO, 0);
    if (fd < 0)
        return -1;
    try {
        auto instance = std::make_shared<Instance>();
        instance->flags = flags & Nonblocking;
        auto& state = State();
        const std::lock_guard lock(state.mutex);
        if (state.descriptors.size() >= 256) {
            _close(fd);
            errno = EMFILE;
            return -1;
        }
        state.descriptors.emplace(fd, Descriptor{std::move(instance), state.next++, (flags & CloseOnExec) != 0});
        return fd;
    }
    catch (const std::bad_alloc&) {
        _close(fd);
        errno = ENOMEM;
        return -1;
    }
}

int AddAndroidInotifyWatch(int fd, const std::filesystem::path& path, uint32_t mask)
{
    const auto instance = Find(fd).instance;
    if (!instance) {
        errno = EBADF;
        return -1;
    }
    if (!(mask & AllEvents) || (mask & ~(AllEvents | Options)) || ((mask & MaskAdd) && (mask & MaskCreate))) {
        errno = EINVAL;
        return -1;
    }
    try {
        CheckAndroidSharedStorageAccess(path);
        std::error_code error;
        const auto status = std::filesystem::status(path, error);
        if (error || !std::filesystem::exists(status)) {
            errno = ENOENT;
            return -1;
        }
        if (!std::filesystem::is_directory(status)) {
            errno = ENOTDIR;
            return -1;
        }
        const std::lock_guard lock(instance->mutex);
        if (instance->closed) {
            errno = EBADF;
            return -1;
        }
        for (auto& [id, watch] : instance->watches) {
            const auto relative = RelativeWindowsGuestPath(path, watch.path);
            if (relative && relative->empty()) {
                if (mask & MaskCreate) {
                    errno = EEXIST;
                    return -1;
                }
                watch.mask = (mask & MaskAdd) ? watch.mask | mask : mask;
                return id;
            }
        }
        if (instance->nextWatch == INT_MAX) {
            errno = ENOSPC;
            return -1;
        }
        const int id = instance->nextWatch++;
        const int backend = WatchAndroidDirectoryPath(
            path, AllEvents,
            [weak = std::weak_ptr(instance), id](int, uint32_t event, uint32_t cookie, const std::wstring& name) {
                Receive(weak, id, event, cookie, name);
            });
        if (backend < 0)
            return -1;
        try {
            instance->watches.emplace(id, Watch{backend, mask, path});
        }
        catch (...) {
            StopAndroidDirectoryWatch(backend);
            throw;
        }
        return id;
    }
    catch (const AndroidSystemPathError& error) {
        errno = error.code;
        return -1;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
    catch (const std::system_error&) {
        errno = EAGAIN;
        return -1;
    }
}

int RemoveAndroidInotifyWatch(int fd, int id)
{
    const auto instance = Find(fd).instance;
    if (!instance) {
        errno = EBADF;
        return -1;
    }
    const std::lock_guard lock(instance->mutex);
    const auto it = instance->watches.find(id);
    if (it == instance->watches.end()) {
        errno = EINVAL;
        return -1;
    }
    StopAndroidDirectoryWatch(it->second.backend);
    instance->watches.erase(it);
    instance->Enqueue(id, Ignored, 0, {});
    return 0;
}

uint64_t AndroidInotifyId(int fd)
{
    return Find(fd).id;
}

uint32_t PollAndroidInotify(int fd, uint64_t id, uint32_t events, uint64_t& generation)
{
    const auto entry = Find(fd);
    if (!entry.instance || entry.id != id)
        return 0;
    const std::lock_guard lock(entry.instance->mutex);
    entry.instance->Revalidate();
    uint32_t ready = entry.instance->events.empty() ? 0 : events & 1;
    if ((events & 0x80000000) && generation == entry.instance->generation)
        ready = 0;
    generation = entry.instance->generation;
    return ready;
}

bool ReadAndroidInotify(int fd, void* buffer, size_t size, intptr_t& result)
{
    const auto instance = Find(fd).instance;
    if (!instance)
        return false;
    result = -1;
    if (size && !buffer) {
        errno = EFAULT;
        return true;
    }
    std::unique_lock lock(instance->mutex);
    for (;;) {
        instance->Revalidate();
        if (instance->closed) {
            errno = EBADF;
            return true;
        }
        if (!instance->events.empty())
            break;
        if (instance->flags & Nonblocking) {
            errno = EAGAIN;
            return true;
        }
        instance->changed.wait_for(lock, std::chrono::milliseconds(50));
        if (PollArm64Signals()) {
            errno = EINTR;
            return true;
        }
    }
    if (size < instance->events.front().size()) {
        errno = EINVAL;
        return true;
    }
    result = 0;
    while (!instance->events.empty() && instance->events.front().size() <= size - static_cast<size_t>(result)) {
        const auto& event = instance->events.front();
        auto* target = static_cast<char*>(buffer) + result;
        std::memcpy(target, &event.header, sizeof(event.header));
        if (event.header.length) {
            std::memset(target + sizeof(event.header), 0, event.header.length);
            std::memcpy(target + sizeof(event.header), event.name.data(), event.name.size());
        }
        result += event.size();
        instance->bytes -= event.size();
        if (event.header.mask & Overflow)
            instance->overflow = false;
        instance->events.pop_front();
    }
    return true;
}

bool CloseAndroidInotify(int fd, int& result)
{
    auto& state = State();
    std::shared_ptr<Instance> instance;
    {
        const std::lock_guard lock(state.mutex);
        const auto it = state.descriptors.find(fd);
        if (it == state.descriptors.end())
            return false;
        instance = it->second.instance;
        state.descriptors.erase(it);
        const bool last = std::none_of(state.descriptors.begin(), state.descriptors.end(), [&](const auto& value) {
            return value.second.instance == instance;
        });
        if (last) {
            const std::lock_guard inner(instance->mutex);
            instance->closed = true;
            for (const auto& [id, watch] : instance->watches)
                StopAndroidDirectoryWatch(watch.backend);
            instance->watches.clear();
            instance->events.clear();
            instance->bytes = 0;
            instance->changed.notify_all();
        }
    }
    ForgetAndroidFileDescriptor(fd);
    result = _close(fd);
    return true;
}

bool DuplicateAndroidInotify(int fd, int minimum, bool closeOnExec, int& result, bool allowed)
{
    auto& state = State();
    const std::lock_guard lock(state.mutex);
    const auto it = state.descriptors.find(fd);
    if (it == state.descriptors.end())
        return false;
    if (!allowed) {
        errno = 95;
        result = -1;
        return true;
    }
    result = DuplicateCrtFd(fd, minimum, !closeOnExec);
    if (result < 0)
        return true;
    try {
        state.descriptors.emplace(result, Descriptor{it->second.instance, state.next++, closeOnExec});
    }
    catch (const std::bad_alloc&) {
        _close(result);
        errno = ENOMEM;
        result = -1;
    }
    return true;
}

bool AndroidInotifyFlags(int fd, int command, int argument, int& result)
{
    auto& state = State();
    const std::lock_guard lock(state.mutex);
    const auto it = state.descriptors.find(fd);
    if (it == state.descriptors.end())
        return false;
    const std::lock_guard inner(it->second.instance->mutex);
    result = 0;
    switch (command) {
    case 1:
        result = it->second.closeOnExec;
        break;
    case 2:
        it->second.closeOnExec = (argument & 1) != 0;
        break;
    case 3:
        result = it->second.instance->flags;
        break;
    case 4:
        it->second.instance->flags = argument & Nonblocking;
        it->second.instance->changed.notify_all();
        break;
    default:
        errno = EINVAL;
        result = -1;
    }
    return true;
}

bool AndroidInotifyAvailable(int fd, int* bytes, int& result)
{
    const auto instance = Find(fd).instance;
    if (!instance)
        return false;
    if (!bytes) {
        errno = EFAULT;
        result = -1;
        return true;
    }
    const std::lock_guard lock(instance->mutex);
    instance->Revalidate();
    *bytes = static_cast<int>(instance->bytes);
    result = 0;
    return true;
}
}
