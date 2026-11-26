#pragma once

#include "TypedAndroidImport.h"
#include <jni.h>

namespace stone::windows {
struct Arm64HostJniTarget {
    AndroidScalarImportInfo native;
    char result;
};

// Only explicitly typed imports can be published as host JNI targets.
uintptr_t PrepareArm64HostJniTarget(std::string_view name, std::string_view signature, uintptr_t source);
std::optional<Arm64HostJniTarget> FindArm64HostJniTarget(uintptr_t token);
// Validate guest-owned JNIEnv storage and the selected ART image's JNI table.
uintptr_t Arm64ArtJniEntry(JNIEnv* environment, size_t memberOffset);
bool IsArm64ElfFunction(uintptr_t address);
}
