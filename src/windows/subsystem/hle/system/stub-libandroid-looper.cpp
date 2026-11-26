#include "stub-libc.h"
#include "abi/AndroidLibraryImports.h"
#include "stub-libc-sockets.h"
#include "stub-libc-inotify.h"
#include "filesystem/AndroidFileObserver.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace stone::windows {
namespace {
constexpr int PollWake = -1, PollCallback = -2, PollTimeout = -3, PollError = -4;
constexpr int Input = 1, Output = 4, AbsoluteTime = 1, CloseOnExec = 0x80000, Nonblocking = 0x800;

struct Timer {
    explicit Timer(HANDLE event, bool nonblocking) : event(event), nonblocking(nonblocking)
    {
    }

    ~Timer()
    {
        CloseHandle(event);
    }

    HANDLE event;
    bool nonblocking;
};

std::mutex timerMutex;
std::unordered_map<int, std::shared_ptr<Timer>> timers;

constexpr uint64_t MaxCounter = UINT64_MAX - 1;

struct EventCounter {
    EventCounter(unsigned initial, int flags)
        : event(CreateEventW(nullptr, TRUE, initial != 0, nullptr)), value(initial), semaphore((flags & 1) != 0),
          flags(flags)
    {
    }

    ~EventCounter()
    {
        if (event)
            CloseHandle(event);
    }

    const HANDLE event;
    std::mutex mutex;
    std::condition_variable changed;
    uint64_t value, readGeneration = 0, writeGeneration = 0;
    const bool semaphore;
    int flags;
};

std::unordered_map<int, std::shared_ptr<EventCounter>> eventCounters;

#pragma pack(push, 1)

struct EpollEvent {
    uint32_t events;
    uint64_t data;
};

#pragma pack(pop)
static_assert(sizeof(EpollEvent) == 12);

struct Epoll {
    struct Registration {
        EpollEvent event;
        uint64_t socket;
        std::shared_ptr<EventCounter> counter;
        uint64_t inotify = 0;
        uint32_t previous = 0;
        uint64_t readGeneration = 0, writeGeneration = 0;
    };

    std::mutex mutex;
    std::unordered_map<int, Registration> registrations;
};

std::unordered_map<int, std::shared_ptr<Epoll>> epolls;

int ReserveFd()
{
    // Keep the guest descriptor distinct from the CRT's live file descriptors.
    int fd = -1;
    _sopen_s(&fd, "NUL", _O_RDONLY | _O_BINARY | _O_NOINHERIT, _SH_DENYNO, 0);
    return fd;
}

std::shared_ptr<EventCounter> FindEventCounter(int fd)
{
    const std::lock_guard lock(timerMutex);
    const auto it = eventCounters.find(fd);
    return it == eventCounters.end() ? nullptr : it->second;
}

int CreateEventCounter(unsigned initial, int flags)
{
    constexpr int Semaphore = 1;
    if (flags & ~(Semaphore | Nonblocking | CloseOnExec)) {
        errno = EINVAL;
        return -1;
    }
    const int fd = ReserveFd();
    if (fd < 0)
        return -1;
    try {
        auto counter = std::make_shared<EventCounter>(initial, flags);
        if (!counter->event) {
            _close(fd);
            errno = EIO;
            return -1;
        }
        const std::lock_guard lock(timerMutex);
        eventCounters.emplace(fd, std::move(counter));
        return fd;
    }
    catch (const std::bad_alloc&) {
        _close(fd);
        errno = ENOMEM;
        return -1;
    }
}

intptr_t TransferEventCounter(EventCounter& counter, void* read, const void* write, size_t size)
{
    if (size < sizeof(uint64_t)) {
        errno = EINVAL;
        return -1;
    }
    if (!read && !write) {
        errno = EFAULT;
        return -1;
    }
    uint64_t amount = 0;
    if (write) {
        std::memcpy(&amount, write, sizeof(amount));
        if (amount > MaxCounter) {
            errno = EINVAL;
            return -1;
        }
    }
    std::unique_lock lock(counter.mutex);
    const auto ready = [&] {
        return write ? amount <= MaxCounter - counter.value : counter.value != 0;
    };
    if (!ready() && (counter.flags & Nonblocking)) {
        errno = EAGAIN;
        return -1;
    }
    counter.changed.wait(lock, ready);
    if (write) {
        counter.value += amount;
        if (amount)
            ++counter.readGeneration;
    }
    else {
        amount = counter.semaphore ? 1 : counter.value;
        if (counter.value == MaxCounter)
            ++counter.writeGeneration;
        counter.value -= amount;
        std::memcpy(read, &amount, sizeof(amount));
    }
    if (counter.value)
        SetEvent(counter.event);
    else
        ResetEvent(counter.event);
    lock.unlock();
    counter.changed.notify_all();
    return sizeof(uint64_t);
}

uint32_t EventCounterReadiness(const EventCounter& counter)
{
    return (counter.value ? Input : 0) | (counter.value < MaxCounter ? Output : 0);
}

std::shared_ptr<Timer> FindTimer(int fd)
{
    const std::lock_guard lock(timerMutex);
    const auto it = timers.find(fd);
    return it == timers.end() ? nullptr : it->second;
}

int CreateTimer(int clock, int flags)
{
    if (clock != 1 || (flags & ~(CloseOnExec | Nonblocking))) {
        errno = EINVAL;
        return -1;
    }
    HANDLE event = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    if (!event) {
        errno = ENOMEM;
        return -1;
    }
    const int fd = ReserveFd();
    if (fd < 0) {
        CloseHandle(event);
        return -1;
    }
    try {
        auto timer = std::make_shared<Timer>(event, (flags & Nonblocking) != 0);
        event = nullptr;
        const std::lock_guard lock(timerMutex);
        timers.emplace(fd, std::move(timer));
        return fd;
    }
    catch (const std::bad_alloc&) {
        if (event)
            CloseHandle(event);
        _close(fd);
        errno = ENOMEM;
        return -1;
    }
}

struct TimerSpec {
    AndroidTimespec interval, value;
};

int SetTimer(int fd, int flags, const TimerSpec* spec, TimerSpec* old)
{
    auto timer = FindTimer(fd);
    if (!timer) {
        errno = EBADF;
        return -1;
    }
    if (!spec || (flags & ~AbsoluteTime) || spec->value.seconds < 0 || spec->value.nanos < 0 ||
        spec->value.nanos >= 1000000000) {
        errno = EINVAL;
        return -1;
    }
    if (old || spec->interval.seconds < 0 || spec->interval.nanos < 0 || spec->interval.nanos >= 1000000000) {
        errno = EINVAL;
        return -1;
    }
    if (spec->value.seconds == 0 && spec->value.nanos == 0) {
        // Reset the signaled state as well as cancelling any pending deadline.
        LARGE_INTEGER future{};
        future.QuadPart = std::numeric_limits<int64_t>::max();
        if (SetWaitableTimer(timer->event, &future, 0, nullptr, nullptr, FALSE) && CancelWaitableTimer(timer->event))
            return 0;
        errno = EINVAL;
        return -1;
    }
    AndroidTimespec now{};
    if (flags & AbsoluteTime)
        AndroidClock(1, &now);
    const auto seconds =
        std::clamp<int64_t>(spec->value.seconds - now.seconds, -1, std::numeric_limits<int64_t>::max() / 10000000 - 1);
    const auto ticks = std::max<int64_t>(1, seconds * 10000000 + (spec->value.nanos - now.nanos + 99) / 100);
    LARGE_INTEGER due{};
    due.QuadPart = -ticks;
    const auto period = std::min<int64_t>(LONG_MAX, std::min<int64_t>(spec->interval.seconds, LONG_MAX / 1000) * 1000 +
                                                        (spec->interval.nanos + 999999) / 1000000);
    if (SetWaitableTimer(timer->event, &due, static_cast<LONG>(period), nullptr, nullptr, FALSE))
        return 0;
    errno = EINVAL;
    return -1;
}

struct Registration {
    int fd, ident;
    uintptr_t callback;
    void* data;
    std::shared_ptr<Timer> timer;
    std::shared_ptr<EventCounter> counter;
};

struct Looper {
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::mutex mutex;
    std::vector<Registration> registrations;
    bool allowNonCallbacks;
    bool skipEmptyZeroTimeout = false;
    unsigned refs = 1;

    explicit Looper(int flags) : allowNonCallbacks((flags & 1) != 0)
    {
    }

    ~Looper()
    {
        if (wake)
            CloseHandle(wake);
    }
};

void ReleaseLooper(Looper* looper)
{
    if (!looper)
        return;
    std::unique_lock lock(looper->mutex);
    if (--looper->refs)
        return;
    lock.unlock();
    delete looper;
}

thread_local std::unique_ptr<Looper, decltype(&ReleaseLooper)> threadLooper(nullptr, ReleaseLooper);

Looper* PrepareLooper(int flags)
{
    if (!threadLooper) {
        auto looper = std::unique_ptr<Looper>(new (std::nothrow) Looper(flags));
        if (looper && looper->wake)
            threadLooper.reset(looper.release());
    }
    return threadLooper.get();
}

int AddFd(Looper* looper, int fd, int ident, int events, uintptr_t callback, void* data)
{
    auto timer = FindTimer(fd);
    auto counter = timer ? nullptr : FindEventCounter(fd);
    if (!looper || (!timer && !counter) || events != Input || (!callback && (ident < 0 || !looper->allowNonCallbacks)))
        return -1;
    const std::lock_guard lock(looper->mutex);
    auto& list = looper->registrations;
    const auto it = std::ranges::find(list, fd, &Registration::fd);
    Registration registration{fd, ident, callback, data, std::move(timer), std::move(counter)};
    if (it != list.end())
        *it = std::move(registration);
    else {
        if (list.size() >= MAXIMUM_WAIT_OBJECTS - 1)
            return -1;
        try {
            list.push_back(std::move(registration));
        }
        catch (const std::bad_alloc&) {
            return -1;
        }
    }
    SetEvent(looper->wake);
    return 1;
}

int RemoveFd(Looper* looper, int fd)
{
    const std::lock_guard lock(looper->mutex);
    auto& list = looper->registrations;
    const auto it = std::ranges::find(list, fd, &Registration::fd);
    if (it == list.end())
        return 0;
    list.erase(it);
    SetEvent(looper->wake);
    return 1;
}

int PollOnce(int timeout, int* fd, int* events, void** data)
{
    if (fd)
        *fd = 0;
    if (events)
        *events = 0;
    if (data)
        *data = nullptr;
    const auto looper = threadLooper.get();
    if (!looper)
        return PollError;
    std::vector<Registration> registrations;
    bool skipEmptyZeroTimeout = false;
    try {
        const std::lock_guard lock(looper->mutex);
        registrations = looper->registrations;
        skipEmptyZeroTimeout = looper->skipEmptyZeroTimeout;
    }
    catch (const std::bad_alloc&) {
        return PollError;
    }
    if (timeout == 0 && registrations.empty() && skipEmptyZeroTimeout) {
        const auto ready = WaitForSingleObject(looper->wake, 0);
        return ready == WAIT_OBJECT_0 ? PollWake : ready == WAIT_TIMEOUT ? PollTimeout : PollError;
    }
    HANDLE handles[MAXIMUM_WAIT_OBJECTS]{looper->wake};
    for (size_t i = 0; i < registrations.size(); ++i) {
        const auto& entry = registrations[i];
        handles[i + 1] = entry.timer ? entry.timer->event : entry.counter->event;
    }
    const auto count = static_cast<DWORD>(registrations.size() + 1);
    const DWORD result =
        WaitForMultipleObjects(count, handles, FALSE, timeout < 0 ? INFINITE : static_cast<DWORD>(timeout));
    if (result == WAIT_TIMEOUT)
        return PollTimeout;
    if (result == WAIT_OBJECT_0)
        return PollWake;
    if (result >= WAIT_OBJECT_0 + count)
        return PollError;
    const auto& entry = registrations[result - WAIT_OBJECT_0 - 1];
    if (entry.callback) {
        if (!CallAndroidCallback(entry.callback, reinterpret_cast<void*>(static_cast<intptr_t>(entry.fd)),
                                 reinterpret_cast<void*>(static_cast<intptr_t>(Input)), entry.data))
            RemoveFd(looper, entry.fd);
        return PollCallback;
    }
    if (fd)
        *fd = entry.fd;
    if (events)
        *events = Input;
    if (data)
        *data = entry.data;
    return entry.ident;
}

int PollAll(int timeout, int* fd, int* events, void** data)
{
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(timeout);
    while (true) {
        const int result = PollOnce(timeout, fd, events, data);
        if (result != PollCallback)
            return result;
        if (timeout <= 0)
            continue;
        const auto remaining = ceil<milliseconds>(deadline - steady_clock::now());
        if (remaining.count() <= 0)
            return PollTimeout;
        timeout = static_cast<int>(remaining.count());
    }
}

std::shared_ptr<Epoll> FindEpoll(int fd)
{
    const std::lock_guard lock(timerMutex);
    const auto it = epolls.find(fd);
    return it == epolls.end() ? nullptr : it->second;
}

int CreateEpoll(int flags)
{
    if (flags & ~CloseOnExec) {
        errno = EINVAL;
        return -1;
    }
    const int fd = ReserveFd();
    if (fd < 0)
        return -1;
    try {
        auto epoll = std::make_shared<Epoll>();
        const std::lock_guard lock(timerMutex);
        epolls.emplace(fd, std::move(epoll));
        return fd;
    }
    catch (const std::bad_alloc&) {
        _close(fd);
        errno = ENOMEM;
        return -1;
    }
}

int ControlEpoll(int epollFd, int operation, int fd, const EpollEvent* event)
{
    const auto epoll = FindEpoll(epollFd);
    if (!epoll || fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (operation < 1 || operation > 3 || (operation != 2 && !event)) {
        errno = EINVAL;
        return -1;
    }
    const auto socket = AndroidSocketId(fd);
    const auto timer = FindTimer(fd);
    const auto counter = FindEventCounter(fd);
    const auto inotify = AndroidInotifyId(fd);
    if (operation != 2 && !timer && !socket && !counter && !inotify &&
        GetFileType(reinterpret_cast<HANDLE>(_get_osfhandle(fd))) != FILE_TYPE_PIPE) {
        errno = EPERM;
        return -1;
    }
    const std::lock_guard lock(epoll->mutex);
    auto& regs = epoll->registrations;
    auto it = regs.find(fd);
    if (it != regs.end() &&
        ((it->second.socket && it->second.socket != socket) || (it->second.counter && it->second.counter != counter) ||
         (it->second.inotify && it->second.inotify != inotify))) {
        regs.erase(it);
        it = regs.end();
    }
    if ((it == regs.end()) != (operation == 1)) {
        errno = operation == 1 ? EEXIST : ENOENT;
        return -1;
    }
    try {
        if (operation == 1)
            regs.emplace(fd, Epoll::Registration{*event, socket, counter, inotify});
        else if (operation == 2)
            regs.erase(it);
        else
            it->second = {*event, socket, counter, inotify};
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int WaitEpoll(int epollFd, EpollEvent* events, int maxEvents, int timeout)
{
    const auto epoll = FindEpoll(epollFd);
    if (!epoll) {
        errno = EBADF;
        return -1;
    }
    if (!events || maxEvents < 1) {
        errno = EINVAL;
        return -1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    for (;;) {
        int count = 0;
        {
            const std::lock_guard lock(epoll->mutex);
            for (auto it = epoll->registrations.begin(); it != epoll->registrations.end();) {
                auto& [fd, registration] = *it;
                const auto& event = registration.event;
                uint32_t ready = 0;
                if (registration.socket) {
                    if (AndroidSocketId(fd) != registration.socket) {
                        it = epoll->registrations.erase(it);
                        continue;
                    }
                    ready = PollAndroidSocket(fd, registration.socket, event.events, registration.previous,
                                              registration.readGeneration, registration.writeGeneration);
                }
                else if (registration.inotify) {
                    if (AndroidInotifyId(fd) != registration.inotify) {
                        it = epoll->registrations.erase(it);
                        continue;
                    }
                    ready = PollAndroidInotify(fd, registration.inotify, event.events, registration.readGeneration);
                    if (ready && (event.events & 0x40000000))
                        registration.event.events = 0;
                }
                else if (const auto& counter = registration.counter) {
                    if (FindEventCounter(fd) != counter) {
                        it = epoll->registrations.erase(it);
                        continue;
                    }
                    constexpr uint32_t Edge = 0x80000000, OneShot = 0x40000000;
                    const std::lock_guard counterLock(counter->mutex);
                    ready = EventCounterReadiness(*counter) & event.events;
                    if (event.events & Edge) {
                        if (registration.readGeneration != counter->readGeneration)
                            registration.previous &= ~Input;
                        if (registration.writeGeneration != counter->writeGeneration)
                            registration.previous &= ~Output;
                        registration.readGeneration = counter->readGeneration;
                        registration.writeGeneration = counter->writeGeneration;
                        const auto current = ready;
                        ready &= ~registration.previous;
                        registration.previous = current;
                    }
                    if (ready && (event.events & OneShot))
                        registration.event.events = 0;
                }
                else if (auto timer = FindTimer(fd)) {
                    if (WaitForSingleObject(timer->event, 0) == WAIT_OBJECT_0)
                        ready = event.events & Input;
                }
                else {
                    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
                    DWORD bytes = 0;
                    if (PeekNamedPipe(handle, nullptr, 0, nullptr, &bytes, nullptr)) {
                        if (bytes > 0)
                            ready = event.events & Input;
                    }
                    else if (GetLastError() == ERROR_BROKEN_PIPE) {
                        ready = 0x10;
                    }
                }
                if (ready)
                    events[count++] = {ready, event.data};
                ++it;
                if (count == maxEvents)
                    break;
            }
        }
        if (count || (timeout >= 0 && std::chrono::steady_clock::now() >= deadline))
            return count;
        if (PollArm64Signals()) {
            errno = EINTR;
            return -1;
        }
        Sleep(1);
    }
}

struct AndroidPollFd {
    int fd;
    int16_t events, returned;
};

static_assert(sizeof(AndroidPollFd) == 8);

int PollDescriptors(AndroidPollFd* descriptors, size_t length, int timeout)
{
    if (length > static_cast<size_t>(_getmaxstdio())) {
        errno = EINVAL;
        return -1;
    }
    if (length && !descriptors) {
        errno = EFAULT;
        return -1;
    }
    constexpr int16_t Hangup = 0x10, Invalid = 0x20;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    for (;;) {
        int ready = 0;
        for (size_t index = 0; index < length; ++index) {
            auto& descriptor = descriptors[index];
            descriptor.returned = 0;
            if (descriptor.fd < 0)
                continue;
            if (const auto socket = AndroidSocketId(descriptor.fd)) {
                uint32_t previous = 0;
                uint64_t readGeneration = 0, writeGeneration = 0;
                descriptor.returned = static_cast<int16_t>(
                    PollAndroidSocket(descriptor.fd, socket, static_cast<uint16_t>(descriptor.events), previous,
                                      readGeneration, writeGeneration));
            }
            else if (const auto inotify = AndroidInotifyId(descriptor.fd)) {
                uint64_t generation = 0;
                descriptor.returned = static_cast<int16_t>(
                    PollAndroidInotify(descriptor.fd, inotify, static_cast<uint16_t>(descriptor.events), generation));
            }
            else if (const auto counter = FindEventCounter(descriptor.fd)) {
                const std::lock_guard lock(counter->mutex);
                descriptor.returned = static_cast<int16_t>(EventCounterReadiness(*counter) & descriptor.events);
            }
            else if (const auto timer = FindTimer(descriptor.fd)) {
                if ((descriptor.events & Input) && WaitForSingleObject(timer->event, 0) == WAIT_OBJECT_0)
                    descriptor.returned = Input;
            }
            else {
                const auto previous = _set_thread_local_invalid_parameter_handler(
                    +[](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
                    });
                const auto file = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor.fd));
                _set_thread_local_invalid_parameter_handler(previous);
                const auto type = GetFileType(file);
                if (type == FILE_TYPE_UNKNOWN)
                    descriptor.returned = Invalid;
                else if (type == FILE_TYPE_PIPE) {
                    DWORD bytes = 0;
                    if (PeekNamedPipe(file, nullptr, 0, nullptr, &bytes, nullptr)) {
                        if (bytes)
                            descriptor.returned = descriptor.events & Input;
                    }
                    else if (GetLastError() == ERROR_BROKEN_PIPE)
                        descriptor.returned = Hangup;
                    else
                        descriptor.returned = descriptor.events & Output;
                }
                else
                    descriptor.returned = descriptor.events & (Input | Output);
            }
            if (descriptor.returned)
                ++ready;
        }
        if (ready || (timeout >= 0 && std::chrono::steady_clock::now() >= deadline))
            return ready;
        if (PollArm64Signals()) {
            errno = EINTR;
            return -1;
        }
        Sleep(1);
    }
}

void CheckFdSet(int fd, const AndroidFdSet* set, size_t size)
{
    if (!set || fd < 0 || fd >= AndroidSelectFdLimit || size < sizeof(AndroidFdSet))
        std::abort();
}

int SelectDescriptors(int length, AndroidFdSet* read, AndroidFdSet* write, AndroidFdSet* except,
                      AndroidTimeval* timeout)
{
    using Clock = std::chrono::steady_clock;
    constexpr int16_t Read = 1, Priority = 2, Write = 4, Error = 8, Hangup = 0x10, Invalid = 0x20;
    if (length < 0 || length > AndroidSelectFdLimit ||
        (timeout && (timeout->seconds < 0 || timeout->micros < 0 || timeout->micros >= 1000000))) {
        errno = EINVAL;
        return -1;
    }
    const auto started = Clock::now();
    const auto maximum = std::chrono::duration_cast<std::chrono::microseconds>(Clock::time_point::max() - started);
    const auto duration = timeout && timeout->seconds <= (maximum.count() - timeout->micros) / 1000000
                              ? std::chrono::microseconds(timeout->seconds * 1000000 + timeout->micros)
                              : maximum;
    const auto deadline = started + duration;

    const auto bit = [](const AndroidFdSet* set, int fd) {
        return set && ((*set)[fd / 64] & (UINT64_C(1) << (fd % 64)));
    };
    std::array<AndroidPollFd, AndroidSelectFdLimit> descriptors;
    size_t count = 0;
    for (int fd = 0; fd < length; ++fd) {
        const auto events = static_cast<int16_t>((bit(read, fd) ? Read : 0) | (bit(write, fd) ? Write : 0) |
                                                 (bit(except, fd) ? Priority : 0));
        if (events)
            descriptors[count++] = {fd, events, 0};
    }
    for (;;) {
        if (PollDescriptors(descriptors.data(), count, 0) < 0)
            return -1;
        AndroidFdSet readable{}, writable{}, exceptional{};
        int ready = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto& [fd, events, returned] = descriptors[i];
            if (returned & Invalid) {
                errno = EBADF;
                return -1;
            }
            if ((events & Read) && (returned & (Read | Hangup | Error))) {
                readable[fd / 64] |= UINT64_C(1) << (fd % 64);
                ++ready;
            }
            if ((events & Write) && (returned & (Write | Error))) {
                writable[fd / 64] |= UINT64_C(1) << (fd % 64);
                ++ready;
            }
            if ((events & Priority) && (returned & Priority)) {
                exceptional[fd / 64] |= UINT64_C(1) << (fd % 64);
                ++ready;
            }
        }
        auto now = Clock::now();
        const bool interrupted = !ready && now < deadline && PollArm64Signals();
        if (ready || now >= deadline || interrupted) {
            if (timeout) {
                if (interrupted)
                    now = Clock::now();
                const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::max(deadline - now, Clock::duration::zero()))
                                           .count();
                *timeout = {remaining / 1000000, remaining % 1000000};
            }
            if (interrupted) {
                errno = EINTR;
                return -1;
            }
            if (read)
                *read = readable;
            if (write)
                *write = writable;
            if (except)
                *except = exceptional;
            return ready;
        }
        Sleep(1);
    }
}
}

int CloseAndroidFd(int fd)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    int specialResult;
    if (CloseAndroidInotify(fd, specialResult) || CloseAndroidMemoryFile(fd, specialResult) ||
        CloseAndroidSocket(fd, specialResult))
        return specialResult;
    const auto previous = _set_thread_local_invalid_parameter_handler(
        +[](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        });
    ForgetAndroidFileDescriptor(fd);
    const std::lock_guard lock(timerMutex);
    timers.erase(fd);
    eventCounters.erase(fd);
    epolls.erase(fd);
    const auto closing = AndroidWritableFileClosing(fd);
    const int result = _close(fd);
    _set_thread_local_invalid_parameter_handler(previous);
    if (result == 0)
        NotifyAndroidFileClosed(closing);
    return result;
}

bool AndroidLooperOwnsFd(int fd)
{
    if (AndroidInotifyId(fd))
        return true;
    const std::lock_guard lock(timerMutex);
    return timers.contains(fd) || eventCounters.contains(fd) || epolls.contains(fd);
}

bool DuplicateAndroidLooperFd(int fd, int minimum, bool closeOnExec, int& result, bool allowEpoll)
{
    const std::lock_guard lock(timerMutex);
    const auto epoll = epolls.find(fd);
    if (epoll == epolls.end() && !timers.contains(fd) && !eventCounters.contains(fd))
        return false;
    if (!allowEpoll || epoll == epolls.end()) {
        result = AndroidUnsupportedOperation();
        return true;
    }
    result = DuplicateCrtFd(fd, minimum, !closeOnExec);
    if (result >= 0) {
        try {
            // All descriptors for this epoll instance share the interest list.
            epolls.emplace(result, epoll->second);
        }
        catch (const std::bad_alloc&) {
            _close(result);
            errno = ENOMEM;
            result = -1;
        }
    }
    return true;
}

intptr_t ReadAndroidFd(int fd, void* buffer, size_t size)
{
    intptr_t result;
    if (ReadAndroidInotify(fd, buffer, size, result))
        return result;
    if (ReadAndroidMemoryFile(fd, buffer, size, result))
        return result;
    if (ReadAndroidRandomDevice(fd, buffer, size, result))
        return result;
    if (ReadAndroidSocket(fd, buffer, size, result))
        return result;
    if (auto counter = FindEventCounter(fd))
        return TransferEventCounter(*counter, buffer, nullptr, size);
    auto timer = FindTimer(fd);
    if (!timer) {
        const auto previous = _set_thread_local_invalid_parameter_handler(
            +[](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
            });
        result = _read(fd, buffer, static_cast<unsigned>(std::min<size_t>(size, INT_MAX)));
        _set_thread_local_invalid_parameter_handler(previous);
        if (result < 0 && errno == EINVAL) {
            unsigned long error = 0;
            // UCRT maps an empty PIPE_NOWAIT read to EINVAL.
            if (_get_doserrno(&error) == 0 && error == ERROR_NO_DATA)
                errno = EAGAIN;
        }
        return result;
    }
    if (size < sizeof(uint64_t)) {
        errno = EINVAL;
        return -1;
    }
    result = WaitForSingleObject(timer->event, timer->nonblocking ? 0 : INFINITE);
    if (result != WAIT_OBJECT_0) {
        errno = result == WAIT_TIMEOUT ? EAGAIN : EIO;
        return -1;
    }
    const TimerSpec disarm{};
    if (SetTimer(fd, 0, &disarm, nullptr) < 0)
        return -1;
    const uint64_t expirations = 1;
    std::memcpy(buffer, &expirations, sizeof(expirations));
    return sizeof(expirations);
}

bool WriteAndroidEventCounter(int fd, const void* buffer, size_t size, intptr_t& result)
{
    const auto counter = FindEventCounter(fd);
    if (!counter)
        return false;
    result = TransferEventCounter(*counter, nullptr, buffer, size);
    return true;
}

bool AndroidEventCounterFlags(int fd, int command, int argument, int& result)
{
    const auto counter = FindEventCounter(fd);
    if (!counter)
        return false;
    const std::lock_guard lock(counter->mutex);
    result = 0;
    switch (command) {
    case 1:
        result = (counter->flags & CloseOnExec) != 0;
        break;
    case 2:
        counter->flags = (counter->flags & ~CloseOnExec) | ((argument & 1) ? CloseOnExec : 0);
        break;
    case 3:
        result = 2 | (counter->flags & Nonblocking);
        break;
    case 4:
        counter->flags = (counter->flags & ~Nonblocking) | (argument & Nonblocking);
        break;
    default:
        errno = EINVAL;
        result = -1;
    }
    return true;
}

bool AndroidFenceReady(int fd)
{
    AndroidPollFd wait{fd, Input, 0};
    return PollDescriptors(&wait, 1, 0) > 0;
}

uintptr_t AndroidLooperImport(std::string_view name, bool hostAbi)
{
    if (hostAbi && name == "stone_looper_set_skip_empty_zero_timeout")
        return reinterpret_cast<uintptr_t>(+[](Looper* looper) {
            const std::lock_guard lock(looper->mutex);
            looper->skipEmptyZeroTimeout = true;
        });

    struct Arm64EpollEvent {
        uint32_t events;
        uint32_t padding;
        uint64_t data;
    };

    static_assert(sizeof(Arm64EpollEvent) == 16);
    if (hostAbi && name == "epoll_ctl")
        return reinterpret_cast<uintptr_t>(+[](int epoll, int operation, int fd, const Arm64EpollEvent* event) {
            EpollEvent packed{};
            if (event)
                packed = {event->events, event->data};
            return ControlEpoll(epoll, operation, fd, event ? &packed : nullptr);
        });
    if (hostAbi && name == "epoll_wait")
        return reinterpret_cast<uintptr_t>(+[](int epoll, Arm64EpollEvent* events, int count, int timeout) {
            if (!events || count < 1 || count > 4096) {
                errno = EINVAL;
                return -1;
            }
            try {
                std::vector<EpollEvent> packed(static_cast<size_t>(count));
                const int ready = WaitEpoll(epoll, packed.data(), count, timeout);
                for (int i = 0; i < ready; ++i)
                    events[i] = {packed[i].events, 0, packed[i].data};
                return ready;
            }
            catch (const std::bad_alloc&) {
                errno = ENOMEM;
                return -1;
            }
        });
#define BIND(symbol, ...)                                                                                              \
    if (name == #symbol) {                                                                                             \
        const auto address = reinterpret_cast<uintptr_t>(__VA_ARGS__);                                                 \
        return hostAbi ? address : BridgeAndroidImport(name, address);                                                 \
    }
    BIND(timerfd_create, &CreateTimer);
    BIND(timerfd_settime, &SetTimer);
    BIND(eventfd, &CreateEventCounter);
    BIND(
        eventfd_read, +[](int fd, uint64_t* value) {
            return ReadAndroidFd(fd, value, sizeof(*value)) == sizeof(*value) ? 0 : -1;
        });
    BIND(
        eventfd_write, +[](int fd, uint64_t value) {
            intptr_t result;
            if (WriteAndroidEventCounter(fd, &value, sizeof(value), result))
                return result == sizeof(value) ? 0 : -1;
            errno = EBADF;
            return -1;
        });
    BIND(
        epoll_create, +[](int size) {
            if (size > 0)
                return CreateEpoll(0);
            errno = EINVAL;
            return -1;
        });
    BIND(epoll_create1, &CreateEpoll);
    BIND(epoll_ctl, &ControlEpoll);
    BIND(epoll_wait, &WaitEpoll);
    if (!hostAbi) {
        BIND(
            epoll_pwait, +[](int epoll, EpollEvent* events, int count, int timeout, const uint64_t* mask) {
                if (mask)
                    return AndroidUnsupportedOperation();
                return WaitEpoll(epoll, events, count, timeout);
            });
    }
    BIND(poll, &PollDescriptors);
    BIND(
        __poll_chk, +[](AndroidPollFd* descriptors, unsigned count, int timeout, size_t bufferSize) {
            if (count > bufferSize / sizeof(AndroidPollFd))
                std::abort();
            return PollDescriptors(descriptors, count, timeout);
        });
    BIND(
        ppoll, +[](AndroidPollFd* descriptors, size_t count, const AndroidTimespec* timeout, const uint64_t* mask) {
            using Clock = std::chrono::steady_clock;
            const auto now = Clock::now();
            auto deadline = Clock::time_point::max();
            if (timeout) {
                const auto available = std::chrono::duration_cast<std::chrono::seconds>(deadline - now).count();
                if (timeout->seconds < 0 || timeout->nanos < 0 || timeout->nanos >= 1000000000) {
                    errno = EINVAL;
                    return -1;
                }
                if (timeout->seconds >= available) {
                    errno = 75;
                    return -1;
                } // Android EOVERFLOW.
                deadline = now + std::chrono::seconds(timeout->seconds) + std::chrono::nanoseconds(timeout->nanos);
            }
            if (IsArm64TranslationEnabled())
                RegisterArm64SignalThread();
            else
                RegisterNativeSignalThread();
            struct RestoreMask {
                bool active;
                uint64_t value;
                ~RestoreMask()
                {
                    if (active)
                        ExchangeAndroidSignalMask(value);
                }
            } restore{mask != nullptr, mask ? ExchangeAndroidSignalMask(*mask) : 0};
            for (;;) {
                int milliseconds = -1;
                if (timeout) {
                    const auto remaining = deadline - Clock::now();
                    auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
                    if (wait < remaining)
                        wait += std::chrono::milliseconds(1);
                    milliseconds = static_cast<int>(std::clamp<int64_t>(wait.count(), 0, INT_MAX));
                }
                const int result = PollDescriptors(descriptors, count, milliseconds);
                if (result || !timeout || Clock::now() >= deadline)
                    return result;
            }
        });
    BIND(select, &SelectDescriptors);
    BIND(
        __FD_SET_chk, +[](int fd, AndroidFdSet* set, size_t size) {
            CheckFdSet(fd, set, size);
            (*set)[fd / 64] |= UINT64_C(1) << (fd % 64);
        });
    BIND(
        __FD_CLR_chk, +[](int fd, AndroidFdSet* set, size_t size) {
            CheckFdSet(fd, set, size);
            (*set)[fd / 64] &= ~(UINT64_C(1) << (fd % 64));
        });
    BIND(
        __FD_ISSET_chk, +[](int fd, const AndroidFdSet* set, size_t size) -> int {
            CheckFdSet(fd, set, size);
            return ((*set)[fd / 64] & (UINT64_C(1) << (fd % 64))) != 0;
        });
    BIND(ALooper_prepare, &PrepareLooper);
    BIND(
        ALooper_forThread, +[] {
            return threadLooper.get();
        });
    BIND(
        ALooper_acquire, +[](Looper* looper) {
            const std::lock_guard lock(looper->mutex);
            ++looper->refs;
        });
    BIND(ALooper_release, &ReleaseLooper);
    BIND(
        ALooper_wake, +[](Looper* looper) {
            SetEvent(looper->wake);
        });
    BIND(ALooper_pollOnce, &PollOnce);
    BIND(ALooper_pollAll, &PollAll);
    BIND(ALooper_addFd, &AddFd);
    BIND(ALooper_removeFd, &RemoveFd);
#undef BIND
    return 0;
}

}
