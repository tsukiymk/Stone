#pragma once

#include <jni.h>
#include <cstdint>
#include <string_view>

namespace stone::windows {
struct AndroidMotionEvent;
AndroidMotionEvent* CopyAndroidMotionEvent(JNIEnv* env, jobject motionEvent);
uintptr_t AndroidInputImport(std::string_view symbol, bool hostAbi = false);
}
