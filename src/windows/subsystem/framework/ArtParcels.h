#pragma once

#include <jni.h>

namespace stone::windows {
struct ArtLocalBinderState {
    jint strictModePolicy = 0;
    jint workSource = -1;
    bool propagateWorkSource = false;
};

ArtLocalBinderState& LocalArtBinderState();
bool RegisterArtLocalParcels(JNIEnv* env);
}
