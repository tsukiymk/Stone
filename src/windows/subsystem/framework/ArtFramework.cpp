#include "cpu/arm64/Arm64ExecutionSession.h"
#include "ArtFramework.h"
#include "ArtFileObserver.h"
#include "ArtDocumentTrees.h"
#include "ArtProcesses.h"
#include "ArtNotifications.h"
#include "ArtApplicationLauncher.h"
#include "../platform/windows/ArtSettingsConfirmation.h"
#include "../runtime/art/ArtJni.h"
#include "../runtime/art/ArtAot.h"
#include "ArtSurfaces.h"
#include "ArtGl.h"
#include "ArtAudio.h"
#include "ArtParcels.h"
#include "../../host/audio/AudioOutput.h"
#include "../abi/Arm64HostJni.h"
#include "../cpu/arm64/Arm64ExecutionProfile.h"
#include "../abi/AndroidLibraryImports.h"
#include "../filesystem/AndroidSystemFiles.h"
#include "../abi/TypedAndroidImport.h"
#include "hle/system/stub-libc.h"
#include "hle/system/stub-libc-properties.h"
#include "stub-liblog.h"
#include "hle/system/stub-libandroid-input.h"
#include "hle/system/stub-libnativeloader.h"
#include "../../client/Window.h"
#include "../../client/HostConfiguration.h"
#include "../../client/BrokerClient.h"
#include "../../client/Sharing.h"
#include "../../client/Documents.h"

#include <windows.h>
#include <atomic>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>
#include <winrt/base.h>

namespace stone::windows {
namespace {
constexpr jint BackAction = -1;
constexpr jint KeyDownAction = -2, KeyUpAction = -3, TextAction = -4, FocusAction = -5;
constexpr jint VerticalScrollAction = -6, HorizontalScrollAction = -7;
constexpr size_t WindowInputSize = 4;
std::mutex windowInputMutex;
std::vector<jint> windowInput;
bool windowInputAllocationFailed = false;
bool windowInputQueueOverflow = false;
thread_local std::vector<jint> framePixels;
thread_local HANDLE applicationLock = nullptr;
thread_local HANDLE legacyApplicationLock = nullptr;
thread_local HANDLE processApplicationLock = nullptr;
void Throw(JNIEnv* env, const char* name, const char* message);

std::wstring NotificationText(JNIEnv* env, jstring value, jsize maximum)
{
    if (!value)
        return {};
    const auto size = ArtJni<&JNINativeInterface_::GetStringLength>(env, value);
    if (size > maximum)
        throw std::runtime_error("notification text exceeds its size limit");
    const auto* chars = ArtJni<&JNINativeInterface_::GetStringChars>(env, value, nullptr);
    if (!chars)
        throw std::runtime_error("could not read notification text");
    try {
        std::wstring result(reinterpret_cast<const wchar_t*>(chars), static_cast<size_t>(size));
        ArtJni<&JNINativeInterface_::ReleaseStringChars>(env, value, chars);
        return result;
    }
    catch (...) {
        ArtJni<&JNINativeInterface_::ReleaseStringChars>(env, value, chars);
        throw;
    }
}

template <class Action> auto NotificationCall(JNIEnv* env, Action action) -> decltype(action())
{
    try {
        return action();
    }
    catch (const winrt::hresult_error& error) {
        Throw(env, "java/io/IOException", winrt::to_string(error.message()).c_str());
    }
    catch (const AndroidSystemPathError&) {
        Throw(env, "java/io/IOException", "could not resolve notification resources");
    }
    catch (const std::exception& error) {
        Throw(env, "java/io/IOException", error.what());
    }
    if constexpr (!std::is_void_v<decltype(action())>)
        return {};
}

void QueueWindowInput(jint action, jint first = 0, jint second = 0, jint third = 0) noexcept
{
    const std::lock_guard lock(windowInputMutex);
    if (windowInputAllocationFailed || windowInputQueueOverflow)
        return;
    const auto size = windowInput.size();
    if (action == 2 && size >= WindowInputSize && windowInput[size - WindowInputSize] == 2) {
        windowInput[size - 3] = first;
        windowInput[size - 2] = second;
        windowInput[size - 1] = third;
        return;
    }
    if (size >= WindowInputSize * 4096) {
        windowInputQueueOverflow = true;
        return;
    }
    try {
        windowInput.insert(windowInput.end(), {action, first, second, third});
    }
    catch (const std::bad_alloc&) {
        windowInputAllocationFailed = true;
    }
}

bool OpenArtWindow()
{
    const bool opened = OpenWindowsArtWindow({
        +[](int action, float x, float y, int buttons) noexcept {
            QueueWindowInput(action, static_cast<jint>(x), static_cast<jint>(y), buttons);
        },
        +[]() noexcept {
            QueueWindowInput(BackAction);
        },
        +[](bool down, int code, int modifiers, int repeat) noexcept {
            QueueWindowInput(down ? KeyDownAction : KeyUpAction, code, modifiers, repeat);
        },
        +[](wchar_t character) noexcept {
            QueueWindowInput(TextAction, character);
        },
        +[](bool focused) noexcept {
            QueueWindowInput(FocusAction, focused);
        },
        +[](bool horizontal, float x, float y, int delta) noexcept {
            QueueWindowInput(horizontal ? HorizontalScrollAction : VerticalScrollAction, static_cast<jint>(x),
                             static_cast<jint>(y), delta);
        },
    });
    if (opened)
        PublishArtApplicationWindow();
    return opened;
}

bool RegisterImageNative(JNIEnv* environment, const char* library, const char* name, bool androidNamespace = true)
{
    const auto symbol = (androidNamespace ? "_ZN7android" : "_Z") + std::to_string(std::strlen(name)) + name +
                        (androidNamespace ? "EP7_JNIEnv" : "P7_JNIEnv");
    const auto address = AndroidRuntimeFunction(library, symbol);
    std::fprintf(stderr, "[art] registering %s: %s\n", library, name);
    return reinterpret_cast<int (*)(JNIEnv*)>(MakeNativeAndroidThunk(address, "(L)I"))(environment) >= 0;
}
}

bool RegisterArtResources(JNIEnv* environment)
{
    const auto library = DlOpen("libandroid_runtime.so", 2);
    if (!library) {
        const auto error = DlError();
        throw std::runtime_error(std::string("could not load Android resource runtime: ") +
                                 (error ? error : "unknown error"));
    }
    LoadArtImageLibrary("libandroid.so");
    for (const auto* name : {"register_android_os_Trace", "register_android_content_AssetManager",
                             "register_android_content_StringBlock", "register_android_content_XmlBlock",
                             "register_android_content_res_ApkAssets", "register_android_content_res_ResourceTimer"}) {
        if (!RegisterImageNative(environment, "libandroid_runtime.so", name))
            return false;
    }
    return true;
}

bool RegisterArtGraphics(JNIEnv* environment)
{
    const auto library = DlOpen("libhwui.so", 2);
    if (!library)
        throw std::runtime_error(std::string("could not load Android graphics: ") + DlError());
    // Bitmap NDK dependencies also import AAsset_*; prepare the real provider
    // when graphics is registered independently of the resource host.
    LoadArtImageLibrary("libandroid.so");
    const auto init = DlSym(library, "init_android_graphics");
    if (!init)
        throw std::runtime_error("missing Android graphics initialization export");
    reinterpret_cast<void (*)()>(MakeNativeAndroidThunk(reinterpret_cast<uintptr_t>(init), "()V"))();
    // Register the software drawing stack first. The aggregate hwui entry point
    // also initializes SurfaceControl and the not-yet-implemented compositor.
    const auto bind = [&](const char* suffix, bool androidNamespace = true) {
        const auto name = std::string("register_android_graphics_") + suffix;
        return RegisterImageNative(environment, "libhwui.so", name.c_str(), androidNamespace);
    };
    for (const auto* name : {"Canvas", "Color", "ColorSpace"})
        if (!bind(name))
            return false;
    for (const auto* name : {"Graphics", "Bitmap"})
        if (!bind(name, false))
            return false;
    for (const auto* name : {"Matrix", "Paint", "Path", "PathMeasure", "Region", "ColorFilter", "DrawFilter"})
        if (!bind(name))
            return false;
    for (const auto* name : {"Typeface", "MaskFilter", "PathEffect", "Shader"})
        if (!bind(name, false))
            return false;
    for (const auto* name : {"FontFamily", "fonts_Font", "fonts_FontFamily"})
        if (!bind(name))
            return false;
    return true;
}

namespace {
template <class Function> Function Looper(const char* name)
{
    return reinterpret_cast<Function>(AndroidLooperImport(name, true));
}

void Throw(JNIEnv* env, const char* name, const char* message)
{
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, name);
    if (type) {
        ArtJni<&JNINativeInterface_::ThrowNew>(env, type, message);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    }
}

class StringChars {
public:
    StringChars(JNIEnv* env, jstring string)
        : env_(env), string_(string),
          value_(string ? ArtJni<&JNINativeInterface_::GetStringUTFChars>(env, string, nullptr) : nullptr)
    {
    }

    ~StringChars()
    {
        if (value_)
            ArtJni<&JNINativeInterface_::ReleaseStringUTFChars>(env_, string_, value_);
    }

    const char* Get() const
    {
        return value_;
    }

private:
    JNIEnv* env_;
    jstring string_;
    const char* value_;
};

std::string_view Property(JNIEnv* env, jstring key)
{
    if (!key) {
        Throw(env, "java/lang/NullPointerException", "system property name is null");
        return {};
    }
    const StringChars name(env, key);
    return name.Get() ? AndroidSystemProperty(name.Get()) : std::string_view{};
}

template <class Integer> Integer PropertyNumber(std::string_view text, Integer fallback)
{
    if (text.empty())
        return fallback;
    const int previousError = errno;
    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoll(text.data(), &end, 0);
    const bool valid = errno != ERANGE && end == text.data() + text.size() &&
                       parsed >= std::numeric_limits<Integer>::min() && parsed <= std::numeric_limits<Integer>::max();
    errno = previousError;
    return valid ? static_cast<Integer>(parsed) : fallback;
}

struct LocalBinderHolder {
    uint64_t identity;

    LocalBinderHolder()
    {
        static std::atomic<uint64_t> next{1};
        identity = next.fetch_add(1);
    }
};

// Only local Java Binder objects are supported at this stage. There is no
// incoming remote identity or pending kernel command stream in this backend.
thread_local bool explicitBinderIdentity = false;

jlong BinderFinalizer()
{
    static const auto address = [] {
        const auto release = +[](jlong holder) {
            delete reinterpret_cast<LocalBinderHolder*>(holder);
        };
        // NativeAllocationRegistry calls this as a raw native function, rather
        // than registering it through JNI. Publish its ARM64 scalar adapter here.
        if (IsArm64TranslationEnabled()) {
            const auto source = ImportAndroidScalars<decltype(release)>("ART local Binder finalizer",
                                                                        reinterpret_cast<uintptr_t>(release));
            return PrepareArm64HostJniTarget("ART local Binder finalizer", "(J)V", source);
        }
        return Import("ART local Binder finalizer", release);
    }();
    return static_cast<jlong>(address);
}

struct MessageQueue {
    void* looper;
    const DWORD owner = GetCurrentThreadId();
    std::atomic<bool> polling{false};

    explicit MessageQueue(void* value) : looper(value)
    {
        Looper<void (*)(void*)>("ALooper_acquire")(looper);
    }

    ~MessageQueue()
    {
        Looper<void (*)(void*)>("ALooper_release")(looper);
    }
};

void WatchDescriptor(JNIEnv* env, jclass, jlong pointer, jint fd, jint events);

jlong CreateQueue(JNIEnv* env, jclass)
{
    auto* looper = Looper<void* (*)(int)>("ALooper_prepare")(1);
    auto* queue = looper ? new (std::nothrow) MessageQueue(looper) : nullptr;
    if (!queue)
        Throw(env, "java/lang/OutOfMemoryError", "could not create the Windows message queue");
    return reinterpret_cast<jlong>(queue);
}

void WakeQueue(JNIEnv*, jclass, jlong pointer)
{
    if (const auto* queue = reinterpret_cast<MessageQueue*>(pointer))
        Looper<void (*)(void*)>("ALooper_wake")(queue->looper);
}

void PollQueue(JNIEnv* env, jobject instance, jlong pointer, jint timeout)
{
    auto* queue = reinterpret_cast<MessageQueue*>(pointer);
    if (!queue)
        return;
    if (queue->owner != GetCurrentThreadId()) {
        Throw(env, "java/lang/IllegalStateException", "message queue polled from a different thread");
        return;
    }
    queue->polling.store(true);

    struct Clear {
        std::atomic<bool>& value;

        ~Clear()
        {
            value.store(false);
        }
    } clear{queue->polling};

    int fd = 0, events = 0;
    const int result = Looper<int (*)(int, int*, int*, void**)>("ALooper_pollOnce")(timeout, &fd, &events, nullptr);
    queue->polling.store(false);
    if (result == -4) {
        Throw(env, "java/lang/IllegalStateException", "Windows message queue polling failed");
        return;
    }
    if (result < 0)
        return;
    const auto type = ArtJni<&JNINativeInterface_::GetObjectClass>(env, instance);
    const auto dispatch =
        type ? ArtJni<&JNINativeInterface_::GetMethodID>(env, type, "dispatchEvents", "(II)I") : nullptr;
    if (dispatch) {
        const jvalue args[]{jvalue{.i = fd}, jvalue{.i = events}};
        const auto remaining = ArtJni<&JNINativeInterface_::CallIntMethodA>(env, instance, dispatch, args);
        if (!ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            WatchDescriptor(env, nullptr, pointer, fd, remaining);
    }
    if (type)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
}

void WatchDescriptor(JNIEnv* env, jclass, jlong pointer, jint fd, jint events)
{
    const auto* queue = reinterpret_cast<MessageQueue*>(pointer);
    if (!queue)
        return;
    if (!events) {
        Looper<int (*)(void*, int)>("ALooper_removeFd")(queue->looper, fd);
        return;
    }
    // The existing Windows looper supports readable timerfd/eventfd registrations.
    if (events != 1 || Looper<int (*)(void*, int, int, int, uintptr_t, void*)>("ALooper_addFd")(queue->looper, fd, fd,
                                                                                                1, 0, nullptr) < 0)
        Throw(env, "java/lang/UnsupportedOperationException", "unsupported Windows message queue descriptor or events");
}

template <class Function> JNINativeMethod Native(const char* name, const char* signature, Function function)
{
    // Import caches by label. Different classes may use the same method name
    // (even with different signatures); never share their ABI entry points.
    const auto label =
        std::string("ART framework ") + name + signature + "@" + std::to_string(reinterpret_cast<uintptr_t>(function));
    return {const_cast<char*>(name), const_cast<char*>(signature),
            reinterpret_cast<void*>(IsArm64TranslationEnabled()
                                        ? ImportAndroidScalars<Function>(label, reinterpret_cast<uintptr_t>(function))
                                        : Import(label, function))};
}

template <size_t Count> bool Bind(JNIEnv* env, const char* name, const JNINativeMethod (&methods)[Count])
{
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, name);
    if (!type)
        return false;
    const bool result =
        ArtJni<&JNINativeInterface_::RegisterNatives>(env, type, methods, static_cast<jint>(Count)) == JNI_OK;
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    return result;
}

template <class Function> JNINativeMethod ScalarNative(const char* name, const char* signature, Function function)
{
    return {const_cast<char*>(name), const_cast<char*>(signature),
            reinterpret_cast<void*>(ImportAndroidScalars<Function>(std::string("ART scalar ") + name + signature,
                                                                   reinterpret_cast<uintptr_t>(function)))};
}

jlong ClockNanos(int clock)
{
    AndroidTimespec time{};
    if (AndroidClock(clock, &time))
        std::abort();
    return time.seconds * 1'000'000'000 + time.nanos;
}

jint HostNetworkState(JNIEnv*, jclass)
{
    return ReadClientHostState().network;
}

using ImageDecoder = jobject (*)(JNIEnv*, jclass, jlong, jobject, jboolean, jint, jint, jobject, jboolean, jint,
                                 jboolean, jboolean, jboolean, jlong, jboolean, jlong);
ImageDecoder decodeImageBitmap = nullptr;

jobject DecodeBitmapFactory(JNIEnv* env, const char* method, const char* signature, const jvalue* args)
{
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/runtime/ArtBitmapFactory");
    if (!type)
        return nullptr;
    const auto entry = ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, method, signature);
    const auto result = entry ? ArtJni<&JNINativeInterface_::CallStaticObjectMethodA>(env, type, entry, args) : nullptr;
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    return result;
}

bool BindSoftwareBitmapFactory(JNIEnv* env)
{
    constexpr auto stream =
        "(Ljava/io/InputStream;[BLandroid/graphics/Rect;Landroid/graphics/BitmapFactory$Options;JJ)Landroid/graphics/Bitmap;";
    constexpr auto descriptor =
        "(Ljava/io/FileDescriptor;Landroid/graphics/Rect;Landroid/graphics/BitmapFactory$Options;JJ)Landroid/graphics/Bitmap;";
    constexpr auto asset =
        "(JLandroid/graphics/Rect;Landroid/graphics/BitmapFactory$Options;JJ)Landroid/graphics/Bitmap;";
    constexpr auto bytes = "([BIILandroid/graphics/BitmapFactory$Options;JJ)Landroid/graphics/Bitmap;";
    // These remain the selected image's decoder functions; only the Options
    // storage preference is adapted before entering them.
    const JNINativeMethod image[]{
        {const_cast<char*>("streamImage"), const_cast<char*>(stream),
         reinterpret_cast<void*>(AndroidRuntimeFunction(
             "libhwui.so", "_ZL18nativeDecodeStreamP7_JNIEnvP8_jobjectS2_P11_jbyteArrayS2_S2_ll", true))},
        {const_cast<char*>("descriptorImage"), const_cast<char*>(descriptor),
         reinterpret_cast<void*>(AndroidRuntimeFunction(
             "libhwui.so", "_ZL26nativeDecodeFileDescriptorP7_JNIEnvP8_jobjectS2_S2_S2_ll", true))},
        {const_cast<char*>("assetImage"), const_cast<char*>(asset),
         reinterpret_cast<void*>(
             AndroidRuntimeFunction("libhwui.so", "_ZL17nativeDecodeAssetP7_JNIEnvP8_jobjectlS2_S2_ll", true))},
        {const_cast<char*>("bytesImage"), const_cast<char*>(bytes),
         reinterpret_cast<void*>(AndroidRuntimeFunction(
             "libhwui.so", "_ZL21nativeDecodeByteArrayP7_JNIEnvP8_jobjectP11_jbyteArrayiiS2_ll", true))},
    };
    if (!Bind(env, "com/trebledot/stone/runtime/ArtBitmapFactory", image))
        return false;
    const JNINativeMethod factory[]{
        ScalarNative(
            "nativeDecodeStream", stream,
            +[](JNIEnv* env, jclass, jobject input, jbyteArray storage, jobject padding, jobject options, jlong bitmap,
                jlong colorSpace) -> jobject {
                const jvalue args[]{jvalue{.l = input},   jvalue{.l = storage}, jvalue{.l = padding},
                                    jvalue{.l = options}, jvalue{.j = bitmap},  jvalue{.j = colorSpace}};
                return DecodeBitmapFactory(env, "decodeStream", stream, args);
            }),
        ScalarNative(
            "nativeDecodeFileDescriptor", descriptor,
            +[](JNIEnv* env, jclass, jobject input, jobject padding, jobject options, jlong bitmap,
                jlong colorSpace) -> jobject {
                const jvalue args[]{jvalue{.l = input}, jvalue{.l = padding}, jvalue{.l = options}, jvalue{.j = bitmap},
                                    jvalue{.j = colorSpace}};
                return DecodeBitmapFactory(env, "decodeFileDescriptor", descriptor, args);
            }),
        ScalarNative(
            "nativeDecodeAsset", asset,
            +[](JNIEnv* env, jclass, jlong input, jobject padding, jobject options, jlong bitmap,
                jlong colorSpace) -> jobject {
                const jvalue args[]{jvalue{.j = input}, jvalue{.l = padding}, jvalue{.l = options}, jvalue{.j = bitmap},
                                    jvalue{.j = colorSpace}};
                return DecodeBitmapFactory(env, "decodeAsset", asset, args);
            }),
        ScalarNative(
            "nativeDecodeByteArray", bytes,
            +[](JNIEnv* env, jclass, jbyteArray input, jint offset, jint length, jobject options, jlong bitmap,
                jlong colorSpace) -> jobject {
                const jvalue args[]{jvalue{.l = input},   jvalue{.i = offset}, jvalue{.i = length},
                                    jvalue{.l = options}, jvalue{.j = bitmap}, jvalue{.j = colorSpace}};
                return DecodeBitmapFactory(env, "decodeByteArray", bytes, args);
            }),
    };
    return Bind(env, "android/graphics/BitmapFactory", factory);
}

jobject DecodeSoftwareBitmap(JNIEnv* env, jclass type, jlong pointer, jobject decoder, jboolean postProcess, jint width,
                             jint height, jobject crop, jboolean isMutable, jint allocator, jboolean unpremultiplied,
                             jboolean conserveMemory, jboolean alphaMask, jlong colorSpace, jboolean extended,
                             jlong allocationLimit)
{
    constexpr jint DefaultAllocator = 0, SoftwareAllocator = 1, HardwareAllocator = 3;
    if (allocator == DefaultAllocator || allocator == HardwareAllocator)
        allocator = SoftwareAllocator;
    return decodeImageBitmap(env, type, pointer, decoder, postProcess, width, height, crop, isMutable, allocator,
                             unpremultiplied, conserveMemory, alphaMask, colorSpace, extended, allocationLimit);
}
}

bool IsArtAssetImport(std::string_view name)
{
    return name.starts_with("AAssetManager_") || name.starts_with("AAsset_") || name.starts_with("AAssetDir_") ||
           name.starts_with("AConfiguration_");
}

uintptr_t ArtFrameworkImport(std::string_view name)
{
    if (IsArtAssetImport(name))
        return ArtImageLibraryExport("libandroid.so", name);
    if (name == "AMotionEvent_fromJava") {
        return Import(
            "ART AMotionEvent_fromJava", +[](JNIEnv* env, jobject event) {
                return CopyAndroidMotionEvent(env, event);
            });
    }
    if (name == "ANativeWindow_fromSurface")
        return Import(
            name, +[](JNIEnv* env, jobject surface) -> void* {
                return reinterpret_cast<void*>(ArtSurfaceFromJava(env, surface));
            });
    if (name == "AHardwareBuffer_toHardwareBuffer" || name == "AHardwareBuffer_fromHardwareBuffer" ||
        name == "ANativeWindow_toSurface") {
        // libhwui resolves these while registering even its software Canvas.
        // Do not pass ART's JNIEnv to the legacy OpenJDK/GuestEnv adapters.
        return Import(
            name, +[](JNIEnv* env, void*) -> void* {
                Throw(env, "java/lang/UnsupportedOperationException",
                      "ART hardware buffers and Surface windows are not connected to the Windows host yet");
                return nullptr;
            });
    }
    return 0;
}

bool RegisterArtFramework(JNIEnv* env)
{
    if (!RegisterArtProcesses(env))
        return false;
    const JNINativeMethod properties[]{
        Native(
            "native_get", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
            +[](JNIEnv* env, jclass, jstring key, jstring fallback) -> jstring {
                const auto value = Property(env, key);
                if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
                    return nullptr;
                if (value.empty() && fallback)
                    return fallback;
                return ArtJni<&JNINativeInterface_::NewStringUTF>(env, value.empty() ? "" : value.data());
            }),
        Native(
            "native_get_int", "(Ljava/lang/String;I)I",
            +[](JNIEnv* env, jclass, jstring key, jint fallback) {
                return PropertyNumber(Property(env, key), fallback);
            }),
        Native(
            "native_get_long", "(Ljava/lang/String;J)J",
            +[](JNIEnv* env, jclass, jstring key, jlong fallback) {
                return PropertyNumber(Property(env, key), fallback);
            }),
        Native(
            "native_get_boolean", "(Ljava/lang/String;Z)Z",
            +[](JNIEnv* env, jclass, jstring key, jboolean fallback) -> jboolean {
                const auto value = Property(env, key);
                if (value == "1" || value == "y" || value == "yes" || value == "on" || value == "true")
                    return JNI_TRUE;
                if (value == "0" || value == "n" || value == "no" || value == "off" || value == "false")
                    return JNI_FALSE;
                return fallback;
            }),
        // Host properties are fixed at ART startup. Java keeps the callbacks.
        Native(
            "native_add_change_callback", "()V",
            +[](JNIEnv*, jclass) {
            }),
    };
    if (!Bind(env, "android/os/SystemProperties", properties))
        return false;
    // AOSP Binder performs local transact dispatch in Java. Its native holder
    // has a real lifetime managed by libcore's NativeAllocationRegistry. Remote
    // Binder proxies and transport are not registered by this bootstrap.
    const JNINativeMethod binders[]{
        // The image declares this method @CriticalNative: it receives neither
        // JNIEnv nor jclass. A failed host allocation propagates to the caller.
        Native(
            "getNativeBBinderHolder", "()J",
            +[]() -> jlong {
                return reinterpret_cast<jlong>(new LocalBinderHolder);
            }),
        Native("getNativeFinalizer", "()J", &BinderFinalizer),
        Native(
            "getCallingPid", "()I",
            +[]() -> jint {
                return static_cast<jint>(GetCurrentProcessId());
            }),
        Native(
            "getCallingUid", "()I",
            +[]() -> jint {
                return static_cast<jint>(AndroidApplicationUid());
            }),
        Native(
            "isDirectlyHandlingTransactionNative", "()Z",
            +[]() -> jboolean {
                return JNI_FALSE;
            }),
        Native(
            "clearCallingIdentity", "()J",
            +[]() -> jlong {
                const auto previous = explicitBinderIdentity;
                explicitBinderIdentity = true;
                return previous ? 1 : 0;
            }),
        Native(
            "restoreCallingIdentity", "(J)V",
            +[](jlong token) {
                explicitBinderIdentity = token != 0;
            }),
        Native(
            "hasExplicitIdentity", "()Z",
            +[]() -> jboolean {
                return explicitBinderIdentity ? JNI_TRUE : JNI_FALSE;
            }),
        Native(
            "setThreadStrictModePolicy", "(I)V",
            +[](jint policy) {
                LocalArtBinderState().strictModePolicy = policy;
            }),
        Native(
            "getThreadStrictModePolicy", "()I",
            +[]() -> jint {
                return LocalArtBinderState().strictModePolicy;
            }),
        Native(
            "setCallingWorkSourceUid", "(I)J",
            +[](jint uid) -> jlong {
                auto& state = LocalArtBinderState();
                const jlong token = static_cast<uint32_t>(state.workSource) | (jlong(state.propagateWorkSource) << 32);
                state.workSource = uid;
                state.propagateWorkSource = true;
                return token;
            }),
        Native(
            "getCallingWorkSourceUid", "()I",
            +[]() -> jint {
                return LocalArtBinderState().workSource;
            }),
        Native(
            "clearCallingWorkSource", "()J",
            +[]() -> jlong {
                auto& state = LocalArtBinderState();
                const jlong token = static_cast<uint32_t>(state.workSource) | (jlong(state.propagateWorkSource) << 32);
                state.workSource = -1;
                state.propagateWorkSource = false;
                return token;
            }),
        Native(
            "restoreCallingWorkSource", "(J)V",
            +[](jlong token) {
                auto& state = LocalArtBinderState();
                state.workSource = static_cast<jint>(token);
                state.propagateWorkSource = (token & (jlong(1) << 32)) != 0;
            }),
        Native(
            "flushPendingCommands", "()V",
            +[] {
                // Local Binder dispatch is synchronous in Java; this backend does
                // not accumulate a remote Binder command buffer to flush.
            }),
    };
    if (!Bind(env, "android/os/Binder", binders))
        return false;
    const JNINativeMethod logs[]{
        Native(
            "isLoggable", "(Ljava/lang/String;I)Z",
            +[](JNIEnv* env, jclass, jstring tag, jint level) -> jboolean {
                const StringChars name(env, tag);
                return name.Get() && IsAndroidLogEnabled(level, name.Get(), 4) ? JNI_TRUE : JNI_FALSE;
            }),
        Native(
            "println_native", "(IILjava/lang/String;Ljava/lang/String;)I",
            +[](JNIEnv* env, jclass, jint buffer, jint priority, jstring tag, jstring message) -> jint {
                if (!message) {
                    Throw(env, "java/lang/NullPointerException", "log message is null");
                    return -1;
                }
                if (buffer != 0 && buffer != 1 && buffer != 3 && buffer != 4) {
                    Throw(env, "java/lang/IllegalArgumentException", "unsupported Windows log buffer");
                    return -1;
                }
                const StringChars text(env, message), name(env, tag);
                if (!text.Get() || (tag && !name.Get()))
                    return -1;
                return WriteAndroidLog(priority, name.Get(), text.Get(), buffer);
            }),
        Native(
            "logger_entry_max_payload_native", "()I",
            +[]() -> jint {
                return 4068;
            }),
    };
    if (!Bind(env, "android/util/Log", logs))
        return false;
    const JNINativeMethod clocks[]{
        Native(
            "uptimeMillis", "()J",
            +[]() -> jlong {
                return ClockNanos(1) / 1'000'000;
            }),
        Native(
            "uptimeNanos", "()J",
            +[]() -> jlong {
                return ClockNanos(1);
            }),
        Native(
            "elapsedRealtime", "()J",
            +[]() -> jlong {
                return ClockNanos(7) / 1'000'000;
            }),
        Native(
            "elapsedRealtimeNanos", "()J",
            +[]() -> jlong {
                return ClockNanos(7);
            }),
        Native(
            "currentThreadTimeMillis", "()J",
            +[]() -> jlong {
                FILETIME created{}, exited{}, kernel{}, user{};
                if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
                    std::abort();
                const auto ticks = [](FILETIME value) {
                    return (uint64_t{value.dwHighDateTime} << 32) | value.dwLowDateTime;
                };
                return (ticks(kernel) + ticks(user)) / 10000;
            }),
    };
    if (!Bind(env, "android/os/SystemClock", clocks))
        return false;
    const JNINativeMethod process[]{
        Native(
            "getThreadPriority", "(I)I",
            +[](JNIEnv* env, jclass, jint tid) -> jint {
                const auto query = reinterpret_cast<int (*)(int, unsigned)>(AndroidLibcImport("getpriority", true));
                const int previous = errno;
                errno = 0;
                const int result = query(0, static_cast<unsigned>(tid));
                if (result == -1 && errno) {
                    Throw(env, "java/lang/IllegalArgumentException", "unknown ART host thread priority");
                    return 0;
                }
                errno = previous;
                return result;
            }),
        Native(
            "sendSignal", "(II)V",
            +[](JNIEnv* env, jclass, jint pid, jint signal) {
                if (static_cast<DWORD>(pid) != GetCurrentProcessId()) {
                    Throw(env, "java/lang/SecurityException", "process signals require the current ART application");
                    return;
                }
                if (signal < 0 || signal > 64) {
                    Throw(env, "java/lang/IllegalArgumentException", "invalid Android signal");
                    return;
                }
                if (!signal)
                    return; // The current process is necessarily alive.
                if (signal != 9) {
                    Throw(env, "java/lang/UnsupportedOperationException",
                          "only current-application SIGKILL is supported");
                    return;
                }
                std::fprintf(stderr, "[art-process] current application requested SIGKILL\n");
                if (IsArm64TranslationEnabled())
                    FinishArm64ExecutionSession();
                std::fflush(stderr);
                if (!TerminateProcess(GetCurrentProcess(), 137))
                    Throw(env, "java/lang/IllegalStateException", "could not terminate the current ART application");
            }),
    };
    if (!Bind(env, "android/os/Process", process))
        return false;
    const JNINativeMethod methods[]{
        Native("nativeInit", "()J", &CreateQueue),
        Native(
            "nativeDestroy", "(J)V",
            +[](JNIEnv*, jclass, jlong pointer) {
                delete reinterpret_cast<MessageQueue*>(pointer);
            }),
        Native("nativePollOnce", "(JI)V", &PollQueue),
        Native("nativeWake", "(J)V", &WakeQueue),
        Native(
            "nativeIsPolling", "(J)Z",
            +[](JNIEnv*, jclass, jlong pointer) -> jboolean {
                const auto* queue = reinterpret_cast<MessageQueue*>(pointer);
                return queue && queue->polling.load() ? JNI_TRUE : JNI_FALSE;
            }),
        Native("nativeSetFileDescriptorEvents", "(JII)V", &WatchDescriptor),
        Native(
            "nativeSetSkipEpollWaitForZeroTimeout", "(J)V",
            +[](JNIEnv*, jclass, jlong pointer) {
                if (const auto* queue = reinterpret_cast<MessageQueue*>(pointer))
                    Looper<void (*)(void*)>("stone_looper_set_skip_empty_zero_timeout")(queue->looper);
            }),
    };
    return Bind(env, "android/os/MessageQueue", methods);
}

bool RegisterArtHost(JNIEnv* env)
{
    InitializeClientServices();
    const JNINativeMethod session[]{Native(
                                        "commands", "()I",
                                        +[](JNIEnv* env, jclass) -> jint {
                                            try {
                                                return ReadClientSessionCommands();
                                            }
                                            catch (const std::exception& error) {
                                                Throw(env, "java/lang/IllegalStateException", error.what());
                                                return 0;
                                            }
                                        }),
                                    Native(
                                        "retire", "()Z", +[](JNIEnv* env, jclass) -> jboolean {
                                            try {
                                                return RetireClientSession() ? JNI_TRUE : JNI_FALSE;
                                            }
                                            catch (const std::exception& error) {
                                                Throw(env, "java/lang/IllegalStateException", error.what());
                                                return JNI_FALSE;
                                            }
                                        })};
    if (!Bind(env, "com/trebledot/stone/runtime/ArtSession", session))
        return false;
    const JNINativeMethod pendingIntents[]{
        Native(
            "available", "()Z",
            +[](JNIEnv*, jclass) -> jboolean {
                return ClientNotificationHostEnabled() ? JNI_TRUE : JNI_FALSE;
            }),
        Native(
            "exchange", "(Ljava/lang/String;[Ljava/lang/String;)[Ljava/lang/String;",
            +[](JNIEnv* env, jclass, jstring package, jobjectArray request) -> jobjectArray {
                return NotificationCall(env, [&]() -> jobjectArray {
                    if (!request)
                        throw std::invalid_argument("Missing PendingIntent operation");
                    auto count = ArtJni<&JNINativeInterface_::GetArrayLength>(env, request);
                    if (count < 2 || count > 4)
                        throw std::invalid_argument("Invalid PendingIntent fields");
                    std::vector<std::wstring> fields{
                        std::filesystem::canonical(ResolveAndroidSystemFile(L"data")).wstring(),
                        NotificationText(env, package, 255)};
                    for (jsize index = 0; index < count; ++index) {
                        auto text = static_cast<jstring>(
                            ArtJni<&JNINativeInterface_::GetObjectArrayElement>(env, request, index));
                        fields.push_back(NotificationText(env, text, 24576));
                        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, text);
                    }
                    auto values = ClientNotificationRequest(fields);
                    auto stringClass = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/String");
                    auto result = ArtJni<&JNINativeInterface_::NewObjectArray>(env, static_cast<jsize>(values.size()),
                                                                               stringClass, nullptr);
                    for (jsize index = 0; result && index < static_cast<jsize>(values.size()); ++index) {
                        auto value = ArtJni<&JNINativeInterface_::NewString>(
                            env, reinterpret_cast<const jchar*>(values[index].data()),
                            static_cast<jsize>(values[index].size()));
                        ArtJni<&JNINativeInterface_::SetObjectArrayElement>(env, result, index, value);
                        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, value);
                    }
                    return result;
                });
            })};
    if (!Bind(env, "com/trebledot/stone/runtime/ArtIntents", pendingIntents))
        return false;
    const JNINativeMethod clipboard[]{
        Native(
            "available", "()Z",
            +[](JNIEnv*, jclass) -> jboolean {
                return ClientClipboardEnabled() ? JNI_TRUE : JNI_FALSE;
            }),
        Native(
            "exchange", "(Ljava/lang/String;[Ljava/lang/String;)[Ljava/lang/String;",
            +[](JNIEnv* env, jclass, jstring package, jobjectArray request) -> jobjectArray {
                return NotificationCall(env, [&]() -> jobjectArray {
                    auto count = request ? ArtJni<&JNINativeInterface_::GetArrayLength>(env, request) : 0;
                    if (count < 1 || count > 4)
                        throw std::invalid_argument("Invalid clipboard fields");
                    std::vector<std::wstring> fields{
                        std::filesystem::canonical(ResolveAndroidSystemFile(L"data")).wstring(),
                        NotificationText(env, package, 255)};
                    for (jsize index = 0; index < count; ++index) {
                        auto text = static_cast<jstring>(
                            ArtJni<&JNINativeInterface_::GetObjectArrayElement>(env, request, index));
                        fields.push_back(NotificationText(env, text, 8192));
                        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, text);
                    }
                    auto values = ClientClipboardRequest(fields);
                    auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/String");
                    auto result = ArtJni<&JNINativeInterface_::NewObjectArray>(env, static_cast<jsize>(values.size()),
                                                                               type, nullptr);
                    for (jsize index = 0; result && index < static_cast<jsize>(values.size()); ++index) {
                        auto value = ArtJni<&JNINativeInterface_::NewString>(
                            env, reinterpret_cast<const jchar*>(values[index].data()),
                            static_cast<jsize>(values[index].size()));
                        ArtJni<&JNINativeInterface_::SetObjectArrayElement>(env, result, index, value);
                        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, value);
                    }
                    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
                    return result;
                });
            })};
    if (!Bind(env, "com/trebledot/stone/runtime/ArtClipboard", clipboard))
        return false;
    const JNINativeMethod jobs[]{
        Native(
            "available", "()Z",
            +[](JNIEnv*, jclass) -> jboolean {
                return ClientJobsEnabled() ? JNI_TRUE : JNI_FALSE;
            }),
        Native(
            "removePackage", "(Ljava/lang/String;)V",
            +[](JNIEnv* env, jclass, jstring name) {
                try {
                    RemoveClientPackageJobs(ResolveAndroidSystemFile(L"data"), NotificationText(env, name, 255));
                }
                catch (const std::exception& error) {
                    Throw(env, "java/io/IOException", error.what());
                }
            }),
        Native(
            "exchange", "([Ljava/lang/String;)[Ljava/lang/String;",
            +[](JNIEnv* env, jclass, jobjectArray request) -> jobjectArray {
                try {
                    if (!request)
                        throw std::invalid_argument("Missing job operation");
                    auto count = ArtJni<&JNINativeInterface_::GetArrayLength>(env, request);
                    if (count < 2 || count > 16)
                        throw std::invalid_argument("Invalid job operation fields");
                    std::vector<std::wstring> fields{
                        std::filesystem::canonical(ResolveAndroidSystemFile(L"data")).wstring(),
                        std::filesystem::canonical(ResolveAndroidSystemFile(L"system")).wstring()};
                    for (jsize index = 0; index < count; ++index) {
                        auto text = static_cast<jstring>(
                            ArtJni<&JNINativeInterface_::GetObjectArrayElement>(env, request, index));
                        fields.push_back(NotificationText(env, text, 24576));
                        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, text);
                    }
                    auto values = ClientJobRequest(fields);
                    auto stringClass = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/String");
                    auto result = ArtJni<&JNINativeInterface_::NewObjectArray>(env, static_cast<jsize>(values.size()),
                                                                               stringClass, nullptr);
                    for (jsize index = 0; result && index < static_cast<jsize>(values.size()); ++index) {
                        auto value = ArtJni<&JNINativeInterface_::NewString>(
                            env, reinterpret_cast<const jchar*>(values[index].data()),
                            static_cast<jsize>(values[index].size()));
                        ArtJni<&JNINativeInterface_::SetObjectArrayElement>(env, result, index, value);
                        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, value);
                    }
                    return result;
                }
                catch (const std::exception& error) {
                    Throw(env, "java/lang/IllegalStateException", error.what());
                    return nullptr;
                }
            })};
    if (!Bind(env, "com/trebledot/stone/runtime/ArtJobs", jobs))
        return false;
    const JNINativeMethod power[]{Native(
                                      "saver", "()Z",
                                      +[](JNIEnv*, jclass) -> jboolean {
                                          return ReadClientHostState().battery[3] == 1 ? JNI_TRUE : JNI_FALSE;
                                      }),
                                  Native(
                                      "setRequired", "(Z)V", +[](JNIEnv* env, jclass, jboolean required) {
                                          try {
                                              SetClientSystemAwake(required == JNI_TRUE);
                                          }
                                          catch (const std::exception& error) {
                                              Throw(env, "java/lang/IllegalStateException", error.what());
                                          }
                                      })};
    if (!Bind(env, "com/trebledot/stone/runtime/ArtPower", power))
        return false;
    const JNINativeMethod shell[]{
        Native(
            "enabled", "()Z",
            +[](JNIEnv*, jclass) -> jboolean {
                return ClientShellEnabled() ? JNI_TRUE : JNI_FALSE;
            }),
        Native(
            "update", "(Ljava/lang/String;Ljava/lang/String;Z)V",
            +[](JNIEnv* env, jclass, jstring name, jstring label, jboolean installed) {
                NotificationCall(env, [&] {
                    UpdateClientShortcut(NotificationText(env, name, 255), NotificationText(env, label, 1024),
                                         installed == JNI_TRUE);
                });
            }),
        Native(
            "icon", "(Ljava/lang/String;Ljava/lang/String;[B)[Ljava/lang/String;",
            +[](JNIEnv* env, jclass, jstring name, jstring label, jbyteArray png) -> jobjectArray {
                jobjectArray result = nullptr;
                NotificationCall(env, [&] {
                    const auto size = png ? ArtJni<&JNINativeInterface_::GetArrayLength>(env, png) : 0;
                    if (size <= 0 || size > 1024 * 1024)
                        throw std::invalid_argument("Invalid application icon size");
                    std::vector<uint8_t> bytes(size);
                    ArtJni<&JNINativeInterface_::GetByteArrayRegion>(env, png, 0, size,
                                                                     reinterpret_cast<jbyte*>(bytes.data()));
                    if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
                        return;
                    auto paths = UpdateClientApplicationIcon(NotificationText(env, name, 255),
                                                             NotificationText(env, label, 1024), bytes);
                    auto strings = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/String");
                    result = strings ? ArtJni<&JNINativeInterface_::NewObjectArray>(env, 2, strings, nullptr) : nullptr;
                    for (jsize index = 0; result && index < 2; ++index) {
                        const auto& path = paths[index];
                        auto value = ArtJni<&JNINativeInterface_::NewString>(
                            env, reinterpret_cast<const jchar*>(path.data()), static_cast<jsize>(path.size()));
                        ArtJni<&JNINativeInterface_::SetObjectArrayElement>(env, result, index, value);
                        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, value);
                    }
                    if (strings)
                        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, strings);
                });
                return result;
            })};
    if (!Bind(env, "com/trebledot/stone/runtime/ArtShell", shell))
        return false;
    const JNINativeMethod configuration[]{
        Native(
            "readHostState", "()Ljava/lang/String;",
            +[](JNIEnv* env, jclass) -> jstring {
                const auto host = ReadHostUiConfiguration();
                const auto value = host.languageTags + (host.dark ? L"|dark" : L"|light");
                return ArtJni<&JNINativeInterface_::NewString>(env, reinterpret_cast<const jchar*>(value.data()),
                                                               static_cast<jsize>(value.size()));
            }),
        Native(
            "readDisplayDensity", "()I", +[](JNIEnv*, jclass) -> jint {
                return AndroidDisplayDensity();
            })};
    if (!Bind(env, "com/trebledot/stone/runtime/ArtConfiguration", configuration))
        return false;
    const JNINativeMethod connectivity[]{Native("queryHostNetworkState", "()I", &HostNetworkState)};
    if (!Bind(env, "com/trebledot/stone/runtime/ArtConnectivity", connectivity))
        return false;
    const JNINativeMethod wifi[]{Native(
        "queryHostState", "()[J", +[](JNIEnv* env, jclass) -> jlongArray {
            const auto state = ReadClientHostState().wifi;
            const jlong values[]{state.state, state.connected, state.interfaceIndex,
                                 state.ipv4,  state.netmask,   state.gateway,
                                 state.dns1,  state.dns2,      static_cast<jlong>(state.samples)};
            const auto result = ArtJni<&JNINativeInterface_::NewLongArray>(env, 9);
            if (result)
                ArtJni<&JNINativeInterface_::SetLongArrayRegion>(env, result, 0, 9, values);
            return result;
        })};
    if (!Bind(env, "com/trebledot/stone/runtime/ArtWifi", wifi))
        return false;
    const JNINativeMethod battery[]{Native(
        "queryHostState", "()[I", +[](JNIEnv* env, jclass) -> jintArray {
            const auto snapshot = ReadClientHostState();
            const jint values[]{snapshot.battery[0], snapshot.battery[1], snapshot.battery[2], snapshot.battery[3]};
            const auto result = ArtJni<&JNINativeInterface_::NewIntArray>(env, 4);
            if (result)
                ArtJni<&JNINativeInterface_::SetIntArrayRegion>(env, result, 0, 4, values);
            return result;
        })};
    if (!Bind(env, "com/trebledot/stone/runtime/ArtBattery", battery))
        return false;
    const JNINativeMethod process[]{
        Native(
            "getProcessGroup", "(I)I",
            +[](JNIEnv* env, jclass, jint pid) -> jint {
                if (pid != 0 && static_cast<DWORD>(pid) != GetCurrentProcessId() &&
                    static_cast<DWORD>(pid) != GetCurrentThreadId()) {
                    const auto thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
                    const bool owned = thread && GetProcessIdOfThread(thread) == GetCurrentProcessId();
                    if (thread)
                        CloseHandle(thread);
                    if (!owned) {
                        Throw(env, "java/lang/IllegalArgumentException", "unknown ART host task");
                        return -1;
                    }
                }
                return 1; // THREAD_GROUP_FOREGROUND: this interactive host has no Android cgroups.
            }),
    };
    if (!Bind(env, "android/os/Process", process))
        return false;
    if (!RegisterImageNative(env, "libandroid_runtime.so", "register_android_hardware_HardwareBuffer", false))
        return false;
    for (const auto* name : {"register_android_view_Surface", "register_android_view_SurfaceControl"})
        if (!RegisterImageNative(env, "libandroid_runtime.so", name))
            return false;
    for (const auto* name :
         {"register_android_view_RenderNode", "register_android_view_DisplayListCanvas",
          "register_android_view_ThreadedRenderer", "register_android_graphics_CanvasProperty",
          "register_android_graphics_text_MeasuredText", "register_android_graphics_text_TextShaper",
          "register_android_graphics_text_LineBreaker", "register_android_graphics_animation_RenderNodeAnimator",
          "register_android_graphics_animation_NativeInterpolatorFactory", "register_android_util_PathParser"})
        if (!RegisterImageNative(env, "libhwui.so", name))
            return false;
    const JNINativeMethod renderer[]{
        // ViewRoot can warm the Android allocator even when Stone supplies a software window.
        // That allocator requires a gralloc HAL, which is absent on the Windows host.
        Native(
            "preInitBufferAllocator", "()V",
            +[](JNIEnv*, jclass) {
            }),
    };
    if (!Bind(env, "android/graphics/HardwareRenderer", renderer))
        return false;
    for (const auto* name : {"register_android_graphics_RenderEffect", "register_android_graphics_BitmapFactory",
                             "register_android_graphics_BitmapRegionDecoder", "register_android_graphics_NinePatch",
                             "register_android_graphics_ImageDecoder", "register_android_graphics_Interpolator",
                             "register_android_graphics_ByteBufferStreamAdaptor",
                             "register_android_graphics_CreateJavaOutputStreamAdaptor"})
        if (!RegisterImageNative(env, "libhwui.so", name, false))
            return false;
    // Keep the image's decoders and metadata. Software windows cannot display
    // hardware bitmaps or use the system image's gralloc HAL.
    constexpr auto imageDecodeSymbol = "_ZL26ImageDecoder_nDecodeBitmapP7_JNIEnvP8_jobjectlS2_hiiS2_hihhhlhl";
    const auto decodeAddress = AndroidRuntimeFunction("libhwui.so", imageDecodeSymbol, true);
    decodeImageBitmap = reinterpret_cast<ImageDecoder>(
        MakeNativeAndroidThunk(decodeAddress, detail::ArtJniSignature<ImageDecoder>::Value()));
    const JNINativeMethod imageDecoder[]{
        {const_cast<char*>("nDecodeBitmap"),
         const_cast<char*>(
             "(JLandroid/graphics/ImageDecoder;ZIILandroid/graphics/Rect;ZIZZZJZJ)Landroid/graphics/Bitmap;"),
         reinterpret_cast<void*>(ImportAndroidScalars<ImageDecoder>(
             "ART software bitmap decoder", reinterpret_cast<uintptr_t>(&DecodeSoftwareBitmap)))},
    };
    if (!Bind(env, "android/graphics/ImageDecoder", imageDecoder))
        return false;
    if (!BindSoftwareBitmapFactory(env))
        return false;
    if (!RegisterImageNative(env, "libhwui.so", "register_android_graphics_drawable_VectorDrawable"))
        return false;
    if (!RegisterImageNative(env, "libhwui.so", "register_android_graphics_drawable_AnimatedVectorDrawable"))
        return false;
    for (const auto* name :
         {"register_android_hardware_OverlayProperties", "register_android_hardware_display_DisplayManagerGlobal"}) {
        if (!RegisterImageNative(env, "libandroid_runtime.so", name, false))
            return false;
    }
    for (const auto* name : {"register_com_android_internal_os_ApplicationSharedMemory",
                             "register_android_app_PropertyInvalidatedCache",
                             "register_android_app_ActivityThread",
                             "register_android_util_EventLog",
                             "register_android_os_Parcel",
                             "register_android_os_SharedMemory",
                             "register_com_android_internal_util_ArrayUtils",
                             "register_com_android_internal_util_VirtualRefBasePtr",
                             "register_android_animation_PropertyValuesHolder",
                             "register_android_database_CursorWindow",
                             "register_android_database_SQLiteConnection",
                             "register_android_database_SQLiteGlobal",
                             "register_android_database_SQLiteDebug",
                             "register_android_database_SQLiteRawStatement",
                             "register_android_os_incremental_IncrementalManager",
                             "register_android_util_jar_StrictJarFile",
                             "register_com_android_internal_os_DebugStore",
                             "register_android_tracing_PerfettoProducer",
                             "register_android_tracing_PerfettoDataSourceInstance",
                             "register_android_tracing_PerfettoDataSource",
                             "register_android_view_InputChannel",
                             "register_android_view_KeyCharacterMap",
                             "register_android_view_KeyEvent",
                             "register_android_view_MotionEvent",
                             "register_android_view_VelocityTracker"})
        if (!RegisterImageNative(env, "libandroid_runtime.so", name))
            return false;
    // The software window dispatches Win32 input directly. The image receiver
    // assumes a libandroid_runtime MessageQueue, whose layout differs from ours.
    if (!RegisterArtFileObserver(env))
        return false;
    const JNINativeMethod inputReceiver[]{
        Native(
            "nativeInit", "(Ljava/lang/ref/WeakReference;Landroid/view/InputChannel;Landroid/os/MessageQueue;)J",
            +[](JNIEnv*, jclass, jobject, jobject, jobject) -> jlong {
                return 1;
            }),
        Native(
            "nativeDispose", "(J)V",
            +[](JNIEnv*, jclass, jlong) {
            }),
        Native(
            "nativeConsumeBatchedInputEvents", "(JJ)Z",
            +[](JNIEnv*, jclass, jlong, jlong) -> jboolean {
                return JNI_FALSE;
            }),
        Native(
            "nativeProbablyHasInput", "(J)Z",
            +[](JNIEnv*, jclass, jlong) -> jboolean {
                return JNI_FALSE;
            }),
        Native(
            "nativeFinishInputEvent", "(JIZ)V",
            +[](JNIEnv*, jclass, jlong, jint, jboolean) {
            }),
        Native(
            "nativeGetFrameMetricsObserver", "(J)J",
            +[](JNIEnv*, jclass, jlong) -> jlong {
                return 0;
            }),
        Native(
            "nativeGetToken", "(J)Landroid/os/IBinder;",
            +[](JNIEnv*, jclass, jlong) -> jobject {
                return nullptr;
            }),
        Native(
            "nativeDump", "(JLjava/lang/String;)Ljava/lang/String;",
            +[](JNIEnv*, jclass, jlong, jstring prefix) -> jstring {
                return prefix;
            }),
    };
    if (!Bind(env, "android/view/InputEventReceiver", inputReceiver))
        return false;
    const JNINativeMethod display[]{
        Native(
            "readMetrics", "()[I",
            +[](JNIEnv* env, jclass) -> jintArray {
                const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
                struct RestoreDpi {
                    DPI_AWARENESS_CONTEXT previous;
                    ~RestoreDpi()
                    {
                        if (previous)
                            SetThreadDpiAwarenessContext(previous);
                    }
                } restore{previous};
                const auto density = AndroidDisplayDensity();
                const auto dpi = MulDiv(density, USER_DEFAULT_SCREEN_DPI, 160);
                BOOL animations = TRUE;
                SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
                const jint values[]{GetSystemMetrics(SM_CXSCREEN),
                                    GetSystemMetrics(SM_CYSCREEN),
                                    density,
                                    static_cast<jint>(dpi),
                                    60, // The software screen and its EGL producer refresh at 60 Hz.
                                    animations ? 1 : 0};
                const auto array = ArtJni<&JNINativeInterface_::NewIntArray>(env, 6);
                if (array)
                    ArtJni<&JNINativeInterface_::SetIntArrayRegion>(env, array, 0, 6, values);
                return array;
            }),
    };
    if (!Bind(env, "com/trebledot/stone/runtime/ArtDisplay", display))
        return false;
    const JNINativeMethod keyboard[]{
        Native(
            "nativeReloadMap", "(Landroid/view/KeyCharacterMap;)I",
            +[](JNIEnv* env, jclass, jobject map) -> jint {
                const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "android/view/KeyCharacterMap");
                const auto field = type ? ArtJni<&JNINativeInterface_::GetFieldID>(env, type, "mPtr", "J") : nullptr;
                const auto pointer = map && field ? ArtJni<&JNINativeInterface_::GetLongField>(env, map, field) : 0;
                if (type)
                    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
                if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
                    return -EINVAL;
                // Image NativeKeyCharacterMap owns {int32_t deviceId; unique_ptr<KeyCharacterMap>}.
                // Keep allocation, parsing, destruction and all character operations in the image.
                struct NativeMap {
                    int32_t deviceId;
                    int32_t padding;
                    void* value;
                };
                static_assert(sizeof(NativeMap) == 16 && offsetof(NativeMap, value) == 8);
                if (!pointer || !reinterpret_cast<const NativeMap*>(pointer)->value)
                    return -EINVAL;
                const auto reload =
                    AndroidRuntimeFunction("libinput.so", "_ZN7android15KeyCharacterMap18reloadBaseFromFileEv");
                return reinterpret_cast<int (*)(void*)>(MakeNativeAndroidThunk(reload, "(L)I"))(
                    reinterpret_cast<const NativeMap*>(pointer)->value);
            }),
    };
    if (!Bind(env, "com/trebledot/stone/runtime/ArtInput", keyboard))
        return false;
    const JNINativeMethod window[]{
        Native(
            "nativeSetOrientation", "(Z)Z",
            +[](JNIEnv*, jclass, jboolean wide) -> jboolean {
                return SetWindowsArtOrientation(wide != JNI_FALSE) ? JNI_TRUE : JNI_FALSE;
            }),
        Native(
            "nativeSetResizable", "(Z)Z",
            +[](JNIEnv*, jclass, jboolean enabled) -> jboolean {
                return SetWindowsArtResizable(enabled != JNI_FALSE) ? JNI_TRUE : JNI_FALSE;
            }),
        Native(
            "nativeClientSize", "()J",
            +[](JNIEnv*, jclass) -> jlong {
                return static_cast<jlong>(WindowsArtClientSize());
            }),
        Native(
            "nativeOpen", "()V",
            +[](JNIEnv* env, jclass) {
                if (!OpenArtWindow())
                    Throw(env, "java/lang/IllegalStateException", "could not create ART Win32 window");
            }),
        Native(
            "nativeMinimize", "()V",
            +[](JNIEnv*, jclass) {
                if (const auto hwnd = WindowsHostWindow())
                    ShowWindowAsync(hwnd, SW_MINIMIZE);
            }),
        Native(
            "nativeSetVisible", "(Z)V",
            +[](JNIEnv*, jclass, jboolean visible) {
                if (const auto hwnd = WindowsHostWindow())
                    ShowWindowAsync(hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
            }),
        Native(
            "nativeSetIcon", "([B)Z",
            +[](JNIEnv* env, jclass, jbyteArray encoded) -> jboolean {
                if (!encoded)
                    return JNI_FALSE;
                const auto size = ArtJni<&JNINativeInterface_::GetArrayLength>(env, encoded);
                if (size <= 0 || size > 1024 * 1024)
                    return JNI_FALSE;
                auto* bytes = ArtJni<&JNINativeInterface_::GetByteArrayElements>(env, encoded, nullptr);
                if (!bytes)
                    return JNI_FALSE;
                const bool applied = SetWindowsHostIcon({bytes, static_cast<size_t>(size)});
                ArtJni<&JNINativeInterface_::ReleaseByteArrayElements>(env, encoded, bytes, JNI_ABORT);
                return applied ? JNI_TRUE : JNI_FALSE;
            }),
        Native(
            "nativeSetAppearance", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
            +[](JNIEnv* env, jclass, jstring label, jstring icon, jstring command) {
                NotificationCall(env, [&] {
                    if (!SetWindowsHostAppearance(NotificationText(env, label, 1024),
                                                  NotificationText(env, icon, 32767),
                                                  NotificationText(env, command, 32767)))
                        throw std::runtime_error("Could not update Windows application appearance");
                });
            }),
        Native(
            "nativeSetAppUserModelId", "(Ljava/lang/String;)V",
            +[](JNIEnv* env, jclass, jstring id) {
                const StringChars text(env, id);
                if (!text.Get())
                    return;
                const std::string_view ascii(text.Get());
                try {
                    SetWindowsHostAppUserModelId(std::wstring(ascii.begin(), ascii.end()).c_str());
                }
                catch (const std::bad_alloc&) {
                    Throw(env, "java/lang/OutOfMemoryError", "could not allocate a Windows taskbar identity");
                }
            }),
        Native(
            "nativeIsOpen", "()Z",
            +[](JNIEnv*, jclass) -> jboolean {
                return WindowsHostWindow() ? JNI_TRUE : JNI_FALSE;
            }),
        Native(
            "nativeCaptureProtection", "()I",
            +[](JNIEnv*, jclass) -> jint {
                return WindowsArtCaptureProtection();
            }),
        Native(
            "nativePresent", "([IIIZ)Z",
            +[](JNIEnv* env, jclass, jintArray pixels, jint width, jint height, jboolean secure) -> jboolean {
                if (!pixels || width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
                    static_cast<uint64_t>(width) * height > 64 * 1024 * 1024 ||
                    ArtJni<&JNINativeInterface_::GetArrayLength>(env, pixels) != static_cast<int64_t>(width) * height) {
                    Throw(env, "java/lang/IllegalArgumentException", "invalid ART window frame");
                    return JNI_FALSE;
                }
                try {
                    framePixels.resize(static_cast<size_t>(width) * height);
                    ArtJni<&JNINativeInterface_::GetIntArrayRegion>(
                        env, pixels, 0, static_cast<jint>(framePixels.size()), framePixels.data());
                    if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
                        return JNI_FALSE;
                    if (!OpenArtWindow())
                        return JNI_FALSE;
                    return PresentWindowsArtFrame(framePixels, width, height, secure != JNI_FALSE) ? JNI_TRUE
                                                                                                   : JNI_FALSE;
                }
                catch (const std::bad_alloc&) {
                    Throw(env, "java/lang/OutOfMemoryError", "could not allocate an ART window frame");
                    return JNI_FALSE;
                }
            }),
        Native(
            "nativePollInput", "()[I",
            +[](JNIEnv* env, jclass) -> jintArray {
                thread_local std::vector<jint> events;
                bool allocationFailed, queueOverflow;
                {
                    const std::lock_guard lock(windowInputMutex);
                    events.swap(windowInput);
                    allocationFailed = std::exchange(windowInputAllocationFailed, false);
                    queueOverflow = std::exchange(windowInputQueueOverflow, false);
                }
                if (allocationFailed || queueOverflow) {
                    events.clear();
                    Throw(env, allocationFailed ? "java/lang/OutOfMemoryError" : "java/lang/IllegalStateException",
                          allocationFailed ? "could not queue ART window input"
                                           : "ART window input queue capacity exceeded");
                    return nullptr;
                }
                const auto size = static_cast<jsize>(events.size());
                const auto result = ArtJni<&JNINativeInterface_::NewIntArray>(env, size);
                if (result && size)
                    ArtJni<&JNINativeInterface_::SetIntArrayRegion>(env, result, 0, size, events.data());
                events.clear();
                return result;
            }),
        Native(
            "nativeClose", "()Z",
            +[](JNIEnv*, jclass) -> jboolean {
                if (!CloseWindowsArtWindow())
                    return JNI_FALSE;
                const std::lock_guard lock(windowInputMutex);
                windowInput.clear();
                windowInputAllocationFailed = windowInputQueueOverflow = false;
                return JNI_TRUE;
            }),
    };
    if (!Bind(env, "com/trebledot/stone/runtime/ArtWindow", window))
        return false;
    const JNINativeMethod application[]{
        Native(
            "prepareAot", "(Ljava/lang/String;)V",
            +[](JNIEnv* env, jclass, jstring source) {
                try {
                    const auto path = std::filesystem::path(NotificationText(env, source, 32767)).generic_u8string();
                    PrepareArtAot({reinterpret_cast<const char*>(path.data()), path.size()});
                }
                catch (const std::exception& error) {
                    Throw(env, "java/io/IOException", error.what());
                }
                catch (const AndroidSystemPathError&) {
                    Throw(env, "java/io/IOException", "Could not resolve APK for AOT");
                }
            }),
        Native(
            "memoryInfo", "()[J",
            +[](JNIEnv* env, jclass) -> jlongArray {
                const auto snapshot = ReadClientHostState();
                const jlong values[]{static_cast<jlong>(snapshot.memory[0]), static_cast<jlong>(snapshot.memory[1]),
                                     static_cast<jlong>(snapshot.memory[2]), static_cast<jlong>(snapshot.memory[3])};
                const auto result = ArtJni<&JNINativeInterface_::NewLongArray>(env, 4);
                if (result)
                    ArtJni<&JNINativeInterface_::SetLongArrayRegion>(env, result, 0, 4, values);
                return result;
            }),
        Native(
            "glEsVersion", "()I",
            +[](JNIEnv*, jclass) -> jint {
                return ArtGlEsVersion();
            }),
        Native(
            "bindNativeLibraries", "(Ljava/lang/ClassLoader;Ljava/lang/String;Ljava/lang/String;)V",
            +[](JNIEnv* env, jclass, jobject loader, jstring directory, jstring data) {
                try {
                    BindArtNativeLibraries(env, loader, NotificationText(env, directory, 32767),
                                           NotificationText(env, data, 32767));
                }
                catch (const std::exception& error) {
                    Throw(env, "java/io/IOException", error.what());
                }
            }),
        Native(
            "registerLaunchProfile", "(Ljava/lang/String;)V",
            +[](JNIEnv* env, jclass, jstring name) {
                try {
                    RegisterClientLaunchProfile(NotificationText(env, name, 255));
                }
                catch (const std::exception& error) {
                    Throw(env, "java/io/IOException", error.what());
                }
            }),
        Native(
            "bindApplicationUid", "(I)V",
            +[](JNIEnv* env, jclass, jint uid) {
                if (applicationLock || uid < 10000 || uid > 19999) {
                    Throw(env, "java/lang/IllegalStateException", "invalid or late installed application identity");
                    return;
                }
                SetAndroidApplicationUid(static_cast<uint32_t>(uid));
            }),
        Native(
            "lockApplication", "(Ljava/lang/String;)V",
            +[](JNIEnv* env, jclass, jstring packageName) {
                const StringChars text(env, packageName);
                if (!text.Get()) {
                    Throw(env, "java/lang/IllegalArgumentException", "application package name is missing");
                    return;
                }
                const std::string_view name(text.Get());
                if (name.empty() ||
                    name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.") !=
                        name.npos ||
                    name.front() == '.' || name.back() == '.' || name.find("..") != name.npos) {
                    Throw(env, "java/lang/IllegalArgumentException", "invalid application package name");
                    return;
                }
                if (applicationLock) {
                    Throw(env, "java/lang/IllegalStateException", "an ART application is already locked");
                    return;
                }
                try {
                    std::wstring wideName;
                    for (const auto ch : name)
                        wideName.push_back(static_cast<wchar_t>(ch));
                    ValidateClientLaunchPackage(wideName);
                    const auto directory = ResolveAndroidSystemFile(L"data/system/stone-packages/app-locks");
                    std::filesystem::create_directories(directory);
                    const auto path = directory / (wideName + L".lock");
                    // All processes of one UID share the package execution lease.
                    // Package replacement/removal still needs a conflicting writer.
                    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                                  FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (file == INVALID_HANDLE_VALUE) {
                        const auto error = GetLastError();
                        Throw(env,
                              error == ERROR_SHARING_VIOLATION ? "java/lang/IllegalStateException"
                                                               : "java/io/IOException",
                              error == ERROR_SHARING_VIOLATION ? "APK already running"
                                                               : "could not lock application data");
                    }
                    else {
                        std::unique_ptr<void, decltype(&CloseHandle)> pending(file, CloseHandle);
                        const auto process = ClientApplicationProcess();
                        const auto processPath =
                            directory /
                            (wideName + (process.empty() ? L"" : L"!" + process.substr(wideName.size() + 1)) +
                             L".process.lock");
                        const auto processFile = CreateFileW(processPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (processFile == INVALID_HANDLE_VALUE) {
                            Throw(env, "java/lang/IllegalStateException", "APK process already running");
                            return;
                        }
                        std::unique_ptr<void, decltype(&CloseHandle)> processPending(processFile, CloseHandle);
                        // The removal transaction uses this same execution lock.
                        // Authorize a queued wake before recreating any app data.
                        if (ClientWakeRequested())
                            RegisterClientLaunchProfile(wideName);
                        const auto legacy = ResolveAndroidSystemFile(L"data/data/" + wideName + L"/.stone-app.lock");
                        std::filesystem::create_directories(legacy.parent_path());
                        {
                            const auto older = CreateFileW(legacy.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (older == INVALID_HANDLE_VALUE) {
                                Throw(env, "java/lang/IllegalStateException",
                                      "APK already running or legacy application lock unavailable");
                                return;
                            }
                            legacyApplicationLock = older;
                        }
                        applicationLock = pending.release();
                        processApplicationLock = processPending.release();
                        SetArtRunningPackage(wideName);
                    }
                }
                catch (...) {
                    Throw(env, "java/io/IOException", "could not resolve the application lock");
                }
            }),
    };
    if (!Bind(env, "com/trebledot/stone/runtime/ArtBoot", application))
        return false;
    const JNINativeMethod storage[]{
        Native(
            "dataIdentityNative", "(Ljava/lang/String;)Ljava/lang/String;",
            +[](JNIEnv* env, jclass, jstring directory) -> jstring {
                try {
                    const auto guest = AndroidSystemGuestPath(NotificationText(env, directory, 32767));
                    if (!guest || !guest->starts_with(L"data/data/") || guest->size() <= 10 ||
                        guest->find(L'/', 10) != guest->npos)
                        throw std::runtime_error("Invalid application data directory");
                    const auto path = ResolveAndroidSystemFile(*guest);
                    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                    if (file == INVALID_HANDLE_VALUE)
                        throw std::runtime_error("Cannot open application data identity");
                    BY_HANDLE_FILE_INFORMATION information{};
                    const bool read = GetFileInformationByHandle(file, &information) != FALSE;
                    CloseHandle(file);
                    const auto index = (uint64_t(information.nFileIndexHigh) << 32) | information.nFileIndexLow;
                    if (!read || !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !index)
                        throw std::runtime_error("Cannot read application data identity");
                    const auto identity =
                        std::to_string(information.dwVolumeSerialNumber) + ":" + std::to_string(index);
                    return ArtJni<&JNINativeInterface_::NewStringUTF>(env, identity.c_str());
                }
                catch (const AndroidSystemPathError&) {
                    Throw(env, "java/io/IOException", "Cannot resolve application data identity");
                }
                catch (const std::exception& error) {
                    Throw(env, "java/io/IOException", error.what());
                }
                return nullptr;
            }),
        Native(
            "configureNative", "(Ljava/lang/String;Z)V",
            +[](JNIEnv* env, jclass, jstring packageName, jboolean allowed) {
                try {
                    ConfigureAndroidSharedStorage(NotificationText(env, packageName, 255), allowed != JNI_FALSE);
                }
                catch (const std::exception& error) {
                    Throw(env, "java/io/IOException", error.what());
                }
            }),
    };
    if (!Bind(env, "com/trebledot/stone/runtime/ArtStorage", storage))
        return false;
    if (!RegisterArtLocalParcels(env))
        return false;
    if (!RegisterArtSurfaces(env))
        return false;
    if (!IsArm64TranslationEnabled()) {
        for (const auto* name :
             {"register_com_google_android_gles_jni_EGLImpl", "register_com_google_android_gles_jni_GLImpl",
              "register_android_opengl_jni_EGL14", "register_android_opengl_jni_EGLExt",
              "register_android_opengl_jni_GLES20", "register_android_opengl_jni_GLES30"})
            if (!RegisterImageNative(env, "libandroid_runtime.so", name, false))
                return false;
        if (!RegisterArtGl(env))
            return false;
    }
    if (!RegisterImageNative(env, "libandroid_runtime.so", "register_android_media_AudioSystem", false))
        return false;
    if (!RegisterImageNative(env, "libandroid_runtime.so", "register_android_media_AudioAttributes", false))
        return false;
    // There is no Android audio policy server. Keep policy discovery explicitly
    // unsupported and use the legacy attribute mapping for the Windows route.
    const JNINativeMethod audioStrategies[]{
        Native(
            "native_list_audio_product_strategies", "(Ljava/util/ArrayList;)I",
            +[](JNIEnv*, jclass, jobject) -> jint {
                return -38;
            }),
        Native(
            "native_get_audio_attributes_for_legacy_stream", "(ILandroid/media/AudioAttributes$Builder;)I",
            +[](JNIEnv*, jclass, jint, jobject) -> jint {
                return -38;
            }),
        Native(
            "native_get_legacy_stream_for_audio_attributes", "(Landroid/media/AudioAttributes;)I",
            +[](JNIEnv* env, jclass, jobject attributes) -> jint {
                const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/runtime/ArtAudio");
                const auto method = type ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(
                                               env, type, "legacyStream", "(Landroid/media/AudioAttributes;)I")
                                         : nullptr;
                const jvalue args[]{jvalue{.l = attributes}};
                const auto result =
                    method ? ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, type, method, args) : -1;
                if (type)
                    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
                return result;
            }),
    };
    if (!Bind(env, "android/media/audiopolicy/AudioProductStrategy", audioStrategies))
        return false;
    // A logical Windows output is registered below on native x64. No Android
    // audio server or image-owned audio port event thread is involved.
    const JNINativeMethod audioEvents[]{
        Native(
            "native_setup", "(Ljava/lang/Object;)V",
            +[](JNIEnv*, jobject, jobject) {
            }),
        Native(
            "native_finalize", "()V",
            +[](JNIEnv*, jobject) {
            }),
    };
    if (!Bind(env, "android/media/AudioPortEventHandler", audioEvents))
        return false;
    const auto emptyAudioPorts = +[](JNIEnv* env, jclass, jobject list, jintArray generation) -> jint {
        if (!list || !generation || ArtJni<&JNINativeInterface_::GetArrayLength>(env, generation) < 1)
            return -22;
        const auto type = ArtJni<&JNINativeInterface_::GetObjectClass>(env, list);
        const auto clear = type ? ArtJni<&JNINativeInterface_::GetMethodID>(env, type, "clear", "()V") : nullptr;
        if (clear)
            ArtJni<&JNINativeInterface_::CallVoidMethodA>(env, list, clear, nullptr);
        if (type)
            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
        const jint value = 1;
        ArtJni<&JNINativeInterface_::SetIntArrayRegion>(env, generation, 0, 1, &value);
        return ArtJni<&JNINativeInterface_::ExceptionCheck>(env) ? -1 : 0;
    };
    const JNINativeMethod audioSystem[]{
        Native(
            "newAudioSessionId", "()I",
            +[](JNIEnv*, jclass) -> jint {
                return IsArm64TranslationEnabled() ? -1 : NewWindowsAudioSession();
            }),
        Native(
            "getPrimaryOutputSamplingRate", "()I",
            +[](JNIEnv*, jclass) -> jint {
                return !IsArm64TranslationEnabled() && WindowsAudioAvailable() ? 48000 : 0;
            }),
        Native(
            "getPrimaryOutputFrameCount", "()I",
            +[](JNIEnv*, jclass) -> jint {
                return !IsArm64TranslationEnabled() && WindowsAudioAvailable() ? 4800 : 0;
            }),
        Native(
            "getDirectPlaybackSupport", "(Landroid/media/AudioFormat;Landroid/media/AudioAttributes;I)I",
            +[](JNIEnv*, jclass, jobject, jobject, jint) -> jint {
                return 0;
            }),
        Native(
            "native_get_offload_support", "(IILjava/lang/Object;I)I",
            +[](JNIEnv*, jclass, jint, jint, jobject, jint) -> jint {
                return 0;
            }),
        Native(
            "getParameters", "(Ljava/lang/String;)Ljava/lang/String;",
            +[](JNIEnv* e, jclass, jstring) -> jstring {
                return ArtJni<&JNINativeInterface_::NewStringUTF>(e, "");
            }),
        Native("listAudioPorts", "(Ljava/util/ArrayList;[I)I", emptyAudioPorts),
        Native("listAudioPatches", "(Ljava/util/ArrayList;[I)I", emptyAudioPorts),
    };
    if (!Bind(env, "android/media/AudioSystem", audioSystem))
        return false;
    if (!RegisterArtAudio(env))
        return false;
    // No Android sensor HAL is exposed by the software Windows host. Keep the
    // image SensorManager and its Java null/empty-device behavior, without
    // advertising fabricated accelerometers or opening Android event queues.
    const JNINativeMethod sensors[]{
        Native(
            "nativeClassInit", "()V",
            +[](JNIEnv*, jclass) {
            }),
        Native(
            "nativeCreate", "(Ljava/lang/String;)J",
            +[](JNIEnv*, jclass, jstring) -> jlong {
                return 1;
            }),
        Native(
            "nativeGetSensorAtIndex", "(JLandroid/hardware/Sensor;I)Z",
            +[](JNIEnv*, jclass, jlong, jobject, jint) -> jboolean {
                return JNI_FALSE;
            }),
        Native(
            "nativeGetDefaultDeviceSensorAtIndex", "(JLandroid/hardware/Sensor;I)Z",
            +[](JNIEnv*, jclass, jlong, jobject, jint) -> jboolean {
                return JNI_FALSE;
            }),
        Native(
            "nativeGetDynamicSensors", "(JLjava/util/List;)V",
            +[](JNIEnv*, jclass, jlong, jobject) {
            }),
        Native(
            "nativeGetRuntimeSensors", "(JILjava/util/List;)V",
            +[](JNIEnv*, jclass, jlong, jint, jobject) {
            }),
        Native(
            "nativeIsDataInjectionEnabled", "(J)Z",
            +[](JNIEnv*, jclass, jlong) -> jboolean {
                return JNI_FALSE;
            }),
        Native(
            "nativeIsReplayDataInjectionEnabled", "(J)Z",
            +[](JNIEnv*, jclass, jlong) -> jboolean {
                return JNI_FALSE;
            }),
        Native(
            "nativeIsHalBypassReplayDataInjectionEnabled", "(J)Z",
            +[](JNIEnv*, jclass, jlong) -> jboolean {
                return JNI_FALSE;
            }),
        Native(
            "nativeRegisterClientListener", "(JLandroid/os/IBinder;)V",
            +[](JNIEnv*, jclass, jlong, jobject) {
            }),
        Native(
            "nativeCreateDirectChannel", "(JIJIILandroid/hardware/HardwareBuffer;)I",
            +[](JNIEnv*, jclass, jlong, jint, jlong, jint, jint, jobject) -> jint {
                return -19;
            }),
        Native(
            "nativeConfigDirectChannel", "(JIII)I",
            +[](JNIEnv*, jclass, jlong, jint, jint, jint) -> jint {
                return -19;
            }),
        Native(
            "nativeDestroyDirectChannel", "(JI)V",
            +[](JNIEnv*, jclass, jlong, jint) {
            }),
        Native(
            "nativeSetOperationParameter", "(JII[F[I)I",
            +[](JNIEnv*, jclass, jlong, jint, jint, jfloatArray, jintArray) -> jint {
                return -19;
            }),
    };
    if (!Bind(env, "android/hardware/SystemSensorManager", sensors))
        return false;
    const JNINativeMethod launcher[]{
        Native(
            "launchNative", "(Ljava/lang/String;Ljava/lang/String;)V",
            +[](JNIEnv* env, jclass, jstring source, jstring name) {
                const StringChars sourceText(env, source), nameText(env, name);
                if (!sourceText.Get() || !nameText.Get())
                    return;
                try {
                    std::wstring wideSource, wideName;
                    // Managed guest paths and package names contain only validated ASCII.
                    for (auto ch : std::string_view(sourceText.Get()))
                        wideSource.push_back(static_cast<unsigned char>(ch));
                    for (auto ch : std::string_view(nameText.Get()))
                        wideName.push_back(static_cast<unsigned char>(ch));
                    LaunchInstalledArtApplication(wideSource, wideName);
                }
                catch (const AndroidSystemPathError&) {
                    Throw(env, "java/io/IOException", "could not resolve installed application code");
                }
                catch (const std::exception& error) {
                    Throw(env, "java/io/IOException", error.what());
                }
            }),
    };
    if (!Bind(env, "com/trebledot/stone/runtime/ArtApplicationLauncher", launcher))
        return false;
    const JNINativeMethod crossActivities[]{
        Native(
            "available", "()Z",
            +[](JNIEnv*, jclass) -> jboolean {
                return ClientNotificationHostEnabled() ? JNI_TRUE : JNI_FALSE;
            }),
        Native(
            "exchange", "(Ljava/lang/String;[Ljava/lang/String;)[Ljava/lang/String;",
            +[](JNIEnv* env, jclass, jstring package, jobjectArray request) -> jobjectArray {
                return NotificationCall(env, [&]() -> jobjectArray {
                    auto count = request ? ArtJni<&JNINativeInterface_::GetArrayLength>(env, request) : 0;
                    if (count < 1 || count > 7)
                        throw std::invalid_argument("Invalid Activity fields");
                    std::vector<std::wstring> fields{NotificationText(env, package, 255)};
                    for (jsize index = 0; index < count; ++index) {
                        auto text = static_cast<jstring>(
                            ArtJni<&JNINativeInterface_::GetObjectArrayElement>(env, request, index));
                        fields.push_back(NotificationText(env, text, 16384));
                        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, text);
                    }
                    auto values = ClientActivityRequest(fields);
                    auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/String");
                    auto result = ArtJni<&JNINativeInterface_::NewObjectArray>(env, static_cast<jsize>(values.size()),
                                                                               type, nullptr);
                    for (jsize index = 0; result && index < static_cast<jsize>(values.size()); ++index) {
                        auto value = ArtJni<&JNINativeInterface_::NewString>(
                            env, reinterpret_cast<const jchar*>(values[index].data()),
                            static_cast<jsize>(values[index].size()));
                        ArtJni<&JNINativeInterface_::SetObjectArrayElement>(env, result, index, value);
                        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, value);
                    }
                    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
                    return result;
                });
            })};
    if (!Bind(env, "com/trebledot/stone/runtime/ArtCrossActivities", crossActivities))
        return false;
    const JNINativeMethod webLinks[]{
        Native(
            "openNative", "(Ljava/lang/String;Ljava/lang/String;)V",
            +[](JNIEnv* env, jclass, jstring package, jstring url) {
                NotificationCall(env, [&] {
                    ClientOpenWebUrl({std::filesystem::canonical(ResolveAndroidSystemFile(L"data")).wstring(),
                                      NotificationText(env, package, 255), NotificationText(env, url, 8192)});
                });
            }),
    };
    if (!Bind(env, "com/trebledot/stone/runtime/ArtWebLinks", webLinks))
        return false;
    const JNINativeMethod shares[]{
        Native(
            "showNative", "(Ljava/lang/String;Ljava/lang/String;)V",
            +[](JNIEnv* env, jclass, jstring title, jstring text) {
                NotificationCall(env, [&] {
                    QueueWindowShare(WindowsHostWindow(),
                                     {NotificationText(env, title, 1024), NotificationText(env, text, 8192)});
                });
            }),
    };
    if (!Bind(env, "com/trebledot/stone/runtime/ArtShares", shares))
        return false;
    const JNINativeMethod shareFiles[]{
        Native(
            "showNative", "(Ljava/lang/String;Ljava/lang/String;I)V",
            +[](JNIEnv* env, jclass, jstring title, jstring name, jint descriptor) {
                NotificationCall(env, [&] {
                    std::unique_ptr<void, decltype(&CloseHandle)> source(
                        reinterpret_cast<HANDLE>(DuplicateAndroidDiskFileHandle(descriptor)), CloseHandle);
                    auto content =
                        SnapshotShareFile(NotificationText(env, title, 1024), NotificationText(env, name, 128),
                                          source.get(), ResolveAndroidSystemFile(L"data/system/stone-shares"));
                    QueueWindowShare(WindowsHostWindow(), std::move(content));
                });
            }),
    };
    if (!Bind(env, "com/trebledot/stone/runtime/ArtShareFiles", shareFiles))
        return false;
    const JNINativeMethod notifications[]{
        Native(
            "initializeNative", "(Ljava/lang/String;)Z",
            +[](JNIEnv* env, jclass, jstring package) -> jboolean {
                return NotificationCall(env, [&]() -> jboolean {
                    return InitializeArtNotifications(NotificationText(env, package, 255));
                });
            }),
        Native(
            "allowedNative", "()Z",
            +[](JNIEnv* env, jclass) -> jboolean {
                return NotificationCall(env, [&]() -> jboolean {
                    return ArtNotificationsAllowed();
                });
            }),
        ScalarNative(
            "postNative",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;IIZZZ[Ljava/lang/String;Ljava/lang/String;I)[Ljava/lang/String;",
            +[](JNIEnv* env, jclass, jstring key, jstring title, jstring text, jobjectArray actions, jint progress,
                jint maximum, jboolean indeterminate, jboolean silent, jboolean suppressPopup, jobjectArray targets,
                jstring channel, jint flags) -> jobjectArray {
                return NotificationCall(env, [&]() -> jobjectArray {
                    ArtNotification request;
                    request.key = NotificationText(env, key, 2048);
                    request.title = NotificationText(env, title, 1024);
                    request.text = NotificationText(env, text, 8192);
                    request.progress = progress;
                    request.maximum = maximum;
                    request.indeterminate = indeterminate;
                    request.silent = silent;
                    request.suppressPopup = suppressPopup;
                    const auto count = actions ? ArtJni<&JNINativeInterface_::GetArrayLength>(env, actions) : 0;
                    if (count > 5)
                        throw std::runtime_error("too many notification actions");
                    for (jsize index = 0; index < count; ++index) {
                        const auto label = static_cast<jstring>(
                            ArtJni<&JNINativeInterface_::GetObjectArrayElement>(env, actions, index));
                        request.actions.push_back(NotificationText(env, label, 256));
                        if (label)
                            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, label);
                    }
                    request.flags = static_cast<uint32_t>(flags);
                    request.channel = NotificationText(env, channel, 1024);
                    auto targetCount = targets ? ArtJni<&JNINativeInterface_::GetArrayLength>(env, targets) : 0;
                    if (targetCount && targetCount != count + 2)
                        throw std::runtime_error("Invalid notification targets");
                    for (jsize index = 0; index < targetCount; ++index) {
                        auto target = static_cast<jstring>(
                            ArtJni<&JNINativeInterface_::GetObjectArrayElement>(env, targets, index));
                        request.targets.push_back(NotificationText(env, target, 32));
                        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, target);
                    }
                    auto values = PostArtNotification(request);
                    auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/String");
                    auto result = ArtJni<&JNINativeInterface_::NewObjectArray>(env, static_cast<jsize>(values.size()),
                                                                               type, nullptr);
                    for (jsize index = 0; result && index < static_cast<jsize>(values.size()); ++index) {
                        auto value = ArtJni<&JNINativeInterface_::NewString>(
                            env, reinterpret_cast<const jchar*>(values[index].data()),
                            static_cast<jsize>(values[index].size()));
                        ArtJni<&JNINativeInterface_::SetObjectArrayElement>(env, result, index, value);
                        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, value);
                    }
                    return result;
                });
            }),
        Native(
            "removeNative", "(Ljava/lang/String;)V",
            +[](JNIEnv* env, jclass, jstring key) {
                NotificationCall(env, [&] {
                    RemoveArtNotification(NotificationText(env, key, 2048));
                });
            }),
        Native(
            "removeAllNative", "()V",
            +[](JNIEnv* env, jclass) {
                NotificationCall(env, [] {
                    RemoveArtNotifications();
                });
            }),
        Native(
            "pollNative", "()[J",
            +[](JNIEnv* env, jclass) -> jlongArray {
                return NotificationCall(env, [&]() -> jlongArray {
                    const auto events = PollArtNotifications();
                    const auto array =
                        ArtJni<&JNINativeInterface_::NewLongArray>(env, static_cast<jsize>(events.size()));
                    if (array && !events.empty())
                        ArtJni<&JNINativeInterface_::SetLongArrayRegion>(env, array, 0,
                                                                         static_cast<jsize>(events.size()),
                                                                         reinterpret_cast<const jlong*>(events.data()));
                    return array;
                });
            }),
    };
    if (!Bind(env, "com/trebledot/stone/runtime/ArtNotifications", notifications))
        return false;
    const JNINativeMethod documents[]{
        Native(
            "createNative", "(Ljava/lang/String;Ljava/lang/String;)J",
            +[](JNIEnv* env, jclass, jstring name, jstring mime) -> jlong {
                return NotificationCall(env, [&]() -> jlong {
                    return static_cast<jlong>(CreateWindowDocument(
                        WindowsHostWindow(), NotificationText(env, name, 255), NotificationText(env, mime, 255)));
                });
            }),
        Native(
            "pickNative", "(J)[Ljava/lang/String;",
            +[](JNIEnv* env, jclass, jlong id) -> jobjectArray {
                return NotificationCall(env, [&]() -> jobjectArray {
                    auto selected = PickWindowDocument(static_cast<uint64_t>(id));
                    if (!selected)
                        return nullptr;
                    auto name = selected->name;
                    auto grant = KeepWindowDocument(std::move(*selected));
                    try {
                        auto number = std::to_wstring(grant);
                        auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/String");
                        auto result =
                            type ? ArtJni<&JNINativeInterface_::NewObjectArray>(env, 2, type, nullptr) : nullptr;
                        for (jsize index = 0; result && index < 2; index++) {
                            const auto& text = index == 0 ? number : name;
                            auto value = ArtJni<&JNINativeInterface_::NewString>(
                                env, reinterpret_cast<const jchar*>(text.data()), static_cast<jsize>(text.size()));
                            if (!value)
                                break;
                            ArtJni<&JNINativeInterface_::SetObjectArrayElement>(env, result, index, value);
                            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, value);
                        }
                        if (!result || ArtJni<&JNINativeInterface_::ExceptionCheck>(env)) {
                            ReleaseWindowDocumentGrant(grant);
                            return nullptr;
                        }
                        return result;
                    }
                    catch (...) {
                        ReleaseWindowDocumentGrant(grant);
                        throw;
                    }
                });
            }),
        Native(
            "cancelNative", "(J)V",
            +[](JNIEnv* env, jclass, jlong id) {
                NotificationCall(env, [&] {
                    CancelWindowDocument(static_cast<uint64_t>(id));
                });
            }),
        Native(
            "releaseNative", "(J)V",
            +[](JNIEnv*, jclass, jlong id) {
                ReleaseWindowDocument(static_cast<uint64_t>(id));
            }),
        Native(
            "releaseGrantNative", "(J)V",
            +[](JNIEnv*, jclass, jlong id) {
                ReleaseWindowDocumentGrant(static_cast<uint64_t>(id));
            }),
        Native(
            "sizeNative", "(J)J",
            +[](JNIEnv* env, jclass, jlong id) -> jlong {
                return NotificationCall(env, [&]() -> jlong {
                    return static_cast<jlong>(WindowDocumentSize(static_cast<uint64_t>(id)));
                });
            }),
        Native(
            "openNative", "(JI)I",
            +[](JNIEnv* env, jclass, jlong id, jint mode) -> jint {
                return NotificationCall(env, [&]() -> jint {
                    constexpr int accessMask = 0x30000000, allowed = accessMask | 0x0e000000;
                    int access = mode & accessMask;
                    if ((mode & ~allowed) || access == 0)
                        throw std::invalid_argument("Invalid document descriptor mode");
                    auto handle =
                        OpenWindowDocument(static_cast<uint64_t>(id), access != 0x20000000, access != 0x10000000,
                                           (mode & 0x04000000) != 0, (mode & 0x02000000) != 0);
                    int descriptor = ImportAndroidSocketFile({reinterpret_cast<uintptr_t>(handle.detach())});
                    if (descriptor < 0)
                        throw std::runtime_error("Cannot import selected document descriptor");
                    return descriptor;
                });
            }),
    };
    if (!Bind(env, "com/trebledot/stone/runtime/ArtDocuments", documents))
        return false;
    if (!RegisterArtDocumentTrees(env))
        return false;
    const JNINativeMethod settings[]{
        Native(
            "enabledNative", "()Z",
            +[](JNIEnv*, jclass) -> jboolean {
                return ArtSettingsEnabled();
            }),
        Native(
            "createNative", "([B)J",
            +[](JNIEnv* env, jclass, jbyteArray bytes) -> jlong {
                try {
                    if (!bytes)
                        throw std::runtime_error("missing settings confirmation");
                    const auto size = ArtJni<&JNINativeInterface_::GetArrayLength>(env, bytes);
                    if (size <= 0 || size > 64000)
                        throw std::runtime_error("invalid settings confirmation size");
                    std::string json(static_cast<size_t>(size), '\0');
                    ArtJni<&JNINativeInterface_::GetByteArrayRegion>(env, bytes, 0, size,
                                                                     reinterpret_cast<jbyte*>(json.data()));
                    if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
                        return 0;
                    return static_cast<jlong>(CreateArtSettingsConfirmation(std::move(json)));
                }
                catch (const std::exception& error) {
                    Throw(env, "java/io/IOException", error.what());
                    return 0;
                }
            }),
        Native(
            "awaitNative", "(J)Z",
            +[](JNIEnv*, jclass, jlong id) -> jboolean {
                return AwaitArtSettingsConfirmation(static_cast<uint64_t>(id));
            }),
        Native(
            "cancelNative", "(J)V",
            +[](JNIEnv*, jclass, jlong id) {
                CancelArtSettingsConfirmation(static_cast<uint64_t>(id));
            }),
    };
    return Bind(env, "com/trebledot/stone/runtime/ArtSettingsConfirmation", settings);
}
}
