#include "memory/GuestHeap.h"
#include "memory/GuestMemory.h"

namespace stone::windows {
GuestHeap& Arm64GuestHeap()
{
    static auto* heap = new GuestHeap;
    return *heap;
}

uintptr_t AllocateArm64GuestMemory(size_t size, bool writable)
{
    return Arm64GuestHeap().Allocate(size, writable);
}

bool FreeArm64GuestMemory(uintptr_t address)
{
    return Arm64GuestHeap().Free(address);
}

bool ReadArm64GuestAllocation(uintptr_t address, void* destination, size_t bytes)
{
    const auto* source = Arm64GuestHeap().At(address, bytes);
    if (!source || !destination)
        return false;
    std::memcpy(destination, source, bytes);
    return true;
}

}
