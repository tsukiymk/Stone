#pragma once

#include <cstddef>
#include <cstdint>

namespace stone::windows {
void* PublishCode(const void* code, size_t size);
void* AllocateReplacementCode(size_t size, uintptr_t nearAddress);
}
