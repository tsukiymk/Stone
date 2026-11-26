#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace stone::windows {
int CreateAndroidInotify(int flags);
int AddAndroidInotifyWatch(int fd, const std::filesystem::path& path, uint32_t mask);
int RemoveAndroidInotifyWatch(int fd, int watch);
uint64_t AndroidInotifyId(int fd);
uint32_t PollAndroidInotify(int fd, uint64_t id, uint32_t events, uint64_t& generation);
bool ReadAndroidInotify(int fd, void* buffer, size_t size, intptr_t& result);
bool CloseAndroidInotify(int fd, int& result);
bool DuplicateAndroidInotify(int fd, int minimum, bool closeOnExec, int& result, bool allowed);
bool AndroidInotifyFlags(int fd, int command, int argument, int& result);
bool AndroidInotifyAvailable(int fd, int* bytes, int& result);
}
