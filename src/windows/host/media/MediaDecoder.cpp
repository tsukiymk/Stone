#include "MediaDecoderState.h"

namespace stone::windows::media_backend {
HRESULT Open(Decoder& decoder, const GUID& clsid, const GUID& major, const GUID& subtype,
             const std::vector<uint8_t>& audioConfig)
{
    HRESULT status = decoder.transform ? S_OK : Activate(clsid, &decoder.transform);
    ComPtr<IMFMediaType> input;
    if (FAILED(status) || FAILED(status = MFCreateMediaType(&input)) ||
        FAILED(status = input->SetGUID(MF_MT_MAJOR_TYPE, major)) ||
        FAILED(status = input->SetGUID(MF_MT_SUBTYPE, subtype)))
        return status;
    if (decoder.video) {
        if (FAILED(status = MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, decoder.width, decoder.height)))
            return status;
    }
    else {
        // HEAACWAVEINFO fields after WAVEFORMATEX, followed by AudioSpecificConfig.
        std::vector<uint8_t> userData(12);
        userData[2] = 0xfe; // unknown AAC profile/level indication
        userData.insert(userData.end(), audioConfig.begin(), audioConfig.end());
        if (FAILED(status = input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, decoder.sampleRate)) ||
            FAILED(status = input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, decoder.channels)) ||
            FAILED(status = input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)) ||
            FAILED(status = input->SetBlob(MF_MT_USER_DATA, userData.data(), static_cast<UINT32>(userData.size()))))
            return status;
    }
    if (FAILED(status = decoder.transform->SetInputType(0, input.Get(), 0)) ||
        FAILED(status = decoder.SetOutputType()) ||
        FAILED(status = decoder.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)))
        return status;
    return decoder.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

bool ProbeAvc()
{
    Apartment apartment;
    Decoder probe;
    probe.video = true;
    probe.width = 640;
    probe.height = 480;
    return SUCCEEDED(Open(probe, CLSID_CMSH264DecoderMFT, MFMediaType_Video, MFVideoFormat_H264));
}

bool FrameArgb(const VideoFrame& frame, std::vector<jint>& pixels)
{
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(frame.sample->GetBufferByIndex(0, &buffer)))
        return false;
    pixels.resize(static_cast<size_t>(frame.width) * frame.height);
    ComPtr<IMF2DBuffer> buffer2d;
    BYTE* luma = nullptr;
    LONG stride = frame.stride;
    DWORD length = 0;
    const bool is2d = SUCCEEDED(buffer.As(&buffer2d));
    if (is2d ? FAILED(buffer2d->Lock2D(&luma, &stride)) : FAILED(buffer->Lock(&luma, nullptr, &length)))
        return false;
    const size_t minimumSize =
        stride > 0 ? static_cast<size_t>(stride) * (frame.codedHeight + (frame.codedHeight + 1) / 2) : 0;
    const auto lastChromaPair = ((frame.left + frame.width - 1) & ~1U) + 2;
    const bool valid = stride > 0 && static_cast<UINT32>(stride) >= lastChromaPair && (is2d || minimumSize <= length);
    if (valid) {
        const auto* chroma = luma + static_cast<size_t>(stride) * frame.codedHeight;
        const int yGain = frame.fullRange ? 256 : 298;
        const int yOffset = frame.fullRange ? 0 : 16;
        const int redV = frame.fullRange ? (frame.bt709 ? 403 : 359) : (frame.bt709 ? 459 : 409);
        const int greenU = frame.fullRange ? (frame.bt709 ? 48 : 88) : (frame.bt709 ? 55 : 100);
        const int greenV = frame.fullRange ? (frame.bt709 ? 120 : 183) : (frame.bt709 ? 136 : 208);
        const int blueU = frame.fullRange ? (frame.bt709 ? 475 : 454) : (frame.bt709 ? 541 : 516);
        const auto channel = [](int value) {
            return static_cast<uint32_t>(std::clamp((value + 128) >> 8, 0, 255));
        };
        for (UINT32 row = 0; row < frame.height; ++row) {
            const auto y = frame.top + row;
            const auto* yRow = luma + static_cast<size_t>(stride) * y;
            const auto* uvRow = chroma + static_cast<size_t>(stride) * (y / 2);
            auto* output = pixels.data() + static_cast<size_t>(frame.width) * row;
            for (UINT32 column = 0; column < frame.width; ++column) {
                const auto x = frame.left + column;
                const int yy = yGain * (yRow[x] - yOffset);
                const int u = uvRow[x & ~1U] - 128;
                const int v = uvRow[(x & ~1U) + 1] - 128;
                output[column] =
                    static_cast<jint>(0xff000000U | (channel(yy + redV * v) << 16) |
                                      (channel(yy - greenU * u - greenV * v) << 8) | channel(yy + blueU * u));
            }
        }
    }
    if (is2d)
        buffer2d->Unlock2D();
    else
        buffer->Unlock();
    return valid;
}

}
