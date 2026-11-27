#pragma once

#include <jni.h>
#include <cstdint>
#include <functional>
#include <string_view>

namespace stone::windows {
bool RegisterArtFramework(JNIEnv* environment);
bool RegisterArtGraphics(JNIEnv* environment);
bool RegisterArtResources(JNIEnv* environment);
bool RegisterArtHost(JNIEnv* environment);
// The Client installs a process-lifetime completion handler before starting the application.
// It must terminate the process after flushing the normal application exit state.
void ConfigureArtSessionExit(std::function<void(int)> exit);
uintptr_t ArtFrameworkImport(std::string_view name);
bool IsArtAssetImport(std::string_view name);
}
