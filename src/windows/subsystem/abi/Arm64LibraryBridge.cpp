#include "loader/AndroidLibraryState.h"

namespace stone::windows {
using namespace android_libraries;

uintptr_t Arm64ArtJniEntry(JNIEnv* environment, size_t memberOffset)
{
    if (memberOffset % sizeof(uintptr_t) || memberOffset > sizeof(JNINativeInterface_) - sizeof(uintptr_t))
        throw std::invalid_argument("invalid ARM64 ART JNI table offset");
    uintptr_t table = 0;
    if (!ReadArm64GuestAllocation(reinterpret_cast<uintptr_t>(environment), &table, sizeof(table)))
        throw std::invalid_argument("ARM64 ART JNI environment is not guest-owned");
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    for (const auto name : {"libart.so", "libartd.so"}) {
        const auto library = state.runtime.images.find(name);
        if (library == state.runtime.images.end())
            continue;
        const auto& image = library->second->image;
        const auto* data = image.DataAt(table, sizeof(JNINativeInterface_), false);
        if (!data)
            continue;
        uintptr_t function = 0;
        std::memcpy(&function, data + memberOffset, sizeof(function));
        if (image.ContainsCode(function, 4))
            return function;
    }
    throw std::invalid_argument("ARM64 ART JNI table or entry is outside the selected ART image");
}

namespace android_libraries {
unsigned ScalarWidth(std::string_view signature, size_t& offset)
{
    if (offset < signature.size()) {
        switch (signature[offset++]) {
        case 'J':
            return 64;
        case 'I':
            return 32;
        case 'V':
            return 0;
        }
    }
    throw std::runtime_error("unsupported JIT JNI type");
}

void* TranslatedThunk(const ElfImage& image, uintptr_t address, std::string_view signature, unsigned hidden)
{
    auto& thunks = Process().nativeThunks;
    const std::string key = std::to_string(address) + (hidden ? ':' : '#') + std::string(signature);
    const auto [slot, inserted] = thunks.try_emplace(key, nullptr);
    if (!inserted)
        return slot->second;

    try {
        if (!signature.starts_with('('))
            throw std::runtime_error("invalid JIT JNI signature");
        std::array<unsigned, 6> arguments{};
        size_t offset = 1, count = hidden;
        while (offset < signature.size() && signature[offset] != ')') {
            if (count == arguments.size())
                throw std::runtime_error("JIT JNI stack arguments are unsupported");
            const auto width = ScalarWidth(signature, offset);
            if (!width)
                throw std::runtime_error("invalid JIT JNI parameter");
            arguments[count++] = width;
        }
        if (offset == signature.size())
            throw std::runtime_error("invalid JIT JNI signature");
        ++offset;
        const auto result = ScalarWidth(signature, offset);
        if (offset != signature.size())
            throw std::runtime_error("invalid JIT JNI signature");

        const auto bytes = image.CodeAt(address, 512);
        if (address % 4 != 0)
            throw std::runtime_error("unaligned AArch64 function entry");
        const auto code = TranslateArm64Leaf(bytes, std::span(arguments).first(count), result);
        slot->second = PublishCode(code.data(), code.size() * sizeof(code.front()));
        return slot->second;
    }
    catch (...) {
        thunks.erase(slot);
        throw;
    }
}

std::string_view Arm64ImportAt(uintptr_t target)
{
    struct CachedImport {
        uintptr_t target;
        std::string_view name;
    };

    thread_local CachedImport cached[64]{};
    static_assert(std::is_trivially_destructible_v<decltype(cached)>);
    auto& recent = cached[((target >> 4) ^ (target >> 12)) & 63];
    if (recent.target == target && !recent.name.empty())
        return recent.name;
    const auto& state = Process();
    const std::lock_guard lock(Process().mutex);
    const auto found = state.arm64ImportNames.find(target);
    if (found == state.arm64ImportNames.end())
        return {};
    // Positive registrations and their node strings live for the process.
    // Never cache a miss: a later library may register that address.
    recent = {target, std::string_view(found->second)};
    return recent.name;
}

void* Arm64NativeThunk(const ElfImage& image, uintptr_t address, std::string_view name, std::string_view signature)
{
    if (address % 4 || !image.ContainsCode(address, 4) || !signature.starts_with('('))
        throw std::runtime_error("invalid ARM64 JNI entry");
    auto& state = Process();
    const auto isApplicationImage = [&](const char* soname) {
        const auto library = state.application.images.find(soname);
        return library != state.application.images.end() && &library->second->image == &image;
    };
    if (isApplicationImage("libmatrix-jnihook.so")) {
        if (auto* fallback = MatrixJniHookFallback(name, signature))
            return fallback;
    }
    const auto key = "arm64:" + std::to_string(address) + ':' + std::string(signature);
    if (const auto existing = state.nativeThunks.find(key); existing != state.nativeThunks.end())
        return existing->second;

    auto call = std::make_unique<Arm64NativeCall>(
        Arm64NativeCall{image, address, std::string(name), std::string(signature), {}});
    if (isApplicationImage("libmmkv.so")) {
        call->filePathArguments = MmkvJniPathArguments(name, signature);
    }
    if (isApplicationImage("libcso.so")) {
        const auto paths = CsoJniPathArguments(name, signature);
        call->filePathArguments = paths.arguments;
        call->filePathArrayArguments = paths.arrayArguments;
    }
    size_t offset = 1;
    while (offset < signature.size() && signature[offset] != ')') {
        if (call->parameters.size() == 255)
            throw std::runtime_error("too many ARM64 JNI parameters");
        call->parameters.push_back(ReadArm64JniType(signature, offset, false));
    }
    if (offset == signature.size())
        throw std::runtime_error("invalid ARM64 JNI signature");
    ++offset;
    call->result = ReadArm64JniType(signature, offset, true);
    if (offset != signature.size())
        throw std::runtime_error("invalid ARM64 JNI result type");
    if (call->parameters.size() <= 4 && (call->result == 'I' || call->result == 'J' || call->result == 'V') &&
        std::ranges::all_of(call->parameters, [](char type) {
            return type == 'I' || type == 'J';
        })) {
        try {
            return TranslatedThunk(image, address, signature);
        }
        catch (const Arm64TranslationError&) {
            // Non-leaf functions use the AAPCS64 interpreter through the Win64 entry.
        }
    }

    const auto [slot, inserted] = state.nativeThunks.try_emplace(key, nullptr);
    if (!inserted)
        return slot->second;
    try {
        state.arm64Calls.push_back(std::move(call));
        try {
            slot->second = MakeThunk(reinterpret_cast<uintptr_t>(state.arm64Calls.back().get()), Arm64NativeEntry);
        }
        catch (...) {
            state.arm64Calls.pop_back();
            throw;
        }
        return slot->second;
    }
    catch (...) {
        state.nativeThunks.erase(slot);
        throw;
    }
}

const ElfImage* FindArm64Code(uintptr_t address);

jint RegisterArm64Natives(JNIEnv* env, jclass type, const ElfImage& image, std::span<const Arm64NativeMethod> input)
{
    const std::lock_guard lock(Process().mutex);
    std::vector<JNINativeMethod> methods;
    methods.reserve(input.size());
    for (const auto& method : input) {
        if (method.name.empty())
            throw std::runtime_error("empty ARM64 JNI method name");
        try {
            const auto* owner = image.ContainsCode(method.entry, 4) ? &image : FindArm64Code(method.entry);
            if (!owner)
                throw std::runtime_error("JNI target is outside loaded ARM64 code");
            methods.push_back({const_cast<char*>(method.name.c_str()), const_cast<char*>(method.signature.c_str()),
                               Arm64NativeThunk(*owner, method.entry, method.name, method.signature)});
        }
        catch (const std::exception& error) {
            throw std::runtime_error(method.name + method.signature + ": " + error.what());
        }
    }
    return RegisterNativeTable(env, type, methods);
}

const ElfImage* FindArm64Code(uintptr_t address)
{
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    for (const auto* scope : {&state.application, &state.runtime, &state.webView}) {
        for (const auto& [name, library] : scope->images)
            if (library->image.ContainsCode(address, 4))
                return &library->image;
        if (scope->imageUnwind && scope->imageUnwind->ContainsCode(address, 4))
            return scope->imageUnwind.get();
    }
    return nullptr;
}

uint8_t* FindArm64Data(uintptr_t address, unsigned bytes, bool write)
{
    static const bool cached = [] {
        const auto savedError = GetLastError();
        const auto savedErrno = errno;
        wchar_t value[2]{};
        const bool enabled = GetEnvironmentVariableW(L"STONE_ARM64_ELF_DATA_CACHE", value, 2) == 1 && value[0] == L'1';
        errno = savedErrno;
        SetLastError(savedError);
        return enabled;
    }();
    thread_local std::array<const ElfImage*, 4> recent{};
    static_assert(std::is_trivially_destructible_v<decltype(recent)>);
    if (cached)
        for (size_t i = 0; i < recent.size(); ++i)
            if (const auto* owner = recent[i]) {
                // Only published scope.images entries enter this cache. They live for
                // the process (dlclose drops handles, not images). Cache ownership,
                // never permissions or a TLS pointer: both are rechecked on every hit.
                auto* memory = owner->DataAt(address, bytes, write);
                if (!memory)
                    memory = owner->ThreadLocalDataAt(address, bytes);
                if (memory) {
                    if (i)
                        std::rotate(recent.begin(), recent.begin() + i, recent.begin() + i + 1);
                    return memory;
                }
            }
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    const auto remember = [&](const ElfImage& owner, uint8_t* memory) {
        if (cached) {
            std::move_backward(recent.begin(), recent.end() - 1, recent.end());
            recent.front() = &owner;
        }
        return memory;
    };
    for (const auto* scope : {&state.application, &state.runtime, &state.webView}) {
        for (const auto& [name, library] : scope->images) {
            if (auto* memory = library->image.DataAt(address, bytes, write))
                return remember(library->image, memory);
            if (auto* memory = library->image.ThreadLocalDataAt(address, bytes))
                return remember(library->image, memory);
        }
        if (scope->imageUnwind) {
            if (auto* memory = scope->imageUnwind->DataAt(address, bytes, write))
                return memory;
            if (auto* memory = scope->imageUnwind->ThreadLocalDataAt(address, bytes))
                return memory;
        }
    }
    return nullptr;
}

Arm64Jni Arm64JniBridge()
{
    return {hostVm,
            FindGuestClassFromHost,
            RegisterArm64Natives,
            FindArm64Code,
            FindArm64Data,
            DlOpen,
            DlSym,
            DlClose,
            DlError,
            DlAddr,
            +[](std::string_view name, bool hostAbi) {
                auto& state = Process();
                const std::lock_guard lock(state.mutex);
                const auto target = name.starts_with("egl") ? Libegl(name, hostAbi) : Libgles(name, hostAbi);
                if (target && !hostAbi)
                    state.arm64ImportNames.try_emplace(target, name);
                return target;
            },
            {},
            MakeArm64WindowBridge(),
            +[] {
                auto& state = Process();
                const std::lock_guard lock(state.mutex);
                std::vector<Arm64LoadedImage> result;
                for (const auto* scope : {&state.application, &state.runtime, &state.webView}) {
                    for (const auto& [name, library] : scope->images)
                        result.push_back({name, &library->image});
                    if (scope->imageUnwind)
                        result.push_back({"libc.so:unwind", scope->imageUnwind.get()});
                }
                return result;
            }};
}

uint64_t RunArm64Lifecycle(const ElfImage& image, uintptr_t address, bool onLoad)
{
    void* code;
    try {
        code = TranslatedThunk(image, address, onLoad ? "()I" : "()V", onLoad ? 2 : 0);
    }
    catch (const Arm64TranslationError&) {
        const auto jni = Arm64JniBridge();
        return RunArm64Initializer(image, address, onLoad ? 32 : 0, Arm64ImportAt, AndroidProperty, &jni);
    }
    if (onLoad)
        return static_cast<uint32_t>(reinterpret_cast<jint (*)(JavaVM*, void*)>(code)(hostVm, nullptr));
    reinterpret_cast<void (*)()>(code)();
    return 0;
}

}

void* MakeArm64FunctionThunk(const ElfImage& image, uintptr_t address, std::string_view signature)
{
    if (image.Architecture() != ElfImage::Machine::Arm64 || address % 4 || !image.ContainsCode(address, 4) ||
        signature.size() < 3 || signature.front() != '(')
        throw std::runtime_error("invalid ARM64 function entry or signature");
    const auto close = signature.find(')');
    if (close == signature.npos || close + 2 != signature.size() ||
        std::string_view("VZBCSIJFDL").find(signature.back()) == std::string_view::npos)
        throw std::runtime_error("invalid ARM64 function result signature");
    auto& state = Process();
    const std::lock_guard lock(state.mutex);
    const auto key = "arm64:raw:" + std::to_string(address) + ':' + std::string(signature);
    if (const auto found = state.nativeThunks.find(key); found != state.nativeThunks.end())
        return found->second;
    auto call =
        std::make_unique<Arm64NativeCall>(Arm64NativeCall{image, address, "raw function", std::string(signature), {}});
    for (size_t offset = 1; offset < close; ++offset) {
        if (call->parameters.size() >= 255 ||
            std::string_view("ZBCSIJFDL").find(signature[offset]) == std::string_view::npos)
            throw std::runtime_error("invalid ARM64 function parameter signature");
        call->parameters.push_back(signature[offset]);
    }
    call->result = signature.back();
    call->raw = true;
    call->scope = NamespaceForCode(address);
    auto* target = MakeThunk(reinterpret_cast<uintptr_t>(call.get()), Arm64NativeEntry);
    state.arm64Calls.push_back(std::move(call));
    state.nativeThunks.emplace(key, target);
    return target;
}

extern "C" uint64_t StoneRunArm64Native(const Arm64NativeCall* call, const Win64JniArguments* slots) noexcept(false)
{
    auto* env = call->raw ? nullptr : reinterpret_cast<JNIEnv*>(slots->integers[0]);
    const auto receiver = call->raw ? nullptr : reinterpret_cast<jobject>(slots->integers[1]);
    try {
        std::vector<Arm64Argument> arguments;
        arguments.reserve(call->parameters.size());
        const auto mapPath = [&](jstring original) -> jstring {
            if (!original)
                return nullptr;
            const auto path = Utf(env, original);
            const auto guestPath = AndroidGuestFilePath(path);
            return guestPath == path ? original : env->NewStringUTF(guestPath.c_str());
        };
        for (size_t i = 0; i < call->parameters.size(); ++i) {
            const size_t position = i + (call->raw ? 0 : 2);
            const char type = call->parameters[i];
            const bool floating = type == 'F' || type == 'D';
            auto bits = position < 4 ? (floating ? slots->floats[position] : slots->integers[position])
                                     : slots->stack[position - 4];
            if (i < 32 && type == 'L' && bits) {
                if (call->filePathArguments & (1u << i)) {
                    const auto guestPath = mapPath(reinterpret_cast<jstring>(bits));
                    if (!guestPath)
                        return 0;
                    bits = reinterpret_cast<uintptr_t>(guestPath);
                }
                else if (call->filePathArrayArguments & (1u << i)) {
                    const auto original = reinterpret_cast<jobjectArray>(bits);
                    const auto length = env->GetArrayLength(original);
                    const auto elementType = env->FindClass("java/lang/String");
                    if (!elementType)
                        return 0;
                    const auto paths = env->NewObjectArray(length, elementType, nullptr);
                    env->DeleteLocalRef(elementType);
                    if (!paths)
                        return 0;
                    for (jsize index = 0; index < length; ++index) {
                        const auto element = static_cast<jstring>(env->GetObjectArrayElement(original, index));
                        if (env->ExceptionCheck())
                            return 0;
                        const auto mapped = mapPath(element);
                        if (element && !mapped)
                            return 0;
                        env->SetObjectArrayElement(paths, index, mapped);
                        if (mapped && mapped != element)
                            env->DeleteLocalRef(mapped);
                        if (element)
                            env->DeleteLocalRef(element);
                        if (env->ExceptionCheck())
                            return 0;
                    }
                    bits = reinterpret_cast<uintptr_t>(paths);
                }
            }
            arguments.push_back({type, bits});
        }
        const auto jni = Arm64JniBridge();
        if (call->raw) {
            const ScopedNamespace context(call->scope ? *call->scope : CurrentNamespace());
            return RunArm64Function(call->image, call->address, call->result, Arm64ImportAt, AndroidProperty, &jni,
                                    arguments);
        }
        return RunArm64Native(call->image, call->address, call->result, Arm64ImportAt, AndroidProperty, jni, env,
                              receiver, arguments);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "[android-jit] %s%s: %s\n", call->name.c_str(), call->signature.c_str(), error.what());
        if (call->raw)
            throw;
        ThrowLinkError(env, error.what());
        return 0;
    }
}

}
