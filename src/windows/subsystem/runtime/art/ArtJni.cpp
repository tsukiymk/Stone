#include "ArtJni.h"
#include "../../cpu/arm64/Arm64Initializer.h"

#include <climits>
#include <cstring>
#include <map>
#include <mutex>
#include <new>

namespace stone::windows::detail {
namespace {
std::mutex methodMutex;

std::map<jmethodID, size_t>& Methods()
{
    static auto* methods = new std::map<jmethodID, size_t>;
    return *methods;
}
}

ArtJniCallStorage::~ArtJniCallStorage()
{
    for (const auto address : allocations_)
        FreeArm64GuestMemory(address);
}

void* ArtJniCallStorage::Buffer(size_t bytes)
{
    if (bytes > UINT_MAX)
        throw std::length_error("ART JNI buffer exceeds ARM64 input limit");
    const auto address = AllocateArm64GuestMemory(bytes, true);
    if (!address)
        throw std::bad_alloc();
    try {
        allocations_.push_back(address);
    }
    catch (...) {
        FreeArm64GuestMemory(address);
        throw;
    }
    return reinterpret_cast<void*>(address);
}

void* ArtJniCallStorage::CopyBytes(const void* source, size_t bytes)
{
    if (!source) {
        if (bytes)
            throw std::invalid_argument("missing ARM64 JNI input buffer");
        return nullptr;
    }
    auto* copied = Buffer(bytes);
    if (bytes)
        std::memcpy(copied, source, bytes);
    return copied;
}

size_t ArtJniCallStorage::ByteCount(jsize count, size_t elementSize)
{
    if (count < 0 || !elementSize || static_cast<size_t>(count) > UINT_MAX / elementSize)
        throw std::invalid_argument("invalid ARM64 JNI buffer count");
    return static_cast<size_t>(count) * elementSize;
}

void ArtJniCallStorage::CopyOutput(JNIEnv* environment, void* destination, const void* source, size_t bytes)
{
    if (bytes && !ArtJni<&JNINativeInterface_::ExceptionCheck>(environment))
        std::memcpy(destination, source, bytes);
}

size_t ArtJniMethodParameterCount(const char* value)
{
    if (!value)
        throw std::invalid_argument("missing ARM64 JNI method signature");
    const std::string_view signature(value);
    if (!signature.starts_with('('))
        throw std::invalid_argument("invalid ARM64 JNI method signature");
    size_t offset = 1, count = 0;
    while (offset < signature.size() && signature[offset] != ')') {
        if (++count > 255)
            throw std::invalid_argument("too many ARM64 JNI method parameters");
        ReadArm64JniType(signature, offset, false);
    }
    if (offset == signature.size())
        throw std::invalid_argument("invalid ARM64 JNI method parameters");
    ++offset;
    ReadArm64JniType(signature, offset, true);
    if (offset != signature.size())
        throw std::invalid_argument("invalid ARM64 JNI method result");
    return count;
}

void RecordArtJniMethod(jmethodID method, size_t count)
{
    if (!method || count > 255)
        throw std::invalid_argument("invalid ARM64 JNI method metadata");
    const std::lock_guard lock(methodMutex);
    Methods().insert_or_assign(method, count);
}

const jvalue* ArtJniCallStorage::CopyValues(jmethodID method, const jvalue* values)
{
    size_t count;
    {
        const std::lock_guard lock(methodMutex);
        const auto found = Methods().find(method);
        if (found == Methods().end())
            throw std::invalid_argument("ARM64 JNI method signature was not recorded");
        count = found->second;
    }
    return static_cast<const jvalue*>(CopyBytes(values, count * sizeof(jvalue)));
}

const char* ArtJniCallStorage::CopyString(const char* value)
{
    if (!value)
        return nullptr;
    const auto length = std::strlen(value);
    if (length >= UINT_MAX)
        throw std::length_error("ART JNI string exceeds ARM64 input limit");
    return static_cast<const char*>(CopyBytes(value, length + 1));
}

const JNINativeMethod* ArtJniCallStorage::CopyMethods(const JNINativeMethod* methods, jint count)
{
    if (count < 0 || (count && !methods) || static_cast<size_t>(count) > UINT_MAX / sizeof(JNINativeMethod))
        throw std::invalid_argument("invalid ARM64 JNI native-method table");
    if (!count)
        return nullptr;
    auto* copied = static_cast<JNINativeMethod*>(Buffer(static_cast<size_t>(count) * sizeof(JNINativeMethod)));
    for (jint i = 0; i < count; ++i) {
        if (!methods[i].name || !methods[i].signature || !methods[i].fnPtr)
            throw std::invalid_argument("incomplete ARM64 JNI native-method entry");
        copied[i].name = const_cast<char*>(CopyString(methods[i].name));
        copied[i].signature = const_cast<char*>(CopyString(methods[i].signature));
        const auto source = reinterpret_cast<uintptr_t>(methods[i].fnPtr);
        copied[i].fnPtr =
            IsArm64ElfFunction(source)
                ? methods[i].fnPtr
                : reinterpret_cast<void*>(PrepareArm64HostJniTarget(methods[i].name, methods[i].signature, source));
    }
    return copied;
}
}
