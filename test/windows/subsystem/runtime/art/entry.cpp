#include "../../support/test-suite.h"
#include "runtime/art/ArtRuntime.h"
#include "runtime/art/ArtGs.h"
#include "test-client.h"
#include "../../../host/audio/audio-loopback.h"
#include <windows.h>
#include <iostream>

namespace {
void Field(std::string_view text)
{
    std::cout << '"';
    for (const auto ch : text) {
        if (ch == '"')
            std::cout << '"';
        std::cout << ch;
    }
    std::cout << '"';
}

void Row(std::string_view kind, std::string_view consumer, std::string_view provider, std::string_view symbol)
{
    Field(kind);
    std::cout << '\t';
    Field(consumer);
    std::cout << '\t';
    Field(provider);
    std::cout << '\t';
    Field(symbol);
    std::cout << '\n';
}
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc == 5 && std::wstring_view(argv[1]) == L"--capture-audio") {
        try {
            const auto pid = std::stoul(argv[2]), duration = std::stoul(argv[4]);
            if (!pid || duration < 1000 || duration > 60000)
                throw std::invalid_argument("Capture needs a PID and 1000-60000 milliseconds");
            stone::windows::test::WithAudioLoopback(
                [&] {
                    Sleep(duration);
                },
                argv[3], pid, false);
            return 0;
        }
        catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
        std::wcout
            << L"Usage: stone-art-probe <prepared-system-directory> <probe.dex> [stone-cit.apk [--graphics|--resources]]\n";
        std::wcout
            << L"       stone-art-probe <prepared-system-directory> <probe.dex> <stone-cit.apk> --application <stone-art-runtime.jar>\n";
        std::wcout
            << L"       stone-art-probe <prepared-system-directory> <probe.dex> <stone-cit.apk> --resize-window <stone-art-runtime.jar>\n";
        std::wcout
            << L"       stone-art-probe <prepared-system-directory> <probe.dex> <stone-cit.apk> --startup-services <stone-art-runtime.jar>\n";
        std::wcout
            << L"       stone-art-probe <prepared-system-directory> <probe.dex> <stone-cit.apk> --startup-natives <stone-art-runtime.jar>\n";
        std::wcout
            << L"       stone-art-probe <prepared-system-directory> <probe.dex> <stone-cit.apk> --video-codecs <stone-art-runtime.jar>\n";
        std::wcout
            << L"       stone-art-probe <prepared-system-directory> <probe.dex> <stone-cit.apk> --audio-output <stone-art-runtime.jar>\n";
        std::wcout
            << L"       stone-art-probe <prepared-system-directory> <probe.dex> <stone-cit.apk> --audio-codecs <stone-art-runtime.jar>\n";
        std::wcout << L"       stone-art-probe --capture-audio <pid> <output.wav> <milliseconds>\n";
        std::wcout
            << L"       stone-art-probe <prepared-system-directory> <probe.dex> <stone-cit.apk> --storage <stone-art-runtime.jar> <write|read>\n";
        std::wcout
            << L"       stone-art-probe <prepared-system-directory> <probe.dex> <sample.apk> --service <stone-art-runtime.jar> <service-class>\n";
        std::wcout
            << L"       stone-art-probe <prepared-system-directory> <probe.dex> <installer.apk> --automatic-updates <stone-art-runtime.jar> <allowed|denied|baseline>\n";
        std::wcout
            << L"       stone-art-probe <prepared-system-directory> <probe.dex> <sample.apk> --foreground <stone-art-runtime.jar> <allowed|denied>\n";
        std::wcout
            << L"       stone-art-probe <prepared-system-directory> <probe.dex> <sample.apk> --battery <stone-art-runtime.jar>\n";
        std::wcout
            << L"       stone-art-probe <prepared-system-directory> <probe.dex> <sample.apk> --jobs <stone-art-runtime.jar> <fixture|recover>\n";
        std::wcout
            << L"       stone-art-probe <prepared-system-directory> <probe.dex> <sample.apk> --native-libraries <stone-art-runtime.jar>\n";
        std::wcout << L"       stone-art-probe --inspect <prepared-system-directory> <library.so>\n";
        std::wcout << L"       stone-art-probe --symbols <prepared-system-directory> <library.so>\n";
        std::wcout << L"       stone-art-probe --audit-imports <prepared-system-directory> <library.so>\n";
        std::wcout << L"       stone-art-probe --stub-overlaps <prepared-system-directory>\n";
        return 0;
    }
    try {
        if (const auto result = stone::windows::test::RunArtTestClient(argc, argv))
            return *result;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    const bool application = argc == 6 && std::wstring_view(argv[4]) == L"--application";
    const bool service = argc == 7 && std::wstring_view(argv[4]) == L"--service";
    const bool battery = argc == 6 && std::wstring_view(argv[4]) == L"--battery";
    const bool storage = argc == 7 && std::wstring_view(argv[4]) == L"--storage";
    const bool jobs = argc == 7 && std::wstring_view(argv[4]) == L"--jobs";
    const bool automaticUpdates = argc == 7 && std::wstring_view(argv[4]) == L"--automatic-updates";
    const bool foreground = argc == 7 && std::wstring_view(argv[4]) == L"--foreground";
    const bool nativeLibraries = argc == 6 && std::wstring_view(argv[4]) == L"--native-libraries";
    const bool resizeWindow = argc == 6 && std::wstring_view(argv[4]) == L"--resize-window";
    const bool startupNatives = argc == 6 && std::wstring_view(argv[4]) == L"--startup-natives";
    const bool videoCodecs = argc == 6 && std::wstring_view(argv[4]) == L"--video-codecs";
    const bool startupServices =
        argc == 6 && (std::wstring_view(argv[4]) == L"--startup-services" || startupNatives || videoCodecs);
    const bool audioCodecs = argc == 6 && std::wstring_view(argv[4]) == L"--audio-codecs";
    const bool audioOutput = argc == 6 && (std::wstring_view(argv[4]) == L"--audio-output" || audioCodecs);
    if (argc < 3 || argc > 7 || (argc == 7 && !service && !foreground && !jobs && !automaticUpdates && !storage) ||
        (argc == 6 && !application && !battery && !nativeLibraries && !resizeWindow && !startupServices &&
         !audioOutput) ||
        (argc == 5 && std::wstring_view(argv[4]) != L"--graphics" && std::wstring_view(argv[4]) != L"--resources")) {
        std::wcerr << L"Invalid arguments; use stone-art-probe --help.\n";
        return 2;
    }
    try {
        stone::windows::test::ConfigureArtTestService();
        if (storage) {
            stone::windows::test::TestArtStorageSample(argv[1], argv[2], argv[3], argv[5], argv[6]);
            return 0;
        }
        if (jobs) {
            stone::windows::test::TestArtJobSample(argv[1], argv[2], argv[3], argv[5], argv[6]);
            return 0;
        }
        if (automaticUpdates) {
            stone::windows::test::TestArtAutomaticUpdateSample(argv[1], argv[2], argv[3], argv[5], argv[6]);
            return 0;
        }
        if (battery) {
            stone::windows::test::TestArtBatterySample(argv[1], argv[2], argv[3], argv[5]);
            return 0;
        }
        if (foreground) {
            stone::windows::test::TestArtForegroundSample(argv[1], argv[2], argv[3], argv[5], argv[6]);
            return 0;
        }
        if (nativeLibraries) {
            stone::windows::test::TestArtNativeLibrariesSample(argv[1], argv[2], argv[3], argv[5]);
            return 0;
        }
        if (resizeWindow) {
            stone::windows::test::TestArtResizeWindowSample(argv[1], argv[2], argv[3], argv[5]);
            return 0;
        }
        if (audioOutput) {
            stone::windows::test::TestArtAudioOutputSample(argv[1], argv[2], argv[3], argv[5], audioCodecs);
            return 0;
        }
        if (startupServices) {
            stone::windows::test::TestArtStartupServicesSample(argv[1], argv[2], argv[3], argv[5], startupNatives,
                                                               videoCodecs);
            return 0;
        }
        if (service) {
            stone::windows::test::TestArtServiceSample(argv[1], argv[2], argv[3], argv[5], argv[6]);
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--stub-overlaps") {
            Row("Kind", "Consumer", "Provider", "Symbol");
            for (const auto& library :
                 stone::windows::InspectAndroidStubExports(stone::windows::ArtRuntimeLibraries(argv[2]))) {
                const auto preference = library.imagePreferred ? "image" : "host-if-available";
                if (!library.imageAvailable) {
                    Row("image-unavailable", library.library, preference, "");
                    continue;
                }
                Row("overlap-count", library.library, preference, std::to_string(library.overlappingExports.size()));
                for (const auto& symbol : library.overlappingExports)
                    Row("overlap", library.library, preference, symbol);
            }
            return 0;
        }
        if (argc == 4 && std::wstring_view(argv[1]) == L"--audit-imports") {
            const auto name = std::filesystem::path(argv[3]).string();
            const auto report =
                stone::windows::InspectAndroidElf(name, "", stone::windows::ArtRuntimeLibraries(argv[2]), true);
            Row("Kind", "Consumer", "Provider", "Symbol");
            Row("library-count", name, "", std::to_string(report.libraries));
            for (const auto& binding : report.bindings)
                Row(binding.hostBridge ? "host" : "image", binding.consumer, binding.provider, binding.symbol);
            for (const auto& missing : report.missingImports)
                Row("missing", name, "", missing);
            return report.missingImports.empty() ? 0 : 1;
        }
        if (argc == 4 && std::wstring_view(argv[1]) == L"--symbols") {
            const auto name = std::filesystem::path(argv[3]).string();
            for (const auto& [symbol, value] :
                 stone::windows::ElfDebugSymbols(stone::windows::ArtRuntimeLibraries(argv[2])(name)))
                std::cout << symbol << ' ' << std::hex << value.address << ' ' << value.size << '\n';
            return 0;
        }
        if (argc == 4 && std::wstring_view(argv[1]) == L"--inspect") {
            const auto name = std::filesystem::path(argv[3]).string();
            const auto report =
                stone::windows::InspectAndroidElf(name, "", stone::windows::ArtRuntimeLibraries(argv[2]));
            std::cout << name << ": " << report.libraries << " libraries, " << report.missingImports.size()
                      << " missing imports.\n";
            for (const auto& symbol : report.missingImports)
                std::cout << symbol << '\n';
            return report.missingImports.empty() ? 0 : 1;
        }
        stone::windows::test::TestArtRuntime(
            argv[1], argv[2], argc >= 4 ? argv[3] : L"", argc == 5 && std::wstring_view(argv[4]) == L"--graphics",
            argc == 5 && std::wstring_view(argv[4]) == L"--resources", application ? argv[5] : L"");
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
