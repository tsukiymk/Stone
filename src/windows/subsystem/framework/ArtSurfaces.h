#pragma once

#include <jni.h>
#include <cstdint>
#include <functional>
#include <memory>

namespace stone::windows {
struct AndroidImageQueue;
bool RegisterArtSurfaces(JNIEnv* env);
uintptr_t ArtSurfaceFromJava(JNIEnv* env, jobject surface);
uintptr_t CreateArtSoftwareSurface(int width, int height, std::function<void()> onFrame = {});
uintptr_t RetainArtSoftwareSurface(uintptr_t token);
bool ReleaseArtSoftwareSurface(uintptr_t token);
std::shared_ptr<AndroidImageQueue> ArtSoftwareSurfaceQueue(uintptr_t token);
}
