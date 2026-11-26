#include "../../../src/windows/client/Window.h"
#include "../../../src/windows/client/LaunchScreen.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <cstdio>
#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace {
using namespace winrt::Windows::Graphics::Capture;

void Check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

void Present(jint color, bool secure)
{
    std::vector<jint> pixels(64 * 64, color);
    Check(PresentWindowsArtFrame(pixels, 64, 64, secure), "Could not present capture fixture frame");
    Check(WindowsArtCaptureProtection() == (secure ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE),
          "Native display affinity does not match the frame policy");
}

struct Colors {
    int red = 0, green = 0, blue = 0, black = 0, count = 0;
};

struct NoCaptureFrame : std::runtime_error {
    NoCaptureFrame() : std::runtime_error("No frame from the own-window Windows capture session")
    {
    }
};

struct ControlWindow {
    HWND window = nullptr;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr), thread = nullptr;

    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_PAINT) {
            PAINTSTRUCT paint;
            auto dc = BeginPaint(window, &paint);
            RECT bounds;
            GetClientRect(window, &bounds);
            auto brush = CreateSolidBrush(RGB(234, 67, 53));
            FillRect(dc, &bounds, brush);
            DeleteObject(brush);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static DWORD WINAPI Run(void* value)
    {
        auto& self = *static_cast<ControlWindow*>(value);
        WNDCLASSW type{};
        type.lpfnWndProc = Procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"Stone.Capture.UnprotectedControl";
        RegisterClassW(&type);
        self.window =
            CreateWindowExW(0, type.lpszClassName, L"Stone unprotected capture control", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, nullptr, nullptr, type.hInstance, nullptr);
        if (self.window) {
            ShowWindow(self.window, SW_SHOWNOACTIVATE);
            UpdateWindow(self.window);
        }
        SetEvent(self.ready);
        if (!self.window)
            return 1;
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return 0;
    }

    ControlWindow()
    {
        Check(ready != nullptr, "Cannot create capture control ready event");
        thread = CreateThread(nullptr, 0, Run, this, 0, nullptr);
        Check(thread && WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0 && window,
              "Cannot create independent Win32 capture control");
    }

    ~ControlWindow()
    {
        if (window)
            PostMessageW(window, WM_CLOSE, 0, 0);
        if (thread) {
            WaitForSingleObject(thread, 5000);
            CloseHandle(thread);
        }
        if (ready)
            CloseHandle(ready);
    }
};

struct Capture {
    HWND target;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureItem item{nullptr};
    winrt::event_token arrived{};
    GraphicsCaptureSession session{nullptr};

    explicit Capture(HWND window) : target(window)
    {
        auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                        nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put());
        if (FAILED(result))
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put());
        winrt::check_hresult(result);
        auto dxgi = device.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        auto native = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        // Only our own Stone fixture HWND is ever targeted. No desktop/monitor capture.
        winrt::check_hresult(
            factory->CreateForWindow(window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
        std::fprintf(stderr, "Capture item: %dx%d\n", item.Size().Width, item.Size().Height);
        pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            native, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, item.Size());
        arrived = pool.FrameArrived([](auto const&, auto const&) {
        });
        session = pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(false);
        session.StartCapture();
    }

    ~Capture()
    {
        try {
            if (session)
                session.Close();
            if (pool) {
                pool.FrameArrived(arrived);
                pool.Close();
            }
        }
        catch (...) {
        }
    }

    Colors Read(DWORD timeout = 5000)
    {
        const auto deadline = GetTickCount64() + timeout;
        while (GetTickCount64() < deadline) {
            RedrawWindow(target, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            auto frame = pool.TryGetNextFrame();
            if (!frame) {
                Sleep(20);
                continue;
            }
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            description.Usage = D3D11_USAGE_STAGING;
            description.BindFlags = 0;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            description.MiscFlags = 0;
            winrt::com_ptr<ID3D11Texture2D> staging;
            winrt::check_hresult(device->CreateTexture2D(&description, nullptr, staging.put()));
            context->CopyResource(staging.get(), texture.get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
            Colors colors;
            const int width = static_cast<int>(description.Width), height = static_cast<int>(description.Height);
            for (int y = std::max(0, height / 2 - 8); y < std::min(height, height / 2 + 8); ++y)
                for (int x = std::max(0, width / 2 - 8); x < std::min(width, width / 2 + 8); ++x) {
                    auto pixel = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                    const int blue = pixel[0], green = pixel[1], red = pixel[2];
                    ++colors.count;
                    if (red > 170 && green < 125 && blue < 125)
                        ++colors.red;
                    if (green > 130 && red < 110 && blue < 140)
                        ++colors.green;
                    if (blue > 160 && red < 130 && green < 190)
                        ++colors.blue;
                    if (red < 8 && green < 8 && blue < 8)
                        ++colors.black;
                }
            context->Unmap(staging.get(), 0);
            frame.Close();
            return colors;
        }
        throw NoCaptureFrame();
    }

    Colors Public(bool blue = false)
    {
        const auto deadline = GetTickCount64() + 5000;
        for (;;) {
            const auto now = GetTickCount64();
            if (now >= deadline)
                break;
            auto pixels = Read(static_cast<DWORD>(deadline - now));
            if (pixels.count > 0 && (blue ? pixels.blue : pixels.red) == pixels.count)
                return pixels;
        }
        throw std::runtime_error("Capture did not deliver the actual public fixture pixels");
    }
};

const char* ObserveProtected(Capture& capture, bool previousPublic)
{
    const auto deadline = GetTickCount64() + 1500;
    bool received = false;
    try {
        for (;;) {
            const auto now = GetTickCount64();
            if (now >= deadline)
                break;
            const auto pixels = capture.Read(static_cast<DWORD>(deadline - now));
            received = true;
            Check(pixels.count > 0 && (pixels.black == pixels.count || (previousPublic && pixels.blue == pixels.count)),
                  "Protected Windows capture exposed private frame pixels");
        }
    }
    catch (const NoCaptureFrame&) {
    }
    return received ? "only black or prior public frames" : "frames withheld";
}

int VerifyControl()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (!GraphicsCaptureSession::IsSupported())
        return 77;
    ControlWindow window;
    try {
        DwmFlush();
        Capture capture(window.window);
        const auto colors = capture.Public();
        auto dc = GetDC(window.window);
        RECT bounds;
        GetClientRect(window.window, &bounds);
        auto actual = GetPixel(dc, (bounds.right - bounds.left) / 2, (bounds.bottom - bounds.top) / 2);
        ReleaseDC(window.window, dc);
        std::printf("Control pixels: GDI=%lu captured red=%d green=%d blue=%d black=%d total=%d\n", actual, colors.red,
                    colors.green, colors.blue, colors.black, colors.count);
        Check(colors.red == colors.count && colors.count > 0,
              "Independent Win32 capture did not return the actual control pixels");
        std::puts("PASS independent unprotected Win32 control capture");
        return 0;
    }
    catch (const NoCaptureFrame&) {
        std::puts("SKIP independent unprotected Win32 control received no Windows capture frames");
        return 77;
    }
    catch (const winrt::hresult_error& error) {
        if (error.code() != E_ACCESSDENIED && error.code() != E_NOTIMPL && error.code() != DXGI_ERROR_UNSUPPORTED &&
            error.code() != HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY))
            throw;
        std::printf("SKIP independent Win32 capture unavailable: 0x%08lx\n",
                    static_cast<unsigned long>(error.code().value));
        return 77;
    }
}

int ControlProcess()
{
    wchar_t executable[32768];
    auto length = GetModuleFileNameW(nullptr, executable, 32768);
    Check(length && length < 32768, "Cannot locate independent capture control executable");
    auto command = L"\"" + std::wstring(executable, length) + L"\" --capture-control";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                         &startup, &process),
          "Cannot start independent capture control process");
    CloseHandle(process.hThread);
    DWORD result = 1;
    const auto waited = WaitForSingleObject(process.hProcess, 15000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    const auto read = GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);
    Check(waited == WAIT_OBJECT_0 && read, "Independent capture control process timed out");
    return static_cast<int>(result);
}

int VerifyCapture()
{
    Check(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != nullptr,
          "Cannot align capture thread DPI awareness");
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (!GraphicsCaptureSession::IsSupported()) {
        std::puts("SKIP Windows Graphics Capture is unavailable; affinity is tested separately");
        return 77;
    }
    const auto control = ControlProcess();
    if (control == 77) {
        std::puts(
            "SKIP actual pixel capture: independent unprotected Win32 control cannot receive frames; native affinity is tested separately");
        return 77;
    }
    Check(control == 0, "Independent unprotected Win32 control failed");
    Present(static_cast<jint>(0xffea4335), false);
    DwmFlush();
    wchar_t station[256]{}, desktop[256]{};
    DWORD needed;
    GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, station, sizeof(station), &needed);
    GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &needed);
    std::fwprintf(stderr, L"Capture baseline: station=%s desktop=%s visible=%d\n", station, desktop,
                  IsWindowVisible(WindowsHostWindow()));
    RECT bounds{};
    GetClientRect(WindowsHostWindow(), &bounds);
    DWORD cloaked = 0;
    auto cloakResult = DwmGetWindowAttribute(WindowsHostWindow(), DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    auto dc = GetDC(WindowsHostWindow());
    auto pixel = GetPixel(dc, bounds.right / 2, bounds.bottom / 2);
    ReleaseDC(WindowsHostWindow(), dc);
    std::fprintf(stderr, "Native fixture: client=%ldx%ld cloaked=%lu (hr=%08lx) GDI=%lu affinity=%d\n", bounds.right,
                 bounds.bottom, cloaked, static_cast<unsigned long>(cloakResult), pixel, WindowsArtCaptureProtection());
    Capture recording(WindowsHostWindow());
    const auto baseline = recording.Public();
    Check(baseline.red == baseline.count && baseline.count > 0,
          "Unprotected Windows capture did not contain the real fixture pixels");
    Present(static_cast<jint>(0xff4285f4), false);
    DwmFlush();
    const auto second = recording.Public(true);
    Check(second.blue == second.count && second.count > 0,
          "Existing capture session did not receive a second public frame");
    std::puts("PASS actual unprotected Stone window pixels captured");
    Present(static_cast<jint>(0xff34a853), true);
    const char* liveProtection;
    try {
        liveProtection = ObserveProtected(recording, true);
    }
    catch (const winrt::hresult_error& error) {
        if (error.code() != E_ACCESSDENIED && error.code() != E_INVALIDARG && error.code() != RO_E_CLOSED)
            throw;
        liveProtection = "session stopped";
    }
    const char* protection;
    try {
        Capture capture(WindowsHostWindow());
        protection = ObserveProtected(capture, false);
    }
    catch (const winrt::hresult_error& error) {
        if (error.code() != E_ACCESSDENIED && error.code() != E_INVALIDARG && error.code() != RO_E_CLOSED)
            throw;
        protection = "session rejected";
    }
    Present(static_cast<jint>(0xffea4335), false);
    DwmFlush();
    Capture restored(WindowsHostWindow());
    const auto fresh = restored.Public();
    Check(fresh.red == fresh.count && fresh.count > 0,
          "Capture did not resume with the new public frame after clearing protection");
    std::printf(
        "PASS actual own-window capture: public pixels, existing recording %s, new secure capture %s, then fresh public pixels\n",
        liveProtection, protection);
    return 0;
}
}

int wmain(int argc, wchar_t** argv)
{
    int result = 1;
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--capture-control")
            return VerifyControl();
        if (argc == 2 && std::wstring_view(argv[1]) == L"--launch-screen") {
            stone::windows::StartLaunchScreen(L"Stone startup regression");
            const auto splash = FindWindowW(L"Stone.LaunchScreen", L"Stone startup regression");
            Check(splash && IsWindowVisible(splash), "Startup screen was not visible before ART window creation");
            const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            RECT bounds{};
            MONITORINFO monitor{sizeof(monitor)};
            Check(GetWindowRect(splash, &bounds) &&
                      GetMonitorInfoW(MonitorFromWindow(splash, MONITOR_DEFAULTTONEAREST), &monitor),
                  "Could not inspect startup screen placement");
            const auto& work = monitor.rcWork;
            const auto dpi = GetDpiForWindow(splash);
            RECT visible{};
            Check(SUCCEEDED(DwmGetWindowAttribute(splash, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible))),
                  "Could not inspect visible startup window bounds");
            std::printf("Startup visible bounds: %ldx%ld at (%ld,%ld)\n", visible.right - visible.left,
                        visible.bottom - visible.top, visible.left, visible.top);
            std::printf("Startup screen: %ldx%ld at (%ld,%ld), DPI=%u, work=(%ld,%ld,%ld,%ld)\n",
                        bounds.right - bounds.left, bounds.bottom - bounds.top, bounds.left, bounds.top, dpi, work.left,
                        work.top, work.right, work.bottom);
            const int invisibleWidth = (bounds.right - bounds.left) - (visible.right - visible.left);
            const int invisibleHeight = (bounds.bottom - bounds.top) - (visible.bottom - visible.top);
            Check(visible.right - visible.left ==
                          std::min<int>(MulDiv(608, dpi, 96), work.right - work.left - invisibleWidth) &&
                      visible.bottom - visible.top ==
                          std::min<int>(MulDiv(342, dpi, 96), work.bottom - work.top - invisibleHeight),
                  "Startup screen dimensions do not match the DPI-scaled reference");
            Check(std::abs((bounds.left + bounds.right) - (work.left + work.right)) <= 1 &&
                      std::abs((bounds.top + bounds.bottom) - (work.top + work.bottom)) <= 1,
                  "Startup screen is not centered in its monitor work area");
            Check(!(GetWindowLongPtrW(splash, GWL_STYLE) & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME)),
                  "Startup screen has extra caption or resize controls");
            if (previousDpi)
                SetThreadDpiAwarenessContext(previousDpi);
            Check(OpenWindowsArtWindow({}) && !IsWindowVisible(WindowsHostWindow()),
                  "Application window became visible before its first frame");
            Check(IsWindowVisible(splash), "Startup screen closed before the first application frame");
            Present(static_cast<jint>(0xffea4335), false);
            for (int attempt = 0; attempt < 100 && IsWindow(splash); ++attempt)
                Sleep(10);
            Check(!IsWindow(splash) && IsWindowVisible(WindowsHostWindow()),
                  "First painted application frame did not replace the startup screen");
            Check(CloseWindowsArtWindow(), "Could not close startup regression window");
            std::puts("PASS startup screen remains visible until the first application frame is painted");
            return 0;
        }
        Check(OpenWindowsArtWindow({}), "Could not open Stone capture fixture");
        if (argc == 2 && std::wstring_view(argv[1]) == L"--capture") {
            result = VerifyCapture();
            CloseWindowsArtWindow();
            return result;
        }
        Present(static_cast<jint>(0xffea4335), false);
        Present(static_cast<jint>(0xff34a853), true);
        Present(static_cast<jint>(0xff4285f4), false);
        Check(CloseWindowsArtWindow() && WindowsArtCaptureProtection() == -1,
              "Capture state survived window destruction");
        Check(OpenWindowsArtWindow({}) && WindowsArtCaptureProtection() == 0,
              "Reopened window inherited stale capture protection");
        Present(static_cast<jint>(0xff34a853), true);
        std::puts(
            "PASS native frame capture protection, clear after fresh paint, first secure frame and window recreation");
        result = 0;
    }
    catch (const winrt::hresult_error& error) {
        std::fprintf(stderr, "FAIL Windows capture HRESULT 0x%08lx\n", static_cast<unsigned long>(error.code().value));
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL capture protection: %s\n", error.what());
    }
    CloseWindowsArtWindow();
    return result;
}
