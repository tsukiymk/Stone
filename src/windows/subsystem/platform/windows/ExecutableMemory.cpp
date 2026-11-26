#include "platform/windows/ExecutableMemory.h"
#include <windows.h>
#include <atomic>
#include <climits>
#include <cstring>
#include <stdexcept>

namespace stone::windows {
void* PublishCode(const void* code, size_t size)
{
    void* memory = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory)
        throw std::runtime_error("could not allocate Android native code");
    std::memcpy(memory, code, size);
    DWORD old;
    if (!VirtualProtect(memory, size, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), memory, size)) {
        VirtualFree(memory, 0, MEM_RELEASE);
        throw std::runtime_error("could not publish Android native code");
    }
    return memory;
}

void* AllocateReplacementCode(size_t size, uintptr_t nearAddress)
{
    if (nearAddress) {
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const uintptr_t granularity = system.dwAllocationGranularity;
        const uintptr_t limit = nearAddress > UINTPTR_MAX - INT32_MAX ? UINTPTR_MAX : nearAddress + INT32_MAX;
        static std::atomic<uintptr_t> cursor{0};
        const uintptr_t first = (nearAddress / granularity + 1) * granularity;
        const uintptr_t hint = cursor.load(std::memory_order_relaxed);
        for (uintptr_t address = hint >= first && hint < limit ? hint : first;
             address < limit && size < limit - address;) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)))
                break;
            const auto end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
            if (region.State == MEM_FREE && size <= end - address) {
                if (auto* memory = VirtualAlloc(reinterpret_cast<void*>(address), size, MEM_RESERVE | MEM_COMMIT,
                                                PAGE_READWRITE)) {
                    cursor.store((address + size + granularity - 1) / granularity * granularity,
                                 std::memory_order_relaxed);
                    return memory;
                }
                address += granularity;
            }
            else {
                if (end > UINTPTR_MAX - granularity)
                    break;
                address = (end + granularity - 1) / granularity * granularity;
            }
        }
    }
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

}
