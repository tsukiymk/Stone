#pragma once

#include <cstddef>
#include <string_view>

namespace stone::windows {
char ReadArm64JniType(std::string_view signature, size_t& offset, bool allowVoid);
}
