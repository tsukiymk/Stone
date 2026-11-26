#pragma once

#include <jni.h>
#include <cstdint>
#include <string_view>

namespace stone::windows {
bool RegisterArtFramework(JNIEnv* environment);
bool RegisterArtGraphics(JNIEnv* environment);
bool RegisterArtResources(JNIEnv* environment);
bool RegisterArtHost(JNIEnv* environment);
uintptr_t ArtFrameworkImport(std::string_view name);
bool IsArtAssetImport(std::string_view name);
}
