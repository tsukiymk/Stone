#include "Arm64HostJni.h"
#include "../cpu/arm64/Arm64Initializer.h"
#include "AndroidLibraryImports.h"
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace stone::windows {
namespace {
auto& Targets()
{
    static auto* targets = new std::map<uintptr_t, Arm64HostJniTarget>;
    return *targets;
}

bool Compatible(char native, char java)
{
    if (native == java)
        return true;
    return std::string_view("BSI").find(native) != std::string_view::npos &&
           std::string_view("ZBCSI").find(java) != std::string_view::npos;
}
}

uintptr_t PrepareArm64HostJniTarget(std::string_view name, std::string_view signature, uintptr_t source)
{
    const auto native = FindAndroidScalarImport(source);
    if (!native)
        throw std::invalid_argument("ARM64 JNI requires an explicitly typed host target: " + std::string(name));
    if (name.empty() || !signature.starts_with('('))
        throw std::invalid_argument("invalid ARM64 host JNI method");
    std::vector<char> parameters;
    size_t offset = 1;
    while (offset < signature.size() && signature[offset] != ')') {
        if (parameters.size() == 255)
            throw std::invalid_argument("too many ARM64 host JNI parameters");
        parameters.push_back(ReadArm64JniType(signature, offset, false));
    }
    if (offset == signature.size())
        throw std::invalid_argument("invalid ARM64 host JNI signature");
    ++offset;
    const auto result = ReadArm64JniType(signature, offset, true);
    if (offset != signature.size() || !Compatible(native->prototype.result, result))
        throw std::invalid_argument("ARM64 host JNI result does not match its C++ prototype: " + std::string(name));
    if (native->prototype.jniEnvironment) {
        parameters.insert(parameters.begin(), {'L', 'L'});
    }
    else {
        for (const auto type : native->prototype.parameters)
            if (type == 'L')
                throw std::invalid_argument("ARM64 critical JNI host pointers are unsupported");
    }
    // A native may deliberately ignore trailing Java parameters, including a
    // constant function that consumes none. Every parameter it reads must match.
    const auto types = native->prototype.parameters;
    if (types.size() > parameters.size())
        throw std::invalid_argument("ARM64 host JNI has extra C++ parameters");
    for (size_t i = 0; i < types.size(); ++i)
        if (!Compatible(types[i], parameters[i]))
            throw std::invalid_argument("ARM64 host JNI parameter type mismatch: " + std::string(name));
    const std::lock_guard lock(AndroidLoaderMutex());
    const auto token = ImportAndroidScalars("ARM64 host JNI " + std::string(name) + std::string(signature) + '@' +
                                                std::to_string(source),
                                            native->address, native->invoke, native->prototype);
    Targets().try_emplace(token, Arm64HostJniTarget{*native, result});
    return token;
}

std::optional<Arm64HostJniTarget> FindArm64HostJniTarget(uintptr_t token)
{
    const std::lock_guard lock(AndroidLoaderMutex());
    const auto entry = Targets().find(token);
    return entry == Targets().end() ? std::optional<Arm64HostJniTarget>{} : entry->second;
}
}
