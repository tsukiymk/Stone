#include "ArtMediaDecoder.h"
#include "MediaDecoderState.h"
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace stone::windows {
namespace {
using Microsoft::WRL::ComPtr;

void Check(HRESULT hr)
{
    if (FAILED(hr)) {
        char text[80];
        std::snprintf(text, sizeof(text), "Windows audio decoder failed (0x%08lx)", static_cast<unsigned long>(hr));
        throw std::runtime_error(text);
    }
}

struct Decoder {
    ComPtr<IMFTransform> transform;
    media_backend::Decoder aac;
    int rate = 0, channels = 0;
    bool opus = false, floating = false;

    void OutputType()
    {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> type;
            Check(transform->GetOutputAvailableType(0, i, &type));
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            bool isFloat = subtype == MFAudioFormat_Float;
            if ((subtype != MFAudioFormat_PCM && !isFloat) ||
                MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0) != (isFloat ? 32u : 16u) ||
                MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0) != static_cast<UINT32>(channels) ||
                MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0) != static_cast<UINT32>(rate))
                continue;
            Check(transform->SetOutputType(0, type.Get(), 0));
            floating = isFloat;
            return;
        }
    }

    void Open(bool isOpus, int sampleRate, int channelCount, const std::vector<uint8_t>& csd)
    {
        opus = isOpus;
        rate = sampleRate;
        channels = channelCount;
        if (!opus) {
            if (rate < 8000 || rate > 96000 || channels < 1 || channels > 2 || csd.size() < 2 || csd.size() > 64 ||
                (csd[0] >> 3) != 2)
                throw std::invalid_argument("Unsupported AAC-LC decoder configuration");
            aac.sampleRate = rate;
            aac.channels = channels;
            Check(media_backend::Open(aac, CLSID_CMSAACDecMFT, MFMediaType_Audio, MFAudioFormat_AAC, csd));
            if (aac.sampleRate != static_cast<UINT32>(rate))
                throw std::runtime_error("AAC output sample rate changed");
            return;
        }
        if (!opus || rate != 48000 || channels < 1 || channels > 2 || csd.size() != 19)
            throw std::invalid_argument("Unsupported Opus decoder configuration");
        Check(CoCreateInstance(CLSID_CMSOpusDecMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform)));
        ComPtr<IMFMediaType> input;
        Check(MFCreateMediaType(&input));
        Check(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        Check(input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Opus));
        Check(input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate));
        Check(input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
        Check(input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        std::vector<uint8_t> config = csd;
        if (!config.empty())
            Check(input->SetBlob(MF_MT_USER_DATA, config.data(), static_cast<UINT32>(config.size())));
        Check(transform->SetInputType(0, input.Get(), 0));
        OutputType();
        Check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        Check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    }

    void Collect(std::vector<uint8_t>& pcm, int64_t& pts)
    {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            Check(transform->GetOutputStreamInfo(0, &info));
            ComPtr<IMFSample> allocated;
            ComPtr<IMFMediaBuffer> buffer;
            if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                Check(MFCreateSample(&allocated));
                Check(MFCreateAlignedMemoryBuffer((std::max)(info.cbSize, DWORD(65536)),
                                                  info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer));
                Check(allocated->AddBuffer(buffer.Get()));
            }
            MFT_OUTPUT_DATA_BUFFER out{};
            out.pSample = allocated.Get();
            DWORD flags = 0;
            HRESULT hr = transform->ProcessOutput(0, 1, &out, &flags);
            if (out.pEvents)
                out.pEvents->Release();
            ComPtr<IMFSample> sample;
            if (out.pSample == allocated.Get())
                sample = allocated;
            else
                sample.Attach(out.pSample);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
                return;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                OutputType();
                continue;
            }
            Check(hr);
            if (!sample)
                throw std::runtime_error("Decoder returned no audio sample");
            Check(sample->ConvertToContiguousBuffer(&buffer));
            DWORD length = 0;
            Check(buffer->GetCurrentLength(&length));
            if (pcm.size() + length > 4 * 1024 * 1024)
                throw std::runtime_error("Decoded audio packet exceeds buffer limit");
            if (pcm.empty()) {
                LONGLONG time = pts * 10;
                sample->GetSampleTime(&time);
                pts = time / 10;
            }
            BYTE* bytes = nullptr;
            Check(buffer->Lock(&bytes, nullptr, nullptr));
            if (floating) {
                if (length % 4) {
                    buffer->Unlock();
                    throw std::runtime_error("Unaligned float audio output");
                }
                for (DWORD i = 0; i < length; i += 4) {
                    float value = 0;
                    std::memcpy(&value, bytes + i, 4);
                    int16_t amplitude =
                        std::isfinite(value)
                            ? static_cast<int16_t>(std::lround(std::clamp(value, -1.0f, 32767.0f / 32768) * 32768))
                            : 0;
                    pcm.push_back(static_cast<uint8_t>(amplitude));
                    pcm.push_back(static_cast<uint8_t>(amplitude >> 8));
                }
            }
            else
                pcm.insert(pcm.end(), bytes, bytes + length);
            buffer->Unlock();
        }
    }

    std::vector<uint8_t> Decode(const std::vector<uint8_t>& packet, int64_t& pts, bool eos)
    {
        if (!opus) {
            if (packet.size() > 1024 * 1024)
                throw std::invalid_argument("AAC access unit exceeds buffer limit");
            media_backend::AudioOutput output;
            Check(aac.Decode(packet, pts, eos, &output));
            if (!output.pcm.empty())
                pts = output.pts;
            return std::move(output.pcm);
        }
        std::vector<uint8_t> pcm;
        if (!packet.empty()) {
            if (packet.size() > 1024 * 1024)
                throw std::invalid_argument("Compressed audio packet too large");
            ComPtr<IMFSample> sample;
            ComPtr<IMFMediaBuffer> buffer;
            Check(MFCreateSample(&sample));
            Check(MFCreateMemoryBuffer(static_cast<DWORD>(packet.size()), &buffer));
            BYTE* bytes = nullptr;
            Check(buffer->Lock(&bytes, nullptr, nullptr));
            std::memcpy(bytes, packet.data(), packet.size());
            buffer->Unlock();
            Check(buffer->SetCurrentLength(static_cast<DWORD>(packet.size())));
            Check(sample->AddBuffer(buffer.Get()));
            int config = packet[0] >> 3;
            int frames = config >= 16        ? 120 << (config & 3)
                         : config >= 12      ? 480 << (config & 1)
                         : (config & 3) == 3 ? 2880
                                             : 480 << (config & 3);
            int count = (packet[0] & 3) == 0 ? 1 : (packet[0] & 3) == 3 ? (packet.size() > 1 ? packet[1] & 63 : 0) : 2;
            if (count == 0 || frames * count > 5760)
                throw std::invalid_argument("Invalid Opus packet duration");
            Check(sample->SetSampleTime(pts * 10));
            Check(sample->SetSampleDuration(int64_t(frames) * count * 10000000 / rate));
            auto hr = transform->ProcessInput(0, sample.Get(), 0);
            if (hr == MF_E_NOTACCEPTING) {
                Collect(pcm, pts);
                hr = transform->ProcessInput(0, sample.Get(), 0);
            }
            Check(hr);
        }
        if (eos) {
            Check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0));
            Check(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
        }
        Collect(pcm, pts);
        return pcm;
    }
};

// All COM objects, including destruction, remain on one MTA thread. Java codec
// configuration and Media3's input queue may run on different threads.
struct Worker {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::function<void()>> jobs;
    std::map<int64_t, Decoder> decoders;
    std::map<int64_t, media_backend::Decoder> videos;
    int64_t next = 1;
    HRESULT startup = E_FAIL;

    Worker()
    {
        std::thread([this] {
            auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            startup = SUCCEEDED(apartment) ? MFStartup(MF_VERSION, MFSTARTUP_LITE) : apartment;
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] {
                        return !jobs.empty();
                    });
                    job = std::move(jobs.front());
                    jobs.pop_front();
                }
                job();
            }
        }).detach();
    }

    template <class F> auto Run(F function)
    {
        auto task = std::make_shared<std::packaged_task<decltype(function())()>>(std::move(function));
        auto result = task->get_future();
        {
            std::lock_guard lock(mutex);
            jobs.push_back([task] {
                (*task)();
            });
        }
        changed.notify_one();
        return result.get();
    }
};

Worker& Host()
{
    static auto* worker = new Worker;
    return *worker;
}
}

int AvailableArtAudioDecoders()
{
    auto& host = Host();
    return host.Run([&] {
        if (FAILED(host.startup))
            return 0;
        ComPtr<IMFTransform> decoder;
        int mask =
            SUCCEEDED(CoCreateInstance(CLSID_CMSOpusDecMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder))) ? 1
                                                                                                                    : 0;
        try {
            Decoder aac;
            aac.Open(false, 48000, 2, {0x11, 0x90});
            mask |= 8;
        }
        catch (const std::exception&) {
        }
        return mask;
    });
}

int64_t OpenArtAudioDecoder(bool opus, int rate, int channels, const std::vector<uint8_t>& csd)
{
    auto& host = Host();
    return host.Run([&] {
        Check(host.startup);
        Decoder decoder;
        decoder.Open(opus, rate, channels, csd);
        auto id = host.next++;
        host.decoders.emplace(id, std::move(decoder));
        return id;
    });
}

std::vector<uint8_t> DecodeArtAudio(int64_t id, const std::vector<uint8_t>& data, int64_t& pts, bool eos)
{
    auto& host = Host();
    return host.Run([&] {
        return host.decoders.at(id).Decode(data, pts, eos);
    });
}

void FlushArtAudioDecoder(int64_t id)
{
    auto& host = Host();
    host.Run([&] {
        auto& decoder = host.decoders.at(id);
        if (!decoder.opus) {
            decoder.aac.Flush();
            return;
        }
        Check(decoder.transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
        Check(decoder.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    });
}

void CloseArtAudioDecoder(int64_t id)
{
    if (id) {
        auto& host = Host();
        host.Run([&] {
            host.decoders.erase(id);
        });
    }
}

bool ArtAvcDecoderAvailable()
{
    auto& host = Host();
    return host.Run([&] {
        return SUCCEEDED(host.startup) && media_backend::ProbeAvc();
    });
}

namespace {
HRESULT OpenHevc(media_backend::Decoder& decoder)
{
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_HEVC};
    IMFActivate** candidates = nullptr;
    UINT32 count = 0;
    HRESULT status = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                               MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &input,
                               nullptr, &candidates, &count);
    if (FAILED(status))
        return status;
    status = MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 i = 0; i < count; ++i) {
        if (FAILED(status)) {
            status = candidates[i]->ActivateObject(IID_PPV_ARGS(&decoder.transform));
            if (SUCCEEDED(status))
                status = media_backend::Open(decoder, GUID_NULL, MFMediaType_Video, MFVideoFormat_HEVC);
            if (FAILED(status))
                decoder.transform.Reset();
        }
        candidates[i]->Release();
    }
    CoTaskMemFree(candidates);
    return status;
}
}

bool ArtHevcDecoderAvailable()
{
    auto& host = Host();
    return host.Run([&] {
        if (FAILED(host.startup))
            return false;
        media_backend::Decoder decoder;
        decoder.video = true;
        decoder.width = 1920;
        decoder.height = 1080;
        return SUCCEEDED(OpenHevc(decoder));
    });
}

int64_t OpenArtVideoDecoder(bool hevc, int width, int height, const std::vector<uint8_t>& headers)
{
    if (width < 2 || height < 2 || width > 1920 || height > 1088 || (width & 1) || (height & 1) ||
        headers.size() > 65536)
        throw std::invalid_argument("Unsupported video dimensions or codec headers");
    auto& host = Host();
    return host.Run([&] {
        Check(host.startup);
        media_backend::Decoder decoder;
        decoder.video = true;
        decoder.width = width;
        decoder.height = height;
        decoder.headers = headers;
        if (!hevc) {
            Check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&decoder.transform)));
            ComPtr<ICodecAPI> options;
            Check(decoder.transform.As(&options));
            VARIANT lowLatency{};
            // The H.264 decoder specifically requires VT_UI4, unlike encoders.
            // Configure before input/output types and BEGIN_STREAMING.
            lowLatency.vt = VT_UI4;
            lowLatency.ulVal = 1;
            Check(options->SetValue(&CODECAPI_AVLowLatencyMode, &lowLatency));
        }
        Check(hevc ? OpenHevc(decoder)
                   : media_backend::Open(decoder, CLSID_CMSH264DecoderMFT, MFMediaType_Video, MFVideoFormat_H264));
        const auto id = host.next++;
        host.videos.emplace(id, std::move(decoder));
        return id;
    });
}

std::vector<ArtVideoFrame> DecodeArtVideo(int64_t id, const std::vector<uint8_t>& packet, int64_t pts, bool eos)
{
    if (packet.size() > 4 * 1024 * 1024)
        throw std::invalid_argument("H.264 access unit exceeds buffer limit");
    auto& host = Host();
    return host.Run([&] {
        auto& decoder = host.videos.at(id);
        Check(decoder.Decode(packet, pts, eos, nullptr));
        std::vector<ArtVideoFrame> frames;
        size_t bytes = 0;
        while (!decoder.videoOutput.empty()) {
            auto source = std::move(decoder.videoOutput.front());
            decoder.videoOutput.pop_front();
            if (source.width > 1920 || source.height > 1088 || frames.size() >= 32)
                throw std::runtime_error("Decoded H.264 frame exceeds supported geometry or frame count");
            ArtVideoFrame frame;
            frame.width = source.width;
            frame.height = source.height;
            frame.pts = source.pts;
            if (!media_backend::FrameArgb(source, frame.pixels))
                throw std::runtime_error("Cannot read decoded H.264 pixels");
            bytes += frame.pixels.size() * sizeof(int32_t);
            if (bytes > 64 * 1024 * 1024)
                throw std::runtime_error("Decoded H.264 output exceeds buffer limit");
            frames.push_back(std::move(frame));
        }
        return frames;
    });
}

void FlushArtVideoDecoder(int64_t id)
{
    auto& host = Host();
    host.Run([&] {
        host.videos.at(id).Flush();
    });
}

void CloseArtVideoDecoder(int64_t id)
{
    if (id) {
        auto& host = Host();
        host.Run([&] {
            host.videos.erase(id);
        });
    }
}
}
