#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace stone::windows {
uint64_t CallArm64OpenSl(std::string_view name, std::span<const uint64_t, 8> arguments,
                         const std::function<uint8_t*(uintptr_t, unsigned, bool)>& memory);
}
