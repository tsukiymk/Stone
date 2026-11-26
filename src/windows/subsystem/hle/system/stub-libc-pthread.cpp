#include "stub-libc.h"
#include "abi/AndroidLibraryImports.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "loader/ElfImage.h"
#include "signals/AndroidSignalContext.h"
#include "loader/AndroidSystemElf.h"

#include <windows.h>
#include <process.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace stone::windows {
namespace {
constexpr int Again = 11, Busy = 16, Invalid = 22, NoProcess = 3, NotSupported = 95, TimedOut = 110;
constexpr DWORD SignalWaitSlice = 10;

struct FutexWaiter;
using FutexQueue = std::multimap<uint32_t*, FutexWaiter*>;

struct FutexWaiter {
    std::condition_variable cv;
    uint32_t mask = UINT32_MAX;
    bool notified = false;
    FutexQueue::iterator entry;
};

std::mutex futexMutex;
FutexQueue futexWaiters;

constexpr int SignalCount = 64, KillSignal = 9, StopSignal = 19, FirstRealtimeSignal = 32;
constexpr uint32_t SignalInfo = 4;
constexpr uint32_t SignalNoChildStop = 1;
constexpr uint32_t SignalOnStack = 0x08000000;
constexpr uint32_t SignalUnsupported = 0x00000400;
constexpr uint32_t SignalRestart = 0x10000000, SignalNoDefer = 0x40000000, SignalResetHand = 0x80000000;
constexpr uintptr_t SignalIgnore = 1;

constexpr uint64_t SignalBit(int signal)
{
    return uint64_t{1} << (signal - 1);
}

constexpr uint64_t UnmaskableSignals = SignalBit(KillSignal) | SignalBit(StopSignal);

std::mutex signalMutex;
std::array<AndroidSignalAction, SignalCount> signalActions{};
std::array<uint64_t, SignalCount> signalIgnoreGeneration{};

struct SignalThread;
SignalThread* signalThreads = nullptr;

struct SignalPayload {
    int code = -6;
    uint64_t value = 0;
};

struct SignalThread {
    DWORD id = 0;
    bool arm64 = false;
    SignalThread* next = nullptr;
    std::atomic<uint64_t> mask{0};
    std::atomic<uint64_t> pending{0};
    std::array<uint64_t, SignalCount> generation{};
    std::array<unsigned, SignalCount - FirstRealtimeSignal + 1> realtime{};
    std::array<SignalPayload, SignalCount> payload{};
    std::array<std::deque<SignalPayload>, SignalCount - FirstRealtimeSignal + 1> realtimePayload;
    std::condition_variable queued;
    FutexWaiter* futex = nullptr; // Accessed with futexMutex held.

    ~SignalThread()
    {
        if (!id)
            return;
        const std::lock_guard lock(signalMutex);
        auto** link = &signalThreads;
        while (*link != this)
            link = &(*link)->next;
        *link = next;
    }

    uint64_t Ready() const
    {
        return pending.load(std::memory_order_acquire) & ~mask;
    }
};

thread_local SignalThread signalThread;

std::optional<bool> DeliverPendingSignals();

struct AndroidThreadCleanup {
    AndroidThreadCleanup* previous;
    uintptr_t routine;
    void* argument;
};

thread_local AndroidThreadCleanup* threadCleanup = nullptr;

void PushCleanup(AndroidThreadCleanup* entry, uintptr_t routine, void* argument)
{
    *entry = {threadCleanup, routine, argument};
    threadCleanup = entry;
}

void PopCleanup(AndroidThreadCleanup* entry, int execute)
{
    threadCleanup = entry->previous;
    if (execute && entry->routine)
        CallAndroidCallback(entry->routine, entry->argument);
}

constexpr size_t DefaultStackSize = 1024 * 1024;

int AttrInit(AndroidThreadAttr* attr)
{
    *attr = {};
    attr->stackSize = DefaultStackSize;
    return 0;
}

int AttrDetach(AndroidThreadAttr* attr, int detached)
{
    if (detached != 0 && detached != 1)
        return Invalid;
    attr->flags = static_cast<uint32_t>(detached);
    return 0;
}

int AttrStackSize(AndroidThreadAttr* attr, size_t size)
{
    if (size < 16384 || size > UINT_MAX)
        return Invalid;
    attr->stackSize = size;
    return 0;
}

struct AndroidThread {
    uintptr_t routine = 0;
    void* argument = nullptr;
    void* result = nullptr;
    bool arm64 = false;
    uint64_t signalMask = 0;
    std::latch registered{1};
};

thread_local std::shared_ptr<AndroidThread> runningNativeThread;

struct ThreadHandle {
    HANDLE handle;
    std::shared_ptr<AndroidThread> thread;
};

std::mutex threadMutex;
std::unordered_map<uint64_t, ThreadHandle> threads;

unsigned __stdcall RunThread(void* arg)
{
    std::unique_ptr<std::shared_ptr<AndroidThread>> context(static_cast<std::shared_ptr<AndroidThread>*>(arg));
    auto& thread = **context;
    signalThread.mask = thread.signalMask;
    if (thread.arm64) {
        RegisterArm64SignalThread();
        thread.registered.count_down();
    }
    else {
        // pthread_exit terminates a guest-created thread before its entry returns.
        runningNativeThread = std::move(*context);
        context.reset();
    }
    try {
        thread.result = reinterpret_cast<void*>(CallAndroidCallback(thread.routine, thread.argument));
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "[android-thread] %s\n", error.what());
    }
    ElfImage::RunThreadDestructors();
    runningNativeThread.reset();
    return 0;
}

[[noreturn]] void ExitNativeThread(void* result)
{
    if (!runningNativeThread)
        throw std::runtime_error("pthread_exit requires a guest-created native thread");
    runningNativeThread->result = result;
    while (threadCleanup)
        PopCleanup(threadCleanup, 1);
    ElfImage::RunThreadDestructors();
    DetachAndroidNativeThread();
    runningNativeThread.reset();
    _endthreadex(0);
}

int CreateThreadForGuest(uint64_t* result, const AndroidThreadAttr* attr, uintptr_t routine, void* arg)
{
    if (attr && attr->policy != 0)
        return attr->policy == 1 || attr->policy == 2 ? EPERM : NotSupported;
    try {
        auto thread = std::make_shared<AndroidThread>();
        thread->routine = routine;
        thread->argument = arg;
        thread->arm64 = signalThread.arm64;
        thread->signalMask = signalThread.mask;
        auto context = std::make_unique<std::shared_ptr<AndroidThread>>(thread);
        unsigned id = 0;
        const auto size = attr ? static_cast<unsigned>(std::max(attr->stackSize, DefaultStackSize)) : 0;
        const auto handle = reinterpret_cast<HANDLE>(_beginthreadex(
            nullptr, size, RunThread, context.get(), CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &id));
        if (!handle)
            return Again;
        const bool detached = attr && (attr->flags & 1);
        if (!detached) {
            try {
                const std::lock_guard lock(threadMutex);
                threads.insert_or_assign(id, ThreadHandle{handle, thread});
            }
            catch (...) {
                TerminateThread(handle, 1);
                CloseHandle(handle);
                throw;
            }
        }
        *result = id;
        auto* ownership = context.release();
        if (ResumeThread(handle) == DWORD(-1)) {
            delete ownership;
            const std::lock_guard lock(threadMutex);
            threads.erase(id);
            TerminateThread(handle, 1);
            CloseHandle(handle);
            return Again;
        }
        if (detached)
            CloseHandle(handle);
        if (thread->arm64)
            thread->registered.wait();
        return 0;
    }
    catch (const std::bad_alloc&) {
        return Again;
    }
}

int JoinThread(uint64_t id, void** result)
{
    if (id == GetCurrentThreadId())
        return 35;
    ThreadHandle entry{};
    {
        const std::lock_guard lock(threadMutex);
        const auto it = threads.find(id);
        if (it == threads.end())
            return NoProcess;
        entry = std::move(it->second);
        threads.erase(it);
    }
    DWORD waited;
    while ((waited = WaitForSingleObject(entry.handle, signalThread.id ? SignalWaitSlice : INFINITE)) == WAIT_TIMEOUT)
        DeliverPendingSignals();
    CloseHandle(entry.handle);
    if (waited != WAIT_OBJECT_0)
        return Invalid;
    if (result)
        *result = entry.thread->result;
    return 0;
}

int DetachThread(uint64_t id)
{
    const std::lock_guard lock(threadMutex);
    const auto it = threads.find(id);
    if (it == threads.end())
        return NoProcess;
    CloseHandle(it->second.handle);
    threads.erase(it);
    return 0;
}

int RegisterThreadDestructor(uintptr_t callback, void* argument, void*)
{
    if (!callback)
        return -1;
    try {
        // The Windows ELF loader retains published DSOs for the process lifetime.
        return ElfImage::RegisterThreadDestructor(
                   +[](uintptr_t entry, void* object) {
                       CallAndroidCallback(entry, object);
                   },
                   callback, argument)
                   ? 0
                   : -1;
    }
    catch (const std::bad_alloc&) {
        return -1;
    }
}

using ThreadValues = std::map<int, void*>;
thread_local ThreadValues* threadValues = nullptr;
std::mutex keysMutex;
std::unordered_map<int, uintptr_t> keys;
unsigned nextKey = 0;

void DestroyThreadValues()
{
    const std::unique_ptr<ThreadValues> values(threadValues);
    constexpr unsigned DestructorIterations = 4;
    for (unsigned pass = 0; pass < DestructorIterations; ++pass) {
        bool called = false;
        auto next = values->begin();
        while (next != values->end()) {
            const auto [key, value] = *next;
            uintptr_t destructor = 0;
            {
                const std::lock_guard lock(keysMutex);
                if (const auto entry = keys.find(key); entry != keys.end())
                    destructor = entry->second;
            }
            if (!destructor) {
                ++next;
                continue;
            }
            values->erase(next);
            try {
                CallAndroidCallback(destructor, value);
            }
            catch (const std::exception& error) {
                std::fprintf(stderr, "[android-native] TLS destructor: %s\n", error.what());
            }
            called = true;
            // Destructors can set or erase any key, including their own.
            next = values->upper_bound(key);
        }
        if (!called)
            break;
    }
    threadValues = nullptr;
}

int CreateKey(int* key, uintptr_t destructor)
{
    const std::lock_guard lock(keysMutex);
    if (nextKey > INT_MAX)
        return Again;
    try {
        *key = static_cast<int>(nextKey++);
        keys.emplace(*key, destructor);
        return 0;
    }
    catch (const std::bad_alloc&) {
        return Again;
    }
}

int DeleteKey(int key)
{
    const std::lock_guard lock(keysMutex);
    return keys.erase(key) ? 0 : Invalid;
}

void* GetKey(int key)
{
    const std::lock_guard lock(keysMutex);
    if (!threadValues || !keys.contains(key))
        return nullptr;
    const auto it = threadValues->find(key);
    return it == threadValues->end() ? nullptr : it->second;
}

int SetKey(int key, void* value)
{
    const std::lock_guard lock(keysMutex);
    if (!keys.contains(key))
        return Invalid;
    try {
        if (!threadValues) {
            if (!value)
                return 0;
            auto storage = std::make_unique<ThreadValues>();
            if (!ElfImage::SetThreadKeyCleanup(DestroyThreadValues))
                return ENOMEM;
            threadValues = storage.release();
        }
        if (value)
            threadValues->insert_or_assign(key, value);
        else
            threadValues->erase(key);
        return 0;
    }
    catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

int Once(int* state, uintptr_t callback)
{
    return RunAndroidOnce(*state, [callback] {
        CallAndroidCallback(callback, nullptr);
    });
}

// The first guest word is the Bionic static initializer. Host locks live
// in a table because some callers only align their guest mutex to four bytes.
struct Mutex {
    std::recursive_timed_mutex lock;

    void Lock()
    {
        if (!signalThread.id) {
            lock.lock();
            return;
        }
        while (!lock.try_lock_for(std::chrono::milliseconds(SignalWaitSlice)))
            DeliverPendingSignals();
    }
};

std::mutex locksMutex;
std::unordered_map<void*, std::unique_ptr<Mutex>> mutexes;

Mutex& GuestMutex(void* address)
{
    const std::lock_guard guard(locksMutex);
    auto& lock = mutexes[address];
    if (!lock)
        lock = std::make_unique<Mutex>();
    return *lock;
}

int MutexInit(void* address, const int32_t*)
{
    try {
        GuestMutex(address);
        return 0;
    }
    catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

int MutexDestroy(void* address)
{
    const std::lock_guard guard(locksMutex);
    mutexes.erase(address);
    return 0;
}

int MutexLock(void* address)
{
    try {
        GuestMutex(address).Lock();
        return 0;
    }
    catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

int MutexTryLock(void* address)
{
    if (MutexInit(address, nullptr))
        return ENOMEM;
    return GuestMutex(address).lock.try_lock() ? 0 : Busy;
}

int MutexUnlock(void* address)
{
    GuestMutex(address).lock.unlock();
    return 0;
}

struct Condition {
    std::mutex mutex;
    std::condition_variable cv;
};

std::unordered_map<void*, std::unique_ptr<Condition>> conditions;

Condition& GuestCondition(int* address)
{
    const std::lock_guard guard(locksMutex);
    auto& cond = conditions[address];
    if (!cond)
        cond = std::make_unique<Condition>();
    return *cond;
}

int CondInit(int* address, const int64_t* attrs)
{
    *address = attrs ? static_cast<int>(*attrs) : 0;
    try {
        GuestCondition(address);
        return 0;
    }
    catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

int CondDestroy(int* address)
{
    const std::lock_guard guard(locksMutex);
    conditions.erase(address);
    return 0;
}

int CondNotify(int* address, bool broadcast)
{
    auto& condition = GuestCondition(address);
    const std::lock_guard guard(condition.mutex);
    if (broadcast)
        condition.cv.notify_all();
    else
        condition.cv.notify_one();
    return 0;
}

int CondClockWait(int* address, void* mutex, int clock, const AndroidTimespec* timeout)
{
    if (clock != 0 && clock != 1)
        return Invalid;
    if (timeout && (timeout->nanos < 0 || timeout->nanos >= 1'000'000'000))
        return Invalid;
    Condition* condition;
    Mutex* guestMutex;
    try {
        condition = &GuestCondition(address);
        guestMutex = &GuestMutex(mutex);
    }
    catch (const std::bad_alloc&) {
        return ENOMEM;
    }
    // Pair the condition's notification lock with the guest unlock so a
    // signal sent after the unlock cannot slip past wait registration.
    std::unique_lock lock(condition->mutex);
    guestMutex->lock.unlock();
    int result = 0;
    if (!timeout && !signalThread.id) {
        condition->cv.wait(lock);
    }
    else {
        for (;;) {
            // A realtime deadline follows wall-clock changes.
            const unsigned cap = clock == 0 ? 1 : 86400;
            auto delay = std::chrono::nanoseconds(std::chrono::seconds(cap));
            if (timeout) {
                AndroidTimespec now{};
                AndroidClock(clock, &now);
                if (timeout->seconds < now.seconds ||
                    (timeout->seconds == now.seconds && timeout->nanos <= now.nanos)) {
                    result = TimedOut;
                    break;
                }
                const auto seconds = uint64_t(timeout->seconds) - uint64_t(now.seconds);
                if (seconds <= cap)
                    delay = std::min(delay, std::chrono::seconds(seconds) +
                                                std::chrono::nanoseconds(timeout->nanos - now.nanos));
            }
            if (signalThread.id)
                delay = std::min(delay, std::chrono::nanoseconds(std::chrono::milliseconds(SignalWaitSlice)));
            if (condition->cv.wait_for(lock, delay) != std::cv_status::timeout)
                break;
            if (signalThread.Ready()) {
                // A guest handler may itself need the condition. Return as a
                // spurious wake so the waiter can recheck its predicate.
                lock.unlock();
                DeliverPendingSignals();
                break;
            }
        }
    }
    if (lock.owns_lock())
        lock.unlock();
    // A paused guest can own the mutex, so dispatch signals during reacquisition.
    guestMutex->Lock();
    return result;
}

int CondWait(int* address, void* mutex, const AndroidTimespec* timeout)
{
    return CondClockWait(address, mutex, (*address & 2) ? 1 : 0, timeout);
}

struct RwLock {
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<DWORD> writer{0};
};

static_assert(sizeof(RwLock) <= 56);
std::unordered_map<void*, std::unique_ptr<RwLock>> rwlocks;

RwLock& GuestRwLock(void* address)
{
    const std::lock_guard guard(locksMutex);
    auto& lock = rwlocks[address];
    if (!lock)
        lock = std::make_unique<RwLock>();
    return *lock;
}

int RwInit(void* address, const int64_t* attr)
{
    if (attr && *attr != 0)
        return NotSupported;
    try {
        GuestRwLock(address);
        return 0;
    }
    catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

int RwDestroy(void* address)
{
    const std::lock_guard guard(locksMutex);
    rwlocks.erase(address);
    return 0;
}

template <bool Exclusive, bool Try> int RwAcquire(void* address)
{
    RwLock* lock;
    try {
        lock = &GuestRwLock(address);
    }
    catch (const std::bad_alloc&) {
        return ENOMEM;
    }
    if constexpr (Exclusive) {
        if constexpr (Try) {
            if (!TryAcquireSRWLockExclusive(&lock->lock))
                return Busy;
        }
        else
            AcquireSRWLockExclusive(&lock->lock);
        lock->writer.store(GetCurrentThreadId(), std::memory_order_relaxed);
    }
    else {
        if constexpr (Try) {
            if (!TryAcquireSRWLockShared(&lock->lock))
                return Busy;
        }
        else
            AcquireSRWLockShared(&lock->lock);
    }
    return 0;
}

int RwUnlock(void* address)
{
    auto* lock = &GuestRwLock(address);
    if (lock->writer.load(std::memory_order_relaxed) == GetCurrentThreadId()) {
        lock->writer.store(0, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&lock->lock);
    }
    else
        ReleaseSRWLockShared(&lock->lock);
    return 0;
}

int SemInit(unsigned* value, int shared, unsigned count)
{
    if (shared || count > INT_MAX) {
        errno = shared ? ENOSYS : EINVAL;
        return -1;
    }
    *value = count;
    return 0;
}

int SemPost(unsigned* value)
{
    std::atomic_ref<unsigned> count(*value);
    auto current = count.load();
    do {
        if (current >= INT_MAX) {
            errno = 75;
            return -1;
        }
    } while (!count.compare_exchange_weak(current, current + 1));
    WakeByAddressSingle(value);
    return 0;
}

int WaitSemaphore(unsigned* value, bool tryOnly, const AndroidTimespec* deadline = nullptr, int clock = 0)
{
    std::atomic_ref<unsigned> count(*value);
    auto current = count.load();
    for (;;) {
        if (current) {
            if (count.compare_exchange_weak(current, current - 1))
                return 0;
            continue;
        }
        if (tryOnly) {
            errno = EAGAIN;
            return -1;
        }
        DWORD timeout = INFINITE;
        if (deadline) {
            if (deadline->nanos < 0 || deadline->nanos >= 1'000'000'000) {
                errno = EINVAL;
                return -1;
            }
            AndroidTimespec now{};
            if (AndroidClock(clock, &now) != 0)
                return -1;
            if (deadline->seconds < now.seconds || (deadline->seconds == now.seconds && deadline->nanos <= now.nanos)) {
                errno = TimedOut;
                return -1;
            }
            const uint64_t seconds = uint64_t(deadline->seconds) - uint64_t(now.seconds);
            const unsigned cap = clock == 0 ? 1 : 86400;
            auto delay = std::chrono::nanoseconds(std::chrono::seconds(cap));
            if (seconds <= cap) {
                delay = std::min(delay,
                                 std::chrono::seconds(seconds) + std::chrono::nanoseconds(deadline->nanos - now.nanos));
            }
            timeout = static_cast<DWORD>(std::chrono::ceil<std::chrono::milliseconds>(delay).count());
        }
        if (signalThread.id)
            timeout = std::min(timeout, SignalWaitSlice);
        if (!WaitOnAddress(value, &current, sizeof(current), timeout) && GetLastError() != ERROR_TIMEOUT) {
            errno = EIO;
            return -1;
        }
        if (const auto restart = DeliverPendingSignals(); restart && !*restart) {
            errno = EINTR;
            return -1;
        }
        current = count.load();
    }
}

int SemClockWait(unsigned* value, int clock, const AndroidTimespec* deadline)
{
    if (clock != 0 && clock != 1)
        return EINVAL; // Bionic's return convention for unsupported clocks.
    if (!deadline) {
        errno = EINVAL;
        return -1;
    }
    return WaitSemaphore(value, false, deadline, clock);
}

HANDLE OpenGuestThread(uint64_t id, DWORD access)
{
    if (!id || id > MAXDWORD)
        return nullptr;
    const auto handle = OpenThread(access | THREAD_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(id));
    if (handle && GetProcessIdOfThread(handle) != GetCurrentProcessId()) {
        CloseHandle(handle);
        return nullptr;
    }
    return handle;
}

constexpr size_t ThreadNameLimit = 16;

int ThreadNiceness(bool set, int which, unsigned id, int nice)
{
    if (which != 0) {
        errno = EINVAL;
        return -1;
    } // PRIO_PROCESS targets a Linux task.
    if (!id || id == GetCurrentProcessId())
        id = GetCurrentThreadId();
    const auto handle = OpenGuestThread(id, set ? THREAD_SET_INFORMATION : THREAD_QUERY_INFORMATION);
    if (!handle) {
        errno = ESRCH;
        return -1;
    }

    struct Close {
        HANDLE handle;

        ~Close()
        {
            CloseHandle(handle);
        }
    } close{handle};

    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(handle, &created, &exited, &kernel, &user)) {
        errno = EIO;
        return -1;
    }
    const uint64_t identity = (uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime;
    static std::mutex mutex;
    static std::unordered_map<DWORD, std::pair<uint64_t, int>> requested;
    const std::lock_guard lock(mutex);
    if (set) {
        nice = std::clamp(nice, -20, 19);
        const int priority = nice >= 19   ? THREAD_PRIORITY_IDLE
                             : nice >= 5  ? THREAD_PRIORITY_LOWEST
                             : nice > 0   ? THREAD_PRIORITY_BELOW_NORMAL
                             : nice <= -5 ? THREAD_PRIORITY_HIGHEST
                             : nice < 0   ? THREAD_PRIORITY_ABOVE_NORMAL
                                          : THREAD_PRIORITY_NORMAL;
        if (!SetThreadPriority(handle, priority)) {
            errno = EPERM;
            return -1;
        }
        requested[id] = {identity, nice};
        return 0;
    }
    const auto saved = requested.find(id);
    if (saved != requested.end()) {
        if (saved->second.first == identity)
            return saved->second.second;
        requested.erase(saved); // Win32 can reuse a terminated thread's ID.
    }
    const int priority = GetThreadPriority(handle);
    if (priority == THREAD_PRIORITY_ERROR_RETURN) {
        errno = EIO;
        return -1;
    }
    return priority <= THREAD_PRIORITY_IDLE      ? 19
           : priority <= THREAD_PRIORITY_LOWEST  ? 5
           : priority < 0                        ? 1
           : priority >= THREAD_PRIORITY_HIGHEST ? -5
           : priority > 0                        ? -1
                                                 : 0;
}

int SetThreadName(uint64_t id, const char* name)
{
    if (strnlen(name, ThreadNameLimit) == ThreadNameLimit)
        return ERANGE;
    wchar_t wide[ThreadNameLimit];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, ARRAYSIZE(wide)))
        return Invalid;
    const auto thread = OpenGuestThread(id, THREAD_SET_LIMITED_INFORMATION);
    if (!thread)
        return NoProcess;
    const auto result = SetThreadDescription(thread, wide);
    CloseHandle(thread);
    return SUCCEEDED(result) ? 0 : Invalid;
}

int GetThreadName(uint64_t id, char* name, size_t capacity)
{
    if (capacity < ThreadNameLimit)
        return ERANGE;
    const auto thread = OpenGuestThread(id, THREAD_QUERY_LIMITED_INFORMATION);
    if (!thread)
        return NoProcess;
    wchar_t* wide = nullptr;
    const auto result = GetThreadDescription(thread, &wide);
    CloseHandle(thread);
    if (FAILED(result))
        return Invalid;
    // Win32 leaves a new thread unnamed. Android exposes the process's default name in this case.
    const wchar_t* text = wide && *wide ? wide : L"Stone";
    const auto converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, name,
                                               static_cast<int>(std::min<size_t>(capacity, INT_MAX)), nullptr, nullptr);
    LocalFree(wide);
    return converted ? 0 : ERANGE;
}

int RaiseSignal(int signal);
int QueueSignal(SignalThread& target, int signal, SignalPayload payload = {});

int KillThread(uint64_t id, int signal)
{
    if (signal < 0 || signal > 64)
        return Invalid;
    if (id == GetCurrentThreadId()) {
        const int saved = errno;
        const int result = RaiseSignal(signal);
        const int error = result == 0 ? 0 : errno;
        errno = saved;
        return error;
    }
    if (id == 0 || id > MAXDWORD)
        return NoProcess;
    {
        const std::lock_guard lock(signalMutex);
        for (auto* target = signalThreads; target; target = target->next) {
            if (target->id == id)
                return signal ? QueueSignal(*target, signal) : 0;
        }
    }
    const auto thread = OpenGuestThread(id, THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE);
    if (!thread)
        return NoProcess;
    const bool alive =
        GetProcessIdOfThread(thread) == GetCurrentProcessId() && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT;
    CloseHandle(thread);
    if (!alive)
        return NoProcess;
    // Signal zero checks liveness without delivering a signal.
    return signal == 0 ? 0 : signalThread.id ? NoProcess : 38;
}

int GetScheduling(uint64_t id, int* policy, int* priority)
{
    const auto thread = OpenGuestThread(id, THREAD_QUERY_INFORMATION);
    if (!thread)
        return NoProcess;
    const auto current = GetThreadPriority(thread);
    CloseHandle(thread);
    if (current == THREAD_PRIORITY_ERROR_RETURN)
        return Invalid;
    *policy = 0;
    *priority = 0;
    return 0;
}

int GetScheduler(int id)
{
    const auto process = GetCurrentProcessId();
    if (id == 0 || static_cast<DWORD>(id) == process)
        return 0; // SCHED_OTHER
    if (id < 0) {
        errno = EINVAL;
        return -1;
    }
    const auto thread = OpenGuestThread(static_cast<uint64_t>(id), THREAD_QUERY_LIMITED_INFORMATION);
    if (thread) {
        const bool owned = GetProcessIdOfThread(thread) == process;
        CloseHandle(thread);
        if (owned)
            return 0;
    }
    errno = NoProcess;
    return -1;
}

int SetScheduling(uint64_t id, int policy, const int* priority)
{
    if (policy != 0 || *priority != 0)
        return EPERM;
    const auto thread = OpenGuestThread(id, THREAD_SET_INFORMATION);
    if (!thread)
        return NoProcess;
    const bool ok = SetThreadPriority(thread, THREAD_PRIORITY_NORMAL) != FALSE;
    CloseHandle(thread);
    return ok ? 0 : EPERM;
}

int GetSchedulingParam(int pid, int* priority)
{
    if (!priority) {
        errno = EINVAL;
        return -1;
    }
    if (GetScheduler(pid) < 0)
        return -1;
    *priority = 0;
    return 0;
}

int SetScheduler(int pid, int policy, const int* priority)
{
    if (pid < 0 || !priority ||
        (policy != 0 && policy != 1 && policy != 2 && policy != 3 && policy != 5 && policy != 6)) {
        errno = EINVAL;
        return -1;
    }
    if (policy != 0) {
        errno = EPERM;
        return -1;
    }
    if (*priority != 0) {
        errno = EINVAL;
        return -1;
    }
    const uint64_t id = pid == 0 || static_cast<DWORD>(pid) == GetCurrentProcessId() ? GetCurrentThreadId() : pid;
    const int error = SetScheduling(id, policy, priority);
    if (!error)
        return 0;
    errno = error;
    return -1;
}

int GetCpuAffinity(int id, size_t size, uint8_t* mask)
{
    if (id < 0 || !size) {
        errno = EINVAL;
        return -1;
    }
    if (!mask) {
        errno = EFAULT;
        return -1;
    }
    const bool current = id == 0 || static_cast<DWORD>(id) == GetCurrentProcessId();
    const auto thread = current ? GetCurrentThread() : OpenGuestThread(id, THREAD_QUERY_LIMITED_INFORMATION);
    if (!thread) {
        errno = ESRCH;
        return -1;
    }
    GROUP_AFFINITY affinity{};
    const bool found =
        GetProcessIdOfThread(thread) == GetCurrentProcessId() && GetThreadGroupAffinity(thread, &affinity);
    if (!current)
        CloseHandle(thread);
    if (!found) {
        errno = ESRCH;
        return -1;
    }
    const size_t offset = affinity.Group * sizeof(affinity.Mask);
    const auto bytes = static_cast<size_t>((std::bit_width(affinity.Mask) + 7) / 8);
    if (offset > size || bytes > size - offset) {
        errno = EINVAL;
        return -1;
    }
    std::memset(mask, 0, size);
    std::memcpy(mask + offset, &affinity.Mask, bytes);
    return 0;
}

int CountCpus(size_t size, const uint8_t* mask)
{
    size_t count = 0;
    for (size_t index = 0; index < size; ++index)
        count += std::popcount(mask[index]);
    return static_cast<int>(std::min<size_t>(count, INT_MAX));
}

template <bool Max> int PriorityBound(int policy)
{
    if (policy == 0 || policy == 3 || policy == 5)
        return 0;
    if (policy == 1 || policy == 2)
        return Max ? 99 : 1;
    errno = EINVAL;
    return -1;
}

template <bool Add> int SignalSet(uint64_t* mask, int signal)
{
    if (signal <= 0 || signal > 64) {
        errno = EINVAL;
        return -1;
    }
    const auto bit = uint64_t{1} << (signal - 1);
    if constexpr (Add)
        *mask |= bit;
    else
        *mask &= ~bit;
    return 0;
}

uint64_t PendingSignals()
{
    if (!signalThread.pending.load(std::memory_order_acquire))
        return 0;
    const std::lock_guard lock(signalMutex);
    auto current = signalThread.pending.load(std::memory_order_relaxed);
    for (auto pending = current; pending; pending &= pending - 1) {
        const auto index = std::countr_zero(pending);
        if (signalThread.generation[index] != signalIgnoreGeneration[index])
            current &= ~(uint64_t{1} << index);
    }
    signalThread.pending.store(current, std::memory_order_relaxed);
    return current;
}

int QueueSignal(SignalThread& target, int signal, SignalPayload payload)
{
    const auto index = signal - 1;
    if (signalActions[index].handler == SignalIgnore)
        return 0;
    const auto bit = SignalBit(signal);
    const auto pending = target.pending.load(std::memory_order_relaxed);
    const auto generation = signalIgnoreGeneration[index];
    if (signal >= FirstRealtimeSignal) {
        auto& count = target.realtime[signal - FirstRealtimeSignal];
        auto& values = target.realtimePayload[signal - FirstRealtimeSignal];
        if (!(pending & bit) || target.generation[index] != generation) {
            count = 0;
            values.clear();
        }
        if (count == UINT_MAX)
            return Again;
        try {
            values.push_back(payload);
        }
        catch (const std::bad_alloc&) {
            return Again;
        }
        ++count;
    }
    else if (!(pending & bit) || target.generation[index] != generation) {
        target.payload[index] = payload;
    }
    target.generation[index] = generation;
    target.pending.store(pending | bit, std::memory_order_release);
    target.queued.notify_one();
    const std::lock_guard lock(futexMutex);
    if (target.futex)
        target.futex->cv.notify_one();
    return 0;
}

AndroidSignalAction TakeSignalAction(int signal)
{
    auto& registered = signalActions[signal - 1];
    const auto action = registered;
    if (action.handler > SignalIgnore && (action.flags & SignalResetHand))
        registered = {};
    return action;
}

struct RestoreSignalMask {
    uint64_t saved = signalThread.mask;

    ~RestoreSignalMask()
    {
        signalThread.mask = saved;
    }
};

int DeliverSignal(int signal, const AndroidSignalAction& action, std::optional<bool>* restart = nullptr,
                  SignalPayload payload = {})
{
    if (action.handler == SignalIgnore)
        return 0;
    if (!action.handler) {
        switch (signal) {
        case 17: // SIGCHLD
        case 18: // SIGCONT
        case 23: // SIGURG
        case 28: // SIGWINCH
            return 0;
        case StopSignal:
        case 20: // SIGTSTP
        case 21: // SIGTTIN
        case 22: // SIGTTOU
            return AndroidUnsupportedOperation();
        default:
            ExitProcess(static_cast<UINT>(128 + signal));
        }
    }

    RestoreSignalMask restore;
    signalThread.mask |= action.mask & ~UnmaskableSignals;
    if (!(action.flags & SignalNoDefer))
        signalThread.mask |= SignalBit(signal);
    const bool withInfo = (action.flags & SignalInfo) != 0;
    if (!RunArm64Signal(action.handler, signal, restore.saved, withInfo)) {
        if (withInfo && IsArm64TranslationEnabled())
            return AndroidUnsupportedOperation();
        if (withInfo) {
            CONTEXT machine{};
            RtlCaptureContext(&machine);
            alignas(16) AndroidSignalContext context;
            CaptureAndroidSignalContext(context, machine, restore.saved);
            AndroidSignalInfo info{signal, 0, payload.code, 0, {}};
            info.detail[0] = GetCurrentProcessId() | (uint64_t{AndroidApplicationUid()} << 32);
            info.detail[1] = payload.value;
            const auto callback = reinterpret_cast<void (*)(int, AndroidSignalInfo*, AndroidSignalContext*)>(
                MakeNativeAndroidThunk(action.handler, "(ILL)V"));
            callback(signal, &info, &context);
            restore.saved = context.mask;
        }
        else
            CallAndroidCallback(action.handler, reinterpret_cast<void*>(static_cast<intptr_t>(signal)));
    }
    if (restart)
        *restart = (action.flags & SignalRestart) != 0;
    return 0;
}

std::optional<bool> DeliverPendingSignals()
{
    std::optional<bool> restart;
    while (const auto ready = signalThread.Ready()) {
        const int signal = std::countr_zero(ready) + 1;
        AndroidSignalAction action;
        SignalPayload payload;
        {
            const std::lock_guard lock(signalMutex);
            const auto index = signal - 1;
            payload = signalThread.payload[index];
            const bool discarded = signalThread.generation[index] != signalIgnoreGeneration[index];
            if (discarded || signal < FirstRealtimeSignal || --signalThread.realtime[signal - FirstRealtimeSignal] == 0)
                signalThread.pending.fetch_and(~SignalBit(signal), std::memory_order_relaxed);
            if (signal >= FirstRealtimeSignal) {
                auto& values = signalThread.realtimePayload[signal - FirstRealtimeSignal];
                if (discarded)
                    values.clear();
                else if (!values.empty()) {
                    payload = values.front();
                    values.pop_front();
                }
            }
            if (discarded)
                continue;
            action = TakeSignalAction(signal);
        }
        const int saved = errno;
        std::optional<bool> current;
        DeliverSignal(signal, action, &current, payload);
        errno = saved;
        if (current)
            restart = restart.value_or(true) && *current;
    }
    return restart;
}

int RaiseSignal(int signal)
{
    if (signal < 0 || signal > SignalCount) {
        errno = EINVAL;
        return -1;
    }
    if (!signal)
        return 0;
    AndroidSignalAction action;
    {
        const std::lock_guard lock(signalMutex);
        if (signalThread.mask & SignalBit(signal)) {
            const auto error = QueueSignal(signalThread, signal);
            if (error) {
                errno = error;
                return -1;
            }
            return 0;
        }
        action = TakeSignalAction(signal);
    }
    const int result = DeliverSignal(signal, action);
    if (result == 0)
        DeliverPendingSignals();
    return result;
}

int SignalMask(int how, const uint64_t* mask, uint64_t* previous)
{
    const uint64_t old = signalThread.mask.load();
    if (mask) {
        const auto requested = *mask & ~UnmaskableSignals;
        switch (how) {
        case 0:
            signalThread.mask |= requested;
            break;
        case 1:
            signalThread.mask &= ~requested;
            break;
        case 2:
            signalThread.mask = requested;
            break;
        default:
            return Invalid;
        }
    }
    if (previous)
        *previous = old;
    DeliverPendingSignals();
    return 0;
}

int WaitSignalInfo(const uint64_t* mask, void* info)
{
    if (!mask) {
        errno = EFAULT;
        return -1;
    }
    RegisterNativeSignalThread();
    std::unique_lock lock(signalMutex);
    for (;;) {
        const auto ready = signalThread.pending.load(std::memory_order_acquire) & *mask & ~UnmaskableSignals;
        if (!ready) {
            signalThread.queued.wait(lock);
            continue;
        }
        const unsigned index = std::countr_zero(ready);
        const int signal = index + 1;
        const bool discarded = signalThread.generation[index] != signalIgnoreGeneration[index];
        if (discarded || signal < FirstRealtimeSignal || --signalThread.realtime[signal - FirstRealtimeSignal] == 0)
            signalThread.pending.fetch_and(~SignalBit(signal), std::memory_order_relaxed);
        auto payload = signalThread.payload[index];
        if (signal >= FirstRealtimeSignal) {
            auto& values = signalThread.realtimePayload[signal - FirstRealtimeSignal];
            if (discarded)
                values.clear();
            else if (!values.empty()) {
                payload = values.front();
                values.pop_front();
            }
        }
        if (discarded)
            continue;
        if (info) {
            struct SignalInfoData {
                int32_t signal, error, code, padding;
                int32_t pid;
                uint32_t uid;
                uint8_t data[104];
            };

            static_assert(sizeof(SignalInfoData) == 128);
            SignalInfoData value{
                signal, 0, payload.code, 0, static_cast<int32_t>(GetCurrentProcessId()), AndroidApplicationUid(), {}};
            std::memcpy(value.data, &payload.value, sizeof(payload.value));
            std::memcpy(info, &value, sizeof(value));
        }
        return signal;
    }
}

int SetSignalAction(int signal, const AndroidSignalAction* action, AndroidSignalAction* previous, bool /*arm64*/)
{
    if (signal <= 0 || signal > SignalCount || (action && (signal == KillSignal || signal == StopSignal))) {
        errno = EINVAL;
        return -1;
    }
    // With no alternate stack registered, SA_ONSTACK uses the regular stack.
    // ART's sigchain requests this flag even when sigaltstack is unavailable.
    const unsigned flags = SignalRestart | SignalNoDefer | SignalResetHand | SignalInfo | SignalOnStack |
                           (signal == 17 ? SignalNoChildStop : 0);
    if (action && !(action->flags & SignalUnsupported) && (action->flags & ~flags))
        return AndroidUnsupportedOperation();
    const std::lock_guard lock(signalMutex);
    auto& registered = signalActions[signal - 1];
    const auto old = registered;
    if (action) {
        registered = *action;
        // Linux's SA_UNSUPPORTED probes which requested flags are available.
        // Readback reports only supported flags, without the probe bit itself.
        registered.flags &= flags;
        if (action->handler == SignalIgnore)
            ++signalIgnoreGeneration[signal - 1];
    }
    if (previous)
        *previous = old;
    return 0;
}

int SignalAction(int signal, const AndroidSignalAction* action, AndroidSignalAction* previous)
{
    return SetSignalAction(signal, action, previous, false);
}

uintptr_t SignalHandler(int signal, uintptr_t handler)
{
    const AndroidSignalAction action{SignalRestart, handler, 0, 0};
    AndroidSignalAction previous{};
    return SignalAction(signal, &action, &previous) == 0 ? previous.handler : UINTPTR_MAX;
}

const char* SignalDescription(int signal)
{
    // Bionic x86_64 uses Linux signal numbers and reserves signals 32 through 41.
    constexpr const char* descriptions[]{
        nullptr,
        "Hangup",
        "Interrupt",
        "Quit",
        "Illegal instruction",
        "Trap",
        "Aborted",
        "Bus error",
        "Floating point exception",
        "Killed",
        "User signal 1",
        "Segmentation fault",
        "User signal 2",
        "Broken pipe",
        "Alarm clock",
        "Terminated",
        "Stack fault",
        "Child exited",
        "Continue",
        "Stopped (signal)",
        "Stopped",
        "Stopped (tty input)",
        "Stopped (tty output)",
        "Urgent I/O condition",
        "CPU time limit exceeded",
        "File size limit exceeded",
        "Virtual timer expired",
        "Profiling timer expired",
        "Window size changed",
        "I/O possible",
        "Power failure",
    };
    if (signal > 0 && signal < static_cast<int>(std::size(descriptions)))
        return descriptions[signal];
    thread_local char message[48];
    if (signal >= 42 && signal <= 64)
        std::snprintf(message, sizeof(message), "Real-time signal %d", signal - 42);
    else
        std::snprintf(message, sizeof(message), "Unknown signal %d", signal);
    return message;
}
}

int AndroidFutexRequeue(uint32_t& source, unsigned operation, uint32_t wake, uint32_t move, uint32_t* destination,
                        uint32_t compare)
{
    const auto command = operation & ~128u;
    if (command != 3 && command != 4)
        return -38;
    if (!destination || reinterpret_cast<uintptr_t>(destination) % alignof(uint32_t) || wake > INT_MAX ||
        move > INT_MAX)
        return -Invalid;
    const std::lock_guard lock(futexMutex);
    if (command == 4 && std::atomic_ref<uint32_t>(source).load() != compare)
        return -Again;
    const auto [first, last] = futexWaiters.equal_range(&source);
    unsigned woken = 0, moved = 0;
    // A destination between source and the original end of this range can insert
    // entries before last. Those entries have already been moved.
    for (auto entry = first; entry != last && entry->first == &source && (woken < wake || moved < move);) {
        auto current = entry++;
        auto& waiter = *current->second;
        if (waiter.notified)
            continue;
        if (woken < wake) {
            waiter.notified = true;
            waiter.cv.notify_one();
            ++woken;
        }
        else {
            if (destination != &source) {
                auto node = futexWaiters.extract(current);
                node.key() = destination;
                waiter.entry = futexWaiters.insert(std::move(node));
            }
            ++moved;
        }
    }
    return static_cast<int>(woken + moved);
}

int AndroidFutex(uint32_t& word, unsigned operation, uint32_t value, const AndroidTimespec* timeout, uint32_t mask)
{
    constexpr unsigned Private = 128, Realtime = 256;
    const auto command = operation & ~(Private | Realtime);
    const bool bitset = command == 9 || command == 10;
    if (command != 0 && command != 1 && !bitset)
        return -38; // ENOSYS
    if ((operation & Realtime) && command != 9)
        return -38;
    if (!bitset)
        mask = UINT32_MAX;
    if (!mask)
        return -Invalid;
    std::unique_lock lock(futexMutex);
    if (command == 1 || command == 10) {
        if (value > INT_MAX)
            return -Invalid;
        uint32_t notified = 0;
        const auto [first, last] = futexWaiters.equal_range(&word);
        for (auto entry = first; entry != last && notified < value; ++entry) {
            auto& waiter = *entry->second;
            if (waiter.notified || !(mask & waiter.mask))
                continue;
            waiter.notified = true;
            waiter.cv.notify_one();
            ++notified;
        }
        return static_cast<int>(notified);
    }
    if (timeout && (timeout->seconds < 0 || timeout->nanos < 0 || timeout->nanos >= 1'000'000'000))
        return -Invalid;
    if (std::atomic_ref<uint32_t>(word).load() != value)
        return -Again;

    const int clock = operation & Realtime ? 0 : 1;
    AndroidTimespec deadline{};
    if (timeout) {
        deadline = *timeout;
        if (command == 0) {
            AndroidTimespec now{};
            AndroidClock(clock, &now);
            const int64_t nanos = deadline.nanos + now.nanos;
            const int64_t add = now.seconds + nanos / 1'000'000'000;
            deadline.seconds = std::min(deadline.seconds, INT64_MAX - add) + add;
            deadline.nanos = nanos % 1'000'000'000;
        }
    }

    FutexWaiter waiter;
    waiter.mask = mask;
    waiter.entry = futexWaiters.emplace(&word, &waiter);
    auto* previousWaiter = std::exchange(signalThread.futex, &waiter);
    const auto ready = [&] {
        return waiter.notified || signalThread.Ready();
    };
    int result = -TimedOut;
    try {
        for (;;) {
            if (waiter.notified) {
                result = 0;
                break;
            }
            if (signalThread.Ready()) {
                lock.unlock();
                const auto restart = DeliverPendingSignals();
                lock.lock();
                if (restart && !*restart) {
                    result = -EINTR;
                    break;
                }
                continue;
            }
            if (!timeout) {
                waiter.cv.wait(lock, ready);
            }
            else {
                AndroidTimespec now{};
                AndroidClock(clock, &now);
                if (deadline.seconds < now.seconds || (deadline.seconds == now.seconds && deadline.nanos <= now.nanos))
                    break;
                const uint64_t seconds = uint64_t(deadline.seconds) - uint64_t(now.seconds);
                const unsigned cap = clock == 0 ? 1 : 86400;
                auto delay = std::chrono::nanoseconds(std::chrono::seconds(cap));
                if (seconds <= cap) {
                    delay = std::min(delay, std::chrono::seconds(seconds) +
                                                std::chrono::nanoseconds(deadline.nanos - now.nanos));
                }
                waiter.cv.wait_for(lock, delay, ready);
            }
        }
    }
    catch (...) {
        if (!lock.owns_lock())
            lock.lock();
        signalThread.futex = previousWaiter;
        futexWaiters.erase(waiter.entry);
        throw;
    }
    signalThread.futex = previousWaiter;
    futexWaiters.erase(waiter.entry);
    return result;
}

void RegisterNativeSignalThread()
{
    if (signalThread.id)
        return;
    const std::lock_guard lock(signalMutex);
    signalThread.id = GetCurrentThreadId();
    signalThread.next = signalThreads;
    signalThreads = &signalThread;
}

void RegisterArm64SignalThread()
{
    signalThread.arm64 = true;
    RegisterNativeSignalThread();
}

extern "C" uint64_t StoneGetAndroidSignalMask() noexcept
{
    return signalThread.mask;
}

bool DeliverAndroidHardwareSignal(const EXCEPTION_RECORD& fault, CONTEXT& machine)
{
    int signal = 0, code = 0;
    switch (fault.ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: {
        signal = 11;
        MEMORY_BASIC_INFORMATION region{};
        code = fault.NumberParameters >= 2 &&
                       VirtualQuery(reinterpret_cast<void*>(fault.ExceptionInformation[1]), &region, sizeof(region)) &&
                       region.State == MEM_COMMIT
                   ? 2
                   : 1;
        break;
    }
    case EXCEPTION_IN_PAGE_ERROR:
        signal = 7;
        code = 2;
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        signal = 4;
        code = 1;
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        signal = 8;
        code = 1;
        break;
    case EXCEPTION_INT_OVERFLOW:
        signal = 8;
        code = 2;
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        signal = 8;
        code = 3;
        break;
    case EXCEPTION_FLT_OVERFLOW:
        signal = 8;
        code = 4;
        break;
    case EXCEPTION_FLT_UNDERFLOW:
        signal = 8;
        code = 5;
        break;
    case EXCEPTION_FLT_INEXACT_RESULT:
        signal = 8;
        code = 6;
        break;
    case EXCEPTION_FLT_INVALID_OPERATION:
        signal = 8;
        code = 7;
        break;
    case EXCEPTION_BREAKPOINT:
        signal = 5;
        code = 1;
        break;
    default:
        return false;
    }
    if (signalThread.mask.load() & SignalBit(signal))
        return false;
    AndroidSignalAction action;
    {
        const std::lock_guard lock(signalMutex);
        action = TakeSignalAction(signal);
    }
    if (action.handler <= SignalIgnore)
        return false;
    RestoreSignalMask restore;
    signalThread.mask |= action.mask & ~UnmaskableSignals;
    if (!(action.flags & SignalNoDefer))
        signalThread.mask |= SignalBit(signal);
    try {
        alignas(16) AndroidSignalContext context;
        CaptureAndroidSignalContext(context, machine, restore.saved);
        AndroidSignalInfo info{signal, 0, code, 0, {}};
        const auto address = fault.NumberParameters >= 2 ? fault.ExceptionInformation[1]
                                                         : reinterpret_cast<uintptr_t>(fault.ExceptionAddress);
        info.detail[0] = address;
        if (signal == 11 || signal == 7) {
            context.machine.registers[19] = 4 | (code == 2 ? 1 : 0) |
                                            (fault.NumberParameters && fault.ExceptionInformation[0] == 1 ? 2 : 0) |
                                            (fault.NumberParameters && fault.ExceptionInformation[0] == 8 ? 16 : 0);
            context.machine.registers[20] = 14; // x86 page-fault trap.
            context.machine.registers[22] = address;
        }
        if (action.flags & SignalInfo) {
            const auto callback = reinterpret_cast<void (*)(int, AndroidSignalInfo*, AndroidSignalContext*)>(
                MakeNativeAndroidThunk(action.handler, "(ILL)V"));
            callback(signal, &info, &context);
        }
        else
            CallAndroidCallback(action.handler, reinterpret_cast<void*>(static_cast<intptr_t>(signal)));
        RestoreAndroidSignalContext(machine, context);
        restore.saved = context.mask;
        return true;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "[android-native] signal dispatch: %s\n", error.what());
        return false;
    }
}

uint64_t ExchangeAndroidSignalMask(uint64_t mask)
{
    return signalThread.mask.exchange(mask & ~UnmaskableSignals);
}

bool DeliverAndroidArm64MemoryFault(uintptr_t address)
{
    constexpr int signal = 11;
    if (!signalThread.arm64 || (signalThread.mask.load() & SignalBit(signal)))
        return false;
    AndroidSignalAction action;
    {
        const std::lock_guard lock(signalMutex);
        action = TakeSignalAction(signal);
    }
    if (action.handler <= SignalIgnore)
        return false;
    RestoreSignalMask restore;
    signalThread.mask |= action.mask & ~UnmaskableSignals;
    if (!(action.flags & SignalNoDefer))
        signalThread.mask |= SignalBit(signal);
    MEMORY_BASIC_INFORMATION region{};
    const int code =
        VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) && region.State == MEM_COMMIT ? 2 : 1;
    const bool delivered =
        RunArm64MemoryFault(action.handler, address, code, restore.saved, (action.flags & SignalInfo) != 0);
    restore.saved &= ~UnmaskableSignals;
    return delivered;
}

extern "C" void StoneRestoreAndroidSignalMask(uint64_t mask) noexcept
{
    signalThread.mask = mask & ~UnmaskableSignals;
}

bool PollArm64Signals()
{
    return DeliverPendingSignals().has_value();
}

int AndroidArm64SuspendSignals(uint64_t mask)
{
    const RestoreSignalMask restore;
    signalThread.mask = mask & ~UnmaskableSignals;
    for (;;) {
        {
            std::unique_lock lock(signalMutex);
            signalThread.queued.wait(lock, [] {
                return signalThread.Ready() != 0;
            });
        }
        if (DeliverPendingSignals()) {
            errno = EINTR;
            return -1;
        }
    }
}

int RunAndroidOnce(int& state, const std::function<void()>& callback)
{
    std::atomic_ref<int> progress(state);
    for (;;) {
        int expected = 0;
        if (progress.compare_exchange_strong(expected, 1)) {
            try {
                callback();
            }
            catch (...) {
                progress.store(0);
                progress.notify_all();
                throw;
            }
            progress.store(2);
            progress.notify_all();
            return 0;
        }
        if (expected == 2)
            return 0;
        if (expected != 1)
            return Invalid;
        progress.wait(1);
    }
}

int CreateAndroidKey(int& key, uintptr_t destructor)
{
    return CreateKey(&key, destructor);
}

int DeleteAndroidKey(int key)
{
    return DeleteKey(key);
}

void* GetAndroidThreadValue(int key)
{
    return GetKey(key);
}

int SetAndroidThreadValue(int key, void* value)
{
    return SetKey(key, value);
}

int AndroidArm64Sigaction(int signal, const AndroidSignalAction* action, AndroidSignalAction* previous)
{
    return SetSignalAction(signal, action, previous, true);
}

uintptr_t AndroidThreadImport(std::string_view name, bool hostAbi)
{
    // Bionic's signal-set entry points have the same layout on both LP64 ABIs.
    if (name == "sigemptyset64")
        return AndroidThreadImport("sigemptyset", hostAbi);
    if (name == "sigfillset64")
        return AndroidThreadImport("sigfillset", hostAbi);
    if (name == "sigaddset64")
        return AndroidThreadImport("sigaddset", hostAbi);
    if (name == "sigdelset64")
        return AndroidThreadImport("sigdelset", hostAbi);
    if (name == "sigismember64")
        return AndroidThreadImport("sigismember", hostAbi);
    if (name == "sigpending64")
        return AndroidThreadImport("sigpending", hostAbi);
    if (name == "sigsuspend64")
        return AndroidThreadImport("sigsuspend", hostAbi);
    if (name == "pthread_sigmask64")
        return AndroidThreadImport("pthread_sigmask", hostAbi);
    if (name == "sigprocmask64")
        return AndroidThreadImport("sigprocmask", hostAbi);
#define BIND(symbol, ...)                                                                                              \
    if (name == #symbol) {                                                                                             \
        const auto target = reinterpret_cast<uintptr_t>(__VA_ARGS__);                                                  \
        return hostAbi ? target : BridgeAndroidImport(name, target);                                                   \
    }
    BIND(
        getpriority, +[](int which, unsigned id) {
            return ThreadNiceness(false, which, id, 0);
        });
    BIND(
        setpriority, +[](int which, unsigned id, int nice) {
            return ThreadNiceness(true, which, id, nice);
        });
    BIND(pthread_attr_init, &AttrInit);
    BIND(pthread_attr_setdetachstate, &AttrDetach);
    BIND(pthread_attr_setstacksize, &AttrStackSize);
    BIND(
        pthread_attr_destroy, +[](AndroidThreadAttr*) {
            return 0;
        });
    BIND(
        pthread_attr_setschedpolicy, +[](AndroidThreadAttr* attr, int policy) {
            attr->policy = policy;
            return 0;
        });
    BIND(
        pthread_attr_setschedparam, +[](AndroidThreadAttr* attr, const int* priority) {
            attr->priority = *priority;
            return 0;
        });
    BIND(
        pthread_attr_getschedpolicy, +[](const AndroidThreadAttr* attr, int* policy) {
            *policy = attr->policy;
            return 0;
        });
    BIND(
        pthread_attr_getschedparam, +[](const AndroidThreadAttr* attr, int* priority) {
            *priority = attr->priority;
            return 0;
        });
    BIND(
        pthread_getattr_np, +[](uint64_t id, AndroidThreadAttr* attr) {
            if (id != GetCurrentThreadId())
                return NotSupported;
            ULONG_PTR low = 0, high = 0;
            GetCurrentThreadStackLimits(&low, &high);
            *attr = {};
            attr->stackBase = reinterpret_cast<void*>(low);
            attr->stackSize = high - low;
            return 0;
        });
    BIND(
        pthread_attr_getstack, +[](const AndroidThreadAttr* attr, void** base, size_t* size) {
            *base = attr->stackBase;
            *size = attr->stackSize;
            return 0;
        });
    BIND(
        pthread_attr_getstacksize, +[](const AndroidThreadAttr* attr, size_t* size) {
            *size = attr->stackSize;
            return 0;
        });
    BIND(
        pthread_attr_getguardsize, +[](const AndroidThreadAttr* attr, size_t* size) {
            *size = attr->guardSize;
            return 0;
        });
    BIND(pthread_setname_np, &SetThreadName);
    BIND(pthread_getname_np, &GetThreadName);
    BIND(pthread_create, &CreateThreadForGuest);
    BIND(pthread_join, &JoinThread);
    BIND(pthread_detach, &DetachThread);
    if (!hostAbi) {
        BIND(pthread_exit, &ExitNativeThread);
    }
    BIND(pthread_kill, &KillThread);
    if (!hostAbi) {
        BIND(
            sigsuspend, +[](const uint64_t* mask) {
                if (!mask) {
                    errno = EFAULT;
                    return -1;
                }
                RegisterNativeSignalThread();
                return AndroidArm64SuspendSignals(*mask);
            });
    }
    BIND(__pthread_cleanup_push, &PushCleanup);
    BIND(__pthread_cleanup_pop, &PopCleanup);
    BIND(__cxa_thread_atexit_impl, &RegisterThreadDestructor);
    BIND(
        pthread_self, +[]() -> uint64_t {
            return GetCurrentThreadId();
        });
    BIND(
        pthread_getcpuclockid, +[](uint64_t id, int* clock) {
            if (!clock)
                return EINVAL;
            if (id >= (uint64_t{1} << 29))
                return NoProcess;
            const auto thread = OpenGuestThread(id, THREAD_QUERY_LIMITED_INFORMATION);
            if (!thread)
                return NoProcess;
            CloseHandle(thread);
            *clock = static_cast<int>((~static_cast<uint32_t>(id) << 3) | 6u);
            return 0;
        });
    BIND(
        pthread_gettid_np, +[](uint64_t id) -> int {
            if (id == GetCurrentThreadId())
                return static_cast<int>(id);
            if (id <= MAXDWORD) {
                const auto thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(id));
                if (thread) {
                    const bool ownThread = GetProcessIdOfThread(thread) == GetCurrentProcessId();
                    CloseHandle(thread);
                    if (ownThread)
                        return static_cast<int>(id);
                }
            }
            errno = NoProcess;
            return -1;
        });
    BIND(
        pthread_equal, +[](uint64_t first, uint64_t second) -> int {
            return first == second;
        });
    BIND(pthread_key_create, &CreateKey);
    BIND(pthread_key_delete, &DeleteKey);
    BIND(pthread_getspecific, &GetKey);
    BIND(pthread_setspecific, &SetKey);
    BIND(pthread_once, &Once);
    BIND(pthread_mutex_init, &MutexInit);
    BIND(pthread_mutex_destroy, &MutexDestroy);
    BIND(pthread_mutex_lock, &MutexLock);
    BIND(pthread_mutex_trylock, &MutexTryLock);
    BIND(pthread_mutex_unlock, &MutexUnlock);
    BIND(
        pthread_mutexattr_init, +[](int32_t* attr) {
            *attr = 0;
            return 0;
        });
    BIND(
        pthread_mutexattr_settype, +[](int32_t* attr, int type) {
            if (type < 0 || type > 2)
                return Invalid;
            *attr = type;
            return 0;
        });
    BIND(
        pthread_mutexattr_destroy, +[](int32_t*) {
            return 0;
        });
    BIND(pthread_cond_init, &CondInit);
    BIND(pthread_cond_destroy, &CondDestroy);
    BIND(
        pthread_condattr_init, +[](int64_t* attr) {
            *attr = 0;
            return 0;
        });
    BIND(
        pthread_condattr_setclock, +[](int64_t* attr, int clock) {
            if (clock != 0 && clock != 1)
                return Invalid;
            *attr = (*attr & ~2ll) | (clock ? 2ll : 0ll);
            return 0;
        });
    BIND(
        pthread_condattr_destroy, +[](int64_t*) {
            return 0;
        });
    BIND(
        pthread_cond_wait, +[](int* cond, void* mutex) {
            return CondWait(cond, mutex, nullptr);
        });
    BIND(pthread_cond_timedwait, &CondWait);
    BIND(pthread_cond_clockwait, &CondClockWait);
    BIND(
        pthread_cond_signal, +[](int* cond) {
            return CondNotify(cond, false);
        });
    BIND(
        pthread_cond_broadcast, +[](int* cond) {
            return CondNotify(cond, true);
        });
    BIND(pthread_rwlock_init, &RwInit);
    BIND(pthread_rwlock_destroy, &RwDestroy);
    BIND(pthread_rwlock_rdlock, &RwAcquire<false, false>);
    BIND(pthread_rwlock_tryrdlock, &RwAcquire<false, true>);
    BIND(pthread_rwlock_wrlock, &RwAcquire<true, false>);
    BIND(pthread_rwlock_trywrlock, &RwAcquire<true, true>);
    BIND(pthread_rwlock_unlock, &RwUnlock);
    BIND(sem_init, &SemInit);
    BIND(
        sem_destroy, +[](unsigned*) {
            return 0;
        });
    BIND(
        sem_getvalue, +[](unsigned* count, int* result) {
            *result = static_cast<int>(std::atomic_ref<unsigned>(*count).load());
            return 0;
        });
    BIND(sem_post, &SemPost);
    BIND(
        sem_wait, +[](unsigned* value) {
            return WaitSemaphore(value, false);
        });
    BIND(
        sem_trywait, +[](unsigned* value) {
            return WaitSemaphore(value, true);
        });
    BIND(
        sem_timedwait, +[](unsigned* value, const AndroidTimespec* deadline) {
            return SemClockWait(value, 0, deadline);
        });
    BIND(sem_clockwait, &SemClockWait);
    BIND(pthread_getschedparam, &GetScheduling);
    BIND(pthread_setschedparam, &SetScheduling);
    BIND(
        pthread_setschedprio, +[](uint64_t id, int priority) {
            return SetScheduling(id, 0, &priority);
        });
    BIND(sched_getaffinity, &GetCpuAffinity);
    BIND(__sched_cpucount, &CountCpus);
    BIND(sched_getscheduler, &GetScheduler);
    BIND(sched_getparam, &GetSchedulingParam);
    BIND(sched_setscheduler, &SetScheduler);
    BIND(sched_get_priority_max, &PriorityBound<true>);
    BIND(sched_get_priority_min, &PriorityBound<false>);
    BIND(
        sigemptyset, +[](uint64_t* mask) {
            *mask = 0;
            return 0;
        });
    BIND(
        sigfillset, +[](uint64_t* mask) {
            *mask = UINT64_MAX;
            return 0;
        });
    BIND(sigaddset, &SignalSet<true>);
    BIND(sigdelset, &SignalSet<false>);
    BIND(
        sigismember, +[](const uint64_t* mask, int signal) {
            if (signal <= 0 || signal > 64) {
                errno = EINVAL;
                return -1;
            }
            return static_cast<int>((*mask >> (signal - 1)) & 1);
        });
    BIND(pthread_sigmask, &SignalMask);
    BIND(sigwaitinfo, &WaitSignalInfo);
    BIND(sigwaitinfo64, &WaitSignalInfo);
    BIND(
        tgkill, +[](int pid, int tid, int signal) {
            if (static_cast<DWORD>(pid) != GetCurrentProcessId()) {
                errno = NoProcess;
                return -1;
            }
            const int error = KillThread(static_cast<uint64_t>(tid), signal);
            if (error) {
                errno = error;
                return -1;
            }
            return 0;
        });
    BIND(
        sigqueue, +[](int pid, int signal, uint64_t value) {
            if (static_cast<DWORD>(pid) != GetCurrentProcessId()) {
                errno = NoProcess;
                return -1;
            }
            if (signal <= 0 || signal > SignalCount) {
                errno = Invalid;
                return -1;
            }
            if (IsArm64TranslationEnabled())
                RegisterArm64SignalThread();
            else
                RegisterNativeSignalThread();
            int error;
            {
                const std::lock_guard lock(signalMutex);
                auto* target = &signalThread;
                for (auto* thread = signalThreads; thread; thread = thread->next) {
                    if (!(thread->mask & SignalBit(signal))) {
                        target = thread;
                        break;
                    }
                }
                error = QueueSignal(*target, signal, {-1, value}); // SI_QUEUE.
            }
            if (error) {
                errno = error;
                return -1;
            }
            DeliverPendingSignals();
            return 0;
        });
    BIND(
        sigprocmask, +[](int how, const uint64_t* mask, uint64_t* previous) {
            const int result = SignalMask(how, mask, previous);
            if (!result)
                return 0;
            errno = result;
            return -1;
        });
    BIND(sigaction, &SignalAction);
    BIND(sigaction64, &SignalAction);
    BIND(signal, &SignalHandler);
    BIND(raise, &RaiseSignal);
    BIND(
        sigpending, +[](uint64_t* mask) {
            *mask = PendingSignals();
            return 0;
        });
    BIND(strsignal, &SignalDescription);
#undef BIND
    return 0;
}
}
