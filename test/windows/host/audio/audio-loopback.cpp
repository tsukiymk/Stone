#include "audio-loopback.h"
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

namespace stone::windows::test {
namespace {
using namespace Microsoft::WRL;

void Check(HRESULT result)
{
    if (FAILED(result))
        throw std::runtime_error("Process audio loopback failed: " +
                                 std::to_string(static_cast<unsigned long>(result)));
}

class Completion final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IActivateAudioInterfaceCompletionHandler, FtmBase> {
public:
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT status = E_PENDING;
    ComPtr<IAudioClient> client;

    ~Completion()
    {
        if (ready)
            CloseHandle(ready);
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override
    {
        ComPtr<IUnknown> object;
        HRESULT activated = E_FAIL;
        status = operation->GetActivateResult(&activated, &object);
        if (SUCCEEDED(status))
            status = activated;
        if (SUCCEEDED(status))
            status = object.As(&client);
        SetEvent(ready);
        return S_OK;
    }
};

double Power(const std::vector<int16_t>& samples, size_t start, int channel, double frequency)
{
    double real = 0, imaginary = 0;
    constexpr size_t frames = 4800;
    for (size_t i = 0; i < frames; ++i) {
        double angle = i * 6.283185307179586 * frequency / 48000;
        real += samples[(start + i) * 2 + channel] * std::cos(angle);
        imaginary += samples[(start + i) * 2 + channel] * std::sin(angle);
    }
    return std::sqrt(real * real + imaginary * imaginary) * 2 / frames;
}
}

void WithAudioLoopback(const std::function<void()>& test, const std::filesystem::path& output, unsigned long process,
                       bool tones)
{
    const auto outputPath = std::filesystem::absolute(output);
    std::atomic<bool> stop = false;
    std::promise<void> ready;
    auto initialized = ready.get_future();
    std::exception_ptr failure;
    std::vector<int16_t> samples;
    std::thread worker([&] {
        bool signaled = false;
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            Check(com);
            auto completion = Make<Completion>();
            AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
            parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
            parameters.ProcessLoopbackParams.TargetProcessId = process ? process : GetCurrentProcessId();
            parameters.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
            PROPVARIANT variant{};
            variant.vt = VT_BLOB;
            variant.blob.cbSize = sizeof(parameters);
            variant.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);
            ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
            Check(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &variant,
                                              completion.Get(), &operation));
            if (WaitForSingleObject(completion->ready, 15000) != WAIT_OBJECT_0)
                throw std::runtime_error("Audio loopback activation timeout");
            Check(completion->status);
            auto client = completion->client;
            WAVEFORMATEX format{WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0};
            Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 1000000, 0,
                                     &format, nullptr));
            ComPtr<IAudioCaptureClient> capture;
            Check(client->GetService(IID_PPV_ARGS(&capture)));
            Check(client->Start());
            ready.set_value();
            signaled = true;
            while (!stop) {
                UINT32 frames = 0;
                Check(capture->GetNextPacketSize(&frames));
                while (frames) {
                    BYTE* bytes = nullptr;
                    DWORD flags = 0;
                    Check(capture->GetBuffer(&bytes, &frames, &flags, nullptr, nullptr));
                    size_t start = samples.size();
                    if (start + size_t(frames) * 2 > 48000 * 2 * 120) {
                        capture->ReleaseBuffer(frames);
                        throw std::runtime_error("Audio capture exceeded two minutes");
                    }
                    samples.resize(start + size_t(frames) * 2);
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                        std::memcpy(samples.data() + start, bytes, size_t(frames) * 4);
                    Check(capture->ReleaseBuffer(frames));
                    Check(capture->GetNextPacketSize(&frames));
                }
                Sleep(5);
            }
            Check(client->Stop());
        }
        catch (...) {
            failure = std::current_exception();
            if (!signaled)
                ready.set_exception(failure);
        }
        if (SUCCEEDED(com))
            CoUninitialize();
    });
    try {
        initialized.get();
        test();
        Sleep(150);
    }
    catch (...) {
        stop = true;
        worker.join();
        throw;
    }
    stop = true;
    worker.join();
    if (failure)
        std::rethrow_exception(failure);
    std::ofstream file(outputPath, std::ios::binary);
    auto word = [&](uint32_t value, int length) {
        for (int i = 0; i < length; i++)
            file.put(static_cast<char>(value >> (i * 8)));
    };
    file.write("RIFF", 4);
    word(36 + static_cast<uint32_t>(samples.size() * 2), 4);
    file.write("WAVEfmt ", 8);
    word(16, 4);
    word(1, 2);
    word(2, 2);
    word(48000, 4);
    word(192000, 4);
    word(4, 2);
    word(16, 2);
    file.write("data", 4);
    word(static_cast<uint32_t>(samples.size() * 2), 4);
    file.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(samples.size() * 2));
    if (!file)
        throw std::runtime_error("Could not save process-only loopback WAV");
    if (!tones) {
        int peak = 0;
        double square = 0;
        size_t nonzero = 0;
        for (auto value : samples) {
            peak = std::max(peak, std::abs(int(value)));
            square += double(value) * value;
            if (value)
                ++nonzero;
        }
        double rms = samples.empty() ? 0 : std::sqrt(square / samples.size());
        std::cout << "Process-only loopback pid=" << process << " frames=" << samples.size() / 2
                  << " nonzero=" << nonzero << " peak=" << peak << " RMS=" << rms << "\n";
        if (peak < 10 || nonzero < 4800)
            throw std::runtime_error("Target process produced no sustained audible PCM");
        return;
    }
    double left = 0, right = 0, wrongLeft = 0, wrongRight = 0, floatTone = 0, byteTone = 0, resampledTone = 0;
    std::vector<std::pair<double, double>> volumeWindows;
    for (size_t start = 0; start + 4800 < samples.size() / 2; start += 2400) {
        double l = Power(samples, start, 0, 997), r = Power(samples, start, 1, 1499);
        if (l + r > left + right) {
            left = l;
            right = r;
            wrongLeft = Power(samples, start, 0, 1499);
            wrongRight = Power(samples, start, 1, 997);
        }
        floatTone = std::max(floatTone, Power(samples, start, 0, 661));
        byteTone = std::max(byteTone, Power(samples, start, 0, 443));
        resampledTone = std::max(resampledTone, Power(samples, start, 0, 337));
        volumeWindows.emplace_back(Power(samples, start, 0, 2711), Power(samples, start, 0, 3121));
    }
    std::cout << "Audio loopback PCM16 peaks left=" << left << " right=" << right << " cross=" << wrongLeft << ","
              << wrongRight << " float=" << floatTone << " PCM8=" << byteTone << " 44.1k=" << resampledTone << "\n";
    if (left < 50 || right < 50 || wrongLeft > left * .1 || wrongRight > right * .1 || floatTone < 50 ||
        byteTone < 50 || resampledTone < 50)
        throw std::runtime_error(
            "Generated stereo tones did not reach Windows process loopback with separate channels");
    bool before = false, muted = false, after = false;
    for (auto [first, second] : volumeWindows) {
        if (first > 300 && second > 200) {
            if (muted)
                after = true;
            else
                before = true;
        }
        if (before && first < 20 && second > 200)
            muted = true;
    }
    if (!before || !muted || !after)
        throw std::runtime_error("Live volume changed another stream or did not affect already queued audio");
    std::cout
        << "PASS Windows process-only audio loopback: 997/1499 Hz stereo, 661 Hz float, 443 Hz PCM8 and 337 Hz at 44.1 kHz\n";
    std::cout << "PASS live per-track mute/restore of prequeued audio while a second stream remains audible\n";
}
}
