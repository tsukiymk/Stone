#include "stub-libmedia_jni.h"
#include "runtime/art/ArtJni.h"
#include "abi/TypedAndroidImport.h"
#include "abi/AndroidLibraryImports.h"
#include "../../../host/media/ArtMediaDecoder.h"
#include "framework/ArtSurfaces.h"
#include "framework/ArtSurfaceTextures.h"
#include "hle/system/stub-libandroid.h"
#include "hle/system/stub-libmediandk-image-reader.h"
#include "abi/Arm64ExecutionBridge.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <cstdio>

namespace stone::windows {
bool ArtNativeLoaderAvailable();

namespace {
void Throw(JNIEnv* env, const char* type, const char* message)
{
    auto exception = ArtJni<&JNINativeInterface_::FindClass>(env, type);
    if (exception) {
        ArtJni<&JNINativeInterface_::ThrowNew>(env, exception, message);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, exception);
    }
}

template <class F> JNINativeMethod Method(const char* name, const char* signature, F function)
{
    return {
        const_cast<char*>(name), const_cast<char*>(signature),
        reinterpret_cast<void*>(ImportAndroidScalars<F>(std::string("ART media capability ") + name + signature + "@" +
                                                            std::to_string(reinterpret_cast<uintptr_t>(function)),
                                                        reinterpret_cast<uintptr_t>(function)))};
}

template <size_t N> bool Bind(JNIEnv* env, const char* name, const JNINativeMethod (&methods)[N])
{
    // Library loading is already inside a media class initializer. Loading
    // without initialization lets us bind native_init before it is invoked.
    auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/Class");
    auto lookup = type ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(
                             env, type, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;")
                       : nullptr;
    auto text = ArtJni<&JNINativeInterface_::NewStringUTF>(env, name);
    jvalue args[]{jvalue{.l = text}, jvalue{.z = JNI_FALSE}, jvalue{.l = nullptr}};
    auto target =
        lookup ? static_cast<jclass>(ArtJni<&JNINativeInterface_::CallStaticObjectMethodA>(env, type, lookup, args))
               : nullptr;
    bool ok =
        target && ArtJni<&JNINativeInterface_::RegisterNatives>(env, target, methods, static_cast<jint>(N)) == JNI_OK;
    if (target)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, target);
    if (text)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, text);
    if (type)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    return ok;
}

template <class R = void> R Call(JNIEnv* env, const char* name, const char* signature, const jvalue* args = nullptr)
{
    auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/runtime/ArtMediaCodecs");
    auto method = type ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, name, signature) : nullptr;
    if constexpr (std::is_same_v<R, void>) {
        if (method)
            ArtJni<&JNINativeInterface_::CallStaticVoidMethodA>(env, type, method, args);
        if (type)
            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    }
    else {
        R result{};
        if (method) {
            if constexpr (std::is_same_v<R, jint>)
                result = ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, type, method, args);
            else
                result = static_cast<R>(ArtJni<&JNINativeInterface_::CallStaticObjectMethodA>(env, type, method, args));
        }
        if (type)
            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
        return result;
    }
}

std::vector<uint8_t> Bytes(JNIEnv* e, jbyteArray data)
{
    if (!data)
        return {};
    std::vector<uint8_t> bytes(ArtJni<&JNINativeInterface_::GetArrayLength>(e, data));
    if (!bytes.empty())
        ArtJni<&JNINativeInterface_::GetByteArrayRegion>(e, data, 0, static_cast<jsize>(bytes.size()),
                                                         reinterpret_cast<jbyte*>(bytes.data()));
    return bytes;
}

void Unsupported(JNIEnv* env)
{
    Throw(env, "java/lang/UnsupportedOperationException", "Unsupported Stone codec operation or encrypted content");
}

struct VideoFrame {
    int64_t decoder;
    ArtVideoFrame frame;
};

std::mutex videoFramesMutex;
std::unordered_map<int64_t, std::shared_ptr<VideoFrame>> videoFrames;
int64_t nextVideoFrame = 1;

void DropVideoFrames(int64_t decoder)
{
    const std::lock_guard lock(videoFramesMutex);
    std::erase_if(videoFrames, [&](const auto& entry) {
        return entry.second->decoder == decoder;
    });
}

void RenderPixels(JNIEnv* env, const std::vector<int32_t>& source, int width, int height, jobject surface,
                  jlong timestamp)
try {
    if (width < 2 || height < 2 || width > 1920 || height > 1088 || source.size() != size_t(width) * height) {
        Throw(env, "java/lang/IllegalArgumentException", "Invalid decoded video frame");
        return;
    }
    std::unique_ptr<NativeWindow, decltype(&ReleaseWindow)> window(
        reinterpret_cast<NativeWindow*>(ArtSurfaceFromJava(env, surface)), ReleaseWindow);
    if (!window)
        return;
    // Decoder producers override a SurfaceTexture's default geometry. Screen
    // Surfaces retain their view geometry and receive scaled pixels below.
    if (IsArtSurfaceTexture(reinterpret_cast<uintptr_t>(window.get())) &&
        ResizeAndroidSoftwareSurfaceQueue(*window->imageQueue, width, height) != 0) {
        Throw(env, "java/lang/IllegalStateException", "Cannot resize video SurfaceTexture");
        return;
    }
    AndroidNativeWindowBuffer buffer{};
    if (LockAndroidImageQueue(*window->imageQueue, buffer, nullptr) != 0) {
        Throw(env, "java/lang/IllegalStateException", "Video Surface is no longer writable");
        return;
    }
    auto* output = static_cast<uint32_t*>(buffer.bits);
    for (int y = 0; y < buffer.height; ++y) {
        const auto* row = source.data() + size_t(y) * height / buffer.height * width;
        for (int x = 0; x < buffer.width; ++x) {
            const auto color = static_cast<uint32_t>(row[buffer.width == width ? x : size_t(x) * width / buffer.width]);
            output[size_t(y) * buffer.stride + x] =
                (color & 0xff00ff00U) | ((color >> 16) & 255U) | ((color & 255U) << 16);
        }
    }
    if (PostAndroidImageQueue(*window->imageQueue, timestamp) != 0)
        Throw(env, "java/lang/IllegalStateException", "Cannot publish decoded video frame");
}
catch (const std::exception& error) {
    Throw(env, "java/lang/IllegalStateException", error.what());
}

void RenderVideo(JNIEnv* env, jclass, jintArray pixels, jint width, jint height, jobject surface, jlong timestamp)
try {
    if (!pixels || width < 2 || height < 2 || width > 1920 || height > 1088 ||
        ArtJni<&JNINativeInterface_::GetArrayLength>(env, pixels) != int64_t(width) * height) {
        Throw(env, "java/lang/IllegalArgumentException", "Invalid video pixels");
        return;
    }
    std::vector<int32_t> source(static_cast<size_t>(width) * height);
    ArtJni<&JNINativeInterface_::GetIntArrayRegion>(env, pixels, 0, static_cast<jsize>(source.size()), source.data());
    if (!ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
        RenderPixels(env, source, width, height, surface, timestamp);
}
catch (const std::exception& error) {
    Throw(env, "java/lang/IllegalStateException", error.what());
}
jint OnLoad(JavaVM* vm, void*)
try {
    JNIEnv* env = nullptr;
    auto getEnv = reinterpret_cast<jint (*)(JavaVM*, void**, jint)>(
        MakeNativeAndroidThunk(reinterpret_cast<uintptr_t>(vm->functions->GetEnv), "(LLI)I"));
    if (getEnv(vm, reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env)
        return JNI_ERR;
    const JNINativeMethod host[]{
        Method(
            "view", "(Ljava/nio/ByteBuffer;)Ljava/nio/ByteBuffer;",
            +[](JNIEnv* e, jclass, jobject allocation) -> jobject {
                auto address = ArtJni<&JNINativeInterface_::GetDirectBufferAddress>(e, allocation);
                auto size = ArtJni<&JNINativeInterface_::GetDirectBufferCapacity>(e, allocation);
                return ArtJni<&JNINativeInterface_::NewDirectByteBuffer>(e, address, size);
            }),
        Method(
            "available", "()I",
            +[](JNIEnv*, jclass) -> jint {
                static int mask = AvailableArtAudioDecoders() | (ArtAvcDecoderAvailable() ? 2 : 0) |
                                  (ArtHevcDecoderAvailable() ? 4 : 0);
                return mask;
            }),
        Method(
            "videoSurface", "(Landroid/view/Surface;)Z",
            +[](JNIEnv* env, jclass, jobject surface) -> jboolean {
                const auto window = reinterpret_cast<NativeWindow*>(ArtSurfaceFromJava(env, surface));
                if (!window)
                    return JNI_FALSE;
                ReleaseWindow(window);
                return JNI_TRUE;
            }),
        Method(
            "openVideo", "(ZII[B)J",
            +[](JNIEnv* env, jclass, jboolean hevc, jint width, jint height, jbyteArray config) -> jlong {
                try {
                    return OpenArtVideoDecoder(hevc != 0, width, height, Bytes(env, config));
                }
                catch (const std::exception& error) {
                    Throw(env, "java/lang/IllegalArgumentException", error.what());
                    return 0;
                }
            }),
        Method(
            "decodeVideo", "(J[BJZ)[J",
            +[](JNIEnv* env, jclass, jlong handle, jbyteArray packet, jlong pts, jboolean eos) -> jlongArray {
                std::vector<jlong> descriptors;
                auto cleanup = [&] {
                    const std::lock_guard lock(videoFramesMutex);
                    for (size_t i = 0; i < descriptors.size(); i += 4)
                        videoFrames.erase(descriptors[i]);
                };
                try {
                    auto frames = DecodeArtVideo(handle, Bytes(env, packet), pts, eos != 0);
                    descriptors.reserve(frames.size() * 4);
                    for (auto& frame : frames) {
                        auto owned = std::make_shared<VideoFrame>(VideoFrame{handle, std::move(frame)});
                        const std::lock_guard lock(videoFramesMutex);
                        const auto id = nextVideoFrame++;
                        videoFrames.emplace(id, owned);
                        descriptors.insert(descriptors.end(),
                                           {id, owned->frame.pts, owned->frame.width, owned->frame.height});
                    }
                    const auto result =
                        ArtJni<&JNINativeInterface_::NewLongArray>(env, static_cast<jsize>(descriptors.size()));
                    if (result)
                        ArtJni<&JNINativeInterface_::SetLongArrayRegion>(
                            env, result, 0, static_cast<jsize>(descriptors.size()), descriptors.data());
                    if (!result || ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
                        cleanup();
                    return result;
                }
                catch (const std::exception& error) {
                    cleanup();
                    Throw(env, "java/lang/IllegalStateException", error.what());
                    return nullptr;
                }
            }),
        Method("renderVideo", "([IIILandroid/view/Surface;J)V", &RenderVideo),
        Method(
            "releaseVideoFrame", "(J)V",
            +[](JNIEnv*, jclass, jlong frame) {
                const std::lock_guard lock(videoFramesMutex);
                videoFrames.erase(frame);
            }),
        Method(
            "renderVideoFrame", "(JLandroid/view/Surface;J)V",
            +[](JNIEnv* env, jclass, jlong frame, jobject surface, jlong timestamp) {
                std::shared_ptr<VideoFrame> owned;
                {
                    const std::lock_guard lock(videoFramesMutex);
                    const auto found = videoFrames.find(frame);
                    if (found != videoFrames.end())
                        owned = found->second;
                }
                if (!owned) {
                    Throw(env, "java/lang/IllegalStateException", "Video output frame was released");
                    return;
                }
                RenderPixels(env, owned->frame.pixels, owned->frame.width, owned->frame.height, surface, timestamp);
            }),
        Method(
            "flushVideo", "(J)V",
            +[](JNIEnv* env, jclass, jlong handle) {
                try {
                    FlushArtVideoDecoder(handle);
                    DropVideoFrames(handle);
                }
                catch (const std::exception& error) {
                    Throw(env, "java/lang/IllegalStateException", error.what());
                }
            }),
        Method(
            "closeVideo", "(J)V",
            +[](JNIEnv* env, jclass, jlong handle) {
                try {
                    CloseArtVideoDecoder(handle);
                    DropVideoFrames(handle);
                }
                catch (const std::exception& error) {
                    Throw(env, "java/lang/IllegalStateException", error.what());
                }
            }),
        Method(
            "open", "(ZII[B)J",
            +[](JNIEnv* e, jclass, jboolean opus, jint rate, jint channels, jbyteArray csd) -> jlong {
                try {
                    return OpenArtAudioDecoder(opus != 0, rate, channels, Bytes(e, csd));
                }
                catch (const std::exception& error) {
                    Throw(e, "java/lang/IllegalArgumentException", error.what());
                    return 0;
                }
            }),
        Method(
            "decode", "(J[B[JZ)[B",
            +[](JNIEnv* e, jclass, jlong handle, jbyteArray input, jlongArray times, jboolean eos) -> jbyteArray {
                try {
                    jlong time = 0;
                    ArtJni<&JNINativeInterface_::GetLongArrayRegion>(e, times, 0, 1, &time);
                    int64_t pts = time;
                    auto bytes = DecodeArtAudio(handle, Bytes(e, input), pts, eos != 0);
                    time = pts;
                    ArtJni<&JNINativeInterface_::SetLongArrayRegion>(e, times, 0, 1, &time);
                    auto result = ArtJni<&JNINativeInterface_::NewByteArray>(e, static_cast<jsize>(bytes.size()));
                    if (result && !bytes.empty())
                        ArtJni<&JNINativeInterface_::SetByteArrayRegion>(e, result, 0, static_cast<jsize>(bytes.size()),
                                                                         reinterpret_cast<const jbyte*>(bytes.data()));
                    return result;
                }
                catch (const std::exception& error) {
                    Throw(e, "java/lang/IllegalStateException", error.what());
                    return nullptr;
                }
            }),
        Method(
            "flushDecoder", "(J)V",
            +[](JNIEnv* e, jclass, jlong handle) {
                try {
                    FlushArtAudioDecoder(handle);
                }
                catch (const std::exception& error) {
                    Throw(e, "java/lang/IllegalStateException", error.what());
                }
            }),
        Method(
            "closeDecoder", "(J)V",
            +[](JNIEnv*, jclass, jlong handle) {
                CloseArtAudioDecoder(handle);
            }),
    };
    if (!Bind(env, "com.trebledot.stone.runtime.ArtMediaCodecs", host))
        return JNI_ERR;
    const JNINativeMethod list[]{
        Method(
            "native_init", "()V",
            +[] {
            }),
        Method(
            "native_getCodecCount", "()I",
            +[](JNIEnv* e, jclass) -> jint {
                return Call<jint>(e, "count", "()I");
            }),
        Method(
            "findCodecByName", "(Ljava/lang/String;)I",
            +[](JNIEnv* e, jclass, jstring name) -> jint {
                jvalue args[]{jvalue{.l = name}};
                return Call<jint>(e, "find", "(Ljava/lang/String;)I", args);
            }),
        Method(
            "getCodecName", "(I)Ljava/lang/String;",
            +[](JNIEnv* e, jclass, jint index) -> jstring {
                jvalue args[]{jvalue{.i = index}};
                return Call<jstring>(e, "name", "(I)Ljava/lang/String;", args);
            }),
        Method(
            "getCanonicalName", "(I)Ljava/lang/String;",
            +[](JNIEnv* e, jclass, jint index) -> jstring {
                jvalue args[]{jvalue{.i = index}};
                return Call<jstring>(e, "name", "(I)Ljava/lang/String;", args);
            }),
        Method(
            "getAttributes", "(I)I",
            +[](JNIEnv*, jclass, jint) -> jint {
                return 4;
            }),
        Method(
            "getSecurityModel", "(I)I",
            +[](JNIEnv*, jclass, jint) -> jint {
                return 0;
            }),
        Method(
            "getSupportedTypes", "(I)[Ljava/lang/String;",
            +[](JNIEnv* e, jclass, jint index) -> jobjectArray {
                jvalue args[]{jvalue{.i = index}};
                return Call<jobjectArray>(e, "types", "(I)[Ljava/lang/String;", args);
            }),
        Method(
            "getCodecCapabilities", "(ILjava/lang/String;)Landroid/media/MediaCodecInfo$CodecCapabilities;",
            +[](JNIEnv* e, jclass, jint index, jstring mime) -> jobject {
                jvalue args[]{jvalue{.i = index}, jvalue{.l = mime}};
                return Call<jobject>(e, "capabilities",
                                     "(ILjava/lang/String;)Landroid/media/MediaCodecInfo$CodecCapabilities;", args);
            }),
        Method(
            "native_getGlobalSettings", "()Ljava/util/Map;",
            +[](JNIEnv* e, jclass) -> jobject {
                auto type = ArtJni<&JNINativeInterface_::FindClass>(e, "java/util/HashMap");
                auto constructor = type ? ArtJni<&JNINativeInterface_::GetMethodID>(e, type, "<init>", "()V") : nullptr;
                auto result =
                    constructor ? ArtJni<&JNINativeInterface_::NewObjectA>(e, type, constructor, nullptr) : nullptr;
                if (type)
                    ArtJni<&JNINativeInterface_::DeleteLocalRef>(e, type);
                return result;
            }),
    };
    const JNINativeMethod codec[]{
        Method(
            "native_init", "()V",
            +[] {
            }),
        Method(
            "native_setup", "(Ljava/lang/String;ZZII)V",
            +[](JNIEnv* e, jobject self, jstring name, jboolean byType, jboolean encoder, jint, jint) {
                jvalue a[]{jvalue{.l = self}, jvalue{.l = name}, jvalue{.z = byType}, jvalue{.z = encoder}};
                Call(e, "setup", "(Landroid/media/MediaCodec;Ljava/lang/String;ZZ)V", a);
            }),
        Method(
            "native_configure",
            "([Ljava/lang/String;[Ljava/lang/Object;Landroid/view/Surface;Landroid/media/MediaCrypto;Landroid/os/IHwBinder;I)V",
            +[](JNIEnv* e, jobject self, jobjectArray keys, jobjectArray values, jobject surface, jobject crypto,
                jobject binder, jint flags) {
                jvalue a[]{jvalue{.l = self},   jvalue{.l = keys},   jvalue{.l = values}, jvalue{.l = surface},
                           jvalue{.l = crypto}, jvalue{.l = binder}, jvalue{.i = flags}};
                Call(
                    e, "configure",
                    "(Landroid/media/MediaCodec;[Ljava/lang/String;[Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;I)V",
                    a);
            }),
        Method(
            "native_finalize", "()V",
            +[](JNIEnv* e, jobject self) {
                jvalue a[]{jvalue{.l = self}};
                Call(e, "release", "(Landroid/media/MediaCodec;)V", a);
            }),
        Method(
            "native_release", "()V",
            +[](JNIEnv* e, jobject self) {
                jvalue a[]{jvalue{.l = self}};
                Call(e, "release", "(Landroid/media/MediaCodec;)V", a);
            }),
        Method(
            "native_reset", "()V",
            +[](JNIEnv* e, jobject self) {
                jvalue a[]{jvalue{.l = self}};
                Call(e, "stop", "(Landroid/media/MediaCodec;)V", a);
            }),
        Method(
            "native_stop", "()V",
            +[](JNIEnv* e, jobject self) {
                jvalue a[]{jvalue{.l = self}};
                Call(e, "stop", "(Landroid/media/MediaCodec;)V", a);
            }),
        Method(
            "native_start", "()V",
            +[](JNIEnv* e, jobject self) {
                jvalue a[]{jvalue{.l = self}};
                Call(e, "start", "(Landroid/media/MediaCodec;)V", a);
            }),
        Method(
            "native_flush", "()V",
            +[](JNIEnv* e, jobject self) {
                jvalue a[]{jvalue{.l = self}};
                Call(e, "flush", "(Landroid/media/MediaCodec;)V", a);
            }),
        Method(
            "native_setCallback", "(Landroid/media/MediaCodec$Callback;)V",
            +[](JNIEnv* e, jobject self, jobject callback) {
                jvalue a[]{jvalue{.l = self}, jvalue{.l = callback}};
                Call(e, "callback", "(Landroid/media/MediaCodec;Ljava/lang/Object;)V", a);
            }),
        Method(
            "getBuffers", "(Z)[Ljava/nio/ByteBuffer;",
            +[](JNIEnv* e, jobject self, jboolean input) -> jobjectArray {
                jvalue a[]{jvalue{.l = self}, jvalue{.z = input}};
                return Call<jobjectArray>(e, "buffers", "(Landroid/media/MediaCodec;Z)[Ljava/nio/ByteBuffer;", a);
            }),
        Method(
            "getBuffer", "(ZI)Ljava/nio/ByteBuffer;",
            +[](JNIEnv* e, jobject self, jboolean input, jint index) -> jobject {
                jvalue a[]{jvalue{.l = self}, jvalue{.z = input}, jvalue{.i = index}};
                return Call<jobject>(e, "buffer", "(Landroid/media/MediaCodec;ZI)Ljava/nio/ByteBuffer;", a);
            }),
        Method(
            "getFormatNative", "(Z)Ljava/util/Map;",
            +[](JNIEnv* e, jobject self, jboolean input) -> jobject {
                jvalue a[]{jvalue{.l = self}, jvalue{.z = input}};
                return Call<jobject>(e, "format", "(Landroid/media/MediaCodec;Z)Ljava/util/Map;", a);
            }),
        Method(
            "getOutputFormatNative", "(I)Ljava/util/Map;",
            +[](JNIEnv* e, jobject self, jint) -> jobject {
                jvalue a[]{jvalue{.l = self}, jvalue{.z = JNI_FALSE}};
                return Call<jobject>(e, "format", "(Landroid/media/MediaCodec;Z)Ljava/util/Map;", a);
            }),
        Method(
            "native_dequeueInputBuffer", "(J)I",
            +[](JNIEnv* e, jobject self, jlong timeout) -> jint {
                jvalue a[]{jvalue{.l = self}, jvalue{.j = timeout}};
                return Call<jint>(e, "input", "(Landroid/media/MediaCodec;J)I", a);
            }),
        Method(
            "native_dequeueOutputBuffer", "(Landroid/media/MediaCodec$BufferInfo;J)I",
            +[](JNIEnv* e, jobject self, jobject info, jlong timeout) -> jint {
                jvalue a[]{jvalue{.l = self}, jvalue{.l = info}, jvalue{.j = timeout}};
                return Call<jint>(e, "output", "(Landroid/media/MediaCodec;Landroid/media/MediaCodec$BufferInfo;J)I",
                                  a);
            }),
        Method(
            "native_queueInputBuffer", "(IIIJI)V",
            +[](JNIEnv* e, jobject self, jint index, jint offset, jint size, jlong pts, jint flags) {
                jvalue a[]{jvalue{.l = self}, jvalue{.i = index}, jvalue{.i = offset},
                           jvalue{.i = size}, jvalue{.j = pts},   jvalue{.i = flags}};
                Call(e, "queue", "(Landroid/media/MediaCodec;IIIJI)V", a);
            }),
        Method(
            "releaseOutputBuffer", "(IZZJ)V",
            +[](JNIEnv* e, jobject self, jint index, jboolean render, jboolean timestamp, jlong nanos) {
                jvalue a[]{jvalue{.l = self}, jvalue{.i = index}, jvalue{.z = render},
                           jvalue{.j = timestamp ? nanos : -1}};
                Call(e, "releaseOutput", "(Landroid/media/MediaCodec;IZJ)V", a);
            }),
        Method(
            "native_setSurface", "(Landroid/view/Surface;)V",
            +[](JNIEnv* env, jobject self, jobject surface) {
                const jvalue args[]{jvalue{.l = self}, jvalue{.l = surface}};
                Call(env, "surface", "(Landroid/media/MediaCodec;Landroid/view/Surface;)V", args);
            }),
        Method(
            "setVideoScalingMode", "(I)V",
            +[](JNIEnv* env, jobject self, jint mode) {
                const jvalue args[]{jvalue{.l = self}, jvalue{.i = mode}};
                Call(env, "scaling", "(Landroid/media/MediaCodec;I)V", args);
            }),
        Method(
            "getCanonicalName", "()Ljava/lang/String;",
            +[](JNIEnv* e, jobject self) -> jstring {
                jvalue a[]{jvalue{.l = self}};
                return Call<jstring>(e, "canonical", "(Landroid/media/MediaCodec;)Ljava/lang/String;", a);
            }),
        Method(
            "getOwnCodecInfo", "()Landroid/media/MediaCodecInfo;",
            +[](JNIEnv* e, jobject self) -> jobject {
                jvalue a[]{jvalue{.l = self}};
                return Call<jobject>(e, "info", "(Landroid/media/MediaCodec;)Landroid/media/MediaCodecInfo;", a);
            }),
        Method(
            "native_getMetrics", "()Landroid/os/PersistableBundle;",
            +[](JNIEnv* e, jobject self) -> jobject {
                jvalue a[]{jvalue{.l = self}};
                return Call<jobject>(e, "metrics", "(Landroid/media/MediaCodec;)Landroid/os/PersistableBundle;", a);
            }),
        Method(
            "setParameters", "([Ljava/lang/String;[Ljava/lang/Object;)V",
            +[](JNIEnv* e, jobject self, jobjectArray keys, jobjectArray values) {
                jvalue a[]{jvalue{.l = self}, jvalue{.l = keys}, jvalue{.l = values}};
                Call(e, "parameters", "(Landroid/media/MediaCodec;[Ljava/lang/String;[Ljava/lang/Object;)V", a);
            }),
        Method(
            "native_enableOnFrameRenderedListener", "(Z)V",
            +[](JNIEnv*, jobject, jboolean) {
            }),
        Method(
            "native_enableOnFirstTunnelFrameReadyListener", "(Z)V",
            +[](JNIEnv*, jobject, jboolean) {
            }),
        Method(
            "native_queueSecureInputBuffer", "(IILandroid/media/MediaCodec$CryptoInfo;JI)V",
            +[](JNIEnv* e, jobject, jint, jint, jobject, jlong, jint) {
                Unsupported(e);
            }),
    };
    return Bind(env, "android.media.MediaCodecList", list) && Bind(env, "android.media.MediaCodec", codec)
               ? JNI_VERSION_1_6
               : JNI_ERR;
}
catch (const std::exception& error) {
    std::fprintf(stderr, "[art-media] JNI initialization failed: %s\n", error.what());
    return JNI_ERR;
}
}

bool ArtMediaJniAvailable()
{
    return ArtNativeLoaderAvailable() && !IsArm64TranslationEnabled();
}

uintptr_t AndroidMediaJniImport(std::string_view name)
{
    if (name == "JNI_OnLoad")
        return ImportAndroidScalars<decltype(&OnLoad)>("ART media capability JNI_OnLoad",
                                                       reinterpret_cast<uintptr_t>(&OnLoad));
    return 0;
}
}
