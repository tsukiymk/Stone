#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

struct JNIEnv_;

namespace stone::windows {
struct AndroidScalarArguments {
    std::array<uint64_t, 6> integers;
    std::array<uint64_t, 8> floats;
    const uint64_t* stack;
};

using AndroidScalarInvoker = uint64_t (*)(const AndroidScalarArguments&, uintptr_t);

struct AndroidScalarPrototype {
    std::span<const char> parameters;
    char result;
    bool jniEnvironment;
};

struct AndroidScalarImportInfo {
    uintptr_t address;
    AndroidScalarInvoker invoke;
    AndroidScalarPrototype prototype;
};

uintptr_t ImportAndroidScalars(std::string_view name, uintptr_t address, AndroidScalarInvoker invoke,
                               const AndroidScalarPrototype& prototype);
// Published descriptors and their prototype storage live through native TLS cleanup.
std::optional<AndroidScalarImportInfo> FindAndroidScalarImport(uintptr_t thunk);

namespace detail {
template <class Type> constexpr char AndroidScalarType()
{
    if constexpr (std::is_void_v<Type>)
        return 'V';
    else if constexpr (std::is_pointer_v<Type>)
        return 'L';
    else if constexpr (std::is_same_v<Type, float>)
        return 'F';
    else if constexpr (std::is_same_v<Type, double>)
        return 'D';
    else if constexpr (std::is_integral_v<Type>) {
        static_assert(sizeof(Type) == 1 || sizeof(Type) == 2 || sizeof(Type) == 4 || sizeof(Type) == 8);
        return sizeof(Type) == 1 ? 'B' : sizeof(Type) == 2 ? 'S' : sizeof(Type) == 4 ? 'I' : 'J';
    }
    else
        static_assert(std::is_void_v<Type>, "unsupported Android scalar prototype type");
}

template <class...> struct JniEnvironmentFirst : std::false_type {};

template <class First, class... Rest>
struct JniEnvironmentFirst<First, Rest...>
    : std::bool_constant<std::is_same_v<First, ::JNIEnv_*> || std::is_same_v<First, const ::JNIEnv_*>> {};

template <class T>
T AndroidScalar(const AndroidScalarArguments& source, size_t& integer, size_t& floating, size_t& stack)
{
    static_assert(std::is_integral_v<T> || std::is_pointer_v<T> || std::is_same_v<T, float> ||
                  std::is_same_v<T, double>);
    static_assert(!std::is_function_v<std::remove_pointer_t<T>>, "callbacks require their own ABI bridge");
    uint64_t bits;
    if constexpr (std::is_floating_point_v<T>)
        bits = floating < source.floats.size() ? source.floats[floating++] : source.stack[stack++];
    else
        bits = integer < source.integers.size() ? source.integers[integer++] : source.stack[stack++];
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(bits);
    else if constexpr (std::is_same_v<T, float>)
        return std::bit_cast<float>(static_cast<uint32_t>(bits));
    else if constexpr (std::is_same_v<T, double>)
        return std::bit_cast<double>(bits);
    else
        return static_cast<T>(bits);
}

template <class Function> struct AndroidScalarCall;

template <class Result, class... Arguments> struct AndroidScalarCall<Result (*)(Arguments...)> {
    inline static constexpr std::array<char, sizeof...(Arguments)> Parameters{AndroidScalarType<Arguments>()...};

    static AndroidScalarPrototype Prototype()
    {
        return {Parameters, AndroidScalarType<Result>(), JniEnvironmentFirst<Arguments...>::value};
    }

    template <size_t... Indices>
    static uint64_t Invoke(const AndroidScalarArguments& source, uintptr_t address, std::index_sequence<Indices...>)
    {
        std::tuple<Arguments...> values;
        size_t integer = 0, floating = 0, stack = 0;
        // SysV allocates register banks independently. Evaluate in parameter
        // order so spills from either bank retain their original stack order.
        ((std::get<Indices>(values) = AndroidScalar<Arguments>(source, integer, floating, stack)), ...);
        const auto function = reinterpret_cast<Result (*)(Arguments...)>(address);
        if constexpr (std::is_void_v<Result>) {
            std::apply(function, values);
            return 0;
        }
        else {
            const auto result = std::apply(function, values);
            if constexpr (std::is_pointer_v<Result>)
                return reinterpret_cast<uintptr_t>(result);
            else if constexpr (std::is_same_v<Result, float>)
                return std::bit_cast<uint32_t>(result);
            else if constexpr (std::is_same_v<Result, double>)
                return std::bit_cast<uint64_t>(result);
            else {
                static_assert(std::is_integral_v<Result>);
                return static_cast<uint64_t>(result);
            }
        }
    }

    static uint64_t Invoke(const AndroidScalarArguments& source, uintptr_t address)
    {
        return Invoke(source, address, std::index_sequence_for<Arguments...>{});
    }
};

template <class Result, class... Arguments>
struct AndroidScalarCall<Result (*)(Arguments...) noexcept> : AndroidScalarCall<Result (*)(Arguments...)> {};
}

// Function supplies the actual Win64 prototype; no guessed integer-only ABI.
template <class Function> uintptr_t ImportAndroidScalars(std::string_view name, uintptr_t address)
{
    using Call = detail::AndroidScalarCall<std::remove_cv_t<Function>>;
    return ImportAndroidScalars(name, address, &Call::Invoke, Call::Prototype());
}
}
