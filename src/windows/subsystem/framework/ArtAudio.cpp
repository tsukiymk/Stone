#include "ArtAudio.h"
#include "../runtime/art/ArtJni.h"
#include "../abi/AndroidLibraryImports.h"
#include "../abi/TypedAndroidImport.h"
#include "../../host/audio/AudioOutput.h"
#include <windows.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <string>

namespace stone::windows {
namespace {
jfieldID nativeTrack;

jlong Handle(JNIEnv* env, jobject self)
{
    return ArtJni<&JNINativeInterface_::GetLongField>(env, self, nativeTrack);
}

void Throw(JNIEnv* env, const char* message)
{
    auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/UnsupportedOperationException");
    if (type) {
        ArtJni<&JNINativeInterface_::ThrowNew>(env, type, message);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    }
}

jint Setup(JNIEnv* env, jobject self, jobject, jobject, jintArray rates, jobject masks, jint encoding, jint bytes,
           jint mode, jintArray sessions, jobject, jlong existing, jboolean offload, jint encapsulation, jobject,
           jstring, jstring)
{
    // Shared-memory/static tracks, compressed offload and image native pointers
    // must never reach a Windows PCM handle.
    if (mode != 1 || existing || offload || encapsulation || !rates || !sessions ||
        ArtJni<&JNINativeInterface_::GetArrayLength>(env, rates) != 1 ||
        ArtJni<&JNINativeInterface_::GetArrayLength>(env, sessions) != 1)
        return -20;
    jint rate = 0, session = 0;
    ArtJni<&JNINativeInterface_::GetIntArrayRegion>(env, rates, 0, 1, &rate);
    ArtJni<&JNINativeInterface_::GetIntArrayRegion>(env, sessions, 0, 1, &session);
    auto type = ArtJni<&JNINativeInterface_::GetObjectClass>(env, self);
    auto method = ArtJni<&JNINativeInterface_::GetMethodID>(env, type, "getChannelCount", "()I");
    int channels = method ? ArtJni<&JNINativeInterface_::CallIntMethodA>(env, self, method, nullptr) : 0;
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
        return -20;
    if (!masks)
        return -20;
    auto maskType = ArtJni<&JNINativeInterface_::GetObjectClass>(env, masks);
    auto positionMethod = ArtJni<&JNINativeInterface_::GetMethodID>(env, maskType, "getPositionMask", "()I");
    auto acnMethod = ArtJni<&JNINativeInterface_::GetMethodID>(env, maskType, "getAcnMask", "()I");
    int positionMask =
        positionMethod ? ArtJni<&JNINativeInterface_::CallIntMethodA>(env, masks, positionMethod, nullptr) : -1;
    int acnMask = acnMethod ? ArtJni<&JNINativeInterface_::CallIntMethodA>(env, masks, acnMethod, nullptr) : -1;
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, maskType);
    if (acnMask || (positionMask != 0 && positionMask != 4 && positionMask != 12))
        return -20;
    if (!rate)
        rate = 48000;
    int sampleBytes = encoding == 2 ? 2 : encoding == 3 ? 1 : encoding == 4 ? 4 : 0;
    if (!sampleBytes || channels < 1 || channels > 2 || bytes <= 0 || bytes % (sampleBytes * channels) || session < 0)
        return -20;
    if (!session)
        session = NewWindowsAudioSession();
    if (session <= 0)
        return -20;
    auto handle = CreateWindowsAudio(rate, channels, encoding, bytes / (sampleBytes * channels));
    if (!handle)
        return -20;
    ArtJni<&JNINativeInterface_::SetLongField>(env, self, nativeTrack, handle);
    ArtJni<&JNINativeInterface_::SetIntArrayRegion>(env, rates, 0, 1, &rate);
    ArtJni<&JNINativeInterface_::SetIntArrayRegion>(env, sessions, 0, 1, &session);
    return 0;
}

void Release(JNIEnv* env, jobject self)
{
    auto handle = Handle(env, self);
    ArtJni<&JNINativeInterface_::SetLongField>(env, self, nativeTrack, 0);
    ReleaseWindowsAudio(handle);
}

template <class Array, class Sample, auto GetRegion>
jint Write(JNIEnv* env, jobject self, Array array, jint offset, jint count, jint encoding, jboolean blocking)
{
    if constexpr (sizeof(Sample) == 2) {
        if (encoding != 2)
            return -2;
    }
    if constexpr (sizeof(Sample) == 4) {
        if (encoding != 4)
            return -2;
    }
    if (!array || offset < 0 || count < 0 ||
        offset > ArtJni<&JNINativeInterface_::GetArrayLength>(env, array) - count ||
        count > INT_MAX / int(sizeof(Sample)))
        return -2;
    int result = WriteWindowsAudio(Handle(env, self), count * int(sizeof(Sample)), blocking != 0,
                                   [&](void* output, int start, int size) {
                                       ArtJni<GetRegion>(env, array, offset + start / int(sizeof(Sample)),
                                                         size / int(sizeof(Sample)), static_cast<Sample*>(output));
                                       return !ArtJni<&JNINativeInterface_::ExceptionCheck>(env);
                                   });
    return result < 0 ? result : result / int(sizeof(Sample));
}

jint WriteDirect(JNIEnv* env, jobject self, jobject data, jint offset, jint count, jint, jboolean blocking)
{
    if (!data || offset < 0 || count < 0 || IsArm64TranslationEnabled())
        return -2;
    auto size = ArtJni<&JNINativeInterface_::GetDirectBufferCapacity>(env, data);
    auto* address = static_cast<const char*>(ArtJni<&JNINativeInterface_::GetDirectBufferAddress>(env, data));
    if (!address || int64_t(offset) + count > size)
        return -2;
    return WriteWindowsAudio(Handle(env, self), count, blocking != 0, [&](void* output, int start, int bytes) {
        std::memcpy(output, address + offset + start, bytes);
        return true;
    });
}

jint Timestamp(JNIEnv* env, jobject self, jlongArray output)
{
    if (!output || ArtJni<&JNINativeInterface_::GetArrayLength>(env, output) < 2)
        return -2;
    if (!QueryWindowsAudio(Handle(env, self), AudioQuery::State))
        return -6;
    auto position = QueryWindowsAudio(Handle(env, self), AudioQuery::Position);
    if (!position)
        return -3;
    // System.nanoTime uses the same monotonic clock as the framework client.
    auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/System");
    auto method = ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "nanoTime", "()J");
    jlong values[]{position, ArtJni<&JNINativeInterface_::CallStaticLongMethodA>(env, type, method, nullptr)};
    ArtJni<&JNINativeInterface_::SetLongArrayRegion>(env, output, 0, 2, values);
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    return 0;
}

template <class F> JNINativeMethod Method(const char* name, const char* signature, F function)
{
    return {const_cast<char*>(name), const_cast<char*>(signature),
            reinterpret_cast<void*>(ImportAndroidScalars<F>(std::string("ART Windows audio ") + name + signature,
                                                            reinterpret_cast<uintptr_t>(function)))};
}

template <size_t N> bool Bind(JNIEnv* env, const char* name, const JNINativeMethod (&methods)[N])
{
    auto type = ArtJni<&JNINativeInterface_::FindClass>(env, name);
    if (!type)
        return false;
    bool ok = ArtJni<&JNINativeInterface_::RegisterNatives>(env, type, methods, int(N)) == 0;
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    return ok;
}
}

bool RegisterArtAudio(JNIEnv* env)
{
    const JNINativeMethod host[]{Method(
        "available", "()Z", +[](JNIEnv*, jclass) -> jboolean {
            return !IsArm64TranslationEnabled() && WindowsAudioAvailable();
        })};
    if (!Bind(env, "com/trebledot/stone/runtime/ArtAudio", host))
        return false;
    if (IsArm64TranslationEnabled())
        return true;
    auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "android/media/AudioTrack");
    nativeTrack = type ? ArtJni<&JNINativeInterface_::GetFieldID>(env, type, "mNativeTrackInJavaObj", "J") : nullptr;
    if (type)
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    if (!nativeTrack)
        return false;
    const JNINativeMethod methods[]{
        Method(
            "native_setup",
            "(Ljava/lang/Object;Ljava/lang/Object;[ILjava/lang/Object;III[ILandroid/os/Parcel;JZILjava/lang/Object;Ljava/lang/String;Ljava/lang/String;)I",
            Setup),
        Method("native_release", "()V", Release),
        Method("native_finalize", "()V", Release),
        Method(
            "native_start", "()V",
            +[](JNIEnv* e, jobject o) {
                StateWindowsAudio(Handle(e, o), 3);
            }),
        Method(
            "native_pause", "()V",
            +[](JNIEnv* e, jobject o) {
                StateWindowsAudio(Handle(e, o), 2);
            }),
        Method(
            "native_stop", "()V",
            +[](JNIEnv* e, jobject o) {
                StateWindowsAudio(Handle(e, o), 1);
            }),
        Method(
            "native_flush", "()V",
            +[](JNIEnv* e, jobject o) {
                FlushWindowsAudio(Handle(e, o));
            }),
        Method("native_write_byte", "([BIIIZ)I", Write<jbyteArray, jbyte, &JNINativeInterface_::GetByteArrayRegion>),
        Method("native_write_short", "([SIIIZ)I",
               Write<jshortArray, jshort, &JNINativeInterface_::GetShortArrayRegion>),
        Method("native_write_float", "([FIIIZ)I",
               Write<jfloatArray, jfloat, &JNINativeInterface_::GetFloatArrayRegion>),
        Method("native_write_native_bytes", "(Ljava/nio/ByteBuffer;IIIZ)I", WriteDirect),
        Method(
            "native_get_min_buff_size", "(III)I",
            +[](JNIEnv*, jclass, jint rate, jint channels, jint format) -> jint {
                int unit = format == 2 ? 2 : format == 3 ? 1 : format == 4 ? 4 : 0;
                return unit && rate >= 8000 && rate <= 192000 && channels >= 1 && channels <= 2 &&
                               WindowsAudioAvailable()
                           ? ((rate + 9) / 10) * channels * unit
                           : -1;
            }),
        Method(
            "native_get_output_sample_rate", "(I)I",
            +[](JNIEnv*, jclass, jint) -> jint {
                return WindowsAudioAvailable() ? 48000 : 0;
            }),
        Method(
            "native_setVolume", "(FF)V",
            +[](JNIEnv* e, jobject o, jfloat l, jfloat r) {
                if (!VolumeWindowsAudio(Handle(e, o), l, r))
                    Throw(e, "Windows audio device does not support stream volume");
            }),
#define QUERY(Name, Kind)                                                                                              \
    Method(                                                                                                            \
        Name, "()I", +[](JNIEnv* e, jobject o) -> jint {                                                               \
            return static_cast<jint>(QueryWindowsAudio(Handle(e, o), AudioQuery::Kind));                               \
        })
        QUERY("native_get_buffer_capacity_frames", Capacity),
        QUERY("native_get_buffer_size_frames", Size),
        QUERY("native_get_playback_rate", Rate),
        QUERY("native_get_position", Position),
        QUERY("native_get_underrun_count", Underruns),
        QUERY("native_get_latency", Latency),
#undef QUERY
        Method(
            "native_get_written_frames_count", "()J",
            +[](JNIEnv* e, jobject o) -> jlong {
                return QueryWindowsAudio(Handle(e, o), AudioQuery::Written);
            }),
        Method("native_get_timestamp", "([J)I", Timestamp),
        Method(
            "native_set_buffer_size_frames", "(I)I",
            +[](JNIEnv* e, jobject o, jint n) -> jint {
                return SizeWindowsAudio(Handle(e, o), n);
            }),
        Method(
            "native_set_playback_rate", "(I)I",
            +[](JNIEnv* e, jobject o, jint rate) -> jint {
                return RateWindowsAudio(Handle(e, o), rate);
            }),
        Method(
            "native_get_flags", "()I",
            +[](JNIEnv*, jobject) -> jint {
                return 0;
            }),
        Method(
            "native_setPlayerIId", "(I)V",
            +[](JNIEnv*, jobject, jint) {
            }),
        Method(
            "native_setLogSessionId", "(Ljava/lang/String;)V",
            +[](JNIEnv*, jobject, jstring) {
            }),
        Method(
            "native_enableDeviceCallback", "()V",
            +[](JNIEnv*, jobject) {
            }),
        Method(
            "native_disableDeviceCallback", "()V",
            +[](JNIEnv*, jobject) {
            }),
        Method(
            "native_setOutputDevice", "(I)Z",
            +[](JNIEnv*, jobject, jint id) -> jboolean {
                return id == 0 || id == 1;
            }),
        Method(
            "native_getRoutedDeviceIds", "()[I",
            +[](JNIEnv* e, jobject o) -> jintArray {
                bool playing = QueryWindowsAudio(Handle(e, o), AudioQuery::State) == 3;
                auto result = ArtJni<&JNINativeInterface_::NewIntArray>(e, playing ? 1 : 0);
                const jint id = 1;
                if (playing && result)
                    ArtJni<&JNINativeInterface_::SetIntArrayRegion>(e, result, 0, 1, &id);
                return result;
            }),
        Method(
            "native_getPortId", "()I",
            +[](JNIEnv*, jobject) -> jint {
                return 0;
            }),
        Method(
            "native_is_direct_output_supported", "(IILjava/lang/Object;III)Z",
            +[](JNIEnv*, jclass, jint, jint, jobject, jint, jint, jint) -> jboolean {
                return JNI_FALSE;
            }),
        Method(
            "native_getFlushWrittenFramesFromPositionSupport",
            "(IILandroid/media/AudioFormat$ChannelMasks;Landroid/media/AudioAttributes;)I",
            +[](JNIEnv*, jclass, jint, jint, jobject, jobject) -> jint {
                return 0;
            }),
        Method(
            "native_flushFromFrame", "(IJ)J",
            +[](JNIEnv*, jobject, jint, jlong) -> jlong {
                return -3;
            }),
        Method(
            "native_setPresentation", "(II)I",
            +[](JNIEnv*, jobject, jint, jint) -> jint {
                return -3;
            }),
        Method(
            "native_set_loop", "(III)I",
            +[](JNIEnv*, jobject, jint, jint, jint) -> jint {
                return -3;
            }),
        Method(
            "native_reload_static", "()I",
            +[](JNIEnv*, jobject) -> jint {
                return -3;
            }),
        Method(
            "native_set_position", "(I)I",
            +[](JNIEnv*, jobject, jint) -> jint {
                return -3;
            }),
        Method(
            "native_set_marker_pos", "(I)I",
            +[](JNIEnv*, jobject, jint) -> jint {
                return -3;
            }),
        Method(
            "native_get_marker_pos", "()I",
            +[](JNIEnv*, jobject) -> jint {
                return 0;
            }),
        Method(
            "native_set_pos_update_period", "(I)I",
            +[](JNIEnv*, jobject, jint) -> jint {
                return -3;
            }),
        Method(
            "native_get_pos_update_period", "()I",
            +[](JNIEnv*, jobject) -> jint {
                return 0;
            }),
        Method(
            "native_attachAuxEffect", "(I)I",
            +[](JNIEnv*, jobject, jint id) -> jint {
                return id == 0 ? 0 : -3;
            }),
        Method(
            "native_setAuxEffectSendLevel", "(F)I",
            +[](JNIEnv*, jobject, jfloat) -> jint {
                return -3;
            }),
        Method(
            "native_applyVolumeShaper",
            "(Landroid/media/VolumeShaper$Configuration;Landroid/media/VolumeShaper$Operation;)I",
            +[](JNIEnv*, jobject, jobject, jobject) -> jint {
                return -3;
            }),
        Method(
            "native_getVolumeShaperState", "(I)Landroid/media/VolumeShaper$State;",
            +[](JNIEnv*, jobject, jint) -> jobject {
                return nullptr;
            }),
        Method(
            "native_getMetrics", "()Landroid/os/PersistableBundle;",
            +[](JNIEnv*, jobject) -> jobject {
                return nullptr;
            }),
        Method(
            "native_set_playback_params", "(Landroid/media/PlaybackParams;)V",
            +[](JNIEnv* e, jobject, jobject) {
                Throw(e, "AudioTrack time stretching is not supported");
            }),
        Method(
            "native_get_playback_params", "()Landroid/media/PlaybackParams;",
            +[](JNIEnv* e, jobject) -> jobject {
                Throw(e, "AudioTrack time stretching is not supported");
                return nullptr;
            }),
        Method(
            "native_set_delay_padding", "(II)V",
            +[](JNIEnv* e, jobject, jint, jint) {
                Throw(e, "AudioTrack offload is not supported");
            }),
        Method(
            "native_set_dual_mono_mode", "(I)I",
            +[](JNIEnv*, jobject, jint) -> jint {
                return -3;
            }),
        Method(
            "native_get_dual_mono_mode", "([I)I",
            +[](JNIEnv*, jobject, jintArray) -> jint {
                return -3;
            }),
        Method(
            "native_set_audio_description_mix_level_db", "(F)I",
            +[](JNIEnv*, jobject, jfloat) -> jint {
                return -3;
            }),
        Method(
            "native_get_audio_description_mix_level_db", "([F)I",
            +[](JNIEnv*, jobject, jfloatArray) -> jint {
                return -3;
            }),
        Method(
            "native_setStartThresholdInFrames", "(I)I",
            +[](JNIEnv*, jobject, jint) -> jint {
                return -3;
            }),
        Method(
            "native_getStartThresholdInFrames", "()I",
            +[](JNIEnv*, jobject) -> jint {
                return -3;
            }),
    };
    if (!Bind(env, "android/media/AudioTrack", methods))
        return false;
    const JNINativeMethod system[]{Method(
        "listAudioPorts", "(Ljava/util/ArrayList;[I)I",
        +[](JNIEnv* e, jclass, jobject list, jintArray generation) -> jint {
            auto type = ArtJni<&JNINativeInterface_::FindClass>(e, "com/trebledot/stone/runtime/ArtAudio");
            auto method =
                ArtJni<&JNINativeInterface_::GetStaticMethodID>(e, type, "ports", "(Ljava/util/ArrayList;[I)I");
            jvalue args[]{jvalue{.l = list}, jvalue{.l = generation}};
            jint result = method ? ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(e, type, method, args) : -1;
            ArtJni<&JNINativeInterface_::DeleteLocalRef>(e, type);
            return result;
        })};
    return Bind(env, "android/media/AudioSystem", system);
}
}
