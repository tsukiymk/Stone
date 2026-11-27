#include "stub-libc.h"
#include "filesystem/AndroidFileObserver.h"
#include "abi/AndroidLibraryImports.h"
#include "filesystem/AndroidSystemFiles.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "loader/ElfImage.h"
#include "stub-libandroid-fonts.h"
#include "stub-libc-sockets.h"
#include "stub-libc-inotify.h"
#include "stub-libc-users.h"
#include "memory/GuestMappings.h"
#include "stub-libc-interfaces.h"
#include "stub-libc-symlinks.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>
#include <bcrypt.h>
#include <direct.h>
#include <fcntl.h>
#include <icu.h>
#include <io.h>
#include <intrin.h>
#include <malloc.h>
#include <locale.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <filesystem>
#include <sys/stat.h>
#include <sys/utime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <memory>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace stone::windows {
std::string AndroidGuestFilePath(std::string_view path)
{
    if (path.size() < 3 || path[1] != ':' || (path[2] != '\\' && path[2] != '/'))
        return std::string(path);
    const auto drive = static_cast<unsigned char>(path[0]) | 0x20;
    if (drive < 'a' || drive > 'z')
        return std::string(path);
    // A leading slash keeps the drive path absolute to Android libc++.
    std::string result = "/";
    result += path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

extern "C" void AndroidVarargsScan();
extern "C" void AndroidVarargsStdout();
extern "C" void AndroidVarargsDescriptor();
extern "C" void AndroidVarargsFormat();
extern "C" void AndroidVarargsUnboundedFormat();
extern "C" int StoneAndroidUnboundedFormat(char* buffer, const char* format, AndroidVaList* args);
extern "C" void AndroidVarargsWideFormat();
extern "C" void AndroidParseLongDouble();
extern "C" void AndroidWideParseLongDouble();
extern "C" void AndroidVarargsAllocateFormat();
extern "C" void AndroidSetJmp();
extern "C" void AndroidSignalSetJmp();
extern "C" void AndroidLongJmp();
extern "C" void AndroidLongLongDiv();

// Bionic x86_64 uses the SysV register save area and an overflow stack area.
uint64_t AndroidVaList::next(bool floating)
{
    uint64_t value;
    if (floating && floatOffset < 176) {
        std::memcpy(&value, registers + floatOffset, sizeof(value));
        floatOffset += 16;
    }
    else if (!floating && integerOffset < 48) {
        std::memcpy(&value, registers + integerOffset, sizeof(value));
        integerOffset += 8;
    }
    else {
        std::memcpy(&value, stack, sizeof(value));
        stack += 8;
    }
    return value;
}

namespace {
std::filesystem::path systemCa;
std::filesystem::path conscryptCa;
std::filesystem::path procDirectory;
std::string programName = "Stone";
std::mutex processNameMutex;
std::string processName = "Stone";
int OpenProcCommandLine(int flags);

bool IsOwnProcPath(std::string_view path, std::string_view file)
{
    constexpr std::string_view prefix = "/proc/";
    if (!path.starts_with(prefix) || !path.ends_with(file))
        return false;
    path.remove_prefix(prefix.size());
    if (path.size() <= file.size())
        return false;
    path.remove_suffix(file.size());
    if (path == "self")
        return true;
    if (path.front() == '0')
        return false;
    DWORD pid = 0;
    const auto end = path.data() + path.size();
    const auto parsed = std::from_chars(path.data(), end, pid);
    return parsed.ec == std::errc{} && parsed.ptr == end && pid == GetCurrentProcessId();
}

std::mutex randomDevicesMutex;
std::unordered_set<int> randomDevices;
constexpr int GuestDescriptorLimit = 8192; // UCRT low-I/O table size.
constexpr int ProcessCpuClock = 2, ThreadCpuClock = 3;
constexpr uint64_t FdOwnerValueMask = (uint64_t{1} << 56) - 1;
constexpr unsigned FileOwner = 1, DirectoryOwner = 2;

enum FdSanLevel {
    FdSanDisabled,
    FdSanWarnOnce,
    FdSanWarnAlways,
    FdSanFatal
};

std::array<std::atomic<uint64_t>, GuestDescriptorLimit> fdOwnerTags{};
std::array<std::atomic<bool>, GuestDescriptorLimit> procCommandLines{};
std::atomic<int> fdSanLevel{FdSanFatal};

bool IsProcCommandLineDescriptor(int fd)
{
    return fd >= 0 && fd < GuestDescriptorLimit && procCommandLines[fd].load();
}

std::atomic<uint64_t>* FdOwnerTagSlot(int fd)
{
    return fd >= 0 && fd < GuestDescriptorLimit ? &fdOwnerTags[fd] : nullptr;
}

uint64_t FdOwnerTag(int fd)
{
    if (const auto* slot = FdOwnerTagSlot(fd))
        return slot->load();
    return 0;
}

uint64_t MakeFdOwnerTag(unsigned type, uint64_t value)
{
    if (type > 255)
        std::abort();
    return value ? (uint64_t{type} << 56) | (value & FdOwnerValueMask) : 0;
}

void ReportFdOwnerMismatch(int fd, uint64_t expected, uint64_t actual, const char* operation)
{
    auto level = fdSanLevel.load();
    if (level == FdSanDisabled)
        return;
    if (level == FdSanWarnOnce && !fdSanLevel.compare_exchange_strong(level, FdSanDisabled) && level == FdSanDisabled)
        return;
    const int saved = errno;
    std::fprintf(stderr, "[android-fdsan] %s fd %d with tag 0x%llx, actual tag 0x%llx\n", operation, fd,
                 static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual));
    if (level == FdSanFatal)
        std::abort();
    errno = saved;
}

void ExchangeFdOwnerTag(int fd, uint64_t expected, uint64_t replacement)
{
    const auto slot = FdOwnerTagSlot(fd);
    if (!slot)
        return;
    auto actual = expected;
    if (!slot->compare_exchange_strong(actual, replacement))
        ReportFdOwnerMismatch(fd, expected, actual, "retag");
}

void CheckFdOwnerClose(int fd, uint64_t expected)
{
    if (const auto slot = FdOwnerTagSlot(fd)) {
        const auto actual = slot->exchange(0);
        if (actual != expected)
            ReportFdOwnerMismatch(fd, expected, actual, "close");
    }
}

int CloseTaggedDescriptor(int fd, uint64_t expected)
{
    CheckFdOwnerClose(fd, expected);
    return CloseAndroidFd(fd);
}

struct AndroidMallInfo {
    uint64_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
};

static_assert(sizeof(AndroidMallInfo) == 80);

AndroidMallInfo* ReadHeapStatistics(AndroidMallInfo* output)
{
    *output = {};
    const auto heap = reinterpret_cast<HANDLE>(_get_heap_handle());
    if (!HeapLock(heap)) {
        errno = EIO;
        return output;
    }
    AndroidMallInfo stats{};
    std::array<bool, 256> regions{};
    std::array<uint64_t, 256> occupied{};
    PROCESS_HEAP_ENTRY entry{};
    while (HeapWalk(heap, &entry)) {
        if (entry.wFlags & PROCESS_HEAP_REGION) {
            regions[entry.iRegionIndex] = true;
            stats.arena += entry.Region.dwCommittedSize;
        }
        else if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
            occupied[entry.iRegionIndex] += uint64_t{entry.cbData} + entry.cbOverhead;
            stats.uordblks += entry.cbData;
        }
        else if (!(entry.wFlags & PROCESS_HEAP_UNCOMMITTED_RANGE)) {
            ++stats.ordblks;
            stats.fordblks += entry.cbData;
        }
    }
    const auto status = GetLastError();
    HeapUnlock(heap);
    if (status != ERROR_NO_MORE_ITEMS) {
        errno = EIO;
        return output;
    }
    for (size_t i = 0; i < regions.size(); ++i) {
        if (!regions[i])
            stats.hblkhd += occupied[i];
    }
    *output = stats;
    return output;
}

constexpr unsigned SealSeals = 0x01, SealShrink = 0x02, SealGrow = 0x04, SealWrite = 0x08, SealFutureWrite = 0x10,
                   SealExec = 0x20;
constexpr unsigned MemfdWriteSeals = SealWrite | SealFutureWrite;
constexpr unsigned KnownMemfdSeals = SealSeals | SealShrink | SealGrow | MemfdWriteSeals | SealExec;
constexpr unsigned TransferMemorySeals = SealSeals | SealShrink | SealGrow | SealExec;

struct MemoryFile {
    std::mutex mutex;
    std::string name;
    unsigned seals = SealSeals;
    unsigned mode = 0777;
    int mappingProtection = 7; // PROT_READ | PROT_WRITE | PROT_EXEC
    int status = 0;
    size_t views = 0;
    size_t writableViews = 0;
    // Imported descriptors carry a read-only file and a bounded section for writes.
    std::unique_ptr<void, decltype(&CloseHandle)> section{nullptr, &CloseHandle};
    bool distributed = false;
};

struct MemoryFileRegistry {
    std::mutex mutex;
    std::unordered_map<int, std::shared_ptr<MemoryFile>> entries;
};

// Unix socket queues can discard their last memory-file references during
// process teardown. Keep the registry alive through those destructors.
auto& memoryFileRegistry = *new MemoryFileRegistry;
auto& memoryFilesMutex = memoryFileRegistry.mutex;
auto& memoryFiles = memoryFileRegistry.entries;

std::shared_ptr<MemoryFile> FindMemoryFile(int fd)
{
    const std::lock_guard lock(memoryFilesMutex);
    const auto found = memoryFiles.find(fd);
    return found == memoryFiles.end() ? nullptr : found->second;
}

std::shared_ptr<MemoryFile> LockMemoryFile(int fd, std::unique_lock<std::mutex>& lock)
{
    const std::lock_guard tableLock(memoryFilesMutex);
    const auto found = memoryFiles.find(fd);
    if (found == memoryFiles.end())
        return nullptr;
    lock = std::unique_lock(found->second->mutex);
    return found->second;
}

bool IsRandomDevice(std::string_view path)
{
    return path == "/dev/urandom" || path == "/dev/random";
}

bool IsReadOnlyImagePath(const std::filesystem::path& path)
{
    return IsAndroidSystemCachePath(path) || IsAndroidImageFontPath(path);
}

std::filesystem::path MapGuestPath(const char* path, bool writable)
{
    if (!path)
        throw AndroidSystemPathError{EFAULT};
    std::string_view name(path);
    if (writable && IsOwnProcPath(name, "/cmdline"))
        throw AndroidSystemPathError{EACCES};
    if (!procDirectory.empty() && (name == "/proc/meminfo" || name == "/proc/cpuinfo")) {
        if (writable)
            throw AndroidSystemPathError{EROFS};
        return procDirectory / name.substr(6);
    }
    if (!procDirectory.empty() &&
        (name == "/sys/devices/system/cpu/present" || name == "/sys/devices/system/cpu/possible" ||
         name == "/sys/devices/system/cpu/online")) {
        if (writable)
            throw AndroidSystemPathError{EROFS};
        return procDirectory / "cpu-list";
    }
    if (name.size() >= 3 && name.front() == '/' && name[2] == ':' && (name.size() == 3 || name[3] == '/')) {
        const auto drive = static_cast<unsigned char>(name[1]) | 0x20;
        if (drive >= 'a' && drive <= 'z') {
            if (name.size() == 3)
                return std::filesystem::path(std::string(name.substr(1)) + "/");
            name.remove_prefix(1);
        }
    }
    const std::filesystem::path host(std::u8string_view(reinterpret_cast<const char8_t*>(name.data()), name.size()));
    if (const auto data = UnresolvedAndroidDataFile(host)) {
        // /data/media/0 is a compatibility mount even in profiles that never
        // had a physical /data/media parent. Let link traversal reach that mount.
        if (name == "/data/media" && !std::filesystem::exists(*data))
            throw AndroidSystemPathError{ENOENT};
        return *data;
    }
    const auto guest = AndroidSystemGuestPath(host);
    if (writable && ((guest && IsAndroidSystemGuestReadOnly(*guest)) || IsReadOnlyImagePath(host)))
        throw AndroidSystemPathError{EROFS};
    const auto caPath = [&](std::string_view prefix, const std::filesystem::path& directory) -> std::filesystem::path {
        if (directory.empty() || !name.starts_with(prefix))
            return {};
        const auto suffix = name.substr(prefix.size());
        if (suffix.empty() || suffix == "/")
            return directory;
        if (suffix.front() != '/')
            return {};
        const auto file = suffix.substr(1);
        if (file.size() < 10 || file[8] != '.' ||
            file.substr(0, 8).find_first_not_of("0123456789abcdef") != file.npos ||
            file.substr(9).find_first_not_of("0123456789") != file.npos)
            return {};
        return directory / std::filesystem::path(file);
    };
    if (auto mapped = caPath("/system/etc/security/cacerts", systemCa); !mapped.empty())
        return mapped;
    if (auto mapped = caPath("/apex/com.android.conscrypt/cacerts", conscryptCa); !mapped.empty())
        return mapped;
    if (auto mapped = AndroidSystemFontPath(name); !mapped.empty())
        return mapped;
    return guest ? ResolveAndroidSystemFile(*guest) : host;
}

std::filesystem::path GuestPath(const char* path, bool writable = false, bool followFinal = true)
{
    if (!path)
        throw AndroidSystemPathError{EFAULT};
    const std::string_view name(path);
    if (writable && IsOwnProcPath(name, "/cmdline"))
        throw AndroidSystemPathError{EACCES};
    if (name == "/dev/null" ||
        (!procDirectory.empty() &&
         (name == "/proc/meminfo" || name == "/proc/cpuinfo" || name == "/sys/devices/system/cpu/present" ||
          name == "/sys/devices/system/cpu/possible" || name == "/sys/devices/system/cpu/online")))
        return MapGuestPath(path, writable);
    auto mapped = ResolveAndroidSymbolicLinks(
        std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(name.data()), name.size())), writable,
        followFinal, &MapGuestPath);
    if (writable && IsReadOnlyImagePath(mapped))
        throw AndroidSystemPathError{EROFS};
    CheckAndroidSharedStorageTarget(
        std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(name.data()), name.size())), mapped);
    return mapped;
}

// Bionic's errno numbers agree with the CRT for the common file errors.
int* AndroidErrno()
{
    return _errno();
}

thread_local std::array<char, 4096> basenameStorage, dirnameStorage;

size_t UnsupportedConversion()
{
    errno = 38;
    return size_t(-1);
}

struct NativeBlock {
    size_t alignment;
    size_t size;
};

// A flat ownership index avoids a host allocation and debug-iterator locking
// for every guest malloc/free. Its caller holds NativeBlocks::mutex.
class NativeBlockTable {
    struct Slot {
        void* memory = nullptr;
        NativeBlock block{};
    };

    std::unique_ptr<Slot[]> slots_;
    size_t capacity_ = 0, count_ = 0;

    static size_t Hash(void* memory) noexcept
    {
        uint64_t value = reinterpret_cast<uintptr_t>(memory);
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
        return static_cast<size_t>(value ^ (value >> 31));
    }

    size_t Locate(void* memory) const noexcept
    {
        size_t index = Hash(memory) & (capacity_ - 1);
        while (slots_[index].memory && slots_[index].memory != memory)
            index = (index + 1) & (capacity_ - 1);
        return index;
    }

    NativeBlock* InsertReady(void* memory, NativeBlock block) noexcept
    {
        const size_t index = Locate(memory);
        if (slots_[index].memory)
            return nullptr;
        slots_[index] = {memory, block};
        ++count_;
        return &slots_[index].block;
    }

    void Grow()
    {
        if (capacity_ > SIZE_MAX / sizeof(Slot) / 2)
            throw std::bad_alloc{};
        const size_t capacity = capacity_ ? capacity_ * 2 : 1024;
        auto slots = std::make_unique<Slot[]>(capacity);
        for (size_t i = 0; i < capacity_; ++i) {
            if (!slots_[i].memory)
                continue;
            size_t index = Hash(slots_[i].memory) & (capacity - 1);
            while (slots[index].memory)
                index = (index + 1) & (capacity - 1);
            slots[index] = slots_[i];
        }
        slots_ = std::move(slots);
        capacity_ = capacity;
    }

public:
    NativeBlock* Find(void* memory) noexcept
    {
        if (!capacity_)
            return nullptr;
        const auto index = Locate(memory);
        return slots_[index].memory ? &slots_[index].block : nullptr;
    }

    NativeBlock* Insert(void* memory, NativeBlock block)
    {
        if (count_ >= capacity_ - capacity_ / 4)
            Grow();
        return InsertReady(memory, block);
    }

    bool Erase(void* memory) noexcept
    {
        if (!capacity_)
            return false;
        size_t hole = Locate(memory);
        if (!slots_[hole].memory)
            return false;
        const auto mask = capacity_ - 1;
        // Shift the remaining probe cluster back across the hole. No tombstones
        // accumulate, including during repeated realloc/free churn.
        for (size_t next = (hole + 1) & mask; slots_[next].memory; next = (next + 1) & mask) {
            const auto home = Hash(slots_[next].memory) & mask;
            if (((next - home) & mask) >= ((next - hole) & mask)) {
                slots_[hole] = slots_[next];
                hole = next;
            }
        }
        slots_[hole] = {};
        --count_;
        return true;
    }

    void Replace(void* previous, void* memory, NativeBlock block) noexcept
    {
        // Capacity was reserved before CRT realloc consumed the old pointer.
        // Erasing and reinserting the same number of entries cannot allocate.
        if (!Erase(previous) || !InsertReady(memory, block))
            std::abort();
    }
};

// Track only guest allocator calls, not Windows UI/loader/bookkeeping memory.
// Aligned blocks still require _aligned_free; foreign CRT blocks remain valid.
struct NativeBlocks {
    std::recursive_mutex mutex;
    NativeBlockTable entries;
    uint64_t bytes = 0;
};

// Guest TLS destructors may release memory during process teardown.
auto& nativeBlocks = *new NativeBlocks;

bool RecordNativeBlock(void* memory, size_t alignment, size_t size)
{
    if (!memory)
        return false;
    try {
        const std::lock_guard lock(nativeBlocks.mutex);
        if (!nativeBlocks.entries.Insert(memory, NativeBlock{alignment, size}))
            std::abort();
        nativeBlocks.bytes += size;
        return true;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return false;
    }
}

AndroidMallInfo* ReadArtHeapStatistics(AndroidMallInfo* output)
{
    *output = {};
    const std::lock_guard lock(nativeBlocks.mutex);
    // Bionic ART's GC uses allocated bytes, not arena/free-list details.
    output->uordblks = nativeBlocks.bytes;
    return output;
}

void* AllocateAligned(size_t alignment, size_t size)
{
    if (!std::has_single_bit(alignment)) {
        errno = EINVAL;
        return nullptr;
    }
    alignment = std::max(alignment, alignof(void*));
    size = std::max<size_t>(size, 1);
    if (size > PTRDIFF_MAX || alignment > PTRDIFF_MAX - size) {
        errno = ENOMEM;
        return nullptr;
    }
    void* memory = _aligned_malloc(size, alignment);
    if (!memory)
        return nullptr;
    if (!RecordNativeBlock(memory, alignment, size)) {
        _aligned_free(memory);
        return nullptr;
    }
    return memory;
}

int PosixMemalign(void** result, size_t alignment, size_t size)
{
    if (!result || alignment < sizeof(void*) || !std::has_single_bit(alignment))
        return EINVAL;
    const int saved = errno;
    void* memory = AllocateAligned(alignment, size);
    const int error = memory ? 0 : errno;
    errno = saved;
    if (memory)
        *result = memory;
    return error;
}

size_t AllocationSize(void* memory)
{
    if (!memory)
        return 0;
    {
        const std::lock_guard lock(nativeBlocks.mutex);
        if (const auto block = nativeBlocks.entries.Find(memory))
            return block->size;
    }
    return _msize(memory);
}

int HostAddressFamily(int family)
{
    return family == 10 ? AF_INET6 : family == AF_INET6 ? -1 : family;
}

struct AndroidHostEntry {
    char* name;
    char** aliases;
    int family;
    int addressLength;
    char** addresses;
};

int* HostLookupError()
{
    thread_local int error = 0;
    return &error;
}

AndroidHostEntry* ConvertHostEntry(const hostent* entry)
{
    if (!entry) {
        switch (WSAGetLastError()) {
        case WSAHOST_NOT_FOUND:
            *HostLookupError() = 1;
            break;
        case WSATRY_AGAIN:
            *HostLookupError() = 2;
            break;
        case WSANO_DATA:
            *HostLookupError() = 4;
            break;
        default:
            *HostLookupError() = 3;
            break;
        }
        return nullptr;
    }
    thread_local AndroidHostEntry result{};
    const int family = entry->h_addrtype == AF_INET6 ? 10 : entry->h_addrtype;
    result = {entry->h_name, entry->h_aliases, family, entry->h_length, entry->h_addr_list};
    return &result;
}

AndroidHostEntry* GetHostByName(const char* name)
{
    if (!InitializeAndroidWinsock()) {
        *HostLookupError() = 3; // NO_RECOVERY
        return nullptr;
    }
#pragma warning(suppress : 4996)
    return ConvertHostEntry(gethostbyname(name));
}

AndroidHostEntry* GetHostByAddress(const void* address, uint32_t size, int family)
{
    const int hostFamily = HostAddressFamily(family);
    if (hostFamily < 0 || size > INT_MAX || !InitializeAndroidWinsock()) {
        *HostLookupError() = 3; // NO_RECOVERY
        return nullptr;
    }
#pragma warning(suppress : 4996)
    return ConvertHostEntry(gethostbyaddr(static_cast<const char*>(address), static_cast<int>(size), hostFamily));
}

int CompareGuest(void* callback, const void* left, const void* right)
{
    return static_cast<int>(
        CallAndroidCallback(reinterpret_cast<uintptr_t>(callback), const_cast<void*>(left), const_cast<void*>(right)));
}

void SortGuest(void* base, size_t count, size_t size, void* callback)
{
    qsort_s(base, count, size, CompareGuest, callback);
}

void* SearchGuest(const void* key, const void* base, size_t count, size_t size, void* callback)
{
    return bsearch_s(key, base, count, size, CompareGuest, callback);
}

int GuestPipe(int* descriptors, int flags)
{
    constexpr int CloseOnExec = 0x80000, Nonblocking = 0x800;
    if (!descriptors) {
        errno = EFAULT;
        return -1;
    }
    if (flags & ~(CloseOnExec | Nonblocking)) {
        errno = EINVAL;
        return -1;
    }
    int pipe[2];
    if (_pipe(pipe, 4096, _O_BINARY | ((flags & CloseOnExec) ? _O_NOINHERIT : 0)) != 0)
        return -1;
    if (flags & Nonblocking) {
        for (const int fd : pipe) {
            DWORD mode = PIPE_NOWAIT;
            if (SetNamedPipeHandleState(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), &mode, nullptr, nullptr))
                continue;
            _close(pipe[0]);
            _close(pipe[1]);
            errno = EIO;
            return -1;
        }
    }
    std::copy_n(pipe, 2, descriptors);
    return 0;
}

bool AndroidOpenWrites(int flags)
{
    constexpr int Access = 3, Create = 0x40, Truncate = 0x200, Append = 0x400;
    return (flags & (Access | Create | Truncate | Append)) != 0;
}

std::atomic<unsigned> fileCreationMask{0022};

int HostFileError(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        errno = ENOENT;
        break;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        errno = EACCES;
        break;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        errno = ENOSPC;
        break;
    case ERROR_TOO_MANY_OPEN_FILES:
        errno = EMFILE;
        break;
    case ERROR_DIR_NOT_EMPTY:
        errno = 39;
        break; // Android ENOTEMPTY.
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
        errno = EINVAL;
        break;
    default:
        errno = EIO;
        break;
    }
    return -1;
}

int OpenWideDescriptor(const wchar_t* path, int flags, int permission)
{
    constexpr int Create = 0x40, Exclusive = 0x80, Truncate = 0x200, Append = 0x400;
    constexpr int CloseOnExec = 0x80000, Access = 3;
    constexpr int Directory = 0x10000, NoFollow = 0x20000, Nonblocking = 0x800, LargeFile = 0x8000;
    if (flags & ~(Access | Create | Exclusive | Truncate | Append | CloseOnExec | Directory | NoFollow | Nonblocking |
                  LargeFile)) {
        errno = EINVAL;
        return -1;
    }
    if (AndroidOpenWrites(flags) && IsReadOnlyImagePath(path)) {
        errno = EROFS;
        return -1;
    }
    int mode = _O_BINARY | (flags & Access);
    if (flags & Create)
        mode |= _O_CREAT;
    if (flags & Exclusive)
        mode |= _O_EXCL;
    if (flags & Truncate)
        mode |= _O_TRUNC;
    if (flags & Append)
        mode |= _O_APPEND;
    if (flags & CloseOnExec)
        mode |= _O_NOINHERIT;
    if (std::wstring_view(path) == L"/dev/null")
        path = L"NUL";
    const auto attributes = GetFileAttributesW(path);
    // CREATE_NEW may otherwise try to open an LX reparse point and report an
    // unsupported tag instead of the existing directory entry.
    if (attributes != INVALID_FILE_ATTRIBUTES && (flags & (Create | Exclusive)) == (Create | Exclusive)) {
        errno = EEXIST;
        return -1;
    }
    if (attributes != INVALID_FILE_ATTRIBUTES && (flags & NoFollow) && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        errno = 40; // Android ELOOP; the Windows CRT uses a different number.
        return -1;
    }
    if (flags & Directory) {
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            errno = ENOENT;
            return -1;
        }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            errno = ENOTDIR;
            return -1;
        }
        const auto handle =
            CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            errno = EACCES;
            return -1;
        }
        const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), mode);
        if (fd < 0)
            CloseHandle(handle);
        return fd;
    }
    if ((flags & Access) == Access) {
        errno = EINVAL;
        return -1;
    }
    const auto effective = unsigned(permission) & ~fileCreationMask.load(std::memory_order_relaxed);
    const DWORD desired = (flags & Access) == 0   ? GENERIC_READ
                          : (flags & Access) == 1 ? GENERIC_WRITE
                                                  : GENERIC_READ | GENERIC_WRITE;
    const DWORD creation = (flags & Create)     ? ((flags & Exclusive)  ? CREATE_NEW
                                                   : (flags & Truncate) ? CREATE_ALWAYS
                                                                        : OPEN_ALWAYS)
                           : (flags & Truncate) ? TRUNCATE_EXISTING
                                                : OPEN_EXISTING;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, (flags & CloseOnExec) ? FALSE : TRUE};
    const auto handle =
        CreateFileW(path, desired, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security, creation,
                    (effective & 0200) ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_READONLY, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return HostFileError(GetLastError());
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), mode & ~(_O_CREAT | _O_EXCL | _O_TRUNC));
    if (descriptor < 0)
        CloseHandle(handle);
    return descriptor;
}

int RemoveGuestPath(const wchar_t* path, bool allowDirectory)
{
    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return HostFileError(GetLastError());
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (directory && !allowDirectory) {
        errno = EISDIR;
        return -1;
    }
    const auto handle = CreateFileW(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return HostFileError(GetLastError());
    FILE_DISPOSITION_INFO_EX disposition{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                                         FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    const bool removed =
        SetFileInformationByHandle(handle, FileDispositionInfoEx, &disposition, sizeof(disposition)) != FALSE;
    const DWORD error = removed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (removed)
        return 0;
    if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED)
        return directory ? _wrmdir(path) : _wunlink(path);
    return HostFileError(error);
}

int OpenDescriptor(const char* path, int flags, int permission)
{
    if (!path) {
        errno = EFAULT;
        return -1;
    }
    if (IsOwnProcPath(path, "/cmdline"))
        return OpenProcCommandLine(flags);
    if (IsRandomDevice(path)) {
        if ((flags & 3) != 0) {
            errno = EACCES;
            return -1;
        }
        int fd = -1;
        if (_sopen_s(&fd, "NUL", _O_RDONLY | _O_BINARY | _O_NOINHERIT, _SH_DENYNO, 0) != 0)
            return -1;
        try {
            const std::lock_guard lock(randomDevicesMutex);
            randomDevices.insert(fd);
            return fd;
        }
        catch (const std::bad_alloc&) {
            _close(fd);
            errno = ENOMEM;
            return -1;
        }
    }
    try {
        const auto wide = GuestPath(path, AndroidOpenWrites(flags), !(flags & 0x20000) && (flags & 0xc0) != 0xc0);
        return OpenWideDescriptor(wide.c_str(), flags, permission);
    }
    catch (const AndroidSystemPathError& error) {
        errno = error.code;
        return -1;
    }
    catch (const std::filesystem::filesystem_error&) {
        errno = EINVAL;
        return -1;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

int TerminalAttributesUnavailable(int fd)
{
    const auto previous = _set_thread_local_invalid_parameter_handler(
        +[](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        });
    const auto handle = _get_osfhandle(fd);
    _set_thread_local_invalid_parameter_handler(previous);
    // Win32 descriptors in Stone do not have an Android terminal line discipline.
    errno = handle == -1 || handle == -2 ? EBADF : ENOTTY;
    return -1;
}

template <typename Create> int MakeTempPath(char* path, size_t extensionLength, Create create)
{
    constexpr std::string_view alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const auto length = std::strlen(path);
    if (length < 6 || extensionLength > length - 6) {
        errno = EINVAL;
        return -1;
    }
    auto* suffix = path + length - extensionLength - 6;
    if (std::string_view(suffix, 6) != "XXXXXX") {
        errno = EINVAL;
        return -1;
    }
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        unsigned char random[6];
        if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
            std::memcpy(suffix, "XXXXXX", 6);
            errno = EIO;
            return -1;
        }
        for (unsigned i = 0; i < 6; ++i)
            suffix[i] = alphabet[random[i] % alphabet.size()];
        const auto result = create(path);
        if (result >= 0)
            return result;
        if (errno != EEXIST)
            break;
    }
    std::memcpy(suffix, "XXXXXX", 6);
    return -1;
}

struct FileLockState {
    HANDLE handle;
    std::mutex mutex;
    int mode = 0;

    explicit FileLockState(HANDLE file) : handle(file)
    {
    }
};

std::mutex fileLocksMutex;
std::unordered_map<int, std::shared_ptr<FileLockState>> fileLocks;

int LockDescriptor(int fd, int operation)
{
    constexpr int Shared = 1, Exclusive = 2, NonBlocking = 4, Unlock = 8;
    const auto mode = operation & ~NonBlocking;
    if (mode != Shared && mode != Exclusive && mode != Unlock) {
        errno = EINVAL;
        return -1;
    }
    const auto previous = _set_thread_local_invalid_parameter_handler(
        +[](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        });
    const auto file = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    _set_thread_local_invalid_parameter_handler(previous);
    if (file == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    try {
        std::shared_ptr<FileLockState> state;
        {
            const std::lock_guard lock(fileLocksMutex);
            const auto existing = fileLocks.find(fd);
            if (existing != fileLocks.end() && existing->second->handle == file)
                state = existing->second;
            else if (mode == Unlock)
                return 0;
            else
                state = fileLocks[fd] = std::make_shared<FileLockState>(file);
        }
        const std::lock_guard lock(state->mutex);
        if (mode == state->mode)
            return 0;
        OVERLAPPED range{};
        // flock replaces the lock held by this open file. LockFileEx can
        // stack another lock on the same handle, leaving one after unlock.
        if (state->mode) {
            if (!UnlockFileEx(file, 0, MAXDWORD, MAXDWORD, &range) && GetLastError() != ERROR_NOT_LOCKED) {
                errno = EIO;
                return -1;
            }
            state->mode = 0;
        }
        if (mode == Unlock)
            return 0;
        const DWORD flags = ((operation & NonBlocking) ? LOCKFILE_FAIL_IMMEDIATELY : 0) |
                            (mode == Exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
        if (LockFileEx(file, flags, 0, MAXDWORD, MAXDWORD, &range)) {
            state->mode = mode;
            return 0;
        }
        const auto error = GetLastError();
        errno = error == ERROR_LOCK_VIOLATION ? EAGAIN : error == ERROR_ACCESS_DENIED ? EACCES : EIO;
        return -1;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

HANDLE FileDescriptorHandle(int fd);

intptr_t WriteSharedSection(const MemoryFile& memory, const void* buffer, size_t size, int64_t offset)
{
    if (!size)
        return 0;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto bias = static_cast<size_t>(offset % system.dwAllocationGranularity);
    if (size > SIZE_MAX - bias) {
        errno = EOVERFLOW;
        return -1;
    }
    const auto base = static_cast<uint64_t>(offset) - bias;
    auto* view = static_cast<char*>(MapViewOfFile(memory.section.get(), FILE_MAP_WRITE, static_cast<DWORD>(base >> 32),
                                                  static_cast<DWORD>(base), size + bias));
    if (!view) {
        errno = EIO;
        return -1;
    }
    std::memcpy(view + bias, buffer, size);
    UnmapViewOfFile(view);
    return static_cast<intptr_t>(size);
}

int TruncateDescriptor(int fd, int64_t length)
{
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    std::unique_lock<std::mutex> lock;
    if (const auto memory = LockMemoryFile(fd, lock)) {
        const auto file = FileDescriptorHandle(fd);
        if (file == INVALID_HANDLE_VALUE)
            return -1;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size)) {
            errno = EIO;
            return -1;
        }
        if (length == size.QuadPart)
            return 0;
        if (memory->seals & (length < size.QuadPart ? SealShrink : SealGrow)) {
            errno = EPERM;
            return -1;
        }
        if (memory->views) {
            errno = EBUSY;
            return -1;
        }
        FILE_END_OF_FILE_INFO end{};
        end.EndOfFile.QuadPart = length;
        if (SetFileInformationByHandle(file, FileEndOfFileInfo, &end, sizeof(end)))
            return 0;
        errno = GetLastError() == ERROR_DISK_FULL ? ENOSPC : EIO;
        return -1;
    }
    const auto previous = _set_thread_local_invalid_parameter_handler(
        +[](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        });
    const auto error = _chsize_s(fd, length);
    _set_thread_local_invalid_parameter_handler(previous);
    if (!error)
        return 0;
    errno = error;
    return -1;
}

int AllocateFileRange(int fd, int mode, int64_t offset, int64_t length)
{
    if (offset < 0 || length <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (offset > INT64_MAX - length) {
        errno = EFBIG;
        return -1;
    }
    // The ordinary allocation mode extends EOF. Allocation beyond EOF can
    // be discarded on Windows when the last handle closes.
    if (mode != 0) {
        errno = 95;
        return -1;
    }
    std::unique_lock<std::mutex> lock;
    const auto memory = LockMemoryFile(fd, lock);
    const auto file = FileDescriptorHandle(fd);
    if (file == INVALID_HANDLE_VALUE)
        return -1;
    if (GetFileType(file) != FILE_TYPE_DISK) {
        errno = ENODEV;
        return -1;
    }
    FILE_STANDARD_INFO info{};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(file, FileStandardInfo, &info, sizeof(info)) ||
        !GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
        errno = EIO;
        return -1;
    }
    if (info.Directory) {
        errno = EISDIR;
        return -1;
    }
    if (attributes.FileAttributes & (FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_COMPRESSED)) {
        errno = 95;
        return -1;
    }
    const auto end = offset + length;
    if (memory && end > info.EndOfFile.QuadPart) {
        if (memory->seals & SealGrow) {
            errno = EPERM;
            return -1;
        }
        if (memory->views) {
            errno = EBUSY;
            return -1;
        }
    }
    // A valid write handle is required even for an already allocated range.
    FILE_ALLOCATION_INFO allocation{};
    allocation.AllocationSize.QuadPart = std::max({info.AllocationSize.QuadPart, info.EndOfFile.QuadPart, end});
    const auto allocationError = [] {
        switch (GetLastError()) {
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            errno = ENOSPC;
            break;
        case ERROR_ACCESS_DENIED:
            errno = EBADF;
            break;
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_FUNCTION:
            errno = 95;
            break;
        default:
            errno = EIO;
        }
        return -1;
    };
    if (!SetFileInformationByHandle(file, FileAllocationInfo, &allocation, sizeof(allocation)))
        return allocationError();
    if (end <= info.EndOfFile.QuadPart)
        return 0;
    FILE_END_OF_FILE_INFO eof{};
    eof.EndOfFile.QuadPart = end;
    return SetFileInformationByHandle(file, FileEndOfFileInfo, &eof, sizeof(eof)) ? 0 : allocationError();
}

intptr_t WriteDescriptor(int fd, const void* buffer, size_t size)
{
    intptr_t result;
    if (WriteAndroidSocket(fd, buffer, size, result))
        return result;
    if (WriteAndroidEventCounter(fd, buffer, size, result))
        return result;
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    std::unique_lock<std::mutex> lock;
    if (const auto memory = LockMemoryFile(fd, lock)) {
        if (size && (memory->seals & MemfdWriteSeals)) {
            errno = EPERM;
            return -1;
        }
        if ((memory->status & 0x400) && _lseeki64(fd, 0, SEEK_END) < 0)
            return -1;
        if (size && (memory->seals & SealGrow)) {
            const auto position = _telli64(fd);
            const auto length = _filelengthi64(fd);
            if (position < 0 || length < 0)
                return -1;
            if (length <= position) {
                errno = EPERM;
                return -1;
            }
            size = static_cast<size_t>(std::min<uint64_t>(size, length - position));
        }
        if (memory->section) {
            const auto position = _telli64(fd);
            if (position < 0)
                return -1;
            const auto written = WriteSharedSection(*memory, buffer, std::min<size_t>(size, INT_MAX), position);
            if (written > 0 && _lseeki64(fd, position + written, SEEK_SET) < 0)
                return -1;
            return written;
        }
    }
    const auto previous = _set_thread_local_invalid_parameter_handler(
        +[](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        });
    result = _write(fd, buffer, static_cast<unsigned>(std::min<size_t>(size, INT_MAX)));
    _set_thread_local_invalid_parameter_handler(previous);
    return result;
}

template <bool Writing> intptr_t TransferVectors(int fd, const AndroidIoVector* vectors, int count)
{
    const auto transfer = [fd](const void* bytes, size_t size) {
        if constexpr (Writing)
            return WriteDescriptor(fd, bytes, size);
        else
            return ReadAndroidFd(fd, const_cast<void*>(bytes), size);
    };
    if (count < 0 || count > 1024) {
        errno = EINVAL;
        return -1;
    }
    if (count && !vectors) {
        errno = EFAULT;
        return -1;
    }
    size_t size = 0;
    int first = -1, last = -1;
    for (int index = 0; index < count; ++index) {
        if (vectors[index].size > PTRDIFF_MAX - size) {
            errno = EINVAL;
            return -1;
        }
        if (vectors[index].size && !vectors[index].data) {
            errno = EFAULT;
            return -1;
        }
        size += vectors[index].size;
        if (vectors[index].size) {
            if (first < 0)
                first = index;
            last = index;
        }
    }
    char dummy{};
    if (first < 0)
        return transfer(&dummy, 0);
    constexpr size_t MaxTransfer = 0x7ffff000;
    const auto length = std::min(size, MaxTransfer);
    if (first == last)
        return transfer(vectors[first].data, length);

    // Use one host operation so vector boundaries cannot interleave with another caller.
    try {
        auto buffer = std::make_unique_for_overwrite<char[]>(length);
        const auto copy = [&](size_t size) {
            size_t offset = 0;
            for (int index = first; index <= last && offset < size; ++index) {
                const auto bytes = std::min(vectors[index].size, size - offset);
                if (!bytes)
                    continue;
                if constexpr (Writing)
                    std::memcpy(buffer.get() + offset, vectors[index].data, bytes);
                else
                    std::memcpy(const_cast<void*>(vectors[index].data), buffer.get() + offset, bytes);
                offset += bytes;
            }
        };
        if constexpr (Writing)
            copy(length);
        const auto transferred = transfer(buffer.get(), length);
        if constexpr (!Writing)
            if (transferred > 0)
                copy(static_cast<size_t>(transferred));
        return transferred;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

HANDLE FileDescriptorHandle(int fd)
{
    if (fd < 0) {
        errno = EBADF;
        return INVALID_HANDLE_VALUE;
    }
    const auto previous = _set_thread_local_invalid_parameter_handler(
        +[](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        });
    const auto handle = _get_osfhandle(fd);
    _set_thread_local_invalid_parameter_handler(previous);
    if (handle == -1 || handle == -2) {
        errno = EBADF;
        return INVALID_HANDLE_VALUE;
    }
    return reinterpret_cast<HANDLE>(handle);
}

int SyncDescriptor(int fd)
{
    // In particular, FileDescriptor.sync() may be asked to sync an unopened
    // descriptor. Pass the POSIX error back instead of invoking a CRT assert.
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (AndroidSocketId(fd) || AndroidLooperOwnsFd(fd)) {
        errno = EINVAL;
        return -1;
    }
    const auto handle = FileDescriptorHandle(fd);
    if (handle == INVALID_HANDLE_VALUE)
        return -1;
    if (GetFileType(handle) != FILE_TYPE_DISK) {
        errno = EINVAL;
        return -1;
    }
    if (FlushFileBuffers(handle))
        return 0;
    const auto error = GetLastError();
    errno = error == ERROR_ACCESS_DENIED || error == ERROR_INVALID_HANDLE ? EBADF
            : error == ERROR_DISK_FULL                                    ? ENOSPC
                                                                          : EIO;
    return -1;
}

struct FileByteLock {
    uint64_t start, end;
    int16_t type;
};

struct FileRangeLocks {
    std::unique_ptr<void, decltype(&CloseHandle)> handle{nullptr, CloseHandle};
    std::vector<FileByteLock> ranges;
};

using FileIdentity = std::pair<DWORD, uint64_t>;
std::mutex byteLocksMutex;
std::map<FileIdentity, FileRangeLocks> byteLocks;

bool ReadFileIdentity(HANDLE file, FileIdentity& identity)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    identity = {info.dwVolumeSerialNumber, (uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow};
    return true;
}

bool SetHostByteLock(HANDLE file, const FileByteLock& lock, bool release = false)
{
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(lock.start);
    position.OffsetHigh = static_cast<DWORD>(lock.start >> 32);
    const auto length = lock.end - lock.start;
    const auto low = static_cast<DWORD>(length);
    const auto high = static_cast<DWORD>(length >> 32);
    if (release)
        return UnlockFileEx(file, 0, low, high, &position) != FALSE;
    const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (lock.type == 1 ? LOCKFILE_EXCLUSIVE_LOCK : 0);
    return LockFileEx(file, flags, 0, low, high, &position) != FALSE;
}

void ForgetByteLocks(const FileIdentity& id)
{
    const std::lock_guard lock(byteLocksMutex);
    const auto existing = byteLocks.extract(id);
    if (existing) {
        for (const auto& range : existing.mapped().ranges)
            SetHostByteLock(existing.mapped().handle.get(), range, true);
    }
}

void ForgetByteLocks(HANDLE file)
{
    FileIdentity id;
    if (file != INVALID_HANDLE_VALUE && ReadFileIdentity(file, id))
        ForgetByteLocks(id);
}

int FileByteLockError(DWORD error)
{
    if (error == ERROR_LOCK_VIOLATION || error == ERROR_SHARING_VIOLATION)
        return EAGAIN;
    if (error == ERROR_ACCESS_DENIED || error == ERROR_INVALID_HANDLE)
        return EBADF;
    return EIO;
}

int ChangeByteLock(HANDLE file, const FileIdentity& id, const FileByteLock& request, AndroidFileRange* query)
{
    constexpr int Unlocked = 2;
    const std::lock_guard lock(byteLocksMutex);
    auto found = byteLocks.find(id);
    if (found != byteLocks.end())
        file = found->second.handle.get();
    const auto overlaps = [&](const auto& existing) {
        return request.start < existing.end && existing.start < request.end;
    };
    if (query) {
        // A kernel probe must not release or report a lock owned by this process.
        if (found != byteLocks.end() && std::ranges::any_of(found->second.ranges, overlaps))
            return 95;
        if (SetHostByteLock(file, request)) {
            if (!SetHostByteLock(file, request, true))
                return EIO;
            query->type = Unlocked;
            return 0;
        }
        const int error = FileByteLockError(GetLastError());
        if (error != EAGAIN)
            return error;
        query->type = 1;
        query->pid = -1;
        return 0;
    }
    if (found == byteLocks.end()) {
        if (request.type == Unlocked)
            return 0;
        FileRangeLocks created;
        created.ranges.reserve(1);
        HANDLE duplicate = nullptr;
        const auto process = GetCurrentProcess();
        if (!DuplicateHandle(process, file, process, &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return FileByteLockError(GetLastError());
        created.handle.reset(duplicate);
        found = byteLocks.try_emplace(id, std::move(created)).first;
        file = duplicate;
    }
    auto& ranges = found->second.ranges;
    if (request.type != Unlocked && std::ranges::any_of(ranges, [&](const auto& range) {
            return range.start <= request.start && range.end >= request.end && range.type == request.type;
        }))
        return 0;
    if (std::ranges::any_of(ranges, [&](const auto& range) {
            return overlaps(range) && (range.start < request.start || range.end > request.end);
        }))
        return 95; // Windows cannot partially release an existing byte lock.

    std::vector<FileByteLock> released;
    released.reserve(ranges.size());
    ranges.reserve(ranges.size() + 1);
    const auto restore = [&] {
        for (const auto& old : released) {
            if (SetHostByteLock(file, old))
                ranges.push_back(old);
        }
    };
    for (auto range = ranges.begin(); range != ranges.end();) {
        if (!overlaps(*range)) {
            ++range;
            continue;
        }
        if (!SetHostByteLock(file, *range, true)) {
            restore();
            return EIO;
        }
        released.push_back(*range);
        range = ranges.erase(range);
    }
    if (request.type != Unlocked) {
        if (!SetHostByteLock(file, request)) {
            const auto error = FileByteLockError(GetLastError());
            restore();
            if (ranges.empty())
                byteLocks.erase(found);
            return error;
        }
        ranges.push_back(request);
    }
    if (ranges.empty())
        byteLocks.erase(found);
    return 0;
}

int DescriptorByteLock(int fd, int command, AndroidFileRange* range)
{
    constexpr int Get = 5, Wait = 7, Unlocked = 2;
    if (!range) {
        errno = EFAULT;
        return -1;
    }
    if (range->type < 0 || range->type > Unlocked || (command == Get && range->type == Unlocked)) {
        errno = EINVAL;
        return -1;
    }
    const auto handle = FileDescriptorHandle(fd);
    if (handle == INVALID_HANDLE_VALUE)
        return -1;
    FileIdentity id;
    if (!ReadFileIdentity(handle, id)) {
        errno = EBADF;
        return -1;
    }
    LARGE_INTEGER base{};
    if (range->origin == SEEK_END) {
        if (!GetFileSizeEx(handle, &base)) {
            errno = EIO;
            return -1;
        }
    }
    else if (range->origin == SEEK_CUR) {
        if (!SetFilePointerEx(handle, {}, &base, FILE_CURRENT)) {
            errno = EIO;
            return -1;
        }
    }
    else if (range->origin != SEEK_SET) {
        errno = EINVAL;
        return -1;
    }
    if (range->start > INT64_MAX - base.QuadPart || range->start < -base.QuadPart) {
        errno = EINVAL;
        return -1;
    }
    const auto position = base.QuadPart + range->start;
    if (range->length < -position || range->length > INT64_MAX - position) {
        errno = EINVAL;
        return -1;
    }
    const auto other = position + range->length;
    const FileByteLock request{uint64_t(std::min(position, other)),
                               range->length ? uint64_t(std::max(position, other)) : uint64_t{1} << 63, range->type};
    try {
        int error;
        do {
            error = ChangeByteLock(handle, id, request, command == Get ? range : nullptr);
            if (command == Wait && error == EAGAIN)
                Sleep(5);
            else
                break;
            if (FileDescriptorHandle(fd) != handle) {
                errno = EBADF;
                return -1;
            }
        } while (true);
        if (!error)
            return 0;
        errno = error;
        return -1;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

int DuplicateDescriptor3(int source, int target, int flags)
{
    constexpr int CloseOnExec = 0x80000;
    if (source == target || (flags & ~CloseOnExec)) {
        errno = EINVAL;
        return -1;
    }
    if (target < 0 || target >= GuestDescriptorLimit) {
        errno = EBADF;
        return -1;
    }
    if (FileDescriptorHandle(source) == INVALID_HANDLE_VALUE)
        return -1;
    {
        const std::lock_guard lock(randomDevicesMutex);
        if (randomDevices.contains(source) || randomDevices.contains(target))
            return AndroidUnsupportedOperation();
    }
    int replaced;
    if (ReplaceAndroidSocket(source, target, (flags & CloseOnExec) != 0, replaced))
        return replaced;
    if (AndroidSocketId(source) || AndroidSocketId(target) || AndroidLooperOwnsFd(source) ||
        AndroidLooperOwnsFd(target))
        return AndroidUnsupportedOperation();
    const std::unique_lock memoryLock(memoryFilesMutex);
    const auto entry = memoryFiles.find(source);
    const auto memory = entry == memoryFiles.end() ? nullptr : entry->second;
    const auto old = memoryFiles.find(target);
    auto previousFile = old == memoryFiles.end() ? nullptr : old->second;
    std::unique_lock<std::mutex> oldLock;
    if (previousFile)
        oldLock = std::unique_lock(previousFile->mutex);
    const auto previous = _set_thread_local_invalid_parameter_handler(
        +[](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        });
    FileIdentity oldIdentity;
    const auto targetHandle = FileDescriptorHandle(target);
    const bool hadTarget = targetHandle != INVALID_HANDLE_VALUE && ReadFileIdentity(targetHandle, oldIdentity);
    const int duplicated = _dup2(source, target);
    _set_thread_local_invalid_parameter_handler(previous);
    if (duplicated < 0)
        return -1;
    procCommandLines[target].store(false);
    if (hadTarget)
        ForgetByteLocks(oldIdentity);
    memoryFiles.erase(target);
    const auto copy = FileDescriptorHandle(target);
    if (!SetHandleInformation(copy, HANDLE_FLAG_INHERIT, flags ? 0 : HANDLE_FLAG_INHERIT)) {
        _close(target);
        errno = EIO;
        return -1;
    }
    try {
        if (memory)
            memoryFiles.emplace(target, memory);
        procCommandLines[target].store(IsProcCommandLineDescriptor(source));
    }
    catch (const std::bad_alloc&) {
        _close(target);
        errno = ENOMEM;
        return -1;
    }
    return target;
}

int DuplicateDescriptor(int source, int minimum, bool closeOnExec, bool allowSpecial = true)
{
    if (minimum < 0 || minimum >= GuestDescriptorLimit) {
        errno = EINVAL;
        return -1;
    }
    int duplicateFd;
    if (DuplicateAndroidInotify(source, minimum, closeOnExec, duplicateFd, allowSpecial) ||
        DuplicateAndroidSocket(source, minimum, closeOnExec, duplicateFd, allowSpecial) ||
        DuplicateAndroidLooperFd(source, minimum, closeOnExec, duplicateFd, allowSpecial))
        return duplicateFd;
    {
        const std::lock_guard lock(randomDevicesMutex);
        if (randomDevices.contains(source))
            return AndroidUnsupportedOperation();
    }
    const std::lock_guard lock(memoryFilesMutex);
    if (FileDescriptorHandle(source) == INVALID_HANDLE_VALUE)
        return -1;
    const auto entry = memoryFiles.find(source);
    const auto memory = entry == memoryFiles.end() ? nullptr : entry->second;
    const int duplicate = DuplicateCrtFd(source, minimum, !closeOnExec);
    if (duplicate < 0)
        return -1;
    try {
        if (memory)
            memoryFiles.emplace(duplicate, memory);
        procCommandLines[duplicate].store(IsProcCommandLineDescriptor(source));
        return duplicate;
    }
    catch (const std::bad_alloc&) {
        _close(duplicate);
        errno = ENOMEM;
        return -1;
    }
}

int DuplicateDescriptor2(int source, int target)
{
    if (source == target)
        return FileDescriptorHandle(source) == INVALID_HANDLE_VALUE ? -1 : source;
    return DuplicateDescriptor3(source, target, 0);
}

int SetDescriptorTimes(int fd, const AndroidTimespec* times)
{
    const auto handle = FileDescriptorHandle(fd);
    if (handle == INVALID_HANDLE_VALUE)
        return -1;
    constexpr int64_t Now = 0x3fffffff, Omit = 0x3ffffffe;
    constexpr int64_t Epoch = 11644473600, TicksPerSecond = 10'000'000;
    std::array<FILETIME, 2> values{};
    std::array<const FILETIME*, 2> pointers{};
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    for (size_t i = 0; i < values.size(); ++i) {
        const auto time = times ? times[i] : AndroidTimespec{0, Now};
        if (time.nanos == Omit)
            continue;
        if (time.nanos == Now) {
            pointers[i] = &now;
            continue;
        }
        if (time.nanos < 0 || time.nanos >= 1'000'000'000) {
            errno = EINVAL;
            return -1;
        }
        const auto fraction = static_cast<uint64_t>(time.nanos / 100);
        if (time.seconds < -Epoch ||
            time.seconds > static_cast<int64_t>((INT64_MAX - fraction) / TicksPerSecond) - Epoch) {
            errno = 75; // Bionic EOVERFLOW
            return -1;
        }
        const uint64_t ticks = static_cast<uint64_t>(time.seconds + Epoch) * TicksPerSecond + fraction;
        values[i] = {static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
        pointers[i] = &values[i];
    }
    if (!pointers[0] && !pointers[1])
        return 0;
    if (SetFileTime(handle, nullptr, pointers[0], pointers[1]))
        return 0;
    errno = GetLastError() == ERROR_ACCESS_DENIED ? EACCES : EIO;
    return -1;
}

int SetDescriptorMicrotimes(int fd, const AndroidTimespec* times)
{
    if (!times)
        return SetDescriptorTimes(fd, nullptr);
    AndroidTimespec precise[2];
    for (size_t i = 0; i < std::size(precise); ++i) {
        if (times[i].nanos < 0 || times[i].nanos >= 1'000'000) {
            errno = EINVAL;
            return -1;
        }
        precise[i] = {times[i].seconds, times[i].nanos * 1000};
    }
    return SetDescriptorTimes(fd, precise);
}

int AdviseDescriptor(int fd, int64_t offset, int64_t length, int advice)
{
    if (offset < 0 || length < 0 || advice < 0 || advice > 5)
        return EINVAL;
    if (AndroidSocketId(fd) || AndroidLooperOwnsFd(fd))
        return ESPIPE;
    const int saved = errno;
    const auto file = FileDescriptorHandle(fd);
    errno = saved;
    if (file == INVALID_HANDLE_VALUE)
        return EBADF;
    // Windows chooses caching behavior for a handle at open time. POSIX advice is optional.
    return GetFileType(file) == FILE_TYPE_DISK ? 0 : ESPIPE;
}

intptr_t SendFile(int output, int input, int64_t* offset, size_t size)
{
    constexpr size_t MaxTransfer = 0x7ffff000;
    const auto source = FileDescriptorHandle(input);
    if (source == INVALID_HANDLE_VALUE || FileDescriptorHandle(output) == INVALID_HANDLE_VALUE)
        return -1;
    if (GetFileType(source) != FILE_TYPE_DISK || input == output) {
        errno = EINVAL;
        return -1;
    }
    char probe;
    DWORD ignored = 0;
    if (!ReadFile(source, &probe, 0, &ignored, nullptr)) {
        errno = EBADF;
        return -1;
    }
    LARGE_INTEGER position{};
    if (offset)
        position.QuadPart = *offset;
    else if (!SetFilePointerEx(source, position, &position, FILE_CURRENT)) {
        errno = EIO;
        return -1;
    }
    if (position.QuadPart < 0) {
        errno = EINVAL;
        return -1;
    }
    const auto available = static_cast<uint64_t>(INT64_MAX - position.QuadPart);
    size = static_cast<size_t>(std::min<uint64_t>(std::min(size, MaxTransfer), available));
    if (!size)
        return 0;

    // An independently opened handle keeps read-ahead from moving the shared
    // input position when a write transfers only part of the buffer.
    const auto readHandle = ReOpenFile(source, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (readHandle == INVALID_HANDLE_VALUE) {
        errno = GetLastError() == ERROR_ACCESS_DENIED ? EACCES : EIO;
        return -1;
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> copied(readHandle, &CloseHandle);
    if (!SetFilePointerEx(readHandle, position, nullptr, FILE_BEGIN)) {
        errno = EIO;
        return -1;
    }
    char buffer[16 * 1024];
    size_t sent = 0;
    while (sent < size) {
        DWORD read = 0;
        if (!ReadFile(readHandle, buffer, static_cast<DWORD>(std::min(size - sent, sizeof(buffer))), &read, nullptr)) {
            if (!sent) {
                errno = EIO;
                return -1;
            }
            break;
        }
        if (!read)
            break;
        const auto written = WriteDescriptor(output, buffer, read);
        if (written < 0 && !sent)
            return -1;
        if (written > 0)
            sent += static_cast<size_t>(written);
        if (written < static_cast<intptr_t>(read))
            break;
    }
    position.QuadPart += static_cast<int64_t>(sent);
    if (offset)
        *offset = position.QuadPart;
    else
        SetFilePointerEx(source, position, nullptr, FILE_BEGIN);
    return static_cast<intptr_t>(sent);
}

template <bool Writing>
intptr_t TransferAtOffset(HANDLE file, void* bytes, size_t size, int64_t offset, const MemoryFile* memory)
{
    if (offset < 0 || size > MAXDWORD) {
        errno = EINVAL;
        return -1;
    }
    OVERLAPPED position{};
    LARGE_INTEGER saved{};
    if (memory) {
        if (!SetFilePointerEx(file, {}, &saved, FILE_CURRENT)) {
            errno = EIO;
            return -1;
        }
        if constexpr (Writing) {
            if (size && (memory->seals & MemfdWriteSeals)) {
                errno = EPERM;
                return -1;
            }
            LARGE_INTEGER length{};
            if (!GetFileSizeEx(file, &length)) {
                errno = EIO;
                return -1;
            }
            if (memory->status & 0x400)
                offset = length.QuadPart;
            if (size && (memory->seals & SealGrow)) {
                if (offset >= length.QuadPart) {
                    errno = EPERM;
                    return -1;
                }
                size = static_cast<size_t>(std::min<uint64_t>(size, length.QuadPart - offset));
            }
            if (memory->section)
                return WriteSharedSection(*memory, bytes, size, offset);
        }
    }
    position.Offset = static_cast<DWORD>(offset);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD count = 0;
    const auto success = Writing ? WriteFile(file, bytes, static_cast<DWORD>(size), &count, &position)
                                 : ReadFile(file, bytes, static_cast<DWORD>(size), &count, &position);
    const auto error = success ? ERROR_SUCCESS : GetLastError();
    if (memory && !SetFilePointerEx(file, saved, nullptr, FILE_BEGIN)) {
        errno = EIO;
        return -1;
    }
    if (success)
        return count;
    if (!Writing && error == ERROR_HANDLE_EOF)
        return 0;
    errno = EIO;
    return -1;
}

template <bool Writing> intptr_t TransferAt(int fd, void* bytes, size_t size, int64_t offset)
{
    std::unique_lock<std::mutex> lock;
    const auto memory = LockMemoryFile(fd, lock);
    const auto file = FileDescriptorHandle(fd);
    if (file == INVALID_HANDLE_VALUE)
        return -1;
    return TransferAtOffset<Writing>(file, bytes, size, offset, memory.get());
}

constexpr int MapShared = 1, MapPrivate = 2, MapFixed = 0x10, MapAnonymous = 0x20;
constexpr int MapGrowDown = 0x100, MapNoReserve = 0x4000, MapPopulate = 0x8000, MapStack = 0x20000;

struct Mapping {
    struct Region {
        uintptr_t start, end;
        DWORD protection;
    };

    uintptr_t start, end;
    void* view = nullptr;
    bool arm64 = false, anonymous = false;
    std::shared_ptr<MemoryFile> memory;
    bool writableView = false;
    int memoryProtection = 7;
    DWORD uniformProtection = 0; // Zero when the active range can have mixed Windows protections.
    std::vector<Region> regions;
    std::vector<std::pair<uintptr_t, uintptr_t>> executable;
    int64_t fileOffset = 0;
};

std::mutex mappingMutex;
std::map<uintptr_t, Mapping> mappings;
std::atomic<uint64_t> arm64MappingGeneration{0};
// Protected by mappingMutex, including publication and every pointer lookup.
// Map nodes remain stable through protection changes and partial unmaps.
std::vector<Mapping*> arm64MappingIndex;
bool arm64MappingIndexDirty = true;

struct Arm64MappingCache {
    struct Region {
        uintptr_t start = 0, end = 0;
        uint64_t generation = 0;
        bool writable = false;
    };

    std::array<Region, 4> regions{};
    size_t next = 0;
};

thread_local Arm64MappingCache arm64MappingCache;

struct Arm64CodeCache {
    struct Region {
        uintptr_t start = 0, end = 0;
        uint64_t generation = 0;
    };

    std::array<Region, 8> regions{};
    size_t next = 0;
};

thread_local Arm64CodeCache arm64CodeCache;

auto CodeRangesAfter(const Mapping& mapping, uintptr_t start, size_t size, bool executable)
{
    const auto end = start + size;
    std::vector<std::pair<uintptr_t, uintptr_t>> ranges;
    for (const auto& [first, last] : mapping.executable) {
        if (last <= start || first >= end)
            ranges.emplace_back(first, last);
        else {
            if (first < start)
                ranges.emplace_back(first, start);
            if (last > end)
                ranges.emplace_back(end, last);
        }
    }
    if (executable)
        ranges.emplace_back(start, end);
    std::ranges::sort(ranges);
    size_t count = 0;
    for (const auto range : ranges) {
        if (count && range.first <= ranges[count - 1].second)
            ranges[count - 1].second = std::max(ranges[count - 1].second, range.second);
        else
            ranges[count++] = range;
    }
    ranges.resize(count);
    return ranges;
}

DWORD PageProtection(int access)
{
    switch (access) {
    case 0:
        return PAGE_NOACCESS;
    case 1:
        return PAGE_READONLY;
    case 2:
    case 3:
        return PAGE_READWRITE;
    case 4:
        return PAGE_EXECUTE;
    case 5:
        return PAGE_EXECUTE_READ;
    case 6:
    case 7:
        return PAGE_EXECUTE_READWRITE;
    default:
        return 0;
    }
}

size_t RoundPage(size_t size)
{
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const size_t mask = info.dwPageSize - 1;
    return size > SIZE_MAX - mask ? 0 : (size + mask) & ~mask;
}

void* MapSharedFile(void* hint, size_t size, size_t rounded, int access, int fd, int64_t offset, DWORD protection,
                    bool arm64)
{
    void* const failed = reinterpret_cast<void*>(-1);
    if (fd < 0) {
        errno = EBADF;
        return failed;
    }
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if (offset < 0 || offset % system.dwPageSize) {
        errno = EINVAL;
        return failed;
    }
    std::unique_lock<std::mutex> lock;
    const auto memory = LockMemoryFile(fd, lock);
    const auto file = FileDescriptorHandle(fd);
    if (file == INVALID_HANDLE_VALUE)
        return failed;
    const size_t bias = static_cast<size_t>(offset % system.dwAllocationGranularity);
    if (size > SIZE_MAX - bias) {
        errno = EOVERFLOW;
        return failed;
    }
    const auto aligned = static_cast<uint64_t>(offset) - bias;
    size_t viewBytes = size;
    LARGE_INTEGER fileSize{};
    if (!memory && GetFileSizeEx(file, &fileSize) && fileSize.QuadPart > offset) {
        const auto available = static_cast<size_t>(fileSize.QuadPart - offset);
        if (available < size && RoundPage(available) == rounded) {
            // Windows permits access to the zero-filled tail of the final
            // page, but the requested view length itself cannot pass EOF.
            viewBytes = available;
        }
    }
    bool write = (access & 2) != 0;
    bool execute = (access & 4) != 0;
    if (memory) {
        if (access & ~memory->mappingProtection) {
            errno = EACCES;
            return failed;
        }
        const bool canWrite = (memory->seals & MemfdWriteSeals) == 0;
        const bool canExecute = (memory->mode & 0111) && (memory->mappingProtection & 4);
        if (write && !canWrite) {
            errno = EPERM;
            return failed;
        }
        if (execute && !canExecute) {
            errno = EACCES;
            return failed;
        }
        write = canWrite;
        execute = canExecute;
    }
    if (arm64)
        execute = false;
    const DWORD sectionProtection =
        execute ? (write ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : (write ? PAGE_READWRITE : PAGE_READONLY);
    const auto importedSection = memory ? memory->section.get() : nullptr;
    const auto section =
        importedSection ? importedSection : CreateFileMappingW(file, nullptr, sectionProtection, 0, 0, nullptr);
    if (!section) {
        errno = GetLastError() == ERROR_ACCESS_DENIED ? EACCES : EINVAL;
        return failed;
    }
    const DWORD viewAccess = (write ? FILE_MAP_WRITE : FILE_MAP_READ) | (execute ? FILE_MAP_EXECUTE : 0);
    uint8_t* view = nullptr;
    const auto desired = reinterpret_cast<uintptr_t>(hint);
    if (desired >= bias && desired) {
        // Linux hints need only page alignment; Windows section bases need
        // allocation-granularity alignment. Preserve ART's low-address hint.
        const auto base = (desired - bias) / system.dwAllocationGranularity * system.dwAllocationGranularity;
        if (base)
            view = static_cast<uint8_t*>(MapViewOfFileEx(section, viewAccess, static_cast<DWORD>(aligned >> 32),
                                                         static_cast<DWORD>(aligned), bias + viewBytes,
                                                         reinterpret_cast<void*>(base)));
    }
    if (!view)
        view = static_cast<uint8_t*>(MapViewOfFile(section, viewAccess, static_cast<DWORD>(aligned >> 32),
                                                   static_cast<DWORD>(aligned), bias + viewBytes));
    const auto error = GetLastError();
    if (!importedSection)
        CloseHandle(section);
    if (!view) {
        errno = error == ERROR_ACCESS_DENIED ? EACCES : EINVAL;
        return failed;
    }
    auto* address = view + bias;
    DWORD oldProtection;
    if (!VirtualProtect(address, rounded, protection, &oldProtection)) {
        UnmapViewOfFile(view);
        errno = EACCES;
        return failed;
    }
    const auto start = reinterpret_cast<uintptr_t>(address);
    try {
        Mapping mapping{start,     start + rounded, view,  arm64,
                        false,     memory,          write, memory ? memory->mappingProtection : 7,
                        protection};
        mapping.fileOffset = offset;
        if (arm64 && (access & 4))
            mapping.executable.emplace_back(start, start + rounded);
        arm64MappingIndexDirty = true;
        mappings.emplace(start, std::move(mapping));
    }
    catch (const std::bad_alloc&) {
        UnmapViewOfFile(view);
        errno = ENOMEM;
        return failed;
    }
    if (memory) {
        ++memory->views;
        if (write)
            ++memory->writableViews;
    }
    return address;
}

auto FindMapping(uintptr_t start, size_t size)
{
    auto it = mappings.upper_bound(start);
    if (it == mappings.begin())
        return mappings.end();
    --it;
    if (start < it->second.start || start >= it->second.end || size > it->second.end - start)
        return mappings.end();
    return it;
}

Mapping* FindArm64DataMapping(uintptr_t address)
{
    if (arm64MappingIndexDirty) {
        struct Errors {
            DWORD win = GetLastError();
            int error = errno;

            ~Errors()
            {
                errno = error;
                SetLastError(win);
            }
        } preserve;

        try {
            std::vector<Mapping*> next;
            next.reserve(mappings.size());
            for (auto& [start, mapping] : mappings)
                if (mapping.arm64)
                    next.push_back(&mapping);
            arm64MappingIndex.swap(next);
            arm64MappingIndexDirty = false;
        }
        catch (const std::bad_alloc&) {
            // Failure to create the acceleration index must not deny valid memory.
            const auto found = FindMapping(address, 0);
            return found != mappings.end() && found->second.arm64 ? &found->second : nullptr;
        }
    }
    // Raw bounded indexing avoids constructing checked tree iterators for every
    // load. Trimming a mapping cannot reorder its disjoint allocation range.
    auto* const entries = arm64MappingIndex.data();
    size_t first = 0, last = arm64MappingIndex.size();
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        if (entries[middle]->start <= address)
            first = middle + 1;
        else
            last = middle;
    }
    if (!first)
        return nullptr;
    auto* mapping = entries[first - 1];
    return address < mapping->end ? mapping : nullptr;
}

int QueryPageResidency(const void* address, size_t length, unsigned char* pages)
{
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const size_t pageSize = system.dwPageSize;
    const auto start = reinterpret_cast<uintptr_t>(address);
    if (start % pageSize) {
        errno = EINVAL;
        return -1;
    }
    if (!length)
        return 0;
    if (!pages) {
        errno = EFAULT;
        return -1;
    }
    const auto rounded = RoundPage(length);
    if (!rounded || start > UINTPTR_MAX - rounded) {
        errno = ENOMEM;
        return -1;
    }
    std::array<PSAPI_WORKING_SET_EX_INFORMATION, 128> queries{};
    auto first = start;
    const auto end = start + rounded;
    while (first < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(first), &info, sizeof(info)) || info.State == MEM_FREE) {
            errno = ENOMEM;
            return -1;
        }
        const auto regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        const auto count = (std::min(regionEnd, end) - first) / pageSize;
        if (info.State == MEM_RESERVE) {
            std::memset(pages, 0, count);
            pages += count;
            first += count * pageSize;
            continue;
        }
        const auto batch = std::min(count, queries.size());
        for (size_t i = 0; i < batch; ++i) {
            queries[i] = {};
            queries[i].VirtualAddress = reinterpret_cast<void*>(first + i * pageSize);
        }
        if (!K32QueryWorkingSetEx(GetCurrentProcess(), queries.data(),
                                  static_cast<DWORD>(batch * sizeof(queries[0])))) {
            errno = EIO;
            return -1;
        }
        for (size_t i = 0; i < batch; ++i)
            *pages++ = queries[i].VirtualAttributes.Valid ? 1 : 0;
        first += batch * pageSize;
    }
    return 0;
}

template <bool Arm64 = false> void* MapMemory(void* hint, size_t size, int access, int flags, int fd, int64_t offset)
{
    void* const failed = reinterpret_cast<void*>(-1);
    const auto protection = PageProtection(Arm64 && (access & 4) ? (access & 3) | 1 : access);
    const auto rounded = RoundPage(size);
    if ((access & ~7) || !protection || !rounded) {
        errno = EINVAL;
        return failed;
    }
    const int type = flags & (MapPrivate | MapShared);
    const bool sharedFile = type == MapShared && !(flags & (MapAnonymous | MapFixed));
    const bool sharedAnonymous = type == MapShared && (flags & MapAnonymous) && !(flags & MapFixed);
    const bool fixedShared = Arm64 && type == MapShared && (flags & MapFixed) && !(flags & MapAnonymous);
    if ((flags & ~(MapPrivate | MapShared | MapFixed | MapAnonymous | MapNoReserve | MapPopulate | MapStack)) ||
        (type != MapPrivate && !sharedFile && !sharedAnonymous && !fixedShared)) {
        errno = 38;
        return failed;
    }
    const auto populate = [&](void* address) {
        if ((flags & MapPopulate) && access && address != failed) {
            WIN32_MEMORY_RANGE_ENTRY range{address, rounded};
            PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
        }
        return address;
    };
    const std::lock_guard lock(mappingMutex);
    if constexpr (!Arm64) {
        if ((access & 4) && !(flags & MapAnonymous)) {
            const auto file = FileDescriptorHandle(fd);
            if (file == INVALID_HANDLE_VALUE)
                return failed;
            std::wstring path(32768, L'\0');
            const auto length =
                GetFinalPathNameByHandleW(file, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED);
            if (length && length < path.size()) {
                path.resize(length);
                for (auto& ch : path)
                    if (ch >= L'A' && ch <= L'Z')
                        ch += L'a' - L'A';
                if (path.ends_with(L".oat") || path.ends_with(L".odex") ||
                    (path.find(L"\\dalvik-cache\\x86_64\\") != path.npos && path.ends_with(L"@classes.dex"))) {
                    // A fallback through ART's raw ELF reader would publish
                    // x64 GS instructions without Stone's TLS adaptation.
                    errno = EACCES;
                    return failed;
                }
            }
        }
    }
    if (fixedShared) {
        const auto start = reinterpret_cast<uintptr_t>(hint);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        if (start % system.dwPageSize || offset < 0 || offset % system.dwPageSize) {
            errno = EINVAL;
            return failed;
        }
        const auto existing = FindMapping(start, rounded);
        if (existing == mappings.end() || !existing->second.arm64 || !existing->second.view || offset < 0) {
            errno = EINVAL;
            return failed;
        }
        auto& mapping = existing->second;
        std::unique_lock<std::mutex> memoryLock;
        const auto memory = LockMemoryFile(fd, memoryLock);
        if (!memory || memory != mapping.memory || FileDescriptorHandle(fd) == INVALID_HANDLE_VALUE ||
            uint64_t(offset) != uint64_t(mapping.fileOffset) + start - existing->first) {
            errno = EINVAL;
            return failed;
        }
        if ((access & ~mapping.memoryProtection) || ((access & 4) && !(memory->mode & 0111)) ||
            ((access & 2) && !mapping.writableView)) {
            errno = EACCES;
            return failed;
        }
        auto code = CodeRangesAfter(mapping, start, rounded, (access & 4) != 0);
        arm64MappingGeneration.fetch_add(1, std::memory_order_release);
        DWORD previous;
        if (!VirtualProtect(hint, rounded, protection, &previous)) {
            errno = EACCES;
            return failed;
        }
        mapping.executable.swap(code);
        mapping.regions.clear();
        mapping.uniformProtection = start == mapping.start && rounded == mapping.end - start ? protection : 0;
        return populate(hint);
    }
    if (sharedFile)
        return populate(MapSharedFile(hint, size, rounded, access, fd, offset, protection, Arm64));
    if (sharedAnonymous) {
        const auto section = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, (!Arm64 && (access & 4)) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE,
            static_cast<DWORD>(uint64_t{rounded} >> 32), static_cast<DWORD>(rounded), nullptr);
        if (!section) {
            errno = ENOMEM;
            return failed;
        }
        const DWORD mapAccess = FILE_MAP_WRITE | ((!Arm64 && (access & 4)) ? FILE_MAP_EXECUTE : 0);
        auto* address = MapViewOfFileEx(section, mapAccess, 0, 0, rounded, hint);
        if (!address && hint)
            address = MapViewOfFile(section, mapAccess, 0, 0, rounded);
        CloseHandle(section);
        if (!address) {
            errno = ENOMEM;
            return failed;
        }
        DWORD previous = 0;
        if (!VirtualProtect(address, rounded, protection, &previous)) {
            UnmapViewOfFile(address);
            errno = EACCES;
            return failed;
        }
        const auto start = reinterpret_cast<uintptr_t>(address);
        try {
            Mapping mapping{start, start + rounded, address, Arm64, true, nullptr, (access & 2) != 0, 7, protection};
            if (Arm64 && (access & 4))
                mapping.executable.emplace_back(start, start + rounded);
            arm64MappingIndexDirty = true;
            mappings.emplace(start, std::move(mapping));
        }
        catch (const std::bad_alloc&) {
            UnmapViewOfFile(address);
            errno = ENOMEM;
            return failed;
        }
        return populate(address);
    }
    if (flags & MapFixed) {
        const auto start = reinterpret_cast<uintptr_t>(hint);
        const auto existing = FindMapping(start, rounded);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        if (start % system.dwPageSize || existing == mappings.end() || existing->second.view ||
            existing->second.arm64 != Arm64) {
            errno = EINVAL;
            return failed;
        }
        const bool fromFile = (flags & MapAnonymous) == 0;
        std::vector<uint8_t> contents;
        if (fromFile) {
            // ART's ELF reader overlays file segments on its own anonymous
            // reservation. MAP_PRIVATE uses a copy, preserving adjacent pages
            // and avoiding Windows' larger file-view alignment requirement.
            if (type != MapPrivate || offset < 0 || offset % system.dwPageSize) {
                errno = EINVAL;
                return failed;
            }
            std::unique_lock<std::mutex> memoryLock;
            const auto memory = LockMemoryFile(fd, memoryLock);
            if (memory && ((access & ~memory->mappingProtection) || ((access & 4) && !(memory->mode & 0111)))) {
                errno = EACCES;
                return failed;
            }
            const auto file = FileDescriptorHandle(fd);
            if (file == INVALID_HANDLE_VALUE)
                return failed;
            try {
                contents.resize(size);
            }
            catch (const std::bad_alloc&) {
                errno = ENOMEM;
                return failed;
            }
            if (TransferAtOffset<false>(file, contents.data(), contents.size(), offset, memory.get()) < 0)
                return failed;
        }
        if constexpr (Arm64)
            arm64MappingGeneration.fetch_add(1, std::memory_order_release);
        auto& mapping = existing->second;
        auto removed = CodeRangesAfter(mapping, start, rounded, false);
        auto code = CodeRangesAfter(mapping, start, rounded, Arm64 && (access & 4));
        mapping.uniformProtection = 0;
        mapping.regions.clear();
        if (!VirtualFree(hint, rounded, MEM_DECOMMIT)) {
            errno = EINVAL;
            return failed;
        }
        mapping.executable.swap(removed);
        if (!access && !fromFile) {
            mapping.executable.swap(code);
            if (start == mapping.start && rounded == mapping.end - start)
                mapping.uniformProtection = protection;
            return populate(hint);
        }
        if (VirtualAlloc(hint, rounded, MEM_COMMIT, fromFile ? PAGE_READWRITE : protection)) {
            if (fromFile) {
                std::memcpy(hint, contents.data(), contents.size());
                DWORD previous;
                if (!VirtualProtect(hint, rounded, protection, &previous)) {
                    errno = EACCES;
                    return failed;
                }
            }
            mapping.executable.swap(code);
            if (start == mapping.start && rounded == mapping.end - start)
                mapping.uniformProtection = protection;
            return populate(hint);
        }
        errno = ENOMEM;
        return failed;
    }
    const bool fromFile = (flags & MapAnonymous) == 0;
    if (fromFile && (fd < 0 || offset < 0)) {
        errno = EINVAL;
        return failed;
    }
    std::unique_lock<std::mutex> memoryLock;
    const auto memory = fromFile ? LockMemoryFile(fd, memoryLock) : nullptr;
    if (memory && ((access & ~memory->mappingProtection) || ((access & 4) && !(memory->mode & 0111)))) {
        errno = EACCES;
        return failed;
    }
    const auto file = fromFile ? FileDescriptorHandle(fd) : INVALID_HANDLE_VALUE;
    if (fromFile && file == INVALID_HANDLE_VALUE)
        return failed;
    const DWORD allocate = MEM_RESERVE | ((access || fromFile) ? MEM_COMMIT : 0);
    void* address = hint ? VirtualAlloc(hint, rounded, allocate, fromFile ? PAGE_READWRITE : protection) : nullptr;
    if constexpr (Arm64) {
        if (!address && hint) {
            // Android hints are page aligned, but Windows reserves in larger allocation units.
            // Preserve useful low-address hints before an arbitrary-address fallback. ART
            // rejects high results and otherwise probes large low-address ranges page by page.
            SYSTEM_INFO system{};
            GetSystemInfo(&system);
            const uintptr_t granularity = system.dwAllocationGranularity;
            const auto start = reinterpret_cast<uintptr_t>(hint);
            if (granularity && start <= UINTPTR_MAX - granularity) {
                auto next = start + granularity - start % granularity;
                constexpr uintptr_t lowLimit = uintptr_t{1} << 32;
                if (start < lowLimit) {
                    // Skip whole Windows regions, never individual pages. Both work and
                    // address range are bounded; VirtualAlloc still arbitrates allocation races.
                    for (unsigned regions = 0; regions < 64 && next < lowLimit && rounded <= lowLimit - next;
                         ++regions) {
                        MEMORY_BASIC_INFORMATION region{};
                        if (!VirtualQuery(reinterpret_cast<void*>(next), &region, sizeof(region)))
                            break;
                        const auto base = reinterpret_cast<uintptr_t>(region.BaseAddress);
                        if (region.RegionSize > UINTPTR_MAX - base)
                            break;
                        const auto end = base + region.RegionSize;
                        if (end <= next)
                            break;
                        if (region.State == MEM_FREE && rounded <= end - next) {
                            address = VirtualAlloc(reinterpret_cast<void*>(next), rounded, allocate,
                                                   fromFile ? PAGE_READWRITE : protection);
                            if (address)
                                break;
                        }
                        if (end > UINTPTR_MAX - (granularity - 1))
                            break;
                        next = (end + granularity - 1) / granularity * granularity;
                    }
                }
                else {
                    address = VirtualAlloc(reinterpret_cast<void*>(next), rounded, allocate,
                                           fromFile ? PAGE_READWRITE : protection);
                }
            }
        }
    }
    if (!address)
        address = VirtualAlloc(nullptr, rounded, allocate, fromFile ? PAGE_READWRITE : protection);
    if (!address) {
        errno = ENOMEM;
        return failed;
    }
    if (fromFile) {
        DWORD oldProtection = 0;
        const auto copied = TransferAtOffset<false>(file, address, size, offset, memory.get());
        if (copied < 0 || !VirtualProtect(address, rounded, protection, &oldProtection)) {
            const auto error = copied < 0 ? errno : EACCES;
            VirtualFree(address, 0, MEM_RELEASE);
            errno = error;
            return failed;
        }
    }
    const auto start = reinterpret_cast<uintptr_t>(address);
    try {
        Mapping mapping{start,     start + rounded, nullptr, Arm64,
                        !fromFile, memory,          false,   memory ? memory->mappingProtection : 7,
                        protection};
        if (Arm64 && (access & 4))
            mapping.executable.emplace_back(start, start + rounded);
        arm64MappingIndexDirty = true;
        mappings.emplace(start, std::move(mapping));
    }
    catch (const std::bad_alloc&) {
        VirtualFree(address, 0, MEM_RELEASE);
        errno = ENOMEM;
        return failed;
    }
    return populate(address);
}

template <bool Arm64 = false> int UnmapMemory(void* address, size_t size)
{
    const auto start = reinterpret_cast<uintptr_t>(address);
    const auto rounded = RoundPage(size);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if (start % system.dwPageSize) {
        errno = EINVAL;
        return -1;
    }
    const std::lock_guard lock(mappingMutex);
    const auto it = FindMapping(start, rounded);
    if (!rounded || it == mappings.end() || it->second.arm64 != Arm64 ||
        (start != it->second.start && start + rounded != it->second.end)) {
        errno = EINVAL;
        return -1;
    }
    if constexpr (Arm64)
        arm64MappingGeneration.fetch_add(1, std::memory_order_release);
    if (start == it->second.start && start + rounded == it->second.end) {
        const auto view = it->second.view;
        if (!(view ? UnmapViewOfFile(view) : VirtualFree(reinterpret_cast<void*>(it->first), 0, MEM_RELEASE))) {
            errno = EINVAL;
            return -1;
        }
        if (view && it->second.memory) {
            const std::lock_guard guard(it->second.memory->mutex);
            --it->second.memory->views;
            if (it->second.writableView)
                --it->second.memory->writableViews;
        }
        arm64MappingIndexDirty = true;
        mappings.erase(it);
    }
    else {
        if (it->second.view && !Arm64) {
            errno = EINVAL;
            return -1;
        }
        auto code = CodeRangesAfter(it->second, start, rounded, false);
        DWORD previous;
        if (it->second.view ? !VirtualProtect(address, rounded, PAGE_NOACCESS, &previous)
                            : !VirtualFree(address, rounded, MEM_DECOMMIT)) {
            errno = EINVAL;
            return -1;
        }
        it->second.executable.swap(code);
        it->second.regions.clear();
        if (start == it->second.start)
            it->second.start += rounded;
        else
            it->second.end -= rounded;
    }
    return 0;
}

template <bool Arm64 = false> int ProtectMemory(void* address, size_t size, int access)
{
    const auto protection = PageProtection(Arm64 && (access & 4) ? (access & 3) | 1 : access);
    const auto rounded = RoundPage(size);
    const auto start = reinterpret_cast<uintptr_t>(address);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if ((access & ~7) || !protection || (size && !rounded) || start % system.dwPageSize) {
        errno = EINVAL;
        return -1;
    }
    if (!size)
        return 0;
    const std::lock_guard lock(mappingMutex);
    const auto mapping = FindMapping(start, rounded);
    if (mapping == mappings.end() || mapping->second.arm64 != Arm64) {
        if constexpr (!Arm64) {
            if ((access == 1 || access == 3) && ProtectAndroidElfData(start, rounded, access == 3))
                return 0;
            ULONG_PTR low = 0, high = 0;
            GetCurrentThreadStackLimits(&low, &high);
            if ((access == 0 || access == 1 || access == 3) && start >= low && start < high &&
                rounded <= high - start) {
                // ART installs a guard at the bottom of the current native stack.
                // Those pages can still be reserved by Windows at this point.
                if (VirtualAlloc(address, rounded, MEM_COMMIT, protection))
                    return 0;
                errno = ENOMEM;
                return -1;
            }
        }
        errno = ENOMEM;
        return -1;
    }
    if (access & ~mapping->second.memoryProtection) {
        errno = EACCES;
        return -1;
    }
    std::unique_lock<std::mutex> memoryLock;
    if (const auto& memory = mapping->second.memory) {
        memoryLock = std::unique_lock(memory->mutex);
        if (((access & 4) && !(memory->mode & 0111)) ||
            ((access & 2) && mapping->second.view && !mapping->second.writableView)) {
            errno = EACCES;
            return -1;
        }
    }
    DWORD old = 0;
    auto code = CodeRangesAfter(mapping->second, start, rounded, Arm64 && (access & 4));
    if constexpr (Arm64)
        arm64MappingGeneration.fetch_add(1, std::memory_order_release);
    const auto uniformProtection =
        start == mapping->second.start && rounded == mapping->second.end - start ? protection : 0;
    mapping->second.uniformProtection = 0;
    mapping->second.regions.clear();
    if (!mapping->second.view) {
        if (access) {
            // PROT_NONE mappings are reserved without committing physical pages.
            if (!VirtualAlloc(address, rounded, MEM_COMMIT, protection)) {
                errno = ENOMEM;
                return -1;
            }
        }
        else {
            for (auto position = start; position < start + rounded;) {
                auto* page = reinterpret_cast<void*>(position);
                MEMORY_BASIC_INFORMATION region{};
                if (!VirtualQuery(page, &region, sizeof(region))) {
                    errno = ENOMEM;
                    return -1;
                }
                const auto next =
                    std::min(start + rounded, reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize);
                if (region.State == MEM_COMMIT && !VirtualProtect(page, next - position, protection, &old)) {
                    errno = EACCES;
                    return -1;
                }
                position = next;
            }
            mapping->second.uniformProtection = uniformProtection;
            mapping->second.executable.swap(code);
            return 0;
        }
    }
    if (!VirtualProtect(address, rounded, protection, &old)) {
        errno = EACCES;
        return -1;
    }
    mapping->second.uniformProtection = uniformProtection;
    mapping->second.executable.swap(code);
    return 0;
}

template <bool Arm64 = false> int SyncMemory(void* address, size_t size, int flags)
{
    constexpr int Async = 1, Invalidate = 2, Sync = 4;
    const auto start = reinterpret_cast<uintptr_t>(address);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if (start % system.dwPageSize || (flags & ~(Async | Invalidate | Sync)) ||
        (flags & (Async | Sync)) == (Async | Sync)) {
        errno = EINVAL;
        return -1;
    }
    if (!size)
        return 0;
    const std::lock_guard lock(mappingMutex);
    const auto mapping = FindMapping(start, size);
    if (mapping == mappings.end() || mapping->second.arm64 != Arm64) {
        errno = ENOMEM;
        return -1;
    }
    if (mapping->second.view && !FlushViewOfFile(address, size)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

const char* ErrorText(int code)
{
    // Android x86_64/ARM64 use asm-generic errno, not the Windows UCRT values.
    static constexpr auto descriptions = [] {
        std::array<const char*, 134> values{};
#define STONE_ERRNO_DESCRIPTION(number, description) values[number] = description;
#include "errno-descriptions.inc"
#undef STONE_ERRNO_DESCRIPTION
        return values;
    }();
    if (code >= 0 && static_cast<size_t>(code) < descriptions.size() && descriptions[code])
        return descriptions[code];
    thread_local char text[64];
    const int saved = errno;
    std::snprintf(text, sizeof(text), "Unknown error %d", code);
    errno = saved;
    return text;
}

char*** EnvironmentSlot()
{
    static const auto slot = [] {
        // Unicode Windows entry points initialize only the wide CRT environment.
        // Image code can read environ directly before any narrow getenv call.
        if (_initialize_narrow_environment() != 0)
            throw std::runtime_error("could not initialize the Android environment");
        return __p__environ();
    }();
    return slot;
}

const char* GetEnv(const char* key)
{
    // Bionic returns a pointer to the current environment entry.
    const std::string_view name(key);
    if (name == "ANDROID_ROOT")
        return "/system";
    const auto environment = *EnvironmentSlot();
    if (!environment)
        return nullptr;
    for (auto entry = environment; *entry; ++entry) {
        const std::string_view pair(*entry);
        const auto separator = pair.find('=');
        if (separator != pair.npos && pair.substr(0, separator) == name)
            return *entry + separator + 1;
    }
    return nullptr;
}

template <class T>
int FormatArgument(char* output, size_t size, const std::string& spec, const std::array<int, 2>& fields, unsigned count,
                   T value)
{
    if (count == 2)
        return std::snprintf(output, size, spec.c_str(), fields[0], fields[1], value);
    if (count == 1)
        return std::snprintf(output, size, spec.c_str(), fields[0], value);
    return std::snprintf(output, size, spec.c_str(), value);
}

template <typename Next, typename ResolveString>
int FormatArguments(char* output, size_t size, const char* format, Next&& next, ResolveString&& resolveString)
{
    size_t length = 0;
    const auto append = [&](char ch) {
        if (size && length < size - 1)
            output[length] = ch;
        ++length;
    };
    try {
        while (*format) {
            if (*format != '%') {
                append(*format++);
                continue;
            }
            const char* begin = format++;
            if (*format == '%') {
                append(*format++);
                continue;
            }
            while (*format && std::strchr("-+ #0", *format))
                ++format;
            std::array<int, 2> fields{};
            unsigned count = 0;
            for (unsigned i = 0; i < 2; ++i) {
                if (i && *format != '.')
                    break;
                if (i)
                    ++format;
                if (*format == '*') {
                    fields[count++] = static_cast<int>(next(false));
                    ++format;
                }
                else
                    while (*format >= '0' && *format <= '9')
                        ++format;
            }
            const char* modifier = format;
            if (*format && std::strchr("hljztL", *format)) {
                ++format;
                if ((*modifier == 'h' || *modifier == 'l') && *format == *modifier)
                    ++format;
            }
            const char type = *format;
            if (!type)
                break;
            ++format;
            std::string spec(begin, modifier);
            const bool integer = std::strchr("diouxX", type) != nullptr;
            const bool wideInteger = integer && std::strchr("ljzt", *modifier) != nullptr;
            if (wideInteger)
                spec += "ll";
            else
                spec.append(modifier, format - 1);
            spec += type;
            char* destination = size && length < size ? output + length : nullptr;
            const size_t remaining = destination ? size - length : 0;
            const auto write = [&](auto value) {
                return FormatArgument(destination, remaining, spec, fields, count, value);
            };
            int result = -1;
            if (std::strchr("aAeEfFgG", type) && *modifier != 'L')
                result = write(std::bit_cast<double>(next(true)));
            else if (type == 'd' || type == 'i') {
                if (wideInteger)
                    result = write(static_cast<int64_t>(next(false)));
                else
                    result = write(static_cast<int>(next(false)));
            }
            else if (integer) {
                if (wideInteger)
                    result = write(next(false));
                else
                    result = write(static_cast<unsigned>(next(false)));
            }
            else if (type == 'p')
                result = write(reinterpret_cast<void*>(next(false)));
            else if (type == 's' && modifier == format - 1) {
                const auto value = resolveString(next(false));
                result = write(value ? value : "(null)");
            }
            else if (type == 'c' && modifier == format - 1)
                result = write(static_cast<int>(next(false)));
            if (result < 0) {
                if (size)
                    output[std::min(length, size - 1)] = 0;
                errno = EINVAL;
                return -1;
            }
            length += static_cast<size_t>(result);
        }
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
    if (size)
        output[std::min(length, size - 1)] = 0;
    return length <= INT_MAX ? static_cast<int>(length) : -1;
}

int AndroidVsnprintf(char* output, size_t size, const char* format, AndroidVaList* args)
{
    return FormatArguments(
        output, size, format,
        [args](bool floating) {
            return args->next(floating);
        },
        [](uint64_t address) {
            return reinterpret_cast<const char*>(address);
        });
}

bool WideFormatHas(const char* choices, uint32_t ch)
{
    return ch && ch < 0x80 && std::strchr(choices, static_cast<int>(ch));
}

void SkipWideFormatField(const uint32_t*& format)
{
    if (*format == '*')
        ++format;
    else
        while (*format >= '0' && *format <= '9')
            ++format;
}

int64_t WideFormatField(const uint32_t* field, AndroidVaList& args, int fallback)
{
    if (!field)
        return fallback;
    if (*field == '*')
        return static_cast<int32_t>(args.next());
    int64_t result = 0;
    while (*field >= '0' && *field <= '9' && result <= INT_MAX) {
        result = result * 10 + (*field++ - '0');
    }
    return result;
}

// Decode only as many code points as the precision permits. A precision-
// limited %s argument can refer to an array without a trailing NUL.
int AppendUtf8(std::vector<uint32_t>& text, const char* bytes, size_t limit, size_t capacity)
{
    auto* input = reinterpret_cast<const uint8_t*>(bytes);
    while (text.size() < limit && *input) {
        if (text.size() == capacity)
            return 75; // Bionic EOVERFLOW
        if (*input >= 0x80 && !U8_IS_LEAD(*input))
            return 84; // Bionic EILSEQ
        const int expected = 1 + U8_COUNT_TRAIL_BYTES(*input);
        int count = 1;
        while (count < expected && input[count])
            ++count;
        int consumed = 0;
        UChar32 codePoint;
        U8_NEXT(input, consumed, count, codePoint);
        if (codePoint < 0)
            return 84; // Bionic EILSEQ
        text.push_back(static_cast<uint32_t>(codePoint));
        input += consumed;
    }
    return 0;
}

// Android wchar_t is UTF-32. Windows CRT wide-string conversion uses UTF-16
// and cannot implement the Bionic UTF-8 string ABI used by NDK libc++.
size_t AndroidMbstowcs(uint32_t* output, const char* source, size_t capacity)
{
    size_t written = 0;
    auto* input = reinterpret_cast<const uint8_t*>(source);
    while (!output || written < capacity) {
        if (!*input) {
            if (output)
                output[written] = 0;
            return written;
        }
        if (*input >= 0x80 && !U8_IS_LEAD(*input)) {
            errno = 84;
            return SIZE_MAX;
        }
        const int expected = 1 + U8_COUNT_TRAIL_BYTES(*input);
        int available = 1;
        while (available < expected && input[available])
            ++available;
        int consumed = 0;
        UChar32 codePoint;
        U8_NEXT(input, consumed, available, codePoint);
        if (codePoint < 0) {
            errno = 84;
            return SIZE_MAX;
        }
        if (output)
            output[written] = static_cast<uint32_t>(codePoint);
        ++written;
        input += consumed;
    }
    return written;
}

size_t AndroidWcstombs(char* output, const uint32_t* source, size_t capacity)
{
    size_t written = 0;
    while (!output || written < capacity) {
        const auto codePoint = *source;
        if (!codePoint) {
            if (output)
                output[written] = 0;
            return written;
        }
        if (codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
            errno = 84;
            return SIZE_MAX;
        }
        uint8_t bytes[4];
        int count = 0;
        U8_APPEND_UNSAFE(bytes, count, codePoint);
        if (output) {
            if (static_cast<size_t>(count) > capacity - written)
                break;
            std::memcpy(output + written, bytes, count);
        }
        written += count;
        ++source;
    }
    return written;
}

int AndroidVswprintf(uint32_t* output, size_t size, const uint32_t* format, AndroidVaList* args)
{
    constexpr int Overflow = 75, IllegalSequence = 84;
    size_t written = 0;
    const auto fail = [&](int error) {
        if (size)
            output[written] = 0;
        errno = error;
        return -1;
    };
    if (!size)
        return fail(Overflow);
    const size_t capacity = std::min<size_t>(size - 1, INT_MAX);
    try {
        std::vector<uint32_t> text;
        while (*format) {
            const size_t remaining = capacity - written;
            if (*format != '%' || format[1] == '%') {
                if (!remaining)
                    return fail(Overflow);
                if (*format == '%')
                    ++format;
                output[written++] = *format++;
                continue;
            }
            const auto* begin = format++;
            bool left = false;
            while (WideFormatHas("-+ #0", *format))
                left |= *format++ == '-';
            const auto* widthField = format;
            SkipWideFormatField(format);
            const uint32_t* precisionField = nullptr;
            if (*format == '.') {
                precisionField = ++format;
                SkipWideFormatField(format);
            }
            const auto* modifier = format;
            if (WideFormatHas("hljztL", *format)) {
                ++format;
                if ((*modifier == 'h' || *modifier == 'l') && *format == *modifier)
                    ++format;
            }
            if (!*format)
                return fail(EINVAL);
            const auto type = *format++;
            text.clear();
            size_t width = 0;
            if (WideFormatHas("csCS", type)) {
                const auto fieldWidth = WideFormatField(widthField, *args, 0);
                left |= fieldWidth < 0;
                width = static_cast<size_t>(fieldWidth < 0 ? -fieldWidth : fieldWidth);
                if (width > remaining)
                    return fail(Overflow);
                const auto precision = WideFormatField(precisionField, *args, -1);
                const size_t limit = precision < 0 ? SIZE_MAX : static_cast<size_t>(precision);
                const bool wide = type == 'S' || type == 'C' || (*modifier == 'l' && modifier + 2 == format);
                const auto value = args->next();
                if (type == 'c' || type == 'C') {
                    if (!remaining)
                        return fail(Overflow);
                    const auto ch = wide ? static_cast<uint32_t>(value) : static_cast<uint8_t>(value);
                    if (!wide && ch >= 0x80)
                        return fail(IllegalSequence);
                    text.push_back(ch);
                }
                else if (!value || !wide) {
                    const char* str = value ? reinterpret_cast<const char*>(value) : "(null)";
                    const int error = AppendUtf8(text, str, limit, remaining);
                    if (error)
                        return fail(error);
                }
                else {
                    const auto* str = reinterpret_cast<const uint32_t*>(value);
                    while (text.size() < limit && *str) {
                        if (text.size() == remaining)
                            return fail(Overflow);
                        text.push_back(*str++);
                    }
                }
            }
            else {
                if (!WideFormatHas("diouxXaAeEfFgGp", type))
                    return fail(EINVAL);
                std::string spec;
                spec.reserve(static_cast<size_t>(format - begin));
                for (auto* ch = begin; ch != format; ++ch)
                    spec.push_back(static_cast<char>(*ch));
                auto retry = *args;
                char buffer[128];
                const int length = AndroidVsnprintf(buffer, sizeof(buffer), spec.c_str(), args);
                if (length < 0)
                    return fail(errno);
                // The CRT's numeric output is ASCII in Stone's default locale.
                if (static_cast<size_t>(length) > remaining)
                    return fail(Overflow);
                std::vector<char> extended;
                const char* str = buffer;
                if (static_cast<size_t>(length) >= sizeof(buffer)) {
                    extended.resize(static_cast<size_t>(length) + 1);
                    if (AndroidVsnprintf(extended.data(), extended.size(), spec.c_str(), &retry) != length) {
                        return fail(EINVAL);
                    }
                    str = extended.data();
                }
                const int error = AppendUtf8(text, str, SIZE_MAX, remaining);
                if (error)
                    return fail(error);
            }
            const auto padding = width > text.size() ? width - text.size() : 0;
            if (!left) {
                std::fill_n(output + written, padding, ' ');
                written += padding;
            }
            std::copy(text.begin(), text.end(), output + written);
            written += text.size();
            if (left) {
                std::fill_n(output + written, padding, ' ');
                written += padding;
            }
        }
    }
    catch (const std::bad_alloc&) {
        return fail(ENOMEM);
    }
    catch (const std::length_error&) {
        return fail(Overflow);
    }
    output[written] = 0;
    return static_cast<int>(written);
}

int ScanArguments(const char* input, const char* format, const std::function<uint64_t(size_t)>& destination,
                  FILE* stream = nullptr)
{
    try {
        std::string windowsFormat;
        std::vector<uint64_t> destinations;
        while (*format) {
            if (*format != '%') {
                windowsFormat += *format++;
                continue;
            }
            const char* start = format++;
            if (*format == '%') {
                windowsFormat.append(start, ++format);
                continue;
            }
            const bool skip = *format == '*';
            if (skip)
                ++format;
            size_t width = 0;
            while (*format >= '0' && *format <= '9') {
                if (width > (UINT_MAX - 9u) / 10) {
                    errno = EINVAL;
                    return -1;
                }
                width = width * 10 + (*format++ - '0');
            }
            const char* modifier = format;
            if (*format && std::strchr("hljztL", *format)) {
                ++format;
                if ((*modifier == 'h' || *modifier == 'l') && *format == *modifier)
                    ++format;
            }
            const char type = *format;
            if (!type || ((type == 'c' || type == 's' || type == '[') && modifier != format) ||
                (*modifier == 'L' && modifier != format)) {
                errno = EINVAL;
                return -1;
            }
            windowsFormat.append(start, modifier);
            if (modifier != format && std::strchr("diouxXn", type) && std::strchr("ljzt", *modifier))
                windowsFormat += "ll";
            else
                windowsFormat.append(modifier, format);
            windowsFormat += *format++;
            if (type == '[') {
                const char* set = format;
                if (*format == '^')
                    ++format;
                if (*format == ']')
                    ++format;
                while (*format && *format != ']')
                    ++format;
                if (!*format) {
                    errno = EINVAL;
                    return -1;
                }
                windowsFormat.append(set, ++format);
            }
            if (!skip) {
                size_t bytes;
                if (type == 'c')
                    bytes = width ? width : 1;
                else if (type == 's' || type == '[')
                    bytes =
                        stream ? (width ? width + 1 : 1) : std::min(width ? width : SIZE_MAX, std::strlen(input)) + 1;
                else if (std::strchr("diouxXn", type)) {
                    bytes = modifier == format - 1 ? 4 : *modifier == 'h' ? (modifier[1] == 'h' ? 1 : 2) : 8;
                }
                else if (std::strchr("aAeEfFgG", type))
                    bytes = *modifier == 'l' ? 8 : 4;
                else if (type == 'p')
                    bytes = sizeof(void*);
                else {
                    errno = EINVAL;
                    return -1;
                }
                destinations.push_back(destination(bytes));
            }
        }
        const auto arguments = reinterpret_cast<va_list>(destinations.data());
        return stream ? std::vfscanf(stream, windowsFormat.c_str(), arguments)
                      : std::vsscanf(input, windowsFormat.c_str(), arguments);
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

int AndroidVsscanf(const char* input, const char* format, AndroidVaList* args)
{
    return ScanArguments(input, format, [&](size_t) {
        return args->next();
    });
}

// NDK stdio addresses the three standard streams by indexing __sF.
struct alignas(8) AndroidStandardFile {
    char opaque[152];
};

AndroidStandardFile standardFiles[3]{};

FILE* HostFile(void* file)
{
    if (file == &standardFiles[0])
        return stdin;
    if (file == &standardFiles[1])
        return stdout;
    if (file == &standardFiles[2])
        return stderr;
    return static_cast<FILE*>(file);
}

struct AndroidMountEntry {
    char* device;
    char* directory;
    char* type;
    char* options;
    int frequency;
    int pass;
};

AndroidMountEntry* ReadMountEntry(void* stream)
{
    if (!stream) {
        errno = EINVAL;
        return nullptr;
    }
    thread_local std::array<std::string, 4> fields;
    thread_local AndroidMountEntry result{};
    std::array<char, 4096> buffer;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), HostFile(stream))) {
        std::string line(buffer.data());
        while (!line.ends_with('\n') && !std::feof(HostFile(stream))) {
            if (line.size() > 1024 * 1024) {
                errno = ERANGE;
                return nullptr;
            }
            if (!std::fgets(buffer.data(), static_cast<int>(buffer.size()), HostFile(stream)))
                break;
            line += buffer.data();
        }
        std::istringstream input(line);
        if (!(input >> fields[0]) || fields[0].starts_with('#'))
            continue;
        if (!(input >> fields[1] >> fields[2] >> fields[3]))
            continue;
        result.frequency = result.pass = 0;
        input >> result.frequency >> result.pass;
        for (auto& field : fields) {
            size_t written = 0;
            for (size_t i = 0; i < field.size(); ++i) {
                if (field[i] == '\\' && i + 3 < field.size() && field[i + 1] >= '0' && field[i + 1] <= '7' &&
                    field[i + 2] >= '0' && field[i + 2] <= '7' && field[i + 3] >= '0' && field[i + 3] <= '7') {
                    field[written++] = static_cast<char>(((field[i + 1] - '0') << 6) | ((field[i + 2] - '0') << 3) |
                                                         (field[i + 3] - '0'));
                    i += 3;
                }
                else
                    field[written++] = field[i];
            }
            field.resize(written);
        }
        result.device = fields[0].data();
        result.directory = fields[1].data();
        result.type = fields[2].data();
        result.options = fields[3].data();
        return &result;
    }
    return nullptr;
}

std::mutex streamModeMutex;
std::unordered_set<FILE*> callerLockedStreams;

bool CallerLocksStream(FILE* file)
{
    const std::lock_guard lock(streamModeMutex);
    return callerLockedStreams.contains(file);
}

int SetStreamLocking(void* guestFile, int mode)
{
    if (!guestFile || mode < 0 || mode > 2) {
        errno = EINVAL;
        return -1;
    }
    auto* file = HostFile(guestFile);
    try {
        const std::lock_guard lock(streamModeMutex);
        const int previous = callerLockedStreams.contains(file) ? 2 : 1;
        if (mode == 1)
            callerLockedStreams.erase(file);
        else if (mode == 2)
            callerLockedStreams.insert(file);
        return previous;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

size_t ReadFileStream(void* data, size_t size, size_t count, void* guestFile)
{
    auto* file = HostFile(guestFile);
    return CallerLocksStream(file) ? _fread_nolock(data, size, count, file) : std::fread(data, size, count, file);
}

int WriteStreamByte(int value, void* guestFile)
{
    auto* file = HostFile(guestFile);
    return CallerLocksStream(file) ? _fputc_nolock(value, file) : std::fputc(value, file);
}

intptr_t ReadDelimitedLine(char** line, size_t* capacity, int delimiter, void* guestFile)
{
    if (!line || !capacity || !guestFile) {
        errno = EINVAL;
        return -1;
    }
    auto* file = HostFile(guestFile);
    const bool lockStream = !CallerLocksStream(file);
    if (lockStream)
        _lock_file(file);
    const std::unique_ptr<FILE, decltype(&_unlock_file)> lock(lockStream ? file : nullptr, &_unlock_file);
    if (std::ferror(file))
        return -1;
    constexpr size_t Limit = static_cast<size_t>(INTPTR_MAX);
    const int end = static_cast<unsigned char>(delimiter);
    size_t length = 0;
    for (;;) {
        const int byte = _fgetc_nolock(file);
        if (byte == EOF)
            break;
        if (length == Limit) {
            errno = 75;
            return -1;
        } // Bionic EOVERFLOW
        const size_t required = length + 2;
        if (!*line || *capacity < required) {
            const size_t previous = *line ? *capacity : 0;
            const size_t grown = std::max(required, std::clamp(previous * 2, size_t{128}, Limit + 1));
            auto* buffer = static_cast<char*>(AndroidRealloc(*line, grown));
            if (!buffer) {
                errno = ENOMEM;
                return -1;
            }
            *line = buffer;
            *capacity = grown;
        }
        (*line)[length++] = static_cast<char>(byte);
        if (byte == end)
            break;
    }
    if (!length || std::ferror(file))
        return -1;
    (*line)[length] = '\0';
    return static_cast<intptr_t>(length);
}

FILE* TagFileStream(FILE* file)
{
    if (file)
        ExchangeFdOwnerTag(_fileno(file), 0, MakeFdOwnerTag(FileOwner, reinterpret_cast<uintptr_t>(file)));
    return file;
}

int CloseFileStream(void* guestFile)
{
    auto* file = HostFile(guestFile);
    const int fd = _fileno(file);
    const auto tag = (file == stdin || file == stdout || file == stderr)
                         ? 0
                         : MakeFdOwnerTag(FileOwner, reinterpret_cast<uintptr_t>(file));
    CheckFdOwnerClose(fd, tag);
    if (fd >= 0 && fd < GuestDescriptorLimit)
        procCommandLines[fd].store(false);
    if (fd >= 0)
        ForgetByteLocks(FileDescriptorHandle(fd));
    {
        const std::lock_guard lock(streamModeMutex);
        callerLockedStreams.erase(file);
    }
    const auto closing = AndroidWritableFileClosing(fd);
    const int result = std::fclose(file);
    if (result == 0)
        NotifyAndroidFileClosed(closing);
    return result;
}

struct AndroidStat {
    uint64_t device, inode, links;
    uint32_t mode, uid, gid, padding;
    uint64_t special;
    int64_t size, blockSize, blocks;
    AndroidTimespec accessed, modified, changed;
    int64_t reserved[3];
};

static_assert(sizeof(AndroidStat) == 144);

template <typename Stat> void CopyStat(const struct _stat64& src, Stat& dst)
{
    dst = {};
    dst.device = src.st_dev;
    dst.inode = src.st_ino;
    dst.links = src.st_nlink;
    dst.mode = src.st_mode;
    dst.size = src.st_size;
    dst.blockSize = 4096;
    dst.blocks = (src.st_size + 511) / 512;
    dst.accessed.seconds = src.st_atime;
    dst.modified.seconds = src.st_mtime;
    dst.changed.seconds = src.st_ctime;
}

template <typename Stat = AndroidStat> int StatFile(int fd, Stat* result)
{
    std::unique_lock<std::mutex> lock;
    const auto memory = LockMemoryFile(fd, lock);
    struct _stat64 value{};
    if (_fstat64(fd, &value) < 0)
        return -1;
    CopyStat(value, *result);
    if (IsProcCommandLineDescriptor(fd)) {
        result->mode = 0100444;
        result->uid = result->gid = AndroidApplicationUid();
        result->size = result->blocks = 0;
        result->links = 1;
    }
    FileIdentity identity;
    const auto handle = FileDescriptorHandle(fd);
    if (handle != INVALID_HANDLE_VALUE && ReadFileIdentity(handle, identity)) {
        result->device = identity.first;
        result->inode = identity.second;
    }
    if (memory) {
        result->mode = static_cast<uint32_t>((result->mode & ~0777u) | memory->mode);
    }
    return 0;
}

bool GuestAbsolutePath(const char* path)
{
    return path[0] == '/' || path[0] == '\\' || (path[0] && path[1] == ':');
}

std::filesystem::path PathAt(int fd, const char* path, std::error_code& error, bool writable, bool followFinal = true)
{
    if (!path)
        throw AndroidSystemPathError{EFAULT};
    if (fd == -100 || GuestAbsolutePath(path))
        return GuestPath(path, writable, followFinal);
    auto result = MapGuestPath(path, writable);
    const auto previous = _set_thread_local_invalid_parameter_handler(
        +[](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        });
    const auto handle = _get_osfhandle(fd);
    _set_thread_local_invalid_parameter_handler(previous);
    if (handle == -1 || handle == -2) {
        error = std::make_error_code(std::errc::bad_file_descriptor);
        return {};
    }
    const auto file = reinterpret_cast<HANDLE>(handle);
    const auto length = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!length) {
        error = std::make_error_code(std::errc::not_a_directory);
        return {};
    }
    std::wstring base(length, L'\0');
    const auto written = GetFinalPathNameByHandleW(file, base.data(), length, FILE_NAME_NORMALIZED);
    if (!written || written >= length) {
        error = std::make_error_code(std::errc::not_a_directory);
        return {};
    }
    base.resize(written);
    const std::filesystem::path directory(base);
    const auto resolved = result.empty() ? directory : directory / result;
    if (writable && IsReadOnlyImagePath(resolved))
        throw AndroidSystemPathError{EROFS};
    const auto cachedDirectory = AndroidSystemCachedPath(directory);
    if (!result.empty() && cachedDirectory && IsAndroidSystemGuestReadOnly(*cachedDirectory)) {
        const auto guest = AndroidSystemCachedPath(resolved);
        if (!guest)
            throw AndroidSystemPathError{ENOENT};
        return ResolveAndroidSystemFile(*guest);
    }
    auto target = ResolveAndroidSymbolicLinks(resolved, writable, followFinal, &MapGuestPath);
    if (writable && IsReadOnlyImagePath(target))
        throw AndroidSystemPathError{EROFS};
    CheckAndroidSharedStorageTarget(resolved, target);
    return target;
}

template <bool Writable = false, typename Function>
int WithPathAt(int fd, const char* path, Function function, bool followFinal = true)
{
    try {
        std::error_code error;
        const auto resolved = PathAt(fd, path, error, Writable, followFinal);
        if (error) {
            errno = error.value();
            return -1;
        }
        if constexpr (Writable) {
            if (IsReadOnlyImagePath(resolved)) {
                errno = EROFS;
                return -1;
            }
        }
        return function(resolved.c_str());
    }
    catch (const AndroidSystemPathError& error) {
        errno = error.code;
        return -1;
    }
    catch (const std::filesystem::filesystem_error&) {
        errno = EINVAL;
        return -1;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

AndroidTimespec WalkTime(FILETIME time);

int RenameAt(int oldFd, const char* oldPath, int newFd, const char* newPath)
{
    return WithPathAt<true>(
        oldFd, oldPath,
        [&](const wchar_t* source) {
            return WithPathAt<true>(
                newFd, newPath,
                [&](const wchar_t* destination) {
                    if (MoveFileExW(source, destination, MOVEFILE_REPLACE_EXISTING))
                        return 0;
                    switch (GetLastError()) {
                    case ERROR_FILE_NOT_FOUND:
                    case ERROR_PATH_NOT_FOUND:
                        errno = ENOENT;
                        break;
                    case ERROR_NOT_SAME_DEVICE:
                        errno = EXDEV;
                        break;
                    default:
                        errno = EACCES;
                        break;
                    }
                    return -1;
                },
                false);
        },
        false);
}

int PathConfiguration(const wchar_t* wide, int key)
{
    // Bionic _PC_* values are ABI constants, not Windows CRT values.
    if (key < 0 || key > 19) {
        errno = EINVAL;
        return -1;
    }
    if (_waccess(wide, 0) != 0)
        return -1;
    switch (key) {
    case 4: { // _PC_NAME_MAX
        const auto absolute = std::filesystem::absolute(wide);
        std::wstring volume(32768, L'\0');
        DWORD limit = 0;
        if (!GetVolumePathNameW(absolute.c_str(), volume.data(), static_cast<DWORD>(volume.size())) ||
            !GetVolumeInformationW(volume.c_str(), nullptr, 0, nullptr, &limit, nullptr, nullptr, 0)) {
            errno = EIO;
            return -1;
        }
        return static_cast<int>(limit);
    }
    case 5:
        return 4096; // _PC_PATH_MAX, Android path limit including NUL
    case 15:
        return 1; // _PC_NO_TRUNC
    default:
        errno = AndroidNotImplemented;
        return -1;
    }
}

int64_t Pathconf(const char* path, int key)
{
    return WithPathAt(-100, path, [key](const wchar_t* wide) {
        return PathConfiguration(wide, key);
    });
}

char* CurrentDirectory(char* output, size_t size)
{
    try {
        const auto path = std::filesystem::current_path();
        const auto guest = AndroidSystemCachedPath(path);
        if (!guest)
            return _getcwd(output, static_cast<int>(std::min<size_t>(size, INT_MAX)));
        const auto text = std::filesystem::path(AndroidSystemVisiblePath(*guest)).generic_u8string();
        const auto required = text.size() + 1;
        if ((output && !size) || (size && size < required)) {
            errno = size ? ERANGE : EINVAL;
            return nullptr;
        }
        if (!output) {
            output = static_cast<char*>(std::malloc(size ? size : required));
            if (!output) {
                errno = ENOMEM;
                return nullptr;
            }
        }
        std::memcpy(output, text.c_str(), required);
        return output;
    }
    catch (const std::filesystem::filesystem_error&) {
        errno = ENOENT;
        return nullptr;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return nullptr;
    }
}

char* Realpath(const char* path, char* output)
{
    try {
        const auto wide = GuestPath(path);
        std::error_code error;
        const auto resolved = std::filesystem::canonical(wide, error);
        if (error) {
            if (error == std::errc::no_such_file_or_directory)
                errno = ENOENT;
            else if (error == std::errc::permission_denied)
                errno = EACCES;
            else if (error == std::errc::filename_too_long)
                errno = AndroidNameTooLong;
            else if (error == std::errc::not_a_directory)
                errno = ENOTDIR;
            else
                errno = EIO;
            return nullptr;
        }
        auto text = resolved.generic_u8string();
        if (path[0] == '/' || path[0] == '\\') {
            const std::filesystem::path input(std::u8string_view(reinterpret_cast<const char8_t*>(path)));
            if (const auto guest = AndroidSystemGuestPath(input)) {
                const auto suffix = AndroidSystemCachedPath(resolved).value_or(*guest);
                text = std::filesystem::path(AndroidSystemVisiblePath(suffix)).generic_u8string();
            }
        }
        if (text.size() >= 4096) {
            errno = AndroidNameTooLong;
            return nullptr;
        }
        if (!output) {
            output = static_cast<char*>(std::malloc(text.size() + 1));
            if (!output) {
                errno = ENOMEM;
                return nullptr;
            }
        }
        std::memcpy(output, text.c_str(), text.size() + 1);
        return output;
    }
    catch (const AndroidSystemPathError& error) {
        errno = error.code;
        return nullptr;
    }
    catch (const std::filesystem::filesystem_error&) {
        errno = EINVAL;
        return nullptr;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return nullptr;
    }
}

bool IsOwnExecutableLink(std::string_view path)
{
    return IsOwnProcPath(path, "/exe");
}

std::shared_ptr<MemoryFile> MemoryFileLink(std::string_view path)
{
    constexpr std::string_view prefix = "/proc/";
    if (!path.starts_with(prefix))
        return nullptr;
    path.remove_prefix(prefix.size());
    const auto separator = path.find('/');
    if (separator == path.npos)
        return nullptr;
    const auto process = path.substr(0, separator);
    if (process != "self") {
        DWORD pid = 0;
        const auto parsed = std::from_chars(process.data(), process.data() + process.size(), pid);
        if (process.empty() || process.front() == '0' || parsed.ec != std::errc{} ||
            parsed.ptr != process.data() + process.size() || pid != GetCurrentProcessId())
            return nullptr;
    }
    path.remove_prefix(separator);
    if (!path.starts_with("/fd/"))
        return nullptr;
    path.remove_prefix(4);
    int fd = -1;
    const auto parsed = std::from_chars(path.data(), path.data() + path.size(), fd);
    if (path.empty() || fd < 0 || (path.size() > 1 && path.front() == '0') || parsed.ec != std::errc{} ||
        parsed.ptr != path.data() + path.size())
        return nullptr;
    return FindMemoryFile(fd);
}

intptr_t ReadGuestLink(const char* path, char* output, size_t capacity)
{
    if (!path || !output) {
        errno = EFAULT;
        return -1;
    }
    if (!capacity) {
        errno = EINVAL;
        return -1;
    }
    try {
        if (IsOwnProcPath(path, "/cmdline")) {
            errno = EINVAL; // cmdline is a regular proc file, not a symbolic link.
            return -1;
        }
        if (const auto memory = MemoryFileLink(path)) {
            const auto link = "/memfd:" + memory->name + " (deleted)";
            const auto copied = std::min(capacity, link.size());
            std::memcpy(output, link.data(), copied);
            return static_cast<intptr_t>(copied);
        }
        const std::filesystem::path input(std::u8string_view(reinterpret_cast<const char8_t*>(path)));
        if (const auto system = AndroidSystemGuestPath(input); system && IsAndroidSystemGuestReadOnly(*system)) {
            const auto link = std::filesystem::path(ReadAndroidSystemLink(*system)).u8string();
            const auto copied = std::min(capacity, link.size());
            std::memcpy(output, link.data(), copied);
            return static_cast<intptr_t>(copied);
        }
        if (!IsOwnExecutableLink(path)) {
            const auto host = GuestPath(path, false, false);
            const auto link = ReadAndroidSymbolicLink(host);
            if (!link) {
                if (GetFileAttributesW(host.c_str()) == INVALID_FILE_ATTRIBUTES)
                    return HostFileError(GetLastError());
                errno = EINVAL;
                return -1;
            }
            const auto copied = std::min(capacity, link->size());
            std::memcpy(output, link->data(), copied);
            return static_cast<intptr_t>(copied);
        }
        std::wstring name(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
        if (!length || length >= name.size()) {
            errno = EIO;
            return -1;
        }
        name.resize(length);
        const auto utf8 = std::filesystem::path(name).generic_u8string();
        const auto guest = AndroidGuestFilePath({reinterpret_cast<const char*>(utf8.data()), utf8.size()});
        const auto copied = std::min(capacity, guest.size());
        std::memcpy(output, guest.data(), copied);
        return static_cast<intptr_t>(copied);
    }
    catch (const AndroidSystemPathError& error) {
        errno = error.code;
        return -1;
    }
    catch (const std::filesystem::filesystem_error&) {
        errno = EIO;
        return -1;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

template <typename Stat = AndroidStat> int StatAt(int fd, const char* path, Stat* result, int flags)
{
    if (!result) {
        errno = EFAULT;
        return -1;
    }
    if (flags & ~0x100) {
        errno = EINVAL;
        return -1;
    }
    if (path && IsOwnProcPath(path, "/cmdline")) {
        *result = {};
        result->mode = 0100444;
        result->uid = result->gid = AndroidApplicationUid();
        result->links = 1;
        result->blockSize = 4096;
        return 0;
    }
    return WithPathAt(
        fd, path,
        [&](const wchar_t* wide) {
            if (flags & 0x100) {
                const auto target = ReadAndroidSymbolicLink(std::filesystem::path(wide));
                if (target) {
                    *result = {};
                    result->mode = 0120777;
                    result->links = 1;
                    result->size = target->size();
                    result->blockSize = 4096;
                    const auto file = CreateFileW(
                        wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                    if (file == INVALID_HANDLE_VALUE) {
                        errno = EIO;
                        return -1;
                    }
                    FileIdentity identity;
                    if (ReadFileIdentity(file, identity)) {
                        result->device = identity.first;
                        result->inode = identity.second;
                    }
                    BY_HANDLE_FILE_INFORMATION info{};
                    if (GetFileInformationByHandle(file, &info)) {
                        result->links = info.nNumberOfLinks;
                        result->accessed = WalkTime(info.ftLastAccessTime);
                        result->modified = result->changed = WalkTime(info.ftLastWriteTime);
                    }
                    CloseHandle(file);
                    return 0;
                }
            }
            struct _stat64 value{};
            if (_wstat64(wide, &value) < 0)
                return -1;
            CopyStat(value, *result);
            const auto file =
                CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                FileIdentity identity;
                if (ReadFileIdentity(file, identity)) {
                    result->device = identity.first;
                    result->inode = identity.second;
                }
                CloseHandle(file);
            }
            return 0;
        },
        (flags & 0x100) == 0);
}

template <typename Stat = AndroidStat> int StatPath(const char* path, Stat* result)
{
    return StatAt(-100, path, result, 0);
}

int StatVolume(const wchar_t* path, AndroidStatFs& result)
{
    const auto absolute = std::filesystem::absolute(path);
    if (GetFileAttributesW(absolute.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        errno = error == ERROR_ACCESS_DENIED ? EACCES : ENOENT;
        return -1;
    }
    std::wstring root(32768, L'\0');
    wchar_t filesystem[32]{};
    DWORD sectors = 0, sectorSize = 0, freeClusters = 0, totalClusters = 0;
    DWORD serial = 0, maxName = 0, flags = 0;
    ULARGE_INTEGER available{}, total{}, free{};
    if (!GetVolumePathNameW(absolute.c_str(), root.data(), static_cast<DWORD>(root.size())) ||
        !GetDiskFreeSpaceW(root.c_str(), &sectors, &sectorSize, &freeClusters, &totalClusters) ||
        !GetDiskFreeSpaceExW(root.c_str(), &available, &total, &free) ||
        !GetVolumeInformationW(root.c_str(), nullptr, 0, &serial, &maxName, &flags, filesystem,
                               ARRAYSIZE(filesystem)) ||
        !sectors || !sectorSize) {
        errno = GetLastError() == ERROR_ACCESS_DENIED ? EACCES : EIO;
        return -1;
    }
    result = {};
    const std::wstring_view fs(filesystem);
    if (fs == L"NTFS")
        result.type = 0x5346544e;
    else if (fs == L"exFAT")
        result.type = 0x2011bab0;
    else if (fs == L"FAT" || fs == L"FAT32")
        result.type = 0x4d44;
    result.blockSize = result.fragmentSize = uint64_t{sectors} * sectorSize;
    result.blocks = total.QuadPart / result.fragmentSize;
    result.freeBlocks = free.QuadPart / result.fragmentSize;
    result.availableBlocks = available.QuadPart / result.fragmentSize;
    result.fsid[0] = serial;
    result.fsid[0] = serial;
    result.nameLimit = maxName;
    if ((flags & FILE_READ_ONLY_VOLUME) || IsReadOnlyImagePath(absolute))
        result.flags |= 1; // ST_RDONLY
    return 0;
}

int StatFs(const char* path, AndroidStatFs* result)
{
    if (!*path) {
        errno = ENOENT;
        return -1;
    }
    return WithPathAt(-100, path, [&](const wchar_t* wide) {
        return StatVolume(wide, *result);
    });
}

int FstatFs(int fd, AndroidStatFs* result)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    return WithPathAt(fd, "", [&](const wchar_t* wide) {
        return StatVolume(wide, *result);
    });
}

template <typename Source> int StatVfs(Source source, AndroidStatVfs* result)
{
    AndroidStatFs volume;
    if (source(&volume) < 0)
        return -1;
    *result = {volume.blockSize,  volume.fragmentSize,    volume.blocks,
               volume.freeBlocks, volume.availableBlocks, volume.files,
               volume.freeFiles,  volume.freeFiles,       volume.fsid[0] | (uint64_t(volume.fsid[1]) << 32),
               volume.flags,      volume.nameLimit};
    return 0;
}

int OpenAt(int fd, const char* path, int flags, int mode)
{
    if (!path || IsOwnProcPath(path, "/cmdline"))
        return OpenDescriptor(path, flags, mode);
    if (IsRandomDevice(path))
        return OpenDescriptor(path, flags, mode);
    const auto open = [&](const wchar_t* wide) {
        return OpenWideDescriptor(wide, flags, mode);
    };
    const bool follow = !(flags & 0x20000) && (flags & 0xc0) != 0xc0;
    return AndroidOpenWrites(flags) ? WithPathAt<true>(fd, path, open, follow) : WithPathAt(fd, path, open, follow);
}

int TruncatePath(const char* path, int64_t length)
{
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }
    const int fd = OpenDescriptor(path, 1, 0);
    if (fd < 0)
        return -1;
    const int result = TruncateDescriptor(fd, length);
    const int error = errno;
    CloseAndroidFd(fd);
    if (result < 0)
        errno = error;
    return result;
}

int ChmodAt(int fd, const char* path, int mode, int flags)
{
    if (flags != 0) {
        errno = EINVAL;
        return -1;
    }
    return WithPathAt<true>(fd, path, [mode](const wchar_t* name) {
        return _wchmod(name, _S_IREAD | ((mode & 0200) ? _S_IWRITE : 0));
    });
}

template <bool Symbolic> int CreateGuestLink(const char* from, const char* to)
{
    if (!from || !to) {
        errno = EFAULT;
        return -1;
    }
    if (!*from || !*to) {
        errno = ENOENT;
        return -1;
    }
    try {
        const auto link = GuestPath(to, true, false);
        if constexpr (Symbolic)
            return CreateAndroidSymbolicLink(from, link);
        const auto source = GuestPath(from, true, false);
        if (IsReadOnlyImagePath(source)) {
            errno = EROFS;
            return -1;
        }
        std::error_code error;
        std::filesystem::create_hard_link(source, link, error);
        if (!error)
            return 0;
        if (error == std::errc::no_such_file_or_directory)
            errno = ENOENT;
        else if (error == std::errc::file_exists)
            errno = EEXIST;
        else if (error == std::errc::cross_device_link)
            errno = EXDEV;
        else if (error == std::errc::permission_denied)
            errno = EACCES;
        else if (error.value() == ERROR_PRIVILEGE_NOT_HELD)
            errno = EPERM;
        else
            errno = EIO;
        return -1;
    }
    catch (const AndroidSystemPathError& error) {
        errno = error.code;
        return -1;
    }
    catch (const std::filesystem::filesystem_error&) {
        errno = EINVAL;
        return -1;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

struct AndroidDir {
    intptr_t handle = -1;
    int fd;
    struct _wfinddata64_t data;
    std::wstring pattern;
    std::vector<std::pair<unsigned, std::wstring>> imageEntries;
    std::mutex mutex;
    bool first = true;
    AndroidDirent entry{};
};

AndroidDir* OpenWideDir(const wchar_t* path, int fd)
{
    try {
        auto dir = std::make_unique<AndroidDir>();
        dir->fd = fd;
        if (const auto system = AndroidSystemCachedPath(path)) {
            dir->imageEntries = ListAndroidSystemDirectory(*system);
        }
        else {
            dir->pattern = (std::filesystem::path(path) / L"*").native();
            dir->handle = _wfindfirst64(dir->pattern.c_str(), &dir->data);
            if (dir->handle == -1)
                return nullptr;
        }
        ExchangeFdOwnerTag(fd, 0, MakeFdOwnerTag(DirectoryOwner, reinterpret_cast<uintptr_t>(dir.get())));
        return dir.release();
    }
    catch (const AndroidSystemPathError& error) {
        errno = error.code;
        return nullptr;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return nullptr;
    }
}

AndroidDir* OpenDir(const char* path)
{
    AndroidDir* result = nullptr;
    WithPathAt(-100, path, [&](const wchar_t* wide) {
        const int fd = OpenWideDescriptor(wide, 0x90000, 0); // O_DIRECTORY | O_CLOEXEC
        if (fd < 0)
            return -1;
        result = OpenWideDir(wide, fd);
        if (result)
            return 0;
        const int error = errno;
        CloseAndroidFd(fd);
        errno = error;
        return -1;
    });
    return result;
}

AndroidDir* OpenFdDir(int fd)
{
    if (fd < 0) {
        errno = EBADF;
        return nullptr;
    }
    AndroidDir* result = nullptr;
    WithPathAt(fd, "", [&](const wchar_t* path) {
        result = OpenWideDir(path, fd);
        return result ? 0 : -1;
    });
    return result;
}

AndroidDirent* ReadDirEntry(AndroidDir* dir)
{
    auto& entry = dir->entry;
    const wchar_t* name;
    if (!dir->imageEntries.empty()) {
        const auto offset = static_cast<size_t>(entry.offset);
        if (offset >= dir->imageEntries.size())
            return nullptr;
        const auto& item = dir->imageEntries[offset];
        entry.type = static_cast<uint8_t>(item.first);
        name = item.second.c_str();
    }
    else {
        if (!dir->first) {
            const int saved = errno;
            if (_wfindnext64(dir->handle, &dir->data) < 0) {
                if (errno == ENOENT)
                    errno = saved;
                return nullptr;
            }
        }
        dir->first = false;
        entry.type = (dir->data.attrib & _A_SUBDIR) ? 4 : 8;
        name = dir->data.name;
        if (IsAndroidSymbolicLink(std::filesystem::path(dir->pattern).parent_path() / name))
            entry.type = 10;
    }
    ++entry.offset;
    entry.length = sizeof(entry);
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, -1, entry.name, sizeof(entry.name), nullptr,
                             nullptr)) {
        errno = AndroidNameTooLong;
        return nullptr;
    }
    return &entry;
}

AndroidDirent* ReadDir(AndroidDir* dir)
{
    const std::lock_guard lock(dir->mutex);
    return ReadDirEntry(dir);
}

int ReadDirReentrant(AndroidDir* dir, AndroidDirent* entry, AndroidDirent** result)
{
    const std::lock_guard lock(dir->mutex);
    const auto saved = errno;
    errno = 0;
    const auto* next = ReadDirEntry(dir);
    const auto error = next ? 0 : errno;
    errno = saved;
    *result = nullptr;
    if (next) {
        *entry = *next;
        *result = entry;
    }
    return error;
}

int CloseDir(AndroidDir* dir)
{
    const int result = dir->handle == -1 ? 0 : _findclose(dir->handle);
    const int error = errno;
    const int closed = CloseTaggedDescriptor(dir->fd, MakeFdOwnerTag(DirectoryOwner, reinterpret_cast<uintptr_t>(dir)));
    delete dir;
    if (result != 0) {
        errno = error;
        return result;
    }
    return closed;
}

enum WalkEntry {
    WalkFile,
    WalkDirectory,
    WalkUnreadable,
    WalkDirectoryEnd,
    WalkUnknown,
    WalkLink,
    WalkDanglingLink
};

enum WalkFlag {
    WalkPhysical = 1,
    WalkSameVolume = 2,
    WalkDepthFirst = 4,
    WalkChangeDirectory = 8
};

struct WalkPosition {
    int base, level;
};

int WalkFileError(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return EACCES;
    case ERROR_DIRECTORY:
        return ENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE:
        return AndroidNameTooLong;
    case ERROR_CANT_RESOLVE_FILENAME:
        return 40; // Bionic ELOOP
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    default:
        return EIO;
    }
}

AndroidTimespec WalkTime(FILETIME time)
{
    constexpr int64_t UnixEpoch = 116444736000000000LL, TicksPerSecond = 10'000'000;
    const auto ticks = static_cast<int64_t>((uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime) - UnixEpoch;
    const auto remainder = (ticks % TicksPerSecond + TicksPerSecond) % TicksPerSecond;
    return {(ticks - remainder) / TicksPerSecond, remainder * 100};
}

struct WalkStat {
    AndroidStat stat{};
    AndroidTimespec created{};
    bool link = false;
};

bool ReadWalkStat(HANDLE file, bool follow, WalkStat& result)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) {
        errno = WalkFileError(GetLastError());
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    result.link = !follow && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                  GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                  (tag.ReparseTag == IO_REPARSE_TAG_SYMLINK || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT ||
                   tag.ReparseTag == AndroidSymlinkTag);
    auto& stat = result.stat;
    stat.device = info.dwVolumeSerialNumber;
    stat.inode = (uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow;
    stat.links = info.nNumberOfLinks;
    const bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    stat.mode = result.link ? 0120777 : (directory ? 0040555 : 0100444);
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
        stat.mode |= 0222;
    stat.size = static_cast<int64_t>((uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow);
    if (result.link && tag.ReparseTag == AndroidSymlinkTag)
        stat.size = ReadAndroidSymbolicLink(reinterpret_cast<uintptr_t>(file))->size();
    stat.blockSize = 4096;
    stat.blocks = stat.size / 512 + (stat.size % 512 != 0);
    stat.accessed = WalkTime(info.ftLastAccessTime);
    stat.modified = stat.changed = WalkTime(info.ftLastWriteTime);
    result.created = WalkTime(info.ftCreationTime);
    return true;
}

bool StatWalkEntry(const std::filesystem::path& path, bool follow, WalkStat& result)
{
    const auto resolved = ResolveAndroidSymbolicLinks(path, false, follow, &MapGuestPath);
    const auto file = CreateFileW(resolved.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        errno = WalkFileError(GetLastError());
        return false;
    }
    const auto found = ReadWalkStat(file, follow, result);
    CloseHandle(file);
    return found;
}

struct StatxTimestamp {
    int64_t seconds;
    uint32_t nanos, reserved;
};

struct AndroidStatx {
    uint32_t mask, blockSize;
    uint64_t attributes;
    uint32_t links, uid, gid;
    uint16_t mode, reserved;
    uint64_t inode, size, blocks, attributesMask;
    StatxTimestamp accessed, created, changed, modified;
    uint32_t specialMajor, specialMinor, deviceMajor, deviceMinor;
    uint64_t extended[14];
};

static_assert(sizeof(AndroidStatx) == 256 && offsetof(AndroidStatx, accessed) == 64);

int Statx(int fd, const char* path, int flags, uint32_t mask, AndroidStatx* output)
{
    constexpr int NoFollow = 0x100, NoAutomount = 0x800, EmptyPath = 0x1000, SyncType = 0x6000;
    constexpr int CurrentDirectory = -100;
    if ((flags & ~(NoFollow | NoAutomount | EmptyPath | SyncType)) || (flags & SyncType) == SyncType ||
        (mask & 0x80000000u)) {
        errno = EINVAL;
        return -1;
    }
    if (!output || (!path && !(flags & EmptyPath))) {
        errno = EFAULT;
        return -1;
    }
    WalkStat entry;
    if (!path || !*path) {
        if (!(flags & EmptyPath)) {
            errno = ENOENT;
            return -1;
        }
        if (fd == CurrentDirectory) {
            if (!StatWalkEntry(L".", true, entry))
                return -1;
        }
        else {
            const auto file = FileDescriptorHandle(fd);
            if (file == INVALID_HANDLE_VALUE || !ReadWalkStat(file, false, entry))
                return -1;
        }
    }
    else {
        if (fd != CurrentDirectory && !GuestAbsolutePath(path)) {
            const auto file = FileDescriptorHandle(fd);
            if (file == INVALID_HANDLE_VALUE)
                return -1;
            FILE_STANDARD_INFO info{};
            if (!GetFileInformationByHandleEx(file, FileStandardInfo, &info, sizeof(info)) || !info.Directory) {
                errno = ENOTDIR;
                return -1;
            }
        }
        if (WithPathAt(
                fd, path,
                [&](const wchar_t* resolved) {
                    return StatWalkEntry(resolved, !(flags & NoFollow), entry) ? 0 : -1;
                },
                !(flags & NoFollow)) < 0)
            return -1;
    }

    const auto copyTime = [](const AndroidTimespec& time) {
        return StatxTimestamp{time.seconds, static_cast<uint32_t>(time.nanos), 0};
    };
    const auto devMajor = [](uint64_t device) {
        return static_cast<uint32_t>((device >> 8 & 0xfff) | (device >> 32 & ~0xfffu));
    };
    const auto devMinor = [](uint64_t device) {
        return static_cast<uint32_t>((device & 0xff) | (device >> 12 & ~0xffu));
    };
    const auto& stat = entry.stat;
    *output = {};
    output->mask = 0xfff; // Basic stat fields and birth time.
    output->blockSize = static_cast<uint32_t>(stat.blockSize);
    output->links = static_cast<uint32_t>(stat.links);
    output->uid = stat.uid;
    output->gid = stat.gid;
    output->mode = static_cast<uint16_t>(stat.mode);
    output->inode = stat.inode;
    output->size = stat.size;
    output->blocks = stat.blocks;
    output->accessed = copyTime(stat.accessed);
    output->created = copyTime(entry.created);
    output->changed = copyTime(stat.changed);
    output->modified = copyTime(stat.modified);
    output->specialMajor = devMajor(stat.special);
    output->specialMinor = devMinor(stat.special);
    output->deviceMajor = devMajor(stat.device);
    output->deviceMinor = devMinor(stat.device);
    return 0;
}

// Android binary layout of FTSENT on LP64. The file name is stored inline.
struct FtsEntry {
    FtsEntry *cycle, *parent, *link;
    int64_t number;
    void* cookie;
    char *accessPath, *path;
    int error, symlinkFd;
    size_t pathLength, nameLength;
    uint64_t inode, device;
    uint32_t links;
    int16_t level;
    uint16_t info, flags, instruction;
    AndroidStat* stat;
    char name[1];
};

static_assert(offsetof(FtsEntry, info) == 102 && offsetof(FtsEntry, stat) == 112 && offsetof(FtsEntry, name) == 120);

enum FtsOption {
    FtsRootFollow = 1,
    FtsLogical = 2,
    FtsNoChdir = 4,
    FtsNoStat = 8,
    FtsPhysical = 16,
    FtsSeedDot = 32,
    FtsXdev = 64
};

enum FtsInfo : uint16_t {
    FtsDirectory = 1,
    FtsCycle,
    FtsDefault,
    FtsUnreadable,
    FtsDot,
    FtsPostorder,
    FtsError,
    FtsRegular,
    FtsInitial,
    FtsMissing,
    FtsNoDetails,
    FtsSymlink,
    FtsDangling
};

enum FtsInstruction {
    FtsAgain = 1,
    FtsFollow,
    FtsNoInstruction,
    FtsSkip
};

struct FtsNode {
    FtsNode(std::filesystem::path hostPath, std::string path, std::string_view name)
        : host(std::move(hostPath)), guest(std::move(path)),
          entry(static_cast<FtsEntry*>(std::calloc(1, sizeof(FtsEntry) + name.size())), &std::free)
    {
        if (!entry)
            throw std::bad_alloc();
        auto* inlineName = reinterpret_cast<char*>(entry.get()) + offsetof(FtsEntry, name);
        std::memcpy(inlineName, name.data(), name.size());
        entry->nameLength = name.size();
        entry->path = entry->accessPath = guest.data();
        entry->pathLength = guest.size();
        entry->instruction = FtsNoInstruction;
        entry->symlinkFd = -1;
    }

    std::filesystem::path host;
    std::string guest;
    AndroidStat stat{};
    std::unique_ptr<FtsEntry, decltype(&std::free)> entry;
};

struct FtsLevel {
    std::vector<std::unique_ptr<FtsNode>> nodes;
    size_t next = 0;
    FtsNode* directory = nullptr;
};

struct FtsCursor {
    int options = 0;
    uintptr_t compare = 0;
    bool failed = false;
    uint64_t rootDevice = 0;
    FtsEntry rootParent{};
    FtsNode* current = nullptr;
    std::vector<FtsLevel> levels;
};

void RefreshFtsEntry(FtsCursor& cursor, FtsNode& node, bool follow)
{
    auto& entry = *node.entry;
    auto& stat = node.stat;
    entry.error = 0;
    entry.cycle = nullptr;
    entry.inode = entry.device = entry.links = 0;
    stat = {};
    entry.stat = (cursor.options & FtsNoStat) ? nullptr : &stat;
    WalkStat found;
    bool dangling = false;
    if (!StatWalkEntry(node.host, false, found)) {
        entry.info = FtsMissing;
        entry.error = errno;
        return;
    }
    if (found.link && follow) {
        WalkStat target;
        if (StatWalkEntry(node.host, true, target)) {
            found = target;
        }
        else {
            dangling = true;
        }
    }
    stat = found.stat;
    entry.inode = stat.inode;
    entry.device = stat.device;
    entry.links = static_cast<uint32_t>(stat.links);
    if (found.link) {
        entry.info = dangling ? FtsDangling : FtsSymlink;
        return;
    }
    if ((stat.mode & 0170000) == 0040000) {
        entry.info = FtsDirectory;
        const std::string_view name(entry.name, entry.nameLength);
        if (entry.level > 0 && (name == "." || name == "..")) {
            entry.info = FtsDot;
            return;
        }
        for (auto* parent = entry.parent; parent && parent->level >= 0; parent = parent->parent) {
            if (parent->device == entry.device && parent->inode == entry.inode) {
                entry.cycle = parent;
                entry.info = FtsCycle;
                break;
            }
        }
        return;
    }
    entry.info = (cursor.options & FtsNoStat)       ? FtsNoDetails
                 : (stat.mode & 0170000) == 0100000 ? FtsRegular
                                                    : FtsDefault;
}

void AddFtsNode(FtsCursor& cursor, FtsLevel& level, std::filesystem::path host, std::string guest,
                std::string_view name)
{
    auto node = std::make_unique<FtsNode>(std::move(host), std::move(guest), name);
    auto& entry = *node->entry;
    entry.parent = level.directory ? level.directory->entry.get() : &cursor.rootParent;
    entry.level = static_cast<int16_t>(entry.parent->level + 1);
    RefreshFtsEntry(cursor, *node, (cursor.options & FtsLogical) || (!entry.level && (cursor.options & FtsRootFollow)));
    level.nodes.push_back(std::move(node));
}

void OrderFtsNodes(FtsCursor& cursor, FtsLevel& level)
{
    if (cursor.compare) {
        std::stable_sort(level.nodes.begin(), level.nodes.end(), [&cursor](const auto& left, const auto& right) {
            const auto* a = left->entry.get();
            const auto* b = right->entry.get();
            return static_cast<int>(CallAndroidCallback(cursor.compare, &a, &b)) < 0;
        });
    }
    for (size_t i = 1; i < level.nodes.size(); ++i)
        level.nodes[i - 1]->entry->link = level.nodes[i]->entry.get();
}

int FtsErrorCode(const std::filesystem::filesystem_error& error)
{
    return error.code().category() == std::system_category() ? WalkFileError(static_cast<DWORD>(error.code().value()))
                                                             : EINVAL;
}

FtsCursor* OpenFts(const char* const* paths, int options, uintptr_t compare)
{
    if (!paths || (options & ~0x7f) || ((options & FtsPhysical) && (options & FtsLogical))) {
        errno = EINVAL;
        return nullptr;
    }
    try {
        auto cursor = std::make_unique<FtsCursor>();
        // Use complete access paths. The Windows current directory is shared by all guest threads.
        cursor->options = options | FtsNoChdir;
        cursor->compare = compare;
        cursor->rootParent.level = -1;
        auto& roots = cursor->levels.emplace_back();
        for (auto path = paths; *path; ++path) {
            std::string guest(*path);
            if (guest.empty()) {
                errno = ENOENT;
                return nullptr;
            }
            auto host = std::filesystem::absolute(GuestPath(*path, false, false));
            while (guest.size() > 1 && (guest.back() == '/' || guest.back() == '\\') && !host.has_filename() &&
                   host.has_relative_path()) {
                host = host.parent_path();
                guest.pop_back();
            }
            const auto separator = guest.find_last_of("/\\");
            const auto name = separator == guest.npos || separator + 1 == guest.size()
                                  ? std::string_view(guest)
                                  : std::string_view(guest).substr(separator + 1);
            AddFtsNode(*cursor, roots, std::move(host), guest, name);
        }
        OrderFtsNodes(*cursor, roots);
        errno = 0;
        return cursor.release();
    }
    catch (const AndroidSystemPathError& error) {
        errno = error.code;
    }
    catch (const std::filesystem::filesystem_error& error) {
        errno = FtsErrorCode(error);
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
    }
    return nullptr;
}

FtsEntry* ReadFts(FtsCursor* cursor)
{
    if (!cursor) {
        errno = EINVAL;
        return nullptr;
    }
    errno = 0;
    if (cursor->failed || cursor->levels.empty())
        return nullptr;
    try {
        if (auto* current = cursor->current) {
            auto& entry = *current->entry;
            const auto instruction = entry.instruction;
            entry.instruction = FtsNoInstruction;
            if (instruction == FtsAgain ||
                (instruction == FtsFollow && (entry.info == FtsSymlink || entry.info == FtsDangling))) {
                RefreshFtsEntry(*cursor, *current,
                                instruction == FtsFollow || (cursor->options & FtsLogical) ||
                                    (!entry.level && (cursor->options & FtsRootFollow)));
                if (!entry.level)
                    cursor->rootDevice = entry.device;
                return &entry;
            }
            if (entry.info == FtsDirectory) {
                if (instruction == FtsSkip || ((cursor->options & FtsXdev) && entry.device != cursor->rootDevice)) {
                    entry.info = FtsPostorder;
                    return &entry;
                }
                if (entry.level == INT16_MAX) {
                    entry.info = FtsError;
                    entry.error = AndroidNameTooLong;
                    return &entry;
                }
                FtsLevel children;
                children.directory = current;
                std::error_code error;
                std::filesystem::directory_iterator item(
                    ResolveAndroidSymbolicLinks(current->host, false, true, &MapGuestPath), error),
                    end;
                if (error) {
                    entry.info = FtsUnreadable;
                    entry.error = WalkFileError(static_cast<DWORD>(error.value()));
                    return &entry;
                }
                const auto addChild = [&](const std::filesystem::path& filename) {
                    const auto utf8 = filename.u8string();
                    const std::string_view name(reinterpret_cast<const char*>(utf8.data()), utf8.size());
                    std::string guest = current->guest;
                    if (guest.back() != '/' && guest.back() != '\\')
                        guest.push_back('/');
                    guest.append(name);
                    AddFtsNode(*cursor, children, current->host / filename, std::move(guest), name);
                };
                if (cursor->options & FtsSeedDot) {
                    addChild(L".");
                    addChild(L"..");
                }
                for (; item != end; item.increment(error))
                    addChild(item->path().filename());
                if (error) {
                    entry.info = FtsError;
                    entry.error = WalkFileError(static_cast<DWORD>(error.value()));
                    return &entry;
                }
                OrderFtsNodes(*cursor, children);
                cursor->levels.push_back(std::move(children));
            }
        }
        auto& level = cursor->levels.back();
        if (level.next == level.nodes.size()) {
            cursor->current = level.directory;
            cursor->levels.pop_back();
            if (!cursor->current)
                return nullptr;
            cursor->current->entry->info = FtsPostorder;
        }
        else {
            cursor->current = level.nodes[level.next++].get();
            if (cursor->current->entry->level == 0)
                cursor->rootDevice = cursor->current->entry->device;
        }
        errno = 0;
        return cursor->current->entry.get();
    }
    catch (const std::filesystem::filesystem_error& error) {
        errno = FtsErrorCode(error);
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
    }
    cursor->failed = true;
    return nullptr;
}

int SetFtsInstruction(FtsCursor* cursor, FtsEntry* entry, int instruction)
{
    if (!cursor || !entry || instruction < 0 || instruction > FtsSkip) {
        errno = EINVAL;
        return -1;
    }
    entry->instruction = static_cast<uint16_t>(instruction);
    return 0;
}

int CloseFts(FtsCursor* cursor)
{
    delete cursor;
    return 0;
}

// Windows files exposed to the Android guest have no Linux xattr namespace.
template <bool Follow, bool Set, bool List = false, typename Path>
intptr_t FileXattr(Path path, const char* name, const void* value, size_t size, int flags = 0)
{
    if (!name || (size && !value)) {
        errno = EFAULT;
        return -1;
    }
    if (Set && (flags < 0 || flags > 2)) {
        errno = EINVAL;
        return -1;
    }
    if constexpr (std::is_same_v<Path, int>) {
        if (FileDescriptorHandle(path) == INVALID_HANDLE_VALUE)
            return -1;
        AndroidStat info;
        if (StatFile(path, &info) < 0)
            return -1;
    }
    else {
        if (!path) {
            errno = EFAULT;
            return -1;
        }
        if (!*path) {
            errno = ENOENT;
            return -1;
        }
        try {
            WalkStat info;
            if (!StatWalkEntry(GuestPath(path, Set, Follow), Follow, info))
                return -1;
        }
        catch (const AndroidSystemPathError& error) {
            errno = error.code;
            return -1;
        }
        catch (const std::filesystem::filesystem_error& error) {
            errno = FtsErrorCode(error);
            return -1;
        }
        catch (const std::bad_alloc&) {
            errno = ENOMEM;
            return -1;
        }
    }
    if constexpr (!Set) {
        // There are no Linux attributes or SELinux labels on these mounts.
        // Listing the empty namespace succeeds; a named lookup has no data.
        if constexpr (List)
            return 0;
        errno = 61; // Bionic ENODATA
    }
    else
        errno = 95; // Bionic ENOTSUP: attribute writes are unavailable.
    return -1;
}

struct WalkCurrentDirectory {
    std::filesystem::path original;

    ~WalkCurrentDirectory()
    {
        if (original.empty())
            return;
        const int saved = errno;
        _wchdir(original.c_str());
        errno = saved;
    }
};

int WalkTree(const char* path, uintptr_t callback, int maxDescriptors, int flags)
{
    if (!path || !callback || maxDescriptors < 1) {
        errno = EINVAL;
        return -1;
    }
    if (!*path) {
        errno = ENOENT;
        return -1;
    }
    const bool physical = flags & WalkPhysical;
    const bool postorder = flags & WalkDepthFirst;
    // Android's FTS_LOGICAL implies FTS_NOCHDIR.
    const bool changeDirectory = physical && (flags & WalkChangeDirectory);
    static std::recursive_mutex directoryMutex;
    std::unique_lock directoryLock(directoryMutex, std::defer_lock);
    if (changeDirectory)
        directoryLock.lock();
    try {
        WalkCurrentDirectory cwd{changeDirectory ? std::filesystem::current_path() : std::filesystem::path{}};
        auto host = std::filesystem::absolute(GuestPath(path, false, false));
        std::string guest(path);
        while (guest.size() > 1 && guest.back() == '/' && !host.has_filename() && host.has_relative_path()) {
            guest.pop_back();
            host = host.parent_path();
        }
        if (guest.size() >= 4096) {
            errno = 36;
            return -1;
        }

        struct Directory {
            std::filesystem::path host;
            std::string guest;
            WalkStat entry;
            std::vector<std::filesystem::path> children;
            size_t next = 0;
        };

        std::vector<Directory> parents;
        const auto notify = [&](const std::filesystem::path& item, const std::string& name, AndroidStat& stat,
                                WalkEntry type) {
            if (changeDirectory && !parents.empty() && _wchdir(item.parent_path().c_str()) != 0)
                return -1;
            const auto last = name.find_last_of("/\\");
            WalkPosition where{static_cast<int>(last == name.npos || name.size() == 1 ? 0 : last + 1),
                               static_cast<int>(parents.size())};
            return static_cast<int>(CallAndroidCallback(callback, const_cast<char*>(name.c_str()), &stat,
                                                        reinterpret_cast<void*>(static_cast<uintptr_t>(type)),
                                                        reinterpret_cast<uintptr_t>(&where)));
        };
        const auto visit = [&](std::filesystem::path item, std::string name) {
            if (name.size() >= 4096) {
                errno = 36;
                return -1;
            }
            WalkStat entry;
            if (!StatWalkEntry(item, false, entry))
                return notify(item, name, entry.stat, WalkUnknown);
            if (entry.link) {
                if (physical && !parents.empty())
                    return notify(item, name, entry.stat, WalkLink);
                WalkStat target;
                if (!StatWalkEntry(item, true, target)) {
                    if (errno == 40)
                        return -1;
                    return notify(item, name, entry.stat, WalkDanglingLink);
                }
                entry = target;
            }
            if ((entry.stat.mode & 0170000) != 0040000)
                return notify(item, name, entry.stat, WalkFile);
            if (std::ranges::any_of(parents, [&](const Directory& parent) {
                    return parent.entry.stat.device == entry.stat.device && parent.entry.stat.inode == entry.stat.inode;
                })) {
                errno = 40;
                return -1;
            }
            if ((flags & WalkSameVolume) && !parents.empty() && parents.front().entry.stat.device != entry.stat.device)
                return notify(item, name, entry.stat, postorder ? WalkDirectoryEnd : WalkDirectory);

            std::error_code error;
            std::filesystem::directory_iterator child(ResolveAndroidSymbolicLinks(item, false, true, &MapGuestPath),
                                                      error),
                end;
            if (error) {
                errno = WalkFileError(static_cast<DWORD>(error.value()));
                return notify(item, name, entry.stat, WalkUnreadable);
            }
            // Read names before descending to keep at most one enumeration handle open.
            std::vector<std::filesystem::path> children;
            for (; child != end; child.increment(error)) {
                children.push_back(child->path().filename());
            }
            if (error) {
                errno = WalkFileError(static_cast<DWORD>(error.value()));
                return -1;
            }
            if (!postorder) {
                if (const int result = notify(item, name, entry.stat, WalkDirectory))
                    return result;
            }
            parents.push_back({std::move(item), std::move(name), entry, std::move(children)});
            return 0;
        };
        if (const int result = visit(std::move(host), std::move(guest)))
            return result;
        while (!parents.empty()) {
            auto& parent = parents.back();
            if (parent.next < parent.children.size()) {
                const auto& child = parent.children[parent.next++];
                auto name = parent.guest;
                if (name.back() != '/' && name.back() != '\\')
                    name.push_back('/');
                const auto utf8 = child.u8string();
                name.append(reinterpret_cast<const char*>(utf8.data()), utf8.size());
                if (const int result = visit(parent.host / child, std::move(name)))
                    return result;
                continue;
            }
            if (postorder) {
                auto finished = std::move(parent);
                parents.pop_back();
                if (const int result = notify(finished.host, finished.guest, finished.entry.stat, WalkDirectoryEnd))
                    return result;
            }
            else {
                parents.pop_back();
            }
        }
        return 0;
    }
    catch (const AndroidSystemPathError& error) {
        errno = error.code;
        return -1;
    }
    catch (const std::filesystem::filesystem_error& error) {
        errno = error.code().category() == std::system_category()
                    ? WalkFileError(static_cast<DWORD>(error.code().value()))
                    : EINVAL;
        return -1;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

int ScanDirectory(const char* path, AndroidDirent*** result, uintptr_t filter, uintptr_t compare)
{
    if (!result) {
        errno = EFAULT;
        return -1;
    }
    const auto saved = errno;
    const std::unique_ptr<AndroidDir, decltype(&CloseDir)> dir(OpenDir(path), &CloseDir);
    if (!dir)
        return -1;
    try {
        std::vector<AndroidDirent> entries;
        for (;;) {
            errno = 0;
            auto* entry = ReadDir(dir.get());
            if (!entry) {
                if (errno)
                    return -1;
                break;
            }
            if (!filter || static_cast<int>(CallAndroidCallback(filter, entry)))
                entries.push_back(*entry);
        }
        if (compare) {
            std::sort(entries.begin(), entries.end(), [compare](const AndroidDirent& left, const AndroidDirent& right) {
                auto* first = &left;
                auto* second = &right;
                return static_cast<int>(CallAndroidCallback(compare, &first, &second)) < 0;
            });
        }
        if (entries.empty()) {
            *result = nullptr;
            errno = saved;
            return 0;
        }
        if (entries.size() > INT_MAX) {
            errno = 75;
            return -1;
        } // Bionic EOVERFLOW
        auto** list = static_cast<AndroidDirent**>(std::malloc(entries.size() * sizeof(AndroidDirent*)));
        if (!list) {
            errno = ENOMEM;
            return -1;
        }
        for (size_t i = 0; i < entries.size(); ++i) {
            auto* item = static_cast<AndroidDirent*>(std::malloc(sizeof(AndroidDirent)));
            if (!item) {
                while (i)
                    std::free(list[--i]);
                std::free(list);
                errno = ENOMEM;
                return -1;
            }
            *item = entries[i];
            list[i] = item;
        }
        *result = list;
        errno = saved;
        return static_cast<int>(entries.size());
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

template <bool Local> AndroidTm* BrokenDownTime(const time_t* seconds, AndroidTm* result)
{
    const auto error = Local ? localtime_s(&result->fields, seconds) : gmtime_s(&result->fields, seconds);
    if (error) {
        errno = error;
        return nullptr;
    }
    long zone = 0, dst = 0;
    if (Local) {
        _get_timezone(&zone);
        if (result->fields.tm_isdst > 0)
            _get_dstbias(&dst);
    }
    result->offset = -static_cast<int64_t>(zone) - dst;
    result->zone = Local ? nullptr : "UTC";
    return result;
}

int64_t TimeInUtc(AndroidTm* value)
{
    using namespace std::chrono;
    constexpr int64_t CycleYears = 400, CycleDays = 146097;
    constexpr auto anchor = sys_days{year{2000} / January / 1};
    const auto floorQuotient = [](int64_t number, int64_t divisor) {
        return number / divisor - (number % divisor < 0);
    };
    auto& fields = value->fields;
    const int64_t years = static_cast<int64_t>(fields.tm_year) + 1900 + floorQuotient(fields.tm_mon, 12);
    const auto months = static_cast<unsigned>((fields.tm_mon % 12 + 12) % 12 + 1);
    const auto cycles = floorQuotient(years - 2000, CycleYears);
    const auto foldedYear = static_cast<int>(years - cycles * CycleYears);
    const auto base = sys_days{year{foldedYear} / month{months} / 1}.time_since_epoch().count() + cycles * CycleDays;
    const auto time = (base + fields.tm_mday - 1) * 86400 + hours(fields.tm_hour) / seconds(1) +
                      minutes(fields.tm_min) / seconds(1) + fields.tm_sec;
    const auto date = floorQuotient(time, 86400);
    const auto outputCycles = floorQuotient(date - anchor.time_since_epoch().count(), CycleDays);
    const auto foldedDate = sys_days{days(date - outputCycles * CycleDays)};
    const year_month_day calendar{foldedDate};
    const auto normalizedYear = static_cast<int>(calendar.year()) + outputCycles * CycleYears - 1900;
    if (normalizedYear < INT_MIN || normalizedYear > INT_MAX) {
        errno = 75;
        return -1;
    }
    const hh_mm_ss clock{seconds(time - date * 86400)};
    fields = {static_cast<int>(clock.seconds().count()),
              static_cast<int>(clock.minutes().count()),
              static_cast<int>(clock.hours().count()),
              static_cast<int>(static_cast<unsigned>(calendar.day())),
              static_cast<int>(static_cast<unsigned>(calendar.month())) - 1,
              static_cast<int>(normalizedYear),
              static_cast<int>(weekday{foldedDate}.c_encoding()),
              static_cast<int>((foldedDate - sys_days{calendar.year() / January / 1}).count()),
              0};
    value->offset = 0;
    value->zone = "UTC";
    return time;
}

struct TimeZone {
    int64_t offset = 0;
    int daylight = 0;
    char standard[128]{}, daylightName[128]{};
    const char* names[2]{standard, daylightName};

    TimeZone()
    {
        refresh();
    }

    void refresh()
    {
        _tzset();
        long seconds = 0;
        size_t length = 0;
        _get_timezone(&seconds);
        offset = seconds;
        _get_daylight(&daylight);
        _get_tzname(&length, standard, sizeof(standard), 0);
        _get_tzname(&length, daylightName, sizeof(daylightName), 1);
    }
};

TimeZone& LocalTimeZone()
{
    static TimeZone zone;
    return zone;
}

int64_t LocalTimeInEpoch(AndroidTm* value)
{
    const auto seconds = std::mktime(&value->fields);
    if (seconds == -1) {
        errno = 75; // EOVERFLOW
        return -1;
    }
    long offset = 0, dst = 0;
    _get_timezone(&offset);
    const bool daylight = value->fields.tm_isdst > 0;
    if (daylight)
        _get_dstbias(&dst);
    value->offset = -static_cast<int64_t>(offset) - dst;
    value->zone = LocalTimeZone().names[daylight];
    return seconds;
}

int TimeSpecGet(AndroidTimespec* time, int base)
{
    constexpr int Utc = 1;
    return base == Utc && AndroidClock(0, time) == 0 ? Utc : 0;
}

int TimeOfDay(AndroidTimespec* result, void* zone)
{
    if (result) {
        AndroidClock(0, result);
        result->nanos /= 1000;
    }
    if (zone)
        std::memset(zone, 0, 2 * sizeof(int));
    return 0;
}

int NanoSleep(const AndroidTimespec* request, AndroidTimespec*)
{
    if (request->seconds < 0 || request->nanos < 0 || request->nanos >= 1'000'000'000) {
        errno = EINVAL;
        return -1;
    }
    for (auto seconds = request->seconds; seconds > 0;) {
        const auto chunk = std::min<int64_t>(seconds, 86400);
        Sleep(static_cast<DWORD>(chunk * 1000));
        seconds -= chunk;
    }
    if (request->nanos)
        Sleep(static_cast<DWORD>((request->nanos + 999999) / 1000000));
    return 0;
}

int ClockNanoSleep(int clock, int flags, const AndroidTimespec* request, AndroidTimespec*)
{
    constexpr int TimerAbsolute = 1;
    if (clock != 0 && clock != 1 && clock != 7)
        return EINVAL;
    if ((flags & ~TimerAbsolute) || request->seconds < 0 || request->nanos < 0 || request->nanos >= 1'000'000'000)
        return EINVAL;
    if (!flags)
        return NanoSleep(request, nullptr) == 0 ? 0 : errno;

    const auto deadline = *request;
    for (;;) {
        AndroidTimespec now{};
        if (AndroidClock(clock, &now))
            return errno;
        if (deadline.seconds < now.seconds || (deadline.seconds == now.seconds && deadline.nanos <= now.nanos))
            return 0;
        // Recheck the clock each second so an absolute wait follows realtime adjustments.
        AndroidTimespec remaining{1, 0};
        if (now.seconds >= deadline.seconds - 1) {
            const auto nanos = (deadline.seconds - now.seconds) * 1'000'000'000 + deadline.nanos - now.nanos;
            if (nanos < 1'000'000'000)
                remaining = {0, nanos};
        }
        if (NanoSleep(&remaining, nullptr))
            return errno;
    }
}

unsigned int SleepSeconds(unsigned int seconds)
{
    const AndroidTimespec request{seconds, 0};
    NanoSleep(&request, nullptr);
    return 0;
}

int SleepMicroseconds(unsigned int micros)
{
    const AndroidTimespec request{micros / 1'000'000, (micros % 1'000'000) * 1'000};
    return NanoSleep(&request, nullptr);
}

int64_t Sysconf(int key)
{
    switch (key) {
    case 6:
        return 100; // _SC_CLK_TCK: guest /proc CPU ticks use 100 Hz.
    case 0x27:
    case 0x28: {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        return info.dwPageSize;
    }
    case 0x60:
    case 0x61:
        return GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    default:
        errno = EINVAL;
        return -1;
    }
}

uint64_t GetAuxValue(uint64_t type)
{
    constexpr uint64_t PageSize = 6, Platform = 15, HardwareCapabilities = 16, HardwareCapabilities2 = 26;
    switch (type) {
    case PageSize:
        return static_cast<uint64_t>(Sysconf(0x27));
    case Platform:
        return reinterpret_cast<uintptr_t>("x86_64");
    case HardwareCapabilities: {
        int cpu[4];
        __cpuid(cpu, 1);
        return static_cast<uint32_t>(cpu[3]);
    }
    case HardwareCapabilities2:
        // Linux's HWCAP2_FSGSBASE describes instructions enabled for user mode by the host.
        return IsProcessorFeaturePresent(PF_RDWRFSGSBASE_AVAILABLE) ? 2 : 0;
    default:
        errno = ENOENT;
        return 0;
    }
}

size_t StringCopy(char* dst, const char* src, size_t size)
{
    const auto length = std::strlen(src);
    if (size) {
        const auto copied = std::min(length, size - 1);
        std::memcpy(dst, src, copied);
        dst[copied] = 0;
    }
    return length;
}

size_t StringConcat(char* dst, const char* src, size_t size)
{
    const auto length = strnlen(dst, size);
    return length == size ? length + std::strlen(src) : length + StringCopy(dst + length, src, size - length);
}

uint32_t* WideCopy(uint32_t* dst, const uint32_t* src, size_t size)
{
    size_t i = 0;
    for (; i < size && src[i]; ++i)
        dst[i] = src[i];
    std::fill(dst + i, dst + size, 0);
    return dst;
}

// Bionic classifies UTF-32 code points using Unicode properties. Its _l
// variants use the same rules regardless of the locale argument.
template <UProperty property> int WideProperty(uint32_t ch)
{
    return ch <= 0x10ffff && u_hasBinaryProperty(static_cast<UChar32>(ch), property);
}

template <UCharCategory category> int WideCategory(uint32_t ch)
{
    return ch <= 0x10ffff && u_charType(static_cast<UChar32>(ch)) == category;
}

int WidePunctuation(uint32_t ch)
{
    if (ch < 0x80)
        return std::ispunct(static_cast<int>(ch));
    if (ch > 0x10ffff)
        return 0;
    const auto category = u_charType(static_cast<UChar32>(ch));
    return category >= U_DASH_PUNCTUATION && category <= U_OTHER_PUNCTUATION;
}

template <auto convert> uint32_t WideCase(uint32_t ch)
{
    return ch <= 0x10ffff ? static_cast<uint32_t>(convert(static_cast<UChar32>(ch))) : ch;
}

_locale_t NumberLocale()
{
    static const auto locale = _create_locale(LC_ALL, "C");
    return locale;
}

// Bionic's C and C.UTF-8 locales share the numeric and collation rules.
struct AndroidLconv {
    char* decimalPoint;
    std::array<char*, 9> otherStrings;
    std::array<char, 14> monetaryFields;
};

static_assert(sizeof(AndroidLconv) == 96);

AndroidLconv* AndroidLocaleConv()
{
    static char decimal[] = ".";
    static char empty[] = "";
    static auto locale = [] {
        AndroidLconv info{};
        info.decimalPoint = decimal;
        info.otherStrings.fill(empty);
        info.monetaryFields.fill(CHAR_MAX);
        return info;
    }();
    return &locale;
}

int WideCompare(const uint32_t* first, const uint32_t* second)
{
    while (*first && *first == *second) {
        ++first;
        ++second;
    }
    return (*first > *second) - (*first < *second);
}

size_t WideTransform(uint32_t* destination, const uint32_t* source, size_t size)
{
    const auto* end = source;
    while (*end)
        ++end;
    const auto length = static_cast<size_t>(end - source);
    if (size)
        std::copy_n(source, std::min(size, length + 1), destination);
    return length;
}

template <typename Parse> auto ParseWideNumber(const uint32_t* source, uint32_t** end, Parse parse)
{
    using Result = decltype(parse(static_cast<const char*>(nullptr), static_cast<char**>(nullptr)));
    if (end)
        *end = const_cast<uint32_t*>(source);
    try {
        const auto* first = source;
        while (WideProperty<UCHAR_WHITE_SPACE>(*first))
            ++first;
        std::string digits;
        for (auto* ch = first; *ch && *ch < 0x80; ++ch)
            digits.push_back(static_cast<char>(*ch));
        char* parsed;
        const auto result = parse(digits.c_str(), &parsed);
        if (end && parsed != digits.c_str())
            *end = const_cast<uint32_t*>(first + (parsed - digits.c_str()));
        return result;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return Result{};
    }
    catch (const std::length_error&) {
        errno = ENOMEM;
        return Result{};
    }
}

template <auto parse> auto WideFloat(const uint32_t* source, uint32_t** end)
{
    return ParseWideNumber(source, end, [](const char* digits, char** parsed) {
        return parse(digits, parsed, NumberLocale());
    });
}

template <auto parse> auto WideInteger(const uint32_t* source, uint32_t** end, int base)
{
    using Result = decltype(parse(nullptr, nullptr, base, nullptr));
    if (base != 0 && (base < 2 || base > 36)) {
        if (end)
            *end = const_cast<uint32_t*>(source);
        errno = EINVAL;
        return Result{};
    }
    return ParseWideNumber(source, end, [base](const char* digits, char** parsed) {
        return parse(digits, parsed, base, NumberLocale());
    });
}

_LDOUBLE ParseLongDouble(const char* source, char** end)
{
    static_assert(sizeof(_LDOUBLE) == 10);
    constexpr uint64_t IntegerBit = uint64_t{1} << 63;
    constexpr uint64_t QuietBit = uint64_t{1} << 62;
    constexpr uint16_t Sign = uint16_t{1} << 15, InfiniteExponent = 0x7fff;
    const auto originalErrno = errno;
    char* parsed;
    // The double parser supplies the end pointer only. Its range and rounded
    // result must not determine the extended-precision result or errno.
    _strtod_l(source, &parsed, NumberLocale());
    errno = originalErrno;
    if (end)
        *end = parsed;
    if (parsed == source)
        return {};

    const auto* first = source;
    while (*first && std::strchr(" \t\n\r\f\v", *first))
        ++first;
    const bool negative = *first == '-';
    if (*first == '-' || *first == '+')
        ++first;
    const auto prefix = static_cast<unsigned char>(*first) | 0x20;
    _LDOUBLE result{};
    if (prefix == 'i' || prefix == 'n') {
        // UCRT's legacy format for non-finite values is not directly loadable
        // as an x87 number. Write the guest's explicit integer bit and exponent.
        const uint64_t significand = IntegerBit | (prefix == 'n' ? QuietBit : 0);
        const auto exponent = static_cast<uint16_t>(InfiniteExponent | (negative ? Sign : 0));
        std::memcpy(result.ld, &significand, sizeof(significand));
        std::memcpy(result.ld + sizeof(significand), &exponent, sizeof(exponent));
        return result;
    }
    const auto status = _atoldbl_l(&result, const_cast<char*>(source), NumberLocale());
    errno = status ? ERANGE : originalErrno;
    // UCRT can also encode overflow with a zero explicit integer bit.
    if ((result.ld[9] & 0x7f) == 0x7f && result.ld[8] == 0xff)
        result.ld[7] |= 0x80;
    return result;
}

struct ExitCallback {
    uintptr_t address;
    void* argument;
    void* module;
};

std::mutex exitMutex;
std::vector<ExitCallback> exitCallbacks;

int RegisterExit(uintptr_t address, void* argument, void* module)
{
    try {
        const std::lock_guard lock(exitMutex);
        exitCallbacks.push_back({address, argument, module});
        return 0;
    }
    catch (const std::bad_alloc&) {
        return -1;
    }
}

void Finalize(void* module)
{
    for (;;) {
        ExitCallback callback{};
        {
            const std::lock_guard lock(exitMutex);
            const auto it = std::find_if(exitCallbacks.rbegin(), exitCallbacks.rend(), [&](const auto& item) {
                return !module || item.module == module;
            });
            if (it == exitCallbacks.rend())
                return;
            callback = *it;
            exitCallbacks.erase(std::next(it).base());
        }
        CallAndroidCallback(callback.address, callback.argument);
    }
}

[[noreturn]] void ExitAndroidProcess(int status)
{
    Finalize(nullptr);
    std::fflush(nullptr);
    ExitProcess(static_cast<uint8_t>(status));
}

template <bool Move> void* CheckedCopy(void* dst, const void* src, size_t count, size_t bound)
{
    if (count > bound)
        std::abort();
    return Move ? std::memmove(dst, src, count) : std::memcpy(dst, src, count);
}

int SetEnv(const char* key, const char* value, int overwrite)
{
    if (!*key || std::strchr(key, '=')) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && GetEnv(key))
        return 0;
    const auto error = _putenv_s(key, value);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

int UnsetEnv(const char* key)
{
    return SetEnv(key, "", 1);
}

FILE* OpenFile(const char* path, const char* mode)
{
    char binaryMode[8]{};
    const size_t length = strnlen(mode, sizeof(binaryMode) - 1);
    if (length == sizeof(binaryMode) - 1) {
        errno = EINVAL;
        return nullptr;
    }
    size_t used = 0;
    if (!*mode || !std::strchr("rwa", *mode)) {
        errno = EINVAL;
        return nullptr;
    }
    binaryMode[used++] = *mode;
    if (std::strchr(mode, '+'))
        binaryMode[used++] = '+';
    binaryMode[used++] = 'b';
    if (std::strchr(mode, 'x'))
        binaryMode[used++] = 'x';
    if (std::strchr(mode, 'e'))
        binaryMode[used++] = 'N';
    if (path && IsOwnProcPath(path, "/cmdline")) {
        if (*mode != 'r' || std::strchr(mode, '+')) {
            errno = EACCES;
            return nullptr;
        }
        const int fd = OpenProcCommandLine(std::strchr(mode, 'e') ? 0x80000 : 0);
        if (fd < 0)
            return nullptr;
        FILE* file = _fdopen(fd, "rb");
        if (!file) {
            procCommandLines[fd].store(false);
            _close(fd);
        }
        return TagFileStream(file);
    }
    try {
        const auto wide = GuestPath(path, *mode != 'r' || std::strchr(mode, '+'), std::strchr(mode, 'x') == nullptr);
        if (std::strchr(mode, 'x') && GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES) {
            errno = EEXIST;
            return nullptr;
        }
        wchar_t wideMode[std::size(binaryMode)]{};
        std::copy(std::begin(binaryMode), std::end(binaryMode), wideMode);
        FILE* file = nullptr;
        _wfopen_s(&file, wide.c_str(), wideMode);
        return TagFileStream(file);
    }
    catch (const AndroidSystemPathError& error) {
        errno = error.code;
        return nullptr;
    }
    catch (const std::filesystem::filesystem_error&) {
        errno = EINVAL;
        return nullptr;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return nullptr;
    }
}

int OpenTemporaryDescriptor(bool closeOnExec, bool executable = false)
{
    try {
        const auto directory = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 32; ++attempt) {
            uint64_t random = 0;
            if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&random), sizeof(random),
                                BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
                errno = EIO;
                return -1;
            }
            const auto path = directory / (L"Stone-" + std::to_wstring(random) + L".tmp");
            SECURITY_ATTRIBUTES security{sizeof(security), nullptr, !closeOnExec};
            const DWORD access = GENERIC_READ | GENERIC_WRITE | (executable ? GENERIC_EXECUTE : 0);
            const HANDLE file = CreateFileW(path.c_str(), access, 0, &security, CREATE_NEW,
                                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                const auto error = GetLastError();
                if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
                    continue;
                errno = error == ERROR_ACCESS_DENIED ? EACCES : error == ERROR_DISK_FULL ? ENOSPC : EIO;
                return -1;
            }
            const int fd =
                _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_BINARY | (closeOnExec ? _O_NOINHERIT : 0));
            if (fd < 0)
                CloseHandle(file);
            return fd;
        }
        errno = EEXIST;
        return -1;
    }
    catch (const std::filesystem::filesystem_error&) {
        errno = ENOENT;
        return -1;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

FILE* OpenTemporaryStream()
{
    const int fd = OpenTemporaryDescriptor(true);
    if (fd < 0)
        return nullptr;
    FILE* stream = _fdopen(fd, "w+b");
    if (!stream)
        _close(fd);
    return TagFileStream(stream);
}

int OpenProcCommandLine(int flags)
{
    constexpr int Access = 3, Create = 0x40, Exclusive = 0x80, Truncate = 0x200, Append = 0x400;
    constexpr int Nonblocking = 0x800, LargeFile = 0x8000, Directory = 0x10000, NoFollow = 0x20000;
    constexpr int CloseOnExec = 0x80000;
    if ((flags & Access) == Access || (flags & ~(Access | Create | Exclusive | Truncate | Append | Nonblocking |
                                                 LargeFile | Directory | NoFollow | CloseOnExec))) {
        errno = EINVAL;
        return -1;
    }
    if ((flags & (Create | Exclusive)) == (Create | Exclusive)) {
        errno = EEXIST;
        return -1;
    }
    if (flags & Directory) {
        errno = ENOTDIR;
        return -1;
    }
    if (flags & (Access | Truncate | Append)) {
        errno = EACCES;
        return -1;
    }
    try {
        std::string contents;
        {
            const std::lock_guard lock(processNameMutex);
            contents = processName;
        }
        contents.push_back('\0');
        int writer = OpenTemporaryDescriptor(true);
        if (writer < 0)
            return -1;
        const auto close = [](int* fd) {
            _close(*fd);
        };
        const std::unique_ptr<int, decltype(close)> guard(&writer, close);
        const int written = _write(writer, contents.data(), static_cast<unsigned>(contents.size()));
        if (written < 0)
            return -1;
        if (static_cast<size_t>(written) != contents.size()) {
            errno = EIO;
            return -1;
        }
        if (_lseeki64(writer, 0, SEEK_SET) < 0)
            return -1;
        // Reduce kernel handle rights as well as CRT access. Closing the last
        // reader deletes the temporary backing file, including on process exit.
        HANDLE reader = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), FileDescriptorHandle(writer), GetCurrentProcess(), &reader,
                             FILE_GENERIC_READ, !(flags & CloseOnExec), 0))
            return HostFileError(GetLastError());
        const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(reader),
                                       _O_RDONLY | _O_BINARY | ((flags & CloseOnExec) ? _O_NOINHERIT : 0));
        if (fd < 0)
            CloseHandle(reader);
        else
            procCommandLines[fd].store(true);
        return fd;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

int CreateMemoryFile(const char* name, unsigned flags)
{
    constexpr unsigned CloseOnExec = 1, AllowSealing = 2, NoExec = 8, Exec = 16;
    constexpr unsigned KnownFlags = CloseOnExec | AllowSealing | NoExec | Exec;
    if (!name) {
        errno = EFAULT;
        return -1;
    }
    if (strnlen(name, 250) == 250 || (flags & ~KnownFlags) || (flags & (NoExec | Exec)) == (NoExec | Exec)) {
        errno = EINVAL;
        return -1;
    }
    try {
        auto memory = std::make_shared<MemoryFile>();
        memory->name = name;
        if (flags & NoExec) {
            memory->mode = 0666;
            memory->seals = SealExec;
            memory->mappingProtection = 3;
        }
        else if (flags & AllowSealing)
            memory->seals = 0;

        const int fd = OpenTemporaryDescriptor((flags & CloseOnExec) != 0, !(flags & NoExec));
        if (fd < 0)
            return -1;
        DWORD returned = 0;
        DeviceIoControl(FileDescriptorHandle(fd), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
        try {
            const std::lock_guard lock(memoryFilesMutex);
            memoryFiles.emplace(fd, std::move(memory));
        }
        catch (const std::bad_alloc&) {
            _close(fd);
            throw;
        }
        return fd;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

int SetMemoryFileSeals(MemoryFile& memory, unsigned seals)
{
    if (seals & ~KnownMemfdSeals) {
        errno = EINVAL;
        return -1;
    }
    if (memory.seals & SealSeals) {
        errno = EPERM;
        return -1;
    }
    // A section already granted to another process cannot have its mapping rights revoked.
    if (memory.distributed && (seals & ~memory.seals))
        return AndroidUnsupportedOperation();
    if ((seals & SealExec) && (memory.mode & 0111)) {
        seals |= SealShrink | SealGrow | MemfdWriteSeals;
    }
    if ((seals & SealWrite) && memory.writableViews) {
        errno = EBUSY;
        return -1;
    }
    memory.seals |= seals;
    return 0;
}

int64_t SeekDescriptor(int fd, int64_t offset, int origin)
{
    std::unique_lock<std::mutex> lock;
    const auto memory = LockMemoryFile(fd, lock);
    if (FileDescriptorHandle(fd) == INVALID_HANDLE_VALUE)
        return -1;
    return _lseeki64(fd, offset, origin);
}
}

bool InitializeAndroidWinsock()
{
    static const bool available = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return available;
}

int DuplicateCrtFd(int fd, int minimum, bool inherit)
{
    try {
        std::vector<int> lower;
        lower.reserve(minimum);
        int duplicate;
        while ((duplicate = _dup(fd)) >= 0 && duplicate < minimum)
            lower.push_back(duplicate);
        const int error = errno;
        for (const int number : lower)
            _close(number);
        if (duplicate < 0) {
            errno = error;
            return -1;
        }
        if (!SetHandleInformation(FileDescriptorHandle(duplicate), HANDLE_FLAG_INHERIT,
                                  inherit ? HANDLE_FLAG_INHERIT : 0)) {
            _close(duplicate);
            errno = EIO;
            return -1;
        }
        return duplicate;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

int DuplicateAndroidFd(int fd)
{
    return DuplicateDescriptor(fd, 0, true);
}

int DuplicateAndroidFileFd(int fd, bool closeOnExec)
{
    return DuplicateDescriptor(fd, 0, closeOnExec, false);
}

DWORD FileHandleAccess(HANDLE file)
{
    static const auto query =
        reinterpret_cast<decltype(&NtQueryObject)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
    PUBLIC_OBJECT_BASIC_INFORMATION info{};
    return query && query(file, ObjectBasicInformation, &info, sizeof(info), nullptr) >= 0 ? info.GrantedAccess : 0;
}

bool RegularDiskFile(HANDLE file)
{
    FILE_STANDARD_INFO info{};
    return GetFileType(file) == FILE_TYPE_DISK &&
           GetFileInformationByHandleEx(file, FileStandardInfo, &info, sizeof(info)) && !info.Directory;
}

bool ExportAndroidSocketFile(int fd, AndroidSocketFile& grant, bool allowDisk)
{
    constexpr unsigned fixedSize = SealShrink | SealGrow | SealExec;
    std::unique_lock<std::mutex> lock;
    const auto memory = LockMemoryFile(fd, lock);
    const auto file = FileDescriptorHandle(fd);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const auto process = GetCurrentProcess();
    if (!memory && allowDisk && RegularDiskFile(file)) {
        constexpr DWORD dataAccess = FILE_GENERIC_READ | FILE_GENERIC_WRITE;
        const DWORD access = FileHandleAccess(file) & dataAccess;
        HANDLE copy = nullptr;
        if (!(access & (FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA)) ||
            !DuplicateHandle(process, file, process, &copy, access, FALSE, 0)) {
            errno = EACCES;
            return false;
        }
        grant.file = reinterpret_cast<uintptr_t>(copy);
        return true;
    }
    if (!memory || (memory->seals & fixedSize) != fixedSize || (memory->mode & ~0666u)) {
        errno = 95; // EOPNOTSUPP
        return false;
    }
    const bool writable = !(memory->seals & MemfdWriteSeals);
    HANDLE readOnly = nullptr, section = nullptr;
    if (!DuplicateHandle(process, file, process, &readOnly, FILE_GENERIC_READ, FALSE, 0)) {
        errno = EACCES;
        return false;
    }
    const auto mapped =
        memory->section ? memory->section.get()
                        : CreateFileMappingW(file, nullptr, writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
    const DWORD access = SECTION_MAP_READ | (writable ? SECTION_MAP_WRITE : 0);
    if (mapped)
        DuplicateHandle(process, mapped, process, &section, access, FALSE, 0);
    if (mapped && !memory->section)
        CloseHandle(mapped);
    if (!section) {
        CloseHandle(readOnly);
        errno = EACCES;
        return false;
    }
    grant = {reinterpret_cast<uintptr_t>(readOnly), reinterpret_cast<uintptr_t>(section), memory->mode, memory->seals};
    memory->distributed = true;
    return true;
}

int ImportAndroidSocketFile(AndroidSocketFile grant)
{
    using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
    Handle file(reinterpret_cast<HANDLE>(grant.file), &CloseHandle);
    Handle section(reinterpret_cast<HANDLE>(grant.section), &CloseHandle);
    constexpr unsigned fixedSize = SealShrink | SealGrow | SealExec;
    if (!file || !RegularDiskFile(file.get()) ||
        (section &&
         ((grant.seals & fixedSize) != fixedSize || (grant.seals & ~KnownMemfdSeals) || (grant.mode & ~0666u))) ||
        (!section && (grant.mode || grant.seals))) {
        errno = EINVAL;
        return -1;
    }
    const DWORD access = FileHandleAccess(file.get());
    int flags = _O_BINARY | _O_NOINHERIT;
    if (access & (FILE_WRITE_DATA | FILE_APPEND_DATA)) {
        flags |= (access & FILE_READ_DATA) ? _O_RDWR : _O_WRONLY;
        if (!(access & FILE_WRITE_DATA))
            flags |= _O_APPEND;
    }
    if (section) {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
            (access & (FILE_WRITE_DATA | FILE_APPEND_DATA))) {
            errno = EINVAL;
            return -1;
        }
        const DWORD mapAccess = (grant.seals & MemfdWriteSeals) ? FILE_MAP_READ : FILE_MAP_WRITE;
        const auto view = MapViewOfFile(section.get(), mapAccess, 0, 0, static_cast<size_t>(size.QuadPart));
        if (!view) {
            errno = EINVAL;
            return -1;
        }
        UnmapViewOfFile(view);
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(file.get()), flags);
    if (fd < 0)
        return -1;
    file.release();
    if (!section)
        return fd;
    try {
        auto memory = std::make_shared<MemoryFile>();
        memory->seals = grant.seals;
        memory->mode = grant.mode;
        memory->mappingProtection = 3;
        memory->section = std::move(section);
        memory->distributed = true;
        const std::lock_guard lock(memoryFilesMutex);
        memoryFiles.emplace(fd, std::move(memory));
        return fd;
    }
    catch (const std::bad_alloc&) {
        _close(fd);
        errno = ENOMEM;
        return -1;
    }
}

uintptr_t DuplicateAndroidDiskFileHandle(int fd)
{
    const int copy = DuplicateAndroidFileFd(fd, true);
    if (copy < 0)
        return 0;
    const auto source = FileDescriptorHandle(copy);
    HANDLE handle = nullptr;
    if (source != INVALID_HANDLE_VALUE && GetFileType(source) == FILE_TYPE_DISK && !FindMemoryFile(copy)) {
        const auto process = GetCurrentProcess();
        DuplicateHandle(process, source, process, &handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    CloseQueuedAndroidFd(copy);
    return reinterpret_cast<uintptr_t>(handle);
}

uintptr_t DuplicateAndroidReadOnlyMemoryHandle(int fd, unsigned& metadata)
{
    std::unique_lock<std::mutex> lock;
    const auto memory = LockMemoryFile(fd, lock);
    if (!memory || (memory->seals & TransferMemorySeals) != TransferMemorySeals || !(memory->seals & MemfdWriteSeals) ||
        (memory->mode & 0111))
        return 0;
    const auto file = FileDescriptorHandle(fd);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    HANDLE readOnly = nullptr;
    const auto process = GetCurrentProcess();
    if (!DuplicateHandle(process, file, process, &readOnly, FILE_GENERIC_READ, FALSE, 0))
        return 0;
    metadata = (memory->mode << 6) | memory->seals;
    return reinterpret_cast<uintptr_t>(readOnly);
}

int AdoptAndroidReadOnlyMemoryHandle(uintptr_t value, unsigned metadata)
{
    const auto handle = reinterpret_cast<HANDLE>(value);
    const unsigned seals = metadata & 63;
    const unsigned mode = metadata >> 6;
    if ((mode & ~0666u) || (seals & TransferMemorySeals) != TransferMemorySeals || !(seals & MemfdWriteSeals)) {
        CloseHandle(handle);
        errno = EINVAL;
        return -1;
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY | _O_NOINHERIT | _O_RDONLY);
    if (fd < 0) {
        CloseHandle(handle);
        return -1;
    }
    try {
        auto memory = std::make_shared<MemoryFile>();
        memory->seals = seals;
        memory->mode = mode;
        memory->mappingProtection = 3;
        const std::lock_guard lock(memoryFilesMutex);
        memoryFiles.emplace(fd, std::move(memory));
        return fd;
    }
    catch (const std::bad_alloc&) {
        _close(fd);
        errno = ENOMEM;
        return -1;
    }
}

void CloseQueuedAndroidFd(int fd)
{
    // Ancillary data holds an internal reference. Discarding it must not release
    // the process's POSIX record locks as closing a guest-visible descriptor does.
    const std::lock_guard lock(memoryFilesMutex);
    memoryFiles.erase(fd);
    if (fd >= 0 && fd < GuestDescriptorLimit)
        procCommandLines[fd].store(false);
    _close(fd);
}

char* AndroidPathComponent(const char* path, bool directory)
{
    const auto pathStorage = AndroidPathStorage(directory);
    std::string_view component = path ? path : "";
    if (component.empty())
        component = ".";
    else if (const auto end = component.find_last_not_of('/'); end == component.npos)
        component = "/";
    else {
        component = component.substr(0, end + 1);
        const auto slash = component.rfind('/');
        if (directory) {
            if (slash == component.npos)
                component = ".";
            else {
                component = component.substr(0, slash);
                const auto dirEnd = component.find_last_not_of('/');
                component = dirEnd == component.npos ? "/" : component.substr(0, dirEnd + 1);
            }
        }
        else if (slash != component.npos)
            component.remove_prefix(slash + 1);
    }
    if (component.size() >= pathStorage.size()) {
        errno = AndroidNameTooLong;
        return nullptr;
    }
    std::memmove(pathStorage.data(), component.data(), component.size());
    pathStorage[component.size()] = '\0';
    return pathStorage.data();
}

std::span<char> AndroidPathStorage(bool directory)
{
    return directory ? dirnameStorage : basenameStorage;
}

namespace {
std::atomic<uint32_t> applicationUid{10000}; // Process.FIRST_APPLICATION_UID on Windows
}

uint32_t AndroidApplicationUid()
{
    return applicationUid;
}

void SetAndroidApplicationUid(uint32_t uid)
{
    applicationUid = uid;
}

int AndroidUname(AndroidUtsname* result, std::string_view machine)
{
    if (!result) {
        errno = EFAULT;
        return -1;
    }
    *result = {"Linux", {}, "0.0.0-stone", "Stone on Windows", {}, "(none)"};
    std::memcpy(result->node, AndroidNodename, sizeof(AndroidNodename));
    std::memcpy(result->machine, machine.data(), std::min(machine.size(), sizeof(result->machine) - 1));
    return 0;
}

int AndroidGetHostname(char* output, size_t size)
{
    if (size < sizeof(AndroidNodename)) {
        errno = AndroidNameTooLong;
        return -1;
    }
    if (!output) {
        errno = EFAULT;
        return -1;
    }
    std::memcpy(output, AndroidNodename, sizeof(AndroidNodename));
    return 0;
}

void ForgetAndroidFileDescriptor(int fd)
{
    if (fd >= 0 && fd < GuestDescriptorLimit)
        procCommandLines[fd].store(false);
    if (const auto slot = FdOwnerTagSlot(fd))
        slot->store(0);
    if (fd >= 0)
        ForgetByteLocks(FileDescriptorHandle(fd));
    {
        const std::lock_guard lock(fileLocksMutex);
        fileLocks.erase(fd);
    }
    const std::lock_guard lock(randomDevicesMutex);
    randomDevices.erase(fd);
}

void ConfigureAndroidFdSan(int targetSdk)
{
    fdSanLevel.store(targetSdk > 0 && targetSdk < 30 ? FdSanWarnOnce : FdSanFatal);
}

bool CloseAndroidMemoryFile(int fd, int& result)
{
    const std::lock_guard tableLock(memoryFilesMutex);
    const auto found = memoryFiles.find(fd);
    if (found == memoryFiles.end())
        return false;
    const auto memory = found->second;
    const std::lock_guard memoryLock(memory->mutex);
    ForgetAndroidFileDescriptor(fd);
    result = _close(fd);
    memoryFiles.erase(found);
    return true;
}

bool ReadAndroidMemoryFile(int fd, void* buffer, size_t size, intptr_t& result)
{
    std::unique_lock<std::mutex> lock;
    const auto memory = LockMemoryFile(fd, lock);
    if (!memory)
        return false;
    result = FileDescriptorHandle(fd) == INVALID_HANDLE_VALUE
                 ? -1
                 : _read(fd, buffer, static_cast<unsigned>(std::min<size_t>(size, INT_MAX)));
    return true;
}

bool ReadAndroidRandomDevice(int fd, void* buffer, size_t size, intptr_t& result)
{
    const std::lock_guard lock(randomDevicesMutex);
    if (!randomDevices.contains(fd))
        return false;
    result = static_cast<intptr_t>(AndroidGetRandom(static_cast<unsigned char*>(buffer), size, 0));
    return true;
}

int CreateAndroidSharedMemory(const char* name, size_t size)
{
    constexpr unsigned CloseOnExec = 1, NoExecSeal = 8;
    if (!size || size > INT64_MAX) {
        errno = EINVAL;
        return -1;
    }
    const int fd = CreateMemoryFile(name ? name : "", CloseOnExec | NoExecSeal);
    if (fd < 0)
        return -1;
    if (TruncateDescriptor(fd, static_cast<int64_t>(size)) < 0) {
        const int error = errno;
        CloseAndroidFd(fd);
        errno = error;
        return -1;
    }
    const auto memory = FindMemoryFile(fd);
    const std::lock_guard guard(memory->mutex);
    memory->seals |= SealShrink | SealGrow;
    return fd;
}

size_t AndroidSharedMemorySize(int fd)
{
    std::unique_lock<std::mutex> lock;
    if (!LockMemoryFile(fd, lock))
        return 0;
    const auto file = FileDescriptorHandle(fd);
    LARGE_INTEGER length{};
    return file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &length) ? static_cast<size_t>(length.QuadPart) : 0;
}

bool AndroidMemoryFileIoctl(int fd, unsigned long request, uintptr_t argument, int& result)
{
    constexpr unsigned long AshmemGetSize = 0x7704;
    constexpr unsigned long AshmemSetProtection = 0x40087705;
    if (request == AshmemSetProtection) {
        result = SetAndroidSharedMemoryProtection(fd, static_cast<int>(argument));
        return true;
    }
    if (request != AshmemGetSize)
        return false;
    std::unique_lock<std::mutex> lock;
    if (!LockMemoryFile(fd, lock))
        return false;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(FileDescriptorHandle(fd), &length)) {
        errno = EBADF;
        result = -1;
    }
    else if (length.QuadPart > INT_MAX) {
        errno = 75;
        result = -1;
    } // EOVERFLOW
    else
        result = static_cast<int>(length.QuadPart);
    return true;
}

int AndroidFileAvailable(int fd, int* available)
{
    const auto file = FileDescriptorHandle(fd);
    if (file == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (GetFileType(file) != FILE_TYPE_DISK) {
        errno = ENOTTY;
        return -1;
    }
    LARGE_INTEGER size{}, position{};
    if (!GetFileSizeEx(file, &size) || !SetFilePointerEx(file, {}, &position, FILE_CURRENT)) {
        errno = EIO;
        return -1;
    }
    *available = static_cast<int>(std::clamp<int64_t>(size.QuadPart - position.QuadPart, 0, INT_MAX));
    return 0;
}

int SetAndroidSharedMemoryProtection(int fd, int protection)
{
    constexpr int Write = 2;
    std::unique_lock<std::mutex> lock;
    const auto memory = LockMemoryFile(fd, lock);
    if (!memory) {
        errno = EBADF;
        return -1;
    }
    if ((protection & ~memory->mappingProtection) || ((protection & Write) && (memory->seals & MemfdWriteSeals))) {
        errno = EINVAL;
        return -1;
    }
    if ((protection & Write) || (memory->seals & MemfdWriteSeals))
        return 0;
    // Android's memfd ashmem path seals future writes and further seal changes.
    return SetMemoryFileSeals(*memory, SealFutureWrite | SealSeals);
}

int AndroidUnsupportedOperation()
{
    errno = AndroidNotImplemented;
    return -1;
}

void* AndroidMapArm64(void* address, size_t length, int access, int flags, int fd, int64_t offset)
{
    if (access & ~7) {
        errno = EINVAL;
        return reinterpret_cast<void*>(-1);
    }
    if ((flags & (MapShared | MapPrivate | MapAnonymous | MapFixed | MapStack)) ==
        (MapPrivate | MapAnonymous | MapStack)) {
        // Reserve the requested stack extent. Windows does not grow this mmap
        // below the returned range when the guest touches a lower page.
        flags &= ~MapGrowDown;
    }
    try {
        return MapMemory<true>(address, length, access, flags, fd, offset);
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return reinterpret_cast<void*>(-1);
    }
}

int AndroidUnmapArm64(void* address, size_t length)
{
    try {
        return UnmapMemory<true>(address, length);
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

int AndroidProtectArm64(void* address, size_t length, int access)
{
    if (access & ~7) {
        errno = EINVAL;
        return -1;
    }
    try {
        return ProtectMemory<true>(address, length, access);
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

template <bool Arm64 = false> static int AdviseMemory(void* address, size_t length, int advice)
{
    constexpr int WillNeed = 3, DontNeed = 4, Free = 8, DontFork = 10;
    const auto start = reinterpret_cast<uintptr_t>(address);
    const auto rounded = RoundPage(length);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if (start % system.dwPageSize || (length && !rounded) || advice < 0 ||
        (advice > DontNeed && advice != Free && !(Arm64 && advice == DontFork))) {
        errno = EINVAL;
        return -1;
    }
    if (!length)
        return 0;
    const std::lock_guard lock(mappingMutex);
    const auto mapping = FindMapping(start, rounded);
    if (mapping == mappings.end() || mapping->second.arm64 != Arm64) {
        errno = ENOMEM;
        return -1;
    }
    // Stone does not fork guest processes; Windows views are not inherited by fork.
    if (Arm64 && advice == DontFork)
        return 0;
    if (advice < WillNeed)
        return 0; // The OS can disregard access-pattern hints.
    if (advice == WillNeed) {
        WIN32_MEMORY_RANGE_ENTRY range{address, rounded};
        PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
        return 0;
    }
    if (!mapping->second.anonymous) {
        errno = EINVAL;
        return -1;
    }
    if (advice == DontNeed && mapping->second.view)
        return 0; // Shared mappings retain their backing contents.
    if constexpr (Arm64)
        arm64MappingGeneration.fetch_add(1, std::memory_order_release);
    if (advice == DontNeed && mapping->second.uniformProtection == PAGE_READWRITE) {
        // The mapping lock guarantees that this fully committed, writable
        // range cannot be unmapped or reprotected while it is cleared. ART
        // discards thousands of small regions within the same large heap;
        // VirtualQuery repeatedly scans to that heap's far protection boundary.
        // Use the already-known state and retain access for concurrent mutators.
        std::memset(address, 0, rounded);
        return 0;
    }
    const auto uniformProtection = std::exchange(mapping->second.uniformProtection, 0);
    mapping->second.regions.clear();
    for (auto position = start; position < start + rounded;) {
        auto* page = reinterpret_cast<void*>(position);
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(page, &region, sizeof(region))) {
            errno = ENOMEM;
            return -1;
        }
        const auto next =
            std::min(start + rounded, reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize);
        const auto bytes = next - position;
        if (region.State == MEM_COMMIT) {
            if (advice == Free) {
                if (!VirtualAlloc(page, bytes, MEM_RESET, PAGE_NOACCESS)) {
                    errno = ENOMEM;
                    return -1;
                }
            }
            else {
                // DONTNEED zero-fills private anonymous memory without invalidating the mapping.
                // ART mutators can still access its GC card table while pages are discarded.
                const auto executable = region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE);
                const DWORD writable = executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
                DWORD original = region.Protect;
                if (region.Protect != writable && !VirtualProtect(page, bytes, writable, &original)) {
                    errno = ENOMEM;
                    return -1;
                }
                std::memset(page, 0, bytes);
                if (original != writable && !VirtualProtect(page, bytes, original, &original)) {
                    errno = ENOMEM;
                    return -1;
                }
            }
        }
        position = next;
    }
    mapping->second.uniformProtection = uniformProtection;
    return 0;
}

int AndroidAdviseArm64(void* address, size_t length, int advice)
{
    return AdviseMemory<true>(address, length, advice);
}

int AndroidSyncArm64(void* address, size_t length, int flags)
{
    return SyncMemory<true>(address, length, flags);
}

uint64_t AndroidArm64CodeGeneration()
{
    return arm64MappingGeneration.load(std::memory_order_acquire);
}

std::span<const uint8_t> AndroidArm64CodeAt(uintptr_t address)
{
    if (address & 3)
        return {};
    // Cache only owned, committed executable ranges. Mapping mutations already
    // invalidate the shared generation before changing their Windows pages.
    const auto generation = arm64MappingGeneration.load(std::memory_order_acquire);
    for (const auto& region : arm64CodeCache.regions) {
        if (region.generation == generation && address >= region.start && address < region.end &&
            region.end - address >= sizeof(uint32_t))
            return {reinterpret_cast<const uint8_t*>(address), region.end - address};
    }
    const std::lock_guard lock(mappingMutex);
    const auto mapping = FindMapping(address, sizeof(uint32_t));
    if (mapping == mappings.end() || !mapping->second.arm64)
        return {};
    for (const auto& [first, last] : mapping->second.executable) {
        if (address < first || address >= last)
            continue;
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) || region.State != MEM_COMMIT ||
            (region.Protect != PAGE_READONLY && region.Protect != PAGE_READWRITE))
            return {};
        const auto end = std::min(last, reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize);
        if (end - address < sizeof(uint32_t))
            return {};
        auto& cache = arm64CodeCache;
        cache.regions[cache.next] = {std::max(first, reinterpret_cast<uintptr_t>(region.BaseAddress)), end,
                                     arm64MappingGeneration.load(std::memory_order_relaxed)};
        cache.next = (cache.next + 1) % cache.regions.size();
        return {reinterpret_cast<const uint8_t*>(address), end - address};
    }
    return {};
}

bool AndroidExecutableMapping(uintptr_t address, size_t length)
{
    if (!length || address > UINTPTR_MAX - length)
        return false;
    // A fault inside the native mmap adapter can occur while mappingMutex is
    // held. Reject host module PCs before taking that lock. Guest mmap uses
    // VirtualAlloc or MapViewOfFile, never SEC_IMAGE mappings.
    MEMORY_BASIC_INFORMATION first{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &first, sizeof(first)) || first.Type == MEM_IMAGE ||
        first.State != MEM_COMMIT || (first.Protect & PAGE_GUARD) ||
        !(first.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return false;
    // Windows permissions alone are insufficient: native host allocations must
    // never become eligible for Android hardware-signal dispatch.
    const std::lock_guard lock(mappingMutex);
    const auto mapping = FindMapping(address, length);
    if (mapping == mappings.end() || mapping->second.arm64)
        return false;
    const auto end = address + length;
    while (address < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) || region.Type == MEM_IMAGE ||
            region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD) ||
            !(region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        address = std::min(end, reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize);
    }
    return true;
}

uint8_t* AndroidCachedArm64Mapping(uintptr_t address, size_t length, bool writable)
{
    // Mapping changes invalidate cached permissions before touching Windows pages.
    const auto generation = arm64MappingGeneration.load(std::memory_order_acquire);
    for (const auto& region : arm64MappingCache.regions) {
        if (region.generation == generation && address >= region.start && address < region.end &&
            length <= region.end - address && (!writable || region.writable))
            return reinterpret_cast<uint8_t*>(address);
    }
    return nullptr;
}

uint8_t* AndroidMappedArm64Data(uintptr_t address, size_t length, bool writable)
{
    if (length > UINTPTR_MAX - address)
        return nullptr;
    const std::lock_guard lock(mappingMutex);
    const auto* direct = FindArm64DataMapping(address);
    if (!direct)
        return nullptr;
    auto* pointer = reinterpret_cast<uint8_t*>(address);
    if (!length)
        return pointer;
    const auto generation = arm64MappingGeneration.load(std::memory_order_relaxed);
    auto& cache = arm64MappingCache;
    const auto permits = [writable](DWORD protection) {
        return protection == PAGE_READWRITE || (!writable && protection == PAGE_READONLY);
    };
    const auto cacheRange = [&](uintptr_t start, uintptr_t end, DWORD protection) {
        cache.regions[cache.next] = {start, end, generation, protection == PAGE_READWRITE};
        cache.next = (cache.next + 1) % cache.regions.size();
    };
    if (direct->uniformProtection && length <= direct->end - address) {
        if (!permits(direct->uniformProtection))
            return nullptr;
        cacheRange(direct->start, direct->end, direct->uniformProtection);
        return pointer;
    }
    auto mapping = FindMapping(address, 0);
    if (mapping == mappings.end() || !mapping->second.arm64)
        return nullptr;
    const auto end = address + length;
    for (auto position = address; position < end;) {
        // Adjacent mmap allocations may back one guest heap object. Validate
        // every mapped byte before giving the interpreter a contiguous pointer.
        if (mapping == mappings.end() || !mapping->second.arm64 || position < mapping->second.start)
            return nullptr;
        auto& range = mapping->second;
        const auto rangeEnd = std::min(range.end, end);
        if (range.uniformProtection) {
            if (!permits(range.uniformProtection))
                return nullptr;
            cacheRange(range.start, range.end, range.uniformProtection);
        }
        else {
            while (position < rangeEnd) {
                const auto next = std::ranges::upper_bound(range.regions, position, {}, &Mapping::Region::start);
                Mapping::Region region{};
                if (next != range.regions.begin() && position < (next - 1)->end) {
                    region = *(next - 1);
                }
                else {
                    MEMORY_BASIC_INFORMATION page{};
                    if (!VirtualQuery(reinterpret_cast<const void*>(position), &page, sizeof(page)))
                        return nullptr;
                    const auto base = reinterpret_cast<uintptr_t>(page.BaseAddress);
                    region = {std::max(base, range.start), std::min(base + page.RegionSize, range.end),
                              page.State == MEM_COMMIT ? page.Protect : PAGE_NOACCESS};
                    // A mapping operation invalidates these page observations.
                    if (range.regions.size() < 64) {
                        try {
                            range.regions.insert(next, region);
                        }
                        catch (const std::bad_alloc&) {
                        }
                    }
                }
                if (!permits(region.protection))
                    return nullptr;
                cacheRange(region.start, region.end, region.protection);
                position = region.end;
            }
        }
        position = rangeEnd;
        ++mapping;
    }
    return pointer;
}

int64_t AndroidGetRandom(unsigned char* buffer, size_t size, unsigned flags)
{
    if ((flags & ~3u) || size > ULONG_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (!size || BCryptGenRandom(nullptr, buffer, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return static_cast<int64_t>(size);
    errno = EIO;
    return -1;
}

void SetAndroidSystemCaDirs(std::wstring system, std::wstring conscrypt)
{
    systemCa = std::move(system);
    conscryptCa = std::move(conscrypt);
}

void SetAndroidProcDirectory(std::wstring directory)
{
    procDirectory = std::move(directory);
}

void SetAndroidProcessName(std::string name)
{
    if (name.empty() || name.size() >= 4096 || name.find('\0') != name.npos)
        throw std::invalid_argument("invalid Android process name");
    const std::lock_guard lock(processNameMutex);
    processName = std::move(name);
}

int AndroidClock(int clock, AndroidTimespec* result)
{
    if (!result) {
        errno = EFAULT;
        return -1;
    }
    if (clock == 0) {
        FILETIME time{};
        GetSystemTimePreciseAsFileTime(&time);
        const int64_t ticks =
            static_cast<int64_t>((uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime) - 116444736000000000ll;
        *result = {ticks / 10000000, (ticks % 10000000) * 100};
    }
    else if (clock == ProcessCpuClock || clock == ThreadCpuClock || (clock < 0 && (clock & 7) == 6)) {
        FILETIME created{}, exited{}, kernel{}, user{};
        const auto thread = clock < 0 ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                                   ~(static_cast<uint32_t>(clock) >> 3) & 0x1fffffffu)
                                      : GetCurrentThread();
        const BOOL available = clock == ProcessCpuClock
                                   ? GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)
                                   : thread && GetProcessIdOfThread(thread) == GetCurrentProcessId() &&
                                         GetThreadTimes(thread, &created, &exited, &kernel, &user);
        if (clock < 0 && thread)
            CloseHandle(thread);
        if (!available) {
            errno = clock < 0 ? EINVAL : EIO;
            return -1;
        }
        const auto ticks = [](const FILETIME& time) {
            return (uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
        };
        const uint64_t total = ticks(kernel) + ticks(user);
        *result = {static_cast<int64_t>(total / 10'000'000), static_cast<int64_t>(total % 10'000'000 * 100)};
    }
    else if (clock == 1 || clock == 7) {
        // Android's monotonic clock and SystemClock uptime share an origin.
        // BOOTTIME includes time spent suspended, while MONOTONIC excludes it.
        ULONGLONG ticks = 0;
        if (clock == 1)
            QueryUnbiasedInterruptTimePrecise(&ticks);
        else
            QueryInterruptTimePrecise(&ticks);
        *result = {static_cast<int64_t>(ticks / 10'000'000), static_cast<int64_t>(ticks % 10'000'000 * 100)};
    }
    else if (clock == 4) {
        LARGE_INTEGER counter{}, frequency{};
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        *result = {counter.QuadPart / frequency.QuadPart,
                   (counter.QuadPart % frequency.QuadPart) * 1000000000 / frequency.QuadPart};
    }
    else {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

template <typename Write> int PrintArguments(const char* format, AndroidVaList* args, Write write)
{
    auto copy = *args;
    const int length = AndroidVsnprintf(nullptr, 0, format, &copy);
    if (length < 0)
        return -1;
    try {
        std::string output(static_cast<size_t>(length), '\0');
        if (AndroidVsnprintf(output.data(), output.size() + 1, format, args) != length)
            return -1;
        return write(output) ? length : -1;
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    }
}

extern "C" int StoneAndroidPrint(void* file, const char* format, AndroidVaList* args)
{
    return PrintArguments(format, args, [&](std::string_view output) {
        return std::fwrite(output.data(), 1, output.size(), HostFile(file)) == output.size();
    });
}

extern "C" int StoneAndroidStdout(const char* format, AndroidVaList* args)
{
    return StoneAndroidPrint(stdout, format, args);
}

extern "C" int StoneAndroidPrintDescriptor(int fd, const char* format, AndroidVaList* args)
{
    return PrintArguments(format, args, [&](std::string_view output) {
        if (output.empty())
            return WriteDescriptor(fd, output.data(), 0) == 0;
        while (!output.empty()) {
            const auto written = WriteDescriptor(fd, output.data(), output.size());
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0) {
                if (!written)
                    errno = EIO;
                return false;
            }
            output.remove_prefix(static_cast<size_t>(written));
        }
        return true;
    });
}

extern "C" int StoneAndroidAllocateFormat(char** output, const char* format, AndroidVaList* args)
{
    *output = nullptr;
    auto measure = *args;
    const int length = AndroidVsnprintf(nullptr, 0, format, &measure);
    if (length < 0)
        return -1;
    const auto size = static_cast<size_t>(length) + 1;
    auto* buffer = static_cast<char*>(std::malloc(size));
    if (!buffer) {
        errno = ENOMEM;
        return -1;
    }
    if (AndroidVsnprintf(buffer, size, format, args) != length) {
        std::free(buffer);
        return -1;
    }
    *output = buffer;
    return length;
}

void* AndroidMalloc(size_t size)
{
    void* memory = std::malloc(size);
    if (memory && !RecordNativeBlock(memory, 0, _msize(memory))) {
        std::free(memory);
        return nullptr;
    }
    return memory;
}

void* AndroidCalloc(size_t count, size_t size)
{
    void* memory = std::calloc(count, size);
    if (memory && !RecordNativeBlock(memory, 0, _msize(memory))) {
        std::free(memory);
        return nullptr;
    }
    return memory;
}

uintptr_t AndroidArtHeapImport(std::string_view name)
{
    if (name != "mallinfo" && name != "mallinfo2")
        return 0;
    return Import("stone-art-mallinfo", &ReadArtHeapStatistics);
}

void AndroidFree(void* memory)
{
    if (!memory)
        return;
    bool aligned = false;
    {
        const std::lock_guard lock(nativeBlocks.mutex);
        if (const auto block = nativeBlocks.entries.Find(memory)) {
            aligned = block->alignment != 0;
            nativeBlocks.bytes -= block->size;
            nativeBlocks.entries.Erase(memory);
        }
    }
    if (aligned)
        _aligned_free(memory);
    else
        std::free(memory);
}

void* AndroidRealloc(void* memory, size_t size)
{
    if (!memory)
        return AndroidMalloc(size);
    if (!size) {
        AndroidFree(memory);
        return nullptr;
    }
    NativeBlock block{};
    {
        const std::lock_guard lock(nativeBlocks.mutex);
        auto* found = nativeBlocks.entries.Find(memory);
        const bool foreign = found == nullptr;
        if (foreign) {
            // Reserve bookkeeping before realloc: on allocation failure the
            // caller must retain its old block, even for a foreign CRT pointer.
            try {
                found = nativeBlocks.entries.Insert(memory, NativeBlock{0, _msize(memory)});
                if (!found)
                    std::abort();
                nativeBlocks.bytes += found->size;
            }
            catch (const std::bad_alloc&) {
                errno = ENOMEM;
                return nullptr;
            }
        }
        block = *found;
        if (!block.alignment) {
            void* moved = std::realloc(memory, size);
            if (!moved) {
                if (foreign) {
                    nativeBlocks.bytes -= block.size;
                    nativeBlocks.entries.Erase(memory);
                }
                return nullptr;
            }
            const size_t allocated = _msize(moved);
            nativeBlocks.entries.Replace(memory, moved, {0, allocated});
            nativeBlocks.bytes = nativeBlocks.bytes - block.size + allocated;
            return moved;
        }
    }
    void* moved = AllocateAligned(block.alignment, size);
    if (!moved)
        return nullptr;
    std::memcpy(moved, memory, std::min(size, block.size));
    AndroidFree(memory);
    return moved;
}

namespace {
struct AndroidResourceLimit {
    uint64_t current, maximum;
};

int GetResourceLimit(int resource, AndroidResourceLimit* result)
{
    if (resource < 0 || resource >= 16) {
        errno = EINVAL;
        return -1;
    }
    if (!result) {
        errno = EFAULT;
        return -1;
    }
    uint64_t limit;
    switch (resource) {
    case 2: // RLIMIT_DATA: the guest has no independent heap size limit.
        limit = UINT64_MAX;
        break;
    case 4: // RLIMIT_CORE: the Windows guest backend cannot write ELF core files.
        limit = 0;
        break;
    case 6: { // RLIMIT_NPROC: isolated guests run under a process-limited Windows job.
        JOBOBJECT_BASIC_LIMIT_INFORMATION job{};
        limit = UINT64_MAX;
        if (QueryInformationJobObject(nullptr, JobObjectBasicLimitInformation, &job, sizeof(job), nullptr) &&
            (job.LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS))
            limit = job.ActiveProcessLimit;
        break;
    }
    case 7: // RLIMIT_NOFILE: guest files and sockets both allocate UCRT low-I/O slots.
        limit = GuestDescriptorLimit;
        break;
    default:
        return AndroidUnsupportedOperation();
    }
    // Linux reports EFAULT when the caller supplies a read-only result buffer.
    __try {
        *result = {limit, limit};
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
        errno = EFAULT;
        return -1;
    }
    return 0;
}

int SetResourceLimit(int resource, const AndroidResourceLimit* requested)
{
    AndroidResourceLimit actual;
    if (GetResourceLimit(resource, &actual) != 0)
        return -1;
    if (!requested) {
        errno = EFAULT;
        return -1;
    }
    if (requested->current > requested->maximum) {
        errno = EINVAL;
        return -1;
    }
    if (requested->maximum > actual.maximum) {
        errno = EPERM;
        return -1;
    }
    if (requested->current == actual.current && requested->maximum == actual.maximum)
        return 0;
    return AndroidUnsupportedOperation();
}

int ClockResolution(int clock, AndroidTimespec* result)
{
    int64_t nanos = 100; // Windows FILETIME and interrupt-time units.
    if (clock == 4) {
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency)) {
            errno = EIO;
            return -1;
        }
        nanos = (999'999'999 + frequency.QuadPart) / frequency.QuadPart;
    }
    else if (clock != 0 && clock != 1 && clock != 7 && clock != ProcessCpuClock && clock != ThreadCpuClock &&
             !(clock < 0 && (clock & 7) == 6)) {
        errno = EINVAL;
        return -1;
    }
    if (result)
        *result = {0, nanos};
    return 0;
}

int ParentProcessId()
{
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        errno = EIO;
        return -1;
    }
    const auto process = GetCurrentProcessId();
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    for (BOOL next = Process32FirstW(snapshot, &entry); next; next = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == process) {
            parent = entry.th32ParentProcessID;
            break;
        }
    }
    CloseHandle(snapshot);
    return static_cast<int>(parent);
}

struct AndroidSystemInfo {
    int64_t uptime;
    uint64_t load[3], totalRam, freeRam, sharedRam, bufferRam, totalSwap, freeSwap;
    uint16_t threadCount, padding;
    uint64_t totalHigh, freeHigh;
    uint32_t memoryUnit;
};

static_assert(sizeof(AndroidSystemInfo) == 112);

int GetAndroidSystemInfo(AndroidSystemInfo* result)
{
    if (!result) {
        errno = EFAULT;
        return -1;
    }
    PERFORMANCE_INFORMATION host{};
    if (!K32GetPerformanceInfo(&host, sizeof(host))) {
        errno = EIO;
        return -1;
    }
    *result = {};
    result->uptime = static_cast<int64_t>(GetTickCount64() / 1000);
    result->totalRam = host.PhysicalTotal * host.PageSize;
    result->freeRam = host.PhysicalAvailable * host.PageSize;
    result->threadCount = static_cast<uint16_t>(std::min<DWORD>(host.ThreadCount, UINT16_MAX));
    result->memoryUnit = 1;
    return 0;
}

constexpr char ApplicationUserName[] = "u0_a0";

int GetAndroidPasswd(uint32_t uid, AndroidPasswd* storage, char* strings, size_t capacity, AndroidPasswd** result)
{
    if (!storage || !result || (capacity && !strings))
        return EINVAL;
    *result = nullptr;
    if (uid != AndroidApplicationUid())
        return 0;
    constexpr char directory[] = "/data", shell[] = "/bin/sh";
    if (capacity < sizeof(ApplicationUserName) + sizeof(directory) + sizeof(shell))
        return ERANGE;
    char* home = strings + sizeof(ApplicationUserName);
    char* command = home + sizeof(directory);
    std::memcpy(strings, ApplicationUserName, sizeof(ApplicationUserName));
    std::memcpy(home, directory, sizeof(directory));
    std::memcpy(command, shell, sizeof(shell));
    *storage = {strings, nullptr, uid, uid, nullptr, home, command};
    *result = storage;
    return 0;
}

AndroidPasswd* LookupAndroidPasswd(uint32_t uid)
{
    thread_local AndroidPasswd storage{};
    thread_local char strings[64]{};
    AndroidPasswd* result = nullptr;
    const int error = GetAndroidPasswd(uid, &storage, strings, sizeof(strings), &result);
    if (error)
        errno = error;
    return result;
}

AndroidGroup* LookupAndroidGroup(uint32_t gid)
{
    if (gid != AndroidApplicationUid())
        return nullptr;
    thread_local char name[] = "u0_a0";
    thread_local char* members[]{name, nullptr};
    thread_local AndroidGroup group{name, nullptr, AndroidApplicationUid(), members};
    return &group;
}

int GetAndroidGroup(uint32_t gid, AndroidGroup* storage, char* strings, size_t capacity, AndroidGroup** result)
{
    if (!storage || !result || (capacity && !strings))
        return EINVAL;
    *result = nullptr;
    if (gid != AndroidApplicationUid())
        return 0;
    const size_t names = sizeof(ApplicationUserName);
    const size_t padding =
        (alignof(char*) - (reinterpret_cast<uintptr_t>(strings) + names) % alignof(char*)) % alignof(char*);
    if (capacity < names + padding + 2 * sizeof(char*))
        return ERANGE;
    auto** members = reinterpret_cast<char**>(strings + names + padding);
    std::memcpy(strings, ApplicationUserName, names);
    members[0] = strings;
    members[1] = nullptr;
    *storage = {strings, nullptr, gid, members};
    *result = storage;
    return 0;
}

int SetAndroidIdentity(uint32_t id)
{
    if (id == AndroidApplicationUid())
        return 0;
    errno = EPERM;
    return -1;
}

std::atomic<uint64_t> random48{0x1234abcd330eull};

constexpr uint64_t AdvanceRandom48(uint64_t seed)
{
    return (seed * 0x5deece66dull + 11) & ((uint64_t{1} << 48) - 1);
}

int64_t NextRandom48()
{
    auto previous = random48.load(std::memory_order_relaxed);
    uint64_t next;
    do {
        next = AdvanceRandom48(previous);
    } while (!random48.compare_exchange_weak(previous, next, std::memory_order_relaxed));
    return static_cast<int64_t>(next >> 17);
}

int64_t Random48FromSeed(uint16_t* seed)
{
    const auto next = AdvanceRandom48(uint64_t(seed[0]) | (uint64_t(seed[1]) << 16) | (uint64_t(seed[2]) << 32));
    for (unsigned i = 0; i < 3; ++i)
        seed[i] = static_cast<uint16_t>(next >> (16 * i));
    return static_cast<int64_t>(next >> 17);
}

uint32_t TerminalSpeed(const uint32_t* flags)
{
    return flags[2] & 0x100fu;
}

int SetTerminalSpeed(uint32_t* flags, uint32_t speed)
{
    flags[2] = (flags[2] & ~0x100fu) | (speed & 0x100fu);
    return 0;
}

}

int64_t AndroidX64Syscall(unsigned number, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f)
{
    const auto result = [](int64_t value) -> int64_t {
        return value == -1 ? -(errno ? errno : EIO) : value;
    };
    switch (number) {
    case 0:
        return result(ReadAndroidFd(static_cast<int>(a), reinterpret_cast<void*>(b), c));
    case 1:
        return result(WriteDescriptor(static_cast<int>(a), reinterpret_cast<const void*>(b), c));
    case 3:
        return result(CloseTaggedDescriptor(static_cast<int>(a), 0));
    case 9:
        return result(
            reinterpret_cast<intptr_t>(MapMemory<>(reinterpret_cast<void*>(a), b, static_cast<int>(c),
                                                   static_cast<int>(d), static_cast<int>(e), static_cast<int64_t>(f))));
    case 11:
        return result(UnmapMemory<>(reinterpret_cast<void*>(a), b));
    case 14: {
        if (d != sizeof(uint64_t))
            return -EINVAL;
        const auto mask =
            reinterpret_cast<int (*)(int, const uint64_t*, uint64_t*)>(AndroidThreadImport("pthread_sigmask", true));
        return -mask(static_cast<int>(a), reinterpret_cast<const uint64_t*>(b), reinterpret_cast<uint64_t*>(c));
    }
    case 24:
        SwitchToThread();
        return 0;
    case 32:
        return result(DuplicateDescriptor(static_cast<int>(a), 0, false));
    case 35:
        return result(NanoSleep(reinterpret_cast<const AndroidTimespec*>(a), reinterpret_cast<AndroidTimespec*>(b)));
    case 39:
        return GetCurrentProcessId();
    case 61:
        return -ECHILD;
    case 157:
        return -EINVAL;
    case 186:
        return GetCurrentThreadId();
    case 202:
        if (!a || a % alignof(uint32_t))
            return -EINVAL;
        if ((b & ~uintptr_t{128}) == 3 || (b & ~uintptr_t{128}) == 4) {
            if (c > INT_MAX || d > INT_MAX)
                return -EINVAL;
            return AndroidFutexRequeue(*reinterpret_cast<uint32_t*>(a), static_cast<unsigned>(b),
                                       static_cast<uint32_t>(c), static_cast<uint32_t>(d),
                                       reinterpret_cast<uint32_t*>(e), static_cast<uint32_t>(f));
        }
        return AndroidFutex(*reinterpret_cast<uint32_t*>(a), static_cast<unsigned>(b), static_cast<uint32_t>(c),
                            reinterpret_cast<const AndroidTimespec*>(d), static_cast<uint32_t>(f));
    case 231:
        ExitProcess(static_cast<UINT>(a) & 0xffu);
    case 257:
        if (!b)
            return -EFAULT;
        return result(
            OpenAt(static_cast<int>(a), reinterpret_cast<const char*>(b), static_cast<int>(c), static_cast<int>(d)));
    case 262:
        if (!b || !c)
            return -EFAULT;
        return result(StatAt<>(static_cast<int>(a), reinterpret_cast<const char*>(b), reinterpret_cast<AndroidStat*>(c),
                               static_cast<int>(d)));
    case 267: {
        if (!b || !c)
            return -EFAULT;
        const auto* name = reinterpret_cast<const char*>(b);
        if (!IsOwnExecutableLink(name))
            return -ENOENT;
        return result(ReadGuestLink(name, reinterpret_cast<char*>(c), d));
    }
    case 292:
        return result(DuplicateDescriptor3(static_cast<int>(a), static_cast<int>(b), static_cast<int>(c)));
    default:
        return -38; // Bionic ENOSYS, including clone and execve.
    }
}

const char* ParseAndroidTime(const char* input, const char* format, AndroidTm* value)
{
    try {
        std::string expanded;
        for (; *format; ++format) {
            if (*format != '%' || !format[1]) {
                expanded += *format;
                continue;
            }
            switch (*++format) {
            case 'D':
                expanded += "%m/%d/%y";
                break;
            case 'F':
                expanded += "%Y-%m-%d";
                break;
            case 'R':
                expanded += "%H:%M";
                break;
            case 'T':
                expanded += "%H:%M:%S";
                break;
            case 'h':
                expanded += "%b";
                break;
            default:
                expanded += '%';
                expanded += *format;
                break;
            }
        }
        auto fields = value->fields;
        std::istringstream stream(input);
        stream.imbue(std::locale::classic());
        stream >> std::noskipws >> std::get_time(&fields, expanded.c_str());
        if (stream.fail())
            return nullptr;
        const auto consumed = stream.rdbuf()->pubseekoff(0, std::ios::cur, std::ios::in);
        if (consumed < 0)
            return nullptr;
        value->fields = fields;
        return input + static_cast<ptrdiff_t>(consumed);
    }
    catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return nullptr;
    }
}

static int AcquireStaticGuard(uint8_t* guard)
{
    // The compiler checks the first byte before calling into the runtime.
    std::atomic_ref initialized(guard[0]);
    std::atomic_ref pending(guard[1]);
    for (;;) {
        if (initialized.load(std::memory_order_acquire))
            return 0;
        uint8_t idle = 0;
        if (pending.compare_exchange_strong(idle, uint8_t{1}, std::memory_order_acquire)) {
            if (!initialized.load(std::memory_order_relaxed))
                return 1;
            pending.store(0, std::memory_order_release);
            pending.notify_all();
            return 0;
        }
        pending.wait(1, std::memory_order_acquire);
    }
}

template <bool Initialized> static void FinishStaticGuard(uint8_t* guard)
{
    if constexpr (Initialized)
        std::atomic_ref(guard[0]).store(1, std::memory_order_release);
    std::atomic_ref pending(guard[1]);
    pending.store(0, std::memory_order_release);
    pending.notify_all();
}

uintptr_t AndroidLibcImport(std::string_view name, bool hostAbi)
{
    if (name == "__pread64_chk")
        name = "__pread_chk";
    if (name == "__pwrite64_chk")
        name = "__pwrite_chk";
#define BIND(symbol, ...)                                                                                              \
    if (name == #symbol) {                                                                                             \
        const auto target = reinterpret_cast<uintptr_t>(__VA_ARGS__);                                                  \
        return hostAbi ? target : BridgeAndroidImport(name, target);                                                   \
    }
    if (hostAbi) {
        if (name == "stat" || name == "lstat" || name == "stat64" || name == "lstat64")
            return reinterpret_cast<uintptr_t>(&StatPath<AndroidArm64Stat>);
        if (name == "fstat" || name == "fstat64")
            return reinterpret_cast<uintptr_t>(&StatFile<AndroidArm64Stat>);
        if (name == "fstatat" || name == "fstatat64")
            return reinterpret_cast<uintptr_t>(&StatAt<AndroidArm64Stat>);
    }
    if (name == "__sF")
        return reinterpret_cast<uintptr_t>(standardFiles);
    if (name == "__stack_chk_guard")
        return reinterpret_cast<uintptr_t>(AndroidGlobalStackGuard());
    static void* streams[]{&standardFiles[0], &standardFiles[1], &standardFiles[2]};
    if (name == "stdin")
        return reinterpret_cast<uintptr_t>(&streams[0]);
    if (name == "stdout")
        return reinterpret_cast<uintptr_t>(&streams[1]);
    if (name == "stderr")
        return reinterpret_cast<uintptr_t>(&streams[2]);
    if (name == "sscanf")
        return reinterpret_cast<uintptr_t>(&AndroidVarargsScan);
    if (!hostAbi && name == "printf")
        return reinterpret_cast<uintptr_t>(&AndroidVarargsStdout);
    if (!hostAbi && name == "dprintf")
        return reinterpret_cast<uintptr_t>(&AndroidVarargsDescriptor);
    if (name == "snprintf")
        return reinterpret_cast<uintptr_t>(&AndroidVarargsFormat);
    if (!hostAbi && name == "sprintf")
        return reinterpret_cast<uintptr_t>(&AndroidVarargsUnboundedFormat);
    if (name == "swprintf" && !hostAbi)
        return reinterpret_cast<uintptr_t>(&AndroidVarargsWideFormat);
    if (!hostAbi && (name == "strtold" || name == "strtold_l"))
        return reinterpret_cast<uintptr_t>(&AndroidParseLongDouble);
    if (!hostAbi && (name == "wcstold" || name == "wcstold_l"))
        return reinterpret_cast<uintptr_t>(&AndroidWideParseLongDouble);
    if (name == "asprintf")
        return reinterpret_cast<uintptr_t>(&AndroidVarargsAllocateFormat);
    if (name == "setjmp")
        return reinterpret_cast<uintptr_t>(&AndroidSetJmp);
    if (name == "longjmp")
        return reinterpret_cast<uintptr_t>(&AndroidLongJmp);
    if (!hostAbi && name == "sigsetjmp")
        return reinterpret_cast<uintptr_t>(&AndroidSignalSetJmp);
    if (!hostAbi && name == "siglongjmp")
        return reinterpret_cast<uintptr_t>(&AndroidLongJmp);
    if (!hostAbi && name == "lldiv")
        return reinterpret_cast<uintptr_t>(&AndroidLongLongDiv);
    if (name == "environ")
        return reinterpret_cast<uintptr_t>(EnvironmentSlot());
    if (!hostAbi && name == "timezone")
        return reinterpret_cast<uintptr_t>(&LocalTimeZone().offset);
    if (!hostAbi && name == "daylight")
        return reinterpret_cast<uintptr_t>(&LocalTimeZone().daylight);
    if (!hostAbi && name == "tzname")
        return reinterpret_cast<uintptr_t>(&LocalTimeZone().names);
    static constexpr std::array<uint8_t, 16> anyIpv6{}, loopbackIpv6{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (!hostAbi && name == "in6addr_any")
        return reinterpret_cast<uintptr_t>(anyIpv6.data());
    if (!hostAbi && name == "in6addr_loopback")
        return reinterpret_cast<uintptr_t>(loopbackIpv6.data());
    BIND(__cxa_atexit, &RegisterExit);
    BIND(__cxa_finalize, &Finalize);
    BIND(__cxa_guard_acquire, &AcquireStaticGuard);
    BIND(__cxa_guard_release, &FinishStaticGuard<true>);
    BIND(__cxa_guard_abort, &FinishStaticGuard<false>);
    BIND(
        umask, +[](unsigned mask) {
            return fileCreationMask.exchange(mask & 0777, std::memory_order_relaxed);
        });
    BIND(
        __umask_chk, +[](unsigned mask) {
            if (mask & ~0777u)
                std::abort();
            return fileCreationMask.exchange(mask, std::memory_order_relaxed);
        });
    BIND(
        atexit, +[](uintptr_t callback) {
            return RegisterExit(callback, nullptr, nullptr);
        });
    BIND(exit, &ExitAndroidProcess);
    BIND(__errno, &AndroidErrno);
    BIND(__stack_chk_fail, &std::abort);
    BIND(__memcpy_chk, &CheckedCopy<false>);
    BIND(__memmove_chk, &CheckedCopy<true>);
    BIND(
        __memset_chk, +[](void* out, int value, size_t size, size_t bound) {
            if (size > bound)
                std::abort();
            return std::memset(out, value, size);
        });
    BIND(
        memset_explicit, +[](void* out, int value, size_t size) -> void* {
            auto* target = static_cast<volatile unsigned char*>(out);
            for (size_t i = 0; i < size; ++i)
                target[i] = static_cast<unsigned char>(value);
            return out;
        });
    BIND(
        memrchr, +[](const void* address, int value, size_t size) -> const void* {
            const auto* start = static_cast<const unsigned char*>(address);
            while (size)
                if (start[--size] == static_cast<unsigned char>(value))
                    return start + size;
            return nullptr;
        });
    BIND(
        __strlen_chk, +[](const char* value, size_t size) {
            const auto length = strnlen(value, size);
            if (length == size)
                std::abort();
            return length;
        });
    BIND(
        __strcpy_chk, +[](char* out, const char* in, size_t size) {
            const auto length = strnlen(in, size);
            if (length == size)
                std::abort();
            return std::memcpy(out, in, length + 1);
        });
    BIND(
        __stpcpy_chk, +[](char* out, const char* in, size_t size) {
            const auto length = strnlen(in, size);
            if (length == size)
                std::abort();
            std::memcpy(out, in, length + 1);
            return out + length;
        });
    BIND(
        __strcat_chk, +[](char* out, const char* in, size_t bound) {
            const auto prefix = strnlen(out, bound);
            const auto suffix = strnlen(in, bound - prefix);
            if (suffix == bound - prefix)
                std::abort();
            std::memcpy(out + prefix, in, suffix + 1);
            return out;
        });
    BIND(
        __strncat_chk, +[](char* out, const char* in, size_t limit, size_t bound) {
            const auto prefix = strnlen(out, bound);
            if (prefix == bound)
                std::abort();
            const auto suffix = strnlen(in, limit);
            if (suffix >= bound - prefix)
                std::abort();
            std::memcpy(out + prefix, in, suffix);
            out[prefix + suffix] = '\0';
            return out;
        });
    BIND(
        __strrchr_chk, +[](const char* text, int byte, size_t bound) {
            if (strnlen(text, bound) == bound)
                std::abort();
            return std::strrchr(text, byte);
        });
    BIND(
        __strncpy_chk, +[](char* out, const char* in, size_t size, size_t bound) {
            if (size > bound)
                std::abort();
            if (size) {
                const auto length = strnlen(in, size);
                std::memcpy(out, in, length);
                std::memset(out + length, 0, size - length);
            }
            return out;
        });
    BIND(
        __strlcpy_chk, +[](char* out, const char* in, size_t size, size_t bound) {
            if (size > bound)
                std::abort();
            return StringCopy(out, in, size);
        });
    BIND(
        __strlcat_chk, +[](char* out, const char* in, size_t size, size_t bound) {
            if (size > bound)
                std::abort();
            return StringConcat(out, in, size);
        });
    BIND(
        __strchr_chk, +[](const char* value, int ch, size_t bound) -> const char* {
            const auto target = static_cast<char>(ch);
            for (size_t i = 0; i < bound; ++i) {
                if (value[i] == target)
                    return value + i;
                if (value[i] == '\0')
                    return nullptr;
            }
            std::abort();
        });
    BIND(
        __strncpy_chk2, +[](char* out, const char* in, size_t size, size_t outBound, size_t inBound) {
            if (size > outBound)
                std::abort();
            if (!size)
                return out;
            const size_t length = strnlen(in, std::min(size, inBound));
            if (length == inBound && length < size)
                std::abort();
            std::memcpy(out, in, length);
            std::memset(out + length, 0, size - length);
            return out;
        });
    BIND(abort, &std::abort);
    BIND(atoi, &std::atoi);
    BIND(atof, &std::atof);
    BIND(atol, &std::atoll);
    BIND(atoll, &std::atoll);
    BIND(
        __assert2, +[](const char* file, int line, const char* function, const char* expression) {
            std::fprintf(stderr, "[android-native] %s:%d: %s: assertion failed: %s\n", file, line, function,
                         expression);
            std::abort();
        });
    BIND(
        __assert, +[](const char* file, int line, const char* expression) {
            std::fprintf(stderr, "[android-native] %s:%d: assertion failed: %s\n", file, line, expression);
            std::abort();
        });
    // Bionic's narrow character classification is ASCII for all locales.
    static constexpr auto lower = +[](int c) -> int {
        return c >= 'a' && c <= 'z';
    };
    static constexpr auto upper = +[](int c) -> int {
        return c >= 'A' && c <= 'Z';
    };
    static constexpr auto hex = +[](int c) -> int {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    BIND(islower, lower);
    BIND(isupper, upper);
    BIND(
        isalpha, +[](int c) -> int {
            return lower(c) || upper(c);
        });
    BIND(
        isalnum, +[](int c) -> int {
            return lower(c) || upper(c) || (c >= '0' && c <= '9');
        });
    BIND(
        tolower, +[](int c) {
            return upper(c) ? c + ('a' - 'A') : c;
        });
    BIND(
        toupper, +[](int c) {
            return lower(c) ? c - ('a' - 'A') : c;
        });
    BIND(
        isspace, +[](int c) {
            return (c == ' ' || (c >= '\t' && c <= '\r')) ? 1 : 0;
        });
    BIND(isxdigit, hex);
    BIND(
        isdigit_l, +[](int c, _locale_t) -> int {
            return c >= '0' && c <= '9';
        });
    BIND(
        islower_l, +[](int c, _locale_t) {
            return lower(c);
        });
    BIND(
        isupper_l, +[](int c, _locale_t) {
            return upper(c);
        });
    BIND(
        isxdigit_l, +[](int c, _locale_t) {
            return hex(c);
        });
    BIND(
        btowc, +[](int c) -> uint32_t {
            const auto value = std::btowc(c);
            return value == WEOF ? UINT32_MAX : value;
        });
    BIND(
        wctob, +[](uint32_t c) {
            return c < WEOF ? std::wctob(static_cast<wint_t>(c)) : EOF;
        });
    BIND(strchr, static_cast<const char* (*)(const char*, int)>(&std::strchr));
    BIND(strrchr, static_cast<const char* (*)(const char*, int)>(&std::strrchr));
    BIND(strstr, static_cast<const char* (*)(const char*, const char*)>(&std::strstr));
    BIND(strpbrk, static_cast<const char* (*)(const char*, const char*)>(&std::strpbrk));
    BIND(strerror, &ErrorText);
    BIND(strlcpy, &StringCopy);
    BIND(strlcat, &StringConcat);
    BIND(
        __gnu_strerror_r, +[](int code, char* buffer, size_t size) {
            StringCopy(buffer, ErrorText(code), size);
            return buffer;
        });
    BIND(
        strerror_r, +[](int code, char* buffer, size_t size) {
            if (size == 0)
                return ERANGE;
            const char* message = ErrorText(code);
            return StringCopy(buffer, message, size) >= size ? ERANGE : 0;
        });
    BIND(strcasecmp, &_stricmp);
    BIND(strncasecmp, &_strnicmp);
    BIND(strcoll, &std::strcoll);
    BIND(strxfrm, &std::strxfrm);
    BIND(strcoll_l, &_strcoll_l);
    BIND(strxfrm_l, &_strxfrm_l);
    BIND(strtod, &std::strtod);
    BIND(strtok_r, &strtok_s);
    BIND(strtol, &std::strtoll);
    BIND(strtoll, &std::strtoll);
    BIND(
        strtok, +[](char* input, const char* separators) -> char* {
            thread_local char* state = nullptr;
            return input || state ? strtok_s(input, separators, &state) : nullptr;
        });
    BIND(strtoul, &std::strtoull);
    BIND(strtoull, &std::strtoull);
    BIND(wcsncpy, &WideCopy);
    BIND(clock_gettime, &AndroidClock);
    BIND(clock_getres, &ClockResolution);
    BIND(
        clock, +[]() -> int64_t {
            AndroidTimespec time{};
            if (AndroidClock(ProcessCpuClock, &time))
                return -1;
            return time.seconds * 1'000'000 + time.nanos / 1000;
        });
    BIND(timespec_get, &TimeSpecGet);
    BIND(gettimeofday, &TimeOfDay);
    BIND(nanosleep, &NanoSleep);
    BIND(clock_nanosleep, &ClockNanoSleep);
    BIND(sleep, &SleepSeconds);
    BIND(usleep, &SleepMicroseconds);
    BIND(gmtime_r, &BrokenDownTime<false>);
    BIND(localtime_r, &BrokenDownTime<true>);
    BIND(
        gmtime, +[](const time_t* seconds) {
            thread_local AndroidTm tm{};
            return BrokenDownTime<false>(seconds, &tm);
        });
    BIND(
        strftime, +[](char* output, size_t size, const char* format, const AndroidTm* tm) {
            return std::strftime(output, size, format, &tm->fields);
        });
    BIND(strptime, &ParseAndroidTime);
    BIND(mktime, &LocalTimeInEpoch);
    BIND(sysconf, &Sysconf);
    BIND(getauxval, &GetAuxValue);
    BIND(getenv, &GetEnv);
    BIND(setenv, &SetEnv);
    BIND(unsetenv, &UnsetEnv);
    BIND(
        getpid, +[] {
            return GetCurrentProcessId();
        });
    BIND(
        getpgid, +[](int pid) -> int {
            if (pid != 0 && static_cast<DWORD>(pid) != GetCurrentProcessId()) {
                errno = ESRCH;
                return -1;
            }
            return static_cast<int>(GetCurrentProcessId());
        });
    BIND(
        getgroups, +[](int size, unsigned*) {
            if (size < 0) {
                errno = EINVAL;
                return -1;
            }
            // Windows-hosted Android processes have no supplementary Android groups.
            return 0;
        });
    // Without guest fork, registered atfork handlers have no invocation point.
    BIND(
        pthread_atfork, +[](uintptr_t, uintptr_t, uintptr_t) {
            return 0;
        });
    BIND(
        __register_atfork, +[](uintptr_t, uintptr_t, uintptr_t, void*) {
            return 0;
        });
    // Android executables need a guest process loader. Return the Bionic
    // failure conventions so libraries can handle unavailable helper processes.
    BIND(
        execv, +[](const char*, char* const*) {
            return AndroidUnsupportedOperation();
        });
    BIND(
        execve, +[](const char*, char* const*, char* const*) {
            return AndroidUnsupportedOperation();
        });
    BIND(
        execvp, +[](const char*, char* const*) {
            return AndroidUnsupportedOperation();
        });
    BIND(
        posix_spawn, +[](int*, const char*, const void*, const void*, char* const*, char* const*) {
            return 38;
        });
    BIND(
        posix_spawnp, +[](int*, const char*, const void*, const void*, char* const*, char* const*) {
            return 38;
        });
    BIND(getppid, &ParentProcessId);
    BIND(
        waitpid, +[](int, int*, int) {
            errno = ECHILD;
            return -1;
        });
    BIND(sysinfo, &GetAndroidSystemInfo);
    BIND(
        gettid, +[] {
            return GetCurrentThreadId();
        });
    BIND(
        sched_getcpu, +[] {
            return static_cast<int>(GetCurrentProcessorNumber());
        });
    BIND(
        get_nprocs_conf, +[] {
            return static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        });
    BIND(getrandom, &AndroidGetRandom);
    BIND(getuid, &AndroidApplicationUid);
    BIND(getgid, &AndroidApplicationUid);
    BIND(getegid, &AndroidApplicationUid);
    BIND(getpwuid_r, &GetAndroidPasswd);
    BIND(
        getpwnam_r,
        +[](const char* name, AndroidPasswd* storage, char* strings, size_t capacity, AndroidPasswd** result) {
            if (!name || !storage || !result || (capacity && !strings))
                return EINVAL;
            *result = nullptr;
            return std::strcmp(name, ApplicationUserName) == 0
                       ? GetAndroidPasswd(AndroidApplicationUid(), storage, strings, capacity, result)
                       : 0;
        });
    BIND(getpwuid, &LookupAndroidPasswd);
    BIND(getgrgid, &LookupAndroidGroup);
    BIND(
        getgrnam, +[](const char* name) -> AndroidGroup* {
            if (!name) {
                errno = EINVAL;
                return nullptr;
            }
            return std::strcmp(name, ApplicationUserName) == 0 ? LookupAndroidGroup(AndroidApplicationUid()) : nullptr;
        });
    BIND(getgrgid_r, &GetAndroidGroup);
    BIND(
        getgrnam_r,
        +[](const char* name, AndroidGroup* storage, char* strings, size_t capacity, AndroidGroup** result) {
            if (!name || !storage || !result || (capacity && !strings))
                return EINVAL;
            *result = nullptr;
            return std::strcmp(name, ApplicationUserName) == 0
                       ? GetAndroidGroup(AndroidApplicationUid(), storage, strings, capacity, result)
                       : 0;
        });
    BIND(
        getpwnam, +[](const char* name) -> AndroidPasswd* {
            return name && std::strcmp(name, ApplicationUserName) == 0 ? LookupAndroidPasswd(AndroidApplicationUid())
                                                                       : nullptr;
        });
    BIND(setuid, &SetAndroidIdentity);
    BIND(setgid, &SetAndroidIdentity);
    BIND(seteuid, &SetAndroidIdentity);
    BIND(setegid, &SetAndroidIdentity);
    const auto setIdentityPair = +[](uint32_t real, uint32_t effective) {
        if ((real != UINT32_MAX && SetAndroidIdentity(real) < 0) ||
            (effective != UINT32_MAX && SetAndroidIdentity(effective) < 0))
            return -1;
        return 0;
    };
    BIND(setreuid, setIdentityPair);
    BIND(setregid, setIdentityPair);
    const auto setIdentityTriple = +[](uint32_t real, uint32_t effective, uint32_t saved) {
        const auto uid = AndroidApplicationUid();
        if ((real != UINT32_MAX && real != uid) || (effective != UINT32_MAX && effective != uid) ||
            (saved != UINT32_MAX && saved != uid)) {
            errno = EPERM;
            return -1;
        }
        return 0;
    };
    BIND(setresuid, setIdentityTriple);
    BIND(setresgid, setIdentityTriple);
    BIND(
        capget, +[](void*, void*) {
            return AndroidUnsupportedOperation();
        });
    BIND(
        capset, +[](void*, const void*) {
            return AndroidUnsupportedOperation();
        });
    BIND(
        splice, +[](int, int64_t*, int, int64_t*, size_t, unsigned) -> intptr_t {
            return AndroidUnsupportedOperation();
        });
    BIND(
        tcsendbreak, +[](int, int) {
            return AndroidUnsupportedOperation();
        });
    BIND(
        setgroups, +[](size_t, const uint32_t*) {
            errno = EPERM;
            return -1;
        });
    BIND(
        basename, +[](const char* path) {
            return AndroidPathComponent(path, false);
        });
    BIND(
        dirname, +[](const char* path) {
            return AndroidPathComponent(path, true);
        });
    BIND(
        getprogname, +[]() -> const char* {
            return programName.c_str();
        });
    BIND(
        setprogname, +[](const char* name) {
            if (!name)
                return;
            const auto slash = std::strrchr(name, '/');
            programName = slash ? slash + 1 : name;
        });
    BIND(
        uname, +[](AndroidUtsname* result) {
            return AndroidUname(result, "x86_64");
        });
    BIND(gethostname, &AndroidGetHostname);
    BIND(
        ptrace, +[]() -> int64_t {
            return AndroidUnsupportedOperation();
        });
    BIND(sigaltstack, &AndroidUnsupportedOperation);
    BIND(
        newlocale, +[](int mask, const char* name, _locale_t base) {
            if (mask < 0 || (mask & ~0x1fff) != 0) {
                errno = EINVAL;
                return static_cast<_locale_t>(nullptr);
            }
            if (std::string_view(name) == "C.UTF-8" || std::string_view(name) == "C.utf8")
                name = ".UTF8";
            auto result = _create_locale(LC_ALL, name);
            if (result && base)
                _free_locale(base);
            return result;
        });
    BIND(freelocale, &_free_locale);
    BIND(
        uselocale, +[](_locale_t locale) {
            thread_local _locale_t current = reinterpret_cast<_locale_t>(-1);
            const auto previous = current;
            if (locale)
                current = locale;
            return previous;
        });
    BIND(strtof, &std::strtof);
    BIND(strtod_l, &_strtod_l);
    BIND(strtof_l, &_strtof_l);
    BIND(strtoimax, &std::strtoll);
    BIND(localeconv, &AndroidLocaleConv);
    BIND(wcscoll, &WideCompare);
    BIND(
        wcscoll_l, +[](const uint32_t* first, const uint32_t* second, _locale_t) {
            return WideCompare(first, second);
        });
    BIND(wcsxfrm, &WideTransform);
    BIND(
        wcsxfrm_l, +[](uint32_t* out, const uint32_t* in, size_t size, _locale_t) {
            return WideTransform(out, in, size);
        });
    BIND(wcstod, &WideFloat<_strtod_l>);
    BIND(wcstof, &WideFloat<_strtof_l>);
    BIND(wcstol, &WideInteger<_strtoi64_l>);
    BIND(wcstoll, &WideInteger<_strtoi64_l>);
    BIND(wcstoul, &WideInteger<_strtoui64_l>);
    BIND(wcstoull, &WideInteger<_strtoui64_l>);
    BIND(
        __ctype_get_mb_cur_max, +[] {
            return static_cast<size_t>(MB_CUR_MAX);
        });
#define BIND_WIDE(symbol, function)                                                                                    \
    BIND(symbol, function);                                                                                            \
    BIND(                                                                                                              \
        symbol##_l, +[](uint32_t ch, _locale_t) {                                                                      \
            return (function)(ch);                                                                                     \
        })
    BIND_WIDE(iswalnum, &WideProperty<UCHAR_POSIX_ALNUM>);
    BIND_WIDE(iswalpha, &WideProperty<UCHAR_ALPHABETIC>);
    BIND_WIDE(iswblank, &WideProperty<UCHAR_POSIX_BLANK>);
    BIND_WIDE(iswcntrl, &WideCategory<U_CONTROL_CHAR>);
    BIND_WIDE(iswdigit, &WideCategory<U_DECIMAL_DIGIT_NUMBER>);
    BIND_WIDE(iswgraph, &WideProperty<UCHAR_POSIX_GRAPH>);
    BIND_WIDE(iswlower, &WideProperty<UCHAR_LOWERCASE>);
    BIND_WIDE(iswprint, &WideProperty<UCHAR_POSIX_PRINT>);
    BIND_WIDE(iswpunct, &WidePunctuation);
    BIND_WIDE(iswspace, &WideProperty<UCHAR_WHITE_SPACE>);
    BIND_WIDE(iswupper, &WideProperty<UCHAR_UPPERCASE>);
    BIND_WIDE(iswxdigit, &WideProperty<UCHAR_POSIX_XDIGIT>);
    BIND_WIDE(towlower, &WideCase<u_tolower>);
    BIND_WIDE(towupper, &WideCase<u_toupper>);
#undef BIND_WIDE
    BIND(strtoll_l, &_strtoi64_l);
    BIND(strtoull_l, &_strtoui64_l);
    BIND(mbstowcs, &AndroidMbstowcs);
    BIND(wcstombs, &AndroidWcstombs);
    BIND(
        mbtowc, +[](uint32_t* result, const char* source, size_t size) {
            if (!source)
                return 0;
            wchar_t value = 0;
            const auto count = std::mbtowc(&value, source, size);
            if (count >= 0 && result)
                *result = value;
            return count;
        });
    BIND(
        mbrtowc, +[](uint32_t* result, const char* source, size_t size, void*) -> size_t {
            if (!source)
                return 0;
            if (!size)
                return size_t(-2);
            wchar_t value = 0;
            const int count = std::mbtowc(&value, source, size);
            if (count >= 0 && result)
                *result = value;
            return static_cast<size_t>(count);
        });
    BIND(
        mbrlen, +[](const char* source, size_t size, void*) -> size_t {
            if (!source)
                return 0;
            if (!size)
                return size_t(-2);
            wchar_t value = 0;
            return static_cast<size_t>(std::mbtowc(&value, source, size));
        });
    BIND(
        wcrtomb, +[](char* dst, uint32_t ch, void*) -> size_t {
            if (!dst)
                return 1;
            if (ch > 0xffff) {
                errno = EILSEQ;
                return size_t(-1);
            }
            int count = 0;
            return wctomb_s(&count, dst, MB_CUR_MAX, static_cast<wchar_t>(ch)) == 0 ? static_cast<size_t>(count)
                                                                                    : size_t(-1);
        });
    BIND(
        wcslen, +[](const uint32_t* s) {
            const auto* end = s;
            while (*end)
                ++end;
            return end - s;
        });
    BIND(
        wcschr, +[](const uint32_t* s, uint32_t ch) -> const uint32_t* {
            for (; *s; ++s)
                if (*s == ch)
                    return s;
            return ch == 0 ? s : nullptr;
        });
    BIND(
        wmemchr, +[](const uint32_t* s, uint32_t ch, size_t n) -> const uint32_t* {
            for (size_t i = 0; i < n; ++i)
                if (s[i] == ch)
                    return s + i;
            return nullptr;
        });
    BIND(
        wmemcmp, +[](const uint32_t* left, const uint32_t* right, size_t count) {
            for (size_t index = 0; index < count; ++index) {
                if (left[index] != right[index])
                    return left[index] < right[index] ? -1 : 1;
            }
            return 0;
        });
    BIND(
        wmemcpy, +[](uint32_t* output, const uint32_t* input, size_t count) {
            std::memcpy(output, input, count * sizeof(*output));
            return output;
        });
    BIND(
        wmemmove, +[](uint32_t* output, const uint32_t* input, size_t count) {
            std::memmove(output, input, count * sizeof(*output));
            return output;
        });
    BIND(
        wmemset, +[](uint32_t* output, uint32_t value, size_t count) {
            std::fill_n(output, count, value);
            return output;
        });
    BIND(setlocale, &::setlocale);
    BIND(
        tzset, +[] {
            LocalTimeZone().refresh();
        });
    BIND(time, &std::time);
    BIND(
        difftime, +[](int64_t later, int64_t earlier) {
            // The Windows CRT rejects pre-epoch timestamps. Compute the unsigned
            // magnitude first to preserve small differences and avoid overflow.
            return later >= earlier ? static_cast<double>(uint64_t(later) - uint64_t(earlier))
                                    : -static_cast<double>(uint64_t(earlier) - uint64_t(later));
        });
    BIND(timegm, &TimeInUtc);
    BIND(
        access, +[](const char* path, int mode) {
            if (path && IsOwnProcPath(path, "/cmdline")) {
                if (mode & ~7) {
                    errno = EINVAL;
                    return -1;
                }
                if (mode & 3) {
                    errno = EACCES;
                    return -1;
                }
                return 0;
            }
            const auto access = [&](const wchar_t* wide) {
                return _waccess(wide, mode & 6);
            };
            return (mode & 2) ? WithPathAt<true>(-100, path, access) : WithPathAt(-100, path, access);
        });
    BIND(pathconf, &Pathconf);
    BIND(
        fpathconf, +[](int fd, int key) -> int64_t {
            return WithPathAt(fd, "", [key](const wchar_t* wide) {
                return PathConfiguration(wide, key);
            });
        });
    BIND(isatty, &_isatty);
    BIND(
        tcgetattr, +[](int fd, void*) {
            return TerminalAttributesUnavailable(fd);
        });
    BIND(tcdrain, &TerminalAttributesUnavailable);
    BIND(
        tcflush, +[](int fd, int queue) {
            if (queue < 0 || queue > 2) {
                errno = EINVAL;
                return -1;
            }
            return TerminalAttributesUnavailable(fd);
        });
    BIND(cfgetispeed, &TerminalSpeed);
    BIND(cfgetospeed, &TerminalSpeed);
    BIND(cfsetispeed, &SetTerminalSpeed);
    BIND(cfsetospeed, &SetTerminalSpeed);
    BIND(
        tcsetattr, +[](int fd, int action, const void*) {
            if (action < 0 || action > 2) {
                errno = EINVAL;
                return -1;
            }
            return TerminalAttributesUnavailable(fd);
        });
    BIND(gethostbyname, &GetHostByName);
    BIND(gethostbyaddr, &GetHostByAddress);
    BIND(__get_h_errno, &HostLookupError);
    BIND(
        gai_strerror, +[](int error) {
            switch (error) {
            case 2:
                return "Temporary failure in name resolution";
            case 5:
                return "Address family not supported";
            case 6:
                return "Name resolution allocation failure";
            case 8:
                return "Name or service not known";
            default:
                return "Name resolution failed";
            }
        });
    const auto parseIpv4 = +[](const char* input, uint32_t* address) -> int {
        using Parse = LONG(NTAPI*)(const char*, BOOLEAN, const char**, in_addr*);
        static const auto parse =
            reinterpret_cast<Parse>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlIpv4StringToAddressA"));
        in_addr value{};
        const char* end = nullptr;
        if (!parse || parse(input, FALSE, &end, &value) != 0 || !end)
            return 0;
        while (*end == ' ' || (*end >= '\t' && *end <= '\r'))
            ++end;
        if (*end)
            return 0;
        if (address)
            *address = value.s_addr;
        return 1;
    };
    BIND(inet_aton, parseIpv4);
    // inet_addr cannot distinguish invalid input from 255.255.255.255.
    BIND(
        inet_addr, +[](const char* input) -> uint32_t {
            return ::inet_addr(input);
        });
    BIND(
        inet_pton, +[](int family, const char* input, void* address) {
            const auto result = InetPtonA(HostAddressFamily(family), input, address);
            if (result < 0)
                errno = 97;
            return result;
        });
    BIND(
        inet_ntop, +[](int family, const void* address, char* output, size_t size) {
            const auto result = InetNtopA(HostAddressFamily(family), address, output, size);
            if (!result)
                errno = WSAGetLastError() == WSAEAFNOSUPPORT ? 97 : ENOSPC;
            return result;
        });
    BIND(
        inet_ntoa, +[](uint32_t bits) -> const char* {
            thread_local char address[INET_ADDRSTRLEN];
            return InetNtopA(AF_INET, &bits, address, sizeof(address));
        });
    BIND(if_indextoname, &AndroidInterfaceName);
    BIND(if_nametoindex, &AndroidInterfaceIndex);
    BIND(getifaddrs, &AndroidGetIfAddrs);
    BIND(freeifaddrs, &AndroidFreeIfAddrs);
    BIND(
        sched_yield, +[] {
            SwitchToThread();
            return 0;
        });
    BIND(
        strdup, +[](const char* input) -> char* {
            const auto size = std::strlen(input) + 1;
            auto* copy = static_cast<char*>(AndroidMalloc(size));
            if (copy)
                std::memcpy(copy, input, size);
            return copy;
        });
    BIND(
        strndup, +[](const char* input, size_t limit) -> char* {
            const auto size = strnlen(input, limit);
            auto* copy = static_cast<char*>(AndroidMalloc(size + 1));
            if (copy) {
                std::memcpy(copy, input, size);
                copy[size] = '\0';
            }
            return copy;
        });
    BIND(
        strcasestr, +[](const char* value, const char* key) -> const char* {
            const auto size = std::strlen(key);
            for (; *value; ++value)
                if (!_strnicmp(value, key, size))
                    return value;
            return size ? nullptr : value;
        });
    BIND(memchr, static_cast<const void* (*)(const void*, int, size_t)>(&std::memchr));
    BIND(
        memccpy, +[](void* output, const void* input, int stop, size_t size) -> void* {
            const auto end = static_cast<const unsigned char*>(std::memchr(input, stop, size));
            const auto count = end ? static_cast<size_t>(end - static_cast<const unsigned char*>(input)) + 1 : size;
            if (count)
                std::memcpy(output, input, count);
            return end ? static_cast<unsigned char*>(output) + count : nullptr;
        });
    BIND(qsort, &SortGuest);
    BIND(bsearch, &SearchGuest);
    BIND(malloc_usable_size, &AllocationSize);
    BIND(
        mallopt, +[](int option, int) -> int {
            if (option != -101 && option != -104 && option != -105)
                return 0; // M_PURGE, M_PURGE_ALL, M_PURGE_FAST
            SetLastError(ERROR_SUCCESS);
            return HeapCompact(reinterpret_cast<HANDLE>(_get_heap_handle()), 0) != 0 || GetLastError() == ERROR_SUCCESS;
        });
    BIND(
        android_mallopt, +[](int, void*, size_t) -> bool {
            errno = 95;
            return false;
        });
    BIND(
        malloc_iterate, +[](uintptr_t, size_t, uintptr_t, void*) {
            errno = 95;
            return -1;
        });
    BIND(clone, &AndroidUnsupportedOperation);
    BIND(
        fmemopen, +[](void*, size_t, const char*) -> void* {
            errno = 38;
            return nullptr;
        });
    BIND(android_reset_stack_guards, &ResetAndroidStackGuards);
    BIND(
        android_set_16kb_appcompat_mode, +[](bool) {
            // This policy enables 4-KiB ELF loading on a 16-KiB kernel. Stone's
            // Windows backend already reports and maps native 4-KiB pages.
        });
    BIND(
        malloc_backtrace, +[](void*, uintptr_t*, size_t) -> intptr_t {
            // Stone's allocator does not enable per-allocation backtrace capture.
            return 0;
        });
    BIND(
        malloc_disable, +[] {
            // Match realloc/bookkeeping lock order. Queries by this same thread
            // remain valid while guest allocation is disabled.
            nativeBlocks.mutex.lock();
            if (!HeapLock(reinterpret_cast<HANDLE>(_get_heap_handle())))
                std::abort();
        });
    BIND(
        malloc_enable, +[] {
            if (!HeapUnlock(reinterpret_cast<HANDLE>(_get_heap_handle())))
                std::abort();
            nativeBlocks.mutex.unlock();
        });
    BIND(
        malloc_info, +[](int options, void* output) -> int {
            if (options || !output) {
                errno = EINVAL;
                return -1;
            }
            AndroidMallInfo info{};
            if (!ReadHeapStatistics(&info))
                return -1;
            return std::fprintf(HostFile(output),
                                "<malloc version=\"1\"><heap nr=\"0\">"
                                "<total type=\"rest\" count=\"%llu\" size=\"%llu\"/>"
                                "<system type=\"current\" size=\"%llu\"/></heap></malloc>\n",
                                info.ordblks, info.fordblks, info.arena) < 0
                       ? -1
                       : 0;
        });
    // The SysV caller supplies the address of the 80-byte return value in RDI.
    // The pointer result also supplies the required return value in RAX.
    if (!hostAbi) {
        BIND(mallinfo, &ReadHeapStatistics);
        BIND(mallinfo2, &ReadHeapStatistics);
    }
    BIND(
        aligned_alloc, +[](size_t alignment, size_t size) -> void* {
            if (!alignment || size % alignment) {
                errno = EINVAL;
                return nullptr;
            }
            return AllocateAligned(alignment, size);
        });
    BIND(
        memalign, +[](size_t alignment, size_t size) -> void* {
            if (alignment > PTRDIFF_MAX) {
                errno = ENOMEM;
                return nullptr;
            }
            return AllocateAligned(std::bit_ceil(alignment), size);
        });
    BIND(posix_memalign, &PosixMemalign);
    BIND(strspn, &std::strspn);
    BIND(strcspn, &std::strcspn);
    BIND(strnlen, &strnlen);
    BIND(
        strncpy, +[](char* out, const char* in, size_t n) {
            const auto count = strnlen(in, n);
            std::memcpy(out, in, count);
            std::memset(out + count, 0, n - count);
            return out;
        });
    BIND(
        strcat, +[](char* out, const char* in) {
            std::memcpy(out + std::strlen(out), in, std::strlen(in) + 1);
            return out;
        });
    BIND(
        strncat, +[](char* out, const char* in, size_t bound) {
            const auto offset = std::strlen(out);
            const auto count = strnlen(in, bound);
            std::memcpy(out + offset, in, count);
            out[offset + count] = '\0';
            return out;
        });
    BIND(
        strcpy, +[](char* out, const char* in) {
            return std::memcpy(out, in, std::strlen(in) + 1);
        });
    BIND(
        stpcpy, +[](char* out, const char* in) {
            const auto length = std::strlen(in);
            std::memcpy(out, in, length + 1);
            return out + length;
        });
    BIND(
        getpagesize, +[] {
            return static_cast<int>(Sysconf(0x27));
        });
    BIND(getrlimit, &GetResourceLimit);
    BIND(getrlimit64, &GetResourceLimit);
    BIND(setrlimit, &SetResourceLimit);
    BIND(setrlimit64, &SetResourceLimit);
    BIND(
        sched_setaffinity, +[](int, size_t, const void*) {
            errno = 38;
            return -1;
        });
    BIND(
        prctl, +[](int) {
            errno = 38;
            return -1;
        });
    if (name == "syscall") {
        // The syscall number shifts Linux's sixth argument onto the SysV stack.
        const auto handler = +[](int64_t number, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e,
                                 uintptr_t f) -> int64_t {
            if (number == 158) { // arch_prctl.
                auto* tls = ElfImage::GuestThreadPointer();
                if (a == 0x1001) { // ARCH_SET_GS, kept in Stone's per-thread guest state.
                    if (b > 0x00007fffffffffff) {
                        errno = EPERM;
                        return -1;
                    }
                    tls[AndroidArtThreadSlot] = b;
                    return 0;
                }
                if (a == 0x1004) { // ARCH_GET_GS.
                    SIZE_T written = 0;
                    if (WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(b), &tls[AndroidArtThreadSlot],
                                           sizeof(uintptr_t), &written) &&
                        written == sizeof(uintptr_t))
                        return 0;
                    errno = EFAULT;
                    return -1;
                }
                return AndroidUnsupportedOperation();
            }
            if (number == 186)
                return GetCurrentThreadId();
            if (number == 318)
                return AndroidGetRandom(reinterpret_cast<unsigned char*>(a), b, static_cast<unsigned>(c));
            if (number == 319)
                return CreateMemoryFile(reinterpret_cast<const char*>(a), static_cast<unsigned>(b));
            if (number != 202)
                return AndroidUnsupportedOperation();

            const auto result = AndroidX64Syscall(202, a, b, c, d, e, f);
            if (result >= 0)
                return result;
            errno = static_cast<int>(-result);
            return -1;
        };
        return hostAbi ? reinterpret_cast<uintptr_t>(handler) : Import(name, handler, AndroidToWindowsWide);
    }
    BIND(memfd_create, &CreateMemoryFile);
    BIND(
        arc4random_buf, +[](unsigned char* buffer, size_t size) {
            while (size) {
                const auto count = static_cast<ULONG>(std::min<size_t>(size, ULONG_MAX));
                if (BCryptGenRandom(nullptr, buffer, count, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
                    std::abort();
                buffer += count;
                size -= count;
            }
        });
    BIND(
        arc4random, +[]() -> uint32_t {
            uint32_t value;
            if (BCryptGenRandom(nullptr, reinterpret_cast<unsigned char*>(&value), sizeof(value),
                                BCRYPT_USE_SYSTEM_PREFERRED_RNG))
                std::abort();
            return value;
        });
    BIND(android_fdsan_create_owner_tag, &MakeFdOwnerTag);
    BIND(
        android_fdsan_get_tag_type, +[](uint64_t tag) {
            return static_cast<unsigned>(tag >> 56);
        });
    BIND(
        android_fdsan_get_tag_value, +[](uint64_t tag) {
            return tag & FdOwnerValueMask;
        });
    BIND(android_fdsan_get_owner_tag, &FdOwnerTag);
    BIND(android_fdsan_exchange_owner_tag, &ExchangeFdOwnerTag);
    BIND(android_fdsan_close_with_tag, &CloseTaggedDescriptor);
    BIND(
        android_fdsan_get_error_level, +[] {
            return fdSanLevel.load();
        });
    BIND(
        android_fdsan_set_error_level, +[](int level) {
            return level >= FdSanDisabled && level <= FdSanFatal ? fdSanLevel.exchange(level) : fdSanLevel.load();
        });
    BIND(
        close, +[](int fd) {
            return CloseTaggedDescriptor(fd, 0);
        });
    BIND(read, &ReadAndroidFd);
    BIND(fdatasync, &SyncDescriptor);
    BIND(
        fdopen, +[](int fd, const char* mode) -> FILE* {
            // A CRT stream can flush outside the guest write hooks, after seals have been added.
            if (FindMemoryFile(fd)) {
                errno = 38;
                return nullptr;
            }
            return TagFileStream(_fdopen(fd, mode));
        });
    BIND(open, &OpenDescriptor);
    BIND(
        mkstemp, +[](char* path) {
            return MakeTempPath(path, 0, [](const char* name) {
                return OpenDescriptor(name, 2 | 0x40 | 0x80, 0600);
            });
        });
    BIND(
        mkstemps, +[](char* path, int suffixLength) {
            if (suffixLength < 0) {
                errno = EINVAL;
                return -1;
            }
            return MakeTempPath(path, static_cast<size_t>(suffixLength), [](const char* name) {
                return OpenDescriptor(name, 2 | 0x40 | 0x80, 0600);
            });
        });
    BIND(
        mkostemps, +[](char* path, int suffixLength, int flags) {
            if (suffixLength < 0) {
                errno = EINVAL;
                return -1;
            }
            return MakeTempPath(path, static_cast<size_t>(suffixLength), [flags](const char* name) {
                return OpenDescriptor(name, flags | 2 | 0x40 | 0x80, 0600);
            });
        });
    BIND(
        process_vm_readv,
        +[](int pid, const AndroidIoVector* local, size_t localCount, const AndroidIoVector* remote, size_t remoteCount,
            unsigned long long flags) -> intptr_t {
            if (flags || localCount > 1024 || remoteCount > 1024) {
                errno = EINVAL;
                return -1;
            }
            if ((!local && localCount) || (!remote && remoteCount)) {
                errno = EFAULT;
                return -1;
            }
            if (static_cast<DWORD>(pid) != GetCurrentProcessId()) {
                errno = EPERM;
                return -1;
            }
            size_t destinationIndex = 0, sourceIndex = 0, destinationOffset = 0, sourceOffset = 0;
            intptr_t total = 0;
            while (destinationIndex < localCount && sourceIndex < remoteCount) {
                const auto& destination = local[destinationIndex];
                const auto& source = remote[sourceIndex];
                const size_t count = std::min(destination.size - destinationOffset, source.size - sourceOffset);
                if (count > static_cast<size_t>(INTPTR_MAX - total)) {
                    errno = EINVAL;
                    return total ? total : -1;
                }
                SIZE_T copied = 0;
                const bool read =
                    !count ||
                    ReadProcessMemory(
                        GetCurrentProcess(),
                        reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(source.data) + sourceOffset),
                        reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(destination.data) + destinationOffset),
                        count, &copied);
                total += static_cast<intptr_t>(copied);
                if (!read) {
                    errno = EFAULT;
                    return total ? total : -1;
                }
                destinationOffset += count;
                sourceOffset += count;
                if (destinationOffset == destination.size) {
                    ++destinationIndex;
                    destinationOffset = 0;
                }
                if (sourceOffset == source.size) {
                    ++sourceIndex;
                    sourceOffset = 0;
                }
            }
            return total;
        });
    BIND(
        pidfd_open, +[](int, unsigned) {
            return AndroidUnsupportedOperation();
        });
    BIND(
        unshare, +[](int) {
            return AndroidUnsupportedOperation();
        });
    BIND(
        waitid, +[](int, unsigned, void*, int) {
            return AndroidUnsupportedOperation();
        });
    BIND(
        mkdtemp, +[](char* path) {
            return MakeTempPath(path, 0,
                                [](const char* name) {
                                    return WithPathAt<true>(-100, name, &_wmkdir);
                                }) == 0
                       ? path
                       : nullptr;
        });
    BIND(
        mknod, +[](const char* path, unsigned mode, uint64_t) {
            constexpr unsigned Type = 0170000, Regular = 0100000, Fifo = 0010000;
            const auto type = mode & Type;
            if (type == Fifo)
                return AndroidUnsupportedOperation();
            if (type && type != Regular) {
                errno = EPERM;
                return -1;
            }
            const auto fd = OpenDescriptor(path, 1 | 0x40 | 0x80, mode & 0777);
            return fd < 0 ? -1 : CloseAndroidFd(fd);
        });
    BIND(
        mkfifo, +[](const char*, unsigned) {
            return AndroidUnsupportedOperation();
        });
    BIND(flock, &LockDescriptor);
    BIND(
        creat, +[](const char* path, int permission) {
            constexpr int WriteOnly = 1, Create = 0x40, Truncate = 0x200;
            return OpenDescriptor(path, WriteOnly | Create | Truncate, permission);
        });
    BIND(pread, &TransferAt<false>);
    BIND(pwrite, &TransferAt<true>);
    BIND(pread64, &TransferAt<false>);
    BIND(pwrite64, &TransferAt<true>);
    BIND(
        __open_2, +[](const char* path, int flags) {
            return OpenDescriptor(path, flags, 0);
        });
    BIND(openat, &OpenAt);
    BIND(
        __openat_2, +[](int fd, const char* path, int flags) {
            return OpenAt(fd, path, flags, 0);
        });
    BIND(fsync, &SyncDescriptor);
    BIND(
        dup, +[](int source) {
            return DuplicateDescriptor(source, 0, false);
        });
    BIND(dup2, &DuplicateDescriptor2);
    BIND(dup3, &DuplicateDescriptor3);
    BIND(futimens, &SetDescriptorTimes);
    BIND(futimes, &SetDescriptorMicrotimes);
    BIND(posix_fadvise, &AdviseDescriptor);
    BIND(posix_fadvise64, &AdviseDescriptor);
    BIND(sendfile, &SendFile);
    BIND(sendfile64, &SendFile);
    BIND(
        fchmod, +[](int fd, int mode) {
            std::unique_lock<std::mutex> lock;
            if (const auto memory = LockMemoryFile(fd, lock)) {
                if ((memory->seals & SealExec) && ((memory->mode ^ mode) & 0111)) {
                    errno = EPERM;
                    return -1;
                }
                memory->mode = mode & 0777;
                return 0;
            }
            const auto file = FileDescriptorHandle(fd);
            if (file == INVALID_HANDLE_VALUE)
                return -1;
            if (WithPathAt<true>(fd, "",
                                 [](const wchar_t*) {
                                     return 0;
                                 }) < 0 &&
                errno == EROFS)
                return -1;
            FILE_BASIC_INFO info{};
            if (file != INVALID_HANDLE_VALUE &&
                GetFileInformationByHandleEx(file, FileBasicInfo, &info, sizeof(info))) {
                if (mode & 0200)
                    info.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
                else
                    info.FileAttributes |= FILE_ATTRIBUTE_READONLY;
                if (!info.FileAttributes)
                    info.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                if (SetFileInformationByHandle(file, FileBasicInfo, &info, sizeof(info)))
                    return 0;
            }
            errno = EACCES;
            return -1;
        });
    BIND(
        fchown, +[](int, unsigned, unsigned) {
            errno = EPERM;
            return -1;
        });
    BIND(
        chown, +[](const char*, unsigned, unsigned) {
            errno = EPERM;
            return -1;
        });
    BIND(
        lchown, +[](const char*, unsigned, unsigned) {
            errno = EPERM;
            return -1;
        });
    BIND(stat64, &StatPath<>);
    BIND(link, &CreateGuestLink<false>);
    BIND(symlink, &CreateGuestLink<true>);
    BIND(statfs, &StatFs);
    BIND(statfs64, &StatFs);
    BIND(fstatfs, &FstatFs);
    BIND(fstatfs64, &FstatFs);
    const auto statVfs = +[](const char* path, AndroidStatVfs* result) {
        return StatVfs(
            [&](AndroidStatFs* stats) {
                return StatFs(path, stats);
            },
            result);
    };
    const auto fstatVfs = +[](int fd, AndroidStatVfs* result) {
        return StatVfs(
            [&](AndroidStatFs* stats) {
                return FstatFs(fd, stats);
            },
            result);
    };
    BIND(statvfs, statVfs);
    BIND(statvfs64, statVfs);
    BIND(fstatvfs, fstatVfs);
    BIND(fstatvfs64, fstatVfs);
    BIND(geteuid, &AndroidApplicationUid);
    BIND(
        chmod, +[](const char* path, int mode) {
            return ChmodAt(-100, path, mode, 0);
        });
    BIND(fchmodat, &ChmodAt);
    BIND(
        utime, +[](const char* path, const AndroidUtime* times) {
            return WithPathAt<true>(-100, path, [&](const wchar_t* name) {
                __utimbuf64 value{times ? times->accessed : 0, times ? times->modified : 0};
                return _wutime64(name, times ? &value : nullptr);
            });
        });
    BIND(
        mkdirat, +[](int fd, const char* path, int) {
            return WithPathAt<true>(fd, path, &_wmkdir, false);
        });
    BIND(
        faccessat, +[](int fd, const char* path, int mode, int flags) {
            if (flags) {
                errno = EINVAL;
                return -1;
            }
            const auto access = [&](const wchar_t* wide) {
                return _waccess(wide, mode & 6);
            };
            return (mode & 2) ? WithPathAt<true>(fd, path, access) : WithPathAt(fd, path, access);
        });
    BIND(
        unlinkat, +[](int fd, const char* path, int flags) {
            if (flags & ~0x200) {
                errno = EINVAL;
                return -1;
            }
            if (flags == 0)
                return WithPathAt<true>(
                    fd, path,
                    [](const wchar_t* resolved) {
                        return RemoveGuestPath(resolved, false);
                    },
                    false);
            return WithPathAt<true>(fd, path, &_wrmdir, false);
        });
    BIND(renameat, &RenameAt);
    BIND(
        utimes, +[](const char*, const void*) {
            errno = 38;
            return -1;
        });
    BIND(readlink, &ReadGuestLink);
    BIND(
        __readlink_chk, +[](const char* path, char* output, size_t count, size_t capacity) {
            if (count > capacity)
                std::abort();
            return ReadGuestLink(path, output, count);
        });
    BIND(
        mremap, +[](void*, size_t, size_t, int) -> void* {
            errno = 38;
            return reinterpret_cast<void*>(-1);
        });
    BIND(
        localtime, +[](const time_t* seconds) {
            thread_local AndroidTm tm{};
            return BrokenDownTime<true>(seconds, &tm);
        });
    BIND(
        lstat, +[](const char* path, AndroidStat* result) {
            return StatAt(-100, path, result, 0x100);
        });
    BIND(
        unlink, +[](const char* path) {
            return WithPathAt<true>(
                -100, path,
                [](const wchar_t* resolved) {
                    return RemoveGuestPath(resolved, false);
                },
                false);
        });
    BIND(
        rmdir, +[](const char* path) {
            return WithPathAt<true>(-100, path, &_wrmdir, false);
        });
    BIND(ftruncate, &TruncateDescriptor);
    BIND(ftruncate64, &TruncateDescriptor);
    BIND(fallocate, &AllocateFileRange);
    BIND(fallocate64, &AllocateFileRange);
    const auto posixAllocate = +[](int fd, int64_t offset, int64_t length) {
        const int saved = errno;
        const int error = AllocateFileRange(fd, 0, offset, length) == 0 ? 0 : errno;
        errno = saved;
        return error;
    };
    BIND(posix_fallocate, posixAllocate);
    BIND(posix_fallocate64, posixAllocate);
    // A guest application cannot mount or unmount host filesystems.
    BIND(
        mount, +[](const char*, const char*, const char*, uint64_t, const void*) {
            errno = EPERM;
            return -1;
        });
    BIND(
        umount2, +[](const char*, int) {
            errno = EPERM;
            return -1;
        });
    BIND(truncate, &TruncatePath);
    BIND(truncate64, &TruncatePath);
    BIND(lseek, &SeekDescriptor);
    BIND(lseek64, &SeekDescriptor);
    BIND(
        chdir, +[](const char* path) {
            return WithPathAt(-100, path, &_wchdir);
        });
    BIND(realpath, &Realpath);
    BIND(pipe2, &GuestPipe);
    BIND(mmap, &MapMemory<>);
    BIND(mmap64, &MapMemory<>);
    BIND(munmap, &UnmapMemory<>);
    BIND(mprotect, &ProtectMemory<>);
    BIND(madvise, hostAbi ? &AdviseMemory<true> : &AdviseMemory<>);
    BIND(msync, &SyncMemory<>);
    if (!hostAbi)
        BIND(mincore, &QueryPageResidency);
    BIND(
        mlock, +[](const void* address, size_t size) {
            if (!size || VirtualLock(const_cast<void*>(address), size))
                return 0;
            errno = GetLastError() == ERROR_WORKING_SET_QUOTA ? ENOMEM : EINVAL;
            return -1;
        });
    BIND(
        munlock, +[](const void* address, size_t size) {
            if (!size || VirtualUnlock(const_cast<void*>(address), size) || GetLastError() == ERROR_NOT_LOCKED)
                return 0;
            errno = EINVAL;
            return -1;
        });
    BIND(
        pipe, +[](int* fds) {
            return GuestPipe(fds, 0);
        });
    BIND(write, &WriteDescriptor);
    BIND(readv, &TransferVectors<false>);
    BIND(writev, &TransferVectors<true>);
    BIND(
        __write_chk, +[](int fd, const void* buffer, size_t size, size_t bound) {
            if (size > bound)
                std::abort();
            return WriteDescriptor(fd, buffer, size);
        });
    BIND(
        __read_chk, +[](int fd, void* buffer, size_t size, size_t bound) {
            if (size > bound)
                std::abort();
            return ReadAndroidFd(fd, buffer, size);
        });
    BIND(
        __pread_chk, +[](int fd, void* buffer, size_t size, int64_t offset, size_t bound) {
            if (size > bound)
                std::abort();
            return TransferAt<false>(fd, buffer, size, offset);
        });
    BIND(
        __pwrite_chk, +[](int fd, void* buffer, size_t size, int64_t offset, size_t bound) {
            if (size > bound)
                std::abort();
            return TransferAt<true>(fd, buffer, size, offset);
        });
    BIND(
        fcntl, +[](int fd, int command, uintptr_t argumentValue) {
            if (command >= 5 && command <= 7)
                return DescriptorByteLock(fd, command, reinterpret_cast<AndroidFileRange*>(argumentValue));
            const auto argument = static_cast<int>(argumentValue);
            if (command == 0 || command == 1030)
                return DuplicateDescriptor(fd, argument, command == 1030);
            int result;
            if (AndroidSocketFlags(fd, command, argument, result))
                return result;
            if (AndroidInotifyFlags(fd, command, argument, result))
                return result;
            if (AndroidEventCounterFlags(fd, command, argument, result))
                return result;
            std::unique_lock<std::mutex> lock;
            const auto memory = LockMemoryFile(fd, lock);
            const auto handle = FileDescriptorHandle(fd);
            if (handle == INVALID_HANDLE_VALUE)
                return -1;
            if (memory) {
                switch (command) {
                case 3:
                    return 2 | 0x8000 | memory->status; // F_GETFL: O_RDWR | O_LARGEFILE
                case 4:
                    memory->status = argument & (0x400 | 0x800);
                    return 0; // O_APPEND | O_NONBLOCK
                case 1033:
                    return SetMemoryFileSeals(*memory, static_cast<unsigned>(argument));
                case 1034:
                    return static_cast<int>(memory->seals);
                }
            }
            if (command == 1) {
                DWORD flags = 0;
                if (GetHandleInformation(handle, &flags))
                    return (flags & HANDLE_FLAG_INHERIT) ? 0 : 1;
            }
            else if (command == 2) {
                const DWORD inherit = (argument & 1) ? 0 : HANDLE_FLAG_INHERIT;
                if (SetHandleInformation(handle, HANDLE_FLAG_INHERIT, inherit))
                    return 0;
            }
            if (command == 3 && GetFileType(handle) == FILE_TYPE_PIPE) {
                DWORD mode = 0;
                if (GetNamedPipeHandleStateW(handle, &mode, nullptr, nullptr, nullptr, nullptr, 0))
                    return (mode & PIPE_NOWAIT) ? 0x800 : 0;
            }
            else if (command == 4 && GetFileType(handle) == FILE_TYPE_PIPE) {
                DWORD mode = (argument & 0x800) ? PIPE_NOWAIT : PIPE_WAIT;
                if (SetNamedPipeHandleState(handle, &mode, nullptr, nullptr))
                    return 0;
            }
            if (command == 3 && RegularDiskFile(handle)) {
                const auto access = FileHandleAccess(handle);
                if (access & (FILE_WRITE_DATA | FILE_APPEND_DATA)) {
                    return ((access & FILE_READ_DATA) ? 2 : 1) | ((access & FILE_WRITE_DATA) ? 0 : 0x400) | 0x8000;
                }
                if (access & FILE_READ_DATA)
                    return 0x8000;
            }
            if (command == 3 && fd >= 0 && fd <= 2)
                return fd ? 1 : 0; // standard output/error are write-only
            errno = EINVAL;
            return -1;
        });
    BIND(fopen, &OpenFile);
    BIND(setmntent, &OpenFile);
    BIND(getmntent, &ReadMountEntry);
    BIND(
        getmntent_r, +[](void* stream, AndroidMountEntry* output, char* buffer, int size) -> AndroidMountEntry* {
            if (!output || !buffer || size < 0) {
                errno = EINVAL;
                return nullptr;
            }
            const auto* entry = ReadMountEntry(stream);
            if (!entry)
                return nullptr;
            const char* fields[]{entry->device, entry->directory, entry->type, entry->options};
            size_t required = 0;
            for (const auto* field : fields)
                required += std::strlen(field) + 1;
            if (required > static_cast<size_t>(size)) {
                errno = ERANGE;
                return nullptr;
            }
            *output = *entry;
            char** targets[]{&output->device, &output->directory, &output->type, &output->options};
            for (size_t i = 0; i < std::size(fields); ++i) {
                const auto count = std::strlen(fields[i]) + 1;
                std::memcpy(buffer, fields[i], count);
                *targets[i] = buffer;
                buffer += count;
            }
            return output;
        });
    BIND(
        endmntent, +[](void* file) {
            if (file)
                CloseFileStream(file);
            return 1;
        });
    BIND(fclose, &CloseFileStream);
    BIND(tmpfile, &OpenTemporaryStream);
    BIND(tmpfile64, &OpenTemporaryStream);
    BIND(__fsetlocking, &SetStreamLocking);
    BIND(
        flockfile, +[](void* f) {
            _lock_file(HostFile(f));
        });
    BIND(
        funlockfile, +[](void* f) {
            _unlock_file(HostFile(f));
        });
    BIND(
        fflush, +[](void* f) {
            auto* file = HostFile(f);
            return CallerLocksStream(file) ? _fflush_nolock(file) : std::fflush(file);
        });
    BIND(
        fileno, +[](void* f) {
            return _fileno(HostFile(f));
        });
    BIND(
        clearerr, +[](void* f) {
            std::clearerr(HostFile(f));
        });
    BIND(
        rewind, +[](void* f) {
            std::rewind(HostFile(f));
        });
    BIND(
        ferror, +[](void* f) {
            return std::ferror(HostFile(f));
        });
    BIND(
        feof, +[](void* f) {
            return std::feof(HostFile(f));
        });
    BIND(
        fputs, +[](const char* p, void* f) {
            return std::fputs(p, HostFile(f));
        });
    BIND(fputc, &WriteStreamByte);
    BIND(putc, &WriteStreamByte);
    BIND(
        putchar, +[](int c) {
            return WriteStreamByte(c, stdout);
        });
    const auto readStreamByte = +[](void* f) {
        auto* file = HostFile(f);
        return CallerLocksStream(file) ? _fgetc_nolock(file) : std::fgetc(file);
    };
    BIND(getc, readStreamByte);
    BIND(fgetc, readStreamByte);
    BIND(
        getchar, +[] {
            return CallerLocksStream(stdin) ? _fgetc_nolock(stdin) : std::fgetc(stdin);
        });
    BIND(
        ungetc, +[](int ch, void* f) {
            return std::ungetc(ch, HostFile(f));
        });
    BIND(
        getwc, +[](void* f) -> uint32_t {
            const auto result = std::fgetwc(HostFile(f));
            return result == WEOF ? UINT32_MAX : result;
        });
    BIND(
        ungetwc, +[](uint32_t ch, void* f) -> uint32_t {
            if (ch >= WEOF)
                return UINT32_MAX;
            const auto result = std::ungetwc(static_cast<wint_t>(ch), HostFile(f));
            return result == WEOF ? UINT32_MAX : result;
        });
    BIND(
        fputwc, +[](uint32_t ch, void* f) -> uint32_t {
            if (ch >= WEOF)
                return UINT32_MAX;
            const auto result = std::fputwc(static_cast<wchar_t>(ch), HostFile(f));
            return result == WEOF ? UINT32_MAX : result;
        });
    BIND(
        setbuf, +[](void* f, char* buffer) {
            std::setvbuf(HostFile(f), buffer, buffer ? _IOFBF : _IONBF, BUFSIZ);
        });
    if (hostAbi && name == "setvbuf")
        return reinterpret_cast<uintptr_t>(+[](void* f, int mode, size_t size) {
            return std::setvbuf(HostFile(f), nullptr, mode, size);
        });
    BIND(
        setvbuf, +[](void* f, char* buffer, int mode, size_t size) {
            constexpr int modes[]{_IOFBF, _IOLBF, _IONBF};
            if (mode < 0 || mode >= static_cast<int>(std::size(modes))) {
                errno = EINVAL;
                return -1;
            }
            return std::setvbuf(HostFile(f), buffer, modes[mode], size);
        });
    BIND(puts, &std::puts);
    BIND(rand, &std::rand);
    BIND(srand, &std::srand);
    BIND(perror, &std::perror);
    BIND(lrand48, &NextRandom48);
    BIND(nrand48, &Random48FromSeed);
    BIND(
        srand48, +[](int64_t seed) {
            random48.store((uint64_t(uint32_t(seed)) << 16) | 0x330e);
        });
    BIND(div, static_cast<std::div_t (*)(int, int)>(&std::div));
    BIND(
        fgets, +[](char* p, int n, void* f) {
            return std::fgets(p, n, HostFile(f));
        });
    BIND(getdelim, &ReadDelimitedLine);
    BIND(
        getline, +[](char** line, size_t* capacity, void* file) {
            return ReadDelimitedLine(line, capacity, '\n', file);
        });
    BIND(fread, &ReadFileStream);
    BIND(
        __fread_chk, +[](void* data, size_t size, size_t count, void* file, size_t bound) {
            if (size && count > bound / size)
                std::abort();
            return ReadFileStream(data, size, count, file);
        });
    BIND(
        fwrite, +[](const void* p, size_t s, size_t n, void* f) {
            auto* file = HostFile(f);
            return CallerLocksStream(file) ? _fwrite_nolock(p, s, n, file) : std::fwrite(p, s, n, file);
        });
    BIND(
        __fwrite_chk, +[](const void* p, size_t size, size_t count, void* file, size_t bound) {
            if (size && count > bound / size)
                std::abort();
            return std::fwrite(p, size, count, HostFile(file));
        });
    BIND(vfprintf, &StoneAndroidPrint);
    BIND(
        vfscanf, +[](void* file, const char* format, AndroidVaList* args) {
            return ScanArguments(
                nullptr, format,
                [&](size_t) {
                    return args->next();
                },
                HostFile(file));
        });
    BIND(vprintf, &StoneAndroidStdout);
    BIND(vdprintf, &StoneAndroidPrintDescriptor);
    BIND(
        fseek, +[](void* f, int64_t offset, int mode) {
            return _fseeki64(HostFile(f), offset, mode);
        });
    BIND(
        ftell, +[](void* f) {
            return _ftelli64(HostFile(f));
        });
    BIND(
        fseeko, +[](void* f, int64_t offset, int mode) {
            return _fseeki64(HostFile(f), offset, mode);
        });
    BIND(
        ftello, +[](void* f) {
            return _ftelli64(HostFile(f));
        });
    BIND(
        fseeko64, +[](void* f, int64_t offset, int mode) {
            return _fseeki64(HostFile(f), offset, mode);
        });
    BIND(
        ftello64, +[](void* f) {
            return _ftelli64(HostFile(f));
        });
    BIND(vsnprintf, &AndroidVsnprintf);
    BIND(vsscanf, &AndroidVsscanf);
    if (!hostAbi) {
        BIND(vswprintf, &AndroidVswprintf);
    }
    BIND(
        __vsnprintf_chk, +[](char* output, size_t size, int, size_t bound, const char* format, AndroidVaList* args) {
            if (size > bound)
                std::abort();
            return AndroidVsnprintf(output, size, format, args);
        });
    BIND(
        __vsprintf_chk, +[](char* output, int, size_t bound, const char* format, AndroidVaList* args) {
            const int length = AndroidVsnprintf(output, bound, format, args);
            if (length >= 0 && static_cast<size_t>(length) >= bound)
                std::abort();
            return length;
        });
    BIND(vsprintf, &StoneAndroidUnboundedFormat);
    BIND(vasprintf, &StoneAndroidAllocateFormat);
    BIND(getcwd, &CurrentDirectory);
    BIND(
        mkdir, +[](const char* path) {
            return WithPathAt<true>(-100, path, &_wmkdir, false);
        });
    BIND(
        remove, +[](const char* path) {
            return WithPathAt<true>(
                -100, path,
                [](const wchar_t* resolved) {
                    return RemoveGuestPath(resolved, true);
                },
                false);
        });
    BIND(
        rename, +[](const char* from, const char* to) {
            return RenameAt(-100, from, -100, to);
        });
    BIND(stat, &StatPath<>);
    BIND(fstat, &StatFile<>);
    BIND(statx, &Statx);
    BIND(
        getxattr, +[](const char* p, const char* n, void* v, size_t s) {
            return FileXattr<true, false>(p, n, v, s);
        });
    BIND(
        lgetxattr, +[](const char* p, const char* n, void* v, size_t s) {
            return FileXattr<false, false>(p, n, v, s);
        });
    BIND(
        fgetxattr, +[](int fd, const char* n, void* v, size_t s) {
            return FileXattr<true, false>(fd, n, v, s);
        });
    BIND(
        setxattr, +[](const char* p, const char* n, const void* v, size_t s, int f) {
            return static_cast<int>(FileXattr<true, true>(p, n, v, s, f));
        });
    BIND(
        lsetxattr, +[](const char* p, const char* n, const void* v, size_t s, int f) {
            return static_cast<int>(FileXattr<false, true>(p, n, v, s, f));
        });
    BIND(
        fsetxattr, +[](int fd, const char* n, const void* v, size_t s, int f) {
            return static_cast<int>(FileXattr<true, true>(fd, n, v, s, f));
        });
    BIND(
        listxattr, +[](const char* path, char* output, size_t size) {
            return FileXattr<true, false, true>(path, "", output, size);
        });
    BIND(
        llistxattr, +[](const char* path, char* output, size_t size) {
            return FileXattr<false, false, true>(path, "", output, size);
        });
    BIND(
        removexattr, +[](const char* path, const char* name) {
            return static_cast<int>(FileXattr<true, true>(path, name, nullptr, 0));
        });
    BIND(fts_open, &OpenFts);
    BIND(fts_read, &ReadFts);
    BIND(fts_set, &SetFtsInstruction);
    BIND(fts_close, &CloseFts);
    BIND(fstatat, &StatAt<>);
    BIND(
        lstat64, +[](const char* path, AndroidStat* result) {
            return StatAt(-100, path, result, 0x100);
        });
    BIND(fstat64, &StatFile<>);
    BIND(fstatat64, &StatAt<>);
    BIND(opendir, &OpenDir);
    BIND(readdir, &ReadDir);
    BIND(closedir, &CloseDir);
    if (!hostAbi) {
        BIND(nftw, &WalkTree);
        BIND(nftw64, &WalkTree);
    }
    BIND(scandir, &ScanDirectory);
    BIND(scandir64, &ScanDirectory);
    BIND(readdir64, &ReadDir);
    BIND(readdir_r, &ReadDirReentrant);
    BIND(readdir64_r, &ReadDirReentrant);
    BIND(fdopendir, &OpenFdDir);
    BIND(
        dirfd, +[](const AndroidDir* dir) {
            return dir->fd;
        });
    BIND(
        rewinddir, +[](AndroidDir* dir) {
            const std::lock_guard lock(dir->mutex);
            if (dir->imageEntries.empty()) {
                _findclose(dir->handle);
                dir->handle = _wfindfirst64(dir->pattern.c_str(), &dir->data);
            }
            dir->first = true;
            dir->entry = {};
        });
    BIND(
        inotify_init, +[] {
            return CreateAndroidInotify(0);
        });
    BIND(inotify_init1, &CreateAndroidInotify);
    BIND(inotify_rm_watch, &RemoveAndroidInotifyWatch);
    BIND(
        inotify_add_watch, +[](int fd, const char* path, uint32_t mask) {
            try {
                return AddAndroidInotifyWatch(fd, GuestPath(path, false, (mask & 0x02000000) == 0), mask);
            }
            catch (const AndroidSystemPathError& error) {
                errno = error.code;
                return -1;
            }
            catch (const std::filesystem::filesystem_error&) {
                errno = EIO;
                return -1;
            }
            catch (const std::bad_alloc&) {
                errno = ENOMEM;
                return -1;
            }
        });
#undef BIND
    if (const auto socketImport = AndroidSocketImport(name, hostAbi))
        return socketImport;
    if (const auto posixImport = AndroidPosixImport(name, hostAbi))
        return posixImport;
    if (name == "wcsnrtombs" || name == "mbsnrtowcs" || name == "mbsrtowcs")
        return BridgeAndroidImport(name, reinterpret_cast<uintptr_t>(&UnsupportedConversion));
    // Callers get ENOSYS for Linux facilities with no host implementation.
    constexpr std::string_view unsupported[]{"utimensat", "fork",       "vfork",    "getrusage", "kill",
                                             "setpgid",   "setsid",     "wait",     "symlinkat", "readlinkat",
                                             "fscanf",    "strftime_l", "recvfrom", "sendto"};
    if (std::ranges::find(unsupported, name) != std::end(unsupported))
        return BridgeAndroidImport(name, reinterpret_cast<uintptr_t>(&AndroidUnsupportedOperation));
    if (name == "android_set_abort_message" || name == "openlog" || name == "closelog" || name == "syslog")
        return BridgeAndroidImport(name, reinterpret_cast<uintptr_t>(+[] {
                                   }));
    if (name == "_exit")
        return BridgeAndroidImport(name, reinterpret_cast<uintptr_t>(&ExitProcess));
    if (name == "posix_madvise")
        return BridgeAndroidImport(name, reinterpret_cast<uintptr_t>(+[] {
                                       return 38;
                                   }));
    if (name == "poll" || name == "__poll_chk" || name == "select" || name.starts_with("__FD_") ||
        name.starts_with("eventfd") || name == "ppoll" || name.starts_with("timerfd_") || name.starts_with("epoll_"))
        return AndroidLooperImport(name, hostAbi);
    return AndroidThreadImport(name, hostAbi);
}

extern "C" int StoneAndroidScan(const char* input, const char* format, AndroidVaList* args)
{
    return AndroidVsscanf(input, format, args);
}

int ScanAndroidArguments(const char* input, const char* format, const std::function<uint64_t(size_t)>& destination)
{
    return ScanArguments(input, format, destination);
}

extern "C" int StoneAndroidFormat(char* buffer, size_t size, const char* format, AndroidVaList* args)
{
    return AndroidVsnprintf(buffer, size, format, args);
}

extern "C" int StoneAndroidUnboundedFormat(char* buffer, const char* format, AndroidVaList* args)
{
    return AndroidVsnprintf(buffer, INT_MAX, format, args);
}

extern "C" int StoneAndroidWideFormat(uint32_t* buffer, size_t size, const uint32_t* format, AndroidVaList* args)
{
    return AndroidVswprintf(buffer, size, format, args);
}

extern "C" void StoneAndroidParseLongDouble(_LDOUBLE* result, const char* source, char** end)
{
    *result = ParseLongDouble(source, end);
}

extern "C" void StoneAndroidWideParseLongDouble(_LDOUBLE* result, const uint32_t* source, uint32_t** end)
{
    *result = ParseWideNumber(source, end, &ParseLongDouble);
}

int FormatAndroidArguments(char* output, size_t size, const char* format, const std::function<uint64_t(bool)>& next,
                           const std::function<const char*(uint64_t)>& stringArgument)
{
    return FormatArguments(output, size, format, next, stringArgument);
}

uintptr_t Arm64LibcFunctionImport(std::string_view symbol)
{
    // Use interpreter tokens for known Bionic functions without a corresponding host bridge.
    // Runtime dispatch reports any unimplemented call by name; unknown symbols still fail linking.
    static constexpr std::string_view names[]{
        "getgrgid_r",
        "memmem",
        "recvmmsg",
        "sched_getcpu",
        "setlogmask",
        "vsyslog",
        "alarm",
        "execl",
        "gethostbyaddr",
        "lldiv",
        "process_vm_readv",
        "signalfd",
        "__sched_cpucount",
        "clock_getres",
        "dup3",
        "epoll_pwait",
        "fstatfs",
        "futimens",
        "getppid",
        "getpwuid_r",
        "getrlimit",
        "lchown",
        "pthread_barrier_destroy",
        "pthread_barrier_init",
        "pthread_barrier_wait",
        "readv",
        "scandir",
        "sched_getaffinity",
        "setgid",
        "setgroups",
        "setuid",
        "sysinfo",
        "ttyname_r",
        "_Exit",
        "__cxa_thread_atexit",
        "__fgets_chk",
        "__fread_chk",
        "__strncat_chk",
        "__strrchr_chk",
        "arc4random",
        "asctime",
        "asctime_r",
        "chown",
        "clock",
        "ctime",
        "ctime_r",
        "difftime",
        "exit",
        "fchmodat",
        "fgetc",
        "fgetpos",
        "freopen",
        "fseeko64",
        "fsetpos",
        "fts_close",
        "fts_open",
        "fts_read",
        "fts_set",
        "ftello64",
        "getchar",
        "isalnum",
        "fnmatch",
        "getegid",
        "getgid",
        "getline",
        "gmtime",
        "if_indextoname",
        "inet_ntoa",
        "isalpha",
        "iscntrl",
        "isdigit_l",
        "islower_l",
        "isprint",
        "ispunct",
        "isupper",
        "isupper_l",
        "isxdigit_l",
        "iswalpha",
        "iswalpha_l",
        "iswblank",
        "iswblank_l",
        "iswcntrl",
        "iswcntrl_l",
        "iswdigit",
        "iswdigit_l",
        "iswlower",
        "iswprint",
        "iswprint_l",
        "iswpunct",
        "iswpunct_l",
        "iswspace",
        "iswspace_l",
        "iswupper",
        "iswupper_l",
        "iswxdigit",
        "iswxdigit_l",
        "link",
        "localeconv",
        "memalign",
        "mincore",
        "nftw",
        "nice",
        "pclose",
        "popen",
        "printf",
        "pthread_attr_setstack",
        "pthread_exit",
        "pthread_getcpuclockid",
        "pthread_kill",
        "putc",
        "putchar",
        "raise",
        "random",
        "readdir_r",
        "regcomp",
        "regexec",
        "rewind",
        "sem_timedwait",
        "sendfile",
        "sendmmsg",
        "setvbuf",
        "siglongjmp",
        "srandom",
        "sigsetjmp",
        "sigsuspend",
        "sprintf",
        "statfs",
        "statvfs",
        "strcoll",
        "strtok",
        "strtold",
        "strtoumax",
        "strxfrm",
        "swprintf",
        "symlink",
        "tolower",
        "toupper",
        "strpbrk",
        "strptime",
        "system",
        "timegm",
        "times",
        "tmpfile",
        "tmpnam",
        "towlower",
        "towlower_l",
        "towupper",
        "towupper_l",
        "truncate",
        "truncate64",
        "waitpid",
        "wcscmp",
        "wcscoll",
        "wcscoll_l",
        "wcstod",
        "wcstof",
        "wcstol",
        "wcstold",
        "wcstoll",
        "wcstoul",
        "wcstoull",
        "wcsxfrm",
        "wcsxfrm_l",
        "wmemcpy",
        "wmemmove",
        "wmemset",
    };
    static const std::array<uint8_t, std::size(names)> tokens{};
    const auto found = std::ranges::find(names, symbol);
    return found == std::end(names) ? 0 : reinterpret_cast<uintptr_t>(&tokens[found - names]);
}

uintptr_t* Arm64EnvironmentSlot()
{
    static const auto slot = [] {
        const auto environment = *EnvironmentSlot();
        size_t count = 0, textBytes = 0;
        if (environment) {
            for (; environment[count]; ++count)
                textBytes += std::strlen(environment[count]) + 1;
        }
        const auto pointerBytes = (count + 2) * sizeof(uintptr_t);
        const auto address = AllocateArm64GuestMemory(pointerBytes + textBytes, true);
        if (!address)
            throw std::bad_alloc();
        auto* entries = reinterpret_cast<uintptr_t*>(address);
        auto* text = reinterpret_cast<char*>(address + pointerBytes);
        entries[0] = address + sizeof(uintptr_t);
        for (size_t i = 0; i < count; ++i) {
            const auto length = std::strlen(environment[i]) + 1;
            entries[i + 1] = reinterpret_cast<uintptr_t>(text);
            std::memcpy(text, environment[i], length);
            text += length;
        }
        return entries;
    }();
    return slot;
}

namespace {
uintptr_t* CTypeSlot()
{
    static const auto slot = [] {
        constexpr unsigned Upper = 1, Lower = 2, Digit = 4, Space = 8;
        constexpr unsigned Punctuation = 16, Control = 32, Hex = 64, Blank = 128;
        constexpr size_t count = 257; // EOF precedes the character entries.
        const auto address = AllocateArm64GuestMemory(sizeof(uintptr_t) + count, true);
        if (!address)
            throw std::bad_alloc();
        auto* pointer = reinterpret_cast<uintptr_t*>(address);
        auto* flags = reinterpret_cast<uint8_t*>(pointer + 1);
        *pointer = reinterpret_cast<uintptr_t>(flags);
        for (unsigned ch = 0; ch < 128; ++ch) {
            unsigned type = 0;
            if (ch < ' ' || ch == 127)
                type = Control;
            else if (ch >= 'A' && ch <= 'Z')
                type = Upper | (ch <= 'F' ? Hex : 0);
            else if (ch >= 'a' && ch <= 'z')
                type = Lower | (ch <= 'f' ? Hex : 0);
            else if (ch >= '0' && ch <= '9')
                type = Digit;
            else if (ch == ' ')
                type = Blank;
            else
                type = Punctuation;
            if (ch == ' ' || (ch >= '\t' && ch <= '\r'))
                type |= Space;
            flags[ch + 1] = static_cast<uint8_t>(type);
        }
        return pointer;
    }();
    return slot;
}
}

uintptr_t Arm64LibcDataImport(std::string_view symbol)
{
    if (symbol == "environ")
        return reinterpret_cast<uintptr_t>(Arm64EnvironmentSlot());
    if (symbol == "_ctype_")
        return reinterpret_cast<uintptr_t>(CTypeSlot());
    if (symbol == "in6addr_any")
        return reinterpret_cast<uintptr_t>(Arm64AnyIpv6.data());
    return 0;
}

}
