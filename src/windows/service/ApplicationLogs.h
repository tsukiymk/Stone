#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace stone::windows {
// Called only after the broker has authenticated the requesting Client's root
// and package. Each journal retains at most 512,000 UTF-16 code units.
std::wstring ApplicationLogRequest(const std::filesystem::path& data, const std::wstring& package,
                                   const std::vector<std::wstring>& fields);
void RemoveApplicationLogs(const std::filesystem::path& data, const std::wstring& package);
}
