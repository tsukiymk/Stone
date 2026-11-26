#include "stub-libc.h"
#include "stub-libandroid-hardware-buffer.h"
#include "stub-libmediandk-image-reader.h"
#include "stub-libmediandk-format.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace stone::windows {
namespace {
using Microsoft::WRL::ComPtr;
constexpr int Unknown = -10000, Unsupported = -10002, InvalidObject = -10003, InvalidParameter = -10004,
              InvalidOperation = -10005;
constexpr uint32_t KeyFrame = 1, CodecConfig = 2, EndOfStream = 4;
constexpr int Planar = 19, SemiPlanar = 21, SurfaceColor = 0x7f000789;
constexpr char CodecName[] = "c2.android.avc.encoder";
constexpr char Mime[] = "video/avc";
constexpr size_t OutputSlots = 4;

struct BufferInfo {
    int32_t offset, size;
    int64_t timeUs;
    uint32_t flags;
};

static_assert(sizeof(BufferInfo) == 24);

struct Configuration {
    int32_t width = 0, height = 0, bitrate = 0, frameRate = 30, color = SemiPlanar, profile = 1;
    int32_t keyFrameInterval = 1, bitrateMode = 1;

    size_t frameBytes() const
    {
        return size_t(width) * height * 3 / 2;
    }
};

struct Parameters {
    int32_t bitrate = 0;
    bool requestKeyFrame = false;
};
enum class OutputState {
    Free,
    Ready,
    Owned
};

struct Output {
    OutputState state = OutputState::Free;
    uint64_t sequence = 0;
    BufferInfo info{};
    std::vector<uint8_t> bytes;
};

HRESULT CodecValue(ICodecAPI* api, const GUID& property, UINT32 value)
{
    if (!api)
        return E_NOINTERFACE;
    VARIANT setting{};
    setting.vt = VT_UI4;
    setting.ulVal = value;
    return api->SetValue(&property, &setting);
}

HRESULT CreateType(const Configuration& config, const GUID& subtype, IMFMediaType** output)
{
    ComPtr<IMFMediaType> type;
    HRESULT status = MFCreateMediaType(&type);
    if (FAILED(status) || FAILED(status = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(status = type->SetGUID(MF_MT_SUBTYPE, subtype)) ||
        FAILED(status = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, config.width, config.height)) ||
        FAILED(status = MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, config.frameRate, 1)) ||
        FAILED(status = MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
        FAILED(status = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)))
        return status;
    if (config.color == SurfaceColor) {
        if (FAILED(status = type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601)) ||
            FAILED(status = type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235)))
            return status;
    }
    *output = type.Detach();
    return S_OK;
}

HRESULT MakeSample(DWORD size, DWORD alignment, IMFSample** sample, IMFMediaBuffer** memory)
{
    HRESULT status = MFCreateSample(sample);
    if (SUCCEEDED(status))
        status =
            alignment ? MFCreateAlignedMemoryBuffer(size, alignment - 1, memory) : MFCreateMemoryBuffer(size, memory);
    if (SUCCEEDED(status))
        status = (*sample)->AddBuffer(*memory);
    return status;
}

struct Encoder {
    ComPtr<IMFTransform> transform;
    ComPtr<ICodecAPI> api;
    MFT_OUTPUT_STREAM_INFO outputInfo{};

    HRESULT Configure(const Configuration& config)
    {
        transform.As(&api);
        CodecValue(api.Get(), CODECAPI_AVEncCommonRateControlMode,
                   config.bitrateMode == 2 ? eAVEncCommonRateControlMode_CBR
                                           : eAVEncCommonRateControlMode_UnconstrainedVBR);
        CodecValue(api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        CodecValue(api.Get(), CODECAPI_AVEncMPVGOPSize, std::max(1, config.keyFrameInterval * config.frameRate));
        if (api) {
            VARIANT lowLatency{};
            lowLatency.vt = VT_BOOL;
            lowLatency.boolVal = VARIANT_TRUE;
            api->SetValue(&CODECAPI_AVLowLatencyMode, &lowLatency);
        }
        ComPtr<IMFMediaType> output, input;
        const UINT32 profile = config.profile == 8   ? eAVEncH264VProfile_High
                               : config.profile == 2 ? eAVEncH264VProfile_Main
                                                     : eAVEncH264VProfile_Base;
        HRESULT status = CreateType(config, MFVideoFormat_H264, &output);
        if (FAILED(status) || FAILED(status = output->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate)) ||
            FAILED(status = output->SetUINT32(MF_MT_MPEG2_PROFILE, profile)) ||
            FAILED(status = transform->SetOutputType(0, output.Get(), 0)) ||
            FAILED(status = CreateType(config, MFVideoFormat_NV12, &input)) ||
            FAILED(status = input->SetUINT32(MF_MT_DEFAULT_STRIDE, config.width)) ||
            FAILED(status = input->SetUINT32(MF_MT_SAMPLE_SIZE, static_cast<UINT32>(config.frameBytes()))) ||
            FAILED(status = transform->SetInputType(0, input.Get(), 0)) ||
            FAILED(status = transform->GetOutputStreamInfo(0, &outputInfo)))
            return status;
        return S_OK;
    }

    std::vector<uint8_t> SequenceHeader() const
    {
        ComPtr<IMFMediaType> type;
        UINT32 length = 0;
        if (FAILED(transform->GetOutputCurrentType(0, &type)) ||
            FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &length)))
            return {};
        std::vector<uint8_t> bytes(length);
        if (length && FAILED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, bytes.data(), length, nullptr)))
            return {};
        return bytes;
    }

    HRESULT Start()
    {
        const auto status = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        return SUCCEEDED(status) ? transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0) : status;
    }

    void Stop()
    {
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        transform->SetInputType(0, nullptr, 0);
        transform->SetOutputType(0, nullptr, 0);
    }

    HRESULT Push(const Configuration& config, const uint8_t* bytes, DWORD size, int64_t timeUs)
    {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> memory;
        HRESULT status = MakeSample(size, 0, &sample, &memory);
        BYTE* data = nullptr;
        if (FAILED(status) || FAILED(status = memory->Lock(&data, nullptr, nullptr)))
            return status;
        if (config.color == Planar) {
            const auto luma = size_t(config.width) * config.height;
            const auto chroma = luma / 4;
            std::memcpy(data, bytes, luma);
            for (size_t i = 0; i < chroma; ++i) {
                data[luma + i * 2] = bytes[luma + i];
                data[luma + i * 2 + 1] = bytes[luma + chroma + i];
            }
        }
        else {
            std::memcpy(data, bytes, size);
        }
        memory->Unlock();
        if (FAILED(status = memory->SetCurrentLength(size)) || FAILED(status = sample->SetSampleTime(timeUs * 10)) ||
            FAILED(status = sample->SetSampleDuration(10'000'000 / config.frameRate)))
            return status;
        return transform->ProcessInput(0, sample.Get(), 0);
    }

    HRESULT Drain()
    {
        const auto status = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        return SUCCEEDED(status) ? transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0) : status;
    }

    HRESULT Pull(Output& output)
    {
        ComPtr<IMFSample> supplied, provided;
        ComPtr<IMFMediaBuffer> memory;
        if (!(outputInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            const auto status = MakeSample(outputInfo.cbSize, outputInfo.cbAlignment, &supplied, &memory);
            if (FAILED(status))
                return status;
        }
        MFT_OUTPUT_DATA_BUFFER response{};
        response.pSample = supplied.Get();
        DWORD flags = 0;
        HRESULT status = transform->ProcessOutput(0, 1, &response, &flags);
        if (response.pEvents)
            response.pEvents->Release();
        if (response.pSample != supplied.Get())
            provided.Attach(response.pSample);
        if (FAILED(status))
            return status;
        if (!response.pSample)
            return E_UNEXPECTED;

        LONGLONG time = 0;
        UINT32 key = 0;
        response.pSample->GetSampleTime(&time);
        response.pSample->GetUINT32(MFSampleExtension_CleanPoint, &key);
        memory.Reset();
        if (FAILED(status = response.pSample->ConvertToContiguousBuffer(&memory)))
            return status;
        DWORD length = 0;
        if (FAILED(status = memory->GetCurrentLength(&length)) || length > INT_MAX)
            return E_UNEXPECTED;
        output.bytes.resize(length);
        BYTE* data = nullptr;
        if (FAILED(status = memory->Lock(&data, nullptr, nullptr)))
            return status;
        if (length)
            std::memcpy(output.bytes.data(), data, length);
        memory->Unlock();
        output.info = {0, static_cast<int32_t>(length), time / 10, key ? KeyFrame : 0};
        return S_OK;
    }
};

struct AsyncCallbacks {
    uintptr_t input, output, format, error;

    bool active() const
    {
        return input || output || format || error;
    }
};

static_assert(sizeof(AsyncCallbacks) == 32);

enum class Operation {
    Configure,
    Start,
    Stop,
    Parameters
};

struct Request {
    Operation operation;
    Configuration config{};
    Parameters parameters{};
    int result = InvalidObject;
    bool done = false;
};
enum class State {
    Idle,
    Configured,
    Running
};
enum class InputState {
    Free,
    Owned,
    Queued
};

struct CodecSynchronization {
    std::mutex mutex;
    std::condition_variable changed;
};

struct Codec {
    const std::shared_ptr<CodecSynchronization> synchronization = std::make_shared<CodecSynchronization>();
    std::mutex& mutex = synchronization->mutex;
    std::condition_variable& changed = synchronization->changed;
    std::thread worker;
    bool ready = false, available = false, closing = false, deleteOnExit = false;
    std::thread::id workerId;
    Request* request = nullptr;
    Configuration config{};
    State state = State::Idle;
    InputState inputState = InputState::Free;
    std::vector<uint8_t> input, header;
    std::shared_ptr<AndroidImageQueue> surface;
    bool surfaceEnded = false;
    size_t offset = 0, length = 0;
    int64_t timeUs = 0;
    bool eosInput = false, draining = false, pulling = false;
    bool formatChanged = false, headerPending = false;
    int failure = 0;
    AsyncCallbacks callbacks{};
    void* callbackContext = nullptr;
    uint64_t sequence = 0;
    std::array<Output, OutputSlots> outputs;
};

void CloseInputSurface(Codec& codec)
{
    if (!codec.surface)
        return;
    CloseAndroidImageQueue(*codec.surface);
    codec.surface.reset();
    codec.surfaceEnded = false;
}

AndroidMediaFormat* CodecFormat(const Codec& codec, bool input)
{
    auto* format = NewAndroidMediaFormat();
    if (!format)
        return nullptr;
    SetAndroidMediaFormatString(format, "mime", input ? "video/raw" : Mime);
    SetAndroidMediaFormatInt(format, "width", codec.config.width);
    SetAndroidMediaFormatInt(format, "height", codec.config.height);
    SetAndroidMediaFormatInt(format, "frame-rate", codec.config.frameRate);
    if (input) {
        SetAndroidMediaFormatInt(format, "color-format", codec.config.color);
        SetAndroidMediaFormatInt(format, "stride", codec.config.width);
        SetAndroidMediaFormatInt(format, "slice-height", codec.config.height);
    }
    else {
        SetAndroidMediaFormatInt(format, "bitrate", codec.config.bitrate);
        if (!codec.header.empty())
            SetAndroidMediaFormatBuffer(format, "csd-0", codec.header.data(), codec.header.size());
    }
    return format;
}

int PerformRequest(Codec& codec, Encoder& encoder, const Request& request)
{
    switch (request.operation) {
    case Operation::Configure: {
        if (codec.state != State::Idle)
            return InvalidOperation;
        const auto status = encoder.Configure(request.config);
        if (FAILED(status)) {
            std::fprintf(stderr, "[android-media-codec] H.264 configuration failed (0x%08lx)\n", status);
            return Unsupported;
        }
        codec.input.resize(request.config.frameBytes());
        codec.header = encoder.SequenceHeader();
        codec.config = request.config;
        codec.state = State::Configured;
        return 0;
    }
    case Operation::Start:
        if (codec.state != State::Configured)
            return InvalidOperation;
        if (codec.config.color == SurfaceColor && !codec.surface)
            return InvalidOperation;
        if (FAILED(encoder.Start()))
            return Unknown;
        codec.state = State::Running;
        codec.formatChanged = true;
        codec.headerPending = !codec.header.empty();
        codec.timeUs = 0;
        return 0;
    case Operation::Stop:
        if (codec.state != State::Running)
            return InvalidOperation;
        CloseInputSurface(codec);
        encoder.Stop();
        codec.state = State::Idle;
        codec.inputState = InputState::Free;
        codec.eosInput = codec.draining = codec.pulling = false;
        codec.failure = 0;
        codec.header.clear();
        for (auto& output : codec.outputs)
            output = {};
        return 0;
    case Operation::Parameters:
        if (codec.state != State::Running)
            return InvalidOperation;
        if (request.parameters.bitrate > 0) {
            if (FAILED(CodecValue(encoder.api.Get(), CODECAPI_AVEncCommonMeanBitRate,
                                  static_cast<UINT32>(request.parameters.bitrate))))
                return Unsupported;
            codec.config.bitrate = request.parameters.bitrate;
        }
        if (request.parameters.requestKeyFrame &&
            FAILED(CodecValue(encoder.api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1)))
            return Unsupported;
        return 0;
    }
    return Unknown;
}

Output* OldestOutput(Codec& codec)
{
    Output* result = nullptr;
    for (auto& output : codec.outputs) {
        if (output.state == OutputState::Ready && (!result || output.sequence < result->sequence))
            result = &output;
    }
    return result;
}

// RGBA_8888 is top down with each component stored in address order. Encoder
// surfaces use limited-range BT.601 NV12 with one U/V pair per 2-by-2 block.
void RgbaToNv12(const uint8_t* rgba, size_t stride, int width, int height, uint8_t* yuv)
{
    uint8_t* const chroma = yuv + size_t(width) * height;
    const auto luma = [](const uint8_t* p) {
        return static_cast<uint8_t>(16 + ((66 * p[0] + 129 * p[1] + 25 * p[2] + 128) >> 8));
    };
    for (int y = 0; y < height; y += 2) {
        const auto* row0 = rgba + size_t(y) * stride;
        const auto* row1 = row0 + stride;
        auto* y0 = yuv + size_t(y) * width;
        auto* y1 = y0 + width;
        auto* uv = chroma + size_t(y / 2) * width;
        for (int x = 0; x < width; x += 2) {
            const auto* p0 = row0 + x * 4;
            const auto* p1 = p0 + 4;
            const auto* p2 = row1 + x * 4;
            const auto* p3 = p2 + 4;
            y0[x] = luma(p0);
            y0[x + 1] = luma(p1);
            y1[x] = luma(p2);
            y1[x + 1] = luma(p3);
            const int r = p0[0] + p1[0] + p2[0] + p3[0];
            const int g = p0[1] + p1[1] + p2[1] + p3[1];
            const int b = p0[2] + p1[2] + p2[2] + p3[2];
            uv[x] = static_cast<uint8_t>(128 + ((-38 * r - 74 * g + 112 * b + 512) >> 10));
            uv[x + 1] = static_cast<uint8_t>(128 + ((112 * r - 94 * g - 18 * b + 512) >> 10));
        }
    }
}

HRESULT ReadSurfaceInput(Codec& codec)
{
    int64_t timestampNs = 0;
    const std::unique_ptr<AndroidHardwareBuffer, decltype(&ReleaseAndroidHardwareBuffer)> frame(
        TakeAndroidImageFrame(*codec.surface, timestampNs), &ReleaseAndroidHardwareBuffer);
    if (!frame) {
        if (!codec.surfaceEnded)
            return S_FALSE;
        codec.length = 0;
        codec.eosInput = true;
        return S_OK;
    }
    AndroidHardwareBufferDesc description{};
    DescribeAndroidHardwareBuffer(frame.get(), &description);
    void* pixels = nullptr;
    if (LockAndroidHardwareBuffer(frame.get(), 3, -1, nullptr, &pixels) != 0)
        return E_FAIL;
    RgbaToNv12(static_cast<const uint8_t*>(pixels), size_t(description.stride) * 4, codec.config.width,
               codec.config.height, codec.input.data());
    UnlockAndroidHardwareBuffer(frame.get(), nullptr);
    codec.offset = 0;
    codec.length = codec.input.size();
    codec.timeUs = timestampNs / 1000;
    return S_OK;
}

HRESULT ProcessCodec(Codec& codec, Encoder& encoder, Output& slot)
{
    if (codec.headerPending) {
        slot.bytes = codec.header;
        slot.info = {0, static_cast<int32_t>(slot.bytes.size()), 0, CodecConfig};
        codec.headerPending = false;
    }
    else if (codec.pulling) {
        const auto status = encoder.Pull(slot);
        if (status == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            codec.pulling = false;
            if (!codec.draining)
                return S_OK;
            codec.draining = false;
            slot.bytes.clear();
            slot.info = {0, 0, codec.timeUs, EndOfStream};
        }
        else if (FAILED(status))
            return status;
    }
    else {
        if (codec.surface) {
            const auto read = ReadSurfaceInput(codec);
            if (read != S_OK)
                return read;
        }
        const auto status = codec.length ? encoder.Push(codec.config, codec.input.data() + codec.offset,
                                                        static_cast<DWORD>(codec.length), codec.timeUs)
                                         : S_OK;
        if (FAILED(status))
            return status;
        if (codec.eosInput && FAILED(encoder.Drain()))
            return E_FAIL;
        codec.draining = codec.eosInput;
        codec.pulling = true;
        codec.inputState = InputState::Free;
        return S_OK;
    }
    slot.state = OutputState::Ready;
    slot.sequence = ++codec.sequence;
    return S_OK;
}

template <class Callback> void Notify(std::unique_lock<std::mutex>& lock, Callback callback)
{
    lock.unlock();
    try {
        callback();
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "[android-media-codec] %s\n", error.what());
    }
    lock.lock();
}

void RunCodec(Codec* codec)
{
    const auto mediaLibrary = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT startup = !mediaLibrary          ? HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)
                            : SUCCEEDED(apartment) ? MFStartup(MF_VERSION, MFSTARTUP_LITE)
                                                   : apartment;
    Encoder encoder;
    const HRESULT activate = SUCCEEDED(startup)
                                 ? CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                                    IID_PPV_ARGS(encoder.transform.GetAddressOf()))
                                 : startup;
    std::unique_lock lock(codec->mutex);
    codec->workerId = std::this_thread::get_id();
    codec->available = SUCCEEDED(activate);
    codec->ready = true;
    codec->changed.notify_all();
    while (!codec->closing && codec->available) {
        if (auto* request = codec->request) {
            try {
                request->result = PerformRequest(*codec, encoder, *request);
            }
            catch (const std::bad_alloc&) {
                request->result = Unknown;
            }
            codec->request = nullptr;
            request->done = true;
            codec->changed.notify_all();
            continue;
        }
        if (codec->state != State::Running || codec->failure) {
            codec->changed.wait(lock);
            continue;
        }
        const auto callbacks = codec->callbacks;
        auto* const context = codec->callbackContext;
        if (codec->formatChanged && callbacks.active()) {
            codec->formatChanged = false;
            if (callbacks.format) {
                if (auto* format = CodecFormat(*codec, false)) {
                    Notify(lock, [&] {
                        CallAndroidCallback(callbacks.format, codec, context, format);
                    });
                }
            }
            continue;
        }
        if (callbacks.output) {
            if (auto* output = OldestOutput(*codec)) {
                output->state = OutputState::Owned;
                const auto index = static_cast<uintptr_t>(output - codec->outputs.data());
                auto info = output->info;
                Notify(lock, [&] {
                    CallAndroidCallback(callbacks.output, codec, context, reinterpret_cast<void*>(index),
                                        reinterpret_cast<uintptr_t>(&info));
                });
                continue;
            }
        }
        const auto free = std::ranges::find(codec->outputs, OutputState::Free, &Output::state);
        const bool surfaceInput =
            codec->surface && !codec->eosInput && (codec->surfaceEnded || AndroidImageQueueHasFrames(*codec->surface));
        if (free != codec->outputs.end() &&
            (codec->headerPending || codec->pulling || codec->inputState == InputState::Queued || surfaceInput)) {
            HRESULT status;
            try {
                status = ProcessCodec(*codec, encoder, *free);
            }
            catch (const std::bad_alloc&) {
                status = E_OUTOFMEMORY;
            }
            if (FAILED(status)) {
                codec->failure = Unknown;
                std::fprintf(stderr, "[android-media-codec] H.264 encoder failed (0x%08lx)\n", status);
                if (callbacks.error) {
                    Notify(lock, [&] {
                        CallAndroidCodecError(callbacks.error, codec, context, Unknown,
                                              "Media Foundation H.264 encoding failed");
                    });
                }
            }
            codec->changed.notify_all();
            continue;
        }
        if (callbacks.input && !codec->surface && !codec->eosInput && codec->inputState == InputState::Free) {
            codec->inputState = InputState::Owned;
            Notify(lock, [&] {
                CallAndroidCallback(callbacks.input, codec, context);
            });
            continue;
        }
        codec->changed.wait(lock);
    }
    if (codec->request) {
        codec->request->done = true;
        codec->request = nullptr;
        codec->changed.notify_all();
    }
    CloseInputSurface(*codec);
    const bool destroy = codec->deleteOnExit;
    lock.unlock();
    if (codec->available && codec->state == State::Running)
        encoder.Stop();
    encoder.api.Reset();
    encoder.transform.Reset();
    if (SUCCEEDED(startup))
        MFShutdown();
    if (SUCCEEDED(apartment))
        CoUninitialize();
    if (mediaLibrary)
        FreeLibrary(mediaLibrary);
    if (destroy)
        delete codec;
}

Codec* CreateCodec()
{
    try {
        auto codec = std::make_unique<Codec>();
        std::unique_lock lock(codec->mutex);
        codec->worker = std::thread(RunCodec, codec.get());
        codec->changed.wait(lock, [&] {
            return codec->ready;
        });
        if (codec->available)
            return codec.release();
        lock.unlock();
        codec->worker.join();
    }
    catch (const std::exception&) {
    }
    return nullptr;
}

int DeleteCodec(Codec* codec)
{
    if (!codec)
        return InvalidObject;
    {
        const std::lock_guard lock(codec->mutex);
        codec->closing = true;
        codec->changed.notify_all();
        if (std::this_thread::get_id() == codec->workerId) {
            codec->deleteOnExit = true;
            codec->worker.detach();
            return 0;
        }
    }
    codec->worker.join();
    delete codec;
    return 0;
}

int Submit(Codec* codec, Request& request)
{
    if (!codec)
        return InvalidObject;
    std::unique_lock lock(codec->mutex);
    if (std::this_thread::get_id() == codec->workerId)
        return InvalidOperation;
    codec->changed.wait(lock, [&] {
        return !codec->request || codec->closing;
    });
    if (codec->closing)
        return InvalidObject;
    codec->request = &request;
    codec->changed.notify_all();
    codec->changed.wait(lock, [&] {
        return request.done;
    });
    return request.result;
}

int ConfigureCodec(Codec* codec, const AndroidMediaFormat* format, void* surface, void* crypto, uint32_t flags)
{
    if (!codec)
        return InvalidObject;
    if (!format)
        return InvalidParameter;
    if (surface || crypto || flags != 1 || GetAndroidMediaFormatString(format, "mime") != Mime)
        return Unsupported;
    Request request{Operation::Configure};
    auto& config = request.config;
    GetAndroidMediaFormatInt(format, "width", &config.width);
    GetAndroidMediaFormatInt(format, "height", &config.height);
    GetAndroidMediaFormatInt(format, "bitrate", &config.bitrate);
    GetAndroidMediaFormatInt(format, "bitrate-mode", &config.bitrateMode);
    GetAndroidMediaFormatInt(format, "frame-rate", &config.frameRate);
    GetAndroidMediaFormatInt(format, "color-format", &config.color);
    GetAndroidMediaFormatInt(format, "profile", &config.profile);
    GetAndroidMediaFormatInt(format, "i-frame-interval", &config.keyFrameInterval);
    if (config.width < 16 || config.height < 16 || config.width > 4096 || config.height > 4096 || config.width % 2 ||
        config.height % 2 || config.bitrate <= 0 || config.frameRate < 1 || config.frameRate > 240 ||
        config.keyFrameInterval < 0 || config.keyFrameInterval > 3600)
        return InvalidParameter;
    if ((config.color != Planar && config.color != SemiPlanar && config.color != SurfaceColor) ||
        (config.profile != 1 && config.profile != 2 && config.profile != 8) ||
        (config.bitrateMode != 1 && config.bitrateMode != 2))
        return Unsupported;
    return Submit(codec, request);
}

int CreateInputSurface(Codec* codec, void** window)
{
    if (!codec)
        return InvalidObject;
    if (!window)
        return InvalidParameter;
    *window = nullptr;
    const std::lock_guard lock(codec->mutex);
    if (codec->state != State::Configured || codec->surface || codec->config.color != SurfaceColor)
        return InvalidOperation;
    try {
        // The returned window can outlive the codec. Its producer never holds a raw codec pointer.
        const std::weak_ptr<CodecSynchronization> signal = codec->synchronization;
        auto queue = CreateAndroidEncoderQueue(codec->config.width, codec->config.height, [signal] {
            if (const auto synchronization = signal.lock()) {
                const std::lock_guard lock(synchronization->mutex);
                synchronization->changed.notify_all();
            }
        });
        if (!queue || !(*window = CreateAndroidImageWindow(queue)))
            return Unknown;
        codec->surface = std::move(queue);
        return 0;
    }
    catch (const std::bad_alloc&) {
        return Unknown;
    }
}

int SignalSurfaceEnd(Codec* codec)
{
    if (!codec)
        return InvalidObject;
    const std::lock_guard lock(codec->mutex);
    if (codec->state != State::Running || !codec->surface || codec->surfaceEnded)
        return InvalidOperation;
    EndAndroidImageQueueInput(*codec->surface);
    codec->surfaceEnded = true;
    codec->changed.notify_all();
    return 0;
}

int ChangeParameters(Codec* codec, const AndroidMediaFormat* format)
{
    if (!format)
        return InvalidParameter;
    Request request{Operation::Parameters};
    int32_t key = 0;
    const bool bitrate = GetAndroidMediaFormatInt(format, "video-bitrate", &request.parameters.bitrate);
    const bool keyframe = GetAndroidMediaFormatInt(format, "request-sync", &key);
    if (!bitrate && !keyframe)
        return Unsupported;
    if (bitrate && request.parameters.bitrate <= 0)
        return InvalidParameter;
    request.parameters.requestKeyFrame = keyframe;
    return Submit(codec, request);
}

int64_t Dequeue(Codec* codec, BufferInfo* info, int64_t timeoutUs, bool input)
{
    if (!codec || (!input && !info))
        return InvalidParameter;
    std::unique_lock lock(codec->mutex);
    if (codec->state != State::Running || codec->callbacks.active() || (input && codec->surface))
        return InvalidOperation;
    const auto ready = [&] {
        return codec->closing || codec->state != State::Running || codec->failure ||
               (input ? !codec->eosInput && codec->inputState == InputState::Free
                      : codec->formatChanged || OldestOutput(*codec));
    };
    if (!ready() && timeoutUs < 0)
        codec->changed.wait(lock, ready);
    if (!ready() && timeoutUs > 0)
        codec->changed.wait_for(lock, std::chrono::microseconds(std::min<int64_t>(timeoutUs, INT64_MAX / 1000 / 2)),
                                ready);
    if (codec->closing || codec->state != State::Running)
        return InvalidOperation;
    if (codec->failure)
        return codec->failure;
    if (!ready())
        return -1;
    if (input) {
        codec->inputState = InputState::Owned;
        return 0;
    }
    if (codec->formatChanged) {
        codec->formatChanged = false;
        return -2;
    }
    auto* output = OldestOutput(*codec);
    output->state = OutputState::Owned;
    *info = output->info;
    return output - codec->outputs.data();
}

uint8_t* CodecInput(Codec* codec, size_t index, size_t* size)
{
    if (size)
        *size = 0;
    if (!codec || index != 0)
        return nullptr;
    const std::lock_guard lock(codec->mutex);
    if (codec->state != State::Running || codec->surface || codec->inputState != InputState::Owned)
        return nullptr;
    if (size)
        *size = codec->input.size();
    return codec->input.data();
}

int QueueInput(Codec* codec, size_t index, int64_t offset, size_t length, uint64_t timeUs, uint32_t flags)
{
    if (!codec)
        return InvalidObject;
    if (index || offset < 0 || timeUs > static_cast<uint64_t>(INT64_MAX / 10) || (flags & ~EndOfStream))
        return InvalidParameter;
    const std::lock_guard lock(codec->mutex);
    if (codec->state != State::Running || codec->surface || codec->eosInput || codec->inputState != InputState::Owned)
        return InvalidOperation;
    const bool eos = (flags & EndOfStream) != 0;
    if ((!eos || length) && length != codec->config.frameBytes())
        return InvalidParameter;
    if (static_cast<uint64_t>(offset) > codec->input.size() ||
        length > codec->input.size() - static_cast<size_t>(offset))
        return InvalidParameter;
    codec->offset = static_cast<size_t>(offset);
    codec->length = length;
    codec->timeUs = static_cast<int64_t>(timeUs);
    codec->eosInput = eos;
    codec->inputState = InputState::Queued;
    codec->changed.notify_all();
    return 0;
}

uint8_t* CodecOutput(Codec* codec, size_t index, size_t* size)
{
    if (size)
        *size = 0;
    if (!codec || index >= OutputSlots)
        return nullptr;
    const std::lock_guard lock(codec->mutex);
    auto& output = codec->outputs[index];
    if (output.state != OutputState::Owned)
        return nullptr;
    if (size)
        *size = output.bytes.size();
    return output.bytes.data();
}

int ReleaseOutput(Codec* codec, size_t index, bool render)
{
    if (!codec)
        return InvalidObject;
    if (render || index >= OutputSlots)
        return InvalidParameter;
    const std::lock_guard lock(codec->mutex);
    auto& output = codec->outputs[index];
    if (output.state != OutputState::Owned)
        return InvalidOperation;
    output.state = OutputState::Free;
    codec->changed.notify_all();
    return 0;
}

int SetCallbacks(Codec* codec, const AsyncCallbacks& callbacks, void* context)
{
    if (!codec)
        return InvalidObject;
    const std::lock_guard lock(codec->mutex);
    if (codec->state == State::Running)
        return InvalidOperation;
    codec->callbacks = callbacks;
    codec->callbackContext = context;
    return 0;
}
}

uintptr_t AndroidMediaCodecImport(std::string_view name)
{
#define BIND(symbol, ...)                                                                                              \
    if (name == #symbol)                                                                                               \
    return BridgeAndroidImport(name, reinterpret_cast<uintptr_t>(__VA_ARGS__))
    BIND(
        AMediaCodec_createCodecByName, +[](const char* name) -> Codec* {
            if (!name)
                return nullptr;
            const std::string_view value(name);
            return value == CodecName || value == "OMX.google.h264.encoder" ? CreateCodec() : nullptr;
        });
    BIND(
        AMediaCodec_createEncoderByType, +[](const char* type) -> Codec* {
            return type && std::string_view(type) == Mime ? CreateCodec() : nullptr;
        });
    // This codec backend currently exposes encoders. The NDK returns null
    // when no decoder for the requested MIME type is available.
    BIND(
        AMediaCodec_createDecoderByType, +[](const char*) -> Codec* {
            return nullptr;
        });
    BIND(AMediaCodec_delete, &DeleteCodec);
    BIND(AMediaCodec_configure, &ConfigureCodec);
    BIND(AMediaCodec_createInputSurface, &CreateInputSurface);
    BIND(AMediaCodec_signalEndOfInputStream, &SignalSurfaceEnd);
    BIND(
        AMediaCodec_start, +[](Codec* codec) {
            Request request{Operation::Start};
            return Submit(codec, request);
        });
    BIND(
        AMediaCodec_stop, +[](Codec* codec) {
            Request request{Operation::Stop};
            return Submit(codec, request);
        });
    BIND(
        AMediaCodec_getName, +[](Codec* codec, char** output) {
            if (!codec)
                return InvalidObject;
            if (!output)
                return InvalidParameter;
            *output = _strdup(CodecName);
            return *output ? 0 : Unknown;
        });
    BIND(
        AMediaCodec_releaseName, +[](Codec*, char* name) {
            std::free(name);
        });
    BIND(
        AMediaCodec_getInputFormat, +[](Codec* codec) -> AndroidMediaFormat* {
            if (!codec)
                return nullptr;
            const std::lock_guard lock(codec->mutex);
            return codec->state == State::Idle ? nullptr : CodecFormat(*codec, true);
        });
    BIND(
        AMediaCodec_getOutputFormat, +[](Codec* codec) -> AndroidMediaFormat* {
            if (!codec)
                return nullptr;
            const std::lock_guard lock(codec->mutex);
            return codec->state == State::Idle ? nullptr : CodecFormat(*codec, false);
        });
    BIND(
        AMediaCodec_dequeueInputBuffer, +[](Codec* codec, int64_t timeout) {
            return Dequeue(codec, nullptr, timeout, true);
        });
    BIND(
        AMediaCodec_dequeueOutputBuffer, +[](Codec* codec, BufferInfo* info, int64_t timeout) {
            return Dequeue(codec, info, timeout, false);
        });
    BIND(AMediaCodec_getInputBuffer, &CodecInput);
    BIND(AMediaCodec_queueInputBuffer, &QueueInput);
    BIND(AMediaCodec_getOutputBuffer, &CodecOutput);
    BIND(AMediaCodec_releaseOutputBuffer, &ReleaseOutput);
    BIND(AMediaCodec_setParameters, &ChangeParameters);
    if (name == "AMediaCodec_setAsyncNotifyCallback")
        return AndroidCodecCallbackThunk(reinterpret_cast<uintptr_t>(&SetCallbacks));
#undef BIND
    return 0;
}
}
