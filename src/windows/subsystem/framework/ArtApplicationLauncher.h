#pragma once

#include <filesystem>
#include <string>

namespace stone::windows {
void ConfigureArtApplicationLauncher(const std::filesystem::path& executable, const std::filesystem::path& dataRoot,
                                     const std::filesystem::path& image);
void LaunchInstalledArtApplication(const std::wstring& guestSource, const std::wstring& packageName);
void SetArtRunningPackage(const std::wstring& packageName);
std::wstring ArtRunningPackage();
void PublishArtApplicationWindow();
}
