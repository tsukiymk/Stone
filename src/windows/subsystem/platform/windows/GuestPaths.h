#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace stone::windows {
std::optional<std::wstring> RelativeWindowsGuestPath(const std::filesystem::path& path,
                                                     const std::filesystem::path& parent);
}
