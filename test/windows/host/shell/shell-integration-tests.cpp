#include "../../../../src/windows/host/shell/ShellIntegration.h"
#include "../../../../src/windows/ipc/NotificationIdentity.h"
#include <windows.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <fstream>

namespace {
void Check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

// Keep registration tests isolated from the user's protocol and profiles.
struct Registry {
    std::wstring path = L"Software\\Stone.ShellTests." + std::to_wstring(GetCurrentProcessId());
    HKEY key = nullptr;
    bool overridden = false;

    Registry()
    {
        Check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) ==
                  ERROR_SUCCESS,
              "test registry");
        overridden = RegOverridePredefKey(HKEY_CURRENT_USER, key) == ERROR_SUCCESS;
        if (!overridden) {
            RegCloseKey(key);
            key = nullptr;
            throw std::runtime_error("test registry override");
        }
    }

    ~Registry()
    {
        if (overridden)
            RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (key)
            RegCloseKey(key);
        RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    }
};

std::wstring RegistryValue(const std::wstring& key, const wchar_t* name)
{
    wchar_t value[32768]{};
    DWORD size = sizeof(value);
    Check(RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS,
          "notification registry value");
    return value;
}

void NotificationRegistration(const std::filesystem::path& root)
{
    namespace fs = std::filesystem;
    Registry registry;
    const auto directory = root / std::to_wstring(GetCurrentProcessId());
    const auto settings = directory / L"settings/StoneSettings.exe";
    fs::create_directories(settings.parent_path());
    {
        std::ofstream file(settings);
        file << "settings fixture";
    }
    const auto identity = stone::windows::MakeNotificationIdentity(settings, {});
    const auto applicationKey = L"Software\\Classes\\AppUserModelId\\" + identity.appId;
    const auto commandKey = L"Software\\Classes\\" + identity.protocol + L"\\shell\\open\\command";
    const auto expectedCommand = L"\"" + settings.wstring() + L"\" --notification-activate \"%1\"";
    stone::windows::RegisterNotificationIdentity(settings, {});
    Check(RegistryValue(applicationKey, L"DisplayName") == L"Stone", "notification display name");
    const auto activator = RegistryValue(applicationKey, L"CustomActivator");
    GUID clsid{};
    Check(SUCCEEDED(CLSIDFromString(activator.c_str(), &clsid)), "notification activator GUID");
    Check(RegistryValue(commandKey, nullptr) == expectedCommand, "notification activation command");
    Check(RegistryValue(L"Software\\Classes\\" + identity.protocol, L"URL Protocol").empty(),
          "notification URL protocol");

    stone::windows::RegisterNotificationIdentity(settings, {});
    Check(RegistryValue(applicationKey, L"CustomActivator") == activator &&
              RegistryValue(commandKey, nullptr) == expectedCommand,
          "repeated registration must preserve notification activation identity");
}

void LaunchRegistration(const std::filesystem::path& directory)
{
    namespace fs = std::filesystem;
    fs::create_directories(directory / L"data with spaces");
    const auto service = directory / L"StoneService.exe", image = directory / L"system.img",
               apk = directory / L"original app.apk";
    for (const auto& path : {service, image, apk}) {
        std::ofstream file(path);
        file << "shell fixture";
    }

    Registry registry;

    const std::wstring name = L"com.trebledot.stone.shelltest";
    stone::windows::ConfigureShellIntegration(service, directory / L"data with spaces", image);
    HKEY key = nullptr;
    Check(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\stone", 0, KEY_READ, &key) == ERROR_FILE_NOT_FOUND,
          "configuration must not register a protocol");
    bool rejected = false;
    try {
        stone::windows::ResolveApplicationLaunch(name);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    Check(rejected, "unregistered launch must fail");
    stone::windows::RegisterApplicationLaunch(name, apk);
    const auto root = fs::canonical(directory / L"data with spaces").wstring();
    Check(stone::windows::ResolveApplicationLaunch(name) ==
              std::vector<std::wstring>{L"--apk", fs::canonical(apk).wstring(), L"--data-dir", root, L"--system-image",
                                        fs::canonical(image).wstring()},
          "first run must preserve APK and profile");
    wchar_t command[32768]{};
    DWORD size = sizeof(command);
    Check(RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\stone\\shell\\open\\command", nullptr, RRF_RT_REG_SZ,
                       nullptr, command, &size) == ERROR_SUCCESS,
          "protocol registration");
    Check(std::wstring(command) == L"\"" + fs::canonical(service).wstring() + L"\" /launch \"%1\"", "protocol command");
    stone::windows::ConfigureShellIntegration(service, directory / L"data with spaces", image);
    Check(stone::windows::ResolveApplicationLaunch(name)[0] == L"--apk",
          "configuration must preserve registered launch");
    const auto differentlyCased = name + L".Other";
    stone::windows::RegisterApplicationLaunch(differentlyCased, apk);
    stone::windows::RegisterApplicationLaunch(name + L".other");
    Check(stone::windows::ResolveApplicationLaunch(differentlyCased)[0] == L"--apk" &&
              stone::windows::ResolveApplicationLaunch(name + L".other")[0] == L"--package",
          "case-sensitive package routes");
    stone::windows::RegisterApplicationLaunch(name);
    Check(stone::windows::ResolveApplicationLaunch(name) == std::vector<std::wstring>{L"--package", name, L"--data-dir",
                                                                                      root, L"--system-image",
                                                                                      fs::canonical(image).wstring()},
          "installation must replace original APK launch with installed package");
    rejected = false;
    try {
        stone::windows::ResolveApplicationLaunch(L"../invalid");
    }
    catch (const std::invalid_argument&) {
        rejected = true;
    }
    Check(rejected, "invalid package registration");
}

std::filesystem::path Link(const std::wstring& name)
{
    PWSTR path = nullptr;
    Check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &path)), "start menu directory");
    auto directory = std::filesystem::path(path) / L"Stone";
    CoTaskMemFree(path);
    for (const auto& item : std::filesystem::directory_iterator(directory)) {
        if (item.path().extension() != L".lnk")
            continue;
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        Check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))),
              "shell link");
        Microsoft::WRL::ComPtr<IPersistFile> file;
        Check(SUCCEEDED(link.As(&file)), "link persistence");
        if (FAILED(file->Load(item.path().c_str(), STGM_READ)))
            continue;
        wchar_t text[32768]{};
        if (SUCCEEDED(link->GetArguments(text, static_cast<int>(std::size(text)))) &&
            std::wstring_view(text).find(L"stone://" + name) != std::wstring_view::npos)
            return item.path();
    }
    throw std::runtime_error("owned shortcut not found");
}
}

// Opt-in: this exercises real HKCU protocol registration and Programs/Stone.
int wmain(int argc, wchar_t** argv)
{
    std::wstring name = L"com.trebledot.stone.shelltest." + std::wstring(120, L'a');
    bool configured = false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--launch-registration") {
            LaunchRegistration(std::filesystem::absolute(argv[2]));
            std::puts(
                "PASS deferred protocol registration, first-run APK routing, installed package routing and profile preservation.");
            CoUninitialize();
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--notification-registration") {
            NotificationRegistration(std::filesystem::absolute(argv[2]));
            std::puts("PASS notification identity and protocol activation survive repeated registration.");
            CoUninitialize();
            return 0;
        }
        Check(argc == 5, "usage: shell-tests Service.exe data-directory system.img expected-AppUserModelID");
        stone::windows::ConfigureShellIntegration(argv[1], argv[2], argv[3]);
        configured = true;
        stone::windows::UpdateApplicationShortcut(name, L"Stone shell test", true);
        auto path = Link(name);
        Check(path.filename() == L"Stone shell test.lnk", "shortcut must contain only the application label");
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        Check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))),
              "application shell link");
        Microsoft::WRL::ComPtr<IPersistFile> file;
        Check(SUCCEEDED(link.As(&file)), "application link persistence");
        Check(SUCCEEDED(file->Load(path.c_str(), STGM_READ)), "application link load");
        wchar_t arguments[32768]{}, target[32768]{};
        Check(SUCCEEDED(link->GetArguments(arguments, 32768)) && std::wstring(arguments) == L"/launch stone://" + name,
              "URI-only shortcut arguments");
        Check(SUCCEEDED(link->GetPath(target, 32768, nullptr, SLGP_RAWPATH)) &&
                  std::filesystem::equivalent(target, argv[1]),
              "shortcut Service target");
        file.Reset();
        link.Reset();
        Microsoft::WRL::ComPtr<IPropertyStore> properties;
        Check(
            SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&properties))),
            "shortcut properties");
        PROPVARIANT id{};
        Check(SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &id)), "taskbar identity");
        PWSTR value = nullptr;
        auto result = PropVariantToStringAlloc(id, &value);
        PropVariantClear(&id);
        Check(SUCCEEDED(result), "taskbar identity string");
        std::wstring actual(value);
        CoTaskMemFree(value);
        Check(actual == argv[4] && actual.size() <= 128, "long package identity must match Android window identity");
        properties.Reset();
        // Simulate a prior version's visible suffix, then migrate it through
        // the same update path used by installation and launcher-name changes.
        auto legacy = path.parent_path() / L"Stone shell test (old package)-old-hash.lnk";
        Check(!std::filesystem::exists(legacy), "legacy fixture path is already occupied");
        std::filesystem::rename(path, legacy);
        stone::windows::UpdateApplicationShortcut(name, L"Stone shell test", true);
        Check(Link(name) == path && !std::filesystem::exists(legacy), "legacy shortcut name migration");
        bool rejected = false;
        try {
            stone::windows::UpdateApplicationShortcut(name + L".other", L"Stone shell test", true);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        Check(rejected && Link(name) == path, "duplicate display label must preserve the original application");
        stone::windows::UpdateApplicationShortcut(name, L"Stone renamed shell test", true);
        Check(!std::filesystem::exists(path), "old display label cleanup");
        path = Link(name);
        Check(path.filename() == L"Stone renamed shell test.lnk", "renamed application label");
        stone::windows::UpdateApplicationShortcut(name, L"", false);
        Check(!std::filesystem::exists(path), "shortcut cleanup");
        std::puts(
            "PASS label-only Windows shortcuts, legacy migration, collision protection, taskbar identity and cleanup");
        CoUninitialize();
        return 0;
    }
    catch (const std::exception& error) {
        if (configured)
            try {
                stone::windows::UpdateApplicationShortcut(name, L"", false);
            }
            catch (...) {
            }
        std::fprintf(stderr, "FAIL %s\n", error.what());
        CoUninitialize();
        return 1;
    }
}
