#include "../../../host/audio/WaveOutTrack.h"
#include <jni.h>
#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace stone::windows {
namespace {
constexpr jint Stopped = 1, Paused = 2, Playing = 3;
constexpr jint BadValue = -2;

jlong JNICALL Create(JNIEnv*, jclass, jint rate, jint channels, jint encoding, jint frames)
{
    return waveout::Create(rate, channels, encoding, frames);
}

void JNICALL Release(JNIEnv*, jclass, jlong handle)
{
    waveout::Release(handle);
}

template <typename Array, typename Sample, auto GetRegion>
jint Write(JNIEnv* env, jlong handle, Array data, jint offset, jint count, bool blocking)
{
    if (!data || offset < 0 || count < 0 || offset > env->GetArrayLength(data) - count)
        return BadValue;
    return waveout::Write(handle, offset, count, sizeof(Sample), blocking, [&](void* target, int start, int size) {
        (env->*GetRegion)(data, start, size, reinterpret_cast<Sample*>(target));
    });
}

jboolean JNICALL Timestamp(JNIEnv* env, jclass, jlong handle, jlongArray output)
{
    if (!output || env->GetArrayLength(output) < 2)
        return JNI_FALSE;
    const auto position = waveout::Read(handle, waveout::Query::Position);
    if (!position)
        return JNI_FALSE;
    const auto system = env->FindClass("java/lang/System");
    const auto nanos = system ? env->GetStaticMethodID(system, "nanoTime", "()J") : nullptr;
    if (!nanos)
        return JNI_FALSE;
    const jlong stamp[] = {static_cast<jlong>(position), env->CallStaticLongMethod(system, nanos)};
    env->SetLongArrayRegion(output, 0, 2, stamp);
    return JNI_TRUE;
}

template <typename Function> JNINativeMethod Native(const char* name, const char* signature, Function function)
{
    return {const_cast<char*>(name), const_cast<char*>(signature), reinterpret_cast<void*>(function)};
}
} // namespace

bool RegisterWindowsAudioTrack(JNIEnv* env)
{
    const JNINativeMethod methods[] = {
        Native("nativeCreate", "(IIII)J", Create),
        Native(
            "nativeWriteBytes", "(J[BIIZ)I",
            +[](JNIEnv* e, jclass, jlong h, jbyteArray data, jint offset, jint count, jboolean block) {
                return Write<jbyteArray, jbyte, &JNIEnv::GetByteArrayRegion>(e, h, data, offset, count, block);
            }),
        Native(
            "nativeWriteShorts", "(J[SIIZ)I",
            +[](JNIEnv* e, jclass, jlong h, jshortArray data, jint offset, jint count, jboolean block) {
                return Write<jshortArray, jshort, &JNIEnv::GetShortArrayRegion>(e, h, data, offset, count, block);
            }),
        Native(
            "nativeWriteFloats", "(J[FIIZ)I",
            +[](JNIEnv* e, jclass, jlong h, jfloatArray data, jint offset, jint count, jboolean block) {
                return Write<jfloatArray, jfloat, &JNIEnv::GetFloatArrayRegion>(e, h, data, offset, count, block);
            }),
        Native(
            "nativePlay", "(J)V",
            +[](JNIEnv*, jclass, jlong h) {
                waveout::ChangeState(h, Playing);
            }),
        Native(
            "nativePause", "(J)V",
            +[](JNIEnv*, jclass, jlong h) {
                waveout::ChangeState(h, Paused);
            }),
        Native(
            "nativeStop", "(J)V",
            +[](JNIEnv*, jclass, jlong h) {
                waveout::ChangeState(h, Stopped);
            }),
        Native(
            "nativeFlush", "(J)V",
            +[](JNIEnv*, jclass, jlong h) {
                waveout::Flush(h);
            }),
        Native("nativeRelease", "(J)V", Release),
        Native(
            "nativeSetVolume", "(JFF)V",
            +[](JNIEnv*, jclass, jlong h, jfloat left, jfloat right) {
                waveout::Volume(h, left, right);
            }),
        Native(
            "nativeGetPlayState", "(J)I",
            +[](JNIEnv*, jclass, jlong h) -> jint {
                return static_cast<jint>(waveout::Read(h, waveout::Query::State));
            }),
        Native(
            "nativeGetPlaybackHeadPosition", "(J)J",
            +[](JNIEnv*, jclass, jlong h) -> jlong {
                return waveout::Read(h, waveout::Query::Position);
            }),
        Native("nativeGetTimestamp", "(J[J)Z", Timestamp),
        Native(
            "nativeSetPlaybackRate", "(JI)I",
            +[](JNIEnv*, jclass, jlong h, jint rate) -> jint {
                return waveout::Rate(h, rate);
            }),
        Native(
            "nativeGetPlaybackRate", "(J)I",
            +[](JNIEnv*, jclass, jlong h) -> jint {
                return static_cast<jint>(waveout::Read(h, waveout::Query::Rate));
            }),
        Native(
            "nativeSetPlaybackParams", "(JFF)I",
            +[](JNIEnv*, jclass, jlong h, jfloat speed, jfloat pitch) -> jint {
                return waveout::Params(h, speed, pitch);
            }),
        Native(
            "nativeGetPlaybackParams", "(J[F)V",
            +[](JNIEnv* e, jclass, jlong h, jfloatArray output) {
                waveout::PlaybackParams(h, [&](float ratio) {
                    const jfloat params[]{ratio, ratio};
                    e->SetFloatArrayRegion(output, 0, 2, params);
                });
            }),
        Native(
            "nativeGetBufferCapacity", "(J)I",
            +[](JNIEnv*, jclass, jlong h) -> jint {
                return static_cast<jint>(waveout::Read(h, waveout::Query::Capacity));
            }),
        Native(
            "nativeSetBufferSize", "(JI)I",
            +[](JNIEnv*, jclass, jlong h, jint frames) -> jint {
                return waveout::BufferSize(h, frames);
            }),
        Native(
            "nativeGetUnderrunCount", "(J)I",
            +[](JNIEnv*, jclass, jlong h) -> jint {
                return static_cast<jint>(waveout::Read(h, waveout::Query::Underruns));
            }),
    };
    const auto type = env->FindClass("com/trebledot/stone/loader/NativeAudio");
    return type && env->RegisterNatives(type, methods, static_cast<jint>(std::size(methods))) == JNI_OK;
}
} // namespace stone::windows
