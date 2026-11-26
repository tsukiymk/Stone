#pragma once

#include "cpu/arm64/cache/Arm64BlockCache.h"
#include <array>
#include <map>
#include <memory>

namespace stone::windows::arm64_cache {
using Digest = std::array<uint8_t, 32>;
using Key = std::pair<uint64_t, uint32_t>;
// The API 37 ART image alone exceeds 8 MiB of emitted hot blocks. Keep a
// per-image bound large enough for that workload within the 64 MiB total.
inline constexpr size_t MaximumCode = 16384, MaximumSource = 128, MaximumImage = 32 * 1024 * 1024;
inline constexpr size_t MaximumMemory = 64 * 1024 * 1024, MaximumFiles = 2048, MaximumDisk = 128 * 1024 * 1024;
inline constexpr uint32_t Format = 1, MaximumBlocks = 65536;
inline constexpr uint64_t AdmissionBits = 1 << 22;

struct Saved {
    Arm64BlockArtifact artifact;
    bool disk = false;
};
struct CacheFile;

struct Bundle {
    Digest key;
    std::map<Key, Saved> blocks;
    size_t bytes = 0;
    bool dirty = false;
    bool loadAttempted = false;
    // Pin the authenticated file object and deny in-place writes. Atomic
    // replacement remains allowed; the file ID cannot be recycled underneath
    // us. This identity is an optimization, never an authentication substitute.
    std::shared_ptr<CacheFile> baseline;
};

size_t Bytes(const Arm64BlockArtifact& artifact);
void Check(bool good);
void Validate(const Arm64BlockArtifact& artifact, uint32_t minimum);
std::shared_ptr<Bundle> Decode(std::span<const uint8_t> plain, const Digest& key);
std::vector<uint8_t> Encode(const Bundle& bundle);
}
