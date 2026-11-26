#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace stone::windows {
void SetAndroidSystemFonts(std::wstring path);
std::filesystem::path AndroidSystemFontPath(std::string_view path);
bool IsAndroidImageFontPath(const std::filesystem::path& path);
uintptr_t AndroidFontImport(std::string_view symbol);
}
