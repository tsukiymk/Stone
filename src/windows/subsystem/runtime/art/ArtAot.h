#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace stone::windows {
void ConfigureArtAot(const std::filesystem::path& compiler, const std::filesystem::path& system,
                     const std::filesystem::path& data, std::string bootClassPath);
void PrepareArtAot(std::string_view apk);
}
