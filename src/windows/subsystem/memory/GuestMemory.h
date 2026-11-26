#pragma once

#include <cstddef>
#include <cstdint>

namespace stone::windows {
uintptr_t AllocateArm64GuestMemory(size_t size, bool writable);
bool FreeArm64GuestMemory(uintptr_t address);
bool ReadArm64GuestAllocation(uintptr_t address, void* destination, size_t bytes);
}
