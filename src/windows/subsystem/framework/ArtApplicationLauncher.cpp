#include "ArtApplicationLauncher.h"
#include "../filesystem/AndroidSystemFiles.h"
#include "../../client/Window.h"
#include "../../client/BrokerClient.h"
#include "../../client/ClientApplicationLogs.h"
#include "../../host/ProcessLog.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace stone::windows {
namespace {
namespace fs = std::filesystem;
std::mutex gate;
fs::path executable, dataRoot, systemImage;
std::wstring runningProperty, runningPackage;

struct Process {
    HANDLE handle;

    explicit Process(HANDLE value) : handle(value)
    {
    }

    ~Process()
    {
        CloseHandle(handle);
    }
};

std::map<std::wstring, std::unique_ptr<Process>> children;

std::wstring Quote(const fs::path& path)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : path.wstring()) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (ch == L'\"' ? 2 : 1), L'\\');
        slashes = 0;
        if (ch == L'\"')
            result.push_back(L'\\');
        result.push_back(ch);
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}

std::wstring WindowProperty(const std::wstring& name)
{
    const auto text = fs::canonical(dataRoot / L"data").wstring() + L"\n" + name;
    std::array<unsigned char, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(text.data())),
                   static_cast<ULONG>(text.size() * sizeof(wchar_t)), digest.data(),
                   static_cast<ULONG>(digest.size())) < 0)
        throw std::runtime_error("could not identify application window");
    std::wstring property = L"Stone.Application.";
    for (auto value : digest) {
        property += L"0123456789abcdef"[value >> 4];
        property += L"0123456789abcdef"[value & 15];
    }
    return property;
}

struct Search {
    std::wstring property;
    HWND window = nullptr;
};

BOOL CALLBACK FindWindow(HWND window, LPARAM value)
{
    auto& search = *reinterpret_cast<Search*>(value);
    if (!GetPropW(window, search.property.c_str()))
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return TRUE;
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const bool queried = QueryFullProcessImageNameW(process, 0, path.data(), &length) != FALSE;
    CloseHandle(process);
    std::error_code error;
    if (!queried ||
        !fs::equivalent(fs::path(path.substr(0, length)), executable.parent_path() / L"StoneClient.exe", error))
        return TRUE;
    search.window = window;
    return FALSE;
}
}

void ConfigureArtApplicationLauncher(const fs::path& runtime, const fs::path& data, const fs::path& image)
{
    const std::lock_guard lock(gate);
    executable = fs::canonical(runtime);
    dataRoot = fs::canonical(data);
    systemImage = fs::canonical(image);
}

void SetArtRunningPackage(const std::wstring& name)
{
    if (!SetProcessLogPackage(name))
        std::fprintf(stderr, "Stone: could not attach the package name to the process log\n");
    const std::lock_guard lock(gate);
    runningPackage = name;
    if (!executable.empty())
        runningProperty = WindowProperty(name);
    RegisterBrokerClientPackage(name);
    StartClientApplicationLogs();
}

std::wstring ArtRunningPackage()
{
    const std::lock_guard lock(gate);
    return runningPackage;
}

void PublishArtApplicationWindow()
{
    const std::lock_guard lock(gate);
    if (!runningProperty.empty())
        if (auto window = WindowsHostWindow(); window && !GetPropW(window, runningProperty.c_str()))
            SetPropW(window, runningProperty.c_str(), reinterpret_cast<HANDLE>(1));
}

void LaunchInstalledArtApplication(const std::wstring& source, const std::wstring& name)
{
    const std::lock_guard lock(gate);
    if (executable.empty())
        throw std::runtime_error("installed application launching is not configured");
    const std::wstring prefix = L"/data/app/stone/";
    if (!source.starts_with(prefix) || source.size() != prefix.size() + 36 + 9 || !source.ends_with(L"/base.apk"))
        throw std::runtime_error("invalid installed application code path");
    const auto apk = fs::canonical(ResolveAndroidSystemFile(source.substr(1)));
    const auto managed = fs::canonical(dataRoot / L"data/app/stone");
    if (apk.filename() != L"base.apk" || !fs::equivalent(apk.parent_path().parent_path(), managed))
        throw std::runtime_error("application code is outside the installed package store");
    Search search{WindowProperty(name)};
    EnumWindows(FindWindow, reinterpret_cast<LPARAM>(&search));
    if (search.window) {
        ShowWindowAsync(search.window, SW_RESTORE);
        SetForegroundWindow(search.window);
        return;
    }
    auto previous = children.find(name);
    if (previous != children.end()) {
        const auto state = WaitForSingleObject(previous->second->handle, 0);
        if (state == WAIT_TIMEOUT)
            return; // Startup is already in flight; never block the caller's UI.
        if (state != WAIT_OBJECT_0)
            throw std::runtime_error("could not inspect application process");
        children.erase(previous);
    }
    auto command = Quote(executable) + L" --apk " + Quote(apk) + L" --data-dir " + Quote(dataRoot) +
                   L" --system-image " + Quote(systemImage);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        executable.parent_path().c_str(), &startup, &process))
        throw std::runtime_error("could not create installed ART application process");
    CloseHandle(process.hThread);
    children.emplace(name, std::make_unique<Process>(process.hProcess));
    std::fprintf(stderr, "[art] installed application process started: pid=%lu\n", process.dwProcessId);
}
}
