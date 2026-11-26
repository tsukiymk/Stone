#pragma once

#include <filesystem>
#include <string>
#include <span>
#include <cstdint>
#include <vector>

namespace stone::windows {
void ConfigureShellIntegration(const std::filesystem::path& client, const std::filesystem::path& data,
                               const std::filesystem::path& image);
bool ShellIntegrationEnabled();
void RegisterApplicationLaunch(const std::wstring& packageName, const std::filesystem::path& source = {});
std::vector<std::wstring> ResolveApplicationLaunch(const std::wstring& packageName);
void UpdateApplicationShortcut(const std::wstring& packageName, const std::wstring& label, bool installed);
std::vector<std::wstring> CacheApplicationIcon(const std::filesystem::path& root, const std::wstring& packageName,
                                               const std::wstring& label, std::span<const uint8_t> png,
                                               bool requireShortcut);
}
