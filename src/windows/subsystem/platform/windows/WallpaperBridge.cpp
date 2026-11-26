#include "../../../client/Window.h"

#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;

// Match HostWallpaper.MAX_SCREEN_PIXELS and bound the decoded image buffer as well.
constexpr int64_t MaxPixels = 8192LL * 8192;

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ~ComApartment()
    {
        if (SUCCEEDED(result))
            CoUninitialize();
    }

    bool available() const
    {
        return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }
};

using ShellString = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>;

ShellString FindWallpaperMonitor(IDesktopWallpaper* desktop, const RECT& monitor)
{
    UINT count = 0;
    desktop->GetMonitorDevicePathCount(&count);
    for (UINT index = 0; index < count; ++index) {
        wchar_t* rawId = nullptr;
        const HRESULT result = desktop->GetMonitorDevicePathAt(index, &rawId);
        ShellString id(rawId, CoTaskMemFree);
        RECT bounds{};
        if (result == S_OK && id && desktop->GetMonitorRECT(id.get(), &bounds) == S_OK && EqualRect(&monitor, &bounds))
            return id;
    }
    // A null monitor ID can resolve an image shared by all monitors.
    return ShellString(nullptr, CoTaskMemFree);
}

DESKTOP_WALLPAPER_POSITION ReadDesktopPosition()
{
    constexpr auto key = L"Control Panel\\Desktop";
    wchar_t value[16]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"TileWallpaper", RRF_RT_REG_SZ, nullptr, value, &bytes) ==
            ERROR_SUCCESS &&
        std::wstring_view(value) == L"1")
        return DWPOS_TILE;

    bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"WallpaperStyle", RRF_RT_REG_SZ, nullptr, value, &bytes) ==
        ERROR_SUCCESS) {
        const std::wstring_view style(value);
        if (style == L"2")
            return DWPOS_STRETCH;
        if (style == L"6")
            return DWPOS_FIT;
        if (style == L"10")
            return DWPOS_FILL;
        if (style == L"22")
            return DWPOS_SPAN;
    }
    return DWPOS_CENTER;
}

jbyteArray JNICALL DesktopInfo(JNIEnv* env, jclass)
try {
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(WindowsHostMonitor(), &monitor))
        return nullptr;

    DESKTOP_WALLPAPER_POSITION position{};
    COLORREF background = GetSysColor(COLOR_BACKGROUND);
    ShellString path(nullptr, CoTaskMemFree);
    const ComApartment apartment;
    ComPtr<IDesktopWallpaper> desktop;
    if (apartment.available() &&
        SUCCEEDED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&desktop))) &&
        SUCCEEDED(desktop->GetPosition(&position))) {
        const auto monitorId = FindWallpaperMonitor(desktop.Get(), monitor.rcMonitor);
        wchar_t* filename = nullptr;
        const auto result = desktop->GetWallpaper(monitorId.get(), &filename);
        path.reset(filename);
        if (result == S_OK) {
            if (!path || !*path)
                return nullptr;
            COLORREF color{};
            if (SUCCEEDED(desktop->GetBackgroundColor(&color)))
                background = color;
        }
        else {
            path.reset();
        }
    }

    wchar_t systemPath[MAX_PATH]{};
    if (!path) {
        // The Shell COM service can be unavailable while the desktop still has a wallpaper.
        if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(systemPath), systemPath, 0) || !*systemPath)
            return nullptr;
        position = ReadDesktopPosition();
    }
    const wchar_t* filename = path ? path.get() : systemPath;

    int width = monitor.rcMonitor.right - monitor.rcMonitor.left;
    int height = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
    if (position == DWPOS_SPAN || position == DWPOS_TILE) {
        // These settings lay out the picture on the whole virtual desktop.
        width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
    if (width <= 0 || height <= 0 || static_cast<int64_t>(width) * height > MaxPixels)
        return nullptr;

    const uint32_t fill =
        0xff000000u | (GetRValue(background) << 16) | (GetGValue(background) << 8) | GetBValue(background);
    const bool clip = position == DWPOS_CENTER || position == DWPOS_FILL || position == DWPOS_SPAN;

    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, filename, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
        return nullptr;
    std::string info(bytes, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, filename, -1, info.data(), bytes, nullptr, nullptr))
        return nullptr;
    info.pop_back();
    info += '\t' + std::to_string(position) + '\t' + std::to_string(clip) + '\t' + std::to_string(fill) + '\t' +
            std::to_string(width) + '\t' + std::to_string(height);
    const auto length = static_cast<jsize>(info.size());
    const auto output = env->NewByteArray(length);
    if (output)
        env->SetByteArrayRegion(output, 0, length, reinterpret_cast<const jbyte*>(info.data()));
    return output;
}
catch (const std::bad_alloc&) {
    return nullptr;
}

uint32_t Composite(uint32_t pixel, uint32_t background)
{
    const uint32_t alpha = pixel >> 24;
    if (alpha == 255)
        return pixel;
    uint32_t result = 0xff000000u;
    for (int shift : {0, 8, 16}) {
        const uint32_t source = (pixel >> shift) & 255;
        const uint32_t fill = (background >> shift) & 255;
        result |= ((source * alpha + fill * (255 - alpha) + 127) / 255) << shift;
    }
    return result;
}

jintArray JNICALL Render(JNIEnv* env, jclass, jbyteArray path, jint position, jint, jint fill, jint screenW,
                         jint screenH, jint outputW, jint outputH)
try {
    if (!path || screenW <= 0 || screenH <= 0 || outputW <= 0 || outputH <= 0 || outputW > screenW ||
        outputH > screenH || static_cast<int64_t>(screenW) * screenH > MaxPixels || position < DWPOS_CENTER ||
        position > DWPOS_SPAN)
        return nullptr;
    const jsize bytes = env->GetArrayLength(path);
    if (bytes == 0)
        return nullptr;
    std::string utf8(bytes, '\0');
    env->GetByteArrayRegion(path, 0, bytes, reinterpret_cast<jbyte*>(utf8.data()));
    if (env->ExceptionCheck() || utf8.find('\0') != std::string::npos)
        return nullptr;
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), bytes, nullptr, 0);
    if (!chars)
        return nullptr;
    std::wstring filename(chars, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), bytes, filename.data(), chars)) {
        return nullptr;
    }

    const ComApartment apartment;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT width = 0, height = 0;
    if (!apartment.available() ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(filename.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || !width || !height ||
        static_cast<uint64_t>(width) * height > MaxPixels || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return nullptr;

    // Straight BGRA bytes map to packed ARGB on little-endian Windows.
    std::vector<uint32_t> source(static_cast<size_t>(width) * height);
    if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(width * sizeof(uint32_t)),
                                     static_cast<UINT>(source.size() * sizeof(uint32_t)),
                                     reinterpret_cast<BYTE*>(source.data())))) {
        return nullptr;
    }

    double scaleX = 1, scaleY = 1;
    if (position == DWPOS_STRETCH || position == DWPOS_FIT || position == DWPOS_FILL || position == DWPOS_SPAN) {
        scaleX = static_cast<double>(screenW) / width;
        scaleY = static_cast<double>(screenH) / height;
        if (position == DWPOS_FIT)
            scaleX = scaleY = std::min(scaleX, scaleY);
        if (position == DWPOS_FILL || position == DWPOS_SPAN)
            scaleX = scaleY = std::max(scaleX, scaleY);
    }
    const bool tile = position == DWPOS_TILE;
    const double left = tile ? 0 : (screenW - width * scaleX) / 2;
    const double top = tile ? 0 : (screenH - height * scaleY) / 2;

    // Project the sample centers through the desktop placement without allocating a screen-sized image.
    std::vector<jint> pixels(static_cast<size_t>(outputW) * outputH, fill);
    for (int y = 0; y < outputH; ++y) {
        double srcY = ((y + 0.5) * screenH / outputH - top) / scaleY;
        if (tile)
            srcY = std::fmod(srcY, height);
        if (srcY < 0 || srcY >= height)
            continue;
        const auto row = static_cast<size_t>(srcY) * width;
        for (int x = 0; x < outputW; ++x) {
            double srcX = ((x + 0.5) * screenW / outputW - left) / scaleX;
            if (tile)
                srcX = std::fmod(srcX, width);
            if (srcX < 0 || srcX >= width)
                continue;
            pixels[static_cast<size_t>(y) * outputW + x] =
                static_cast<jint>(Composite(source[row + static_cast<size_t>(srcX)], static_cast<uint32_t>(fill)));
        }
    }
    const auto count = static_cast<jsize>(pixels.size());
    const auto output = env->NewIntArray(count);
    if (output)
        env->SetIntArrayRegion(output, 0, count, pixels.data());
    return output;
}
catch (const std::bad_alloc&) {
    return nullptr;
}
} // namespace

bool RegisterWallpaperBridgeMethods(JNIEnv* env)
{
    const auto type = env->FindClass("com/trebledot/stone/loader/monet/HostWallpaper");
    if (!type)
        return false;
    const JNINativeMethod methods[] = {
        {const_cast<char*>("nativeDesktopInfo"), const_cast<char*>("()[B"), reinterpret_cast<void*>(DesktopInfo)},
        {const_cast<char*>("nativeRender"), const_cast<char*>("([BIIIIIII)[I"), reinterpret_cast<void*>(Render)},
    };
    const bool registered = env->RegisterNatives(type, methods, ARRAYSIZE(methods)) == JNI_OK;
    env->DeleteLocalRef(type);
    return registered;
}
