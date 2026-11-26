#pragma once

#include <filesystem>
#include <string_view>

namespace stone::windows {
bool StartProcessLog(std::wstring_view operation = {});
// Name non-application activity under the executable's Operations directory.
bool SetProcessLogOperation(std::wstring_view operation);
// Attach the resolved APK identity, retaining records captured during startup.
bool SetProcessLogPackage(std::wstring_view packageName);
std::filesystem::path ProcessLogPath();
void StopProcessLog();
}
