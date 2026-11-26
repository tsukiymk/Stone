#include "test-client.h"
#include "../../../../../src/windows/client/ArtLaunch.h"
#include "../../../../../src/windows/client/BrokerClient.h"
#include "../../../../../src/windows/host/ProcessLog.h"
#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace stone::windows::test {
std::optional<int> RunArtTestClient(int argc, wchar_t* argv[])
{
    // Explicit developer host for native confirmation diagnostics.
    // StoneClient.exe never reads these test variables.
    if (argc > 2 && std::wstring_view(argv[1]) == L"--manage-package") {
        ConfigureArtTestService();
        wchar_t runtime[32768]{}, diagnostics[32768]{};
        const auto size = GetEnvironmentVariableW(L"STONE_MANAGEMENT_RUNTIME", runtime, 32768);
        const auto diagnosticSize = GetEnvironmentVariableW(L"STONE_SETTINGS_TEST_ARGUMENTS", diagnostics, 32768);
        if (!size || size >= 32768 || diagnosticSize >= 32768)
            throw std::runtime_error("Invalid management test configuration");
        return LaunchArtPackageManagement(std::vector<std::wstring>(argv + 1, argv + argc), runtime, diagnostics);
    }
    bool applicationRequest = false;
    for (int index = 1; index < argc; ++index)
        applicationRequest |=
            std::wstring_view(argv[index]) == L"--apk" || std::wstring_view(argv[index]) == L"--package";
    if (!applicationRequest)
        return std::nullopt;

    std::wstring own(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, own.data(), static_cast<DWORD>(own.size()));
    if (!size || size >= own.size())
        throw std::runtime_error("Cannot locate the isolated runtime");
    const auto directory = std::filesystem::path(own.substr(0, size)).parent_path();
    StartProcessLog();
    bool background = false, jit = false;
    std::vector<std::wstring> arguments;
    for (int index = 1; index < argc; ++index) {
        if (std::wstring_view(argv[index]) == L"--background")
            background = true;
        else if (std::wstring_view(argv[index]) == L"--enable-jit")
            jit = true;
        else
            arguments.emplace_back(argv[index]);
    }
    return LaunchArtApplication(arguments, directory, jit, background);
}

void ConfigureArtTestService()
{
    wchar_t service[32768]{};
    const auto size = GetEnvironmentVariableW(L"STONE_PROBE_SERVICE", service, 32768);
    if (size >= 32768)
        throw std::runtime_error("Invalid probe service path");
    ConfigureClientService(size ? service : STONE_TEST_SERVICE);
}
}
