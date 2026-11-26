#include "abi/AndroidLibraryImports.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>

namespace stone::windows {
namespace {
template <typename T> void SinCos(T angle, T* sine, T* cosine)
{
    *sine = std::sin(angle);
    *cosine = std::cos(angle);
}

int AndroidIlogb(float value)
{
    const int exponent = std::ilogb(value);
    // Bionic uses -INT_MAX for FP_ILOGB0. UCRT uses INT_MIN.
    return value == 0.0f ? -INT_MAX : exponent;
}
}

uintptr_t Libm(std::string_view name)
{
    using FloatUnary = float (*)(float);
    using DoubleUnary = double (*)(double);
    using FloatBinary = float (*)(float, float);
    using DoubleBinary = double (*)(double, double);
    if (name == "nanf")
        return Import(name, &std::nanf);
    if (name == "cbrt")
        return Import(name, static_cast<DoubleUnary>(std::cbrt));
    if (name == "asinh")
        return Import(name, static_cast<DoubleUnary>(std::asinh));
    if (name == "acosh")
        return Import(name, static_cast<DoubleUnary>(std::acosh));
    if (name == "atanh")
        return Import(name, static_cast<DoubleUnary>(std::atanh));
    if (name == "floor")
        return Import(name, static_cast<DoubleUnary>(std::floor));
    if (name == "ceil")
        return Import(name, static_cast<DoubleUnary>(std::ceil));
    if (name == "trunc")
        return Import(name, static_cast<DoubleUnary>(std::trunc));
    if (name == "round")
        return Import(name, static_cast<DoubleUnary>(std::round));
    if (name == "roundf")
        return Import(name, static_cast<FloatUnary>(std::round));
    if (name == "cos")
        return Import(name, static_cast<DoubleUnary>(std::cos));
    if (name == "sin")
        return Import(name, static_cast<DoubleUnary>(std::sin));
    if (name == "asin")
        return Import(name, static_cast<DoubleUnary>(std::asin));
    if (name == "acos")
        return Import(name, static_cast<DoubleUnary>(std::acos));
    if (name == "tan")
        return Import(name, static_cast<DoubleUnary>(std::tan));
    if (name == "atan")
        return Import(name, static_cast<DoubleUnary>(std::atan));
    if (name == "exp")
        return Import(name, static_cast<DoubleUnary>(std::exp));
    if (name == "log")
        return Import(name, static_cast<DoubleUnary>(std::log));
    if (name == "log10")
        return Import(name, static_cast<DoubleUnary>(std::log10));
    if (name == "sinh")
        return Import(name, static_cast<DoubleUnary>(std::sinh));
    if (name == "log2")
        return Import(name, static_cast<DoubleUnary>(std::log2));
    if (name == "cosh")
        return Import(name, static_cast<DoubleUnary>(std::cosh));
    if (name == "erf")
        return Import(name, static_cast<DoubleUnary>(std::erf));
    if (name == "expm1")
        return Import(name, static_cast<DoubleUnary>(std::expm1));
    if (name == "fabs")
        return Import(name, static_cast<DoubleUnary>(std::fabs));
    if (name == "log1p")
        return Import(name, static_cast<DoubleUnary>(std::log1p));
    if (name == "tanh")
        return Import(name, static_cast<DoubleUnary>(std::tanh));
    if (name == "exp2")
        return Import(name, static_cast<DoubleUnary>(std::exp2));
    if (name == "sqrtf")
        return Import(name, static_cast<FloatUnary>(std::sqrt));
    if (name == "sqrt")
        return Import(name, static_cast<DoubleUnary>(std::sqrt));
    if (name == "cbrtf")
        return Import(name, static_cast<FloatUnary>(std::cbrt));
    if (name == "acoshf")
        return Import(name, static_cast<FloatUnary>(std::acosh));
    if (name == "asinhf")
        return Import(name, static_cast<FloatUnary>(std::asinh));
    if (name == "atanhf")
        return Import(name, static_cast<FloatUnary>(std::atanh));
    if (name == "coshf")
        return Import(name, static_cast<FloatUnary>(std::cosh));
    if (name == "erfcf")
        return Import(name, static_cast<FloatUnary>(std::erfc));
    if (name == "expm1f")
        return Import(name, static_cast<FloatUnary>(std::expm1));
    if (name == "sinhf")
        return Import(name, static_cast<FloatUnary>(std::sinh));
    if (name == "fabsf")
        return Import(name, static_cast<FloatUnary>(std::fabs));
    if (name == "erff")
        return Import(name, static_cast<FloatUnary>(std::erf));
    if (name == "acosf")
        return Import(name, static_cast<FloatUnary>(std::acos));
    if (name == "log2f")
        return Import(name, static_cast<FloatUnary>(std::log2));
    if (name == "cosf")
        return Import(name, static_cast<FloatUnary>(std::cos));
    if (name == "expf")
        return Import(name, static_cast<FloatUnary>(std::exp));
    if (name == "log1pf")
        return Import(name, static_cast<FloatUnary>(std::log1p));
    if (name == "logb")
        return Import(name, static_cast<DoubleUnary>(std::logb));
    if (name == "ceilf")
        return Import(name, static_cast<FloatUnary>(std::ceil));
    if (name == "floorf")
        return Import(name, static_cast<FloatUnary>(std::floor));
    if (name == "truncf")
        return Import(name, static_cast<FloatUnary>(std::trunc));
    if (name == "nearbyintf")
        return Import(name, static_cast<FloatUnary>(std::nearbyint));
    if (name == "lrintf")
        return Import(name, static_cast<long long (*)(float)>(std::llrint));
    if (name == "tanf")
        return Import(name, static_cast<FloatUnary>(std::tan));
    if (name == "asinf")
        return Import(name, static_cast<FloatUnary>(std::asin));
    if (name == "exp2f")
        return Import(name, static_cast<FloatUnary>(std::exp2));
    if (name == "tanhf")
        return Import(name, static_cast<FloatUnary>(std::tanh));
    if (name == "sinf")
        return Import(name, static_cast<FloatUnary>(std::sin));
    if (name == "tanf")
        return Import(name, static_cast<FloatUnary>(std::tan));
    if (name == "atanf")
        return Import(name, static_cast<FloatUnary>(std::atan));
    if (name == "logf")
        return Import(name, static_cast<FloatUnary>(std::log));
    if (name == "log10f")
        return Import(name, static_cast<FloatUnary>(std::log10));
    if (name == "remainder")
        return Import(name, static_cast<DoubleBinary>(std::remainder));
    if (name == "remainderf")
        return Import(name, static_cast<FloatBinary>(std::remainder));
    if (name == "hypot")
        return Import(name, static_cast<DoubleBinary>(std::hypot));
    if (name == "pow")
        return Import(name, static_cast<DoubleBinary>(std::pow));
    if (name == "atan2")
        return Import(name, static_cast<DoubleBinary>(std::atan2));
    if (name == "fmod")
        return Import(name, static_cast<DoubleBinary>(std::fmod));
    if (name == "nextafter")
        return Import(name, static_cast<DoubleBinary>(std::nextafter));
    if (name == "nextafterf")
        return Import(name, static_cast<FloatBinary>(std::nextafter));
    if (name == "hypotf")
        return Import(name, static_cast<FloatBinary>(std::hypot));
    if (name == "fmodf")
        return Import(name, static_cast<FloatBinary>(std::fmod));
    if (name == "atan2f")
        return Import(name, static_cast<FloatBinary>(std::atan2));
    if (name == "powf")
        return Import(name, static_cast<FloatBinary>(std::pow));
    if (name == "fma")
        return Import(name, static_cast<double (*)(double, double, double)>(std::fma));
    if (name == "fmaf")
        return Import(name, static_cast<float (*)(float, float, float)>(std::fma));
    if (name == "nan")
        return Import(name, &std::nan);
    if (name == "ilogbf")
        return Import(name, &AndroidIlogb);
    if (name == "llround")
        return Import(name, static_cast<long long (*)(double)>(std::llround));
    if (name == "llroundf")
        return Import(name, static_cast<long long (*)(float)>(std::llround));
    if (name == "lround")
        return Import(name, static_cast<long long (*)(double)>(std::llround));
    if (name == "lroundf")
        return Import(name, static_cast<long long (*)(float)>(std::llround));
    if (name == "modf")
        return Import(name, static_cast<double (*)(double, double*)>(std::modf), AndroidToWindowsFloatFirst);
    if (name == "scalbn")
        return Import(name, static_cast<double (*)(double, int)>(std::scalbn), AndroidToWindowsFloatFirst);
    if (name == "modff")
        return Import(name, static_cast<float (*)(float, float*)>(std::modf), AndroidToWindowsFloatFirst);
    if (name == "scalbnf")
        return Import(name, static_cast<float (*)(float, int)>(std::scalbn), AndroidToWindowsFloatFirst);
    if (name == "frexp")
        return Import(name, static_cast<double (*)(double, int*)>(std::frexp), AndroidToWindowsFloatFirst);
    if (name == "frexpf")
        return Import(name, static_cast<float (*)(float, int*)>(std::frexp), AndroidToWindowsFloatFirst);
    if (name == "ldexp")
        return Import(name, static_cast<double (*)(double, int)>(std::ldexp), AndroidToWindowsFloatFirst);
    if (name == "ldexpf")
        return Import(name, static_cast<float (*)(float, int)>(std::ldexp), AndroidToWindowsFloatFirst);
    if (name == "sincos")
        return Import(name, &SinCos<double>, AndroidToWindowsFloatFirst);
    if (name == "sincosf")
        return Import(name, &SinCos<float>, AndroidToWindowsFloatFirst);
    return 0;
}

uintptr_t Arm64FenvImport(std::string_view symbol)
{
    // Relocation tokens, consumed only by the interpreter's import dispatcher.
    static const uint8_t holdExcept = 1, setEnv = 2;
    if (symbol == "feholdexcept")
        return reinterpret_cast<uintptr_t>(&holdExcept);
    if (symbol == "fesetenv")
        return reinterpret_cast<uintptr_t>(&setEnv);
    static constexpr std::string_view names[]{"feclearexcept", "fedisableexcept", "feenableexcept", "fegetexcept",
                                              "fegetround",    "fesetround",      "fetestexcept"};
    static const std::array<uint8_t, std::size(names)> tokens{};
    const auto found = std::ranges::find(names, symbol);
    if (found != std::end(names))
        return reinterpret_cast<uintptr_t>(&tokens[found - names]);
    return 0;
}

}
