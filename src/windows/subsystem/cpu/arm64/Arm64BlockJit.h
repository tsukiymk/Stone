#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace stone::windows {

enum class Arm64BlockExit : uint32_t {
    Continue,
    Branch,
    Return,
    Failure
};
enum class Arm64BlockMemory : uint32_t {
    Transfer,
    Pair4,
    Pair8,
    Pair4Vector,
    Pair16Vector,
    Literal,
    StackArithmetic
};

// Registers remain owned by the interpreter. Unsupported operations, imports,
// signal delivery and exceptions return to that same owner.
struct Arm64BlockContext {
    uint64_t* registers = nullptr;
    std::array<uint64_t, 2>* vectors = nullptr;
    uintptr_t sp = 0, pc = 0, instruction = 0;
    void* owner = nullptr;
    void (*memory)(Arm64BlockContext*, uint32_t, Arm64BlockMemory) noexcept = nullptr;
    uint32_t nzcv = 0, flagsValid = 0;
    Arm64BlockExit exit = Arm64BlockExit::Continue;
    uint32_t executed = 0;
    uint32_t budget = 0, completed = 0;
    // Reset before each store callback; only the guarded interpreter may
    // certify that all writes stayed inside its non-executable guest stack.
    uint32_t stackStoreOnly = 0;
};

class Arm64Block {
public:
    ~Arm64Block();
    Arm64Block(const Arm64Block&) = delete;
    Arm64Block& operator=(const Arm64Block&) = delete;
    bool Matches(uint64_t image, uintptr_t pc, uintptr_t bias, uint64_t generation,
                 std::span<const uint8_t> source) const;
    uint32_t Instructions() const;
    size_t CodeBytes() const; // Emitted instructions, excluding page rounding/unwind metadata.
    bool RequiresFlags() const;
    std::span<const uint32_t> Words() const;
    void Run(Arm64BlockContext& context) const;

private:
    Arm64Block();
    struct Implementation;
    std::unique_ptr<Implementation> impl;
    friend std::shared_ptr<const Arm64Block> FindArm64Block(uint64_t, uintptr_t, uintptr_t, uint64_t,
                                                            std::span<const uint8_t>, uint32_t);
};

struct Arm64BlockStatistics {
    uint64_t lookups = 0, hits = 0, compiled = 0, unsupported = 0, invalidations = 0, evictions = 0, failures = 0;
    uint64_t executions = 0, instructions = 0, dispatches = 0;
    uint64_t warmupSkipped = 0;
    uint64_t unprofitable = 0;
    uint64_t unwindTables = 0, activeUnwindTables = 0, committedCodeBytes = 0;
    uint64_t compileTicks = 0, publishTicks = 0;
    size_t entries = 0;
};

bool Arm64BlockJitEnabled();
bool Arm64StackStoreTraceEnabled();
bool Arm64CompactPcEnabled();
bool WarmArm64Block(uint64_t image, uintptr_t pc, uintptr_t bias);
// Nonzero identity denotes an ElfImage with immutable executable pages; zero
// denotes mutable/aliased mapped code. Stores end execution unless experimental
// tracing is enabled and the callback certifies only non-executable stack writes.
std::shared_ptr<const Arm64Block> FindArm64Block(uint64_t image, uintptr_t pc, uintptr_t bias, uint64_t generation,
                                                 std::span<const uint8_t> source, uint32_t minimumInstructions = 1);
// Interpreter-local batches can retain at most 4095 pending calls or warmup skips.
void RecordArm64BlockExecution(uint64_t instructions, uint64_t calls = 1, uint64_t warmupSkips = 0,
                               uint64_t dispatches = 0);
Arm64BlockStatistics GetArm64BlockStatistics();
void ClearArm64BlockCache();

} // namespace stone::windows
