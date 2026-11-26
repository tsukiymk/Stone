#pragma once

#include "abi/Arm64ExecutionBridge.h"
#include "abi/Arm64JniSignature.h"
#include "memory/GuestMemory.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace stone::windows {

class ElfImage;

// Interprets AArch64 functions outside the native leaf JIT.
// Imports are dispatched by symbol. No guest address is called as host code.
using Arm64ImportName = std::function<std::string_view(uintptr_t)>;
uintptr_t Arm64TlsDescriptorResolver();
uintptr_t RunArm64IfuncResolver(const ElfImage& image, uintptr_t entry, const Arm64ImportName& importName,
                                Arm64PropertyGet propertyGet);
uint64_t RunArm64Initializer(const ElfImage& image, uintptr_t entry, unsigned resultWidth,
                             const Arm64ImportName& importName, Arm64PropertyGet propertyGet,
                             const Arm64Jni* jni = nullptr);
uint64_t RunArm64Native(const ElfImage& image, uintptr_t entry, char result, const Arm64ImportName& importName,
                        Arm64PropertyGet propertyGet, const Arm64Jni& jni, JNIEnv* env, jobject receiver,
                        std::span<const Arm64Argument> arguments);
uint64_t RunArm64Function(const ElfImage& image, uintptr_t entry, char result, const Arm64ImportName& importName,
                          Arm64PropertyGet propertyGet, const Arm64Jni* jni, std::span<const Arm64Argument> arguments);
uint64_t RunArm64Callback(const ElfImage& image, uintptr_t entry, const Arm64ImportName& importName,
                          Arm64PropertyGet propertyGet, const Arm64Jni& jni, std::span<const uint64_t> arguments,
                          std::span<uint8_t> writable);
bool RunArm64Signal(uintptr_t handler, int signal, uint64_t previousMask, bool withInfo);
bool RunArm64MemoryFault(uintptr_t handler, uintptr_t address, int code, uint64_t& mask, bool withInfo);
uintptr_t CopyArm64GuestConstantString(const char* value);
void RunArm64Destructors(const Arm64ImportName& importName, Arm64PropertyGet propertyGet, const Arm64Jni& jni);

} // namespace stone::windows
