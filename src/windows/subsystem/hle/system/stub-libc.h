#pragma once

#include "memory/GuestMappings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace stone::windows {
inline constexpr std::array<uint8_t, 16> Arm64AnyIpv6{};
inline constexpr int AndroidNameTooLong = 36;
inline constexpr int AndroidNotImplemented = 38;
uintptr_t* Arm64EnvironmentSlot();
uintptr_t Arm64LibcDataImport(std::string_view symbol);
uintptr_t Arm64LibcFunctionImport(std::string_view symbol);
std::string AndroidGuestFilePath(std::string_view path);
void ForgetAndroidFileDescriptor(int fd);
void ConfigureAndroidFdSan(int targetSdk);
bool ReadAndroidRandomDevice(int fd, void* buffer, size_t size, intptr_t& result);
bool ReadAndroidMemoryFile(int fd, void* buffer, size_t size, intptr_t& result);
bool CloseAndroidMemoryFile(int fd, int& result);

struct AndroidTimespec {
    int64_t seconds, nanos;
};

struct AndroidTimeval {
    int64_t seconds, micros;
};

struct AndroidIoVector {
    const void* data;
    size_t size;
};

static_assert(sizeof(AndroidIoVector) == 16);
inline constexpr int AndroidSelectFdLimit = 1024;
using AndroidFdSet = std::array<uint64_t, AndroidSelectFdLimit / 64>;

struct AndroidUtime {
    int64_t accessed, modified;
};

struct AndroidTm {
    std::tm fields;
    int64_t offset;
    const char* zone;
};

static_assert(sizeof(AndroidTm) == 56);
const char* ParseAndroidTime(const char* input, const char* format, AndroidTm* value);

struct AndroidUtsname {
    char system[65], node[65], release[65], version[65], machine[65], domain[65];
};

static_assert(sizeof(AndroidUtsname) == 390);

struct AndroidArm64Stat {
    uint64_t device, inode;
    uint32_t mode, links, uid, gid;
    uint64_t special, padding;
    int64_t size;
    int32_t blockSize, padding2;
    int64_t blocks;
    AndroidTimespec accessed, modified, changed;
    uint32_t reserved[2];
};

static_assert(sizeof(AndroidArm64Stat) == 128);

struct AndroidStatFs {
    uint64_t type, blockSize, blocks, freeBlocks, availableBlocks, files, freeFiles;
    uint32_t fsid[2];
    uint64_t nameLimit, fragmentSize, flags, reserved[4];
};

static_assert(sizeof(AndroidStatFs) == 120);

struct AndroidStatVfs {
    uint64_t blockSize, fragmentSize, blocks, freeBlocks, availableBlocks, files, freeFiles, availableFiles;
    uint64_t fsid, flags, nameLimit;
    uint32_t reserved[6];
};

static_assert(sizeof(AndroidStatVfs) == 112 && offsetof(AndroidStatVfs, reserved) == 88);

struct AndroidFileRange {
    int16_t type, origin;
    int64_t start, length;
    int32_t pid;
};

static_assert(offsetof(AndroidFileRange, start) == 8 && sizeof(AndroidFileRange) == 32);

struct AndroidDirent {
    uint64_t inode;
    int64_t offset;
    uint16_t length;
    uint8_t type;
    char name[256];
};

static_assert(sizeof(AndroidDirent) == 280 && offsetof(AndroidDirent, name) == 19);

struct AndroidThreadAttr {
    uint32_t flags;
    void* stackBase;
    size_t stackSize, guardSize;
    int32_t policy, priority;
    uint64_t reserved[2];
};

static_assert(sizeof(AndroidThreadAttr) == 56);

struct AndroidSignalAction {
    uint32_t flags;
    uintptr_t handler;
    uint64_t mask;
    uintptr_t restorer;
};

static_assert(sizeof(AndroidSignalAction) == 32);
int AndroidArm64Sigaction(int signal, const AndroidSignalAction* action, AndroidSignalAction* previous);
void RegisterArm64SignalThread();
void RegisterNativeSignalThread();
bool PollArm64Signals();
bool DeliverAndroidArm64MemoryFault(uintptr_t address);
uint64_t ExchangeAndroidSignalMask(uint64_t mask);
int AndroidArm64SuspendSignals(uint64_t mask);

struct AndroidVaList {
    uint32_t integerOffset, floatOffset;
    const char* stack;
    const char* registers;
    uint64_t next(bool floating = false);
};

static_assert(sizeof(AndroidVaList) == 24);
int FormatAndroidArguments(char* output, size_t size, const char* format, const std::function<uint64_t(bool)>& next,
                           const std::function<const char*(uint64_t)>& stringArgument);
int ScanAndroidArguments(const char* input, const char* format, const std::function<uint64_t(size_t)>& destination);
int WriteAndroidLog(int priority, const char* tag, const char* message, int buffer = 0);
int RunAndroidOnce(int& state, const std::function<void()>& callback);
int CreateAndroidKey(int& key, uintptr_t destructor);
int DeleteAndroidKey(int key);
void* GetAndroidThreadValue(int key);
int SetAndroidThreadValue(int key, void* value);
int AndroidClock(int clock, AndroidTimespec* result);
int AndroidFutex(uint32_t& word, unsigned operation, uint32_t value, const AndroidTimespec* timeout, uint32_t mask);
int AndroidFutexRequeue(uint32_t& source, unsigned operation, uint32_t wake, uint32_t move, uint32_t* destination,
                        uint32_t compare);
int64_t AndroidX64Syscall(unsigned number, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e,
                          uintptr_t f);
char* AndroidPathComponent(const char* path, bool directory);
std::span<char> AndroidPathStorage(bool directory);
uint32_t AndroidApplicationUid();
void SetAndroidApplicationUid(uint32_t uid);
inline constexpr char AndroidNodename[] = "localhost";
int AndroidUname(AndroidUtsname* result, std::string_view machine);
int AndroidGetHostname(char* output, size_t size);
int AndroidUnsupportedOperation();
int64_t AndroidGetRandom(unsigned char* buffer, size_t size, unsigned flags);
int CreateAndroidSharedMemory(const char* name, size_t size);
size_t AndroidSharedMemorySize(int fd);
bool AndroidMemoryFileIoctl(int fd, unsigned long request, uintptr_t argument, int& result);
int AndroidFileAvailable(int fd, int* available);
int SetAndroidSharedMemoryProtection(int fd, int protection);
void* AndroidMapArm64(void* address, size_t length, int access, int flags, int fd, int64_t offset);
int AndroidUnmapArm64(void* address, size_t length);
int AndroidProtectArm64(void* address, size_t length, int access);
int AndroidAdviseArm64(void* address, size_t length, int advice);
int AndroidSyncArm64(void* address, size_t length, int flags);
// Committed executable bytes owned by an x64 guest mmap, including mixed protections.
bool AndroidExecutableMapping(uintptr_t address, size_t length);
bool InitializeAndroidWinsock();
int CloseAndroidFd(int fd);
int DuplicateAndroidFd(int fd);
int DuplicateAndroidFileFd(int fd, bool closeOnExec);

struct AndroidSocketFile {
    uintptr_t file = 0;
    uintptr_t section = 0;
    unsigned mode = 0;
    unsigned seals = 0;
};

bool ExportAndroidSocketFile(int fd, AndroidSocketFile& grant, bool allowDisk);
int ImportAndroidSocketFile(AndroidSocketFile grant);
uintptr_t DuplicateAndroidDiskFileHandle(int fd);
uintptr_t DuplicateAndroidReadOnlyMemoryHandle(int fd, unsigned& metadata);
int AdoptAndroidReadOnlyMemoryHandle(uintptr_t handle, unsigned metadata);
void CloseQueuedAndroidFd(int fd);
int DuplicateCrtFd(int fd, int minimum, bool inherit);
bool AndroidLooperOwnsFd(int fd);
bool DuplicateAndroidLooperFd(int fd, int minimum, bool closeOnExec, int& result, bool allowEpoll);
intptr_t ReadAndroidFd(int fd, void* buffer, size_t size);
bool WriteAndroidEventCounter(int fd, const void* buffer, size_t size, intptr_t& result);
bool AndroidEventCounterFlags(int fd, int command, int argument, int& result);
bool AndroidFenceReady(int fd);
uintptr_t AndroidLooperImport(std::string_view name, bool hostAbi = false);
void DetachAndroidNativeThread();
uintptr_t CallAndroidCallback(uintptr_t target, void* argument, void* second = nullptr, void* third = nullptr,
                              uintptr_t fourth = 0, std::span<uint8_t> writable = {});
uintptr_t BridgeAndroidImport(std::string_view name, uintptr_t target);
uintptr_t AndroidLibcImport(std::string_view name, bool hostAbi = false);
int AndroidEncodeBase64(const unsigned char* input, size_t size, char* output, size_t capacity);
int AndroidMakeDnsQuery(int operation, const char* name, int family, int type, const void* data, int size,
                        const void* additional, unsigned char* output, int capacity);
uintptr_t AndroidPosixImport(std::string_view name, bool hostAbi);
uintptr_t AndroidLogImport(std::string_view name);
void* AndroidRealloc(void* memory, size_t size);
void* AndroidMalloc(size_t size);
void* AndroidCalloc(size_t count, size_t size);
uintptr_t AndroidArtHeapImport(std::string_view name);
void AndroidFree(void* memory);
uintptr_t AndroidThreadImport(std::string_view name, bool hostAbi = false);
uintptr_t AndroidAudioImport(std::string_view name, bool hostAbi = false);
void SetAndroidSystemCaDirs(std::wstring system, std::wstring conscrypt);
void SetAndroidProcDirectory(std::wstring directory);
}
