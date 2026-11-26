#pragma once

#include <jni.h>
#include <cstdint>

namespace stone::windows {
uint32_t AppContainerWorkerParent(uintptr_t input, uintptr_t output);
bool RegisterWindowsIsolatedProcess(JNIEnv* env);
}
