#pragma once

#include "../../loader/AndroidSystemElf.h"
#include "../../abi/Arm64HostJni.h"
#include <jni.h>

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace stone::windows {
bool IsArm64TranslationEnabled();

namespace detail {
class ArtJniCallStorage {
public:
    ArtJniCallStorage() = default;
    ArtJniCallStorage(const ArtJniCallStorage&) = delete;
    ArtJniCallStorage& operator=(const ArtJniCallStorage&) = delete;
    ~ArtJniCallStorage();
    const char* CopyString(const char* value);
    const JNINativeMethod* CopyMethods(const JNINativeMethod* methods, jint count);
    void* Buffer(size_t bytes);
    void* CopyBytes(const void* source, size_t bytes);
    static size_t ByteCount(jsize count, size_t elementSize);
    void CopyOutput(JNIEnv* environment, void* destination, const void* source, size_t bytes);
    const jvalue* CopyValues(jmethodID method, const jvalue* values);

    template <class Parameter, bool CopyStrings, bool RawPointers> Parameter Prepare(Parameter value)
    {
        if constexpr (CopyStrings && std::is_same_v<Parameter, const char*>)
            return CopyString(value);
        else if constexpr (std::is_pointer_v<Parameter> && !RawPointers && !std::is_convertible_v<Parameter, jobject> &&
                           !std::is_same_v<Parameter, jmethodID> && !std::is_same_v<Parameter, jfieldID>)
            throw std::invalid_argument("ARM64 JNI pointer needs an explicit buffer bridge");
        else
            return value;
    }

private:
    std::vector<uintptr_t> allocations_;
};

template <auto> inline constexpr bool CopyArtJniStrings = true;
template <> inline constexpr bool CopyArtJniStrings<&JNINativeInterface_::ReleaseStringUTFChars> = false;
enum class ArtJniCallKind {
    Ordinary,
    Register,
    MethodId,
    MethodA,
    NonvirtualA,
    NewString,
    SetRegion,
    GetRegion,
    GetElements,
    Release
};
template <auto> inline constexpr ArtJniCallKind ArtJniKind = ArtJniCallKind::Ordinary;
#define STONE_JNI_KIND(Member, Kind)                                                                                   \
    template <> inline constexpr ArtJniCallKind ArtJniKind<&JNINativeInterface_::Member> = ArtJniCallKind::Kind;
STONE_JNI_KIND(RegisterNatives, Register)
STONE_JNI_KIND(GetMethodID, MethodId)
STONE_JNI_KIND(GetStaticMethodID, MethodId)
STONE_JNI_KIND(NewObjectA, MethodA)
STONE_JNI_KIND(NewString, NewString)
STONE_JNI_KIND(GetStringRegion, GetRegion)
STONE_JNI_KIND(GetStringChars, GetElements)
STONE_JNI_KIND(GetStringUTFChars, GetElements)
STONE_JNI_KIND(ReleaseStringChars, Release)
STONE_JNI_KIND(ReleaseStringUTFChars, Release)
#define STONE_JNI_ARRAY(Type)                                                                                          \
    STONE_JNI_KIND(Set##Type##ArrayRegion, SetRegion)                                                                  \
    STONE_JNI_KIND(Get##Type##ArrayRegion, GetRegion)                                                                  \
    STONE_JNI_KIND(Get##Type##ArrayElements, GetElements)                                                              \
    STONE_JNI_KIND(Release##Type##ArrayElements, Release)
STONE_JNI_ARRAY(Boolean)
STONE_JNI_ARRAY(Byte)
STONE_JNI_ARRAY(Char)
STONE_JNI_ARRAY(Short)
STONE_JNI_ARRAY(Int)
STONE_JNI_ARRAY(Long)
STONE_JNI_ARRAY(Float)
STONE_JNI_ARRAY(Double)
#undef STONE_JNI_ARRAY
#define STONE_JNI_METHOD(Type)                                                                                         \
    STONE_JNI_KIND(Call##Type##MethodA, MethodA)                                                                       \
    STONE_JNI_KIND(CallStatic##Type##MethodA, MethodA)                                                                 \
    STONE_JNI_KIND(CallNonvirtual##Type##MethodA, NonvirtualA)
STONE_JNI_METHOD(Object)
STONE_JNI_METHOD(Boolean)
STONE_JNI_METHOD(Byte)
STONE_JNI_METHOD(Char)
STONE_JNI_METHOD(Short)
STONE_JNI_METHOD(Int)
STONE_JNI_METHOD(Long)
STONE_JNI_METHOD(Float)
STONE_JNI_METHOD(Double)
STONE_JNI_METHOD(Void)
#undef STONE_JNI_METHOD
#undef STONE_JNI_KIND

size_t ArtJniMethodParameterCount(const char* signature);
void RecordArtJniMethod(jmethodID method, size_t count);

template <auto Member, class Result, class Environment, class... Parameters, class... Arguments>
Result InvokeArm64Jni(Result (*target)(Environment*, Parameters...), Environment* environment, Arguments&&... arguments)
{
    static_assert(sizeof...(Parameters) == sizeof...(Arguments));
    ArtJniCallStorage storage;
    constexpr auto kind = ArtJniKind<Member>;
    auto values = std::tuple<Parameters...>{static_cast<Parameters>(std::forward<Arguments>(arguments))...};
    if constexpr (kind == ArtJniCallKind::Register) {
        return target(environment, std::get<0>(values), storage.CopyMethods(std::get<1>(values), std::get<2>(values)),
                      std::get<2>(values));
    }
    else if constexpr (kind == ArtJniCallKind::MethodId) {
        const auto count = ArtJniMethodParameterCount(std::get<2>(values));
        const auto method = target(environment, std::get<0>(values), storage.CopyString(std::get<1>(values)),
                                   storage.CopyString(std::get<2>(values)));
        if (method)
            RecordArtJniMethod(method, count);
        return method;
    }
    else if constexpr (kind == ArtJniCallKind::MethodA) {
        return target(environment, std::get<0>(values), std::get<1>(values),
                      storage.CopyValues(std::get<1>(values), std::get<2>(values)));
    }
    else if constexpr (kind == ArtJniCallKind::NonvirtualA) {
        return target(environment, std::get<0>(values), std::get<1>(values), std::get<2>(values),
                      storage.CopyValues(std::get<2>(values), std::get<3>(values)));
    }
    else if constexpr (kind == ArtJniCallKind::NewString) {
        const auto count = std::get<1>(values);
        return target(
            environment,
            static_cast<const jchar*>(storage.CopyBytes(std::get<0>(values), storage.ByteCount(count, sizeof(jchar)))),
            count);
    }
    else if constexpr (kind == ArtJniCallKind::SetRegion || kind == ArtJniCallKind::GetRegion) {
        using Pointer = std::tuple_element_t<3, std::tuple<Parameters...>>;
        const auto bytes = storage.ByteCount(std::get<2>(values), sizeof(std::remove_pointer_t<Pointer>));
        const auto original = std::get<3>(values);
        if (bytes && !original)
            throw std::invalid_argument("missing ARM64 JNI region buffer");
        if constexpr (kind == ArtJniCallKind::SetRegion) {
            target(environment, std::get<0>(values), std::get<1>(values), std::get<2>(values),
                   static_cast<Pointer>(storage.CopyBytes(original, bytes)));
        }
        else {
            const auto output = static_cast<Pointer>(storage.Buffer(bytes));
            target(environment, std::get<0>(values), std::get<1>(values), std::get<2>(values), output);
            storage.CopyOutput(environment, original, output, bytes);
        }
    }
    else if constexpr (kind == ArtJniCallKind::GetElements) {
        const auto original = std::get<1>(values);
        const auto copied = original ? static_cast<jboolean*>(storage.Buffer(sizeof(jboolean))) : nullptr;
        const auto result = target(environment, std::get<0>(values), copied);
        if (original)
            storage.CopyOutput(environment, original, copied, sizeof(jboolean));
        return result;
    }
    else {
        return target(environment,
                      storage.Prepare<Parameters, CopyArtJniStrings<Member>, kind == ArtJniCallKind::Release>(
                          std::forward<Arguments>(arguments))...);
    }
}

template <class T> constexpr char ArtJniType()
{
    if constexpr (std::is_void_v<T>)
        return 'V';
    else if constexpr (std::is_pointer_v<T>)
        return 'L';
    else if constexpr (std::is_same_v<T, float>)
        return 'F';
    else if constexpr (std::is_same_v<T, double>)
        return 'D';
    else if constexpr (std::is_integral_v<T>) {
        static_assert(sizeof(T) <= 4 || sizeof(T) == 8, "unsupported ART JNI integer width");
        return sizeof(T) == 8 ? 'J' : 'I';
    }
    else {
        static_assert(std::is_void_v<T>, "unsupported ART JNI type");
    }
}

template <class Function> struct ArtJniSignature;

template <class Result, class... Arguments> struct ArtJniSignature<Result (*)(Arguments...)> {
    static std::string Value()
    {
        return std::string("(") + (std::string{} + ... + ArtJniType<Arguments>()) + ')' + ArtJniType<Result>();
    }
};
}

// Invoke a fixed-signature JNI entry through the selected image ABI bridge.
// Use the ...MethodA entries and jvalue arrays for Java method calls; va_list
// layouts differ between Win64 and Android, so ...MethodV is not supported.
template <auto Member, class Environment, class... Arguments>
decltype(auto) ArtJni(Environment* environment, Arguments&&... arguments)
{
    using Function = std::remove_cvref_t<decltype(environment->functions->*Member)>;
    Function function;
    if (IsArm64TranslationEnabled()) {
        JNINativeInterface_ layout{};
        const auto offset = reinterpret_cast<uintptr_t>(&(layout.*Member)) - reinterpret_cast<uintptr_t>(&layout);
        function = reinterpret_cast<Function>(Arm64ArtJniEntry(environment, offset));
    }
    else {
        if (!environment || !environment->functions)
            throw std::invalid_argument("missing ART JNI environment");
        function = environment->functions->*Member;
    }
    if (!function)
        throw std::invalid_argument("missing ART JNI function");
    const auto target = reinterpret_cast<Function>(
        MakeNativeAndroidThunk(reinterpret_cast<uintptr_t>(function), detail::ArtJniSignature<Function>::Value()));
    if (IsArm64TranslationEnabled())
        return detail::InvokeArm64Jni<Member>(target, environment, std::forward<Arguments>(arguments)...);
    return target(environment, std::forward<Arguments>(arguments)...);
}
}
