#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace stone::windows {
bool NativeImageZlibLoaded();
uintptr_t Libz(std::string_view name);
uintptr_t NativeImageZlibExport(std::string_view name);
int CallNativeImageZlib(std::string_view name, const std::array<uintptr_t, 4>& arguments);
uint64_t CallArm64Zlib(std::string_view name, std::span<const uint64_t, 8> args,
                       const std::function<uint8_t*(uintptr_t, unsigned, bool)>& memory,
                       const std::function<std::string(uintptr_t, unsigned)>& string);
}
