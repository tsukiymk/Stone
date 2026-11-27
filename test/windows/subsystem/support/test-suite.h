#pragma once

#include "loader/AndroidSystemElf.h"

#include <jni.h>
#include <filesystem>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace stone::windows::test {
class Suite;
void TestInotifyStorage(Suite& suite);
void CheckArm64ScalarDecode();
void TestArtAudioOutputSample(const std::filesystem::path&, const std::filesystem::path&, const std::filesystem::path&,
                              const std::filesystem::path&, bool codecs = false);
void TestArtStartupServicesSample(const std::filesystem::path&, const std::filesystem::path&,
                                  const std::filesystem::path&, const std::filesystem::path&, bool nativeOnly = false,
                                  bool videoCodecs = false);
void TestArtResizeWindowSample(const std::filesystem::path&, const std::filesystem::path&, const std::filesystem::path&,
                               const std::filesystem::path&);
void TestArtNativeLibrariesSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                                  const std::filesystem::path& apk, const std::filesystem::path& artHost);
void TestArtJobSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                      const std::filesystem::path& apk, const std::filesystem::path& artHost, std::wstring_view stage);
void TestArtBatterySample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                          const std::filesystem::path& apk, const std::filesystem::path& artHost);
void TestArtStorageSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                          const std::filesystem::path& apk, const std::filesystem::path& artHost,
                          std::wstring_view stage);
void Require(bool condition, std::string_view expected,
             std::source_location location = std::source_location::current());

#define STONE_CHECK(...) ::stone::windows::test::Require(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__)

template <typename Function> Function Guest(uintptr_t address, std::string_view signature)
{
    Require(address != 0, "guest symbol resolves");
    return reinterpret_cast<Function>(MakeNativeAndroidThunk(address, signature));
}

template <typename Function> Function Host(uintptr_t address)
{
    Require(address != 0, "host symbol resolves");
    return reinterpret_cast<Function>(address);
}

class Suite {
public:
    explicit Suite(std::string profile) : profile_(std::move(profile))
    {
    }

    void Run(std::string_view module, auto test)
    {
        ++total_;
        try {
            test();
            Report(module, nullptr);
        }
        catch (const std::exception& error) {
            ++failed_;
            Report(module, error.what());
        }
    }

    int Finish() const;

private:
    void Report(std::string_view module, const char* error) const;
    std::string profile_;
    unsigned total_ = 0;
    unsigned failed_ = 0;
};

void TestAndroid(Suite& suite, int api);
void TestGraphics(Suite& suite);
void TestLogAndMath(Suite& suite);
void TestLibc(Suite& suite, int api);
void TestLibcEnvironment(Suite& suite, std::wstring_view first);
void TestAllocations(Suite& suite);
int ProbeLocalListener(const wchar_t* peerName = nullptr);
void TestArm64DataCache(Suite& suite);
void TestSignalSets(Suite& suite);
void TestArm64ImageZlib(Suite& suite, const std::filesystem::path& directory);
void TestMedia(Suite& suite);
void TestImage(Suite& suite, int api, const std::filesystem::path& image);
void InspectArtImage(const std::filesystem::path& image, const std::filesystem::path& cache);
void InspectPreparedArm64Art(const std::filesystem::path& directory, bool framework = false);
void TestArtRuntime(const std::filesystem::path& directory, const std::filesystem::path& dex,
                    const std::filesystem::path& citApk = {}, bool graphics = false, bool resources = false,
                    const std::filesystem::path& artHost = {});
void TestArtServiceSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                          const std::filesystem::path& apk, const std::filesystem::path& artHost,
                          std::wstring_view service);
void TestArtAutomaticUpdateSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                                  const std::filesystem::path& apk, const std::filesystem::path& artHost,
                                  std::wstring_view scenario);
void TestArtForegroundSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                             const std::filesystem::path& apk, const std::filesystem::path& artHost,
                             std::wstring_view expectation);
void TestExternal(Suite& suite, bool includeNetwork = true);
void TestArm64ExecutionProfile(Suite& suite);
void TestArm64Blocks(Suite& suite);
void TestArm64Dispatch(Suite& suite);
void TestArm64StackTrace(Suite& suite);
void TestArm64CompactPc(Suite& suite);
void BenchmarkArm64Blocks(Suite& suite);
void TestUbsanImage(Suite& suite, const std::filesystem::path& elf);
}
