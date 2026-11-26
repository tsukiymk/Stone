#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace stone::windows {
struct AndroidHardwareBuffer;

struct AndroidAddrInfo {
    int flags, family, type, protocol;
    uint32_t length;
    char* canonicalName;
    void* address;
    AndroidAddrInfo* next;
};

static_assert(sizeof(AndroidAddrInfo) == 48 && offsetof(AndroidAddrInfo, canonicalName) == 24);
int AndroidGetAddrInfo(const char* name, const char* service, const AndroidAddrInfo* hints, AndroidAddrInfo** output);
void AndroidFreeAddrInfo(AndroidAddrInfo* address);
uintptr_t AndroidSocketImport(std::string_view name, bool hostAbi = false);
uint64_t AndroidSocketId(int fd);
uintptr_t ExportAndroidUnixSocket(int fd, uintptr_t target, unsigned peerUid, unsigned& flags);
int ImportAndroidUnixSocket(uintptr_t handle, unsigned flags, int peerPid, unsigned peerUid);
bool CloseAndroidSocket(int fd, int& result);
bool DuplicateAndroidSocket(int fd, int minimum, bool closeOnExec, int& result, bool allowSocket = true);
bool ReplaceAndroidSocket(int source, int target, bool closeOnExec, int& result);
bool ReadAndroidSocket(int fd, void* buffer, size_t size, intptr_t& result);
bool WriteAndroidSocket(int fd, const void* buffer, size_t size, intptr_t& result);
int SendAndroidHardwareBuffer(const AndroidHardwareBuffer* buffer, int fd);
int ReceiveAndroidHardwareBuffer(int fd, AndroidHardwareBuffer** buffer);
bool AndroidSocketFlags(int fd, int command, int argument, int& result);
uint32_t PollAndroidSocket(int fd, uint64_t id, uint32_t events, uint32_t& previous, uint64_t& readGeneration,
                           uint64_t& writeGeneration);
}
