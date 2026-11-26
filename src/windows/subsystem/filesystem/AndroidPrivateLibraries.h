#pragma once

#include <filesystem>
#include <string_view>
#include <vector>
#include <cstdint>

namespace stone::windows {
std::vector<uint8_t> ReadAndroidPrivateLibrary(const std::filesystem::path& directory, std::string_view prefix,
                                               std::string_view name);
}
