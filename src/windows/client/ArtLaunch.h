#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace stone::windows {
int LaunchArtApplication(const std::vector<std::wstring>& arguments, const std::filesystem::path& runtimeDirectory,
                         bool enableJit, bool background = false, bool enableAot = false);
// Diagnostic arguments are supplied only by the isolated native test host.
int LaunchArtPackageManagement(const std::vector<std::wstring>& arguments,
                               const std::filesystem::path& runtimeDirectory, std::wstring diagnosticArguments = {});
}
