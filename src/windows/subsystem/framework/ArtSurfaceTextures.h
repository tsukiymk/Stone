#pragma once
#include <jni.h>
#include <cstdint>

namespace stone::windows {
bool RegisterArtSurfaceTextures(JNIEnv* env);
bool IsArtSurfaceTexture(uintptr_t surface);
}
