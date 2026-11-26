#include "LaunchScreen.h"
#include "Window.h"
#include "resource.h"
#include "../host/ProcessLog.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <utility>

namespace stone::windows {
namespace {
constexpr wchar_t ClassName[] = L"Stone.LaunchScreen";
constexpr UINT Complete = WM_APP + 1;
constexpr UINT Failure = WM_APP + 2;
constexpr int WindowWidth = 608;
constexpr int WindowHeight = 342;
constexpr UINT_PTR AnimationTimer = 2;
std::atomic<HWND> screen = nullptr;
HANDLE worker = nullptr;
bool foreground = false;
std::wstring launchLabel;
HICON appIcon = nullptr;
HWND progress = nullptr;

LRESULT CALLBACK ProgressProcedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_ERASEBKGND)
        return 1;
    if (message != WM_PAINT)
        return DefSubclassProc(hwnd, message, wParam, lParam);
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(hwnd, &paint);
    RECT client{};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    const auto track = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1, client.bottom, client.bottom);
    const auto gray = CreateSolidBrush(RGB(211, 211, 211));
    FillRgn(dc, track, gray);
    SelectClipRgn(dc, track);
    const int segment = client.right / 3;
    const int left = static_cast<int>((GetTickCount64() % 1400) * (client.right + segment) / 1400) - segment;
    const auto moving =
        CreateRoundRectRgn(left, 0, left + segment + 1, client.bottom + 1, client.bottom, client.bottom);
    const auto blue = CreateSolidBrush(RGB(0, 120, 215));
    FillRgn(dc, moving, blue);
    SelectClipRgn(dc, nullptr);
    DeleteObject(blue);
    DeleteObject(moving);
    DeleteObject(gray);
    DeleteObject(track);
    EndPaint(hwnd, &paint);
    return 0;
}

void Paint(HWND hwnd)
{
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(hwnd, &paint);
    RECT client{};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    const auto dpi = GetDpiForWindow(hwnd);
    const auto scale = [dpi](int value) {
        return MulDiv(value, dpi, 96);
    };
    const int size = scale(72);
    const int center = client.bottom / 2 - scale(12);
    if (appIcon)
        DrawIconEx(dc, (client.right - size) / 2, center - size / 2, appIcon, size, size, 0, nullptr, DI_NORMAL);
    const auto font =
        CreateFontW(-scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    const auto previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(32, 32, 32));
    RECT text{scale(14), client.bottom - scale(28), client.right - scale(14), client.bottom - scale(10)};
    DrawTextW(dc, L"正在启动 Android 应用…", -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, previous);
    DeleteObject(font);
    EndPaint(hwnd, &paint);
}

LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        progress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 0, 0, 0, 0, hwnd,
                                   nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowSubclass(progress, ProgressProcedure, 1, 0);
        SetTimer(hwnd, AnimationTimer, 16, nullptr);
        SetTimer(hwnd, 1, 100, nullptr);
        return 0;
    case WM_SIZE: {
        RECT client{};
        GetClientRect(hwnd, &client);
        const auto dpi = GetDpiForWindow(hwnd);
        const int width = std::clamp<int>(client.right - MulDiv(28, dpi, 96), 0, MulDiv(144, dpi, 96));
        MoveWindow(progress, (client.right - width) / 2, client.bottom / 2 + MulDiv(68, dpi, 96), width,
                   std::max(1, MulDiv(5, dpi, 192)), TRUE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DPICHANGED: {
        const auto& bounds = *reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_TIMER:
        if (wParam == AnimationTimer) {
            InvalidateRect(progress, nullptr, FALSE);
            return 0;
        }
        // Android resolves the real label and icon before its first frame.
        if (const auto app = WindowsHostWindow()) {
            wchar_t label[1024]{};
            if (GetWindowTextW(app, label, 1024) && std::wstring_view(label) != L"Stone" && launchLabel != label) {
                launchLabel = label;
                SetWindowTextW(hwnd, label);
            }
            DWORD_PTR icon = 0;
            if (SendMessageTimeoutW(app, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 20, &icon) && icon) {
                if (const auto copy = CopyIcon(reinterpret_cast<HICON>(icon))) {
                    if (appIcon)
                        DestroyIcon(appIcon);
                    appIcon = copy;
                    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
        }
        return 0;
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case Failure:
        KillTimer(hwnd, 1);
        KillTimer(hwnd, AnimationTimer);
        ShowWindow(progress, SW_HIDE);
        MessageBoxW(hwnd, reinterpret_cast<const wchar_t*>(lParam), L"应用启动失败 — Stone", MB_OK | MB_ICONERROR);
        DestroyWindow(hwnd);
        return TRUE;
    case Complete: {
        const bool active = GetForegroundWindow() == hwnd;
        DestroyWindow(hwnd);
        if (const auto app = WindowsHostWindow(); active && app)
            SetForegroundWindow(app);
        return 0;
    }
    case WM_CLOSE:
        // ART cannot safely be unwound while it is initializing on another thread.
        // Closing the startup screen cancels this standalone Client process.
        ExitProcess(ERROR_CANCELLED);
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        KillTimer(hwnd, AnimationTimer);
        screen = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

DWORD WINAPI Run(void* ready)
{
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.lpfnWndProc = Procedure;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = ClassName;
    RegisterClassW(&type);
    appIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_STONE_CLIENT), IMAGE_ICON, 96, 96, 0));
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const auto& work = monitor.rcWork;
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    // Create on the selected monitor first so Windows supplies that monitor's DPI.
    screen = CreateWindowExW(0, ClassName, launchLabel.c_str(), style, work.left, work.top, 1, 1, nullptr, nullptr,
                             instance, nullptr);
    if (const auto hwnd = screen.load()) {
        const auto dpi = GetDpiForWindow(hwnd);
        const int width = std::min<int>(MulDiv(WindowWidth, dpi, 96), work.right - work.left);
        const int height = std::min<int>(MulDiv(WindowHeight, dpi, 96), work.bottom - work.top);
        SetWindowPos(hwnd, nullptr, work.left + (work.right - work.left - width) / 2,
                     work.top + (work.bottom - work.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        const COLORREF caption = RGB(237, 245, 249);
        const BOOL dark = FALSE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
        ShowWindow(hwnd, SW_SHOWNORMAL);
        // GetWindowRect includes invisible DWM margins. Match the reference's
        // visible size while centering the complete native window rectangle.
        RECT outer{}, visible{};
        if (GetWindowRect(hwnd, &outer) &&
            SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible)))) {
            const int frameWidth = std::min<int>(width + (outer.right - outer.left) - (visible.right - visible.left),
                                                 work.right - work.left);
            const int frameHeight = std::min<int>(height + (outer.bottom - outer.top) - (visible.bottom - visible.top),
                                                  work.bottom - work.top);
            SetWindowPos(hwnd, nullptr, work.left + (work.right - work.left - frameWidth) / 2,
                         work.top + (work.bottom - work.top - frameHeight) / 2, frameWidth, frameHeight,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        UpdateWindow(hwnd);
    }
    SetEvent(ready);
    if (screen) {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (appIcon)
        DestroyIcon(std::exchange(appIcon, nullptr));
    return 0;
}
}

void StartLaunchScreen(std::wstring_view label)
{
    foreground = true;
    launchLabel = label.empty() ? L"Stone" : std::wstring(label);
    const auto ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready)
        return;
    worker = CreateThread(nullptr, 0, Run, ready, 0, nullptr);
    if (worker)
        WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
}

void FinishLaunchScreen()
{
    if (const auto hwnd = screen.load())
        PostMessageW(hwnd, Complete, 0, 0);
}

void FailLaunchScreen(std::string_view error)
{
    if (!foreground)
        return;
    const int count = MultiByteToWideChar(CP_UTF8, 0, error.data(), static_cast<int>(error.size()), nullptr, 0);
    std::wstring text(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, error.data(), static_cast<int>(error.size()), text.data(), count);
    const auto log = ProcessLogPath();
    text += log.empty() ? L"\n\n无法创建本次启动日志。" : L"\n\n日志位置：\n" + log.wstring();
    // Flush and drain output before the user opens the reported file.
    StopProcessLog();
    const auto hwnd = screen.load();
    if (!hwnd || !SendMessageW(hwnd, Failure, 0, reinterpret_cast<LPARAM>(text.c_str())))
        MessageBoxW(nullptr, text.c_str(), L"应用启动失败 — Stone", MB_OK | MB_ICONERROR);
    if (worker) {
        WaitForSingleObject(worker, INFINITE);
        CloseHandle(std::exchange(worker, nullptr));
    }
}
}
