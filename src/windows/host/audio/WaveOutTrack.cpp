#include "WaveOutTrack.h"
#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <list>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace stone::windows::waveout {
namespace {
constexpr int Stopped = 1, Paused = 2, Playing = 3, BadValue = -2, InvalidOperation = -3, DeadObject = -6;

struct Buffer {
    std::vector<char> bytes;
    WAVEHDR header{};
};

struct Track {
    std::mutex mutex;
    HWAVEOUT device = nullptr;
    HANDLE changed = nullptr;
    std::list<Buffer> queue;
    int capacity = 0, limit = 0, frameBytes = 0, rate = 0, playbackRate = 0;
    int state = Stopped, underruns = 0;
    uint64_t written = 0, played = 0, completed = 0;
    bool starved = false, draining = false, closed = false;

    void Reap()
    {
        while (!queue.empty() && (queue.front().header.dwFlags & WHDR_DONE)) {
            auto& buffer = queue.front();
            waveOutUnprepareHeader(device, &buffer.header, sizeof(buffer.header));
            completed += buffer.bytes.size() / frameBytes;
            queue.pop_front();
        }
        if (state == Playing && written && queue.empty() && !starved) {
            ++underruns;
            starved = true;
        }
    }

    uint64_t Position()
    {
        Reap();
        MMTIME time{};
        time.wType = TIME_SAMPLES;
        if (waveOutGetPosition(device, &time, sizeof(time)) == MMSYSERR_NOERROR && time.wType == TIME_SAMPLES) {
            const auto delta = static_cast<uint32_t>(time.u.sample - static_cast<uint32_t>(played));
            if (delta <= written - played)
                played += delta;
        }
        played = std::max(played, completed);
        return played;
    }

    void Reset()
    {
        waveOutReset(device);
        Reap();
        written = played = completed = 0;
        starved = draining = false;
    }

    ~Track()
    {
        if (device) {
            Reset();
            waveOutClose(device);
        }
        if (changed)
            CloseHandle(changed);
    }
};

std::mutex tracksMutex;
std::unordered_map<int64_t, std::shared_ptr<Track>> tracks;
int64_t nextTrack = 1;

std::shared_ptr<Track> Find(int64_t handle)
{
    const std::lock_guard lock(tracksMutex);
    const auto track = tracks.find(handle);
    return track == tracks.end() ? nullptr : track->second;
}

template <typename Operation> auto WithTrack(int64_t handle, Operation operation)
{
    const auto track = Find(handle);
    if (track) {
        const std::lock_guard lock(track->mutex);
        if (!track->closed)
            return operation(*track);
    }
    if constexpr (!std::is_void_v<decltype(operation(*track))>)
        return decltype(operation(*track)){};
}

} // namespace

int64_t Create(int rate, int channels, int encoding, int frames)
{
    if (rate <= 0 || rate > 384000 || channels < 1 || channels > 8 || frames <= 0 || frames > rate * 10)
        return 0;
    try {
        auto track = std::make_shared<Track>();
        WAVEFORMATEXTENSIBLE format{};
        const WORD bits = encoding == 4 ? 32 : encoding == 3 ? 8 : 16;
        auto& base = format.Format;
        base.wFormatTag = channels > 2    ? WAVE_FORMAT_EXTENSIBLE
                          : encoding == 4 ? WAVE_FORMAT_IEEE_FLOAT
                                          : WAVE_FORMAT_PCM;
        base.nChannels = static_cast<WORD>(channels);
        base.nSamplesPerSec = rate;
        base.wBitsPerSample = bits;
        base.nBlockAlign = static_cast<WORD>(channels * (bits / 8));
        base.nAvgBytesPerSec = rate * base.nBlockAlign;
        if (channels > 2) {
            constexpr DWORD masks[] = {0, 0, 0, 0x7, 0x33, 0x37, 0x3f, 0x70f, 0x63f};
            base.cbSize = sizeof(format) - sizeof(base);
            format.Samples.wValidBitsPerSample = bits;
            format.dwChannelMask = masks[channels];
            format.SubFormat = {encoding == 4 ? 3UL : 1UL, 0, 0x10, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        }
        track->changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!track->changed ||
            waveOutOpen(&track->device, WAVE_MAPPER, &base, reinterpret_cast<DWORD_PTR>(track->changed), 0,
                        CALLBACK_EVENT) != MMSYSERR_NOERROR)
            return 0;
        if (waveOutPause(track->device) != MMSYSERR_NOERROR)
            return 0;
        track->rate = track->playbackRate = rate;
        track->frameBytes = base.nBlockAlign;
        track->capacity = track->limit = frames;
        const std::lock_guard lock(tracksMutex);
        const auto handle = nextTrack++;
        tracks.emplace(handle, std::move(track));
        return handle;
    }
    catch (const std::bad_alloc&) {
        return 0;
    }
}

int Write(int64_t handle, int offset, int count, int unit, bool blocking, const CopySamples& copy)
{
    auto track = Find(handle);
    if (!track)
        return DeadObject;
    const int unitsPerFrame = track->frameBytes / unit;
    if (!unitsPerFrame || track->frameBytes % unit)
        return BadValue;
    int consumed = 0;
    std::unique_lock lock(track->mutex);
    const int initialState = track->state;
    while (count - consumed >= unitsPerFrame) {
        if (track->closed)
            return consumed ? consumed : DeadObject;
        if (track->state != initialState)
            break;
        if (track->draining) {
            track->Reset();
            waveOutPause(track->device);
        }
        const auto occupied = track->written - track->Position();
        if (occupied >= static_cast<uint64_t>(track->limit)) {
            if (!blocking || track->state != Playing)
                break;
            lock.unlock();
            WaitForSingleObject(track->changed, 20);
            lock.lock();
            continue;
        }
        const auto frames = std::min((count - consumed) / unitsPerFrame, static_cast<int>(track->limit - occupied));
        const auto size = frames * unitsPerFrame;
        try {
            auto& buffer = track->queue.emplace_back();
            try {
                buffer.bytes.resize(static_cast<size_t>(size) * unit);
            }
            catch (const std::bad_alloc&) {
                track->queue.pop_back();
                throw;
            }
            copy(buffer.bytes.data(), offset + consumed, size);
            buffer.header.lpData = buffer.bytes.data();
            buffer.header.dwBufferLength = static_cast<DWORD>(buffer.bytes.size());
            if (waveOutPrepareHeader(track->device, &buffer.header, sizeof(buffer.header)) != MMSYSERR_NOERROR ||
                waveOutWrite(track->device, &buffer.header, sizeof(buffer.header)) != MMSYSERR_NOERROR) {
                if (buffer.header.dwFlags & WHDR_PREPARED)
                    waveOutUnprepareHeader(track->device, &buffer.header, sizeof(buffer.header));
                track->queue.pop_back();
                return consumed ? consumed : DeadObject;
            }
            consumed += size;
            track->written += frames;
            track->starved = false;
        }
        catch (const std::bad_alloc&) {
            return consumed ? consumed : InvalidOperation;
        }
        if (!blocking)
            break;
    }
    return consumed;
}

void Release(int64_t handle)
{
    std::shared_ptr<Track> track;
    {
        const std::lock_guard lock(tracksMutex);
        const auto found = tracks.find(handle);
        if (found == tracks.end())
            return;
        track = std::move(found->second);
        tracks.erase(found);
    }
    const std::lock_guard lock(track->mutex);
    track->closed = true;
    SetEvent(track->changed);
}

void ChangeState(int64_t handle, int state)
{
    WithTrack(handle, [state](Track& track) {
        if (track.state == state)
            return;
        if (track.draining)
            track.Reset();
        if (state == Playing)
            waveOutRestart(track.device);
        if (state == Paused)
            waveOutPause(track.device);
        if (state == Stopped && track.state == Paused)
            track.Reset();
        // A playing stream drains its submitted samples on stop.
        track.draining = state == Stopped && track.state == Playing;
        track.state = state;
        SetEvent(track.changed);
    });
}

int SetRate(Track& track, int rate)
{
    if (rate <= 0 || rate > track.rate * 8)
        return BadValue;
    if (rate == track.playbackRate)
        return 0;
    const auto fraction = static_cast<DWORD>(static_cast<uint64_t>(rate) * 65536 / track.rate);
    if (waveOutSetPlaybackRate(track.device, fraction) != MMSYSERR_NOERROR)
        return InvalidOperation;
    track.playbackRate = rate;
    return 0;
}

void Flush(int64_t h)
{
    WithTrack(h, [](Track& track) {
        if (track.state == Playing)
            return;
        track.Reset();
        waveOutPause(track.device);
        SetEvent(track.changed);
    });
}

void Volume(int64_t h, float left, float right)
{
    WithTrack(h, [&](Track& track) {
        const auto volume = [](float value) -> DWORD {
            return std::isfinite(value) ? static_cast<DWORD>(std::clamp(value, 0.0f, 1.0f) * 65535) : 0;
        };
        waveOutSetVolume(track.device, volume(left) | (volume(right) << 16));
    });
}

int64_t Read(int64_t h, Query query)
{
    return WithTrack(h, [=](Track& track) -> int64_t {
        switch (query) {
        case Query::State:
            return track.state;
        case Query::Position:
            return track.state == Stopped ? 0 : track.Position();
        case Query::Rate:
            return track.playbackRate;
        case Query::Capacity:
            return track.capacity;
        case Query::Underruns:
            track.Reap();
            return track.underruns;
        }
        return 0;
    });
}

int Rate(int64_t h, int rate)
{
    return WithTrack(h, [=](Track& track) {
        return SetRate(track, rate);
    });
}

int Params(int64_t h, float speed, float pitch)
{
    return WithTrack(h, [=](Track& track) {
        if (!std::isfinite(speed) || speed <= 0 || speed > 8 || speed != pitch)
            return BadValue;
        return SetRate(track, static_cast<int>(track.rate * speed));
    });
}

void PlaybackParams(int64_t h, const std::function<void(float)>& copy)
{
    WithTrack(h, [&](Track& track) {
        copy(static_cast<float>(track.playbackRate) / track.rate);
    });
}

int BufferSize(int64_t h, int frames)
{
    return WithTrack(h, [=](Track& track) {
        return track.limit = std::clamp(frames, 1, track.capacity);
    });
}
} // namespace stone::windows::waveout
