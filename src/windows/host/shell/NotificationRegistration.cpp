#include "../../ipc/NotificationIdentity.h"
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <winrt/base.h>
#include <array>
#include <stdexcept>
#include <cstdio>

namespace stone::windows {
namespace fs = std::filesystem;

namespace {
std::wstring Hash(const std::wstring& text)
{
    std::array<unsigned char, 32> bytes{};
    winrt::check_nt(
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(text.data())),
                   static_cast<ULONG>(text.size() * sizeof(wchar_t)), bytes.data(), static_cast<ULONG>(bytes.size())));
    std::wstring result;
    for (auto value : bytes) {
        result += L"0123456789abcdef"[value >> 4];
        result += L"0123456789abcdef"[value & 15];
    }
    return result;
}
}

void RemoveDiagnosticNotificationIdentity(const fs::path& settings, const fs::path& diagnostic)
{
    if (diagnostic.empty())
        throw std::invalid_argument("Production notification registration cannot be removed by a diagnostic");
    const auto identity = MakeNotificationIdentity(settings, diagnostic);
    for (const auto& key :
         {L"Software\\Classes\\" + identity.protocol, L"Software\\Classes\\AppUserModelId\\" + identity.appId}) {
        const auto result = RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
            std::fprintf(stderr, "[notifications] test registration cleanup failed: %ld\n", result);
    }
    PWSTR programs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
        const auto path =
            fs::path(programs) / L"Stone" / (L"Stone Test " + Hash(identity.appId).substr(0, 8) + L".lnk");
        CoTaskMemFree(programs);
        if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
            std::fprintf(stderr, "[notifications] test shortcut cleanup failed\n");
    }
}

void RegistryString(HKEY root, const wchar_t* name, const std::wstring& text)
{
    winrt::check_win32(RegSetValueExW(root, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
                                      static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t))));
}

void RegisterNotificationIdentity(const fs::path& settingsExecutable, const fs::path& diagnosticExecutable)
{
    winrt::check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    struct Apartment {
        ~Apartment()
        {
            CoUninitialize();
        }
    } apartment;

    const auto sender = MakeNotificationIdentity(settingsExecutable, diagnosticExecutable);
    const auto& appId = sender.appId;
    const auto& protocol = sender.protocol;
    const auto activatorHash = Hash(appId);
    const auto activatorText = L"{" + activatorHash.substr(0, 8) + L"-" + activatorHash.substr(8, 4) + L"-" +
                               activatorHash.substr(12, 4) + L"-" + activatorHash.substr(16, 4) + L"-" +
                               activatorHash.substr(20, 12) + L"}";
    GUID activator{};
    winrt::check_hresult(CLSIDFromString(activatorText.c_str(), &activator));
    // Unpackaged toast discovery reads these display assets from the registry.
    HKEY application = nullptr;
    winrt::check_win32(RegCreateKeyExW(HKEY_CURRENT_USER, (L"Software\\Classes\\AppUserModelId\\" + appId).c_str(), 0,
                                       nullptr, 0, KEY_SET_VALUE, nullptr, &application, nullptr));
    try {
        RegistryString(application, L"DisplayName", diagnosticExecutable.empty() ? L"Stone" : L"Stone Test");
        const auto icon = settingsExecutable.parent_path() / L"Assets/StoreLogo.png";
        if (fs::is_regular_file(icon))
            RegistryString(application, L"IconUri", icon.wstring());
        RegistryString(application, L"IconBackgroundColor", L"FFDDDDDD");
        RegistryString(application, L"CustomActivator", activatorText);
    }
    catch (...) {
        RegCloseKey(application);
        throw;
    }
    RegCloseKey(application);
    const auto commandKey = L"Software\\Classes\\" + protocol + L"\\shell\\open\\command";
    HKEY command = nullptr;
    winrt::check_win32(RegCreateKeyExW(HKEY_CURRENT_USER, commandKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                                       &command, nullptr));
    try {
        RegistryString(command, nullptr,
                       L"\"" + settingsExecutable.wstring() + L"\" --notification-activate \"%1\"" +
                           (diagnosticExecutable.empty() ? L""
                                                         : L" --notification-activation-test-server \"" +
                                                               diagnosticExecutable.wstring() + L"\""));
    }
    catch (...) {
        RegCloseKey(command);
        throw;
    }
    RegCloseKey(command);
    HKEY scheme = nullptr;
    winrt::check_win32(RegCreateKeyExW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + protocol).c_str(), 0, nullptr, 0,
                                       KEY_SET_VALUE, nullptr, &scheme, nullptr));
    try {
        RegistryString(scheme, nullptr, L"URL:Stone notification");
        RegistryString(scheme, L"URL Protocol", L"");
    }
    catch (...) {
        RegCloseKey(scheme);
        throw;
    }
    RegCloseKey(scheme);
    // Normal startup must not publish a second settings entry or rewrite a Start
    // menu shortcut. Notification identity and protocol activation are registered
    // above; the installer owns the settings entry. Only opt-in diagnostics need
    // an isolated test shortcut.
    if (diagnosticExecutable.empty())
        return;

    PWSTR programs = nullptr;
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_CREATE, nullptr, &programs));
    fs::path directory(programs);
    CoTaskMemFree(programs);
    directory /= L"Stone";
    fs::create_directories(directory);
    winrt::com_ptr<IShellLinkW> link;
    winrt::check_hresult(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())));
    winrt::check_hresult(link->SetPath(settingsExecutable.c_str()));
    winrt::check_hresult(link->SetDescription(L"Stone Android application settings"));
    auto properties = link.as<IPropertyStore>();
    PROPVARIANT identity{};
    winrt::check_hresult(InitPropVariantFromString(appId.c_str(), &identity));
    const auto status = properties->SetValue(PKEY_AppUserModel_ID, identity);
    PropVariantClear(&identity);
    winrt::check_hresult(status);
    // All toast interactions use protocol activation; the shortcut still needs
    // an activator identity for unpackaged notification discovery.
    PROPVARIANT activation{};
    winrt::check_hresult(InitPropVariantFromCLSID(activator, &activation));
    const auto activationStatus = properties->SetValue(PKEY_AppUserModel_ToastActivatorCLSID, activation);
    PropVariantClear(&activation);
    winrt::check_hresult(activationStatus);
    winrt::check_hresult(properties->Commit());
    const auto shortcutName = L"Stone Test " + Hash(appId).substr(0, 8) + L".lnk";
    winrt::check_hresult(link.as<IPersistFile>()->Save((directory / shortcutName).c_str(), TRUE));
}
}
