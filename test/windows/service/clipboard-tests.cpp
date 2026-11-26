#include "WindowsClipboard.h"
#include <windows.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
using namespace stone::windows;

namespace {
struct Unavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void Trace(const std::string& message)
{
    std::cout << message << '\n';
}

void Require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

template <class Action> void Reject(Action action, const char* message)
{
    try {
        action();
    }
    catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

struct Station {
    HWINSTA original = GetProcessWindowStation(), station = nullptr;
    HDESK originalDesktop = GetThreadDesktop(GetCurrentThreadId()), desktop = nullptr;

    Station()
    {
        try {
            Trace("Creating isolated clipboard station");
            auto name = L"Stone.Clipboard.Test." + std::to_wstring(GetCurrentProcessId()) + L"." +
                        std::to_wstring(GetTickCount64());
            station = CreateWindowStationW(name.c_str(), CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, nullptr);
            // Explicit station names require administrator membership. An
            // unnamed create-only station is supported for a normal user.
            if (!station && GetLastError() == ERROR_ACCESS_DENIED)
                station = CreateWindowStationW(nullptr, CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, nullptr);
            if (!station)
                throw Unavailable(
                    "Cannot create a new private clipboard window station; user clipboard was not accessed");
            Trace("Created isolated clipboard station");
            wchar_t previousName[512]{}, createdName[512]{};
            DWORD length = 0;
            Require(GetUserObjectInformationW(original, UOI_NAME, previousName, sizeof(previousName), &length) &&
                        GetUserObjectInformationW(station, UOI_NAME, createdName, sizeof(createdName), &length) &&
                        wcscmp(previousName, createdName) != 0,
                    "Clipboard test station is not isolated");
            Require(SetProcessWindowStation(station) != FALSE, "Cannot select private clipboard window station");
            desktop = CreateDesktopW(L"Default", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
            Require(desktop && SetThreadDesktop(desktop), "Cannot select private clipboard desktop");
            Trace("Selected isolated clipboard desktop");
        }
        catch (...) {
            Restore();
            throw;
        }
    }

    void Restore() noexcept
    {
        SetProcessWindowStation(original);
        SetThreadDesktop(originalDesktop);
        if (desktop) {
            CloseDesktop(desktop);
            desktop = nullptr;
        }
        if (station) {
            CloseWindowStation(station);
            station = nullptr;
        }
    }

    ~Station()
    {
        Restore();
    }
};

void ExternalText(const wchar_t* text)
{
    auto window = CreateWindowExW(0, L"STATIC", L"External clipboard test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    Require(window != nullptr, "Cannot create external clipboard owner");
    try {
        Require(OpenClipboard(window) != FALSE, "Cannot open private clipboard");
        try {
            Require(EmptyClipboard() != FALSE, "Cannot empty private clipboard");
            auto bytes = (wcslen(text) + 1) * sizeof(wchar_t);
            auto data = GlobalAlloc(GMEM_MOVEABLE, bytes);
            Require(data != nullptr, "Cannot allocate external text");
            auto buffer = GlobalLock(data);
            if (!buffer) {
                GlobalFree(data);
                throw std::runtime_error("Cannot lock external text");
            }
            memcpy(buffer, text, bytes);
            GlobalUnlock(data);
            if (!SetClipboardData(CF_UNICODETEXT, data)) {
                GlobalFree(data);
                throw std::runtime_error("Cannot publish external text");
            }
        }
        catch (...) {
            CloseClipboard();
            throw;
        }
        CloseClipboard();
    }
    catch (...) {
        DestroyWindow(window);
        throw;
    }
    DestroyWindow(window);
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 2 && std::wstring_view(argv[1]) == L"--current-desktop-check") {
        // Availability only: do not request any format's data or change ownership.
        if (!OpenClipboard(nullptr)) {
            std::cout << "Current desktop OpenClipboard denied/unavailable: " << GetLastError() << '\n';
            return 77;
        }
        std::cout << "Current desktop clipboard formats:";
        UINT format = 0;
        while ((format = EnumClipboardFormats(format)) != 0)
            std::cout << ' ' << format;
        std::cout << '\n';
        CloseClipboard();
        Trace("PASS current desktop clipboard can be opened; no content read or changed.");
        return 0;
    }
    if (argc != 1) {
        std::cerr << "Usage: stone-clipboard-tests [--current-desktop-check]\n";
        return 2;
    }
    try {
        Station station;
        if (!OpenClipboard(nullptr)) {
            auto error = GetLastError();
            if (error == ERROR_ACCESS_DENIED) {
                Trace(
                    "SKIP Windows denied direct clipboard access in the isolated station; native content behavior remains unverified.");
                return 77;
            }
            throw std::runtime_error("Private station control OpenClipboard failed: " + std::to_string(error));
        }
        CloseClipboard();
        Trace("Private station control clipboard access passed");
        Trace("Starting clipboard workers");
        WindowsClipboard first, second;
        Trace("Testing clipboard text");
        auto empty = first.Exchange({L"describe"});
        Require(empty.size() == 6 && empty[1] == L"0", "Private clipboard was not empty");
        Trace("Empty clipboard read passed");
        auto sent = first.Exchange({L"set", L"标签", L"Unicode 剪贴板\n\"quoted\"", L"QUJD"});
        Trace("Clipboard write passed");
        Require(second.Exchange({L"get"}) == sent, "Independent clipboard hosts lost text or metadata");
        auto description = second.Exchange({L"describe"});
        Require(description[4].empty() && description[3] == L"标签" && description[2] == L"1",
                "Description leaked text or lost metadata");
        Reject(
            [&] {
                first.Exchange({L"set", L"label", std::wstring(8193, L'x'), L""});
            },
            "Oversized text accepted");
        Reject(
            [&] {
                first.Exchange({L"set", L"label", L"ok", L"bad metadata"});
            },
            "Invalid metadata accepted");
        Require(second.Exchange({L"get"}) == sent, "Rejected clipboard write changed old data");
        Trace("Testing clipboard busy failure");
        Require(OpenClipboard(nullptr) != FALSE, "Cannot hold private clipboard busy");
        try {
            Reject(
                [&] {
                    second.Exchange({L"clear"});
                },
                "Busy clipboard clear succeeded");
        }
        catch (...) {
            CloseClipboard();
            throw;
        }
        CloseClipboard();
        Require(second.Exchange({L"get"}) == sent, "Busy failure erased data");
        Trace("Testing clipboard owner exit");
        Require(first.Stop(5000), "First clipboard owner did not stop");
        Require(second.Exchange({L"get"}) == sent, "Owner exit discarded immediate clipboard data");
        Trace("Testing external clipboard owner");
        ExternalText(L"Other Windows app\n外部文本");
        auto external = second.Exchange({L"get"});
        Require(external[4] == L"Other Windows app\n外部文本" && external[3] == L"Windows" && external[5].empty() &&
                    external[0] != sent[0],
                "External Windows text was not observed");
        ExternalText(L"");
        auto blank = second.Exchange({L"get"});
        Require(blank[1] == L"1" && blank[2] == L"0", "Empty text was confused with absent data");
        second.Exchange({L"clear"});
        Require(second.Exchange({L"get"})[1] == L"0", "Clipboard clear failed");
        Require(second.Stop(5000), "Second clipboard owner did not stop");
        Trace(
            "PASS real Unicode clipboard, independent hosts, owner exit, metadata, failures and external text in a private window station; user clipboard untouched.");
        return 0;
    }
    catch (const Unavailable& error) {
        std::cout << "SKIP " << error.what() << '\n';
        return 77;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << " (Windows error " << GetLastError() << ")\n";
        return 1;
    }
}
