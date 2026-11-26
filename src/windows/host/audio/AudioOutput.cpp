#include "AudioOutput.h"
#include <windows.h>
#include <sddl.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace stone::windows {
namespace {
using Microsoft::WRL::ComPtr;
constexpr int Stopped = 1, Paused = 2, Playing = 3;
constexpr int BadValue = -2, InvalidOperation = -3, DeadObject = -6;

struct Buffer {
    std::vector<float> samples;
    size_t offset = 0;
};

struct Track {
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    std::deque<Buffer> queue;
    int rate = 0, channels = 0, encoding = 0, frameBytes = 0, capacity = 0, limit = 0;
    int state = Stopped, playbackRate = 0, requestedRate = 0, rateResult = 0, underruns = 0, latencyMillis = 0;
    uint64_t written = 0, submitted = 0, played = 0, generation = 0, command = 0, done = 0;
    float left = 1, right = 1;
    bool initialized = false, ready = false, closed = false, dead = false, reset = false, draining = false,
         starved = false, volumeOk = true;

    void Run()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com)) {
            std::lock_guard lock(mutex);
            ready = true;
            dead = true;
            changed.notify_all();
            return;
        }
        {
            // Creation, rendering and destruction of WASAPI interfaces all run
            // on this MTA thread. Java writers never hold pinned arrays here.
            ComPtr<IMMDeviceEnumerator> enumerator;
            ComPtr<IMMDevice> endpoint;
            ComPtr<IAudioClient> client;
            ComPtr<IAudioRenderClient> render;
            ComPtr<IAudioClock> clock;
            ComPtr<IAudioStreamVolume> volume;
            ComPtr<IAudioClockAdjustment> adjustment;
            UINT32 deviceFrames = 0;
            UINT64 frequency = 0;
            HRESULT status =
                CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
            if (SUCCEEDED(status))
                status = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint);
            if (SUCCEEDED(status))
                status = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
            WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT,
                                static_cast<WORD>(channels),
                                static_cast<DWORD>(rate),
                                static_cast<DWORD>(rate * channels * 4),
                                static_cast<WORD>(channels * 4),
                                32,
                                0};
            if (SUCCEEDED(status))
                status =
                    client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                                           AUDCLNT_STREAMFLAGS_RATEADJUST,
                                       1000000, 0, &format, nullptr);
            if (SUCCEEDED(status))
                status = client->GetBufferSize(&deviceFrames);
            REFERENCE_TIME latency = 0;
            if (SUCCEEDED(status) && SUCCEEDED(client->GetStreamLatency(&latency)))
                latencyMillis = static_cast<int>((latency + 9999) / 10000);
            if (SUCCEEDED(status))
                status = client->GetService(IID_PPV_ARGS(&render));
            if (SUCCEEDED(status))
                status = client->GetService(IID_PPV_ARGS(&clock));
            if (SUCCEEDED(status))
                status = clock->GetFrequency(&frequency);
            if (SUCCEEDED(status))
                status = client->GetService(IID_PPV_ARGS(&volume));
            if (SUCCEEDED(status))
                client->GetService(IID_PPV_ARGS(&adjustment));
            std::unique_lock lock(mutex);
            initialized = SUCCEEDED(status) && frequency != 0;
            dead = !initialized;
            ready = true;
            changed.notify_all();
            bool running = false;
            int appliedState = Stopped;
            auto position = [&] {
                UINT64 ticks = 0;
                UINT32 padding = 0;
                if (SUCCEEDED(clock->GetPosition(&ticks, nullptr)) && SUCCEEDED(client->GetCurrentPadding(&padding))) {
                    // Count consumed application frames, excluding silence the
                    // shared engine inserts during underruns.
                    played = std::max(played, submitted - std::min<uint64_t>(submitted, padding));
                }
            };
            auto clear = [&] {
                HRESULT result = running ? client->Stop() : S_OK;
                running = false;
                if (SUCCEEDED(result))
                    result = client->Reset();
                if (SUCCEEDED(result)) {
                    queue.clear();
                    written = submitted = played = 0;
                    draining = starved = false;
                }
                return result;
            };
            while (!closed && !dead) {
                if (command != done) {
                    const auto target = command;
                    if (reset) {
                        status = clear();
                        reset = false;
                    }
                    if (SUCCEEDED(status) && state != appliedState) {
                        if (state == Playing) {
                            if (appliedState == Stopped && draining)
                                status = clear();
                            if (SUCCEEDED(status) && !running) {
                                status = client->Start();
                                running = SUCCEEDED(status);
                            }
                        }
                        else if (state == Paused) {
                            if (running)
                                status = client->Stop();
                            running = false;
                            position();
                            draining = false;
                        }
                        else if (appliedState == Playing) {
                            // Streaming stop drains accepted frames; the next
                            // play/write starts a new playback-head epoch.
                            draining = true;
                        }
                        else
                            status = clear();
                        appliedState = state;
                    }
                    volumeOk = SUCCEEDED(volume->SetChannelVolume(0, left));
                    if (channels == 2)
                        volumeOk = SUCCEEDED(volume->SetChannelVolume(1, right)) && volumeOk;
                    if (requestedRate != playbackRate) {
                        auto result =
                            adjustment ? adjustment->SetSampleRate(static_cast<float>(requestedRate)) : E_NOINTERFACE;
                        rateResult = SUCCEEDED(result) ? 0 : InvalidOperation;
                        if (SUCCEEDED(result))
                            playbackRate = requestedRate;
                        else
                            requestedRate = playbackRate;
                    }
                    done = target;
                    if (FAILED(status))
                        dead = true;
                    changed.notify_all();
                }
                if (dead)
                    break;
                UINT32 padding = 0;
                status = client->GetCurrentPadding(&padding);
                if (FAILED(status)) {
                    dead = true;
                    break;
                }
                position();
                UINT32 available = deviceFrames > padding ? deviceFrames - padding : 0;
                while (available && !queue.empty()) {
                    auto& buffer = queue.front();
                    UINT32 frames = static_cast<UINT32>(
                        std::min<size_t>(available, (buffer.samples.size() - buffer.offset) / channels));
                    BYTE* target = nullptr;
                    status = render->GetBuffer(frames, &target);
                    if (FAILED(status)) {
                        dead = true;
                        break;
                    }
                    std::memcpy(target, buffer.samples.data() + buffer.offset,
                                size_t(frames) * channels * sizeof(float));
                    status = render->ReleaseBuffer(frames, 0);
                    if (FAILED(status)) {
                        dead = true;
                        break;
                    }
                    submitted += frames;
                    available -= frames;
                    buffer.offset += size_t(frames) * channels;
                    if (buffer.offset == buffer.samples.size())
                        queue.pop_front();
                }
                if (running && played == written && written && queue.empty()) {
                    if (draining) {
                        status = clear();
                        if (FAILED(status))
                            dead = true;
                    }
                    else if (!starved) {
                        ++underruns;
                        starved = true;
                    }
                }
                changed.notify_all();
                if (dead || closed)
                    break;
                if (running)
                    changed.wait_for(lock, std::chrono::milliseconds(3));
                else
                    changed.wait(lock);
            }
            if (client && initialized) {
                client->Stop();
                client->Reset();
            }
            dead = dead || FAILED(status);
            changed.notify_all();
        }
        CoUninitialize();
    }

    ~Track()
    {
        {
            std::lock_guard lock(mutex);
            closed = true;
            changed.notify_all();
        }
        if (worker.joinable())
            worker.join();
    }

    bool Command(std::unique_lock<std::mutex>& lock)
    {
        const auto request = ++command;
        changed.notify_all();
        changed.wait(lock, [&] {
            return done >= request || closed || dead;
        });
        return !closed && !dead;
    }
};

std::mutex tracksMutex;
std::unordered_map<int64_t, std::shared_ptr<Track>> tracks;
int64_t nextTrack = 1;

std::shared_ptr<Track> Find(int64_t handle)
{
    const std::lock_guard lock(tracksMutex);
    auto it = tracks.find(handle);
    return it == tracks.end() ? nullptr : it->second;
}
}
int64_t CreateWindowsAudio(int rate, int channels, int encoding, int frames)
try {
    if (rate < 8000 || rate > 192000 || channels < 1 || channels > 2 || frames < 1 || frames > rate * 10 ||
        (encoding != 2 && encoding != 3 && encoding != 4))
        return 0;
    auto track = std::make_shared<Track>();
    track->rate = track->playbackRate = track->requestedRate = rate;
    track->channels = channels;
    track->encoding = encoding;
    track->frameBytes = channels * (encoding == 2 ? 2 : encoding == 3 ? 1 : 4);
    track->capacity = track->limit = frames;
    track->worker = std::thread([pointer = track.get()] {
        pointer->Run();
    });
    {
        std::unique_lock lock(track->mutex);
        track->changed.wait(lock, [&] {
            return track->ready;
        });
    }
    if (!track->initialized)
        return 0;
    const std::lock_guard lock(tracksMutex);
    auto handle = nextTrack++;
    tracks.emplace(handle, std::move(track));
    return handle;
}
catch (const std::exception&) {
    return 0;
}

int WriteWindowsAudio(int64_t handle, int bytes, bool blocking, const std::function<bool(void*, int, int)>& copy)
{
    if (bytes < 0)
        return BadValue;
    auto track = Find(handle);
    if (!track)
        return DeadObject;
    std::unique_lock lock(track->mutex);
    if (track->closed || track->dead)
        return DeadObject;
    if (track->draining) {
        track->reset = true;
        if (!track->Command(lock))
            return DeadObject;
    }
    const auto generation = track->generation;
    int consumed = 0;
    while (bytes - consumed >= track->frameBytes) {
        if (track->closed || track->dead)
            return consumed ? consumed : DeadObject;
        if (track->generation != generation)
            break;
        auto occupied = track->written - track->played;
        if (occupied >= static_cast<uint64_t>(track->limit)) {
            if (!blocking || track->state != Playing)
                break;
            track->changed.wait_for(lock, std::chrono::milliseconds(20));
            continue;
        }
        int frames = std::min((bytes - consumed) / track->frameBytes, track->limit - static_cast<int>(occupied));
        try {
            std::vector<char> raw(size_t(frames) * track->frameBytes);
            if (!copy(raw.data(), consumed, static_cast<int>(raw.size())))
                return consumed ? consumed : BadValue;
            Buffer buffer;
            buffer.samples.resize(size_t(frames) * track->channels);
            for (size_t i = 0; i < buffer.samples.size(); i++) {
                float value;
                if (track->encoding == 2) {
                    int16_t sample;
                    std::memcpy(&sample, raw.data() + i * 2, 2);
                    value = sample / 32768.0f;
                }
                else if (track->encoding == 3)
                    value = (static_cast<unsigned char>(raw[i]) - 128) / 128.0f;
                else {
                    std::memcpy(&value, raw.data() + i * 4, 4);
                    value = std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0;
                }
                buffer.samples[i] = value;
            }
            track->queue.push_back(std::move(buffer));
            consumed += frames * track->frameBytes;
            track->written += frames;
            track->starved = false;
            track->changed.notify_all();
        }
        catch (const std::bad_alloc&) {
            return consumed ? consumed : InvalidOperation;
        }
        if (!blocking)
            break;
    }
    return consumed;
}

void ReleaseWindowsAudio(int64_t handle)
{
    std::shared_ptr<Track> track;
    {
        const std::lock_guard lock(tracksMutex);
        auto it = tracks.find(handle);
        if (it == tracks.end())
            return;
        track = std::move(it->second);
        tracks.erase(it);
    }
    {
        const std::lock_guard lock(track->mutex);
        track->closed = true;
        ++track->generation;
        track->changed.notify_all();
    }
}

void StateWindowsAudio(int64_t handle, int state)
{
    auto track = Find(handle);
    if (!track)
        return;
    std::unique_lock lock(track->mutex);
    if (track->state == state)
        return;
    track->state = state;
    ++track->generation;
    track->Command(lock);
}

void FlushWindowsAudio(int64_t handle)
{
    auto track = Find(handle);
    if (!track)
        return;
    std::unique_lock lock(track->mutex);
    if (track->state == Playing)
        return;
    track->reset = true;
    ++track->generation;
    track->Command(lock);
}

bool VolumeWindowsAudio(int64_t handle, float left, float right)
{
    auto track = Find(handle);
    if (!track)
        return false;
    std::unique_lock lock(track->mutex);
    track->left = std::isfinite(left) ? std::clamp(left, 0.0f, 1.0f) : 0;
    track->right = std::isfinite(right) ? std::clamp(right, 0.0f, 1.0f) : 0;
    return track->Command(lock) && track->volumeOk;
}

int64_t QueryWindowsAudio(int64_t handle, AudioQuery query)
{
    auto track = Find(handle);
    if (!track)
        return 0;
    const std::lock_guard lock(track->mutex);
    switch (query) {
    case AudioQuery::Capacity:
        return track->capacity;
    case AudioQuery::Size:
        return track->limit;
    case AudioQuery::Rate:
        return track->playbackRate;
    case AudioQuery::Position:
        return track->state == Stopped ? 0 : track->played;
    case AudioQuery::Written:
        return track->written;
    case AudioQuery::Underruns:
        return track->underruns;
    case AudioQuery::State:
        return track->dead || track->closed ? 0 : track->state;
    case AudioQuery::Latency:
        return track->latencyMillis;
    }
    return 0;
}

int SizeWindowsAudio(int64_t handle, int frames)
{
    auto track = Find(handle);
    if (!track)
        return DeadObject;
    if (frames < 1)
        return BadValue;
    const std::lock_guard lock(track->mutex);
    return track->limit = std::min(frames, track->capacity);
}

int RateWindowsAudio(int64_t handle, int rate)
{
    auto track = Find(handle);
    if (!track)
        return DeadObject;
    std::unique_lock lock(track->mutex);
    if (rate < 8000 || rate > 192000 || rate > track->rate * 8)
        return BadValue;
    track->requestedRate = rate;
    track->rateResult = 0;
    return track->Command(lock) ? track->rateResult : DeadObject;
}

bool WindowsAudioAvailable()
{
    WAVEFORMATEX format{WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0};
    return waveOutOpen(nullptr, WAVE_MAPPER, &format, 0, 0, WAVE_FORMAT_QUERY) == MMSYSERR_NOERROR;
}

int NewWindowsAudioSession()
{
    static volatile LONG* sequence = []()->volatile LONG*
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return nullptr;
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<char> storage(size);
        bool ok = GetTokenInformation(token, TokenUser, storage.data(), size, &size) != FALSE;
        CloseHandle(token);
        LPWSTR sid = nullptr;
        if (!ok || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid))
            return nullptr;
        std::wstring name = L"Local\\Stone.AudioSessionIds.v1.";
        name += sid;
        LocalFree(sid);
        HANDLE mapping =
            CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG), name.c_str());
        if (!mapping)
            return nullptr;
        auto* result = static_cast<volatile LONG*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG)));
        if (!result)
            CloseHandle(mapping); // Successful handle/view live until process teardown.
        return result;
    }
    ();
    if (!sequence || !WindowsAudioAvailable())
        return -1;
    LONG current = *sequence;
    for (;;) {
        if (current == LONG_MAX)
            return -1;
        LONG old = InterlockedCompareExchange(sequence, current + 1, current);
        if (old == current)
            return current + 1;
        current = old;
    }
}
}
