#pragma once

#include <cstdint>
#include <functional>

namespace stone::windows {
// PCM streams belong to the Client, not the broker or an Android audio server.
int64_t CreateWindowsAudio(int rate, int channels, int encoding, int frames);
int WriteWindowsAudio(int64_t handle, int bytes, bool blocking, const std::function<bool(void*, int, int)>& copy);
void ReleaseWindowsAudio(int64_t handle);
void StateWindowsAudio(int64_t handle, int state);
void FlushWindowsAudio(int64_t handle);
bool VolumeWindowsAudio(int64_t handle, float left, float right);
enum class AudioQuery {
    Capacity,
    Size,
    Rate,
    Position,
    Written,
    Underruns,
    State,
    Latency
};
int64_t QueryWindowsAudio(int64_t handle, AudioQuery query);
int SizeWindowsAudio(int64_t handle, int frames);
int RateWindowsAudio(int64_t handle, int rate);
int NewWindowsAudioSession();
bool WindowsAudioAvailable();
}
