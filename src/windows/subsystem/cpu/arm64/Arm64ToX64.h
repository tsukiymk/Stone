#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace stone::windows {

class Arm64TranslationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Compile an AAPCS64 integer leaf to a Windows x64 function. A zero width
// marks an unavailable argument or a void result.
std::vector<uint8_t> TranslateArm64Leaf(std::span<const uint8_t> code, std::span<const unsigned> arguments,
                                        unsigned resultWidth);

} // namespace stone::windows
