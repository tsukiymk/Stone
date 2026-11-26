#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;

void Check(HRESULT hr)
{
    if (FAILED(hr)) {
        char text[80];
        std::snprintf(text, sizeof(text), "AAC fixture encoder failed: 0x%08lx", hr);
        throw std::runtime_error(text);
    }
}

struct Packet {
    int64_t pts;
    std::vector<uint8_t> bytes;
};

template <class T> void Write(std::ofstream& out, T value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
}
int wmain(int argc, wchar_t** argv)
try {
    if (argc != 2)
        throw std::invalid_argument("Expected AAC fixture output path");
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    Check(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    ComPtr<IMFTransform> encoder;
    Check(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder)));
    constexpr uint32_t rate = 48000, channels = 2;
    ComPtr<IMFMediaType> output, input;
    Check(MFCreateMediaType(&output));
    Check(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    Check(output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
    Check(output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate));
    Check(output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
    Check(output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    Check(output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000));
    Check(output->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0));
    Check(encoder->SetOutputType(0, output.Get(), 0));
    Check(MFCreateMediaType(&input));
    Check(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    Check(input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    Check(input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate));
    Check(input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
    Check(input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    Check(input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4));
    Check(input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 4));
    Check(encoder->SetInputType(0, input.Get(), 0));
    Check(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    Check(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    std::vector<Packet> packets;
    const auto collect = [&] {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            Check(encoder->GetOutputStreamInfo(0, &info));
            ComPtr<IMFSample> sample;
            ComPtr<IMFMediaBuffer> buffer;
            Check(MFCreateSample(&sample));
            Check(MFCreateMemoryBuffer((std::max)(info.cbSize, DWORD(65536)), &buffer));
            Check(sample->AddBuffer(buffer.Get()));
            MFT_OUTPUT_DATA_BUFFER out{};
            out.pSample = sample.Get();
            DWORD flags = 0;
            auto status = encoder->ProcessOutput(0, 1, &out, &flags);
            if (out.pEvents)
                out.pEvents->Release();
            if (out.pSample != sample.Get())
                sample.Attach(out.pSample);
            if (status == MF_E_TRANSFORM_NEED_MORE_INPUT)
                return;
            Check(status);
            Check(sample->ConvertToContiguousBuffer(&buffer));
            DWORD size = 0;
            BYTE* bytes = nullptr;
            Check(buffer->GetCurrentLength(&size));
            Check(buffer->Lock(&bytes, nullptr, nullptr));
            Packet packet{};
            packet.bytes.assign(bytes, bytes + size);
            buffer->Unlock();
            LONGLONG time = 0;
            Check(sample->GetSampleTime(&time));
            packet.pts = time / 10;
            packets.push_back(std::move(packet));
        }
    };
    for (int block = 0; block < 48; block++) {
        int16_t pcm[2048];
        for (int i = 0; i < 1024; i++) {
            const auto value =
                static_cast<int16_t>(std::sin((block * 1024 + i) * 6.283185307179586 * 440 / rate) * 6000);
            pcm[2 * i] = pcm[2 * i + 1] = value;
        }
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        BYTE* bytes = nullptr;
        Check(MFCreateSample(&sample));
        Check(MFCreateMemoryBuffer(sizeof(pcm), &buffer));
        Check(buffer->Lock(&bytes, nullptr, nullptr));
        std::memcpy(bytes, pcm, sizeof(pcm));
        buffer->Unlock();
        Check(buffer->SetCurrentLength(sizeof(pcm)));
        Check(sample->AddBuffer(buffer.Get()));
        Check(sample->SetSampleTime(int64_t(block) * 1024 * 10000000 / rate));
        Check(sample->SetSampleDuration(1024LL * 10000000 / rate));
        auto status = encoder->ProcessInput(0, sample.Get(), 0);
        if (status == MF_E_NOTACCEPTING) {
            collect();
            status = encoder->ProcessInput(0, sample.Get(), 0);
        }
        Check(status);
        collect();
    }
    Check(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0));
    Check(encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
    collect();
    Check(encoder->GetOutputCurrentType(0, &output));
    UINT32 length = 0;
    Check(output->GetBlobSize(MF_MT_USER_DATA, &length));
    if (length <= 12 || packets.size() < 40)
        throw std::runtime_error("Incomplete AAC fixture");
    std::vector<uint8_t> csd(length);
    Check(output->GetBlob(MF_MT_USER_DATA, csd.data(), length, nullptr));
    csd.erase(csd.begin(), csd.begin() + 12);
    std::ofstream file(std::filesystem::path(argv[1]), std::ios::binary | std::ios::trunc);
    file.write("STAAC001", 8);
    Write(file, rate);
    Write(file, channels);
    Write(file, uint32_t(csd.size()));
    Write(file, uint32_t(packets.size()));
    file.write(reinterpret_cast<const char*>(csd.data()), csd.size());
    for (const auto& packet : packets) {
        Write(file, packet.pts);
        Write(file, uint32_t(packet.bytes.size()));
        file.write(reinterpret_cast<const char*>(packet.bytes.data()), packet.bytes.size());
    }
    if (!file)
        throw std::runtime_error("Cannot write AAC fixture");
    std::cout << "Generated AAC-LC 440 Hz stereo fixture, packets=" << packets.size() << '\n';
    return 0;
}
catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
