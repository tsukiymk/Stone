#include "stub-libc.h"

#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace stone::windows {
namespace {
constexpr int Disconnected = -899, IllegalArgument = -898, InvalidState = -895;
constexpr int Unimplemented = -890, Unavailable = -889, NoMemory = -887, Timeout = -885, InvalidFormat = -883;
constexpr int Open = 2, Started = 4, Paused = 6, Stopping = 9, Stopped = 10, Closing = 11, DisconnectedState = 13;
constexpr int Exclusive = 0, Shared = 1;

struct Builder {
    int channels = 2;
    int sampleRate = 48000;
    int format = 1;
    int frames = 0;
    int capacity = 0;
    int direction = 0;
    int deviceId = 0;
    int usage = 1;       // AAUDIO_USAGE_MEDIA
    int contentType = 2; // AAUDIO_CONTENT_TYPE_MUSIC
    int sharingMode = Shared;
    int inputPreset = 6; // AAUDIO_INPUT_PRESET_VOICE_RECOGNITION
    uintptr_t dataCallback = 0, errorCallback = 0;
    void* data = nullptr;
    void* errorData = nullptr;
};

struct Stream {
    Builder config;
    HWAVEOUT device = nullptr;
    HANDLE changed = nullptr;
    std::atomic<int> state{Open};
    std::mutex mutex;
    std::condition_variable stateChanged;
    bool closing = false;
    unsigned waiting = 0;
    int queueDepth = 2;
    int underruns = 0;
    bool queuedSinceStart = false;
    int64_t framesWritten = 0, framesRead = 0;
    uint32_t lastDevicePosition = 0;
    std::thread worker;
    std::array<std::vector<char>, 2> buffers;
    std::array<WAVEHDR, 2> headers{};

    ~Stream()
    {
        if (device) {
            waveOutReset(device);
            for (auto& header : headers) {
                if (header.dwFlags & WHDR_PREPARED)
                    waveOutUnprepareHeader(device, &header, sizeof(header));
            }
            waveOutClose(device);
        }
        if (changed)
            CloseHandle(changed);
    }
};

int QueuedBuffers(const Stream& stream)
{
    return static_cast<int>(std::ranges::count_if(stream.headers, [](const WAVEHDR& header) {
        return (header.dwFlags & WHDR_INQUEUE) != 0;
    }));
}

void UpdateState(Stream& stream, int state)
{
    stream.state = state;
    stream.stateChanged.notify_all();
}

void CompleteDrain(Stream& stream)
{
    stream.framesRead = stream.framesWritten;
    // A completed drain can account for a final position reported late by WinMM.
    stream.lastDevicePosition = static_cast<uint32_t>(stream.framesRead);
    UpdateState(stream, Stopped);
}

void Drain(Stream& stream)
{
    if (QueuedBuffers(stream))
        UpdateState(stream, Stopping);
    else
        CompleteDrain(stream);
}

void Play(Stream* stream)
{
    while (WaitForSingleObject(stream->changed, INFINITE) == WAIT_OBJECT_0) {
        {
            const std::lock_guard lock(stream->mutex);
            if (stream->closing)
                return;
            if (stream->state == Stopping && !QueuedBuffers(*stream))
                CompleteDrain(*stream);
            if (stream->state != Started)
                continue;
            if (stream->queueDepth > 1 && stream->queuedSinceStart && !QueuedBuffers(*stream)) {
                if (stream->underruns < INT32_MAX)
                    ++stream->underruns;
                stream->queuedSinceStart = false;
            }
        }
        for (auto& header : stream->headers) {
            {
                const std::lock_guard lock(stream->mutex);
                if (stream->closing)
                    return;
                if (stream->state != Started || QueuedBuffers(*stream) >= stream->queueDepth)
                    break;
                if (header.dwFlags & WHDR_INQUEUE)
                    continue;
            }
            const auto& config = stream->config;
            int result = 1;
            try {
                result = static_cast<int>(CallAndroidCallback(
                    config.dataCallback, stream, config.data, header.lpData, static_cast<uintptr_t>(config.frames),
                    {reinterpret_cast<uint8_t*>(header.lpData), header.dwBufferLength}));
            }
            catch (const std::exception& error) {
                std::fprintf(stderr, "[android-audio] %s\n", error.what());
            }
            bool failed = false;
            {
                const std::lock_guard lock(stream->mutex);
                if (stream->closing)
                    return;
                if (stream->state != Started)
                    break;
                if (result != 0) {
                    Drain(*stream);
                    break;
                }
                failed = waveOutWrite(stream->device, &header, sizeof(header)) != MMSYSERR_NOERROR;
                if (failed)
                    UpdateState(*stream, DisconnectedState);
                else {
                    stream->framesWritten += config.frames;
                    stream->queuedSinceStart = true;
                }
            }
            if (failed) {
                if (config.errorCallback) {
                    try {
                        CallAndroidCallback(config.errorCallback, stream, config.errorData,
                                            reinterpret_cast<void*>(static_cast<intptr_t>(Disconnected)));
                    }
                    catch (const std::exception& error) {
                        std::fprintf(stderr, "[android-audio] %s\n", error.what());
                    }
                }
                break;
            }
        }
    }
}

int OpenStream(const Builder* builder, Stream** result)
{
    *result = nullptr;
    if (builder->direction != 0 || !builder->dataCallback)
        return Unimplemented;
    if (builder->deviceId < 0 || builder->channels < 0 || builder->channels > 2 || builder->sampleRate < 0 ||
        builder->sampleRate > 384000 || builder->frames < 0 || builder->frames > 65536 || builder->capacity < 0 ||
        builder->capacity > 131072)
        return IllegalArgument;
    if (builder->format != 0 && builder->format != 1 && builder->format != 2 && builder->format != 4)
        return InvalidFormat;
    if (builder->sharingMode != Exclusive && builder->sharingMode != Shared)
        return IllegalArgument;
    try {
        auto stream = std::make_unique<Stream>();
        auto& config = stream->config;
        config = *builder;
        if (!config.channels)
            config.channels = 2;
        if (!config.sampleRate)
            config.sampleRate = 48000;
        if (!config.format)
            config.format = 1;
        config.sharingMode = Shared; // The Windows wave mapper uses a shared endpoint.
        if (!config.frames)
            config.frames = std::max(1, config.sampleRate / 50);
        config.capacity = 2 * config.frames;
        WAVEFORMATEX format{};
        format.wFormatTag = config.format == 2 ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(config.channels);
        format.nSamplesPerSec = config.sampleRate;
        format.wBitsPerSample = config.format == 1 ? 16 : 32;
        format.nBlockAlign = format.nChannels * (format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        stream->changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!stream->changed)
            return Unavailable;
        // AudioDeviceInfo exposes the system default output under guest ID 1.
        if (config.deviceId > 1)
            return Unavailable;
        if (waveOutOpen(&stream->device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(stream->changed), 0,
                        CALLBACK_EVENT) != MMSYSERR_NOERROR)
            return Unavailable;
        config.deviceId = 1;
        for (size_t i = 0; i < stream->headers.size(); ++i) {
            auto& buffer = stream->buffers[i];
            auto& header = stream->headers[i];
            buffer.resize(static_cast<size_t>(config.frames) * format.nBlockAlign);
            header.lpData = buffer.data();
            header.dwBufferLength = static_cast<DWORD>(buffer.size());
            if (waveOutPrepareHeader(stream->device, &header, sizeof(header)) != MMSYSERR_NOERROR)
                return Unavailable;
        }
        stream->worker = std::thread(Play, stream.get());
        *result = stream.release();
        return 0;
    }
    catch (const std::bad_alloc&) {
        return NoMemory;
    }
    catch (const std::system_error&) {
        return Unavailable;
    }
}

template <int State> int ChangeState(Stream* stream)
{
    const std::lock_guard lock(stream->mutex);
    const auto current = stream->state.load();
    if (stream->closing)
        return InvalidState;
    if (current == DisconnectedState)
        return Disconnected;
    if (current == State)
        return 0;
    if (current == Stopping)
        return State == Stopped ? 0 : InvalidState;
    MMRESULT error = MMSYSERR_NOERROR;
    if constexpr (State == Started)
        error = waveOutRestart(stream->device);
    else if constexpr (State == Paused) {
        if (current != Started)
            return InvalidState;
        error = waveOutPause(stream->device);
    }
    else if (current == Paused && QueuedBuffers(*stream)) {
        // Resume the endpoint so pending frames can drain while callbacks stop.
        error = waveOutRestart(stream->device);
    }
    if (error != MMSYSERR_NOERROR) {
        UpdateState(*stream, DisconnectedState);
        return Disconnected;
    }
    if constexpr (State == Stopped)
        Drain(*stream);
    else
        UpdateState(*stream, State);
    if constexpr (State == Started)
        stream->queuedSinceStart = false;
    SetEvent(stream->changed);
    return 0;
}

int CloseStream(Stream* stream)
{
    if (stream->worker.get_id() == std::this_thread::get_id())
        return InvalidState;
    {
        std::unique_lock lock(stream->mutex);
        stream->closing = true;
        UpdateState(*stream, Closing);
        stream->stateChanged.wait(lock, [&] {
            return stream->waiting == 0;
        });
    }
    SetEvent(stream->changed);
    stream->worker.join();
    delete stream;
    return 0;
}

int WaitForStateChange(Stream* stream, int input, int* next, int64_t timeoutNanos)
{
    std::unique_lock lock(stream->mutex);
    const auto changed = [&] {
        return stream->closing || stream->state.load() != input;
    };
    ++stream->waiting;
    const bool ready =
        changed() ||
        (timeoutNanos > 0 && stream->stateChanged.wait_for(lock, std::chrono::nanoseconds(timeoutNanos), changed));
    if (next)
        *next = stream->state.load();
    --stream->waiting;
    if (!stream->waiting)
        stream->stateChanged.notify_all();
    return ready ? 0 : Timeout;
}

int SetBufferSize(Stream* stream, int frames)
{
    if (frames < 0)
        return IllegalArgument;
    const std::lock_guard lock(stream->mutex);
    if (stream->closing)
        return InvalidState;
    if (stream->state == DisconnectedState)
        return Disconnected;
    // WinMM queues whole callback buffers. Apply the capacity change when the
    // worker can submit the next one, without truncating an outstanding buffer.
    stream->queueDepth = frames > stream->config.frames ? 2 : 1;
    SetEvent(stream->changed);
    return stream->queueDepth * stream->config.frames;
}

bool ReadPosition(Stream& stream)
{
    MMTIME time{};
    time.wType = TIME_SAMPLES;
    if (waveOutGetPosition(stream.device, &time, sizeof(time)) != MMSYSERR_NOERROR || time.wType != TIME_SAMPLES)
        return false;
    const auto position = static_cast<uint32_t>(time.u.sample);
    const uint32_t delta = position - stream.lastDevicePosition;
    // WinMM positions wrap at 32 bits. Ignore a transient backward report
    // rather than treating it as a large forward jump.
    if (delta < 0x80000000) {
        stream.framesRead += std::min<int64_t>(delta, stream.framesWritten - stream.framesRead);
        stream.lastDevicePosition = position;
    }
    return true;
}

int Timestamp(Stream* stream, int clock, int64_t* position, int64_t* nanos)
{
    const std::lock_guard lock(stream->mutex);
    if (stream->state != Started)
        return InvalidState;
    if (!ReadPosition(*stream))
        return Unavailable;
    AndroidTimespec now{};
    if (AndroidClock(clock, &now))
        return IllegalArgument;
    *position = stream->framesRead;
    *nanos = now.seconds * 1000000000 + now.nanos;
    return 0;
}

const char* ErrorName(int error)
{
    switch (error) {
    case 0:
        return "AAUDIO_OK";
    case Disconnected:
        return "AAUDIO_ERROR_DISCONNECTED";
    case IllegalArgument:
        return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";
    case InvalidState:
        return "AAUDIO_ERROR_INVALID_STATE";
    case Unimplemented:
        return "AAUDIO_ERROR_UNIMPLEMENTED";
    case Unavailable:
        return "AAUDIO_ERROR_UNAVAILABLE";
    case NoMemory:
        return "AAUDIO_ERROR_NO_MEMORY";
    case Timeout:
        return "AAUDIO_ERROR_TIMEOUT";
    case InvalidFormat:
        return "AAUDIO_ERROR_INVALID_FORMAT";
    default:
        return "Unknown AAudio error";
    }
}

const char* StateName(int state)
{
    switch (state) {
    case Open:
        return "AAUDIO_STREAM_STATE_OPEN";
    case Started:
        return "AAUDIO_STREAM_STATE_STARTED";
    case Paused:
        return "AAUDIO_STREAM_STATE_PAUSED";
    case Stopping:
        return "AAUDIO_STREAM_STATE_STOPPING";
    case Stopped:
        return "AAUDIO_STREAM_STATE_STOPPED";
    case Closing:
        return "AAUDIO_STREAM_STATE_CLOSING";
    case DisconnectedState:
        return "AAUDIO_STREAM_STATE_DISCONNECTED";
    default:
        return "AAUDIO_STREAM_STATE_UNKNOWN";
    }
}
}

uintptr_t AndroidAudioImport(std::string_view name, bool hostAbi)
{
#define BIND(symbol, ...)                                                                                              \
    if (name == #symbol) {                                                                                             \
        const auto target = reinterpret_cast<uintptr_t>(__VA_ARGS__);                                                  \
        return hostAbi ? target : BridgeAndroidImport(name, target);                                                   \
    }
#define SET(symbol, field)                                                                                             \
    BIND(                                                                                                              \
        AAudioStreamBuilder_##symbol, +[](Builder* b, int value) {                                                     \
            b->field = value;                                                                                          \
        })
#define GET(symbol, field)                                                                                             \
    BIND(                                                                                                              \
        AAudioStream_##symbol, +[](Stream* s) {                                                                        \
            return s->config.field;                                                                                    \
        })
    BIND(
        AAudio_createStreamBuilder, +[](Builder** result) {
            *result = new (std::nothrow) Builder;
            return *result ? 0 : NoMemory;
        });
    BIND(
        AAudioStreamBuilder_delete, +[](Builder* builder) {
            delete builder;
            return 0;
        });
    SET(setSampleRate, sampleRate);
    SET(setChannelCount, channels);
    SET(setFormat, format);
    SET(setSamplesPerFrame, channels);
    SET(setFramesPerDataCallback, frames);
    SET(setBufferCapacityInFrames, capacity);
    SET(setDirection, direction);
    SET(setDeviceId, deviceId);
    SET(setUsage, usage);
    SET(setInputPreset, inputPreset);
    SET(setContentType, contentType);
    SET(setSharingMode, sharingMode);
    BIND(
        AAudioStreamBuilder_setPerformanceMode, +[](Builder*, int) {
        });
    BIND(
        AAudioStreamBuilder_setDataCallback, +[](Builder* b, uintptr_t cb, void* data) {
            b->dataCallback = cb;
            b->data = data;
        });
    BIND(
        AAudioStreamBuilder_setErrorCallback, +[](Builder* b, uintptr_t cb, void* data) {
            b->errorCallback = cb;
            b->errorData = data;
        });
    BIND(AAudioStreamBuilder_openStream, &OpenStream);
    GET(getSampleRate, sampleRate);
    GET(getChannelCount, channels);
    GET(getFormat, format);
    GET(getSamplesPerFrame, channels);
    GET(getContentType, contentType);
    GET(getSharingMode, sharingMode);
    GET(getFramesPerDataCallback, frames);
    GET(getFramesPerBurst, frames);
    GET(getBufferCapacityInFrames, capacity);
    GET(getDeviceId, deviceId);
    GET(getUsage, usage);
    GET(getInputPreset, inputPreset);
    // WinMM is a shared, buffered output; accepting a low-latency hint does
    // not turn it into Android's MMAP/low-latency transport. Oboe requires
    // this core AAudio query after every successful stream open.
    BIND(
        AAudioStream_getPerformanceMode, +[](Stream*) {
            return 10;
        }); // AAUDIO_PERFORMANCE_MODE_NONE
    BIND(
        AAudioStream_isMMapUsed, +[](Stream*) -> bool {
            return false;
        });
    BIND(AAudioStream_setBufferSizeInFrames, &SetBufferSize);
    BIND(
        AAudioStream_getBufferSizeInFrames, +[](Stream* s) {
            const std::lock_guard lock(s->mutex);
            return s->queueDepth * s->config.frames;
        });
    BIND(
        AAudioStream_getFramesWritten, +[](Stream* s) {
            const std::lock_guard lock(s->mutex);
            return s->framesWritten;
        });
    BIND(
        AAudioStream_getXRunCount, +[](Stream* s) {
            const std::lock_guard lock(s->mutex);
            return s->underruns;
        });
    BIND(
        AAudioStream_getFramesRead, +[](Stream* s) {
            const std::lock_guard lock(s->mutex);
            ReadPosition(*s);
            return s->framesRead;
        });
    BIND(AAudioStream_waitForStateChange, &WaitForStateChange);
    BIND(
        AAudioStream_getState, +[](Stream* s) {
            return s->state.load();
        });
    BIND(AAudioStream_getTimestamp, &Timestamp);
    BIND(AAudioStream_requestStart, &ChangeState<Started>);
    BIND(AAudioStream_requestPause, &ChangeState<Paused>);
    BIND(AAudioStream_requestStop, &ChangeState<Stopped>);
    BIND(AAudioStream_close, &CloseStream);
    BIND(AAudio_convertResultToText, &ErrorName);
    BIND(AAudio_convertStreamStateToText, &StateName);
#undef GET
#undef SET
#undef BIND
    return 0;
}
}
