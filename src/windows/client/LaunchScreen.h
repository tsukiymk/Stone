#pragma once

#include <string_view>

namespace stone::windows {
// Foreground launches only. The screen has its own message pump during ART startup.
void StartLaunchScreen(std::wstring_view label);
void FinishLaunchScreen();
void FailLaunchScreen(std::string_view error);
}
