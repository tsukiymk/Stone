#pragma once

#include <cstdint>
#include <string_view>

namespace stone::windows {
struct AndroidWindowRect;
struct AndroidHardwareBuffer;

struct AndroidHardwareBufferDesc {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, reserved0;
    uint64_t reserved1;
};

static_assert(sizeof(AndroidHardwareBufferDesc) == 40);

inline constexpr uint64_t AndroidBufferCpuWriteOften = 0x30;

bool SupportsAndroidHardwareBuffer(const AndroidHardwareBufferDesc& description);
int AllocateAndroidHardwareBuffer(const AndroidHardwareBufferDesc* description, AndroidHardwareBuffer** result);
void AcquireAndroidHardwareBuffer(const AndroidHardwareBuffer* buffer);
void ReleaseAndroidHardwareBuffer(const AndroidHardwareBuffer* buffer);
void DescribeAndroidHardwareBuffer(const AndroidHardwareBuffer* buffer, AndroidHardwareBufferDesc* result);
int LockAndroidHardwareBuffer(AndroidHardwareBuffer* buffer, uint64_t usage, int fence, const AndroidWindowRect* rect,
                              void** address);
int UnlockAndroidHardwareBuffer(AndroidHardwareBuffer* buffer, int* fence);

uintptr_t AndroidHardwareBufferImport(std::string_view symbol, bool hostAbi = false);
}
