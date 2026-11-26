#pragma once

#include <filesystem>
#include <functional>

namespace stone::windows::test {
void WithAudioLoopback(const std::function<void()>& test, const std::filesystem::path& output,
                       unsigned long process = 0, bool tones = true);
}
