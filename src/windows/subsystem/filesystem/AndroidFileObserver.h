#pragma once

#include <filesystem>
#include <functional>
#include <cstdint>
#include <string>

namespace stone::windows {
using AndroidFileEvent = std::function<void(int, int, const std::wstring&)>;
int WatchAndroidDirectory(const std::wstring& guestPath, int mask, AndroidFileEvent callback);
using AndroidDirectoryEvent = std::function<void(int, uint32_t, uint32_t, const std::wstring&)>;
// Resolved host path; callers must apply the guest filesystem access policy first.
int WatchAndroidDirectoryPath(const std::filesystem::path& path, uint32_t mask, AndroidDirectoryEvent callback);
void StopAndroidDirectoryWatch(int watch);
bool AndroidDirectoryWatchAccessible(int watch);
// Capture before closing, publish only after a successful close. Read-only and
// non-file descriptors return an empty path. No JNI runs on filesystem threads.
std::filesystem::path AndroidWritableFileClosing(int fd);
void NotifyAndroidFileClosed(const std::filesystem::path& path);
}
