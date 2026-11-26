#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace stone::windows {
void SetAndroidSystemDirectory(const std::filesystem::path& directory, const std::filesystem::path& dataDirectory = {});
std::wstring AndroidSystemVisiblePath(const std::wstring& path);
std::optional<std::wstring> AndroidSystemGuestPath(const std::filesystem::path& path);
bool IsAndroidSystemGuestReadOnly(const std::wstring& path);
std::optional<std::wstring> AndroidSystemCachedPath(const std::filesystem::path& path);
bool IsAndroidSystemCachePath(const std::filesystem::path& path);
std::optional<std::filesystem::path> UnresolvedAndroidDataFile(const std::filesystem::path& path);
std::filesystem::path ResolveAndroidSystemFile(const std::wstring& path);
std::vector<std::pair<unsigned, std::wstring>> ListAndroidSystemDirectory(const std::wstring& path);
std::wstring ReadAndroidSystemLink(const std::wstring& path);
void ConfigureAndroidSharedStorage(const std::wstring& packageName, bool allFiles);
void CheckAndroidSharedStorageAccess(const std::filesystem::path& path);
void CheckAndroidSharedStorageTarget(const std::filesystem::path& source, const std::filesystem::path& target);

struct AndroidSystemPathError {
    int code;
};
}
