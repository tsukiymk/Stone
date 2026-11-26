#pragma once

#include <filesystem>

namespace stone::windows {
int RunWindowsService(const std::filesystem::path& directory);
}
