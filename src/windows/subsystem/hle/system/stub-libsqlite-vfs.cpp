#include "stub-libsqlite.h"
#include "abi/AndroidLibraryImports.h"
#include "filesystem/AndroidSystemFiles.h"

#include <windows.h>
#include <bcrypt.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>

namespace stone::windows {
namespace {
constexpr DWORD PathCapacity = 32768;
constexpr std::wstring_view ExtendedPathPrefix = L"\\\\?\\";
constexpr std::wstring_view ExtendedUncPrefix = L"\\\\?\\UNC\\";
constexpr uint64_t PendingByte = 0x40000000;
constexpr uint64_t ReservedByte = PendingByte + 1;
constexpr uint64_t SharedFirst = PendingByte + 2;
constexpr DWORD SharedSize = 510;
constexpr sqlite3_int64 FileTimeEpochInJulianMillis = 199222286400000ll;
char AndroidPaths;
constexpr uint64_t ShmLockFirst = (22 + SQLITE_SHM_NLOCK) * 4;
constexpr uint64_t ShmDeadman = ShmLockFirst + SQLITE_SHM_NLOCK;
// A private byte-range lock in the index file (not in its mapped header).
// Serialize writers and checkpointers to cover the WAL-reset race in older
// image SQLite builds. Ordinary reader locks remain independent.
constexpr uint64_t ShmOperation = PendingByte;
constexpr unsigned WriterOrCheckpoint = 3;

struct ShmRegion {
    HANDLE mapping = nullptr;
    void* view = nullptr;
    void* address = nullptr;

    ~ShmRegion()
    {
        if (view)
            UnmapViewOfFile(view);
        if (mapping)
            CloseHandle(mapping);
    }
};

using FileIdentity = std::array<DWORD, 3>;

struct ShmNode {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::wstring path;
    FileIdentity identity{};
    bool readOnly = false, deleteRequested = false;
    int regionSize = 0;
    std::mutex mutex;
    std::map<int, std::unique_ptr<ShmRegion>> regions;

    ~ShmNode()
    {
        regions.clear();
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
        if (deleteRequested && !readOnly)
            DeleteFileW(path.c_str());
    }
};

struct ShmConnection {
    std::shared_ptr<ShmNode> node;
    HANDLE handle = INVALID_HANDLE_VALUE;
    unsigned shared = 0, exclusive = 0;

    ~ShmConnection()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};

std::mutex shmRegistryMutex;
std::map<FileIdentity, std::weak_ptr<ShmNode>> shmRegistry;

struct File {
    sqlite3_file base;
    HANDLE handle;
    int lock;
    bool readOnlyMount;
    std::wstring* path;
    ShmConnection* shm;
};

int ShmUnmap(sqlite3_file*, int);

bool WidePath(const char* name, wchar_t* output, bool* prefixed = nullptr)
{
    if (prefixed)
        *prefixed = false;
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, output, PathCapacity);
    if (!size)
        return false;
    if (size <= MAX_PATH)
        return true;
    std::replace(output, output + size - 1, L'/', L'\\');
    const std::wstring_view path(output, size - 1);
    if (path.starts_with(ExtendedPathPrefix) || path.starts_with(L"\\\\.\\"))
        return true;
    const bool unc = path.starts_with(L"\\\\");
    if (!unc && (path.size() < 3 || path[1] != L':' || path[2] != L'\\'))
        return true;
    const auto prefix = unc ? ExtendedUncPrefix : ExtendedPathPrefix;
    const size_t skip = unc ? 2 : 0;
    if (size + prefix.size() - skip > PathCapacity)
        return false;
    std::memmove(output + prefix.size(), output + skip, (size - skip) * sizeof(wchar_t));
    prefix.copy(output, prefix.size());
    if (prefixed)
        *prefixed = true;
    return true;
}

bool VfsPath(sqlite3_vfs* vfs, const char* name, wchar_t* output, bool* readOnly = nullptr)
{
    if (vfs->pAppData != &AndroidPaths)
        return WidePath(name, output);
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, output, PathCapacity) || output[0] != L'/' ||
        std::wstring_view(output).find_first_of(L"\\:") != std::wstring_view::npos)
        return false;
    try {
        const auto guest = AndroidSystemGuestPath(output);
        if (!guest)
            return false;
        const auto native = ResolveAndroidSystemFile(*guest).native();
        if (native.size() >= PathCapacity)
            return false;
        std::copy(native.c_str(), native.c_str() + native.size() + 1, output);
        if (readOnly)
            *readOnly = IsAndroidSystemGuestReadOnly(*guest);
        return true;
    }
    catch (const AndroidSystemPathError&) {
        return false;
    }
    catch (const std::exception&) {
        return false;
    }
}

OVERLAPPED Position(uint64_t offset)
{
    OVERLAPPED result{};
    result.Offset = static_cast<DWORD>(offset);
    result.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return result;
}

bool Lock(HANDLE handle, uint64_t offset, DWORD length, bool exclusive)
{
    auto position = Position(offset);
    return LockFileEx(handle, LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0), 0, length, 0,
                      &position) != FALSE;
}

bool Unlock(HANDLE handle, uint64_t offset, DWORD length)
{
    auto position = Position(offset);
    return UnlockFileEx(handle, 0, length, 0, &position) != FALSE;
}

int Close(sqlite3_file* base)
{
    auto* file = reinterpret_cast<File*>(base);
    ShmUnmap(base, 0);
    delete file->path;
    file->path = nullptr;
    return CloseHandle(file->handle) ? SQLITE_OK : SQLITE_IOERR_CLOSE;
}

int Read(sqlite3_file* base, void* output, int length, sqlite3_int64 offset)
{
    auto position = Position(offset);
    DWORD read = 0;
    if (!ReadFile(reinterpret_cast<File*>(base)->handle, output, static_cast<DWORD>(length), &read, &position) &&
        GetLastError() != ERROR_HANDLE_EOF)
        return SQLITE_IOERR_READ;
    if (read == static_cast<DWORD>(length))
        return SQLITE_OK;
    std::memset(static_cast<char*>(output) + read, 0, static_cast<size_t>(length) - read);
    return SQLITE_IOERR_SHORT_READ;
}

int Write(sqlite3_file* base, const void* data, int length, sqlite3_int64 offset)
{
    auto position = Position(offset);
    DWORD written = 0;
    if (WriteFile(reinterpret_cast<File*>(base)->handle, data, static_cast<DWORD>(length), &written, &position) &&
        written == static_cast<DWORD>(length))
        return SQLITE_OK;
    return GetLastError() == ERROR_DISK_FULL ? SQLITE_FULL : SQLITE_IOERR_WRITE;
}

int Truncate(sqlite3_file* base, sqlite3_int64 size)
{
    FILE_END_OF_FILE_INFO value{};
    value.EndOfFile.QuadPart = size;
    return SetFileInformationByHandle(reinterpret_cast<File*>(base)->handle, FileEndOfFileInfo, &value, sizeof(value))
               ? SQLITE_OK
               : SQLITE_IOERR_TRUNCATE;
}

int Sync(sqlite3_file* base, int)
{
    return FlushFileBuffers(reinterpret_cast<File*>(base)->handle) ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

int Size(sqlite3_file* base, sqlite3_int64* size)
{
    LARGE_INTEGER value{};
    if (!GetFileSizeEx(reinterpret_cast<File*>(base)->handle, &value))
        return SQLITE_IOERR_FSTAT;
    *size = value.QuadPart;
    return SQLITE_OK;
}

int LockFileForSqlite(sqlite3_file* base, int target)
{
    auto& file = *reinterpret_cast<File*>(base);
    if (target <= file.lock)
        return SQLITE_OK;
    const HANDLE handle = file.handle;
    if (file.lock == SQLITE_LOCK_NONE) {
        if (!Lock(handle, PendingByte, 1, false))
            return SQLITE_BUSY;
        const bool locked = Lock(handle, SharedFirst, SharedSize, false);
        const bool released = Unlock(handle, PendingByte, 1);
        if (!released)
            return SQLITE_IOERR_UNLOCK;
        if (!locked)
            return SQLITE_BUSY;
        file.lock = SQLITE_LOCK_SHARED;
    }
    if (target == SQLITE_LOCK_RESERVED) {
        if (!Lock(handle, ReservedByte, 1, true))
            return SQLITE_BUSY;
        file.lock = SQLITE_LOCK_RESERVED;
    }
    else if (target == SQLITE_LOCK_EXCLUSIVE) {
        // Hold pending while upgrading so new readers cannot starve this writer.
        if (file.lock != SQLITE_LOCK_PENDING) {
            if (!Lock(handle, PendingByte, 1, true))
                return SQLITE_BUSY;
            if (file.lock == SQLITE_LOCK_SHARED && !Lock(handle, ReservedByte, 1, true)) {
                if (!Unlock(handle, PendingByte, 1))
                    return SQLITE_IOERR_UNLOCK;
                return SQLITE_BUSY;
            }
            file.lock = SQLITE_LOCK_PENDING;
        }
        if (!Unlock(handle, SharedFirst, SharedSize))
            return SQLITE_IOERR_UNLOCK;
        if (!Lock(handle, SharedFirst, SharedSize, true)) {
            return Lock(handle, SharedFirst, SharedSize, false) ? SQLITE_BUSY : SQLITE_IOERR_LOCK;
        }
        file.lock = SQLITE_LOCK_EXCLUSIVE;
    }
    return SQLITE_OK;
}

int UnlockFileForSqlite(sqlite3_file* base, int target)
{
    auto& file = *reinterpret_cast<File*>(base);
    if (target >= file.lock)
        return SQLITE_OK;
    const HANDLE handle = file.handle;
    if (file.lock == SQLITE_LOCK_EXCLUSIVE) {
        if (!Unlock(handle, SharedFirst, SharedSize))
            return SQLITE_IOERR_UNLOCK;
        if (target == SQLITE_LOCK_SHARED && !Lock(handle, SharedFirst, SharedSize, false))
            return SQLITE_IOERR_LOCK;
    }
    else if (target == SQLITE_LOCK_NONE && !Unlock(handle, SharedFirst, SharedSize)) {
        return SQLITE_IOERR_UNLOCK;
    }
    if (file.lock >= SQLITE_LOCK_RESERVED && !Unlock(handle, ReservedByte, 1))
        return SQLITE_IOERR_UNLOCK;
    if (file.lock >= SQLITE_LOCK_PENDING && !Unlock(handle, PendingByte, 1))
        return SQLITE_IOERR_UNLOCK;
    file.lock = target;
    return SQLITE_OK;
}

int CheckReserved(sqlite3_file* base, int* reserved)
{
    auto& file = *reinterpret_cast<File*>(base);
    *reserved = 1;
    if (file.lock >= SQLITE_LOCK_RESERVED)
        return SQLITE_OK;
    // A shared probe also works on a read-only handle.
    if (!Lock(file.handle, ReservedByte, 1, false)) {
        return GetLastError() == ERROR_LOCK_VIOLATION ? SQLITE_OK : SQLITE_IOERR_CHECKRESERVEDLOCK;
    }
    *reserved = 0;
    return Unlock(file.handle, ReservedByte, 1) ? SQLITE_OK : SQLITE_IOERR_UNLOCK;
}

int FileControl(sqlite3_file* base, int operation, void* argument)
{
    if (operation != SQLITE_FCNTL_LOCKSTATE)
        return SQLITE_NOTFOUND;
    *static_cast<int*>(argument) = reinterpret_cast<File*>(base)->lock;
    return SQLITE_OK;
}

int SectorSize(sqlite3_file*)
{
    return 4096;
}

int DeviceCharacteristics(sqlite3_file*)
{
    return 0;
}

int OpenSharedMemory(File& file)
{
    if (file.shm)
        return SQLITE_OK;
    // A read-only Android mount must not acquire a writable companion file.
    if (!file.path || file.readOnlyMount)
        return SQLITE_READONLY_CANTINIT;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.handle, &info))
        return SQLITE_IOERR_SHMOPEN;
    const FileIdentity identity{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
    try {
        const std::lock_guard registryLock(shmRegistryMutex);
        auto& entry = shmRegistry[identity];
        auto node = entry.lock();
        if (!node) {
            node = std::make_shared<ShmNode>();
            node->identity = identity;
            node->path = *file.path + L"-shm";
            if (node->path.size() >= PathCapacity)
                return SQLITE_IOERR_SHMOPEN;
            constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
            node->handle = CreateFileW(node->path.c_str(), GENERIC_READ | GENERIC_WRITE, share, nullptr, OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
            if (node->handle == INVALID_HANDLE_VALUE) {
                node->handle = CreateFileW(node->path.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
                node->readOnly = true;
            }
            if (node->handle == INVALID_HANDLE_VALUE)
                return SQLITE_IOERR_SHMOPEN;
            // The first process rebuilds the transient WAL index. Other processes
            // retain a shared deadman lock for the complete mapping lifetime.
            if (Lock(node->handle, ShmDeadman, 1, true)) {
                int result = SQLITE_OK;
                if (node->readOnly)
                    result = SQLITE_READONLY_CANTINIT;
                else {
                    FILE_END_OF_FILE_INFO empty{};
                    if (!SetFileInformationByHandle(node->handle, FileEndOfFileInfo, &empty, sizeof(empty))) {
                        // A departing process may still have an old section mapped.
                        // Invalidating the header also forces a WAL-index rebuild.
                        uint64_t zero = 0;
                        DWORD written = 0;
                        auto offset = Position(0);
                        if (!WriteFile(node->handle, &zero, sizeof(zero), &written, &offset) || written != sizeof(zero))
                            result = SQLITE_IOERR_SHMOPEN;
                    }
                }
                if (!Unlock(node->handle, ShmDeadman, 1))
                    return SQLITE_IOERR_SHMLOCK;
                if (result != SQLITE_OK)
                    return result;
            }
            else if (GetLastError() != ERROR_LOCK_VIOLATION)
                return SQLITE_IOERR_SHMLOCK;
            if (!Lock(node->handle, ShmDeadman, 1, false))
                return GetLastError() == ERROR_LOCK_VIOLATION ? SQLITE_BUSY : SQLITE_IOERR_SHMLOCK;
            entry = node;
        }
        auto connection = std::make_unique<ShmConnection>();
        connection->node = node;
        connection->handle = CreateFileW(node->path.c_str(), GENERIC_READ | (node->readOnly ? 0 : GENERIC_WRITE),
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (connection->handle == INVALID_HANDLE_VALUE)
            return SQLITE_IOERR_SHMOPEN;
        file.shm = connection.release();
        return SQLITE_OK;
    }
    catch (const std::bad_alloc&) {
        return SQLITE_IOERR_NOMEM;
    }
}

int ShmMap(sqlite3_file* base, int region, int size, int extend, void volatile** output)
{
    *output = nullptr;
    if (region < 0 || size <= 0)
        return SQLITE_IOERR_SHMSIZE;
    auto& file = *reinterpret_cast<File*>(base);
    const auto opened = OpenSharedMemory(file);
    if (opened != SQLITE_OK)
        return opened;
    auto& node = *file.shm->node;
    try {
        const std::lock_guard lock(node.mutex);
        if (node.regionSize && node.regionSize != size)
            return SQLITE_IOERR_SHMSIZE;
        const auto found = node.regions.find(region);
        if (found != node.regions.end()) {
            *output = found->second->address;
            return node.readOnly ? SQLITE_READONLY : SQLITE_OK;
        }
        const uint64_t required = (uint64_t(region) + 1) * uint64_t(size);
        LARGE_INTEGER current{};
        if (!GetFileSizeEx(node.handle, &current))
            return SQLITE_IOERR_SHMSIZE;
        if (static_cast<uint64_t>(current.QuadPart) < required) {
            if (!extend)
                return SQLITE_OK;
            if (node.readOnly)
                return SQLITE_READONLY;
            FILE_END_OF_FILE_INFO length{};
            length.EndOfFile.QuadPart = required;
            if (!SetFileInformationByHandle(node.handle, FileEndOfFileInfo, &length, sizeof(length)))
                return SQLITE_IOERR_SHMSIZE;
        }
        auto mapping = std::make_unique<ShmRegion>();
        mapping->mapping =
            CreateFileMappingW(node.handle, nullptr, node.readOnly ? PAGE_READONLY : PAGE_READWRITE,
                               static_cast<DWORD>(required >> 32), static_cast<DWORD>(required), nullptr);
        if (!mapping->mapping)
            return SQLITE_IOERR_SHMMAP;
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const uint64_t offset = uint64_t(region) * uint64_t(size), shift = offset % system.dwAllocationGranularity;
        const auto aligned = offset - shift;
        mapping->view = MapViewOfFile(mapping->mapping, node.readOnly ? FILE_MAP_READ : FILE_MAP_READ | FILE_MAP_WRITE,
                                      static_cast<DWORD>(aligned >> 32), static_cast<DWORD>(aligned),
                                      static_cast<size_t>(size + shift));
        if (!mapping->view)
            return SQLITE_IOERR_SHMMAP;
        mapping->address = static_cast<uint8_t*>(mapping->view) + shift;
        const auto address = mapping->address;
        node.regions.emplace(region, std::move(mapping));
        node.regionSize = size;
        *output = address;
        return node.readOnly ? SQLITE_READONLY : SQLITE_OK;
    }
    catch (const std::bad_alloc&) {
        return SQLITE_IOERR_NOMEM;
    }
}

int ShmLock(sqlite3_file* base, int offset, int count, int flags)
{
    auto* connection = reinterpret_cast<File*>(base)->shm;
    if (!connection || offset < 0 || count < 1 || count > SQLITE_SHM_NLOCK || offset > SQLITE_SHM_NLOCK - count ||
        (flags != (SQLITE_SHM_LOCK | SQLITE_SHM_SHARED) && flags != (SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE) &&
         flags != (SQLITE_SHM_UNLOCK | SQLITE_SHM_SHARED) && flags != (SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE)) ||
        (count != 1 && (flags & SQLITE_SHM_SHARED)))
        return SQLITE_IOERR_SHMLOCK;
    const unsigned mask = ((1u << count) - 1) << offset;
    const auto previousExclusive = connection->exclusive;
    auto& owned = flags & SQLITE_SHM_SHARED ? connection->shared : connection->exclusive;
    if (flags & SQLITE_SHM_UNLOCK) {
        if (!(owned & mask))
            return SQLITE_OK;
        if ((owned & mask) != mask || !Unlock(connection->handle, ShmLockFirst + offset, count))
            return SQLITE_IOERR_SHMLOCK;
        owned &= ~mask;
        if ((previousExclusive & WriterOrCheckpoint) && !(connection->exclusive & WriterOrCheckpoint) &&
            !Unlock(connection->handle, ShmOperation, 1))
            return SQLITE_IOERR_SHMLOCK;
    }
    else {
        if ((owned & mask) == mask)
            return SQLITE_OK;
        if ((connection->shared | connection->exclusive) & mask)
            return SQLITE_IOERR_SHMLOCK;
        const bool operation =
            (flags & SQLITE_SHM_EXCLUSIVE) && (mask & WriterOrCheckpoint) && !(previousExclusive & WriterOrCheckpoint);
        if (operation && !Lock(connection->handle, ShmOperation, 1, true))
            return GetLastError() == ERROR_LOCK_VIOLATION ? SQLITE_BUSY : SQLITE_IOERR_SHMLOCK;
        if (!Lock(connection->handle, ShmLockFirst + offset, count, (flags & SQLITE_SHM_EXCLUSIVE) != 0)) {
            const auto error = GetLastError();
            if (operation && !Unlock(connection->handle, ShmOperation, 1))
                return SQLITE_IOERR_SHMLOCK;
            return error == ERROR_LOCK_VIOLATION ? SQLITE_BUSY : SQLITE_IOERR_SHMLOCK;
        }
        owned |= mask;
    }
    return SQLITE_OK;
}

void ShmBarrier(sqlite3_file*)
{
    MemoryBarrier();
}

int ShmUnmap(sqlite3_file* base, int deleteFile)
{
    auto& file = *reinterpret_cast<File*>(base);
    if (!file.shm)
        return SQLITE_OK;
    const std::lock_guard lock(shmRegistryMutex);
    auto node = file.shm->node;
    node->deleteRequested = node->deleteRequested || deleteFile != 0;
    delete file.shm;
    file.shm = nullptr;
    if (node.use_count() == 1) {
        shmRegistry.erase(node->identity);
        node.reset();
    }
    return SQLITE_OK;
}

template <typename Fn> Fn Guest(Fn fn)
{
    return reinterpret_cast<Fn>(MakeAndroidCallbackThunk(reinterpret_cast<uintptr_t>(fn)));
}

const sqlite3_io_methods& IoMethods()
{
    static const sqlite3_io_methods methods{2,
                                            Guest(&Close),
                                            Guest(&Read),
                                            Guest(&Write),
                                            Guest(&Truncate),
                                            Guest(&Sync),
                                            Guest(&Size),
                                            Guest(&LockFileForSqlite),
                                            Guest(&UnlockFileForSqlite),
                                            Guest(&CheckReserved),
                                            Guest(&FileControl),
                                            Guest(&SectorSize),
                                            Guest(&DeviceCharacteristics),
                                            Guest(&ShmMap),
                                            Guest(&ShmLock),
                                            Guest(&ShmBarrier),
                                            Guest(&ShmUnmap)};
    return methods;
}

int Open(sqlite3_vfs* vfs, const char* name, sqlite3_file* base, int flags, int* resultFlags)
{
    auto& file = *reinterpret_cast<File*>(base);
    file.base.pMethods = nullptr;
    file.path = nullptr;
    file.shm = nullptr;
    wchar_t path[PathCapacity];
    bool guestReadOnly = false;
    if (name) {
        if (!VfsPath(vfs, name, path, &guestReadOnly))
            return SQLITE_CANTOPEN;
        if (guestReadOnly && (!(flags & SQLITE_OPEN_READONLY) || (flags & SQLITE_OPEN_DELETEONCLOSE)))
            return SQLITE_READONLY;
    }
    else {
        wchar_t temp[PathCapacity];
        DWORD length = GetTempPathW(PathCapacity, temp);
        if (!length || length >= PathCapacity || !GetTempFileNameW(temp, L"stn", 0, path))
            return SQLITE_CANTOPEN;
    }
    constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    bool readOnly = (flags & SQLITE_OPEN_READONLY) != 0;
    DWORD mode = FILE_ATTRIBUTE_NORMAL;
    if (!name || (flags & SQLITE_OPEN_DELETEONCLOSE))
        mode |= FILE_FLAG_DELETE_ON_CLOSE;
    const DWORD disposition = !name ? OPEN_EXISTING
                              : (flags & SQLITE_OPEN_CREATE)
                                  ? ((flags & SQLITE_OPEN_EXCLUSIVE) ? CREATE_NEW : OPEN_ALWAYS)
                                  : OPEN_EXISTING;
    DWORD access = GENERIC_READ | (readOnly ? 0 : GENERIC_WRITE);
    if (mode & FILE_FLAG_DELETE_ON_CLOSE)
        access |= DELETE;
    file.handle = CreateFileW(path, access, share, nullptr, disposition, mode, nullptr);
    if (file.handle == INVALID_HANDLE_VALUE && !readOnly && (flags & SQLITE_OPEN_MAIN_DB) &&
        !(flags & SQLITE_OPEN_EXCLUSIVE)) {
        file.handle = CreateFileW(path, GENERIC_READ, share, nullptr, OPEN_EXISTING, mode, nullptr);
        readOnly = true;
    }
    if (file.handle == INVALID_HANDLE_VALUE) {
        if (!name)
            DeleteFileW(path);
        return SQLITE_CANTOPEN;
    }
    if (flags & SQLITE_OPEN_MAIN_DB) {
        try {
            file.path = new std::wstring(path);
        }
        catch (const std::bad_alloc&) {
            CloseHandle(file.handle);
            return SQLITE_NOMEM;
        }
    }
    file.readOnlyMount = guestReadOnly;
    if (resultFlags)
        *resultFlags =
            readOnly ? (flags & ~(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) | SQLITE_OPEN_READONLY : flags;
    file.lock = SQLITE_LOCK_NONE;
    file.base.pMethods = &IoMethods();
    return SQLITE_OK;
}

int Delete(sqlite3_vfs* vfs, const char* name, int)
{
    wchar_t path[PathCapacity];
    bool readOnly = false;
    if (!VfsPath(vfs, name, path, &readOnly))
        return vfs->pAppData == &AndroidPaths ? SQLITE_IOERR_DELETE_NOENT : SQLITE_IOERR_DELETE;
    if (readOnly)
        return SQLITE_READONLY;
    if (DeleteFileW(path))
        return SQLITE_OK;
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? SQLITE_IOERR_DELETE_NOENT
                                                                          : SQLITE_IOERR_DELETE;
}

int Access(sqlite3_vfs* vfs, const char* name, int flags, int* result)
{
    wchar_t path[PathCapacity];
    bool readOnly = false;
    if (!VfsPath(vfs, name, path, &readOnly)) {
        *result = 0;
        return vfs->pAppData == &AndroidPaths ? SQLITE_OK : SQLITE_IOERR_ACCESS;
    }
    const DWORD attributes = GetFileAttributesW(path);
    *result = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) &&
              (flags != SQLITE_ACCESS_READWRITE || (!readOnly && !(attributes & FILE_ATTRIBUTE_READONLY)));
    return SQLITE_OK;
}

int FullPath(sqlite3_vfs* vfs, const char* name, int capacity, char* output)
{
    wchar_t path[PathCapacity], absolute[PathCapacity];
    if (vfs->pAppData == &AndroidPaths) {
        if (!VfsPath(vfs, name, path))
            return SQLITE_CANTOPEN;
        try {
            const auto relative = AndroidSystemCachedPath(path);
            if (!relative)
                return SQLITE_CANTOPEN;
            const auto guest = AndroidSystemVisiblePath(*relative);
            return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, guest.c_str(), -1, output, capacity, nullptr,
                                       nullptr)
                       ? SQLITE_OK
                       : SQLITE_CANTOPEN;
        }
        catch (const std::exception&) {
            return SQLITE_CANTOPEN;
        }
    }
    bool prefixed = false;
    if (!WidePath(name, path, &prefixed))
        return SQLITE_CANTOPEN;
    const DWORD length = GetFullPathNameW(path, PathCapacity, absolute, nullptr);
    if (!length || length >= PathCapacity)
        return SQLITE_CANTOPEN;
    const auto* result = absolute;
    // Keep SQLite's database name and journal paths in their original namespace.
    if (prefixed) {
        const std::wstring_view expanded(absolute, length);
        if (expanded.starts_with(ExtendedUncPrefix)) {
            absolute[ExtendedUncPrefix.size() - 2] = L'\\';
            result += ExtendedUncPrefix.size() - 2;
        }
        else if (expanded.starts_with(ExtendedPathPrefix)) {
            result += ExtendedPathPrefix.size();
        }
    }
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, result, -1, output, capacity, nullptr, nullptr)
               ? SQLITE_OK
               : SQLITE_CANTOPEN;
}

int Random(sqlite3_vfs*, int length, char* output)
{
    return BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(output), static_cast<ULONG>(length),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0
               ? length
               : 0;
}

int SleepMicros(sqlite3_vfs*, int micros)
{
    const auto milliseconds = (static_cast<unsigned>(std::max(0, micros)) + 999) / 1000;
    Sleep(milliseconds);
    return static_cast<int>(milliseconds * 1000);
}

int CurrentTime(sqlite3_vfs*, sqlite3_int64* output)
{
    FILETIME time{};
    GetSystemTimePreciseAsFileTime(&time);
    *output = static_cast<sqlite3_int64>(((uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime) / 10000) +
              FileTimeEpochInJulianMillis;
    return SQLITE_OK;
}

int CurrentDay(sqlite3_vfs* vfs, double* output)
{
    sqlite3_int64 millis;
    CurrentTime(vfs, &millis);
    *output = static_cast<double>(millis) / 86400000.0;
    return SQLITE_OK;
}
}

sqlite3_vfs* WindowsSqliteVfs(sqlite3_vfs* storage, bool useAndroidPaths)
{
    IoMethods();
    static sqlite3_vfs vfs{2,
                           sizeof(File),
                           PathCapacity,
                           nullptr,
                           "stone-win32",
                           nullptr,
                           Guest(&Open),
                           Guest(&Delete),
                           Guest(&Access),
                           Guest(&FullPath),
                           nullptr,
                           nullptr,
                           nullptr,
                           nullptr,
                           Guest(&Random),
                           Guest(&SleepMicros),
                           Guest(&CurrentDay),
                           nullptr,
                           Guest(&CurrentTime)};
    if (!storage)
        return &vfs;
    *storage = vfs;
    storage->pNext = nullptr;
    if (useAndroidPaths)
        storage->pAppData = &AndroidPaths;
    return storage;
}
}
