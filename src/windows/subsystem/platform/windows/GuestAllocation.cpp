#include "platform/windows/GuestAllocation.h"
#include <malloc.h>

namespace stone::windows {
void* AllocateWindowsGuestAllocation(size_t bytes, size_t alignment)
{
    return _aligned_malloc(bytes, alignment);
}

void FreeWindowsGuestAllocation(void* memory)
{
    _aligned_free(memory);
}
}
