#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
void Check(HRESULT status)
{
    if (FAILED(status)) {
        char text[64];
        std::snprintf(text, sizeof(text), "H.264 fixture encoder failed: 0x%08lx", status);
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
        throw std::invalid_argument("Expected fixture output path");
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    Check(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    ComPtr<IMFTransform> encoder;
    Check(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder)));
    constexpr UINT32 width = 1920, height = 1080, fps = 24;
    const auto type = [&](const GUID& subtype) {
        ComPtr<IMFMediaType> result;
        Check(MFCreateMediaType(&result));
        Check(result->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        Check(result->SetGUID(MF_MT_SUBTYPE, subtype));
        Check(MFSetAttributeSize(result.Get(), MF_MT_FRAME_SIZE, width, height));
        Check(MFSetAttributeRatio(result.Get(), MF_MT_FRAME_RATE, fps, 1));
        Check(MFSetAttributeRatio(result.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
        Check(result->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        return result;
    };
    auto output = type(MFVideoFormat_H264);
    Check(output->SetUINT32(MF_MT_AVG_BITRATE, 256000));
    Check(output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base));
    Check(encoder->SetOutputType(0, output.Get(), 0));
    auto input = type(MFVideoFormat_NV12);
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
            Check(MFCreateMemoryBuffer((std::max)(info.cbSize, DWORD(width * height * 4)), &buffer));
            Check(sample->AddBuffer(buffer.Get()));
            MFT_OUTPUT_DATA_BUFFER data{};
            data.pSample = sample.Get();
            DWORD flags = 0;
            const auto status = encoder->ProcessOutput(0, 1, &data, &flags);
            if (data.pEvents)
                data.pEvents->Release();
            if (status == MF_E_TRANSFORM_NEED_MORE_INPUT)
                return;
            Check(status);
            if (data.pSample != sample.Get())
                sample.Attach(data.pSample);
            Check(sample->ConvertToContiguousBuffer(&buffer));
            DWORD size = 0;
            BYTE* bytes = nullptr;
            Check(buffer->GetCurrentLength(&size));
            Check(buffer->Lock(&bytes, nullptr, nullptr));
            Packet packet{};
            packet.bytes.assign(bytes, bytes + size);
            buffer->Unlock();
            LONGLONG pts = 0;
            Check(sample->GetSampleTime(&pts));
            packet.pts = pts / 10;
            packets.push_back(std::move(packet));
        }
    };
    for (int i = 0; i < 24; ++i) {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        Check(MFCreateSample(&sample));
        Check(MFCreateMemoryBuffer(width * height * 3 / 2, &buffer));
        BYTE* bytes = nullptr;
        Check(buffer->Lock(&bytes, nullptr, nullptr));
        std::fill_n(bytes, width * height, i < 12 ? 81 : 41);
        for (size_t at = width * height; at < width * height * 3 / 2; at += 2) {
            bytes[at] = i < 12 ? 90 : 240;
            bytes[at + 1] = i < 12 ? 240 : 110;
        }
        buffer->Unlock();
        Check(buffer->SetCurrentLength(width * height * 3 / 2));
        Check(sample->AddBuffer(buffer.Get()));
        Check(sample->SetSampleTime(int64_t(i) * 10000000 / fps));
        Check(sample->SetSampleDuration(10000000 / fps));
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
    if (packets.size() != 24)
        throw std::runtime_error("Fixture encoder did not produce 24 frames");
    Check(encoder->GetOutputCurrentType(0, &output));
    UINT32 count = 0;
    Check(output->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &count));
    std::vector<uint8_t> headers(count);
    Check(output->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, headers.data(), count, nullptr));
    std::ofstream file(std::filesystem::path(argv[1]), std::ios::binary | std::ios::trunc);
    file.write("STAVC001", 8);
    Write(file, width);
    Write(file, height);
    Write(file, count);
    Write(file, uint32_t(packets.size()));
    file.write(reinterpret_cast<const char*>(headers.data()), headers.size());
    for (const auto& packet : packets) {
        Write(file, packet.pts);
        Write(file, uint32_t(packet.bytes.size()));
        file.write(reinterpret_cast<const char*>(packet.bytes.data()), packet.bytes.size());
    }
    if (!file)
        throw std::runtime_error("Cannot write H.264 fixture");
    std::cout << "Generated 24 H.264 frames: red then blue, 1920x1080, 24 fps\n";
    return 0;
}
catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
