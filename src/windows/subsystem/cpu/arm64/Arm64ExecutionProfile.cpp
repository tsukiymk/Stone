#include "Arm64ExecutionProfile.h"
#include "Arm64BlockJit.h"
#include "cache/Arm64BlockCache.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <tuple>

namespace stone::windows {
namespace {
constexpr size_t MaximumHotspots = 65536;
std::atomic<uint64_t> profileErrors{0};

struct HostState {
    DWORD lastError = GetLastError();
    int error = errno;

    ~HostState()
    {
        errno = error;
        SetLastError(lastError);
    }
};

struct ThreadProfile {
    Arm64ExecutionProfile value;
    HANDLE thread = nullptr;

    ~ThreadProfile()
    {
        if (thread)
            CloseHandle(thread);
    }
};

struct State {
    std::mutex mutex;
    Arm64ExecutionStatistics totals;
    std::map<uint64_t, std::string> names;
    std::map<std::tuple<uint64_t, uintptr_t, uint32_t>, uint64_t> hotspots;
    std::map<std::string, uint64_t, std::less<>> imports;
    std::atomic<uint64_t> leafCalls{0}, leafInstructions{0};
    std::vector<std::unique_ptr<ThreadProfile>> threads;
    const ULONGLONG startedAt = GetTickCount64();
    ULONGLONG reportAt = startedAt + 5000;
};

State& Global()
{
    // Native leaf entries and thread-local cleanup can outlive C++ statics.
    static auto* state = new State;
    return *state;
}

unsigned Mode()
{
    static const unsigned mode = [] {
        const HostState preserve;
        char value[8]{};
        const auto length = GetEnvironmentVariableA("STONE_ARM64_PROFILE", value, sizeof(value));
        if (length == 1 && value[0] == '1')
            return 1u;
        return length == 7 && std::string_view(value, 7) == "imports" ? 2u : 0u;
    }();
    return mode;
}

bool Enabled()
{
    return Mode() != 0;
}

void Merge(State& state, Arm64ExecutionProfile& profile)
{
    state.totals.interpretedInstructions += profile.instructions;
    state.totals.mappedInstructions += profile.mappedInstructions;
    state.totals.interpreterRuns += profile.runs;
    for (size_t i = 0; i < profile.opcodeGroups.size(); ++i)
        state.totals.opcodeGroups[i] += profile.opcodeGroups[i];
    profile.instructions = profile.mappedInstructions = profile.runs = 0;
    profile.opcodeGroups.fill(0);
    for (auto& [name, count] : profile.imports) {
        const auto found = state.imports.find(name);
        if (found != state.imports.end())
            found->second += count;
        else
            state.imports[state.imports.size() < 4096 ? name : "<other-imports>"] += count;
    }
    profile.recentImports = {};
    profile.imports.clear();
}

void CollectFinishedThreads(State& state)
{
    for (auto item = state.threads.begin(); item != state.threads.end();) {
        if (WaitForSingleObject((*item)->thread, 0) != WAIT_OBJECT_0) {
            ++item;
            continue;
        }
        // Wait for the actual thread exit, including FLS cleanup after C++ TLS
        // destructors. Never inspect another running thread's local counters.
        Merge(state, (*item)->value);
        item = state.threads.erase(item);
    }
}

void Print(State& state)
{
    const auto& totals = state.totals;
    std::fprintf(stderr, "[arm64-profile] snapshot=published-counters live-thread-local-tails-may-be-pending=1\n");
    std::fprintf(stderr, "[arm64-profile] mode=%s instruction-counting=%d\n", Mode() == 1 ? "instructions" : "imports",
                 Mode() == 1);
    std::fprintf(
        stderr,
        "[arm64-profile] elapsed-ms=%llu interpreted=%llu mapped=%llu interpreter-runs=%llu native-leaf-calls=%llu native-leaf-instructions=%llu samples=%llu dropped=%llu errors=%llu\n",
        GetTickCount64() - state.startedAt, totals.interpretedInstructions, totals.mappedInstructions,
        totals.interpreterRuns, state.leafCalls.load(std::memory_order_relaxed),
        state.leafInstructions.load(std::memory_order_relaxed), totals.samples, totals.droppedSamples,
        profileErrors.load(std::memory_order_relaxed));
    const auto blocks = GetArm64BlockStatistics();
    std::fprintf(
        stderr,
        "[arm64-profile] native-block-calls=%llu native-block-instructions=%llu block-lookups=%llu block-hits=%llu block-compiled=%llu block-unsupported=%llu block-invalidations=%llu block-evictions=%llu block-failures=%llu block-entries=%zu block-warmup-skipped=%llu\n",
        blocks.executions, blocks.instructions, blocks.lookups, blocks.hits, blocks.compiled, blocks.unsupported,
        blocks.invalidations, blocks.evictions, blocks.failures, blocks.entries, blocks.warmupSkipped);
    std::fprintf(
        stderr,
        "[arm64-profile] block-unwind-tables=%llu block-active-unwind-tables=%llu block-code-bytes=%llu block-unprofitable=%llu block-dispatches=%llu\n",
        blocks.unwindTables, blocks.activeUnwindTables, blocks.committedCodeBytes, blocks.unprofitable,
        blocks.dispatches);
    std::string groups = "[arm64-profile] opcode-groups:";
    for (size_t i = 0; i < totals.opcodeGroups.size(); ++i) {
        groups += ' ';
        groups += "0123456789abcdef"[i];
        groups += '=';
        groups += std::to_string(totals.opcodeGroups[i]);
    }
    std::fprintf(stderr, "%s\n", groups.c_str());
    using Hot = decltype(state.hotspots)::value_type;
    std::vector<const Hot*> hottest;
    for (const auto& item : state.hotspots)
        hottest.push_back(&item);
    const auto count = std::min(hottest.size(), size_t{16});
    std::partial_sort(hottest.begin(), hottest.begin() + count, hottest.end(), [](const Hot* a, const Hot* b) {
        return a->second > b->second;
    });
    for (size_t i = 0; i < count; ++i) {
        const auto& [key, samples] = *hottest[i];
        const auto& [image, offset, instruction] = key;
        const auto name = state.names.find(image);
        std::fprintf(stderr,
                     "[arm64-profile] hot kind=%s image=%llu name=%s offset=0x%llx instruction=0x%08x samples=%llu\n",
                     image ? "elf" : "mapped", image, name == state.names.end() ? "unknown" : name->second.c_str(),
                     static_cast<unsigned long long>(offset), instruction, samples);
    }
    using Import = decltype(state.imports)::value_type;
    std::vector<const Import*> imports;
    for (const auto& item : state.imports)
        imports.push_back(&item);
    const auto importCount = std::min(imports.size(), size_t{16});
    std::partial_sort(imports.begin(), imports.begin() + importCount, imports.end(),
                      [](const Import* a, const Import* b) {
                          return a->second > b->second;
                      });
    for (size_t i = 0; i < importCount; ++i)
        std::fprintf(stderr, "[arm64-profile] import name=%s calls=%llu\n", imports[i]->first.c_str(),
                     imports[i]->second);
    // These can fall out of the top imports after a mapping fix. Report explicit
    // zeros so absence from the hotspot list is never mistaken for a zero count.
    for (const auto name : {"mmap", "mmap64", "munmap", "mprotect", "msync"}) {
        const auto found = state.imports.find(name);
        std::fprintf(stderr, "[arm64-profile] memory-import name=%s calls=%llu\n", name,
                     found == state.imports.end() ? uint64_t{0} : found->second);
    }
    std::fflush(stderr);
}
}

Arm64ExecutionProfile* CurrentArm64ExecutionProfile()
{
    if (!Enabled())
        return nullptr;
    thread_local Arm64ExecutionProfile* profile = []() noexcept -> Arm64ExecutionProfile* {
        const HostState preserve;
        try {
            auto owner = std::make_unique<ThreadProfile>();
            owner->value.instructionProfiling = Mode() == 1;
            if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &owner->thread, 0, FALSE,
                                 DUPLICATE_SAME_ACCESS))
                throw std::runtime_error("cannot retain profiling thread identity");
            owner->value.random = 0x9e3779b97f4a7c15ull ^ (uint64_t{GetCurrentThreadId()} << 32) ^ GetTickCount64();
            if (!owner->value.random)
                owner->value.random = 1;
            auto& state = Global();
            const std::lock_guard lock(state.mutex);
            CollectFinishedThreads(state);
            if (state.threads.size() >= 4096)
                throw std::runtime_error("too many profiling threads");
            auto* result = &owner->value;
            state.threads.push_back(std::move(owner));
            return result;
        }
        catch (...) {
            profileErrors.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
    }();
    return profile;
}

void RegisterArm64ProfileImage(uint64_t identity, std::string_view name) noexcept
{
    if (!Enabled())
        return;
    const HostState preserve;
    try {
        auto& state = Global();
        const std::lock_guard lock(state.mutex);
        if (state.names.size() < 65536)
            state.names[identity] = name.substr(0, 256);
    }
    catch (...) {
        profileErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

void FlushArm64ExecutionProfile(Arm64ExecutionProfile& profile) noexcept
{
    const HostState preserve;
    try {
        auto& state = Global();
        const std::lock_guard lock(state.mutex);
        Merge(state, profile);
    }
    catch (...) {
        profileErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

void RecordArm64ProfileImport(Arm64ExecutionProfile& profile, std::string_view name) noexcept
{
    const HostState preserve;
    try {
        auto& recent = profile.recentImports[std::hash<std::string_view>{}(name) & (profile.recentImports.size() - 1)];
        if (recent.count && recent.name == name) {
            ++*recent.count;
            return;
        }
        auto found = profile.imports.find(name);
        if (found == profile.imports.end())
            found =
                profile.imports
                    .try_emplace(
                        profile.imports.size() < 512 && name.size() <= 256 ? std::string(name) : "<other-imports>", 0)
                    .first;
        ++found->second;
        recent = {std::string_view(found->first), &found->second};
    }
    catch (...) {
        profileErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

void SampleArm64Execution(Arm64ExecutionProfile& profile, uint64_t image, uintptr_t offset,
                          uint32_t instruction) noexcept
{
    const HostState preserve;
    try {
        auto& state = Global();
        const std::lock_guard lock(state.mutex);
        Merge(state, profile);
        ++state.totals.samples;
        const auto key = std::tuple{image, offset, instruction};
        auto found = state.hotspots.find(key);
        if (found != state.hotspots.end())
            ++found->second;
        else if (state.hotspots.size() < MaximumHotspots)
            state.hotspots.emplace(key, 1);
        else
            ++state.totals.droppedSamples;
        const auto now = GetTickCount64();
        if (now >= state.reportAt) {
            state.reportAt = now + 5000;
            CollectFinishedThreads(state);
            Print(state);
        }
    }
    catch (...) {
        profileErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

Arm64ExecutionStatistics GetArm64ExecutionStatistics()
{
    const HostState preserve;
    Arm64ExecutionStatistics result;
    if (!(result.enabled = Enabled()))
        return result;
    if (auto* profile = CurrentArm64ExecutionProfile())
        FlushArm64ExecutionProfile(*profile);
    auto& state = Global();
    const std::lock_guard lock(state.mutex);
    CollectFinishedThreads(state);
    result = state.totals;
    result.enabled = true;
    result.instructionProfiling = Mode() == 1;
    result.elapsedMilliseconds = GetTickCount64() - state.startedAt;
    result.nativeLeafCalls = state.leafCalls.load(std::memory_order_relaxed);
    result.nativeLeafInstructions = state.leafInstructions.load(std::memory_order_relaxed);
    const auto blocks = GetArm64BlockStatistics();
    result.nativeBlockCalls = blocks.executions;
    result.nativeBlockInstructions = blocks.instructions;
    result.errors = profileErrors.load(std::memory_order_relaxed);
    for (const auto& [key, samples] : state.hotspots) {
        const auto& [image, offset, instruction] = key;
        const auto name = state.names.find(image);
        result.hotspots.push_back({image, offset, instruction, samples, name == state.names.end() ? "" : name->second});
    }
    for (const auto& item : state.imports)
        result.imports.push_back(item);
    return result;
}

void DumpArm64ExecutionProfile() noexcept
{
    const HostState preserve;
    if (Arm64BlockDiskCacheEnabled()) {
        const auto disk = GetArm64DiskCacheStatistics();
        std::fprintf(
            stderr,
            "[arm64-cache] images-loaded=%llu blocks-loaded=%llu hits=%llu misses=%llu rejected=%llu recorded=%llu files-written=%llu errors=%llu limited=%llu evicted=%llu merge-reads=%llu merge-skipped=%llu blocks-merged=%llu\n",
            disk.imagesLoaded, disk.blocksLoaded, disk.hits, disk.misses, disk.rejected, disk.recorded,
            disk.filesWritten, disk.errors, disk.limited, disk.evicted, disk.mergeReads, disk.mergeSkipped,
            disk.blocksMerged);
    }
    if (Arm64CacheTimingStart()) {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const auto us = [&](uint64_t ticks) {
            return ticks * 1000000 / static_cast<uint64_t>(frequency.QuadPart);
        };
        const auto disk = GetArm64DiskCacheStatistics();
        const auto blocks = GetArm64BlockStatistics();
        std::fprintf(
            stderr,
            "[arm64-cache-timing] configure-us=%llu images-us=%llu admission-queries=%llu admission-us=%llu lookup-us=%llu record-us=%llu flush-us=%llu compile-us=%llu publish-us=%llu\n",
            us(disk.configureTicks), us(disk.imageTicks), disk.admissionQueries, us(disk.admissionTicks),
            us(disk.lookupTicks), us(disk.recordTicks), us(disk.flushTicks), us(blocks.compileTicks),
            us(blocks.publishTicks));
        std::fprintf(stderr,
                     "[arm64-cache-io-timing] protect-us=%llu unprotect-us=%llu parse-us=%llu serialize-us=%llu\n",
                     us(disk.protectTicks), us(disk.unprotectTicks), us(disk.parseTicks), us(disk.serializeTicks));
    }
    if (!Enabled())
        return;
    try {
        if (auto* profile = CurrentArm64ExecutionProfile())
            FlushArm64ExecutionProfile(*profile);
        auto& state = Global();
        const std::lock_guard lock(state.mutex);
        CollectFinishedThreads(state);
        Print(state);
    }
    catch (...) {
        profileErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

std::vector<uint8_t> ProfileArm64Leaf(std::vector<uint8_t> code, uint32_t instructions)
{
    if (!Enabled())
        return code;
    const HostState preserve;
    try {
        auto& state = Global();
        static_assert(std::atomic<uint64_t>::is_always_lock_free && sizeof(std::atomic<uint64_t>) == 8);
        std::vector<uint8_t> prefix;
        const auto pointer = [&](const void* address) {
            prefix.insert(prefix.end(), {0x49, 0xbb}); // mov r11,imm64; r11 is the leaf translator's scratch.
            const auto bits = reinterpret_cast<uintptr_t>(address);
            for (unsigned i = 0; i < 8; ++i)
                prefix.push_back(static_cast<uint8_t>(bits >> (8 * i)));
        };
        pointer(&state.leafCalls);
        prefix.insert(prefix.end(), {0xf0, 0x49, 0xff, 0x03}); // lock inc qword [r11].
        pointer(&state.leafInstructions);
        prefix.insert(prefix.end(), {0xf0, 0x49, 0x81, 0x03}); // lock add qword [r11],imm32.
        for (unsigned i = 0; i < 4; ++i)
            prefix.push_back(static_cast<uint8_t>(instructions >> (8 * i)));
        prefix.insert(prefix.end(), code.begin(), code.end());
        return prefix;
    }
    catch (...) {
        profileErrors.fetch_add(1, std::memory_order_relaxed);
        return code;
    }
}
} // namespace stone::windows
