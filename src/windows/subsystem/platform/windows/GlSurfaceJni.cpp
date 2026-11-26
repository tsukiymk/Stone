#include "GlSurfaceJni.h"
#include "../../../host/graphics/AngleState.h"
#include <windows.h>
using namespace stone::windows::angle_backend;

namespace {
void NoContext(JNIEnv* env)
{
    const auto error = env->FindClass("java/lang/IllegalStateException");
    if (error) {
        env->ThrowNew(error, "No current Windows OpenGL ES surface");
        env->DeleteLocalRef(error);
    }
}

template <auto Function, typename... Args> void JNICALL GlVoid(JNIEnv* env, jclass, Args... args)
{
    if (current)
        (angle.*Function)(args...);
    else
        NoContext(env);
}

jint JNICALL GlError(JNIEnv* env, jclass)
{
    if (current)
        return static_cast<jint>(angle.glGetError());
    NoContext(env);
    return 0;
}

jstring JNICALL GlString(JNIEnv* env, jclass, jint name)
{
    if (!current) {
        NoContext(env);
        return nullptr;
    }
    const auto value = angle.glGetString(name);
    return value ? env->NewStringUTF(reinterpret_cast<const char*>(value)) : nullptr;
}

jboolean JNICALL Withhold(JNIEnv*, jclass, jboolean)
{
    withheld = true;
    return JNI_FALSE;
}
}

bool RegisterWindowsGles(JNIEnv* env)
{
#define BIND(name, signature, ...)                                                                                     \
    {                                                                                                                  \
        const_cast<char*>(name), const_cast<char*>(signature), reinterpret_cast<void*>(__VA_ARGS__)                    \
    }
    const JNINativeMethod gles[]{
        BIND("glViewport", "(IIII)V", GlVoid<&Angle::glViewport, jint, jint, jint, jint>),
        BIND("glClear", "(I)V", GlVoid<&Angle::glClear, jint>),
        BIND("glClearColor", "(FFFF)V", GlVoid<&Angle::glClearColor, jfloat, jfloat, jfloat, jfloat>),
        BIND("glEnable", "(I)V", GlVoid<&Angle::glEnable, jint>),
        BIND("glDisable", "(I)V", GlVoid<&Angle::glDisable, jint>),
        BIND("glScissor", "(IIII)V", GlVoid<&Angle::glScissor, jint, jint, jint, jint>),
        BIND("glGetString", "(I)Ljava/lang/String;", GlString),
        BIND("glGetError", "()I", GlError),
    };
    const JNINativeMethod cocoa[]{BIND("beginWithheldTick", "(Z)Z", Withhold)};
#undef BIND
    const auto type = env->FindClass("android/opengl/GLES20");
    if (!type)
        return false;
    const bool bound = env->RegisterNatives(type, gles, ARRAYSIZE(gles)) == JNI_OK;
    env->DeleteLocalRef(type);
    if (!bound)
        return false;
    const auto bridge = env->FindClass("com/trebledot/stone/loader/NativeCocoa");
    if (!bridge)
        return false;
    const bool result = env->RegisterNatives(bridge, cocoa, ARRAYSIZE(cocoa)) == JNI_OK;
    env->DeleteLocalRef(bridge);
    return result;
}
