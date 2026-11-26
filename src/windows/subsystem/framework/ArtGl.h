#pragma once

#include <jni.h>

namespace stone::windows {
bool RegisterArtGl(JNIEnv* env);
int ArtGlEsVersion();
}
