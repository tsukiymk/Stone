#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace stone::windows {
inline constexpr uint32_t AndroidSymlinkTag = 0xa000001d;
using AndroidPathMapper = std::filesystem::path (*)(const char*, bool);
bool IsAndroidSymbolicLink(const std::filesystem::path& path);
std::optional<std::string> ReadAndroidSymbolicLink(uintptr_t handle);
std::optional<std::string> ReadAndroidSymbolicLink(const std::filesystem::path& path);
int CreateAndroidSymbolicLink(std::string_view target, const std::filesystem::path& link);
std::filesystem::path ResolveAndroidSymbolicLinks(const std::filesystem::path& path, bool writable, bool followFinal,
                                                  AndroidPathMapper mapper);
}
