#include "ArtSurfaceTextures.h"
#include "ArtSurfaces.h"
#include "../runtime/art/ArtJni.h"
#include "../abi/AndroidLibraryImports.h"
#include "../abi/TypedAndroidImport.h"
#include "../hle/system/stub-libandroid-hardware-buffer.h"
#include "../hle/system/stub-libmediandk-image-reader.h"
#include "../../host/graphics/GlSurface.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cstring>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace stone::windows {
namespace {
JavaVM* vm;
jclass textureClass;
jfieldID textureField, producerField;
jmethodID postFrame;

void Throw(JNIEnv* env, const char* type, const char* message)
{
    const auto klass = ArtJni<&JNINativeInterface_::FindClass>(env, type);
    if (klass) {
        ArtJni<&JNINativeInterface_::ThrowNew>(env, klass, message);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, klass);
    }
}

template <class F> F Proc(bool egl, const char* name)
{
    auto address = WindowsAngleExport(egl, name);
    if (!address) {
        const auto lookup =
            reinterpret_cast<decltype(&eglGetProcAddress)>(WindowsAngleExport(true, "eglGetProcAddress"));
        if (lookup)
            address = reinterpret_cast<uintptr_t>(lookup(name));
    }
    if (!address)
        throw std::runtime_error(std::string("Unavailable SurfaceTexture GL function: ") + name);
    return reinterpret_cast<F>(address);
}

EGLContext Current()
{
    return Proc<decltype(&eglGetCurrentContext)>(true, "eglGetCurrentContext")();
}

struct Texture {
    std::mutex mutex;
    uintptr_t surface = 0;
    jobject weakSelf = nullptr;
    std::atomic_bool released = false;
    EGLContext context = EGL_NO_CONTEXT;
    EGLDisplay display = EGL_NO_DISPLAY;
    GLuint consumer = 0, source = 0;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    int width = 0, height = 0, allocatedWidth = 0, allocatedHeight = 0;
    int64_t timestamp = 0;
    std::vector<uint8_t> rgba;

    void Event()
    {
        const std::lock_guard lock(mutex);
        if (released || !weakSelf)
            return;
        JNIEnv* env = nullptr;
        const auto getEnv = reinterpret_cast<jint (*)(JavaVM*, void**, jint)>(
            MakeNativeAndroidThunk(reinterpret_cast<uintptr_t>(vm->functions->GetEnv), "(LLI)I"));
        // Producers entering from image EGL or MediaCodec are attached ART threads.
        if (getEnv(vm, reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env)
            return;
        const jvalue args[]{jvalue{.l = weakSelf}};
        ArtJni<&JNINativeInterface_::CallStaticVoidMethodA>(env, textureClass, postFrame, args);
    }

    void Upload()
    {
        if (context == EGL_NO_CONTEXT || Current() != context)
            throw std::runtime_error("SurfaceTexture GL context is not current");
        if (rgba.empty())
            return;
        const auto bind = Proc<decltype(&glBindTexture)>(false, "glBindTexture");
        const auto parameter = Proc<decltype(&glTexParameteri)>(false, "glTexParameteri");
        GLint previous = 0, alignment = 0;
        Proc<decltype(&glGetIntegerv)>(false, "glGetIntegerv")(GL_TEXTURE_BINDING_2D, &previous);
        Proc<decltype(&glGetIntegerv)>(false, "glGetIntegerv")(GL_UNPACK_ALIGNMENT, &alignment);
        if (!source)
            Proc<decltype(&glGenTextures)>(false, "glGenTextures")(1, &source);
        bind(GL_TEXTURE_2D, source);
        parameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        parameter(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        parameter(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        parameter(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        Proc<decltype(&glPixelStorei)>(false, "glPixelStorei")(GL_UNPACK_ALIGNMENT, 4);
        if (image == EGL_NO_IMAGE_KHR || allocatedWidth != width || allocatedHeight != height) {
            if (image != EGL_NO_IMAGE_KHR)
                Proc<PFNEGLDESTROYIMAGEKHRPROC>(true, "eglDestroyImageKHR")(display, image);
            image = EGL_NO_IMAGE_KHR;
            Proc<decltype(&glTexImage2D)>(false, "glTexImage2D")(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                                                                 GL_UNSIGNED_BYTE, rgba.data());
            const EGLint attributes[]{EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
            image = Proc<PFNEGLCREATEIMAGEKHRPROC>(true, "eglCreateImageKHR")(
                display, context, EGL_GL_TEXTURE_2D_KHR, reinterpret_cast<EGLClientBuffer>(uintptr_t(source)),
                attributes);
            allocatedWidth = width;
            allocatedHeight = height;
        }
        else
            Proc<decltype(&glTexSubImage2D)>(false, "glTexSubImage2D")(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
                                                                       GL_UNSIGNED_BYTE, rgba.data());
        Proc<decltype(&glPixelStorei)>(false, "glPixelStorei")(GL_UNPACK_ALIGNMENT, alignment);
        bind(GL_TEXTURE_2D, previous);
        if (image == EGL_NO_IMAGE_KHR)
            throw std::runtime_error("Cannot create SurfaceTexture EGL image");
        bind(GL_TEXTURE_EXTERNAL_OES, consumer);
        Proc<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(false, "glEGLImageTargetTexture2DOES")(GL_TEXTURE_EXTERNAL_OES,
                                                                                         image);
        if (Proc<decltype(&glGetError)>(false, "glGetError")() != GL_NO_ERROR)
            throw std::runtime_error("Cannot upload SurfaceTexture pixels");
    }

    void ClearGl()
    {
        if (image != EGL_NO_IMAGE_KHR) {
            Proc<PFNEGLDESTROYIMAGEKHRPROC>(true, "eglDestroyImageKHR")(display, image);
            image = EGL_NO_IMAGE_KHR;
        }
        // GL names belong to the owning context. If it is no longer current,
        // its eventual destruction releases the remaining texture storage.
        if (context != EGL_NO_CONTEXT && Current() == context) {
            const auto remove = Proc<decltype(&glDeleteTextures)>(false, "glDeleteTextures");
            if (source)
                remove(1, &source);
            if (consumer)
                remove(1, &consumer);
        }
        source = consumer = 0;
        context = EGL_NO_CONTEXT;
        allocatedWidth = allocatedHeight = 0;
    }
};

std::mutex texturesMutex;
std::unordered_map<uintptr_t, std::shared_ptr<Texture>> textures;

std::shared_ptr<Texture> Find(JNIEnv* env, jobject object, bool allowReleased = false)
{
    const auto token = static_cast<uintptr_t>(ArtJni<&JNINativeInterface_::GetLongField>(env, object, textureField));
    const std::lock_guard lock(texturesMutex);
    const auto found = textures.find(token);
    if (found == textures.end() || (!allowReleased && found->second->released)) {
        Throw(env, "java/lang/IllegalStateException", "SurfaceTexture has been released");
        return {};
    }
    return found->second;
}

void Init(JNIEnv* env, jobject object, jboolean detached, jint name, jboolean single, jobject weakSelf)
try {
    if (single) {
        Throw(env, "java/lang/UnsupportedOperationException", "Single-buffer SurfaceTexture is unsupported");
        return;
    }
    auto state = std::make_shared<Texture>();
    if (!detached) {
        state->context = Current();
        state->display = Proc<decltype(&eglGetCurrentDisplay)>(true, "eglGetCurrentDisplay")();
        if (state->context == EGL_NO_CONTEXT || name <= 0)
            throw std::runtime_error("SurfaceTexture requires a current GL context and texture name");
        state->consumer = name;
    }
    state->weakSelf = ArtJni<&JNINativeInterface_::NewGlobalRef>(env, weakSelf);
    if (!state->weakSelf)
        return;
    std::weak_ptr<Texture> callback = state;
    state->surface = CreateArtSoftwareSurface(1, 1, [callback] {
        if (auto state = callback.lock())
            state->Event();
    });
    if (!state->surface) {
        ArtJni<&JNINativeInterface_::DeleteGlobalRef>(env, state->weakSelf);
        throw std::runtime_error("Cannot allocate SurfaceTexture queue");
    }
    const auto token = reinterpret_cast<uintptr_t>(state.get());
    {
        const std::lock_guard lock(texturesMutex);
        textures.emplace(token, state);
    }
    ArtJni<&JNINativeInterface_::SetLongField>(env, object, textureField, token);
    ArtJni<&JNINativeInterface_::SetLongField>(env, object, producerField, state->surface);
}
catch (const std::exception& error) {
    Throw(env, "java/lang/IllegalStateException", error.what());
}

void Release(JNIEnv* env, jobject object)
try {
    const auto state = Find(env, object, true);
    if (!state)
        return;
    const std::lock_guard lock(state->mutex);
    if (state->released)
        return;
    state->released = true;
    if (auto queue = ArtSoftwareSurfaceQueue(state->surface))
        CloseAndroidImageQueue(*queue);
    ReleaseArtSoftwareSurface(state->surface);
    if (state->weakSelf)
        ArtJni<&JNINativeInterface_::DeleteGlobalRef>(env, state->weakSelf);
    state->weakSelf = nullptr;
    state->rgba.clear();
    state->ClearGl();
}
catch (const std::exception& error) {
    Throw(env, "java/lang/IllegalStateException", error.what());
}

void Finalize(JNIEnv* env, jobject object)
{
    const auto token = static_cast<uintptr_t>(ArtJni<&JNINativeInterface_::GetLongField>(env, object, textureField));
    if (!token)
        return;
    Release(env, object);
    {
        const std::lock_guard lock(texturesMutex);
        textures.erase(token);
    }
    ArtJni<&JNINativeInterface_::SetLongField>(env, object, textureField, 0);
    ArtJni<&JNINativeInterface_::SetLongField>(env, object, producerField, 0);
}

void Update(JNIEnv* env, jobject object)
try {
    const auto state = Find(env, object);
    if (!state)
        return;
    const std::lock_guard lock(state->mutex);
    if (state->context == EGL_NO_CONTEXT || Current() != state->context)
        throw std::runtime_error("SurfaceTexture GL context is not current");
    const auto queue = ArtSoftwareSurfaceQueue(state->surface);
    if (!queue)
        throw std::runtime_error("SurfaceTexture queue is unavailable");
    std::unique_ptr<AndroidHardwareBuffer, decltype(&ReleaseAndroidHardwareBuffer)> latest(
        nullptr, ReleaseAndroidHardwareBuffer);
    int64_t timestamp;
    while (auto* next = TakeAndroidImageFrame(*queue, timestamp)) {
        latest.reset(next);
        state->timestamp = timestamp;
    }
    if (!latest)
        return;
    AndroidHardwareBufferDesc desc{};
    DescribeAndroidHardwareBuffer(latest.get(), &desc);
    void* pixels = nullptr;
    if (LockAndroidHardwareBuffer(latest.get(), 3, -1, nullptr, &pixels) != 0)
        throw std::runtime_error("Cannot read SurfaceTexture frame");

    struct Unlock {
        AndroidHardwareBuffer* buffer;

        ~Unlock()
        {
            UnlockAndroidHardwareBuffer(buffer, nullptr);
        }
    } unlock{latest.get()};

    state->width = desc.width;
    state->height = desc.height;
    state->rgba.resize(size_t(desc.width) * desc.height * 4);
    for (uint32_t y = 0; y < desc.height; y++)
        std::memcpy(state->rgba.data() + size_t(y) * desc.width * 4,
                    static_cast<uint8_t*>(pixels) + size_t(y) * desc.stride * 4, size_t(desc.width) * 4);
    state->Upload();
}
catch (const std::exception& error) {
    Throw(env, "java/lang/IllegalStateException", error.what());
}

template <class F> JNINativeMethod Method(const char* name, const char* signature, F function)
{
    return {const_cast<char*>(name), const_cast<char*>(signature),
            reinterpret_cast<void*>(ImportAndroidScalars<F>(std::string("ART SurfaceTexture ") + name + signature,
                                                            reinterpret_cast<uintptr_t>(function)))};
}
}

bool IsArtSurfaceTexture(uintptr_t surface)
{
    const std::lock_guard lock(texturesMutex);
    for (const auto& [token, state] : textures)
        if (state->surface == surface && !state->released)
            return true;
    return false;
}

bool RegisterArtSurfaceTextures(JNIEnv* env)
{
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "android/graphics/SurfaceTexture");
    if (!type)
        return false;
    textureClass = static_cast<jclass>(ArtJni<&JNINativeInterface_::NewGlobalRef>(env, type));
    textureField = ArtJni<&JNINativeInterface_::GetFieldID>(env, type, "mSurfaceTexture", "J");
    producerField = ArtJni<&JNINativeInterface_::GetFieldID>(env, type, "mProducer", "J");
    postFrame = ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "postEventFromNative",
                                                                "(Ljava/lang/ref/WeakReference;)V");
    ArtJni<&JNINativeInterface_::GetJavaVM>(env, &vm);
    if (!textureClass || !textureField || !producerField || !postFrame || !vm) {
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
        return false;
    }
    const JNINativeMethod methods[]{
        Method("nativeInit", "(ZIZLjava/lang/ref/WeakReference;)V", Init),
        Method("nativeFinalize", "()V", Finalize),
        Method("nativeRelease", "()V", Release),
        Method("nativeUpdateTexImage", "()V", Update),
        Method(
            "nativeReleaseTexImage", "()V",
            +[](JNIEnv* env, jobject object) {
                Find(env, object);
            }),
        Method(
            "nativeSetDefaultBufferSize", "(II)V",
            +[](JNIEnv* env, jobject object, jint width, jint height) {
                const auto state = Find(env, object);
                if (!state)
                    return;
                const std::lock_guard lock(state->mutex);
                const auto queue = ArtSoftwareSurfaceQueue(state->surface);
                if (!queue || ResizeAndroidSoftwareSurfaceQueue(*queue, width, height) != 0)
                    Throw(env, "java/lang/IllegalArgumentException", "Invalid or busy SurfaceTexture geometry");
            }),
        Method(
            "nativeIsReleased", "()Z",
            +[](JNIEnv* env, jobject object) -> jboolean {
                const auto state = Find(env, object, true);
                return !state || state->released;
            }),
        Method(
            "nativeGetTimestamp", "()J",
            +[](JNIEnv* env, jobject object) -> jlong {
                const auto state = Find(env, object);
                if (!state)
                    return 0;
                const std::lock_guard lock(state->mutex);
                return state->timestamp;
            }),
        Method(
            "nativeGetDataSpace", "()I",
            +[](JNIEnv* env, jobject object) -> jint {
                Find(env, object);
                return 0;
            }),
        Method(
            "nativeGetTransformMatrix", "([F)V",
            +[](JNIEnv* env, jobject object, jfloatArray matrix) {
                const auto state = Find(env, object);
                if (!state)
                    return;
                if (!matrix || ArtJni<&JNINativeInterface_::GetArrayLength>(env, matrix) != 16) {
                    Throw(env, "java/lang/IllegalArgumentException", "Invalid texture transform array");
                    return;
                }
                const jfloat transform[]{1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1};
                ArtJni<&JNINativeInterface_::SetFloatArrayRegion>(env, matrix, 0, 16, transform);
            }),
        Method(
            "nativeDetachFromGLContext", "()I",
            +[](JNIEnv* env, jobject object) -> jint {
                try {
                    const auto state = Find(env, object);
                    if (!state)
                        return -22;
                    const std::lock_guard lock(state->mutex);
                    if (state->context == EGL_NO_CONTEXT || Current() != state->context)
                        return -38;
                    state->ClearGl();
                    return 0;
                }
                catch (const std::exception& error) {
                    Throw(env, "java/lang/IllegalStateException", error.what());
                    return -38;
                }
            }),
        Method(
            "nativeAttachToGLContext", "(I)I", +[](JNIEnv* env, jobject object, jint name) -> jint {
                try {
                    const auto state = Find(env, object);
                    if (!state)
                        return -22;
                    const std::lock_guard lock(state->mutex);
                    if (state->context != EGL_NO_CONTEXT || name <= 0 || Current() == EGL_NO_CONTEXT)
                        return -38;
                    state->consumer = name;
                    state->context = Current();
                    state->display = Proc<decltype(&eglGetCurrentDisplay)>(true, "eglGetCurrentDisplay")();
                    state->Upload();
                    return 0;
                }
                catch (const std::exception& error) {
                    Throw(env, "java/lang/IllegalStateException", error.what());
                    return -38;
                }
            })};
    const bool ok = ArtJni<&JNINativeInterface_::RegisterNatives>(env, type, methods,
                                                                  static_cast<jint>(std::size(methods))) == JNI_OK;
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    if (!ok)
        return false;
    const auto surface = ArtJni<&JNINativeInterface_::FindClass>(env, "android/view/Surface");
    const JNINativeMethod create[]{Method(
        "nativeCreateFromSurfaceTexture", "(Landroid/graphics/SurfaceTexture;)J",
        +[](JNIEnv* env, jclass, jobject texture) -> jlong {
            const auto state = Find(env, texture);
            if (!state)
                return 0;
            const std::lock_guard lock(state->mutex);
            return RetainArtSoftwareSurface(state->surface);
        })};
    const bool result = surface && ArtJni<&JNINativeInterface_::RegisterNatives>(env, surface, create, 1) == JNI_OK;
    if (surface)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, surface);
    return result;
}
}
