#include "platform/windows/ExecutableMemory.h"
#include "loader/AndroidLibraryState.h"

namespace stone::windows {
using namespace android_libraries;

namespace android_libraries {
uintptr_t CallElfResolver(uintptr_t resolver, LibraryNamespace& scope)
{
    Call call(resolver, "()J", false, true);
    call.scope = &scope;
    const auto target = CallAndroid(&call, nullptr, nullptr);
    if (target) {
        MEMORY_BASIC_INFORMATION memory{};
        constexpr DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!VirtualQuery(reinterpret_cast<void*>(target), &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
            !(memory.Protect & executable) || (memory.Protect & PAGE_GUARD))
            throw std::runtime_error("ELF indirect resolver returned a non-executable address");
    }
    return target;
}

// Each thunk is entered like an ordinary function and tail-jumps without changing
// any argument registers or the stack. Images and thunks live for the app process.
void* MakeThunk(uintptr_t data, void (*bridge)())
{
    uint8_t code[] = {0x49, 0xba, 0,   0, 0, 0, 0, 0, 0, 0, // mov r10, data
                      0x49, 0xbb, 0,   0, 0, 0, 0, 0, 0, 0, // mov r11, bridge
                      0x41, 0xff, 0xe3};                    // jmp r11
    const auto address = reinterpret_cast<uintptr_t>(bridge);
    std::memcpy(code + 2, &data, 8);
    std::memcpy(code + 12, &address, 8);
    return PublishCode(code, sizeof(code));
}

}

uintptr_t Import(std::string_view name, uintptr_t address, void (*bridge)())
{
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    auto& cache = state.imports;
    if (const auto it = cache.find(std::string(name)); it != cache.end())
        return reinterpret_cast<uintptr_t>(it->second.thunk);
    const auto thunk = MakeThunk(address, bridge);
    cache.emplace(name, State::ImportedFunction{thunk, address, bridge});
    return reinterpret_cast<uintptr_t>(thunk);
}

bool AndroidImportMatches(uintptr_t thunk, uintptr_t address, void (*bridge)())
{
    const auto& state = Process();
    const std::lock_guard lock(Process().mutex);
    for (const auto& [name, entry] : state.imports)
        if (reinterpret_cast<uintptr_t>(entry.thunk) == thunk)
            return entry.target == address && entry.bridge == bridge;
    return false;
}

uintptr_t ImportAndroidCallback(std::string_view name, AndroidHostCallback callback)
{
    if (!callback)
        throw std::invalid_argument("missing Android host callback");
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    const auto address = reinterpret_cast<uintptr_t>(callback);
    const auto thunk = Import(name, address);
    auto& entry = state.imports.at(std::string(name));
    if (entry.target != address || entry.bridge != AndroidToWindows)
        throw std::invalid_argument("conflicting Android host callback registration");
    entry.callback = true;
    return thunk;
}

uintptr_t MakeAndroidCallbackThunk(uintptr_t address)
{
    return reinterpret_cast<uintptr_t>(MakeThunk(address, AndroidToWindows));
}

uintptr_t BridgeAndroidImport(std::string_view name, uintptr_t target)
{
    return Import(name, target);
}

uintptr_t AndroidCodecCallbackThunk(uintptr_t target)
{
    return Import("AMediaCodec_setAsyncNotifyCallback", target, AndroidToWindowsCodecCallbacks);
}

void CallAndroidCodecError(uintptr_t callback, void* codec, void* context, int error, const char* detail)
{
    Call call(callback, "(LLLLL)J", false, true);
    call.scope = NamespaceForCode(callback);
    CallAndroidFive(&call, codec, context, static_cast<uintptr_t>(error), 0, detail);
}

uintptr_t CallAndroidCallback(uintptr_t target, void* argument, void* second, void* third, uintptr_t fourth,
                              std::span<uint8_t> writable)
{
    auto& state = Process();
    if (state.translate && !NativeSystemImageForCode(target, 1)) {
        const auto* owner = FindArm64Code(target);
        if (!owner && Arm64ImportAt(target) == "free") {
            if (!FreeArm64GuestMemory(reinterpret_cast<uintptr_t>(argument)))
                throw std::runtime_error("invalid ARM64 free callback address");
            return 0;
        }
        if (!owner) {
            // This is a host-side callback API with four integer/pointer slots.
            // Only explicitly registered Win64 callbacks are eligible; guest
            // instruction dispatch never uses this trusted-host shortcut.
            uintptr_t imported = 0;
            {
                const std::lock_guard lock(state.mutex);
                for (const auto& [name, entry] : state.imports) {
                    if (entry.callback && reinterpret_cast<uintptr_t>(entry.thunk) == target) {
                        imported = entry.target;
                        break;
                    }
                }
            }
            if (imported)
                return reinterpret_cast<AndroidHostCallback>(imported)(reinterpret_cast<uintptr_t>(argument), second,
                                                                       third, reinterpret_cast<void*>(fourth));
        }
        if (!owner || target % 4)
            throw std::runtime_error("invalid ARM64 callback target");
        const std::array<uint64_t, 4> arguments{reinterpret_cast<uintptr_t>(argument),
                                                reinterpret_cast<uintptr_t>(second), reinterpret_cast<uintptr_t>(third),
                                                fourth};
        return RunArm64Callback(*owner, target, Arm64ImportAt, AndroidProperty, Arm64JniBridge(), arguments, writable);
    }
    Call call(target, "(LLLL)J", false, true);
    call.scope = NamespaceForCode(target);
    return CallAndroid(&call, argument, second, third, fourth);
}

}
