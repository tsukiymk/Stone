#pragma once

#include <windows.h>
#include <cstdint>
#include <span>
#include <string_view>

namespace stone::windows {
class ElfImage;
bool PrepareUbsanRuntime(std::string_view soname, std::span<const uint8_t> file, ElfImage& image);
bool DispatchUbsanSyscall(const ElfImage& image, CONTEXT& context) noexcept;
}
