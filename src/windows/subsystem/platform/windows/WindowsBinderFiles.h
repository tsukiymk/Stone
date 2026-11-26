#pragma once

#include <jni.h>
#include <cstdint>

namespace stone::windows {
struct GuestBinderFiles {
    uintptr_t (*duplicateDisk)(int) = nullptr;
    uintptr_t (*duplicateReadOnlyMemory)(int, unsigned&) = nullptr;
    int (*adoptReadOnlyMemory)(uintptr_t, unsigned) = nullptr;
    const char* (*unsupportedFile)(int) = nullptr;
    uintptr_t (*exportSocket)(int, uintptr_t, unsigned, unsigned&) = nullptr;
    int (*importSocket)(uintptr_t, unsigned, int, unsigned) = nullptr;
};

bool RegisterWindowsBinderFiles(JNIEnv* env, GuestBinderFiles guest = {});
jobject AdoptWindowsHandle(JNIEnv* env, uintptr_t handle);
}
