#include "ArtJit.h"
#include "ArtGs.h"
#include "../../loader/ElfImage.h"
#include "../../abi/AndroidLibraryImports.h"
#include "../../loader/AndroidSystemElf.h"
#include "../../abi/Arm64HostJni.h"
#include "../../cpu/arm64/Arm64Initializer.h"
#include "memory/GuestMappings.h"
#include "hle/system/stub-libc.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace stone::windows {
namespace {
// GDB's public JIT registration prefix and Android's versioned extension.
// This callback runs synchronously under ART's JIT/debug locks, before the
// method entry point is published. It must not call back into managed code.
struct Entry {
    uintptr_t next, previous;
    const uint8_t* file;
    uint64_t size;
};

struct Descriptor {
    uint32_t version, action;
    const Entry* relevant;
    uintptr_t head;
    char magic[8];
    uint32_t flags, descriptorSize, entrySize, sequence;
    uint64_t timestamp;
};

static_assert(sizeof(Entry) == 32 && sizeof(Descriptor) == 56);

struct Method {
    uintptr_t end;
    std::unique_ptr<ArtGsCode> gs;
    std::string name;
};

struct State {
    const Descriptor* descriptor = nullptr;
    void (*notify)() = nullptr;
    std::mutex mutex;
    std::map<uintptr_t, Method> methods;
    std::map<uintptr_t, ArtGsCode*> replacements;
    std::atomic<bool> enabled{false};
    bool trace = false;
    bool arm64 = false;
    const ElfImage* runtime = nullptr;
    std::atomic<size_t> compiled{0}, instructions{0};
    std::atomic<size_t> tlsRecoveries{0}, breakpoints{0};
};

State& Jit()
{
    static State state;
    return state;
}

void ReadRegistration(uintptr_t source, void* destination, size_t size)
{
    auto& state = Jit();
    if (!state.arm64) {
        std::memcpy(destination, reinterpret_cast<const void*>(source), size);
        return;
    }
    if (ReadArm64GuestAllocation(source, destination, size))
        return;
    const auto* data = state.runtime->DataAt(source, size, false);
    if (!data)
        data = AndroidMappedArm64Data(source, size, false);
    if (!data)
        throw std::runtime_error("ART JIT registration is outside guest readable memory");
    std::memcpy(destination, data, size);
}

bool Arm64PublishedCode(uintptr_t address, size_t size)
{
    if (!size || (address & 3) || (size & 3) || size > UINTPTR_MAX - address)
        return false;
    const auto end = address + size;
    while (address < end) {
        const auto range = AndroidArm64CodeAt(address);
        if (range.empty())
            return false;
        address += std::min(range.size(), end - address);
    }
    return true;
}

void Notify() noexcept
{
    auto& state = Jit();
    std::string methodName;
    try {
        Descriptor descriptor{};
        ReadRegistration(reinterpret_cast<uintptr_t>(state.descriptor), &descriptor, sizeof(descriptor));
        if (descriptor.version != 1 || std::memcmp(descriptor.magic, "Android2", 8) || descriptor.flags ||
            descriptor.descriptorSize < sizeof(Descriptor) || descriptor.entrySize < sizeof(Entry))
            throw std::runtime_error("unsupported ART JIT registration descriptor");
        if (descriptor.action == 1 && descriptor.relevant) {
            Entry entry{};
            ReadRegistration(reinterpret_cast<uintptr_t>(descriptor.relevant), &entry, sizeof(entry));
            if (!entry.file || entry.size > 16 * 1024 * 1024)
                throw std::runtime_error("invalid ART JIT symbol file");
            std::vector<uint8_t> snapshot;
            if (state.arm64) {
                snapshot.resize(static_cast<size_t>(entry.size));
                ReadRegistration(reinterpret_cast<uintptr_t>(entry.file), snapshot.data(), snapshot.size());
                entry.file = snapshot.data();
            }
            constexpr uint8_t compressed[]{0xfd, 0x37, 0x7a, 0x58, 0x5a, 0};
            // ART repacks older debug records, sometimes as XZ streams. All
            // methods in those records were adapted on their original event.
            if (entry.size < sizeof(compressed) || std::memcmp(entry.file, compressed, sizeof(compressed))) {
                const auto symbols = ElfDebugSymbols({entry.file, static_cast<size_t>(entry.size)});
                const std::lock_guard lock(state.mutex);
                for (const auto& [name, symbol] : symbols) {
                    if (!symbol.code || !symbol.size)
                        continue;
                    methodName = name;
                    if (symbol.size > 16 * 1024 * 1024 ||
                        !(state.arm64 ? Arm64PublishedCode(symbol.address, static_cast<size_t>(symbol.size))
                                      : AndroidExecutableMapping(symbol.address, static_cast<size_t>(symbol.size))))
                        throw std::runtime_error("ART JIT symbol is outside guest executable memory");
                    const auto start = static_cast<uintptr_t>(symbol.address),
                               end = start + static_cast<size_t>(symbol.size);
                    auto found = state.methods.find(start);
                    if (found != state.methods.end() && found->second.end == end &&
                        (state.arm64 ? found->second.name == name : found->second.gs->MatchesPublishedCode()))
                        continue;
                    // Code collection can reuse or subdivide an earlier method's
                    // allocation. Remove all overlapping adapters before reuse.
                    found = state.methods.upper_bound(start);
                    if (found != state.methods.begin() && std::prev(found)->second.end > start)
                        --found;
                    while (found != state.methods.end() && found->first < end) {
                        if (found->second.gs)
                            state.replacements.erase(found->second.gs->ReplacementCodeBegin());
                        found = state.methods.erase(found);
                    }
                    if (state.arm64) {
                        state.compiled.fetch_add(1, std::memory_order_relaxed);
                        state.methods.emplace(start, Method{end, nullptr, name});
                        if (state.trace)
                            std::fprintf(stderr, "[art-jit] published ARM64 %s at 0x%llx (%llu bytes)\n", name.c_str(),
                                         static_cast<unsigned long long>(start),
                                         static_cast<unsigned long long>(symbol.size));
                        continue;
                    }
                    auto gs = std::make_unique<ArtGsCode>(
                        std::span<uint8_t>{reinterpret_cast<uint8_t*>(start), static_cast<size_t>(symbol.size)});
                    state.compiled.fetch_add(1, std::memory_order_relaxed);
                    state.instructions.fetch_add(gs->Size(), std::memory_order_relaxed);
                    if (state.trace)
                        std::fprintf(stderr, "[art-jit] adapted %zu GS instructions in %s at 0x%llx (%llu bytes)\n",
                                     gs->Size(), name.c_str(), static_cast<unsigned long long>(start),
                                     static_cast<unsigned long long>(symbol.size));
                    if (gs->Size()) {
                        if (gs->ReplacementCodeBegin())
                            state.replacements.emplace(gs->ReplacementCodeBegin(), gs.get());
                        state.methods.emplace(start, Method{end, std::move(gs), name});
                    }
                }
            }
        }
        // Keep the image's debugger notification available to attached tools.
        state.notify();
    }
    catch (const std::exception& error) {
        // Never let an unadapted method be published after a failed rewrite.
        std::fprintf(stderr, "[art-jit] publication failed in %s: %s\n", methodName.c_str(), error.what());
        std::fflush(stderr);
        ExitProcess(3);
    }
}
}

void ConfigureArtJit(ElfImage& runtime)
{
    auto& state = Jit();
    if (state.descriptor)
        throw std::runtime_error("ART JIT was already configured");
    const auto descriptor = runtime.Export("__jit_debug_descriptor");
    const auto slot = runtime.Export("__jit_debug_register_code_ptr");
    auto* pointer = reinterpret_cast<uintptr_t*>(runtime.DataAt(slot, sizeof(uintptr_t), true));
    const auto* data = runtime.DataAt(descriptor, sizeof(Descriptor), false);
    if (!pointer || !data || !runtime.ContainsCode(*pointer, 1))
        throw std::runtime_error("image has no supported ART JIT publication interface");
    const bool arm64 = runtime.Architecture() == ElfImage::Machine::Arm64;
    const auto callback = arm64 ? PrepareArm64HostJniTarget(
                                      "Stone ARM64 ART JIT publication", "()V",
                                      ImportAndroidScalars<decltype(&Notify)>("Stone ARM64 ART JIT publication target",
                                                                              reinterpret_cast<uintptr_t>(&Notify)))
                                : Import("Stone ART JIT publication", &Notify);
    state.notify = reinterpret_cast<void (*)()>(MakeNativeAndroidThunk(*pointer, "()V"));
    wchar_t trace[2]{};
    state.trace = GetEnvironmentVariableW(L"STONE_ART_DIAGNOSTICS", trace, 2) == 1 && trace[0] == L'1';
    state.descriptor = reinterpret_cast<const Descriptor*>(data);
    state.arm64 = arm64;
    state.runtime = &runtime;
    state.enabled.store(true, std::memory_order_release);
    *pointer = callback;
}

bool DispatchArtJitBreakpoint(uintptr_t address, CONTEXT& context)
{
    auto& state = Jit();
    if (!state.enabled.load(std::memory_order_acquire) || state.arm64)
        return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) || region.Type == MEM_IMAGE)
        return false;
    const std::lock_guard lock(state.mutex);
    auto found = state.methods.upper_bound(address);
    if (found == state.methods.begin())
        return false;
    --found;
    if (address >= found->second.end || !found->second.gs->Dispatch(address, context))
        return false;
    state.breakpoints.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool RecoverArtJitTlsLoad(CONTEXT& context)
{
    auto& state = Jit();
    if (!state.enabled.load(std::memory_order_acquire) || state.arm64)
        return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void*>(context.Rip), &region, sizeof(region)) || region.Type == MEM_IMAGE)
        return false;
    const std::lock_guard lock(state.mutex);
    auto found = state.replacements.upper_bound(static_cast<uintptr_t>(context.Rip));
    if (found == state.replacements.begin())
        return false;
    --found;
    if (context.Rip >= found->second->ReplacementCodeEnd() || !found->second->RecoverTlsLoad(context))
        return false;
    state.tlsRecoveries.fetch_add(1, std::memory_order_relaxed);
    return true;
}

ArtJitStatistics GetArtJitStatistics()
{
    return {Jit().compiled.load(std::memory_order_relaxed), Jit().instructions.load(std::memory_order_relaxed),
            Jit().tlsRecoveries.load(std::memory_order_relaxed), Jit().breakpoints.load(std::memory_order_relaxed)};
}

bool ArtJitMethodCompiled(std::string_view name)
{
    auto& state = Jit();
    const std::lock_guard lock(state.mutex);
    for (const auto& [start, method] : state.methods)
        if (method.name == name)
            return true;
    return false;
}
}
