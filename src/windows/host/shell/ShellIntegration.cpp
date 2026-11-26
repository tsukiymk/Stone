#include "ShellIntegration.h"
#include "../CommandLine.h"
#include "../UserPaths.h"
#include <shlobj.h>
#include <bcrypt.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>

namespace stone::windows {
namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
std::mutex gate;
fs::path client, data, image;
std::map<fs::path, std::pair<fs::path, fs::path>> profiles;

void Check(HRESULT result)
{
    if (FAILED(result))
        throw std::runtime_error("Windows shell integration failed");
}

struct Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    Apartment()
    {
        if (result != RPC_E_CHANGED_MODE)
            Check(result);
    }

    ~Apartment()
    {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};

void RegistryString(HKEY key, const wchar_t* name, const std::wstring& value)
{
    if (RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot register the stone URI protocol for the current user");
}

void RegisterProtocol()
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\stone", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot register the stone URI protocol");
    try {
        RegistryString(key, nullptr, L"URL:Stone Android application");
        RegistryString(key, L"URL Protocol", L"");
    }
    catch (...) {
        RegCloseKey(key);
        throw;
    }
    RegCloseKey(key);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\stone\\shell\\open\\command", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot register the stone URI handler");
    try {
        RegistryString(key, nullptr, stone::session::Quote(client.wstring()) + L" /launch \"%1\"");
    }
    catch (...) {
        RegCloseKey(key);
        throw;
    }
    RegCloseKey(key);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

constexpr auto launchProfiles = L"Software\\Stone\\LaunchProfiles";
std::wstring Digest(const std::wstring& text);

std::vector<std::wstring> ReadLaunch(const std::wstring& name)
{
    const auto value = Digest(name); // Android package identities are case-sensitive.
    DWORD size = 0;
    auto result =
        RegGetValueW(HKEY_CURRENT_USER, launchProfiles, value.c_str(), RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size);
    if (result == ERROR_FILE_NOT_FOUND)
        return {};
    if (result != ERROR_SUCCESS || size < 2 * sizeof(wchar_t) || size > 262144 || size % sizeof(wchar_t))
        throw std::runtime_error("Cannot read application launch registration");
    std::vector<wchar_t> bytes(size / sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, launchProfiles, value.c_str(), RRF_RT_REG_MULTI_SZ, nullptr, bytes.data(),
                     &size) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot read application launch registration");
    std::vector<std::wstring> fields;
    for (size_t at = 0; at < size / sizeof(wchar_t) && bytes[at];) {
        auto end = std::find(bytes.begin() + at, bytes.begin() + size / sizeof(wchar_t), L'\0');
        if (end == bytes.begin() + size / sizeof(wchar_t))
            throw std::runtime_error("Invalid application launch registration");
        fields.emplace_back(bytes.data() + at, static_cast<size_t>(end - (bytes.begin() + at)));
        at += fields.back().size() + 1;
    }
    if (fields.size() != 4 || (fields[2] != L"--apk" && fields[2] != L"--package") ||
        !fs::path(fields[0]).is_absolute() || !fs::path(fields[1]).is_absolute() ||
        (fields[2] == L"--apk" && !fs::path(fields[3]).is_absolute()))
        throw std::runtime_error("Invalid application launch registration");
    return fields;
}

void RegisterLaunch(const std::wstring& name, const fs::path& source)
{
    if (client.empty())
        return;
    const std::vector<std::wstring> fields{data.wstring(), image.wstring(), source.empty() ? L"--package" : L"--apk",
                                           source.empty() ? name : fs::canonical(source).wstring()};
    std::vector<wchar_t> bytes;
    for (const auto& field : fields) {
        bytes.insert(bytes.end(), field.begin(), field.end());
        bytes.push_back(L'\0');
    }
    bytes.push_back(L'\0');
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, launchProfiles, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        throw std::runtime_error("Cannot register application launch profile");
    const auto result =
        RegSetValueExW(key, Digest(name).c_str(), 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(bytes.data()),
                       static_cast<DWORD>(bytes.size() * sizeof(wchar_t)));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
        throw std::runtime_error("Cannot register application launch profile");
    RegisterProtocol();
}

fs::path Programs()
{
    PWSTR path = nullptr;
    Check(SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_CREATE, nullptr, &path));
    fs::path result(path);
    CoTaskMemFree(path);
    return result / L"Stone";
}

std::wstring LegacyMarker(const std::wstring& name)
{
    return L"Stone.Application|" + data.wstring() + L"|" + name;
}

std::wstring BytesDigest(std::span<const uint8_t> bytes)
{
    std::array<unsigned char, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
                   static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0)
        throw std::runtime_error("Cannot identify application shortcut");
    std::wstring result;
    for (auto byte : digest) {
        result += L"0123456789abcdef"[byte >> 4];
        result += L"0123456789abcdef"[byte & 15];
    }
    return result;
}

std::wstring Digest(const std::wstring& text)
{
    return BytesDigest({reinterpret_cast<const uint8_t*>(text.data()), text.size() * sizeof(wchar_t)});
}

std::wstring Marker(const std::wstring& name)
{
    return L"Stone.Application|" + Digest(LegacyMarker(name));
}

std::wstring AppId(const std::wstring& name)
{
    const std::wstring prefix = L"Trebledot.Stone.";
    if (prefix.size() + name.size() <= 128)
        return prefix + name;
    // Match ArtWindow's UUID.nameUUIDFromBytes(packageName UTF-8) fallback.
    // Package names have already been restricted to ASCII.
    std::string bytes;
    bytes.reserve(name.size());
    for (wchar_t ch : name)
        bytes.push_back(static_cast<char>(ch));
    std::array<unsigned char, 16> digest{};
    if (BCryptHash(BCRYPT_MD5_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(bytes.data()),
                   static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0)
        throw std::runtime_error("Cannot identify application taskbar entry");
    digest[6] = (digest[6] & 0x0f) | 0x30;
    digest[8] = (digest[8] & 0x3f) | 0x80;
    std::wstring result = prefix;
    for (size_t index = 0; index < digest.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            result += L'-';
        result += L"0123456789abcdef"[digest[index] >> 4];
        result += L"0123456789abcdef"[digest[index] & 15];
    }
    return result;
}

bool OwnedShortcut(const fs::path& path, const std::wstring& name)
{
    ComPtr<IShellLinkW> link;
    Check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)));
    ComPtr<IPersistFile> file;
    Check(link.As(&file));
    if (FAILED(file->Load(path.c_str(), STGM_READ)))
        return false;
    wchar_t description[32768]{};
    if (FAILED(link->GetDescription(description, static_cast<int>(std::size(description)))))
        return false;
    const auto legacy = LegacyMarker(name);
    const auto suffix = L"-" + Digest(legacy).substr(0, 16) + L".lnk";
    return description == Marker(name) || description == legacy ||
           (legacy.size() > 260 && description == legacy.substr(0, 260) && path.filename().wstring().ends_with(suffix));
}

void RemoveShortcuts(const fs::path& directory, const std::wstring& name, const fs::path& keep = {})
{
    if (!fs::is_directory(directory))
        return;
    for (const auto& item : fs::directory_iterator(directory)) {
        if (!item.is_regular_file() || item.path().extension() != L".lnk")
            continue;
        if (!keep.empty() && CompareStringOrdinal(item.path().c_str(), -1, keep.c_str(), -1, TRUE) == CSTR_EQUAL)
            continue;
        if (OwnedShortcut(item.path(), name)) {
            fs::remove(item.path());
            SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW, item.path().c_str(), nullptr);
        }
    }
}

fs::path IconDirectory(const std::wstring& name)
{
    return UserStateDirectory() / L"Shell" / Digest(LegacyMarker(name));
}

fs::path CurrentIcon(const std::wstring& name)
{
    std::ifstream file(IconDirectory(name) / L"current");
    std::string digest;
    if (!(file >> digest) || digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != digest.npos)
        return {};
    auto path = IconDirectory(name) / (digest + ".ico");
    return fs::is_regular_file(path) ? path : fs::path{};
}

void Publish(const fs::path& path, std::span<const uint8_t> bytes)
{
    auto temporary = path;
    temporary += L".tmp";
    HANDLE file =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create application icon cache");
    DWORD written = 0;
    const bool saved = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                       written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!saved || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Cannot publish application icon cache");
    }
}

void UpdateShortcut(const std::wstring& name, const std::wstring& label, bool installed);

ComPtr<IShellLinkW> ApplicationLink(const std::wstring& name, const fs::path& icon)
{
    ComPtr<IShellLinkW> link;
    Check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)));
    Check(link->SetPath(client.c_str()));
    Check(link->SetWorkingDirectory(client.parent_path().c_str()));
    const auto arguments = L"/launch stone://" + name;
    Check(link->SetArguments(arguments.c_str()));
    Check(link->SetDescription(Marker(name).c_str()));
    Check(link->SetShowCmd(SW_SHOWNORMAL));
    if (!icon.empty())
        Check(link->SetIconLocation(icon.c_str(), 0));
    ComPtr<IPropertyStore> properties;
    Check(link.As(&properties));
    PROPVARIANT id{};
    Check(InitPropVariantFromString(AppId(name).c_str(), &id));
    const auto assigned = properties->SetValue(PKEY_AppUserModel_ID, id);
    PropVariantClear(&id);
    Check(assigned);
    Check(properties->Commit());
    return link;
}

void PublishLink(IShellLinkW* link, const fs::path& destination)
{
    fs::path temporary = destination;
    temporary += L".tmp";
    ComPtr<IPersistFile> file;
    Check(link->QueryInterface(IID_PPV_ARGS(&file)));
    Check(file->Save(temporary.c_str(), TRUE));
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Cannot publish application shortcut");
    }
}
}

void ConfigureShellIntegration(const fs::path& executable, const fs::path& root, const fs::path& systemImage)
{
    const std::lock_guard lock(gate);
    client = fs::canonical(executable);
    data = fs::canonical(root);
    image = fs::canonical(systemImage);
    profiles[data] = {client, image};
}

bool ShellIntegrationEnabled()
{
    const std::lock_guard lock(gate);
    return !client.empty();
}

void RegisterApplicationLaunch(const std::wstring& name, const fs::path& source)
{
    if (!stone::session::IsPackageName(name))
        throw std::invalid_argument("Invalid launch package name");
    const std::lock_guard lock(gate);
    RegisterLaunch(name, source);
}

std::vector<std::wstring> ResolveApplicationLaunch(const std::wstring& name)
{
    if (!stone::session::IsPackageName(name))
        throw std::invalid_argument("Invalid launch package name");
    const std::lock_guard lock(gate);
    const auto fields = ReadLaunch(name);
    if (fields.empty())
        throw std::runtime_error("Application has not been run or installed");
    if (fields[2] == L"--package" && fields[3] != name)
        throw std::runtime_error("Application launch identity changed");
    return {fields[2], fields[3], L"--data-dir", fields[0], L"--system-image", fields[1]};
}

void UpdateApplicationShortcut(const std::wstring& name, const std::wstring& label, bool installed)
{
    if (!stone::session::IsPackageName(name))
        throw std::invalid_argument("Invalid shortcut package name");
    const std::lock_guard lock(gate);
    UpdateShortcut(name, label, installed);
}

namespace {
void UpdateShortcut(const std::wstring& name, const std::wstring& label, bool installed)
{
    if (client.empty())
        return;
    Apartment apartment;
    auto directory = Programs();
    if (!installed) {
        const auto launch = ReadLaunch(name);
        if (!launch.empty() && fs::path(launch[0]) == data) {
            const auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, launchProfiles, Digest(name).c_str());
            if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
                throw std::runtime_error("Cannot remove application launch registration");
        }
        RemoveShortcuts(directory, name);
        const auto icons = IconDirectory(name);
        if (fs::is_directory(icons)) {
            for (const auto& item : fs::directory_iterator(icons)) {
                auto stem = item.path().stem().wstring();
                if (item.is_regular_file() &&
                    (item.path().filename() == L"current" || item.path().filename() == L"launch.lnk" ||
                     (item.path().extension() == L".ico" && stem.size() == 64 &&
                      stem.find_first_not_of(L"0123456789abcdef") == stem.npos)))
                    fs::remove(item.path());
            }
            std::error_code ignored;
            fs::remove(icons, ignored);
        }
        return;
    }
    RegisterLaunch(name, {});
    fs::create_directories(directory);
    std::wstring title = label.empty() ? L"Android app" : label.substr(0, 80);
    for (auto& ch : title)
        if (ch < L' ' || std::wstring_view(L"<>:\"/\\|?*").find(ch) != std::wstring_view::npos)
            ch = L'_';
    while (!title.empty() && (title.back() == L'.' || title.back() == L' '))
        title.pop_back();
    if (title.empty())
        title = L"Android app";
    auto destination = directory / (title + L".lnk");
    auto publication = destination.wstring();
    CharUpperBuffW(publication.data(), static_cast<DWORD>(publication.size()));
    HANDLE mutex = CreateMutexW(nullptr, FALSE, (L"Local\\Stone.Shortcut." + Digest(publication)).c_str());
    if (!mutex)
        throw std::runtime_error("Cannot lock application shortcut publication");

    struct Release {
        HANDLE value;
        bool acquired = false;

        ~Release()
        {
            if (acquired)
                ReleaseMutex(value);
            CloseHandle(value);
        }
    } release{mutex};

    const auto waited = WaitForSingleObject(mutex, INFINITE);
    release.acquired = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    if (!release.acquired)
        throw std::runtime_error("Cannot lock application shortcut publication");
    // Display only the application label. Ownership remains in the link's
    // description, and conflicting labels must not overwrite another profile.
    if (fs::exists(destination) && !OwnedShortcut(destination, name))
        throw std::runtime_error("Application shortcut name is already used by another application or profile");
    const bool existed = fs::is_regular_file(destination);
    auto link = ApplicationLink(name, CurrentIcon(name));
    PublishLink(link.Get(), destination);
    RemoveShortcuts(directory, name, destination);
    SHChangeNotify(existed ? SHCNE_UPDATEITEM : SHCNE_CREATE, SHCNF_PATHW, destination.c_str(), nullptr);
}
}

std::vector<std::wstring> CacheApplicationIcon(const fs::path& root, const std::wstring& name,
                                               const std::wstring& label, std::span<const uint8_t> png,
                                               bool requireShortcut)
{
    if (!stone::session::IsPackageName(name) || label.size() > 1024 || png.empty() || png.size() > 1024 * 1024)
        throw std::invalid_argument("Invalid application icon");
    const std::lock_guard lock(gate);
    auto profile = profiles.find(root);
    if (profile == profiles.end())
        throw std::invalid_argument("Application icon profile is not registered");
    client = profile->second.first;
    data = profile->first;
    image = profile->second.second;
    Apartment apartment;
    auto programs = Programs();
    bool installed = false;
    if (fs::is_directory(programs))
        for (const auto& item : fs::directory_iterator(programs))
            if (item.is_regular_file() && item.path().extension() == L".lnk" && OwnedShortcut(item.path(), name)) {
                installed = true;
                break;
            }
    if (requireShortcut && !installed)
        throw std::invalid_argument("Application icon target has no managed shortcut");
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;
    Check(factory->CreateStream(&stream));
    Check(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    Check(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder));
    GUID format{};
    Check(decoder->GetContainerFormat(&format));
    UINT count = 0;
    Check(decoder->GetFrameCount(&count));
    if (format != GUID_ContainerFormatPng || count != 1)
        throw std::invalid_argument("Application icon must be a single PNG");
    ComPtr<IWICBitmapFrameDecode> frame;
    Check(decoder->GetFrame(0, &frame));
    UINT width = 0, height = 0;
    Check(frame->GetSize(&width, &height));
    if (width != 256 || height != 256)
        throw std::invalid_argument("Application icon must be 256 by 256 pixels");
    ComPtr<IWICFormatConverter> pixels;
    Check(factory->CreateFormatConverter(&pixels));
    Check(pixels->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                             WICBitmapPaletteTypeCustom));
    std::vector<BYTE> decoded(256 * 256 * 4);
    Check(pixels->CopyPixels(nullptr, 256 * 4, static_cast<UINT>(decoded.size()), decoded.data()));
    auto digest = BytesDigest(png);
    auto directory = IconDirectory(name);
    fs::create_directories(directory);
    std::vector<uint8_t> ico{0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 32, 0};
    auto little = [&](uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            ico.push_back(static_cast<uint8_t>(value >> shift));
    };
    little(static_cast<uint32_t>(png.size()));
    little(22);
    ico.insert(ico.end(), png.begin(), png.end());
    auto path = directory / (digest + L".ico");
    Publish(path, ico);
    std::string identity(digest.begin(), digest.end());
    Publish(directory / L"current", {reinterpret_cast<const uint8_t*>(identity.data()), identity.size()});
    // Keep the relaunch target stable when an APK changes its display name.
    // The taskbar's window property rejects the full, potentially long command.
    auto relaunch = directory / L"launch.lnk";
    auto link = ApplicationLink(name, path);
    PublishLink(link.Get(), relaunch);
    if (installed)
        UpdateShortcut(name, label, true);
    return {path.wstring(), stone::session::Quote(relaunch.wstring())};
}
}
