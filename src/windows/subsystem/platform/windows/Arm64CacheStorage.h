#pragma once

#include "cpu/arm64/cache/Arm64CacheFormat.h"
#include <windows.h>
#include <cerrno>
#include <functional>

namespace stone::windows::arm64_cache {
struct HostState {
    DWORD win = GetLastError();
    int error = errno;

    ~HostState()
    {
        errno = error;
        SetLastError(win);
    }
};

Digest TranslatorIdentity();
Digest SourceKey(const Digest& translator, std::span<const uint8_t> file);
std::wstring Name(const Digest& digest);
bool Identified(const std::shared_ptr<CacheFile>& file);
std::shared_ptr<CacheFile> Open(const std::filesystem::path& file);
bool SameFile(const std::shared_ptr<CacheFile>& first, const std::shared_ptr<CacheFile>& second);
// Compression is inside DPAPI. Unpack only authenticated plaintext, with an
// explicit bounded size; raw format-1 payloads remain readable.
std::vector<uint8_t> Pack(std::vector<uint8_t> plain);
std::vector<uint8_t> Unpack(std::vector<uint8_t> authenticated);
std::shared_ptr<Bundle> Read(std::shared_ptr<CacheFile>, const Digest&, Arm64DiskCacheStatistics&, bool pinBaselines);
// Callers drop older pins if the filesystem requires the unpinned fallback.
std::shared_ptr<CacheFile> Write(const std::filesystem::path&, const Bundle&, Arm64DiskCacheStatistics&,
                                 bool& pinBaselines, const std::function<void()>& releaseBaselines);
std::shared_ptr<void> AcquireWriter(const std::filesystem::path& root);

struct StoredFile {
    std::filesystem::path path;
    std::filesystem::file_time_type time;
    uint64_t bytes;
    Digest key;
};

std::vector<StoredFile> ListFiles(const std::filesystem::path& root);
bool RemoveFile(const std::filesystem::path& file);
}
