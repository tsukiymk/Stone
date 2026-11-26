#pragma once

#include <cstdint>
#include <functional>

namespace stone::windows::waveout {
using CopySamples = std::function<void(void*, int, int)>;
int64_t Create(int rate, int channels, int encoding, int frames);
int Write(int64_t handle, int offset, int count, int unit, bool blocking, const CopySamples& copy);
void Release(int64_t handle);
void ChangeState(int64_t handle, int state);
void Flush(int64_t handle);
void Volume(int64_t handle, float left, float right);
enum class Query {
    State,
    Position,
    Rate,
    Capacity,
    Underruns
};
int64_t Read(int64_t handle, Query query);
int Rate(int64_t handle, int rate);
int Params(int64_t handle, float speed, float pitch);
void PlaybackParams(int64_t handle, const std::function<void(float)>& copy);
int BufferSize(int64_t handle, int frames);
} // namespace stone::windows::waveout
