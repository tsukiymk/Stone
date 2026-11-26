#pragma once

#include <cstdint>
#include <cstddef>
#include <span>

namespace stone::windows {
// Logical AArch64 execute permission. These pages are never host executable.
std::span<const uint8_t> AndroidArm64CodeAt(uintptr_t address);
uint64_t AndroidArm64CodeGeneration();
// These queries validate existing mapping ownership and permissions; they never implement mmap.
uint8_t* AndroidCachedArm64Mapping(uintptr_t address, size_t length, bool writable);
uint8_t* AndroidMappedArm64Data(uintptr_t address, size_t length, bool writable);
}
