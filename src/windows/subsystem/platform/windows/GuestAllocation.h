#pragma once

#include <cstddef>

namespace stone::windows {
void* AllocateWindowsGuestAllocation(size_t bytes, size_t alignment);
void FreeWindowsGuestAllocation(void* memory);
}
