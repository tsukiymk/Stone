#include "stub-libandroid-hardware-buffer.h"
#include "stub-libc.h"
#include "stub-libc-sockets.h"
#include "cpu/arm64/Arm64Initializer.h"

#include <windows.h>

#include <atomic>
#include <climits>
#include <memory>
#include <mutex>
#include <new>

namespace stone::windows {
namespace {
constexpr uint64_t CpuReadMask = 0xf, CpuWriteMask = 0xf0;
constexpr uint32_t Blob = 0x21;
constexpr int Invalid = -22, NoMemory = -12, Busy = -16, Unsupported = -38;

bool ValidCpuUsage(uint64_t usage)
{
    if ((usage & ~(CpuReadMask | CpuWriteMask)) || !usage)
        return false;
    const auto read = usage & CpuReadMask;
    const auto write = (usage & CpuWriteMask) >> 4;
    return (read == 0 || read == 2 || read == 3) && (write == 0 || write == 2 || write == 3);
}

uint32_t BytesPerPixel(uint32_t format)
{
    switch (format) {
    case 1:    // RGBA_8888
    case 2:    // RGBX_8888
    case 0x2b: // RGBA_1010102
        return 4;
    case 3:
        return 3; // RGB_888
    case 4:
        return 2; // RGB_565
    case 0x16:
        return 8; // RGBA_FP16
    case Blob:
        return 1;
    default:
        return 0;
    }
}

uint64_t GetLayout(AndroidHardwareBufferDesc& desc)
{
    const auto pixel = BytesPerPixel(desc.format);
    if (!pixel || !desc.width || !desc.height || desc.width > INT_MAX || desc.height > INT_MAX || desc.layers != 1 ||
        !ValidCpuUsage(desc.usage) || (desc.format == Blob && desc.height != 1))
        return 0;
    desc.stride = desc.format == Blob ? desc.width : (desc.width + 15u) & ~15u;
    desc.reserved0 = 0;
    desc.reserved1 = 0;
    const auto rowBytes = uint64_t{desc.stride} * pixel;
    return rowBytes <= uint64_t{INT64_MAX} / desc.height ? rowBytes * desc.height : 0;
}

}

struct AndroidHardwareBuffer {
    AndroidHardwareBufferDesc description{};
    HANDLE mapping = nullptr;
    void* data = nullptr;
    mutable std::atomic_uint references{1};
    std::mutex mutex;
    unsigned readers = 0;
    bool writer = false;

    ~AndroidHardwareBuffer()
    {
        if (data)
            UnmapViewOfFile(data);
        if (mapping)
            CloseHandle(mapping);
    }
};

bool SupportsAndroidHardwareBuffer(const AndroidHardwareBufferDesc& description)
{
    auto layout = description;
    return GetLayout(layout) != 0;
}

int AllocateAndroidHardwareBuffer(const AndroidHardwareBufferDesc* description, AndroidHardwareBuffer** result)
{
    if (!result)
        return Invalid;
    *result = nullptr;
    if (!description)
        return Invalid;
    auto layout = *description;
    const auto bytes = GetLayout(layout);
    if (!bytes)
        return Invalid;
    auto buffer = std::unique_ptr<AndroidHardwareBuffer>(new (std::nothrow) AndroidHardwareBuffer);
    if (!buffer)
        return NoMemory;
    buffer->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(bytes >> 32),
                                         static_cast<DWORD>(bytes), nullptr);
    if (!buffer->mapping)
        return NoMemory;
    buffer->data = MapViewOfFile(buffer->mapping, FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(bytes));
    if (!buffer->data)
        return NoMemory;
    buffer->description = layout;
    *result = buffer.release();
    return 0;
}

void AcquireAndroidHardwareBuffer(const AndroidHardwareBuffer* buffer)
{
    if (buffer)
        ++buffer->references;
}

void ReleaseAndroidHardwareBuffer(const AndroidHardwareBuffer* buffer)
{
    if (buffer && --buffer->references == 0)
        delete buffer;
}

void DescribeAndroidHardwareBuffer(const AndroidHardwareBuffer* buffer, AndroidHardwareBufferDesc* result)
{
    if (buffer && result)
        *result = buffer->description;
}

int LockAndroidHardwareBuffer(AndroidHardwareBuffer* buffer, uint64_t usage, int fence, const AndroidWindowRect* rect,
                              void** address)
{
    if (!address)
        return Invalid;
    *address = nullptr;
    if (!buffer || !ValidCpuUsage(usage))
        return Invalid;
    const auto& desc = buffer->description;
    const bool read = (usage & CpuReadMask) != 0, write = (usage & CpuWriteMask) != 0;
    if ((read && !(desc.usage & CpuReadMask)) || (write && !(desc.usage & CpuWriteMask)))
        return Invalid;
    if (rect && (rect->left < 0 || rect->top < 0 || rect->right < rect->left || rect->bottom < rect->top ||
                 static_cast<uint32_t>(rect->right) > desc.width || static_cast<uint32_t>(rect->bottom) > desc.height))
        return Invalid;
    // Software buffers are synchronous. Sync-file fences from GPU or renderer
    // processes require a fence transport before they can be accepted here.
    if (fence >= 0)
        return Unsupported;
    const std::lock_guard lock(buffer->mutex);
    if (buffer->writer || (write && buffer->readers))
        return Busy;
    if (write)
        buffer->writer = true;
    else
        ++buffer->readers;
    *address = buffer->data;
    return 0;
}

int UnlockAndroidHardwareBuffer(AndroidHardwareBuffer* buffer, int* fence)
{
    if (!buffer)
        return Invalid;
    const std::lock_guard lock(buffer->mutex);
    if (buffer->writer)
        buffer->writer = false;
    else if (buffer->readers)
        --buffer->readers;
    else
        return Invalid;
    if (fence)
        *fence = -1;
    return 0;
}

uintptr_t AndroidHardwareBufferImport(std::string_view symbol, bool hostAbi)
{
#define BIND(name, ...)                                                                                                \
    if (symbol == #name) {                                                                                             \
        const auto target = reinterpret_cast<uintptr_t>(__VA_ARGS__);                                                  \
        return hostAbi ? target : BridgeAndroidImport(symbol, target);                                                 \
    }
    BIND(AHardwareBuffer_allocate, &AllocateAndroidHardwareBuffer);
    BIND(AHardwareBuffer_acquire, &AcquireAndroidHardwareBuffer);
    BIND(AHardwareBuffer_release, &ReleaseAndroidHardwareBuffer);
    BIND(AHardwareBuffer_describe, &DescribeAndroidHardwareBuffer);
    BIND(AHardwareBuffer_lock, &LockAndroidHardwareBuffer);
    BIND(AHardwareBuffer_unlock, &UnlockAndroidHardwareBuffer);
    BIND(AHardwareBuffer_sendHandleToUnixSocket, &SendAndroidHardwareBuffer);
    BIND(AHardwareBuffer_recvHandleFromUnixSocket, &ReceiveAndroidHardwareBuffer);
    BIND(
        AHardwareBuffer_isSupported, +[](const AndroidHardwareBufferDesc* description) -> int {
            return description && SupportsAndroidHardwareBuffer(*description);
        });
#undef BIND
    return 0;
}
}
