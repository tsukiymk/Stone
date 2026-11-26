#include "../subsystem/platform/windows/GlSurfaceJni.h"
#include "Window.h"
#include "LaunchScreen.h"
#include "../subsystem/platform/windows/DocumentPicker.h"
#include "Documents.h"
#include "Sharing.h"
#include "../host/graphics/GlSurface.h"
#include "HostConfiguration.h"
#include "BrokerClient.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
constexpr wchar_t WindowClass[] = L"Stone.Windows.Runtime";
constexpr wchar_t CaptionClass[] = L"Stone.Windows.Runtime.Caption";
constexpr UINT ApplyTitle = WM_APP + 1;
constexpr UINT ShowApp = WM_APP + 2;
constexpr UINT HideApp = WM_APP + 3;
constexpr UINT PaintFrame = WM_APP + 4;
constexpr UINT ApplyOrientation = WM_APP + 5;
constexpr UINT ApplyPointerIcon = WM_APP + 7;
constexpr UINT ApplyIcon = WM_APP + 8;
constexpr UINT ShowFirstArtFrame = WM_APP + 9;
constexpr UINT ApplyCaptureProtection = WM_APP + 11;
constexpr UINT ApplyAppearance = WM_APP + 12;
constexpr UINT ApplyArtResize = WM_APP + 13;
constexpr UINT QueryArtSize = WM_APP + 14;

struct Appearance {
    const std::wstring &label, &icon, &command;
};

constexpr UINT_PTR FrameTimer = 1;
constexpr int CanvasWidth = 360;
constexpr int CanvasHeight = 640;
constexpr int MinimumClientSize = 160;
constexpr int BackButtonWidth = 36;
constexpr int CaptionTitleGap = 4;
constexpr int CaptionCornerRadius = 8;
constexpr int ResizeIconSize = 72;
constexpr ULONGLONG ResizeFrameTimeoutMs = 1000;
constexpr DWORD WindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
constexpr DWORD ResizableWindowStyle = WindowStyle | WS_THICKFRAME | WS_MAXIMIZEBOX;

JNIEnv* uiEnv = nullptr;
thread_local WindowsArtInput artInput{};
thread_local bool nativeTouchDown = false;
thread_local float nativeTouchX = 0, nativeTouchY = 0;
std::atomic<HWND> window = nullptr;
std::atomic<bool> vulkanPresentation = false;
std::atomic<UINT> windowDpi = USER_DEFAULT_SCREEN_DPI;
std::atomic_bool shown = false;
std::atomic_bool closed = false;
std::atomic_bool callbackFailed = false;
std::atomic_bool paintPending = false;
std::atomic_bool landscape = false;
bool resizable = false;
bool awaitingSizeCommit = false;
bool firstFramePainted = false;
bool firstGlFramePainted = false;
std::atomic_bool firstNativeFramePainted = false;
std::atomic_bool captureProtected = false;
enum class ResizeMask {
    Hidden,
    Dragging,
    WaitingForFrame
};
ResizeMask resizeMask = ResizeMask::Hidden;
ULONGLONG resizeFrameDeadline = 0;
SIZE clientSize{};
std::atomic<COLORREF> background = RGB(255, 255, 255);
std::mutex stateMutex;
std::wstring title = L"Stone";
std::mutex iconRequestMutex;
std::mutex iconCallerMutex;
std::span<const jbyte> iconRequest;
std::vector<DWORD> windowIconPng;
DWORD windowIconSize = 0;
HICON smallWindowIcon = nullptr;
HICON largeWindowIcon = nullptr;
HICON resizeWindowIcon = nullptr;
HICON captionPlaceholderIcon = nullptr;
HWND captionWindow = nullptr;
HWND backTooltip = nullptr;
bool captionActive = false;
WindowsBackSink backSink = nullptr;
HANDLE artWindowThread = nullptr;
enum class BackButton {
    Idle,
    Hover,
    Pressed
};
BackButton backButton = BackButton::Idle;
bool backButtonCaptured = false;
jobject touchDispatcher = nullptr;
bool trackingMouseLeave = false;
bool hovering = false;
WPARAM hoverSequence = 0;
HCURSOR pointerCursor = nullptr;
std::unordered_map<jlong, jobject> renderers;
jlong nextSurface = 1;
jlong glSurface = 0;
jlong activeSurface = 0;
jlong initializedSurface = 0;
int initializedWidth = 0;
int initializedHeight = 0;
std::mutex frameMutex;
std::vector<jint> pixels;
int frameWidth = 0;
int frameHeight = 0;
std::vector<jint> glPixels;
std::vector<jint> composedPixels;
std::vector<jint> overlayPixels;
std::vector<jint> nativePixels;
int nativeWidth = 0;
int nativeHeight = 0;
int glWidth = 0;
int glHeight = 0;
int overlayWidth = 0;
int overlayHeight = 0;

enum class FrameSource {
    Canvas,
    OpenGl,
    Native
};

struct Frame {
    const std::vector<jint>& pixels;
    int width;
    int height;
    FrameSource source;
};

// The caller holds frameMutex while using the returned buffers.
Frame CurrentFrame()
{
    if (!nativePixels.empty())
        return {nativePixels, nativeWidth, nativeHeight, FrameSource::Native};
    if (!glPixels.empty())
        return {glPixels, glWidth, glHeight, FrameSource::OpenGl};
    return {pixels, frameWidth, frameHeight, FrameSource::Canvas};
}

RECT FrameViewport(const RECT& client, const Frame& frame)
{
    const LONG width = client.right - client.left;
    const LONG height = client.bottom - client.top;
    if (frame.pixels.empty() || width <= 0 || height <= 0)
        return client;
    const auto clientRatio = static_cast<int64_t>(width) * frame.height;
    const auto frameRatio = static_cast<int64_t>(height) * frame.width;
    if (clientRatio > frameRatio) {
        const int fittedWidth = std::max(1, MulDiv(height, frame.width, frame.height));
        const LONG left = client.left + (width - fittedWidth) / 2;
        return {left, client.top, left + fittedWidth, client.bottom};
    }
    if (clientRatio < frameRatio) {
        const int fittedHeight = std::max(1, MulDiv(width, frame.height, frame.width));
        const LONG top = client.top + (height - fittedHeight) / 2;
        return {client.left, top, client.right, top + fittedHeight};
    }
    return client;
}

void RequestPaint()
{
    if (const auto hwnd = window.load(); hwnd && !paintPending.exchange(true)) {
        if (!PostMessageW(hwnd, PaintFrame, 0, 0))
            paintPending = false;
    }
}

float BackingScale()
{
    return static_cast<float>(windowDpi.load()) / USER_DEFAULT_SCREEN_DPI;
}

bool HasPaintedFrame()
{
    return firstFramePainted || firstGlFramePainted || firstNativeFramePainted;
}

void WaitForResizeFrame()
{
    resizeMask = ResizeMask::WaitingForFrame;
    resizeFrameDeadline = GetTickCount64() + ResizeFrameTimeoutMs;
}

RECT FrameBounds(RECT client, UINT dpi)
{
    AdjustWindowRectExForDpi(&client, resizable ? ResizableWindowStyle : WindowStyle, FALSE, 0, dpi);
    return client;
}

RECT WindowBounds(UINT dpi)
{
    const bool wide = landscape.load();
    return FrameBounds({0, 0, MulDiv(wide ? CanvasHeight : CanvasWidth, dpi, USER_DEFAULT_SCREEN_DPI),
                        MulDiv(wide ? CanvasWidth : CanvasHeight, dpi, USER_DEFAULT_SCREEN_DPI)},
                       dpi);
}

bool ApplyWindowIcons(HWND hwnd, UINT dpi, DWORD* png, DWORD size)
{
    auto createIcon = [&](int width, int height) {
        return CreateIconFromResourceEx(reinterpret_cast<PBYTE>(png), size, TRUE, 0x00030000, width, height,
                                        LR_DEFAULTCOLOR);
    };
    const auto smallIcon =
        createIcon(GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi));
    const auto largeIcon = createIcon(GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi));
    if (!smallIcon || !largeIcon) {
        if (smallIcon)
            DestroyIcon(smallIcon);
        if (largeIcon)
            DestroyIcon(largeIcon);
        return false;
    }
    if (!captionPlaceholderIcon)
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
    if (const auto old = std::exchange(smallWindowIcon, smallIcon))
        DestroyIcon(old);
    if (const auto old = std::exchange(largeWindowIcon, largeIcon))
        DestroyIcon(old);
    const int resizeSize = MulDiv(ResizeIconSize, dpi, USER_DEFAULT_SCREEN_DPI);
    const auto resizeIcon = resizable ? createIcon(resizeSize, resizeSize) : nullptr;
    if (const auto old = std::exchange(resizeWindowIcon, resizeIcon))
        DestroyIcon(old);
    return true;
}

void DrawBackButtonPixels(uint32_t* output, const SIZE& size, LONG stride, UINT dpi)
{
    const int iconWidth = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    const int iconHeight = GetSystemMetricsForDpi(SM_CYSMICON, dpi);
    const float scale = 16.f / std::min(iconWidth, iconHeight);
    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                              (contrast.dwFlags & HCF_HIGHCONTRASTON);
    const bool dark = stone::windows::WindowsAppsUseDarkMode();
    const auto foreground = highContrast ? GetSysColor(COLOR_CAPTIONTEXT)
                            : dark       ? RGB(255, 255, 255)
                                         : GetSysColor(COLOR_WINDOWTEXT);
    const auto backgroundColor = dark && !highContrast ? RGB(255, 255, 255) : GetSysColor(COLOR_WINDOWFRAME);
    const unsigned shade = backButton == BackButton::Pressed ? 64 : backButton == BackButton::Hover ? 32 : 0;
    const float strokeRadius = std::max(highContrast ? 0.55f : 0.375f, scale * 0.5f);
    for (LONG y = 0; y < size.cy; ++y) {
        const float dy = std::abs((y + 0.5f - size.cy / 2.f) * scale);
        for (LONG x = 0; x < size.cx; ++x) {
            const float dx = (x + 0.5f - size.cx / 2.f) * scale;
            const float shaft = std::max(dy - strokeRadius, std::abs(dx) - 5.5f);
            const float head =
                std::max(std::abs(dy - dx - 5.f) * 0.7071f - strokeRadius, std::max(-dx - 5.5f, dx - 0.3f));
            const auto ink = static_cast<unsigned>(255 * std::clamp(0.5f - std::min(shaft, head) / scale, 0.f, 1.f));
            const auto fill = shade * (255 - ink) / 255;
            output[y * stride + x] = ((ink + fill) << 24) |
                                     (((GetRValue(foreground) * ink + GetRValue(backgroundColor) * fill) / 255) << 16) |
                                     (((GetGValue(foreground) * ink + GetGValue(backgroundColor) * fill) / 255) << 8) |
                                     ((GetBValue(foreground) * ink + GetBValue(backgroundColor) * fill) / 255);
        }
    }
}

void DrawCaptionTitle(HDC dc, uint32_t* output, const SIZE& size, LONG left, const std::wstring& text, UINT dpi)
{
    if (text.empty() || left >= size.cx)
        return;
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
        return;
    metrics.lfCaptionFont.lfQuality = ANTIALIASED_QUALITY;
    const auto font = CreateFontIndirectW(&metrics.lfCaptionFont);
    if (!font)
        return;
    const auto previous = SelectObject(dc, font);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    RECT area{left, 0, size.cx, size.cy};
    DrawTextW(dc, text.c_str(), -1, &area, DT_END_ELLIPSIS | DT_NOPREFIX | DT_SINGLELINE | DT_VCENTER);
    GdiFlush();
    SelectObject(dc, previous);
    DeleteObject(font);

    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                              (contrast.dwFlags & HCF_HIGHCONTRASTON);
    const auto color = highContrast ? GetSysColor(captionActive ? COLOR_CAPTIONTEXT : COLOR_INACTIVECAPTIONTEXT)
                       : stone::windows::WindowsAppsUseDarkMode()
                           ? (captionActive ? RGB(255, 255, 255) : RGB(160, 160, 160))
                           : GetSysColor(captionActive ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT);
    for (LONG y = 0; y < size.cy; ++y) {
        for (LONG x = left; x < size.cx; ++x) {
            auto& pixel = output[y * size.cx + x];
            const auto alpha = pixel & 0xff;
            pixel = (alpha << 24) | ((GetRValue(color) * alpha / 255) << 16) | ((GetGValue(color) * alpha / 255) << 8) |
                    (GetBValue(color) * alpha / 255);
        }
    }
}

bool PaintCaptionWindow(const RECT& button, LONG right, const std::wstring& text, UINT dpi)
{
    const SIZE buttonSize{button.right - button.left, button.bottom - button.top};
    SIZE size{right - button.left, buttonSize.cy};
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size.cx;
    info.bmiHeader.biHeight = -size.cy;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    const auto dc = CreateCompatibleDC(nullptr);
    if (!dc)
        return false;
    void* bits = nullptr;
    const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool painted = false;
    if (bitmap) {
        if (const auto previous = SelectObject(dc, bitmap)) {
            auto* output = static_cast<uint32_t*>(bits);
            std::fill_n(output, static_cast<size_t>(size.cx) * size.cy, 0);
            DrawBackButtonPixels(output, buttonSize, size.cx, dpi);
            DrawCaptionTitle(dc, output, size, buttonSize.cx + MulDiv(CaptionTitleGap, dpi, USER_DEFAULT_SCREEN_DPI),
                             text, dpi);
            POINT position{button.left, button.top};
            POINT source{};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            painted = UpdateLayeredWindow(captionWindow, nullptr, &position, &size, dc, &source, 0, &blend,
                                          ULW_ALPHA) != FALSE;
            SelectObject(dc, previous);
        }
        DeleteObject(bitmap);
    }
    DeleteDC(dc);
    return painted;
}

bool GetBackButtonBounds(HWND hwnd, RECT& button)
{
    if (!backSink || IsIconic(hwnd))
        return false;
    TITLEBARINFO titleBar{sizeof(titleBar)};
    POINT origin{};
    if (!GetTitleBarInfo(hwnd, &titleBar) || !ClientToScreen(hwnd, &origin))
        return false;
    const auto& caption = titleBar.rcTitleBar;
    const UINT dpi = GetDpiForWindow(hwnd);
    const LONG width = MulDiv(BackButtonWidth, dpi, USER_DEFAULT_SCREEN_DPI);
    LONG top = caption.top;
    RECT frame{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
        !IsRectEmpty(&frame))
        top = std::min(top, frame.top + GetSystemMetricsForDpi(SM_CYBORDER, dpi));
    button = {origin.x, top, origin.x + width, std::min(caption.bottom, origin.y)};
    return !IsRectEmpty(&button);
}

TOOLINFOW BackTooltipTool(HWND hwnd)
{
    TOOLINFOW tool{TTTOOLINFOW_V2_SIZE};
    tool.hwnd = hwnd;
    tool.uId = 1;
    return tool;
}

void ShowBackTooltip(HWND hwnd)
{
    RECT bounds{};
    if (!IsWindowVisible(hwnd) || !GetBackButtonBounds(hwnd, bounds))
        return;
    auto tool = BackTooltipTool(hwnd);
    if (!backTooltip) {
        const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
        if (!InitCommonControlsEx(&controls))
            return;
        backTooltip = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
                                      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, hwnd, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
        if (!backTooltip)
            return;
        static wchar_t backChinese[] = L"\u8fd4\u56de";
        static wchar_t backEnglish[] = L"Back";
        tool.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_TRANSPARENT;
        tool.lpszText = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? backChinese : backEnglish;
        if (!SendMessageW(backTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) {
            DestroyWindow(std::exchange(backTooltip, nullptr));
            return;
        }
    }
    const int gap = MulDiv(4, GetDpiForWindow(hwnd), USER_DEFAULT_SCREEN_DPI);
    SendMessageW(backTooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(bounds.left + gap, bounds.bottom + gap));
    SendMessageW(backTooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tool));
    // The message pump is scoped to the host. Paint the owned tooltip before returning.
    UpdateWindow(backTooltip);
}

void HideBackTooltip(HWND hwnd)
{
    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_CANCEL | TME_HOVER | TME_NONCLIENT, hwnd};
    TrackMouseEvent(&tracking);
    if (backTooltip) {
        auto tool = BackTooltipTool(hwnd);
        SendMessageW(backTooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&tool));
    }
}

LRESULT CALLBACK CaptionProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void RefreshCaption(HWND hwnd)
{
    RECT button{};
    if (!IsWindowVisible(hwnd) || !GetBackButtonBounds(hwnd, button)) {
        if (captionWindow)
            ShowWindow(captionWindow, SW_HIDE);
        return;
    }
    if (!captionWindow) {
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW definition{};
        definition.lpfnWndProc = CaptionProc;
        definition.hInstance = instance;
        definition.lpszClassName = CaptionClass;
        if (!RegisterClassW(&definition) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return;
        captionWindow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                        CaptionClass, nullptr, WS_POPUP, 0, 0, 0, 0, hwnd, nullptr, instance, nullptr);
        if (!captionWindow)
            return;
    }
    const UINT dpi = GetDpiForWindow(hwnd);
    RECT frame{}, controls{};
    if (!GetWindowRect(hwnd, &frame))
        return;
    LONG right = frame.right - 3 * GetSystemMetricsForDpi(SM_CXSIZE, dpi);
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CAPTION_BUTTON_BOUNDS, &controls, sizeof(controls))) &&
        !IsRectEmpty(&controls))
        right = frame.left + controls.left;
    right = std::max(button.right, right - MulDiv(CaptionTitleGap, dpi, USER_DEFAULT_SCREEN_DPI));
    std::wstring text;
    {
        const std::lock_guard lock(stateMutex);
        text = title;
    }
    if (!PaintCaptionWindow(button, right, text, dpi))
        return;
    if (!captionPlaceholderIcon) {
        constexpr BYTE andMask[]{0xff, 0xff};
        constexpr BYTE xorMask[]{0, 0};
        captionPlaceholderIcon = CreateIcon(nullptr, 1, 1, 1, 1, andMask, xorMask);
        if (!captionPlaceholderIcon)
            return;
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(captionPlaceholderIcon));
        WTA_OPTIONS theme{WTNCA_NODRAWCAPTION, WTNCA_NODRAWCAPTION};
        SetWindowThemeAttribute(hwnd, WTA_NONCLIENT, &theme, sizeof(theme));
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (IsZoomed(hwnd)) {
        SetWindowRgn(captionWindow, nullptr, FALSE);
    }
    else {
        const int radius = MulDiv(CaptionCornerRadius, dpi, USER_DEFAULT_SCREEN_DPI);
        // The extra width and height leave only the outer top-left corner rounded.
        const auto region = CreateRoundRectRgn(0, 0, right - button.left + radius, button.bottom - button.top + radius,
                                               radius * 2, radius * 2);
        if (region && !SetWindowRgn(captionWindow, region, FALSE))
            DeleteObject(region);
    }
    if (!IsWindowVisible(captionWindow))
        ShowWindow(captionWindow, SW_SHOWNOACTIVATE);
}

void UpdateBackButton(HWND hwnd, BackButton state)
{
    if (std::exchange(backButton, state) == state)
        return;
    if (state == BackButton::Hover) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_HOVER | TME_LEAVE | TME_NONCLIENT, hwnd, HOVER_DEFAULT};
        TrackMouseEvent(&tracking);
    }
    else {
        HideBackTooltip(hwnd);
    }
    RefreshCaption(hwnd);
}

LRESULT HitTestWindow(HWND hwnd, LPARAM screenPosition)
{
    const auto hit = DefWindowProcW(hwnd, WM_NCHITTEST, 0, screenPosition);
    RECT button{};
    if (!captionPlaceholderIcon || (hit != HTCAPTION && hit != HTBORDER && hit != HTSYSMENU) ||
        !GetBackButtonBounds(hwnd, button))
        return hit;
    const POINT point{GET_X_LPARAM(screenPosition), GET_Y_LPARAM(screenPosition)};
    if (PtInRect(&button, point))
        return HTSYSMENU;
    return hit == HTSYSMENU ? HTCAPTION : hit;
}

bool BackButtonContainsClientPoint(HWND hwnd, LPARAM position)
{
    POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    return captionPlaceholderIcon && ClientToScreen(hwnd, &point) &&
           HitTestWindow(hwnd, MAKELPARAM(point.x, point.y)) == HTSYSMENU;
}

void ReleaseBackButton(HWND hwnd, bool clicked)
{
    backButtonCaptured = false;
    ReleaseCapture();
    UpdateBackButton(hwnd, clicked ? BackButton::Hover : BackButton::Idle);
    if (clicked)
        backSink();
}

void CheckCallback()
{
    if (!uiEnv->ExceptionCheck())
        return;
    uiEnv->ExceptionDescribe();
    callbackFailed = true;
    PostMessageW(window.load(), WM_CLOSE, 0, 0);
}

void Render(HWND hwnd)
{
    RECT client{};
    if (resizeMask == ResizeMask::Dragging || IsIconic(hwnd) || !GetClientRect(hwnd, &client) || IsRectEmpty(&client))
        return;
    jobject renderer = nullptr;
    jlong surface;
    bool gl;
    {
        const std::lock_guard lock(stateMutex);
        surface = activeSurface;
        gl = glSurface && surface == glSurface;
        const auto entry = renderers.find(surface);
        if (entry != renderers.end())
            renderer = uiEnv->NewLocalRef(entry->second);
    }
    if (renderer) {
        const jclass type = uiEnv->GetObjectClass(renderer);
        const bool first = initializedSurface != surface;
        const bool resized = initializedWidth != client.right || initializedHeight != client.bottom;
        if (first) {
            const std::lock_guard lock(frameMutex);
            pixels.clear();
            glPixels.clear();
            nativePixels.clear();
            overlayPixels.clear();
        }
        if (first || resized || awaitingSizeCommit) {
            // Commit guest display metrics before presenting a frame at the new size.
            const jclass display = uiEnv->FindClass("com/trebledot/stone/loader/DisplayConfig");
            const jmethodID noteSize =
                display ? uiEnv->GetStaticMethodID(display, "stoneNoteSurfaceSize", "(II)Z") : nullptr;
            const bool committed =
                noteSize && uiEnv->CallStaticBooleanMethod(display, noteSize, client.right, client.bottom);
            if (display)
                uiEnv->DeleteLocalRef(display);
            // The client can return to its last rendered size while a commit is pending.
            awaitingSizeCommit = !committed;
            if (uiEnv->ExceptionCheck() || (!committed && !first)) {
                uiEnv->DeleteLocalRef(type);
                uiEnv->DeleteLocalRef(renderer);
                CheckCallback();
                return;
            }
        }
        if (!uiEnv->ExceptionCheck() && (!gl || BeginWindowsGlFrame(client.right, client.bottom))) {
            if (first || resized) {
                const jmethodID changed = uiEnv->GetMethodID(type, "stoneSurfaceChanged", "(II)V");
                if (changed)
                    uiEnv->CallVoidMethod(renderer, changed, client.right, client.bottom);
                initializedSurface = surface;
                initializedWidth = client.right;
                initializedHeight = client.bottom;
            }
            if (!uiEnv->ExceptionCheck()) {
                const jmethodID draw = uiEnv->GetMethodID(type, "stoneDrawFrame", "()V");
                if (draw)
                    uiEnv->CallVoidMethod(renderer, draw);
            }
            if (gl) {
                {
                    const std::lock_guard lock(frameMutex);
                    if (ReadWindowsGlFrame(glPixels, client.right, client.bottom)) {
                        glWidth = client.right;
                        glHeight = client.bottom;
                    }
                }
                RequestPaint();
            }
        }
        uiEnv->DeleteLocalRef(type);
        uiEnv->DeleteLocalRef(renderer);
    }
    CheckCallback();
}

jobject InputTarget()
{
    const std::lock_guard lock(stateMutex);
    return touchDispatcher ? uiEnv->NewLocalRef(touchDispatcher) : nullptr;
}

struct InputPoint {
    float x, y, width, height;
};

bool MapInputPoint(HWND hwnd, POINT point, bool requireInside, InputPoint& mapped)
{
    RECT client{};
    if (!GetClientRect(hwnd, &client) || IsRectEmpty(&client))
        return false;
    const std::lock_guard lock(frameMutex);
    const Frame frame = CurrentFrame();
    const RECT viewport = FrameViewport(client, frame);
    if (IsRectEmpty(&viewport) || (requireInside && !PtInRect(&viewport, point)))
        return false;
    const auto width = static_cast<float>(frame.pixels.empty() ? client.right : frame.width);
    const auto height = static_cast<float>(frame.pixels.empty() ? client.bottom : frame.height);
    const float x =
        static_cast<float>(point.x - viewport.left) * (width / static_cast<float>(viewport.right - viewport.left));
    const float y =
        static_cast<float>(point.y - viewport.top) * (height / static_cast<float>(viewport.bottom - viewport.top));
    const float scale = BackingScale();
    // TouchDispatcher expects bottom-up host points. Win32 reports top-down client pixels.
    mapped = {x / scale, (height - y) / scale, width / scale, height / scale};
    return true;
}

void DispatchTouch(HWND hwnd, int action, LPARAM position, int buttons = 0)
{
    if (action == 0 && resizeMask != ResizeMask::Hidden)
        return;
    InputPoint point{};
    if (!MapInputPoint(hwnd, {GET_X_LPARAM(position), GET_Y_LPARAM(position)}, action == 0, point))
        return;
    if (artInput.touch) {
        if (action == 0)
            SetCapture(hwnd);
        const float scale = BackingScale();
        nativeTouchDown = action != 1 && action != 3;
        nativeTouchX = point.x * scale;
        nativeTouchY = (point.height - point.y) * scale;
        artInput.touch(action, nativeTouchX, nativeTouchY, buttons);
        return;
    }
    const jobject target = InputTarget();
    if (!target)
        return;
    if (action == 0)
        SetCapture(hwnd);
    const jclass type = uiEnv->GetObjectClass(target);
    const jmethodID method = uiEnv->GetMethodID(type, "dispatch", "(IFFFF)V");
    if (method)
        uiEnv->CallVoidMethod(target, method, action, point.x, point.y, point.width, point.height);
    uiEnv->DeleteLocalRef(type);
    uiEnv->DeleteLocalRef(target);
    CheckCallback();
}

HCURSOR LoadPointerIcon(jint type)
{
    // Android PointerIcon.TYPE_* values.
    switch (type) {
    case 0:
        return nullptr;
    case 1002:
        return LoadCursorW(nullptr, IDC_HAND);
    case 1003:
        return LoadCursorW(nullptr, IDC_HELP);
    case 1004:
        return LoadCursorW(nullptr, IDC_WAIT);
    case 1007:
        return LoadCursorW(nullptr, IDC_CROSS);
    case 1008:
        return LoadCursorW(nullptr, IDC_IBEAM);
    case 1012:
        return LoadCursorW(nullptr, IDC_NO);
    case 1013:
        return LoadCursorW(nullptr, IDC_SIZEALL);
    case 1014:
        return LoadCursorW(nullptr, IDC_SIZEWE);
    case 1015:
        return LoadCursorW(nullptr, IDC_SIZENS);
    case 1016:
        return LoadCursorW(nullptr, IDC_SIZENESW);
    case 1017:
        return LoadCursorW(nullptr, IDC_SIZENWSE);
    default:
        return LoadCursorW(nullptr, IDC_ARROW);
    }
}

void UpdatePointerCursor(HWND hwnd)
{
    // The resolved icon arrives asynchronously from the Android UI thread.
    POINT position{};
    RECT client{};
    if (GetCursorPos(&position) && WindowFromPoint(position) == hwnd && ScreenToClient(hwnd, &position) &&
        GetClientRect(hwnd, &client) && PtInRect(&client, position)) {
        SetCursor(hovering ? pointerCursor : LoadPointerIcon(1000));
    }
}

InputPoint lastHoverPoint{};

void DispatchHover(bool inside, int modifiers)
{
    const jobject target = InputTarget();
    if (!target)
        return;
    const jclass type = uiEnv->GetObjectClass(target);
    const jmethodID method = uiEnv->GetMethodID(type, "dispatchWindowsHover", "(JZIFFFF)V");
    if (method)
        uiEnv->CallVoidMethod(target, method, static_cast<jlong>(hoverSequence), inside ? JNI_TRUE : JNI_FALSE,
                              modifiers, lastHoverPoint.x, lastHoverPoint.y, lastHoverPoint.width,
                              lastHoverPoint.height);
    uiEnv->DeleteLocalRef(type);
    uiEnv->DeleteLocalRef(target);
    CheckCallback();
}

void EndHover(HWND hwnd)
{
    if (!std::exchange(hovering, false))
        return;
    ++hoverSequence;
    UpdatePointerCursor(hwnd);
    DispatchHover(false, 0);
}

void MoveHover(HWND hwnd, WPARAM state, LPARAM position)
{
    constexpr WPARAM buttons = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2;
    InputPoint point{};
    if (!uiEnv || (state & buttons) || GetCapture() || resizeMask != ResizeMask::Hidden ||
        !MapInputPoint(hwnd, {GET_X_LPARAM(position), GET_Y_LPARAM(position)}, true, point)) {
        EndHover(hwnd);
        return;
    }
    if (!trackingMouseLeave) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
        trackingMouseLeave = TrackMouseEvent(&tracking) != FALSE;
    }
    if (!hovering)
        pointerCursor = LoadPointerIcon(1000);
    hovering = true;
    lastHoverPoint = point;
    ++hoverSequence;
    const int modifiers =
        (state & MK_SHIFT ? 1 : 0) | (state & MK_CONTROL ? 0x1000 : 0) | (GetKeyState(VK_MENU) < 0 ? 2 : 0);
    DispatchHover(true, modifiers);
}

int AndroidKey(WPARAM key)
{
    if (key >= 'A' && key <= 'Z')
        return 29 + static_cast<int>(key - 'A');
    if (key >= '0' && key <= '9')
        return 7 + static_cast<int>(key - '0');
    switch (key) {
    case VK_ESCAPE:
        return 4;
    case VK_UP:
        return 19;
    case VK_DOWN:
        return 20;
    case VK_LEFT:
        return 21;
    case VK_RIGHT:
        return 22;
    case VK_TAB:
        return 61;
    case VK_SPACE:
        return 62;
    case VK_RETURN:
        return 66;
    case VK_BACK:
        return 67;
    case VK_PRIOR:
        return 92;
    case VK_NEXT:
        return 93;
    case VK_DELETE:
        return 112;
    case VK_HOME:
        return 122;
    case VK_END:
        return 123;
    default:
        return 0;
    }
}

void DispatchKey(bool down, WPARAM key, LPARAM state)
{
    const int code = AndroidKey(key);
    if (code == 0)
        return;
    if (artInput.key) {
        if (key == VK_ESCAPE) {
            if (!down)
                artInput.back();
            return;
        }
        const int modifiers = (GetKeyState(VK_SHIFT) < 0 ? 1 : 0) | (GetKeyState(VK_MENU) < 0 ? 2 : 0) |
                              (GetKeyState(VK_CONTROL) < 0 ? 0x1000 : 0);
        const bool textKey = (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || key == VK_SPACE;
        // TranslateMessage provides the Unicode text for unmodified printable keys.
        if (!textKey || (modifiers & 0x1002))
            artInput.key(down, code, modifiers, down && (state & (1LL << 30)) ? LOWORD(state) : 0);
        return;
    }
    const jobject target = InputTarget();
    if (!target)
        return;
    BYTE keyboard[256]{};
    wchar_t text[8]{};
    int modifiers = 0;
    int character = 0;
    if (GetKeyboardState(keyboard)) {
        if (keyboard[VK_SHIFT] & 0x80)
            modifiers |= 1;
        if (keyboard[VK_MENU] & 0x80)
            modifiers |= 2;
        if (keyboard[VK_CONTROL] & 0x80)
            modifiers |= 0x1000;
        if (down &&
            ToUnicodeEx(static_cast<UINT>(key), (state >> 16) & 0xFF, keyboard, text, ARRAYSIZE(text), 4,
                        GetKeyboardLayout(0)) == 1 &&
            text[0] >= L' ') {
            character = text[0];
        }
    }
    const jclass type = uiEnv->GetObjectClass(target);
    const jmethodID method = uiEnv->GetMethodID(type, "dispatchHostKey", "(IIIIZ)V");
    const jboolean repeat = down && (state & (1LL << 30)) ? JNI_TRUE : JNI_FALSE;
    if (method)
        uiEnv->CallVoidMethod(target, method, down ? 0 : 1, code, character, modifiers, repeat);
    uiEnv->DeleteLocalRef(type);
    uiEnv->DeleteLocalRef(target);
    CheckCallback();
}

jint BlendPixel(jint base, jint overlay)
{
    const auto a = static_cast<unsigned>(overlay) >> 24;
    if (a == 0)
        return base;
    if (a == 255)
        return overlay;
    unsigned color = 0;
    for (unsigned shift : {0u, 8u, 16u}) {
        const auto foreground = (static_cast<unsigned>(overlay) >> shift) & 0xffu;
        const auto under = (static_cast<unsigned>(base) >> shift) & 0xffu;
        color |= ((foreground * a + under * (255 - a) + 127) / 255) << shift;
    }
    return static_cast<jint>(color);
}

void PaintBackground(HDC dc, const RECT& area, const RECT& viewport)
{
    if (EqualRect(&area, &viewport))
        return;
    const RECT margins[] = {
        {area.left, area.top, area.right, viewport.top},
        {area.left, viewport.bottom, area.right, area.bottom},
        {area.left, viewport.top, viewport.left, viewport.bottom},
        {viewport.right, viewport.top, area.right, viewport.bottom},
    };
    const auto brush = CreateSolidBrush(background.load());
    for (const auto& margin : margins) {
        if (!IsRectEmpty(&margin))
            FillRect(dc, &margin, brush);
    }
    DeleteObject(brush);
}

void PaintResizeMask(HDC dc, const RECT& area)
{
    const auto brush = CreateSolidBrush(RGB(32, 32, 32));
    FillRect(dc, &area, brush);
    DeleteObject(brush);
    const int size = std::min({MulDiv(ResizeIconSize, windowDpi.load(), USER_DEFAULT_SCREEN_DPI),
                               static_cast<int>(area.right), static_cast<int>(area.bottom)});
    auto icon = resizeWindowIcon ? resizeWindowIcon : largeWindowIcon;
    if (!icon)
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    if (icon && size > 0) {
        DrawIconEx(dc, (area.right - size) / 2, (area.bottom - size) / 2, icon, size, size, 0, nullptr, DI_NORMAL);
    }
}

void Paint(HWND hwnd)
{
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(hwnd, &paint);
    if (vulkanPresentation.load()) {
        EndPaint(hwnd, &paint);
        return;
    }
    RECT area{};
    GetClientRect(hwnd, &area);
    {
        const std::lock_guard lock(frameMutex);
        const Frame frame = CurrentFrame();
        if (resizeMask == ResizeMask::WaitingForFrame && !frame.pixels.empty() && frame.width == area.right &&
            frame.height == area.bottom) {
            resizeMask = ResizeMask::Hidden;
        }
        if (resizeMask != ResizeMask::Hidden) {
            PaintResizeMask(dc, area);
        }
        else if (!frame.pixels.empty()) {
            const int width = frame.width;
            const int height = frame.height;
            const bool composite = frame.source != FrameSource::Canvas;
            const bool hasCanvas = composite && !pixels.empty() && frameWidth == width && frameHeight == height;
            const bool hasOverlay =
                composite && !overlayPixels.empty() && overlayWidth == width && overlayHeight == height;
            const jint* image = frame.pixels.data();
            if (hasCanvas || hasOverlay) {
                composedPixels = frame.pixels;
                if (hasCanvas) {
                    for (size_t i = 0; i < composedPixels.size(); ++i) {
                        composedPixels[i] = BlendPixel(composedPixels[i], pixels[i]);
                    }
                }
                if (hasOverlay) {
                    for (size_t i = 0; i < composedPixels.size(); ++i) {
                        composedPixels[i] = BlendPixel(composedPixels[i], overlayPixels[i]);
                    }
                }
                image = composedPixels.data();
            }
            // The last buffer can still have the previous aspect ratio while Android relayouts.
            const RECT viewport = FrameViewport(area, frame);
            PaintBackground(dc, area, viewport);
            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = width;
            bitmap.bmiHeader.biHeight = -height;
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            const int rows = StretchDIBits(dc, viewport.left, viewport.top, viewport.right - viewport.left,
                                           viewport.bottom - viewport.top, 0, 0, width, height, image, &bitmap,
                                           DIB_RGB_COLORS, SRCCOPY);
            if (frame.source == FrameSource::Native && !firstNativeFramePainted && rows > 0) {
                firstNativeFramePainted = true;
                std::fprintf(stderr, "[win32] first Android native frame painted (%d x %d)\n", width, height);
            }
            else if (frame.source == FrameSource::OpenGl && !firstGlFramePainted && rows > 0) {
                firstGlFramePainted = true;
                std::fprintf(stderr, "[win32] first OpenGL ES frame painted (%d x %d)\n", width, height);
            }
            else if (frame.source == FrameSource::Canvas && !firstFramePainted && rows > 0) {
                firstFramePainted = true;
                std::fprintf(stderr, "[win32] first Canvas frame painted (%d x %d)\n", width, height);
            }
        }
        else {
            PaintBackground(dc, area, {});
        }
    }
    EndPaint(hwnd, &paint);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case ApplyTitle: {
        std::wstring text;
        {
            const std::lock_guard lock(stateMutex);
            text = title;
        }
        SetWindowTextW(hwnd, text.c_str());
        if (backSink)
            RefreshCaption(hwnd);
        return 0;
    }
    case ShowApp:
        shown =
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW) != FALSE;
        return 0;
    case HideApp:
        EndHover(hwnd);
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case ApplyPointerIcon:
        if (hovering && wParam == hoverSequence) {
            pointerCursor = LoadPointerIcon(static_cast<jint>(lParam));
            UpdatePointerCursor(hwnd);
        }
        return 0;
    case ApplyIcon: {
        const std::lock_guard lock(iconRequestMutex);
        return SetWindowsHostIcon(iconRequest);
    }
    case ApplyAppearance: {
        auto checked = [](HRESULT result, const char* step) {
            if (FAILED(result))
                std::fprintf(stderr, "[win32] application appearance %s failed: 0x%08lx\n", step,
                             static_cast<unsigned long>(result));
            return SUCCEEDED(result);
        };
        const auto& request = *reinterpret_cast<const Appearance*>(lParam);
        {
            const std::lock_guard lock(stateMutex);
            title = request.label;
        }
        SendMessageW(hwnd, ApplyTitle, 0, 0);
        if (request.icon.empty() || request.command.empty())
            return TRUE;
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (initialized != RPC_E_CHANGED_MODE && !checked(initialized, "COM initialization"))
            return FALSE;

        struct Apartment {
            HRESULT result;

            ~Apartment()
            {
                if (SUCCEEDED(result))
                    CoUninitialize();
            }
        } apartment{initialized};

        Microsoft::WRL::ComPtr<IPropertyStore> properties;
        if (!checked(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&properties)), "window property store"))
            return FALSE;
        auto set = [&](REFPROPERTYKEY key, const wchar_t* text) {
            PROPVARIANT value{};
            HRESULT result = InitPropVariantFromString(text, &value);
            if (SUCCEEDED(result))
                result = properties->SetValue(key, value);
            if (FAILED(result))
                std::fprintf(stderr, "[win32] application property %lu length %zu\n", key.pid, std::wcslen(text));
            PropVariantClear(&value);
            return checked(result, "window property");
        };
        PWSTR appId = nullptr;
        if (!checked(GetCurrentProcessExplicitAppUserModelID(&appId), "process identity"))
            return FALSE;
        const bool assigned = set(PKEY_AppUserModel_RelaunchCommand, request.command.c_str()) &&
                              set(PKEY_AppUserModel_RelaunchDisplayNameResource, request.label.c_str()) &&
                              set(PKEY_AppUserModel_RelaunchIconResource, (request.icon + L",0").c_str()) &&
                              set(PKEY_AppUserModel_ID, appId);
        CoTaskMemFree(appId);
        // A window property store applies SetValue immediately.
        return assigned;
    }
    case ApplyCaptureProtection: {
        const bool protect = wParam != 0;
        if (captureProtected.load() == protect)
            return TRUE;
        // Replace the last protected frame in the compositor before lifting
        // protection on an Activity transition or flag change.
        if (!protect && (!RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN) ||
                         FAILED(DwmFlush())))
            return FALSE;
        if (!SetWindowDisplayAffinity(hwnd, protect ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE)) {
            std::fprintf(stderr, "[win32] capture protection failed: %lu\n", GetLastError());
            return FALSE;
        }
        captureProtected = protect;
        return TRUE;
    }
    case ShowFirstArtFrame:
        if (!IsWindowVisible(hwnd) &&
            !SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW))
            return FALSE;
        // ART bypasses ShowApp, but the resize mask uses the same visible-window gate.
        shown = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        if (firstNativeFramePainted)
            stone::windows::FinishLaunchScreen();
        return firstNativeFramePainted;
    case PaintFrame:
        paintPending = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case ApplyOrientation: {
        const bool wide = wParam != 0;
        if (landscape.exchange(wide) == wide)
            return 0;
        const UINT dpi = windowDpi.load();
        RECT bounds{};
        if (resizable) {
            RECT client{};
            if (IsZoomed(hwnd) || IsIconic(hwnd) || !GetClientRect(hwnd, &client) ||
                wide == (client.right > client.bottom))
                return 0;
            bounds = FrameBounds({0, 0, client.bottom, client.right}, dpi);
        }
        else {
            bounds = WindowBounds(dpi);
        }
        SetWindowPos(hwnd, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WindowsDocumentPickerMessage:
        if (uiEnv) {
            ShowWindowsDocumentPicker(uiEnv, hwnd);
            CheckCallback();
        }
        return 0;
    case stone::windows::WindowsShareMessage:
        stone::windows::ShowWindowShare(hwnd);
        return 0;
    case ApplyArtResize: {
        const bool enabled = wParam != 0;
        if (resizable == enabled)
            return TRUE;
        RECT client{};
        if (!GetClientRect(hwnd, &client))
            return FALSE;
        const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        const auto next = enabled ? style | WS_THICKFRAME | WS_MAXIMIZEBOX : style & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        SetLastError(0);
        if (!SetWindowLongPtrW(hwnd, GWL_STYLE, next) && GetLastError())
            return FALSE;
        resizable = enabled;
        const RECT bounds = FrameBounds(client, windowDpi.load());
        return SetWindowPos(hwnd, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    case QueryArtSize: {
        RECT client{};
        if (IsIconic(hwnd) || resizeMask == ResizeMask::Dragging || !GetClientRect(hwnd, &client))
            return 0;
        return static_cast<LRESULT>((uint64_t(client.right) << 32) | uint32_t(client.bottom));
    }
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_SIZING:
        if (resizable && shown && HasPaintedFrame() && resizeMask != ResizeMask::Dragging) {
            EndHover(hwnd);
            resizeMask = ResizeMask::Dragging;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return TRUE;
    case WM_EXITSIZEMOVE:
        if (resizeMask == ResizeMask::Dragging) {
            WaitForResizeFrame();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SIZE: {
        if (wParam == SIZE_MINIMIZED) {
            EndHover(hwnd);
            UpdateBackButton(hwnd, BackButton::Idle);
            resizeMask = ResizeMask::Hidden;
            return 0;
        }
        RECT client{};
        if (GetClientRect(hwnd, &client)) {
            const SIZE previous = std::exchange(clientSize, SIZE{client.right, client.bottom});
            if (resizable && shown && HasPaintedFrame() && resizeMask != ResizeMask::Dragging &&
                (clientSize.cx != previous.cx || clientSize.cy != previous.cy)) {
                EndHover(hwnd);
                WaitForResizeFrame();
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        if (!resizable)
            break;
        DefWindowProcW(hwnd, message, wParam, lParam);
        const UINT dpi = windowDpi.load();
        const int minSide = MulDiv(MinimumClientSize, dpi, USER_DEFAULT_SCREEN_DPI);
        const RECT bounds = FrameBounds({0, 0, minSide, minSide}, dpi);
        auto& trackSize = reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize;
        trackSize.x = std::max(trackSize.x, bounds.right - bounds.left);
        trackSize.y = std::max(trackSize.y, bounds.bottom - bounds.top);
        return 0;
    }
    case WM_GETDPISCALEDSIZE: {
        if (resizable)
            return FALSE;
        const RECT bounds = WindowBounds(static_cast<UINT>(wParam));
        auto& size = *reinterpret_cast<SIZE*>(lParam);
        size = {bounds.right - bounds.left, bounds.bottom - bounds.top};
        return TRUE;
    }
    case WM_DPICHANGED: {
        const UINT dpi = LOWORD(wParam);
        if (windowDpi.exchange(dpi) != dpi && !windowIconPng.empty())
            ApplyWindowIcons(hwnd, dpi, windowIconPng.data(), windowIconSize);
        const auto& bounds = *reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_WINDOWPOSCHANGED:
        if (backSink)
            RefreshCaption(hwnd);
        break;
    case WM_SHOWWINDOW:
        if (!wParam) {
            backButton = BackButton::Idle;
            HideBackTooltip(hwnd);
            if (captionWindow)
                ShowWindow(captionWindow, SW_HIDE);
        }
        break;
    case WM_NCACTIVATE: {
        const auto result = DefWindowProcW(hwnd, message, wParam, lParam);
        captionActive = wParam != 0;
        if (!captionActive && backButton == BackButton::Hover)
            HideBackTooltip(hwnd);
        if (backSink)
            RefreshCaption(hwnd);
        return result;
    }
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE: {
        const BOOL dark = stone::windows::WindowsAppsUseDarkMode();
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }
        if (backSink)
            RefreshCaption(hwnd);
        break;
    case WM_NCHITTEST:
        return HitTestWindow(hwnd, lParam);
    case WM_GETICON:
        if (captionPlaceholderIcon && smallWindowIcon && wParam == ICON_SMALL2)
            return reinterpret_cast<LRESULT>(smallWindowIcon);
        break;
    case WM_NCMOUSEMOVE:
        if (captionPlaceholderIcon && !backButtonCaptured) {
            const bool overBack = HitTestWindow(hwnd, lParam) == HTSYSMENU;
            UpdateBackButton(hwnd, overBack ? BackButton::Hover : BackButton::Idle);
        }
        break;
    case WM_NCMOUSELEAVE:
        if (!backButtonCaptured)
            UpdateBackButton(hwnd, BackButton::Idle);
        break;
    case WM_NCMOUSEHOVER:
        if (backButton == BackButton::Hover && HitTestWindow(hwnd, lParam) == HTSYSMENU)
            ShowBackTooltip(hwnd);
        return 0;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (captionPlaceholderIcon && HitTestWindow(hwnd, lParam) == HTSYSMENU) {
            SetCapture(hwnd);
            backButtonCaptured = GetCapture() == hwnd;
            if (backButtonCaptured)
                UpdateBackButton(hwnd, BackButton::Pressed);
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (backButtonCaptured) {
            ReleaseBackButton(hwnd, HitTestWindow(hwnd, lParam) == HTSYSMENU);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == FrameTimer) {
            // A fixed-size native buffer may never match the new client size.
            if (resizeMask == ResizeMask::WaitingForFrame && GetTickCount64() >= resizeFrameDeadline) {
                resizeMask = ResizeMask::Hidden;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            Render(hwnd);
        }
        return 0;
    case WM_LBUTTONDOWN:
        EndHover(hwnd);
        DispatchTouch(hwnd, 0, lParam, 1);
        return 0;
    case WM_RBUTTONDOWN:
        EndHover(hwnd);
        DispatchTouch(hwnd, 0, lParam, 2);
        return 0;
    case WM_MOUSEMOVE:
        if (backButtonCaptured) {
            UpdateBackButton(hwnd,
                             BackButtonContainsClientPoint(hwnd, lParam) ? BackButton::Pressed : BackButton::Idle);
            return 0;
        }
        if ((wParam & (MK_LBUTTON | MK_RBUTTON)) && GetCapture() == hwnd)
            DispatchTouch(hwnd, 2, lParam, (wParam & MK_LBUTTON ? 1 : 0) | (wParam & MK_RBUTTON ? 2 : 0));
        else
            MoveHover(hwnd, wParam, lParam);
        return 0;
    case WM_MOUSELEAVE:
        trackingMouseLeave = false;
        EndHover(hwnd);
        return 0;
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wParam) == hwnd && LOWORD(lParam) == HTCLIENT) {
            SetCursor(hovering ? pointerCursor : LoadPointerIcon(1000));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (backButtonCaptured) {
            ReleaseBackButton(hwnd, BackButtonContainsClientPoint(hwnd, lParam));
            return 0;
        }
        if (GetCapture() == hwnd) {
            DispatchTouch(hwnd, 1, lParam);
            ReleaseCapture();
        }
        MoveHover(hwnd, wParam, lParam);
        return 0;
    case WM_KEYDOWN:
        DispatchKey(true, wParam, lParam);
        return 0;
    case WM_KEYUP:
        DispatchKey(false, wParam, lParam);
        return 0;
    case WM_CHAR:
        if (artInput.text && wParam >= L' ' && wParam != 127) {
            artInput.text(static_cast<wchar_t>(wParam));
            return 0;
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (artInput.focus)
            artInput.focus(message == WM_SETFOCUS);
        break;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        if (!artInput.scroll)
            break;
        POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        InputPoint point{};
        if (ScreenToClient(hwnd, &client) && MapInputPoint(hwnd, client, true, point)) {
            const float scale = BackingScale();
            artInput.scroll(message == WM_MOUSEHWHEEL, point.x * scale, (point.height - point.y) * scale,
                            GET_WHEEL_DELTA_WPARAM(wParam));
        }
        return 0;
    }
    case WM_CANCELMODE:
        if (backButtonCaptured)
            ReleaseCapture();
        break;
    case WM_CAPTURECHANGED:
        if (std::exchange(backButtonCaptured, false))
            UpdateBackButton(hwnd, BackButton::Idle);
        if (artInput.touch && std::exchange(nativeTouchDown, false))
            artInput.touch(3, nativeTouchX, nativeTouchY, 0);
        break;
    case WM_CLOSE:
        stone::windows::CloseWindowDocuments(hwnd);
        CloseWindowsDocumentPicker();
        CloseWindowsGl();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE) {
            Microsoft::WRL::ComPtr<IPropertyStore> properties;
            if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&properties)))) {
                PROPVARIANT empty{};
                for (const auto& key :
                     {PKEY_AppUserModel_ID, PKEY_AppUserModel_RelaunchCommand,
                      PKEY_AppUserModel_RelaunchDisplayNameResource, PKEY_AppUserModel_RelaunchIconResource})
                    properties->SetValue(key, empty);
            }
            properties.Reset();
            if (SUCCEEDED(initialized))
                CoUninitialize();
        }
    }
        stone::windows::CloseWindowSharing(hwnd);
        KillTimer(hwnd, FrameTimer);
        window = nullptr;
        closed = true;
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY: {
        const auto result = DefWindowProcW(hwnd, message, wParam, lParam);
        if (const auto icon = std::exchange(captionPlaceholderIcon, nullptr))
            DestroyIcon(icon);
        captionWindow = nullptr;
        backTooltip = nullptr;
        captionActive = false;
        backSink = nullptr;
        backButton = BackButton::Idle;
        backButtonCaptured = false;
        if (const auto icon = std::exchange(smallWindowIcon, nullptr))
            DestroyIcon(icon);
        if (const auto icon = std::exchange(largeWindowIcon, nullptr))
            DestroyIcon(icon);
        if (const auto icon = std::exchange(resizeWindowIcon, nullptr))
            DestroyIcon(icon);
        windowIconPng.clear();
        windowIconSize = 0;
        return result;
    }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool CreateWindowForDpi()
{
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW definition{};
    definition.lpfnWndProc = WindowProc;
    definition.hInstance = instance;
    definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    definition.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    definition.lpszClassName = WindowClass;
    if (!RegisterClassW(&definition)) {
        WNDCLASSW registered{};
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS || !GetClassInfoW(instance, WindowClass, &registered) ||
            registered.lpfnWndProc != WindowProc)
            return false;
    }

    shown = false;
    closed = false;
    callbackFailed = false;
    paintPending = false;
    firstFramePainted = firstGlFramePainted = firstNativeFramePainted = false;
    trackingMouseLeave = hovering = false;
    awaitingSizeCommit = false;
    resizable = false;
    resizeMask = ResizeMask::Hidden;

    const UINT systemDpi = GetDpiForSystem();
    const RECT bounds = WindowBounds(systemDpi);
    const auto hwnd =
        CreateWindowExW(0, WindowClass, title.c_str(), WindowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                        bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance, nullptr);
    if (!hwnd)
        return false;
    const BOOL dark = stone::windows::WindowsAppsUseDarkMode();
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    window = hwnd;
    stone::windows::SetClientHostChangedCallback(+[]() noexcept {
        if (auto target = WindowsHostWindow())
            PostMessageW(target, WM_THEMECHANGED, 0, 0);
    });
    const UINT dpi = GetDpiForWindow(hwnd);
    if (!dpi)
        return false;
    windowDpi = dpi;
    if (dpi != systemDpi) {
        const RECT corrected = WindowBounds(dpi);
        if (!SetWindowPos(hwnd, nullptr, 0, 0, corrected.right - corrected.left, corrected.bottom - corrected.top,
                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
            return false;
    }
    return true;
}

struct ArtWindowStartup {
    HANDLE ready;
    WindowsArtInput input;
    bool created = false;
};

DWORD WINAPI RunArtWindow(void* parameter)
{
    auto& startup = *static_cast<ArtWindowStartup*>(parameter);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool created = CreateWindowForDpi();
    const auto hwnd = window.load();
    if (created) {
        stone::windows::AttachWindowSharing(hwnd);
        artInput = startup.input;
        backSink = artInput.back;
        if (backSink)
            RefreshCaption(hwnd);
    }
    else if (hwnd) {
        DestroyWindow(hwnd);
    }
    startup.created = created;
    SetEvent(startup.ready);
    if (!created)
        return 0;

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (window.load() == hwnd)
        DestroyWindow(hwnd);
    return 0;
}

jboolean JNICALL Init(JNIEnv* env, jclass)
{
    uiEnv = env;
    const auto hwnd = window.load();
    return hwnd && SetTimer(hwnd, FrameTimer, 16, nullptr) ? JNI_TRUE : JNI_FALSE;
}

jboolean JNICALL EnableResize(JNIEnv*, jclass)
{
    const auto hwnd = window.load();
    if (!hwnd)
        return JNI_FALSE;
    RECT client{};
    if (!GetClientRect(hwnd, &client))
        return JNI_FALSE;
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_THICKFRAME | WS_MAXIMIZEBOX))
        return JNI_FALSE;
    resizable = true;
    const RECT bounds = FrameBounds(client, windowDpi.load());
    return SetWindowPos(hwnd, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED)
               ? JNI_TRUE
               : JNI_FALSE;
}

jboolean JNICALL SetWindowIcon(JNIEnv* env, jclass, jbyteArray encoded)
{
    if (!encoded)
        return JNI_FALSE;
    const auto size = env->GetArrayLength(encoded);
    if (size <= 0 || size > 1024 * 1024)
        return JNI_FALSE;
    auto* bytes = env->GetByteArrayElements(encoded, nullptr);
    if (!bytes)
        return JNI_FALSE;
    const bool applied = SetWindowsHostIcon({bytes, static_cast<size_t>(size)});
    env->ReleaseByteArrayElements(encoded, bytes, JNI_ABORT);
    return applied ? JNI_TRUE : JNI_FALSE;
}

void JNICALL SetAppUserModelId(JNIEnv* env, jclass, jstring id)
{
    const jchar* chars = env->GetStringChars(id, nullptr);
    if (!chars)
        return;
    const std::wstring appId(reinterpret_cast<const wchar_t*>(chars), env->GetStringLength(id));
    env->ReleaseStringChars(id, chars);
    SetWindowsHostAppUserModelId(appId.c_str());
}

void JNICALL RunLoop(JNIEnv*, jclass)
{
    MSG message{};
    int result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (result < 0)
        callbackFailed = true;
}

jboolean JNICALL WindowSucceeded(JNIEnv*, jclass)
{
    return shown && closed && HasPaintedFrame() && !callbackFailed;
}

jlong JNICALL Placeholder(JNIEnv*, jclass)
{
    return reinterpret_cast<jlong>(window.load());
}

jint JNICALL Width(JNIEnv*, jclass)
{
    return landscape ? CanvasHeight : CanvasWidth;
}

jint JNICALL Height(JNIEnv*, jclass)
{
    return landscape ? CanvasWidth : CanvasHeight;
}

jint JNICALL DisplayHz(JNIEnv*, jclass)
{
    return 60;
}

jfloat JNICALL Scale(JNIEnv*, jclass)
{
    return BackingScale();
}

void JNICALL SetTitle(JNIEnv* env, jclass, jlong, jstring value)
{
    if (!value)
        return;
    const jchar* chars = env->GetStringChars(value, nullptr);
    if (!chars)
        return;
    {
        const std::lock_guard lock(stateMutex);
        title.assign(reinterpret_cast<const wchar_t*>(chars), env->GetStringLength(value));
    }
    env->ReleaseStringChars(value, chars);
    if (const auto hwnd = window.load())
        PostMessageW(hwnd, ApplyTitle, 0, 0);
}

void JNICALL Show(JNIEnv*, jclass, jlong)
{
    if (const auto hwnd = window.load())
        PostMessageW(hwnd, ShowApp, 0, 0);
}

void JNICALL Close(JNIEnv*, jclass, jlong)
{
    if (const auto hwnd = window.load())
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

void JNICALL LaunchFailed(JNIEnv* env, jclass, jstring diagnostic)
{
    callbackFailed = true;
    if (const char* text = env->GetStringUTFChars(diagnostic, nullptr)) {
        std::fprintf(stderr, "Stone: %s\n", text);
        env->ReleaseStringUTFChars(diagnostic, text);
    }
    Close(env, nullptr, 0);
}

void JNICALL MoveToBack(JNIEnv*, jclass)
{
    if (const auto hwnd = window.load())
        PostMessageW(hwnd, HideApp, 0, 0);
}

void JNICALL MoveToFront(JNIEnv* env, jclass type)
{
    Show(env, type, 0);
}

void JNICALL StartingColor(JNIEnv*, jclass, jint argb)
{
    background = RGB((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff);
}

void JNICALL Orient(JNIEnv*, jclass, jboolean wide)
{
    if (const auto hwnd = window.load())
        PostMessageW(hwnd, ApplyOrientation, wide != JNI_FALSE, 0);
}

void JNICALL RestoreOrientation(JNIEnv* env, jclass type)
{
    Orient(env, type, JNI_FALSE);
}

jlong JNICALL CreateSurface(JNIEnv*, jclass, jint, jint)
{
    const std::lock_guard lock(stateMutex);
    return nextSurface++;
}

jlong JNICALL CreateGlSurface(JNIEnv*, jclass, jint, jint)
{
    const std::lock_guard lock(stateMutex);
    if (!glSurface)
        glSurface = nextSurface++;
    return glSurface;
}

void JNICALL ReleaseRenderer(JNIEnv* env, jclass, jlong surface)
{
    const std::lock_guard lock(stateMutex);
    const auto entry = renderers.find(surface);
    if (entry == renderers.end())
        return;
    env->DeleteGlobalRef(entry->second);
    renderers.erase(entry);
    if (activeSurface == surface)
        activeSurface = 0;
}

void JNICALL SetRenderer(JNIEnv* env, jclass type, jlong surface, jobject renderer)
{
    if (!renderer) {
        ReleaseRenderer(env, type, surface);
        return;
    }
    const std::lock_guard lock(stateMutex);
    if (const auto ref = env->NewGlobalRef(renderer)) {
        auto [entry, inserted] = renderers.try_emplace(surface, ref);
        if (!inserted) {
            env->DeleteGlobalRef(entry->second);
            entry->second = ref;
        }
    }
}

void JNICALL SetContent(JNIEnv*, jclass, jlong, jlong surface)
{
    const std::lock_guard lock(stateMutex);
    activeSurface = surface;
}

void JNICALL RegisterInput(JNIEnv* env, jclass, jobject dispatcher)
{
    const std::lock_guard lock(stateMutex);
    if (touchDispatcher)
        env->DeleteGlobalRef(touchDispatcher);
    touchDispatcher = dispatcher ? env->NewGlobalRef(dispatcher) : nullptr;
}

void JNICALL SetWindowsPointerIcon(JNIEnv*, jclass, jlong sequence, jint type)
{
    if (const auto hwnd = window.load()) {
        PostMessageW(hwnd, ApplyPointerIcon, static_cast<WPARAM>(sequence), static_cast<LPARAM>(type));
    }
}

void JNICALL PresentBitmap(JNIEnv* env, jclass, jintArray argb, jint width, jint height)
{
    const auto count = static_cast<jlong>(width) * height;
    if (!argb || width <= 0 || height <= 0 || count > env->GetArrayLength(argb))
        return;
    {
        const std::lock_guard lock(frameMutex);
        pixels.resize(static_cast<size_t>(count));
        env->GetIntArrayRegion(argb, 0, static_cast<jsize>(count), pixels.data());
        frameWidth = width;
        frameHeight = height;
    }
    RequestPaint();
}

void JNICALL PresentOverlayBitmap(JNIEnv* env, jclass, jintArray argb, jint width, jint height)
{
    const auto count = static_cast<jlong>(width) * height;
    {
        const std::lock_guard lock(frameMutex);
        if (!argb) {
            overlayPixels.clear();
        }
        else {
            if (width <= 0 || height <= 0 || count > env->GetArrayLength(argb))
                return;
            overlayPixels.resize(static_cast<size_t>(count));
            env->GetIntArrayRegion(argb, 0, static_cast<jsize>(count), overlayPixels.data());
            overlayWidth = width;
            overlayHeight = height;
        }
    }
    RequestPaint();
}

jint JNICALL PresentFrame(JNIEnv* env, jclass type, jintArray argb, jint width, jint height, jint, jint, jint, jint,
                          jint)
{
    PresentBitmap(env, type, argb, width, height);
    return 1;
}

void JNICALL PrintLog(JNIEnv* env, jclass, jint level, jstring tag, jstring message)
{
    if (level < 4 || !tag || !message)
        return;
    const char* label = env->GetStringUTFChars(tag, nullptr);
    const char* text = label ? env->GetStringUTFChars(message, nullptr) : nullptr;
    if (text) {
        std::fprintf(stderr, "[%s] %s\n", label, text);
        env->ReleaseStringUTFChars(message, text);
    }
    if (label)
        env->ReleaseStringUTFChars(tag, label);
}

void JNICALL IgnoreAspectRatio(JNIEnv*, jclass, jfloat)
{
}

void JNICALL IgnoreShimDirectory(JNIEnv*, jclass, jstring)
{
}

template <typename Function> JNINativeMethod Native(const char* name, const char* signature, Function implementation)
{
    return {const_cast<char*>(name), const_cast<char*>(signature), reinterpret_cast<void*>(implementation)};
}

template <size_t Count> bool Bind(JNIEnv* env, const char* className, const JNINativeMethod (&methods)[Count])
{
    const jclass type = env->FindClass(className);
    if (!type)
        return false;
    const bool success = env->RegisterNatives(type, methods, static_cast<jint>(Count)) == JNI_OK;
    env->DeleteLocalRef(type);
    return success;
}
} // namespace

bool OpenWindowsArtWindow(WindowsArtInput input)
{
    if (artWindowThread)
        return window.load() != nullptr;
    if (window.load())
        return false;
    captureProtected = false;
    ArtWindowStartup startup{CreateEventW(nullptr, FALSE, FALSE, nullptr), input};
    if (!startup.ready)
        return false;
    artWindowThread = CreateThread(nullptr, 0, RunArtWindow, &startup, 0, nullptr);
    if (artWindowThread)
        WaitForSingleObject(startup.ready, INFINITE);
    CloseHandle(startup.ready);
    if (startup.created)
        return true;
    if (artWindowThread)
        CloseHandle(std::exchange(artWindowThread, nullptr));
    return false;
}

bool CloseWindowsArtWindow()
{
    if (!artWindowThread)
        return true;
    if (const auto hwnd = window.load(); hwnd && !PostMessageW(hwnd, WM_CLOSE, 0, 0) && window.load() == hwnd)
        return false;
    if (WaitForSingleObject(artWindowThread, INFINITE) != WAIT_OBJECT_0)
        return false;
    CloseHandle(std::exchange(artWindowThread, nullptr));
    return true;
}

HWND WindowsHostWindow()
{
    return window.load();
}

bool SetWindowsArtOrientation(bool wide)
{
    const auto hwnd = window.load();
    if (!hwnd)
        return false;
    SendMessageW(hwnd, ApplyOrientation, wide, 0);
    return true;
}

bool SetWindowsArtResizable(bool enabled)
{
    const auto hwnd = window.load();
    return hwnd && SendMessageW(hwnd, ApplyArtResize, enabled, 0) != 0;
}

uint64_t WindowsArtClientSize()
{
    const auto hwnd = window.load();
    return hwnd ? static_cast<uint64_t>(SendMessageW(hwnd, QueryArtSize, 0, 0)) : 0;
}

bool SetWindowsHostAppearance(const std::wstring& label, const std::wstring& icon, const std::wstring& command)
{
    const auto hwnd = window.load();
    if (!hwnd)
        return false;
    Appearance request{label, icon, command};
    return SendMessageW(hwnd, ApplyAppearance, 0, reinterpret_cast<LPARAM>(&request)) != 0;
}

bool SetWindowsHostIcon(std::span<const jbyte> encoded)
{
    const auto hwnd = window.load();
    if (!hwnd || encoded.empty() || encoded.size() > 1024 * 1024)
        return false;
    if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) {
        const std::lock_guard caller(iconCallerMutex);
        {
            const std::lock_guard lock(iconRequestMutex);
            iconRequest = encoded;
        }
        const bool applied = SendMessageW(hwnd, ApplyIcon, 0, 0) != 0;
        const std::lock_guard lock(iconRequestMutex);
        iconRequest = {};
        return applied;
    }
    try {
        // CreateIconFromResourceEx requires DWORD-aligned PNG data.
        std::vector<DWORD> png((encoded.size() + sizeof(DWORD) - 1) / sizeof(DWORD));
        std::memcpy(png.data(), encoded.data(), encoded.size());
        const auto size = static_cast<DWORD>(encoded.size());
        if (!ApplyWindowIcons(hwnd, windowDpi.load(), png.data(), size))
            return false;
        windowIconPng = std::move(png);
        windowIconSize = size;
        return true;
    }
    catch (const std::bad_alloc&) {
        return false;
    }
}

void SetWindowsHostAppUserModelId(const wchar_t* id)
{
    if (FAILED(SetCurrentProcessExplicitAppUserModelID(id)))
        std::fprintf(stderr, "[win32] [WARN] could not assign an APK taskbar identity\n");
}

void SetWindowsVulkanPresentation(bool active)
{
    if (vulkanPresentation.exchange(active) && !active)
        RequestPaint();
}

HMONITOR WindowsHostMonitor()
{
    return MonitorFromWindow(WindowsHostWindow(), MONITOR_DEFAULTTOPRIMARY);
}

void PresentWindowsNativeFrame(std::span<const jint> frame, int width, int height)
{
    {
        const std::lock_guard lock(frameMutex);
        nativePixels.assign(frame.begin(), frame.end());
        nativeWidth = width;
        nativeHeight = height;
    }
    RequestPaint();
}

int WindowsArtCaptureProtection()
{
    const auto hwnd = window.load();
    DWORD affinity = 0;
    return hwnd && GetWindowDisplayAffinity(hwnd, &affinity) ? static_cast<int>(affinity) : -1;
}

bool PresentWindowsArtFrame(std::vector<jint>& frame, int width, int height, bool secure)
{
    const auto hwnd = window.load();
    if (!hwnd)
        return false;
    // Protection must take effect before any pixels from the secure frame can
    // reach WM_PAINT, including the initial visible frame.
    if (secure && !captureProtected.load() && !SendMessageW(hwnd, ApplyCaptureProtection, TRUE, 0))
        return false;
    {
        const std::lock_guard lock(frameMutex);
        nativePixels.swap(frame);
        nativeWidth = width;
        nativeHeight = height;
    }
    if (!secure && captureProtected.load() && !SendMessageW(hwnd, ApplyCaptureProtection, FALSE, 0))
        return false;
    if (!firstNativeFramePainted)
        return SendMessageW(hwnd, ShowFirstArtFrame, 0, 0) != 0;
    RequestPaint();
    return true;
}

int AndroidDisplayDensity()
{
    if (window.load())
        return MulDiv(160, windowDpi.load(), USER_DEFAULT_SCREEN_DPI);
    // Resources are initialized before the application creates its HWND. Avoid
    // DPI virtualization in hosts that have not declared process awareness.
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const UINT dpi = GetDpiForSystem();
    if (previous)
        SetThreadDpiAwarenessContext(previous);
    return MulDiv(160, dpi, USER_DEFAULT_SCREEN_DPI);
}

bool RegisterWindowsImageCodec(JNIEnv* env);
bool RegisterWallpaperBridgeMethods(JNIEnv* env);

namespace stone::windows {
bool RegisterWindowsAudioTrack(JNIEnv* env);
bool RegisterWindowsMediaDecoder(JNIEnv* env);
}

bool RegisterWindowsNatives(JNIEnv* env)
{
    const JNINativeMethod cocoa[] = {
        Native("init", "()Z", Init),
        Native("enableWindowsWindowResize", "()Z", EnableResize),
        Native("setWindowsWindowIcon", "([B)Z", SetWindowIcon),
        Native("setWindowsPointerIcon", "(JI)V", SetWindowsPointerIcon),
        Native("runLoop", "()V", RunLoop),
        Native("getPlaceholderWindow", "()J", Placeholder),
        Native("phoneWidth", "()I", Width),
        Native("phoneHeight", "()I", Height),
        Native("displayHz", "()I", DisplayHz),
        Native("backingScale", "()F", Scale),
        Native("setWindowTitle", "(JLjava/lang/String;)V", SetTitle),
        Native("showWindow", "(J)V", Show),
        Native("closeWindow", "(J)V", Close),
        Native("windowsLaunchFailed", "(Ljava/lang/String;)V", LaunchFailed),
        Native("moveTaskToBack", "()V", MoveToBack),
        Native("moveTaskToFront", "()V", MoveToFront),
        Native("setStartingWindowColor", "(I)V", StartingColor),
        Native("applyRequestedOrientation", "(Z)V", Orient),
        Native("restoreUserOrientation", "()V", RestoreOrientation),
        Native("createMetalSurfaceView", "(II)J", CreateSurface),
        Native("createMetalSurfaceViewForGl", "(II)J", CreateGlSurface),
        Native("setRenderer", "(JLcom/trebledot/stone/loader/MetalSurfaceCallbacks;)V", SetRenderer),
        Native("releaseRenderer", "(J)V", ReleaseRenderer),
        Native("setContentView", "(JJ)V", SetContent),
        Native("registerTouchDispatcher", "(Lcom/trebledot/stone/loader/TouchDispatcher;)V", RegisterInput),
        Native("presentBitmap", "([III)V", PresentBitmap),
        Native("presentOverlayBitmap", "([III)V", PresentOverlayBitmap),
        Native("presentFrame", "([IIIIIIII)I", PresentFrame),
        Native("setMaxAspectRatio", "(F)V", IgnoreAspectRatio),
        Native("setDataSubdir", "(Ljava/lang/String;)V", IgnoreShimDirectory),
        Native("setNativeLibDir", "(Ljava/lang/String;)V", IgnoreShimDirectory),
        Native("setSharedStorageRoot", "(Ljava/lang/String;)V", IgnoreShimDirectory),
    };
    const JNINativeMethod boot[] = {
        Native("setAppUserModelId", "(Ljava/lang/String;)V", SetAppUserModelId),
        Native("windowSucceeded", "()Z", WindowSucceeded),
    };
    return Bind(env, "com/trebledot/stone/loader/NativeCocoa", cocoa) &&
           Bind(env, "com/trebledot/stone/loader/WindowsBoot", boot) && RegisterWindowsDocumentPicker(env) &&
           RegisterWindowsImageCodec(env) && stone::windows::RegisterWindowsAudioTrack(env) &&
           stone::windows::RegisterWindowsMediaDecoder(env) && RegisterWallpaperBridgeMethods(env) &&
           RegisterWindowsGles(env);
}

bool RegisterWindowsLog(JNIEnv* env)
{
    const JNINativeMethod log[] = {Native("print", "(ILjava/lang/String;Ljava/lang/String;)V", PrintLog)};
    return Bind(env, "com/trebledot/stone/loader/NativeLog", log);
}
