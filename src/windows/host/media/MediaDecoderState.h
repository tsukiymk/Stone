#pragma once

// JNI integer aliases preserve existing buffer and timestamp types; no Java objects are owned here.
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

namespace stone::windows::media_backend {
using Microsoft::WRL::ComPtr;

class Apartment {
public:
    Apartment() : status(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
    }

    ~Apartment()
    {
        if (SUCCEEDED(status))
            CoUninitialize();
    }

    const HRESULT status;
};

inline HRESULT Activate(const GUID& clsid, IMFTransform** transform)
{
    static const HRESULT startup = [] {
        const auto library = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return library ? MFStartup(MF_VERSION, MFSTARTUP_LITE) : HRESULT_FROM_WIN32(GetLastError());
    }();
    if (FAILED(startup))
        return startup;
    return CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(transform));
}

struct VideoFrame {
    ComPtr<IMFSample> sample;
    UINT32 width = 0, height = 0, codedHeight = 0;
    LONG stride = 0;
    UINT32 left = 0, top = 0;
    bool fullRange = false, bt709 = false;
    jlong pts = 0;
};

struct AudioOutput {
    std::vector<uint8_t> pcm;
    jlong pts = 0;
};

struct Decoder {
    ComPtr<IMFTransform> transform;
    std::vector<uint8_t> headers;
    std::deque<VideoFrame> videoOutput;
    UINT32 width = 0, height = 0, sampleRate = 0, channels = 0;
    bool video = false, sendHeaders = true, ended = false;
    jlong endPts = 0;

    HRESULT SetOutputType()
    {
        for (DWORD index = 0;; ++index) {
            ComPtr<IMFMediaType> type;
            HRESULT status = transform->GetOutputAvailableType(0, index, &type);
            if (FAILED(status))
                return status;
            GUID subtype{};
            if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
                subtype != (video ? MFVideoFormat_NV12 : MFAudioFormat_PCM))
                continue;
            if (!video && (MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0) != channels ||
                           MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0) != 16))
                continue;
            status = transform->SetOutputType(0, type.Get(), 0);
            if (SUCCEEDED(status) && !video)
                sampleRate = MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
            return status;
        }
    }

    HRESULT Pull(ComPtr<IMFSample>& sample)
    {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            HRESULT status = transform->GetOutputStreamInfo(0, &info);
            if (FAILED(status))
                return status;
            ComPtr<IMFSample> allocated;
            if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                ComPtr<IMFMediaBuffer> buffer;
                status = MFCreateAlignedMemoryBuffer(info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer);
                if (FAILED(status) || FAILED(status = MFCreateSample(&allocated)) ||
                    FAILED(status = allocated->AddBuffer(buffer.Get())))
                    return status;
            }
            MFT_OUTPUT_DATA_BUFFER output{};
            output.pSample = allocated.Get();
            DWORD flags = 0;
            status = transform->ProcessOutput(0, 1, &output, &flags);
            if (output.pEvents)
                output.pEvents->Release();
            if (output.pSample != allocated.Get())
                sample.Attach(output.pSample);
            else
                sample = std::move(allocated);
            if (status != MF_E_TRANSFORM_STREAM_CHANGE)
                return status;
            sample.Reset();
            status = SetOutputType();
            if (FAILED(status))
                return status;
        }
    }

    HRESULT AppendVideo(ComPtr<IMFSample> sample)
    {
        ComPtr<IMFMediaType> type;
        HRESULT status = transform->GetOutputCurrentType(0, &type);
        if (FAILED(status))
            return status;
        VideoFrame frame;
        UINT32 codedWidth = 0;
        status = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &codedWidth, &frame.codedHeight);
        if (FAILED(status))
            return status;
        if (codedWidth < 2 || codedWidth > 4096 || frame.codedHeight < 2 || frame.codedHeight > 4096)
            return MF_E_INVALIDMEDIATYPE;
        frame.width = codedWidth;
        frame.height = frame.codedHeight;
        frame.stride = static_cast<LONG>(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, codedWidth));
        MFVideoArea aperture{};
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&aperture),
                                    sizeof(aperture), nullptr)) &&
            aperture.OffsetX.value >= 0 && aperture.OffsetY.value >= 0 && aperture.Area.cx > 0 &&
            aperture.Area.cy > 0 && aperture.OffsetX.value + aperture.Area.cx <= static_cast<LONG>(codedWidth) &&
            aperture.OffsetY.value + aperture.Area.cy <= static_cast<LONG>(frame.codedHeight)) {
            frame.left = aperture.OffsetX.value;
            frame.top = aperture.OffsetY.value;
            frame.width = aperture.Area.cx;
            frame.height = aperture.Area.cy;
        }
        frame.fullRange = MFGetAttributeUINT32(type.Get(), MF_MT_VIDEO_NOMINAL_RANGE, 0) == MFNominalRange_0_255;
        frame.bt709 =
            MFGetAttributeUINT32(type.Get(), MF_MT_YUV_MATRIX,
                                 frame.height >= 720 ? MFVideoTransferMatrix_BT709 : MFVideoTransferMatrix_BT601) ==
            MFVideoTransferMatrix_BT709;
        LONGLONG time = 0;
        sample->GetSampleTime(&time);
        frame.pts = time / 10;
        frame.sample = std::move(sample);
        videoOutput.push_back(std::move(frame));
        return S_OK;
    }

    HRESULT Collect(AudioOutput* audio)
    {
        for (;;) {
            ComPtr<IMFSample> sample;
            HRESULT status = Pull(sample);
            if (status == MF_E_TRANSFORM_NEED_MORE_INPUT)
                return S_OK;
            if (FAILED(status))
                return status;
            if (!sample)
                return E_UNEXPECTED;
            if (video) {
                status = AppendVideo(std::move(sample));
                if (FAILED(status))
                    return status;
                continue;
            }
            ComPtr<IMFMediaBuffer> buffer;
            status = sample->ConvertToContiguousBuffer(&buffer);
            if (FAILED(status))
                return status;
            DWORD length = 0;
            status = buffer->GetCurrentLength(&length);
            if (FAILED(status))
                return status;
            if (length == 0)
                continue;
            const auto offset = audio->pcm.size();
            if (offset == 0) {
                LONGLONG time = 0;
                if (SUCCEEDED(sample->GetSampleTime(&time)))
                    audio->pts = time / 10;
            }
            audio->pcm.resize(offset + length);
            BYTE* source = nullptr;
            status = buffer->Lock(&source, nullptr, nullptr);
            if (FAILED(status))
                return status;
            std::memcpy(audio->pcm.data() + offset, source, length);
            buffer->Unlock();
        }
    }

    HRESULT Decode(const std::vector<uint8_t>& input, jlong pts, bool eos, AudioOutput* audio)
    {
        if (!input.empty()) {
            const auto extra = sendHeaders ? headers.size() : 0;
            const auto size = static_cast<DWORD>(input.size() + extra);
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMFSample> sample;
            BYTE* bytes = nullptr;
            HRESULT status = MFCreateMemoryBuffer(size, &buffer);
            if (FAILED(status) || FAILED(status = buffer->Lock(&bytes, nullptr, nullptr)))
                return status;
            if (extra)
                std::memcpy(bytes, headers.data(), extra);
            std::memcpy(bytes + extra, input.data(), input.size());
            buffer->Unlock();
            if (FAILED(status = buffer->SetCurrentLength(size)) || FAILED(status = MFCreateSample(&sample)) ||
                FAILED(status = sample->AddBuffer(buffer.Get())) || FAILED(status = sample->SetSampleTime(pts * 10)))
                return status;
            if (!video)
                sample->SetSampleDuration(1024LL * 10000000 / sampleRate);
            status = transform->ProcessInput(0, sample.Get(), 0);
            if (status == MF_E_NOTACCEPTING) {
                status = Collect(audio);
                if (SUCCEEDED(status))
                    status = transform->ProcessInput(0, sample.Get(), 0);
            }
            if (FAILED(status))
                return status;
            sendHeaders = false;
        }
        if (eos) {
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            const HRESULT status = transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            if (FAILED(status))
                return status;
            ended = true;
            endPts = pts;
        }
        return Collect(audio);
    }

    void Flush()
    {
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        videoOutput.clear();
        sendHeaders = true;
        ended = false;
    }
};

HRESULT Open(Decoder&, const GUID&, const GUID&, const GUID&, const std::vector<uint8_t>& audioConfig = {});
bool ProbeAvc();
bool FrameArgb(const VideoFrame&, std::vector<jint>&);
}
