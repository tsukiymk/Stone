#include "../../../host/media/MediaDecoderState.h"
#include <jni.h>
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

namespace stone::windows {
namespace {
using namespace media_backend;

std::vector<uint8_t> Bytes(JNIEnv* env, jbyteArray data)
{
    if (!data)
        return {};
    std::vector<uint8_t> result(env->GetArrayLength(data));
    if (!result.empty())
        env->GetByteArrayRegion(data, 0, static_cast<jsize>(result.size()), reinterpret_cast<jbyte*>(result.data()));
    return result;
}

bool IsAvc(JNIEnv* env, jstring mime)
{
    if (!mime)
        return false;
    const char* value = env->GetStringUTFChars(mime, nullptr);
    if (!value)
        return false;
    const bool avc = std::strcmp(value, "video/avc") == 0;
    env->ReleaseStringUTFChars(mime, value);
    return avc;
}

jobjectArray JNICALL VideoMimes(JNIEnv* env, jclass)
{
    static const bool available = ProbeAvc();
    auto mimes = env->NewObjectArray(available ? 1 : 0, env->FindClass("java/lang/String"), nullptr);
    if (mimes && available)
        env->SetObjectArrayElement(mimes, 0, env->NewStringUTF("video/avc"));
    return mimes;
}

jboolean JNICALL AacAvailable(JNIEnv*, jclass)
{
    static const bool available = [] {
        Apartment apartment;
        Decoder probe;
        probe.sampleRate = 48000;
        probe.channels = 2;
        return SUCCEEDED(Open(probe, CLSID_CMSAACDecMFT, MFMediaType_Audio, MFAudioFormat_AAC, {0x11, 0x90}));
    }();
    return available;
}

jlong JNICALL OpenAudio(JNIEnv* env, jclass, jint format, jint rate, jint channels, jbyteArray csd)
{
    if (format != 1 || rate <= 0 || channels <= 0)
        return 0;
    try {
        Apartment apartment;
        auto decoder = std::make_unique<Decoder>();
        decoder->sampleRate = rate;
        decoder->channels = channels;
        auto config = Bytes(env, csd);
        if (config.empty()) {
            constexpr int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
            const auto index = std::find(std::begin(rates), std::end(rates), rate) - std::begin(rates);
            if (index >= std::size(rates) || channels > 6)
                return 0;
            const auto asc = (2 << 11) | (index << 7) | (channels << 3);
            config = {static_cast<uint8_t>(asc >> 8), static_cast<uint8_t>(asc)};
        }
        const HRESULT status = Open(*decoder, CLSID_CMSAACDecMFT, MFMediaType_Audio, MFAudioFormat_AAC, config);
        if (SUCCEEDED(status))
            return reinterpret_cast<jlong>(decoder.release());
        std::fprintf(stderr, "[win32-media] AAC open failed (0x%08lx)\n", status);
    }
    catch (const std::bad_alloc&) {
    }
    return 0;
}

jlong JNICALL OpenVideo(JNIEnv* env, jclass, jstring mime, jint width, jint height, jbyteArray csd0, jbyteArray csd1,
                        jboolean)
{
    if (!IsAvc(env, mime) || width < 2 || height < 2 || width > 4096 || height > 4096)
        return 0;
    try {
        Apartment apartment;
        auto decoder = std::make_unique<Decoder>();
        decoder->video = true;
        decoder->width = width;
        decoder->height = height;
        decoder->headers = Bytes(env, csd0);
        const auto spsExtension = Bytes(env, csd1);
        decoder->headers.insert(decoder->headers.end(), spsExtension.begin(), spsExtension.end());
        const HRESULT status = Open(*decoder, CLSID_CMSH264DecoderMFT, MFMediaType_Video, MFVideoFormat_H264);
        if (SUCCEEDED(status))
            return reinterpret_cast<jlong>(decoder.release());
        std::fprintf(stderr, "[win32-media] H.264 open failed (0x%08lx)\n", status);
    }
    catch (const std::bad_alloc&) {
    }
    return 0;
}

jbyteArray JNICALL DecodeAudio(JNIEnv* env, jclass, jlong handle, jbyteArray data, jlong pts, jboolean eos,
                               jlongArray time)
{
    try {
        Apartment apartment;
        AudioOutput audio{{}, pts};
        const auto status = reinterpret_cast<Decoder*>(handle)->Decode(Bytes(env, data), pts, eos, &audio);
        if (FAILED(status)) {
            std::fprintf(stderr, "[win32-media] AAC decode failed (0x%08lx)\n", status);
            return nullptr;
        }
        const auto size = static_cast<jsize>(audio.pcm.size());
        auto result = env->NewByteArray(size);
        if (!result)
            return nullptr;
        if (size)
            env->SetByteArrayRegion(result, 0, size, reinterpret_cast<const jbyte*>(audio.pcm.data()));
        env->SetLongArrayRegion(time, 0, 1, &audio.pts);
        return result;
    }
    catch (const std::bad_alloc&) {
        return nullptr;
    }
}

jint JNICALL DecodeVideo(JNIEnv* env, jclass, jlong handle, jbyteArray data, jlong pts, jboolean eos)
{
    try {
        Apartment apartment;
        const auto status = reinterpret_cast<Decoder*>(handle)->Decode(Bytes(env, data), pts, eos, nullptr);
        if (FAILED(status))
            std::fprintf(stderr, "[win32-media] H.264 decode failed (0x%08lx)\n", status);
        return status;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
}

jlongArray JNICALL PollVideo(JNIEnv* env, jclass, jlong handle)
{
    auto& decoder = *reinterpret_cast<Decoder*>(handle);
    if (decoder.videoOutput.empty() && !decoder.ended)
        return nullptr;
    auto result = env->NewLongArray(3);
    if (!result)
        return nullptr;
    jlong info[] = {0, decoder.endPts, 4}; // BUFFER_FLAG_END_OF_STREAM
    if (decoder.videoOutput.empty()) {
        decoder.ended = false;
    }
    else {
        auto* frame = new (std::nothrow) VideoFrame(std::move(decoder.videoOutput.front()));
        if (!frame)
            return nullptr;
        decoder.videoOutput.pop_front();
        info[0] = reinterpret_cast<jlong>(frame);
        info[1] = frame->pts;
        info[2] = 0;
    }
    env->SetLongArrayRegion(result, 0, 3, info);
    return result;
}

void JNICALL RenderFrame(JNIEnv* env, jclass, jlong handle, jobject surface)
{
    if (!surface)
        return;
    try {
        Apartment apartment;
        const auto& frame = *reinterpret_cast<VideoFrame*>(handle);
        std::vector<jint> pixels;
        if (!FrameArgb(frame, pixels))
            return;
        const auto count = static_cast<jsize>(pixels.size());
        auto array = env->NewIntArray(count);
        if (!array)
            return;
        env->SetIntArrayRegion(array, 0, count, pixels.data());
        const auto method = env->GetMethodID(env->GetObjectClass(surface), "stonePostDecodedFrame", "([III)V");
        if (method && !env->ExceptionCheck())
            env->CallVoidMethod(surface, method, array, static_cast<jint>(frame.width),
                                static_cast<jint>(frame.height));
    }
    catch (const std::bad_alloc&) {
    }
}

jint JNICALL ReadFrame(JNIEnv* env, jclass, jlong handle, jbyteArray output, jintArray geometry)
{
    try {
        Apartment apartment;
        const auto& frame = *reinterpret_cast<VideoFrame*>(handle);
        const auto size = static_cast<jsize>(frame.width * frame.height * sizeof(jint));
        if (env->GetArrayLength(output) < size || env->GetArrayLength(geometry) < 3)
            return -1;
        std::vector<jint> pixels;
        if (!FrameArgb(frame, pixels))
            return -1;
        env->SetByteArrayRegion(output, 0, size, reinterpret_cast<const jbyte*>(pixels.data()));
        const jint dimensions[] = {static_cast<jint>(frame.width), static_cast<jint>(frame.height),
                                   static_cast<jint>(frame.width * sizeof(jint))};
        env->SetIntArrayRegion(geometry, 0, 3, dimensions);
        return 0;
    }
    catch (const std::bad_alloc&) {
        return -1;
    }
}

void JNICALL Flush(JNIEnv*, jclass, jlong handle)
{
    Apartment apartment;
    reinterpret_cast<Decoder*>(handle)->Flush();
}

void JNICALL Close(JNIEnv*, jclass, jlong handle)
{
    Apartment apartment;
    delete reinterpret_cast<Decoder*>(handle);
}

template <typename Function> JNINativeMethod Native(const char* name, const char* signature, Function function)
{
    return {const_cast<char*>(name), const_cast<char*>(signature), reinterpret_cast<void*>(function)};
}
} // namespace

bool RegisterWindowsMediaDecoder(JNIEnv* env)
{
    const JNINativeMethod methods[] = {
        Native("nativeWindowsAacAvailable", "()Z", AacAvailable),
        Native("nativeDecOpen", "(III[B)J", OpenAudio),
        Native("nativeWindowsDecDecode", "(J[BJZ[J)[B", DecodeAudio),
        Native(
            "nativeDecOutSampleRate", "(J)I",
            +[](JNIEnv*, jclass, jlong handle) -> jint {
                return reinterpret_cast<Decoder*>(handle)->sampleRate;
            }),
        Native(
            "nativeDecOutChannels", "(J)I",
            +[](JNIEnv*, jclass, jlong handle) -> jint {
                return reinterpret_cast<Decoder*>(handle)->channels;
            }),
        Native("nativeDecClose", "(J)V", Close),
        Native("nativeWindowsDecFlush", "(J)V", Flush),
        Native("nativeVideoSupportedMimes", "()[Ljava/lang/String;", VideoMimes),
        Native("nativeVidOpen", "(Ljava/lang/String;II[B[BZ)J", OpenVideo),
        Native("nativeVidDecode", "(J[BJZ)I", DecodeVideo),
        Native("nativeVidPoll", "(J)[J", PollVideo),
        Native(
            "nativeWindowsVideoCanAcceptInput", "(J)Z",
            +[](JNIEnv*, jclass, jlong handle) -> jboolean {
                return reinterpret_cast<Decoder*>(handle)->videoOutput.size() < 8;
            }),
        Native("nativeWindowsVidRender", "(JLandroid/view/Surface;)V", RenderFrame),
        Native("nativeVidFrameBgra", "(J[B[I)I", ReadFrame),
        Native(
            "nativeVidReleaseFrame", "(J)V",
            +[](JNIEnv*, jclass, jlong handle) {
                Apartment apartment;
                delete reinterpret_cast<VideoFrame*>(handle);
            }),
        Native("nativeVidFlush", "(J)V", Flush),
        Native("nativeVidClose", "(J)V", Close),
    };
    const auto type = env->FindClass("com/trebledot/stone/loader/NativeMedia");
    return type && env->RegisterNatives(type, methods, static_cast<jint>(std::size(methods))) == JNI_OK;
}
} // namespace stone::windows
