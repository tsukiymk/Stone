#include "cpu/arm64/cache/Arm64CacheFormat.h"
#include "platform/windows/Arm64CacheStorage.h"
#include "Arm64BlockCache.h"
#include "../../../../host/WindowsPath.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace stone::windows {
namespace {
using namespace arm64_cache;
namespace fs = std::filesystem;
// Constant-initialized so the empty fast path need not initialize Cache().
std::atomic<bool> admissionsReady{false};

struct Timer {
    uint64_t& ticks;
    uint64_t start;

    ~Timer()
    {
        if (start)
            ticks += Arm64CacheTimingElapsed(start);
    }
};

struct State {
    std::mutex mutex;
    std::atomic<bool> enabled{false};
    bool eagerAdmission = false;
    bool preload = false;
    fs::path root;
    Digest translator{};
    std::map<uint64_t, std::shared_ptr<Bundle>> images;
    std::map<Digest, std::shared_ptr<Bundle>> bundles;
    size_t bytes = 0;
    bool pinBaselines = true;
    Arm64DiskCacheStatistics stats;
    // Admission is consulted on interpreter hotness collisions, millions of
    // times per launch. Never lock or search artifact maps on this path.
    std::array<std::atomic<uint64_t>, AdmissionBits / 64> admission{};
    std::atomic<uint64_t> admissionQueries{0}, admissionTicks{0};
};

State& Cache()
{
    static auto* state = new State;
    return *state;
}

uint64_t AdmissionHash(uint64_t image, uintptr_t pc, uint32_t minimum)
{
    auto hash = pc ^ (image * 0x9e3779b97f4a7c15ull) ^ (uint64_t{minimum} << 57);
    hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ull;
    hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebull;
    return hash ^ (hash >> 31);
}

void Admit(State& state, uint64_t image, const Bundle& bundle)
{
    if (!state.eagerAdmission)
        return;
    bool added = false;
    for (const auto& [key, saved] : bundle.blocks)
        if (saved.disk) {
            const auto hash = AdmissionHash(image, key.first, key.second);
            for (auto bit : {hash & (AdmissionBits - 1), (hash >> 22) & (AdmissionBits - 1)})
                state.admission[bit >> 6].fetch_or(uint64_t{1} << (bit & 63), std::memory_order_relaxed);
            added = true;
        }
    if (added)
        admissionsReady.store(true, std::memory_order_release);
}

// Called with the state mutex held. Keep the Bundle object stable: several
// live ELF registrations may refer to this same authenticated source identity.
void EnsureLoaded(State& state, Bundle& bundle)
{
    if (bundle.loadAttempted)
        return;
    bundle.loadAttempted = true;
    try {
        auto saved = Read(Open(state.root / Name(bundle.key)), bundle.key, state.stats, state.pinBaselines);
        if (!saved)
            return;
        if (state.bytes + saved->bytes > MaximumMemory) {
            ++state.stats.limited;
            return;
        }
        Check(bundle.blocks.empty() && !bundle.bytes && !bundle.dirty);
        bundle.blocks = std::move(saved->blocks);
        bundle.baseline = std::move(saved->baseline);
        bundle.bytes = saved->bytes;
        state.bytes += bundle.bytes;
        ++state.stats.imagesLoaded;
        state.stats.blocksLoaded += bundle.blocks.size();
    }
    catch (...) {
        ++state.stats.rejected;
    }
}

void Trim(State& state)
{
    auto files = ListFiles(state.root);
    uint64_t total = 0;
    for (const auto& file : files)
        total += file.bytes;
    std::sort(files.begin(), files.end(), [](const StoredFile& a, const StoredFile& b) {
        return a.time < b.time;
    });
    size_t count = files.size();
    for (const auto& file : files) {
        if (total <= MaximumDisk && count <= MaximumFiles)
            break;
        if (RemoveFile(file.path)) {
            // Release our pin on an evicted file instead of retaining deleted
            // cache storage until this process exits.
            if (const auto bundle = state.bundles.find(file.key); bundle != state.bundles.end())
                bundle->second->baseline.reset();
            total -= file.bytes;
            --count;
            ++state.stats.evicted;
        }
    }
}
}

uint64_t Arm64CacheTimingStart() noexcept
{
    static const bool enabled = [] {
        const HostState preserve;
        char value[2]{};
        return GetEnvironmentVariableA("STONE_ARM64_CACHE_TIMING", value, 2) == 1 && value[0] == '1';
    }();
    LARGE_INTEGER now{};
    if (enabled)
        QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

uint64_t Arm64CacheTimingElapsed(uint64_t start) noexcept
{
    if (!start)
        return 0;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart) - start;
}

bool Arm64BlockDiskCacheEnabled() noexcept
{
    return Cache().enabled.load(std::memory_order_acquire);
}

bool ConfigureArm64BlockDiskCache(const fs::path& directory) noexcept
{
    const HostState preserve;
    const auto start = Arm64CacheTimingStart();
    auto& state = Cache();
    const std::lock_guard lock(state.mutex);
    const Timer timer{state.stats.configureTicks, start};
    try {
        if (state.enabled.load())
            return fs::equivalent(state.root, directory);
        fs::create_directories(directory);
        state.root = ExtendedWindowsPath(fs::canonical(directory));
        state.translator = TranslatorIdentity();
        wchar_t eager[2]{};
        state.eagerAdmission = GetEnvironmentVariableW(L"STONE_ARM64_CACHE_EAGER", eager, 2) == 1 && eager[0] == L'1';
        wchar_t preload[2]{};
        state.preload = GetEnvironmentVariableW(L"STONE_ARM64_CACHE_PRELOAD", preload, 2) == 1 && preload[0] == L'1';
        state.enabled.store(true, std::memory_order_release);
        return true;
    }
    catch (...) {
        ++state.stats.errors;
        return false;
    }
}

void RegisterArm64BlockCacheImage(uint64_t image, std::span<const uint8_t> file) noexcept
{
    if (!Arm64BlockDiskCacheEnabled())
        return;
    const HostState preserve;
    auto& state = Cache();
    try {
        const auto start = Arm64CacheTimingStart();
        const std::lock_guard lock(state.mutex);
        const Timer timer{state.stats.imageTicks, start};
        const auto key = SourceKey(state.translator, file);
        if (state.images.contains(image))
            return;
        auto found = state.bundles.find(key);
        if (found == state.bundles.end()) {
            if (state.bundles.size() >= MaximumFiles) {
                ++state.stats.limited;
                return;
            }
            auto bundle = std::make_shared<Bundle>();
            bundle->key = key;
            found = state.bundles.emplace(key, std::move(bundle)).first;
        }
        // Eager hints require an index up front. Normal hot-only admission can
        // wait until the first actual block lookup instead of reading every ELF.
        if (state.eagerAdmission || state.preload)
            EnsureLoaded(state, *found->second);
        state.images.emplace(image, found->second);
        // Only products loaded from a previous process qualify. Blocks just
        // recorded by this process keep the original cold hotness policy.
        Admit(state, image, *found->second);
    }
    catch (...) {
        const std::lock_guard lock(state.mutex);
        ++state.stats.errors;
    }
}

void UnregisterArm64BlockCacheImage(uint64_t image) noexcept
{
    if (!Arm64BlockDiskCacheEnabled())
        return;
    const HostState preserve;
    auto& state = Cache();
    const std::lock_guard lock(state.mutex);
    state.images.erase(image);
}

bool MayHaveArm64DiskBlock(uint64_t image, uintptr_t pc, uint32_t minimum) noexcept
{
    if (!image)
        return false;
    // Empty or hot-only indexes cannot contain an admission hint. Avoid error
    // state saves, hashing and timing calls for millions of cold queries.
    if (!admissionsReady.load(std::memory_order_acquire))
        return false;
    const HostState preserve;
    const auto start = Arm64CacheTimingStart();
    auto& state = Cache();
    const auto hash = AdmissionHash(image, pc, minimum);
    const auto first = hash & (AdmissionBits - 1), second = (hash >> 22) & (AdmissionBits - 1);
    const bool present =
        (state.admission[first >> 6].load(std::memory_order_relaxed) & (uint64_t{1} << (first & 63))) &&
        (state.admission[second >> 6].load(std::memory_order_relaxed) & (uint64_t{1} << (second & 63)));
    if (start) {
        state.admissionQueries.fetch_add(1, std::memory_order_relaxed);
        state.admissionTicks.fetch_add(Arm64CacheTimingElapsed(start), std::memory_order_relaxed);
    }
    return present;
}

std::optional<Arm64BlockArtifact> LoadArm64DiskBlock(uint64_t image, uintptr_t pc, uint32_t minimum,
                                                     std::span<const uint8_t> source) noexcept
{
    if (!image || !Arm64BlockDiskCacheEnabled())
        return {};
    const HostState preserve;
    const auto start = Arm64CacheTimingStart();
    auto& state = Cache();
    const std::lock_guard lock(state.mutex);
    const Timer timer{state.stats.lookupTicks, start};
    try {
        const auto bundle = state.images.find(image);
        if (bundle != state.images.end()) {
            EnsureLoaded(state, *bundle->second);
            const auto found = bundle->second->blocks.find({pc, minimum});
            if (found != bundle->second->blocks.end()) {
                const auto& a = found->second.artifact;
                if (source.size() >= a.source.size() && !std::memcmp(source.data(), a.source.data(), a.source.size())) {
                    ++state.stats.hits;
                    return a;
                }
                ++state.stats.rejected;
            }
        }
        ++state.stats.misses;
        return {};
    }
    catch (...) {
        ++state.stats.errors;
        return {};
    }
}

void RecordArm64DiskBlock(uint64_t image, uintptr_t pc, uint32_t minimum, Arm64BlockArtifact artifact) noexcept
{
    if (!image || !Arm64BlockDiskCacheEnabled())
        return;
    const HostState preserve;
    const auto start = Arm64CacheTimingStart();
    auto& state = Cache();
    const std::lock_guard lock(state.mutex);
    const Timer timer{state.stats.recordTicks, start};
    try {
        Validate(artifact, minimum);
        const auto found = state.images.find(image);
        if (found == state.images.end())
            return;
        auto& bundle = *found->second;
        EnsureLoaded(state, bundle);
        const auto bytes = Bytes(artifact);
        const auto existing = bundle.blocks.find({pc, minimum});
        const size_t previous = existing == bundle.blocks.end() ? 0 : Bytes(existing->second.artifact);
        if (bytes + state.bytes - previous > MaximumMemory || bytes + bundle.bytes - previous > MaximumImage ||
            (existing == bundle.blocks.end() && bundle.blocks.size() >= MaximumBlocks)) {
            ++state.stats.limited;
            return;
        }
        bundle.blocks.insert_or_assign(Key{pc, minimum}, Saved{std::move(artifact), false});
        bundle.dirty = true;
        bundle.bytes = bundle.bytes + bytes - previous;
        state.bytes = state.bytes + bytes - previous;
        ++state.stats.recorded;
    }
    catch (...) {
        ++state.stats.errors;
    }
}

void FlushArm64BlockDiskCache() noexcept
{
    if (!Arm64BlockDiskCacheEnabled())
        return;
    const HostState preserve;
    const auto start = Arm64CacheTimingStart();
    auto& state = Cache();
    const std::lock_guard lock(state.mutex);
    const Timer timer{state.stats.flushTicks, start};
    try {
        auto writer = AcquireWriter(state.root);
        if (!writer) {
            ++state.stats.limited;
            return;
        }
        for (auto& [key, bundle] : state.bundles) {
            if (!bundle->dirty)
                continue;
            try {
                // Merge another completed process's bundle while holding the
                // cross-process writer lease; readers only see atomic files.
                try {
                    auto current = Open(state.root / Name(key));
                    if (SameFile(bundle->baseline, current))
                        ++state.stats.mergeSkipped;
                    else if (auto previous = Read(std::move(current), key, state.stats, state.pinBaselines)) {
                        ++state.stats.mergeReads;
                        for (auto& [entry, saved] : previous->blocks) {
                            const auto bytes = Bytes(saved.artifact);
                            if (!bundle->blocks.contains(entry) && bundle->bytes + bytes <= MaximumImage &&
                                state.bytes + bytes <= MaximumMemory && bundle->blocks.size() < MaximumBlocks) {
                                bundle->blocks.emplace(entry, std::move(saved));
                                bundle->bytes += bytes;
                                state.bytes += bytes;
                                ++state.stats.blocksMerged;
                            }
                        }
                    }
                }
                catch (...) {
                    ++state.stats.rejected;
                }
                bundle->baseline = Write(state.root, *bundle, state.stats, state.pinBaselines, [&] {
                    for (auto& [otherKey, saved] : state.bundles)
                        saved->baseline.reset();
                });
                if (!state.pinBaselines)
                    for (auto& [otherKey, saved] : state.bundles)
                        saved->baseline.reset();
                bundle->dirty = false;
                ++state.stats.filesWritten;
            }
            catch (const std::exception& failure) {
                if (!state.stats.errors)
                    std::fprintf(stderr, "[arm64-cache] %s\n", failure.what());
                ++state.stats.errors;
            }
            catch (...) {
                ++state.stats.errors;
            }
        }
        Trim(state);
    }
    catch (...) {
        ++state.stats.errors;
    }
}

Arm64DiskCacheStatistics GetArm64DiskCacheStatistics() noexcept
{
    const HostState preserve;
    auto& state = Cache();
    const std::lock_guard lock(state.mutex);
    auto result = state.stats;
    result.admissionQueries = state.admissionQueries.load(std::memory_order_relaxed);
    result.admissionTicks = state.admissionTicks.load(std::memory_order_relaxed);
    return result;
}
}
