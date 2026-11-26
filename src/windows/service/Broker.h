#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace stone::windows {
int RunService(const std::vector<std::wstring>& arguments, const std::filesystem::path& directory);
}
