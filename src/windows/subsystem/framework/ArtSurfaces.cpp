#include "ArtSurfaces.h"
#include "ArtSurfaceTextures.h"
#include "../runtime/art/ArtJni.h"
#include "../abi/AndroidLibraryImports.h"
#include "../loader/AndroidSystemElf.h"
#include "../abi/TypedAndroidImport.h"
#include "../hle/system/stub-libandroid.h"
#include "../hle/system/stub-libandroid-hardware-buffer.h"
#include "../hle/system/stub-libmediandk-image-reader.h"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cmath>

namespace stone::windows {
namespace {
struct SoftwareSurface {
    std::shared_ptr<AndroidImageQueue> queue;
    NativeWindow* window = nullptr;
    std::mutex pixelsMutex;
    std::vector<jint> pixels;
    size_t owners = 1; // Java Surface and SurfaceTexture references, guarded by gate.

    ~SoftwareSurface()
    {
        if (queue)
            CloseAndroidImageQueue(*queue);
        if (window)
            ReleaseAndroidImageWindow(window);
    }
};

std::mutex gate;
std::unordered_map<uintptr_t, std::shared_ptr<SoftwareSurface>> surfaces;
jfieldID nativeObject;
uintptr_t imageValid, imageRelease, imageDestroy, clearTransaction, imageCopy, imageSetFrameRate;

std::shared_ptr<SoftwareSurface> Find(uintptr_t token)
{
    const std::lock_guard lock(gate);
    auto found = surfaces.find(token);
    return found == surfaces.end() ? nullptr : found->second;
}

void Throw(JNIEnv* env, const char* message)
{
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/UnsupportedOperationException");
    if (type)
        ArtJni<&JNINativeInterface_::ThrowNew>(env, type, message);
}
jlong Create(JNIEnv* env, jclass, jobject surface, jint width, jint height)
try {
    if (!surface || width <= 0 || height <= 0 || width > 8192 || height > 8192 || uint64_t(width) * height > 16777216) {
        Throw(env, "invalid software Surface dimensions");
        return 0;
    }
    const auto token = CreateArtSoftwareSurface(width, height);
    if (!token) {
        Throw(env, "cannot allocate software Surface queue");
        return 0;
    }
    ArtJni<&JNINativeInterface_::SetLongField>(env, surface, nativeObject, static_cast<jlong>(token));
    return static_cast<jlong>(token);
}
catch (const std::exception& error) {
    Throw(env, error.what());
    return 0;
}

jintArray SurfacePixels(JNIEnv* env, jlong token, bool snapshot)
{
    const auto state = Find(static_cast<uintptr_t>(token));
    if (!state)
        return nullptr;
    const std::lock_guard lock(state->pixelsMutex);
    AndroidHardwareBuffer* latest = snapshot ? PeekAndroidImageFrame(*state->queue) : nullptr;
    int64_t time;
    while (!snapshot) {
        auto* next = TakeAndroidImageFrame(*state->queue, time);
        if (!next)
            break;
        if (latest)
            ReleaseAndroidHardwareBuffer(latest);
        latest = next;
    }
    if (!latest && (!snapshot || state->pixels.empty()))
        return nullptr;
    if (latest) {
        struct Release {
            AndroidHardwareBuffer* buffer;

            ~Release()
            {
                ReleaseAndroidHardwareBuffer(buffer);
            }
        } release{latest};

        AndroidHardwareBufferDesc description{};
        DescribeAndroidHardwareBuffer(latest, &description);
        void* address = nullptr;
        if (LockAndroidHardwareBuffer(latest, 3, -1, nullptr, &address) != 0)
            return nullptr;

        struct Unlock {
            AndroidHardwareBuffer* buffer;

            ~Unlock()
            {
                if (buffer)
                    UnlockAndroidHardwareBuffer(buffer, nullptr);
            }
        } unlock{latest};

        std::vector<jint> pixels(static_cast<size_t>(description.width) * description.height);
        const auto* rgba = static_cast<const uint8_t*>(address);
        for (size_t y = 0; y < description.height; ++y)
            for (size_t x = 0; x < description.width; ++x) {
                const auto* p = rgba + (y * description.stride + x) * 4;
                pixels[y * description.width + x] =
                    static_cast<jint>((uint32_t(p[3]) << 24) | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]);
            }
        UnlockAndroidHardwareBuffer(latest, nullptr);
        unlock.buffer = nullptr;
        state->pixels = std::move(pixels);
    }
    const jsize offset = snapshot ? 2 : 0;
    auto result = ArtJni<&JNINativeInterface_::NewIntArray>(env, static_cast<jsize>(state->pixels.size()) + offset);
    if (result && snapshot) {
        const jint size[]{AndroidImageQueueWidth(*state->queue), AndroidImageQueueHeight(*state->queue)};
        ArtJni<&JNINativeInterface_::SetIntArrayRegion>(env, result, 0, 2, size);
    }
    if (result)
        ArtJni<&JNINativeInterface_::SetIntArrayRegion>(env, result, offset, static_cast<jsize>(state->pixels.size()),
                                                        state->pixels.data());
    return result;
}
jintArray Pixels(JNIEnv* env, jclass, jlong token)
try {
    return SurfacePixels(env, token, false);
}
catch (const std::exception& error) {
    Throw(env, error.what());
    return nullptr;
}
void CopySurface(JNIEnv* env, jclass type, jobject surface, jint left, jint top, jint right, jint bottom,
                 jobject request)
try {
    const auto token = surface ? ArtJni<&JNINativeInterface_::GetLongField>(env, surface, nativeObject) : 0;
    if (!Find(static_cast<uintptr_t>(token))) {
        using Function = void (*)(JNIEnv*, jclass, jobject, jint, jint, jint, jint, jobject);
        reinterpret_cast<Function>(MakeNativeAndroidThunk(imageCopy, "(LLLIIIIL)V"))(env, type, surface, left, top,
                                                                                     right, bottom, request);
        return;
    }
    const auto pixels = SurfacePixels(env, token, true);
    if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
        return;
    const auto helper = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/runtime/ArtPixelCopy");
    const auto copy =
        helper ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, helper, "copy", "([IIIIILjava/lang/Object;)V")
               : nullptr;
    const jvalue args[]{jvalue{.l = pixels}, jvalue{.i = left},   jvalue{.i = top},
                        jvalue{.i = right},  jvalue{.i = bottom}, jvalue{.l = request}};
    if (copy)
        ArtJni<&JNINativeInterface_::CallStaticVoidMethodA>(env, helper, copy, args);
    if (helper)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, helper);
    if (pixels)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, pixels);
}
catch (const std::exception& error) {
    Throw(env, error.what());
}

void Stop(JNIEnv*, jclass, jlong token)
{
    if (const auto state = Find(static_cast<uintptr_t>(token)))
        EndAndroidImageQueueInput(*state->queue);
}

void Destroy(JNIEnv*, jclass, jlong token)
{
    if (const auto state = Find(static_cast<uintptr_t>(token))) {
        CloseAndroidImageQueue(*state->queue);
        return;
    }
    if (token)
        reinterpret_cast<void (*)(void*)>(MakeNativeAndroidThunk(imageDestroy, "(L)V"))(reinterpret_cast<void*>(token));
}

void Release(JNIEnv* env, jclass type, jlong token)
{
    if (ReleaseArtSoftwareSurface(static_cast<uintptr_t>(token)))
        return;
    if (token)
        reinterpret_cast<void (*)(JNIEnv*, jclass, jlong)>(MakeNativeAndroidThunk(imageRelease, "(LLJ)V"))(env, type,
                                                                                                           token);
}

jboolean Valid(JNIEnv* env, jclass type, jlong token)
{
    if (Find(static_cast<uintptr_t>(token)))
        return JNI_TRUE;
    if (!token)
        return JNI_FALSE;
    return reinterpret_cast<jboolean (*)(JNIEnv*, jclass, jlong)>(MakeNativeAndroidThunk(imageValid, "(LLJ)I"))(
        env, type, token);
}

jint SetFrameRate(JNIEnv* env, jclass type, jlong token, jfloat rate, jint compatibility, jint strategy)
{
    if (const auto state = Find(static_cast<uintptr_t>(token))) {
        if (!std::isfinite(rate) || rate < 0 ||
            (rate != 0 && (compatibility < 0 || compatibility > 2 || strategy < 0 || strategy > 1)))
            return -22; // EINVAL
        if (rate == 0)
            return 0; // No refresh-rate vote is retained on a software queue.
        // A Stone software queue is not an image Surface/ANativeWindow. It has
        // no compositor refresh-rate voting or display-mode switching support.
        return -38; // ENOSYS
    }
    if (!token)
        return -22;
    using Function = jint (*)(JNIEnv*, jclass, jlong, jfloat, jint, jint);
    return reinterpret_cast<Function>(MakeNativeAndroidThunk(imageSetFrameRate, "(LLJFII)I"))(env, type, token, rate,
                                                                                              compatibility, strategy);
}

template <class F> JNINativeMethod Method(const char* name, const char* signature, F function)
{
    return {const_cast<char*>(name), const_cast<char*>(signature),
            reinterpret_cast<void*>(ImportAndroidScalars<F>(std::string("ART software Surface ") + name + signature,
                                                            reinterpret_cast<uintptr_t>(function)))};
}

bool Bind(JNIEnv* env, const char* name, const JNINativeMethod* methods, int count)
{
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, name);
    if (!type)
        return false;
    bool result = ArtJni<&JNINativeInterface_::RegisterNatives>(env, type, methods, count) == JNI_OK;
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    return result;
}
}

uintptr_t ArtSurfaceFromJava(JNIEnv* env, jobject surface)
{
    if (!surface)
        return 0;
    if (!nativeObject) {
        Throw(env, "software Surface conversion requires x64 image ART");
        return 0;
    }
    const auto token = ArtJni<&JNINativeInterface_::GetLongField>(env, surface, nativeObject);
    const auto state = Find(static_cast<uintptr_t>(token));
    if (!state) {
        Throw(env, "image Surface has no Windows software queue");
        return 0;
    }
    AcquireWindow(state->window);
    return reinterpret_cast<uintptr_t>(state->window);
}

uintptr_t CreateArtSoftwareSurface(int width, int height, std::function<void()> onFrame)
{
    auto queue = CreateAndroidSoftwareSurfaceQueue(width, height, std::move(onFrame));
    if (!queue)
        return 0;
    auto state = std::make_shared<SoftwareSurface>();
    state->queue = std::move(queue);
    state->window = static_cast<NativeWindow*>(CreateAndroidImageWindow(state->queue));
    const auto token = reinterpret_cast<uintptr_t>(state->window);
    const std::lock_guard lock(gate);
    surfaces.emplace(token, state);
    return token;
}

uintptr_t RetainArtSoftwareSurface(uintptr_t token)
{
    const std::lock_guard lock(gate);
    auto found = surfaces.find(token);
    if (found == surfaces.end())
        return 0;
    ++found->second->owners;
    return token;
}

bool ReleaseArtSoftwareSurface(uintptr_t token)
{
    std::shared_ptr<SoftwareSurface> released;
    {
        const std::lock_guard lock(gate);
        auto found = surfaces.find(token);
        if (found == surfaces.end())
            return false;
        if (--found->second->owners == 0) {
            released = std::move(found->second);
            surfaces.erase(found);
        }
    }
    return true;
}

std::shared_ptr<AndroidImageQueue> ArtSoftwareSurfaceQueue(uintptr_t token)
{
    const auto state = Find(token);
    return state ? state->queue : nullptr;
}

bool RegisterArtSurfaces(JNIEnv* env)
{
    // Only x64 application-native rendering is supported by this bridge.
    if (IsArm64TranslationEnabled())
        return true;
    imageValid =
        AndroidRuntimeFunction("libandroid_runtime.so", "_ZN7androidL13nativeIsValidEP7_JNIEnvP7_jclassl", true);
    imageRelease =
        AndroidRuntimeFunction("libandroid_runtime.so", "_ZN7androidL13nativeReleaseEP7_JNIEnvP7_jclassl", true);
    imageSetFrameRate = AndroidRuntimeFunction("libandroid_runtime.so",
                                               "_ZN7androidL18nativeSetFrameRateEP7_JNIEnvP7_jclasslfii", true);
    imageDestroy = AndroidRuntimeFunction("libgui.so", "_ZN7android7Surface7destroyEv");
    imageCopy = AndroidRuntimeFunction(
        "libhwui.so", "_ZN7androidL45android_view_ThreadedRenderer_copySurfaceIntoEP7_JNIEnvP8_jobjectS3_iiiiS3_",
        true);
    clearTransaction = AndroidRuntimeFunction("libgui.so", "_ZN7android21SurfaceComposerClient11Transaction5clearEv");
    auto surface = ArtJni<&JNINativeInterface_::FindClass>(env, "android/view/Surface");
    nativeObject = surface ? ArtJni<&JNINativeInterface_::GetFieldID>(env, surface, "mNativeObject", "J") : nullptr;
    if (surface)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, surface);
    if (!nativeObject)
        return false;
    const JNINativeMethod native[]{Method("createNative", "(Landroid/view/Surface;II)J", Create),
                                   Method("pixelsNative", "(J)[I", Pixels), Method("stopNative", "(J)V", Stop)};
    if (!Bind(env, "com/trebledot/stone/runtime/ArtSurfaceViews", native, 3))
        return false;
    const JNINativeMethod methods[]{Method("nativeIsValid", "(J)Z", Valid), Method("nativeRelease", "(J)V", Release),
                                    Method("nativeDestroy", "(J)V", Destroy),
                                    Method("nativeSetFrameRate", "(JFII)I", SetFrameRate)};
    if (!Bind(env, "android/view/Surface", methods, 4))
        return false;
    const JNINativeMethod copy[]{Method("nCopySurfaceInto",
                                        "(Landroid/view/Surface;IIIILandroid/graphics/HardwareRenderer$CopyRequest;)V",
                                        CopySurface)};
    if (!Bind(env, "android/graphics/HardwareRenderer", copy, 1))
        return false;
    const JNINativeMethod transaction[]{Method(
        "nativeApplyTransaction", "(JZZ)V", +[](JNIEnv*, jclass, jlong object, jboolean, jboolean) {
            // No image compositor layers can be created in this software host.
            // SurfaceView still applies empty release/metadata transactions.
            if (object)
                reinterpret_cast<void (*)(void*)>(MakeNativeAndroidThunk(clearTransaction, "(L)V"))(
                    reinterpret_cast<void*>(object));
        })};
    return Bind(env, "android/view/SurfaceControl", transaction, 1) && RegisterArtSurfaceTextures(env);
}
}
