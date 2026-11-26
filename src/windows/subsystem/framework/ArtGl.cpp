#include "ArtGl.h"
#include "ArtSurfaces.h"
#include "../runtime/art/ArtJni.h"
#include "../abi/AndroidLibraryImports.h"
#include "../abi/TypedAndroidImport.h"
#include "../hle/system/stub-libandroid.h"
#include <EGL/egl.h>
#include <mutex>

namespace stone::windows {
namespace {
template <class F> F Egl(const char* name)
{
    return reinterpret_cast<F>(Libegl(name, true));
}

jfieldID displayHandle, configHandle, egl14Handle;

void Throw(JNIEnv* env, const char* message)
{
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/IllegalArgumentException");
    if (type)
        ArtJni<&JNINativeInterface_::ThrowNew>(env, type, message);
}

jlong WindowSurface(JNIEnv* env, jobject, jobject display, jobject config, jobject surface, jintArray attributes)
{
    if (!display || !config || !surface) {
        Throw(env, "EGL display, config and software Surface are required");
        return 0;
    }
    const auto displayType = ArtJni<&JNINativeInterface_::FindClass>(env, "com/google/android/gles_jni/EGLDisplayImpl");
    const auto configType = ArtJni<&JNINativeInterface_::FindClass>(env, "com/google/android/gles_jni/EGLConfigImpl");
    const bool handles = displayType && configType &&
                         ArtJni<&JNINativeInterface_::IsInstanceOf>(env, display, displayType) &&
                         ArtJni<&JNINativeInterface_::IsInstanceOf>(env, config, configType);
    if (displayType)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, displayType);
    if (configType)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, configType);
    if (!handles) {
        Throw(env, "EGL display and config must belong to the image EGL implementation");
        return 0;
    }
    if (attributes && ArtJni<&JNINativeInterface_::GetArrayLength>(env, attributes) > 0) {
        jint first = 0;
        ArtJni<&JNINativeInterface_::GetIntArrayRegion>(env, attributes, 0, 1, &first);
        if (first != EGL_NONE) {
            Throw(env, "Software EGL window attributes are not supported");
            return 0;
        }
    }
    const auto surfaceType = ArtJni<&JNINativeInterface_::FindClass>(env, "android/view/Surface");
    const bool valid = surfaceType && ArtJni<&JNINativeInterface_::IsInstanceOf>(env, surface, surfaceType);
    if (surfaceType)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, surfaceType);
    if (!valid) {
        Throw(env, "EGL requires an Android software Surface");
        return 0;
    }
    const auto window = reinterpret_cast<NativeWindow*>(ArtSurfaceFromJava(env, surface));
    if (!window)
        return 0;
    const auto create =
        Egl<EGLSurface (*)(EGLDisplay, EGLConfig, NativeWindow*, const EGLint*)>("eglCreateWindowSurface");
    const auto result =
        create
            ? create(
                  reinterpret_cast<EGLDisplay>(ArtJni<&JNINativeInterface_::GetLongField>(env, display, displayHandle)),
                  reinterpret_cast<EGLConfig>(ArtJni<&JNINativeInterface_::GetLongField>(env, config, configHandle)),
                  window, nullptr)
            : EGL_NO_SURFACE;
    ReleaseWindow(window);
    return reinterpret_cast<jlong>(result);
}

jobject WindowSurface14(JNIEnv* env, jclass, jobject display, jobject config, jobject surface, jintArray attributes,
                        jint offset)
{
    if (!display || !config || !surface || offset < 0 ||
        (attributes && offset >= ArtJni<&JNINativeInterface_::GetArrayLength>(env, attributes)) ||
        (!attributes && offset != 0)) {
        Throw(env, "Invalid EGL14 software window arguments");
        return nullptr;
    }
    if (attributes) {
        jint first = 0;
        ArtJni<&JNINativeInterface_::GetIntArrayRegion>(env, attributes, offset, 1, &first);
        if (first != EGL_NONE) {
            Throw(env, "Software EGL14 window attributes are unsupported");
            return nullptr;
        }
    }
    const auto window = reinterpret_cast<NativeWindow*>(ArtSurfaceFromJava(env, surface));
    if (!window)
        return nullptr;
    const auto create =
        Egl<EGLSurface (*)(EGLDisplay, EGLConfig, NativeWindow*, const EGLint*)>("eglCreateWindowSurface");
    const auto value =
        create
            ? create(
                  reinterpret_cast<EGLDisplay>(ArtJni<&JNINativeInterface_::GetLongField>(env, display, egl14Handle)),
                  reinterpret_cast<EGLConfig>(ArtJni<&JNINativeInterface_::GetLongField>(env, config, egl14Handle)),
                  window, nullptr)
            : EGL_NO_SURFACE;
    ReleaseWindow(window);
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "android/opengl/EGLSurface");
    const auto constructor = type ? ArtJni<&JNINativeInterface_::GetMethodID>(env, type, "<init>", "(J)V") : nullptr;
    const jvalue args[]{jvalue{.j = reinterpret_cast<jlong>(value)}};
    const auto result = constructor ? ArtJni<&JNINativeInterface_::NewObjectA>(env, type, constructor, args) : nullptr;
    if (type)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    return result;
}

jobject TextureSurface14(JNIEnv* env, jclass klass, jobject display, jobject config, jobject texture,
                         jintArray attributes, jint offset)
{
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "android/view/Surface");
    const auto constructor =
        type ? ArtJni<&JNINativeInterface_::GetMethodID>(env, type, "<init>", "(Landroid/graphics/SurfaceTexture;)V")
             : nullptr;
    const jvalue args[]{jvalue{.l = texture}};
    const auto surface = constructor ? ArtJni<&JNINativeInterface_::NewObjectA>(env, type, constructor, args) : nullptr;
    jobject result = nullptr;
    if (surface && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env)) {
        result = WindowSurface14(env, klass, display, config, surface, attributes, offset);
        const auto release = ArtJni<&JNINativeInterface_::GetMethodID>(env, type, "release", "()V");
        if (release && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            ArtJni<&JNINativeInterface_::CallVoidMethodA>(env, surface, release, nullptr);
    }
    if (surface)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, surface);
    if (type)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    return result;
}
}

int ArtGlEsVersion()
{
    if (IsArm64TranslationEnabled())
        return 0;
    static std::once_flag once;
    static int version = 0;
    std::call_once(once, [] {
        const auto get = Egl<decltype(&eglGetDisplay)>("eglGetDisplay");
        const auto init = Egl<decltype(&eglInitialize)>("eglInitialize");
        const auto choose = Egl<decltype(&eglChooseConfig)>("eglChooseConfig");
        const auto create = Egl<decltype(&eglCreateContext)>("eglCreateContext");
        const auto destroy = Egl<decltype(&eglDestroyContext)>("eglDestroyContext");
        const auto terminate = Egl<decltype(&eglTerminate)>("eglTerminate");
        const auto bind = Egl<decltype(&eglBindAPI)>("eglBindAPI");
        const auto api = Egl<decltype(&eglQueryAPI)>("eglQueryAPI");
        if (!get || !init || !choose || !create || !destroy || !terminate || !bind || !api)
            return;
        const auto display = get(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY || !init(display, nullptr, nullptr))
            return;
        const auto previous = api();
        if (bind(EGL_OPENGL_ES_API))
            for (int major : {3, 2}) {
                const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                          major == 3 ? 0x40 : EGL_OPENGL_ES2_BIT, EGL_NONE};
                EGLConfig config = nullptr;
                EGLint count = 0;
                if (!choose(display, attributes, &config, 1, &count) || count == 0)
                    continue;
                const EGLint options[]{EGL_CONTEXT_CLIENT_VERSION, major, EGL_NONE};
                const auto context = create(display, config, EGL_NO_CONTEXT, options);
                if (context != EGL_NO_CONTEXT) {
                    version = major << 16;
                    destroy(display, context);
                    break;
                }
            }
        bind(previous);
        terminate(display);
    });
    return version;
}

bool RegisterArtGl(JNIEnv* env)
{
    if (IsArm64TranslationEnabled())
        return true;
    auto field = [&](const char* type, const char* name) {
        const auto klass = ArtJni<&JNINativeInterface_::FindClass>(env, type);
        const auto result = klass ? ArtJni<&JNINativeInterface_::GetFieldID>(env, klass, name, "J") : nullptr;
        if (klass)
            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, klass);
        return result;
    };
    displayHandle = field("com/google/android/gles_jni/EGLDisplayImpl", "mEGLDisplay");
    configHandle = field("com/google/android/gles_jni/EGLConfigImpl", "mEGLConfig");
    egl14Handle = field("android/opengl/EGLObjectHandle", "mHandle");
    if (!displayHandle || !configHandle || !egl14Handle)
        return false;
    JNINativeMethod method{
        const_cast<char*>("_eglCreateWindowSurface"),
        const_cast<char*>(
            "(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;Ljava/lang/Object;[I)J"),
        reinterpret_cast<void*>(ImportAndroidScalars<decltype(&WindowSurface)>(
            "ART EGL software window", reinterpret_cast<uintptr_t>(&WindowSurface)))};
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "com/google/android/gles_jni/EGLImpl");
    const bool result = type && ArtJni<&JNINativeInterface_::RegisterNatives>(env, type, &method, 1) == JNI_OK;
    if (type)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    if (!result)
        return false;
    const JNINativeMethod methods[]{
        {const_cast<char*>("_eglCreateWindowSurface"),
         const_cast<char*>(
             "(Landroid/opengl/EGLDisplay;Landroid/opengl/EGLConfig;Ljava/lang/Object;[II)Landroid/opengl/EGLSurface;"),
         reinterpret_cast<void*>(ImportAndroidScalars<decltype(&WindowSurface14)>(
             "ART EGL14 software window", reinterpret_cast<uintptr_t>(&WindowSurface14)))},
        {const_cast<char*>("_eglCreateWindowSurfaceTexture"),
         const_cast<char*>(
             "(Landroid/opengl/EGLDisplay;Landroid/opengl/EGLConfig;Ljava/lang/Object;[II)Landroid/opengl/EGLSurface;"),
         reinterpret_cast<void*>(ImportAndroidScalars<decltype(&TextureSurface14)>(
             "ART EGL14 texture window", reinterpret_cast<uintptr_t>(&TextureSurface14)))}};
    const auto egl14 = ArtJni<&JNINativeInterface_::FindClass>(env, "android/opengl/EGL14");
    const bool bound = egl14 && ArtJni<&JNINativeInterface_::RegisterNatives>(env, egl14, methods, 2) == JNI_OK;
    if (egl14)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, egl14);
    return bound;
}
}
