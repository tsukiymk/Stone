#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace stone::windows {
// Address immediates in code are relative to the ELF load bias on disk.
// No host callbacks, guest heap objects or ART JIT mappings are persisted.
struct Arm64BlockArtifact {
    uint32_t instructions = 0, flags = 0;
    std::vector<uint8_t> source, code;
    std::vector<uint32_t> relocations;
};

struct Arm64DiskCacheStatistics {
    uint64_t imagesLoaded = 0, blocksLoaded = 0, hits = 0, misses = 0, rejected = 0;
    uint64_t recorded = 0, filesWritten = 0, errors = 0, limited = 0, evicted = 0;
    uint64_t mergeReads = 0, mergeSkipped = 0, blocksMerged = 0;
    uint64_t admissionQueries = 0;
    uint64_t configureTicks = 0, imageTicks = 0, admissionTicks = 0, lookupTicks = 0, recordTicks = 0, flushTicks = 0;
    uint64_t protectTicks = 0, unprotectTicks = 0, parseTicks = 0, serializeTicks = 0;
};

// Opt-in diagnostics only. Returns zero when STONE_ARM64_CACHE_TIMING is off;
// accumulated ticks are QPC wall time, including lock waits, not CPU cycles.
uint64_t Arm64CacheTimingStart() noexcept;
uint64_t Arm64CacheTimingElapsed(uint64_t start) noexcept;
bool ConfigureArm64BlockDiskCache(const std::filesystem::path& directory) noexcept;
bool Arm64BlockDiskCacheEnabled() noexcept;
void RegisterArm64BlockCacheImage(uint64_t image, std::span<const uint8_t> file) noexcept;
void UnregisterArm64BlockCacheImage(uint64_t image) noexcept;
// Bounded lock-free hint for opt-in eager admission; hot admission returns false.
// False positives only advance warmup;
// LoadArm64DiskBlock still checks the exact image, block key and live bytes.
bool MayHaveArm64DiskBlock(uint64_t image, uintptr_t relativePc, uint32_t minimumInstructions) noexcept;
std::optional<Arm64BlockArtifact> LoadArm64DiskBlock(uint64_t image, uintptr_t relativePc, uint32_t minimumInstructions,
                                                     std::span<const uint8_t> source) noexcept;
void RecordArm64DiskBlock(uint64_t image, uintptr_t relativePc, uint32_t minimumInstructions,
                          Arm64BlockArtifact artifact) noexcept;
void FlushArm64BlockDiskCache() noexcept;
Arm64DiskCacheStatistics GetArm64DiskCacheStatistics() noexcept;
}
