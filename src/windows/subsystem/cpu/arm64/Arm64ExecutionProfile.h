#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace stone::windows {

// Opt-in observations, never an execution policy or a substitute for ART JIT.
struct Arm64ExecutionProfile {
    Arm64ExecutionProfile() = default;
    Arm64ExecutionProfile(const Arm64ExecutionProfile&) = delete;
    Arm64ExecutionProfile& operator=(const Arm64ExecutionProfile&) = delete;
    uint64_t instructions = 0, mappedInstructions = 0, runs = 0;
    std::array<uint64_t, 16> opcodeGroups{};
    std::map<std::string, uint64_t, std::less<>> imports;
    uint64_t random = 0;
    uint32_t untilSample = 4096;
    bool instructionProfiling = true;
    // Optional current-thread fetch observation. Counts interpreted instructions
    // only; callers must keep native blocks disabled for exact entry evidence.
    uintptr_t observedAddress = 0;
    uint64_t observedInstructions = 0;

    // Views and counters belong to imports nodes; merging invalidates them.
    struct RecentImport {
        std::string_view name;
        uint64_t* count = nullptr;
    };

    std::array<RecentImport, 8> recentImports{};
};

struct Arm64ProfileHotspot {
    uint64_t image = 0;
    uintptr_t offset = 0;
    uint32_t instruction = 0;
    uint64_t samples = 0;
    std::string name;
};

struct Arm64ExecutionStatistics {
    bool enabled = false;
    bool instructionProfiling = false;
    uint64_t elapsedMilliseconds = 0;
    uint64_t interpretedInstructions = 0, mappedInstructions = 0, interpreterRuns = 0;
    uint64_t nativeLeafCalls = 0, nativeLeafInstructions = 0;
    uint64_t nativeBlockCalls = 0, nativeBlockInstructions = 0;
    uint64_t samples = 0, droppedSamples = 0, errors = 0;
    std::array<uint64_t, 16> opcodeGroups{};
    std::vector<Arm64ProfileHotspot> hotspots;
    std::vector<std::pair<std::string, uint64_t>> imports;
};

Arm64ExecutionProfile* CurrentArm64ExecutionProfile();
void RegisterArm64ProfileImage(uint64_t identity, std::string_view name) noexcept;
void FlushArm64ExecutionProfile(Arm64ExecutionProfile& profile) noexcept;
void RecordArm64ProfileImport(Arm64ExecutionProfile& profile, std::string_view name) noexcept;
void SampleArm64Execution(Arm64ExecutionProfile& profile, uint64_t image, uintptr_t offset,
                          uint32_t instruction) noexcept;
Arm64ExecutionStatistics GetArm64ExecutionStatistics();
void DumpArm64ExecutionProfile() noexcept;
std::vector<uint8_t> ProfileArm64Leaf(std::vector<uint8_t> code, uint32_t instructions);

inline void RecordArm64Instruction(Arm64ExecutionProfile& profile, uint64_t image, uintptr_t offset,
                                   uint32_t instruction, uintptr_t address = 0)
{
    if (profile.observedAddress && profile.observedAddress == address)
        ++profile.observedInstructions;
    ++profile.instructions;
    if (!image)
        ++profile.mappedInstructions;
    ++profile.opcodeGroups[(instruction >> 25) & 15];
    if (--profile.untilSample)
        return;
    // Jitter avoids repeatedly sampling the same position in short loops.
    profile.random ^= profile.random << 13;
    profile.random ^= profile.random >> 7;
    profile.random ^= profile.random << 17;
    profile.untilSample = 2048 + static_cast<uint32_t>(profile.random & 4095);
    SampleArm64Execution(profile, image, offset, instruction);
}

} // namespace stone::windows
