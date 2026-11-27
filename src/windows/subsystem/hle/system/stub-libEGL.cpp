#include "abi/AndroidLibraryImports.h"
#include "../../../host/graphics/GlSurface.h"
#include "../../../client/Window.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "stub-libandroid.h"
#include "stub-libmediandk-image-reader.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>

namespace stone::windows {
namespace {
struct ScreenPacing {
    std::mutex mutex;
    int interval = 1;
    int minimum = 0, maximum = 1;
    std::chrono::steady_clock::time_point next{};

    void Wait()
    {
        const std::lock_guard lock(mutex);
        if (!interval)
            return;
        // Software screen refresh is independent of the UI consuming frames.
        // Never wait for the UI here: it may itself be waiting for a GL event.
        const auto period = std::chrono::nanoseconds(1000000000LL * interval / 60);
        const auto now = std::chrono::steady_clock::now();
        if (next == std::chrono::steady_clock::time_point{} || next + period < now)
            next = now;
        next += period;
        std::this_thread::sleep_until(next);
    }
};

struct GuestEglWindow {
    NativeWindow* window;
    EGLDisplay display;
    EGLnsecsANDROID timestamp = -1;
    std::shared_ptr<ScreenPacing> pacing;
};

using GuestEglWindows = std::unordered_map<EGLSurface, GuestEglWindow>;
std::mutex eglWindowsMutex;
GuestEglWindows eglWindows;
std::unordered_map<EGLDisplay, unsigned> displayReferences;
std::mutex displayReferencesMutex;
thread_local EGLint eglBridgeError = EGL_SUCCESS;

EGLBoolean InitializeGuestDisplay(EGLDisplay display, EGLint* major, EGLint* minor)
{
    const std::lock_guard lock(displayReferencesMutex);
    if (!InitializeWindowsAngleDisplay(display, major, minor))
        return EGL_FALSE;
    ++displayReferences[display];
    return EGL_TRUE;
}

EGLBoolean TerminateGuestDisplay(EGLDisplay display)
{
    static const auto terminate = reinterpret_cast<decltype(&eglTerminate)>(WindowsAngleExport(true, "eglTerminate"));
    const std::lock_guard lock(displayReferencesMutex);
    const auto found = displayReferences.find(display);
    if (found != displayReferences.end() && found->second > 1) {
        --found->second;
        return EGL_TRUE;
    }
    if (!terminate || !terminate(display))
        return EGL_FALSE;
    if (found != displayReferences.end())
        displayReferences.erase(found);
    return EGL_TRUE;
}

uintptr_t AngleEglProc(const char* name)
{
    if (const auto address = WindowsAngleExport(true, name))
        return address;
    static const auto lookup =
        reinterpret_cast<decltype(&eglGetProcAddress)>(WindowsAngleExport(true, "eglGetProcAddress"));
    return lookup ? reinterpret_cast<uintptr_t>(lookup(name)) : 0;
}

EGLint EglGetError()
{
    static const auto get = reinterpret_cast<decltype(&eglGetError)>(WindowsAngleExport(true, "eglGetError"));
    const auto hostError = get ? get() : EGL_SUCCESS;
    const auto bridgeError = std::exchange(eglBridgeError, EGL_SUCCESS);
    return bridgeError != EGL_SUCCESS ? bridgeError : hostError;
}

void EglBeginFrame(EGLDisplay display, EGLSurface surface)
{
    static const auto query = reinterpret_cast<decltype(&eglQuerySurface)>(WindowsAngleExport(true, "eglQuerySurface"));
    EGLint width;
    if (query)
        query(display, surface, EGL_WIDTH, &width);
    else
        eglBridgeError = EGL_NOT_INITIALIZED;
}

EGLSyncKHR EglCreateSync(EGLDisplay display, EGLenum type, const EGLint* attributes)
{
    static const auto create = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(AngleEglProc("eglCreateSyncKHR"));
    // Stone does not transfer Android sync_file descriptors to Windows EGL.
    if (!create || type == EGL_SYNC_NATIVE_FENCE_ANDROID) {
        eglBridgeError = create ? EGL_BAD_ATTRIBUTE : EGL_NOT_INITIALIZED;
        return EGL_NO_SYNC_KHR;
    }
    return create(display, type, attributes);
}

EGLBoolean EglSetDamage(EGLDisplay display, EGLSurface surface, EGLint* rectangles, EGLint count)
{
    static const auto damage = reinterpret_cast<PFNEGLSETDAMAGEREGIONKHRPROC>(AngleEglProc("eglSetDamageRegionKHR"));
    if (damage)
        return damage(display, surface, rectangles, count);
    eglBridgeError = EGL_BAD_MATCH;
    return EGL_FALSE;
}

EGLSurface EglWindowSurface(EGLDisplay display, EGLConfig config, NativeWindow* window, const EGLint*)
{
    static const auto create =
        reinterpret_cast<decltype(&eglCreatePbufferSurface)>(WindowsAngleExport(true, "eglCreatePbufferSurface"));
    if (!create || !window)
        return EGL_NO_SURFACE;
    if (window->imageQueue && WindowFormat(window) != Rgba8888 && WindowFormat(window) != 2)
        return EGL_NO_SURFACE;
    const EGLint width = WindowWidth(window);
    const EGLint height = WindowHeight(window);
    if (width <= 0 || height <= 0)
        return EGL_NO_SURFACE;
    const EGLint attributes[]{EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    const auto surface = create(display, config, attributes);
    if (surface != EGL_NO_SURFACE) {
        try {
            std::shared_ptr<ScreenPacing> pacing;
            if (window->imageQueue && IsAndroidSoftwareSurfaceQueue(*window->imageQueue)) {
                pacing = std::make_shared<ScreenPacing>();
                const auto get =
                    reinterpret_cast<decltype(&eglGetConfigAttrib)>(WindowsAngleExport(true, "eglGetConfigAttrib"));
                if (get) {
                    get(display, config, EGL_MIN_SWAP_INTERVAL, &pacing->minimum);
                    get(display, config, EGL_MAX_SWAP_INTERVAL, &pacing->maximum);
                }
                pacing->interval = std::clamp(1, pacing->minimum, pacing->maximum);
            }
            const std::lock_guard lock(eglWindowsMutex);
            if (eglWindows.emplace(surface, GuestEglWindow{window, display, -1, std::move(pacing)}).second)
                AcquireWindow(window);
        }
        catch (const std::bad_alloc&) {
            const auto destroy =
                reinterpret_cast<decltype(&eglDestroySurface)>(WindowsAngleExport(true, "eglDestroySurface"));
            if (destroy)
                destroy(display, surface);
            return EGL_NO_SURFACE;
        }
    }
    return surface;
}

EGLBoolean EglSwapInterval(EGLDisplay display, EGLint interval)
{
    static const auto set = reinterpret_cast<decltype(&eglSwapInterval)>(WindowsAngleExport(true, "eglSwapInterval"));
    static const auto current =
        reinterpret_cast<decltype(&eglGetCurrentSurface)>(WindowsAngleExport(true, "eglGetCurrentSurface"));
    if (!set || !set(display, interval))
        return EGL_FALSE;
    std::shared_ptr<ScreenPacing> pacing;
    {
        const std::lock_guard lock(eglWindowsMutex);
        const auto found = eglWindows.find(current ? current(EGL_DRAW) : EGL_NO_SURFACE);
        if (found != eglWindows.end() && found->second.display == display)
            pacing = found->second.pacing;
    }
    if (pacing) {
        const std::lock_guard lock(pacing->mutex);
        pacing->interval = std::clamp(interval, pacing->minimum, pacing->maximum);
        pacing->next = {};
    }
    return EGL_TRUE;
}

EGLBoolean EglDestroySurface(EGLDisplay display, EGLSurface surface)
{
    static const auto destroy =
        reinterpret_cast<decltype(&eglDestroySurface)>(WindowsAngleExport(true, "eglDestroySurface"));
    if (!destroy || !destroy(display, surface))
        return EGL_FALSE;
    NativeWindow* window = nullptr;
    {
        const std::lock_guard lock(eglWindowsMutex);
        auto entry = eglWindows.extract(surface);
        if (entry)
            window = entry.mapped().window;
    }
    if (window)
        ReleaseWindow(window);
    return EGL_TRUE;
}

EGLBoolean EglChooseConfig(EGLDisplay display, const EGLint* attributes, EGLConfig* configs, EGLint size, EGLint* count)
{
    static const auto choose =
        reinterpret_cast<decltype(&eglChooseConfig)>(WindowsAngleExport(true, "eglChooseConfig"));
    if (!choose)
        return EGL_FALSE;
    if (!attributes)
        return choose(display, nullptr, configs, size, count);
    std::array<EGLint, 129> adjusted{};
    size_t input = 0, output = 0;
    EGLint recordable = EGL_DONT_CARE;
    size_t surfaceType = adjusted.size();
    for (; input + 2 < adjusted.size() && attributes[input] != EGL_NONE; input += 2) {
        const auto key = attributes[input];
        const auto value = attributes[input + 1];
        if (key == EGL_RECORDABLE_ANDROID) {
            if (value != EGL_TRUE && value != EGL_FALSE && value != EGL_DONT_CARE)
                return EGL_FALSE;
            recordable = value;
            continue;
        }
        adjusted[output++] = key;
        if (key == EGL_SURFACE_TYPE)
            surfaceType = output;
        adjusted[output++] = key == EGL_SURFACE_TYPE && value != EGL_DONT_CARE && (value & EGL_WINDOW_BIT)
                                 ? (value & ~EGL_WINDOW_BIT) | EGL_PBUFFER_BIT
                                 : value;
    }
    if (attributes[input] != EGL_NONE)
        return EGL_FALSE;
    if (recordable == EGL_TRUE) {
        if (surfaceType == adjusted.size()) {
            adjusted[output++] = EGL_SURFACE_TYPE;
            adjusted[output++] = EGL_PBUFFER_BIT;
        }
        else {
            auto& type = adjusted[surfaceType];
            type = type == EGL_DONT_CARE ? EGL_PBUFFER_BIT : (type | EGL_PBUFFER_BIT);
        }
    }
    adjusted[output] = EGL_NONE;
    if (recordable != EGL_FALSE)
        return choose(display, adjusted.data(), configs, size, count);
    if (!count || (configs && size < 0))
        return EGL_FALSE;
    EGLint matches = 0;
    if (!choose(display, adjusted.data(), nullptr, 0, &matches))
        return EGL_FALSE;
    if (!matches) {
        *count = 0;
        return EGL_TRUE;
    }
    static const auto get =
        reinterpret_cast<decltype(&eglGetConfigAttrib)>(WindowsAngleExport(true, "eglGetConfigAttrib"));
    if (!get)
        return EGL_FALSE;
    try {
        std::vector<EGLConfig> candidates(matches);
        if (!choose(display, adjusted.data(), candidates.data(), matches, &matches))
            return EGL_FALSE;
        *count = 0;
        for (EGLint i = 0; i < matches && (!configs || *count < size); ++i) {
            EGLint type = 0;
            if (!get(display, candidates[i], EGL_SURFACE_TYPE, &type))
                return EGL_FALSE;
            if (type & EGL_PBUFFER_BIT)
                continue;
            if (configs)
                configs[*count] = candidates[i];
            ++*count;
        }
        return EGL_TRUE;
    }
    catch (const std::bad_alloc&) {
        return EGL_FALSE;
    }
}

EGLBoolean EglGetConfigAttribute(EGLDisplay display, EGLConfig config, EGLint attribute, EGLint* value)
{
    static const auto get =
        reinterpret_cast<decltype(&eglGetConfigAttrib)>(WindowsAngleExport(true, "eglGetConfigAttrib"));
    if (!get)
        return EGL_FALSE;
    if (attribute != EGL_RECORDABLE_ANDROID)
        return get(display, config, attribute, value);
    EGLint surface = 0;
    if (!value || !get(display, config, EGL_SURFACE_TYPE, &surface))
        return EGL_FALSE;
    *value = (surface & EGL_PBUFFER_BIT) ? EGL_TRUE : EGL_FALSE;
    return EGL_TRUE;
}

EGLBoolean EglSetPresentationTime(EGLDisplay display, EGLSurface surface, EGLnsecsANDROID timestamp)
{
    const std::lock_guard lock(eglWindowsMutex);
    const auto window = eglWindows.find(surface);
    if (window == eglWindows.end() || window->second.display != display || timestamp < 0)
        return EGL_FALSE;
    window->second.timestamp = timestamp;
    return EGL_TRUE;
}

const char* EglQueryString(EGLDisplay display, EGLint name)
{
    static const auto query = reinterpret_cast<decltype(&eglQueryString)>(WindowsAngleExport(true, "eglQueryString"));
    const char* value = query ? query(display, name) : nullptr;
    if (!value || name != EGL_EXTENSIONS)
        return value;
    static std::mutex mutex;
    static std::unordered_map<EGLDisplay, std::string> extensions;
    try {
        const std::lock_guard lock(mutex);
        if (const auto found = extensions.find(display); found != extensions.end())
            return found->second.c_str();
        std::string result;
        const auto append = [&](std::string_view extension) {
            if (!result.empty())
                result += ' ';
            result += extension;
        };
        std::string_view remaining(value);
        while (!remaining.empty()) {
            const auto delimiter = remaining.find(' ');
            const auto extension = remaining.substr(0, delimiter);
            // The ANGLE diagnostic callback requires a host-to-guest ABI bridge.
            if (!extension.empty() && extension != "EGL_KHR_debug")
                append(extension);
            if (delimiter == remaining.npos)
                break;
            remaining.remove_prefix(delimiter + 1);
        }
        if (display != EGL_NO_DISPLAY) {
            for (const std::string_view extension : {"EGL_ANDROID_recordable", "EGL_ANDROID_presentation_time"}) {
                if (result.find(extension) == result.npos)
                    append(extension);
            }
        }
        return extensions.emplace(display, std::move(result)).first->second.c_str();
    }
    catch (const std::bad_alloc&) {
        eglBridgeError = EGL_BAD_ALLOC;
        return nullptr;
    }
}

EGLBoolean EglSwap(EGLDisplay display, EGLSurface surface)
{
    static const auto swap = reinterpret_cast<decltype(&eglSwapBuffers)>(WindowsAngleExport(true, "eglSwapBuffers"));
    static const auto query = reinterpret_cast<decltype(&eglQuerySurface)>(WindowsAngleExport(true, "eglQuerySurface"));
    NativeWindow* window = nullptr;
    EGLnsecsANDROID timestamp = -1;
    std::shared_ptr<ScreenPacing> pacing;
    {
        const std::lock_guard lock(eglWindowsMutex);
        const auto it = eglWindows.find(surface);
        if (it != eglWindows.end() && it->second.display == display) {
            window = it->second.window;
            timestamp = std::exchange(it->second.timestamp, -1);
            pacing = it->second.pacing;
            AcquireWindow(window);
        }
    }
    if (!window)
        return swap ? swap(display, surface) : EGL_FALSE;
    const auto release = std::unique_ptr<NativeWindow, decltype(&ReleaseWindow)>(window, ReleaseWindow);
    EGLint width = 0, height = 0;
    if (!query || !query(display, surface, EGL_WIDTH, &width) || !query(display, surface, EGL_HEIGHT, &height))
        return EGL_FALSE;
    if (window->imageQueue) {
        static const auto current =
            reinterpret_cast<decltype(&eglGetCurrentSurface)>(WindowsAngleExport(true, "eglGetCurrentSurface"));
        if (!current || current(EGL_DRAW) != surface || current(EGL_READ) != surface)
            return EGL_FALSE;
    }
    try {
        if (pacing)
            pacing->Wait();
        thread_local std::vector<jint> pixels;
        if (!CaptureWindowsGlFrame(pixels, width, height))
            return EGL_FALSE;
        if (window->imageQueue) {
            AndroidNativeWindowBuffer buffer{};
            if (LockAndroidImageQueue(*window->imageQueue, buffer, nullptr) != 0)
                return EGL_FALSE;
            for (int y = 0; y < height; ++y) {
                auto* row = static_cast<uint32_t*>(buffer.bits) + size_t(y) * buffer.stride;
                for (int x = 0; x < width; ++x) {
                    const auto argb = static_cast<uint32_t>(pixels[size_t(y) * width + x]);
                    row[x] = (argb & 0xff00ff00u) | ((argb & 0xffu) << 16) | ((argb >> 16) & 0xffu);
                }
            }
            return PostAndroidImageQueue(*window->imageQueue, timestamp) == 0 ? EGL_TRUE : EGL_FALSE;
        }
        PresentWindowsNativeFrame(pixels, width, height);
        return EGL_TRUE;
    }
    catch (const std::bad_alloc&) {
        return EGL_FALSE;
    }
}

EGLBoolean EglSwapWithDamage(EGLDisplay display, EGLSurface surface, const EGLint* rectangles, EGLint count)
{
    if (count < 0 || (count && !rectangles)) {
        eglBridgeError = EGL_BAD_PARAMETER;
        return EGL_FALSE;
    }
    // Windows presentation copies the complete rendered frame. The damage
    // rectangle list is an optimization hint for the compositor.
    return EglSwap(display, surface);
}

EGLDisplay EglGuestDisplay(EGLNativeDisplayType nativeDisplay)
{
    if (nativeDisplay == EGL_DEFAULT_DISPLAY)
        return WindowsAngleGuestDisplay();
    const auto getDisplay = reinterpret_cast<decltype(&eglGetDisplay)>(WindowsAngleExport(true, "eglGetDisplay"));
    return getDisplay ? getDisplay(nativeDisplay) : EGL_NO_DISPLAY;
}
}

uintptr_t Libegl(std::string_view name, bool hostAbi)
{
    const auto resolve = [name, hostAbi](auto function) {
        return hostAbi ? reinterpret_cast<uintptr_t>(function) : Import(name, function);
    };
    if (name == "eglGetProcAddress")
        return Import(
            name, +[](const char* symbol) {
                const std::lock_guard lock(AndroidLoaderMutex());
                return std::string_view(symbol).starts_with("egl") ? Libegl(symbol) : Libgles(symbol);
            });
    if (name == "eglCreateWindowSurface")
        return resolve(&EglWindowSurface);
    if (name == "eglCreatePlatformWindowSurface")
        return resolve(+[](EGLDisplay display, EGLConfig config, NativeWindow* window, const EGLAttrib*) {
            return EglWindowSurface(display, config, window, nullptr);
        });
    if (name == "eglDestroySurface")
        return resolve(&EglDestroySurface);
    if (name == "eglChooseConfig")
        return resolve(&EglChooseConfig);
    if (name == "eglGetConfigAttrib")
        return resolve(&EglGetConfigAttribute);
    if (name == "eglGetError")
        return resolve(&EglGetError);
    if (name == "eglPresentationTimeANDROID")
        return resolve(&EglSetPresentationTime);
    if (name == "eglQueryString")
        return resolve(&EglQueryString);
    if (name == "eglGetDisplay")
        return resolve(&EglGuestDisplay);
    if (name == "eglInitialize")
        return resolve(&InitializeGuestDisplay);
    if (name == "eglTerminate")
        return resolve(&TerminateGuestDisplay);
    if (name == "_ZN7android18egl_get_init_countEPv")
        return resolve(+[](EGLDisplay display) -> int {
            const std::lock_guard lock(displayReferencesMutex);
            const auto found = displayReferences.find(display);
            return found == displayReferences.end() ? 0 : static_cast<int>(found->second);
        });
    if (name == "eglSwapBuffers")
        return resolve(&EglSwap);
    if (name == "eglSwapInterval")
        return resolve(&EglSwapInterval);
    if (name == "eglSwapBuffersWithDamageKHR")
        return resolve(&EglSwapWithDamage);
    if (name == "eglSetDamageRegionKHR")
        return resolve(&EglSetDamage);
    if (name == "eglCreateSyncKHR")
        return resolve(&EglCreateSync);
    if (name == "eglDupNativeFenceFDANDROID")
        return resolve(+[](EGLDisplay, EGLSyncKHR) -> EGLint {
            eglBridgeError = EGL_BAD_PARAMETER;
            return EGL_NO_NATIVE_FENCE_FD_ANDROID;
        });
    if (name == "eglGetNativeClientBufferANDROID")
        return resolve(+[](const void*) -> EGLClientBuffer {
            eglBridgeError = EGL_BAD_PARAMETER; // Stone AHardwareBuffers do not have a host EGL client buffer.
            return nullptr;
        });
    if (name == "_Z13eglBeginFramePvS_")
        return resolve(&EglBeginFrame);
    if (name == "_ZN7android22egl_set_cache_filenameEPKc")
        return resolve(+[](const char* filename) {
            if (filename && *filename) {
                static std::once_flag reported;
                std::call_once(reported, [] {
                    std::fputs("[android-egl] persistent shader cache is unavailable\n", stderr);
                });
            }
        });
    constexpr std::string_view regular[]{"eglBindTexImage",       "eglClientWaitSync",
                                         "eglCreateImage",        "eglCreatePbufferFromClientBuffer",
                                         "eglCreateSync",         "eglDestroyImage",
                                         "eglDestroySync",        "eglGetConfigs",
                                         "eglGetSyncAttrib",      "eglQueryAPI",
                                         "eglReleaseTexImage",    "eglWaitClient",
                                         "eglWaitSync",           "eglBindAPI",
                                         "eglClientWaitSyncKHR",  "eglCreateContext",
                                         "eglCreateImageKHR",     "eglCreatePbufferSurface",
                                         "eglDestroyContext",     "eglDestroyImageKHR",
                                         "eglDestroySyncKHR",     "eglGetCurrentContext",
                                         "eglGetCurrentDisplay",  "eglGetCurrentSurface",
                                         "eglGetPlatformDisplay", "eglGetPlatformDisplayEXT",
                                         "eglGetSyncAttribKHR",   "eglMakeCurrent",
                                         "eglQueryContext",       "eglQuerySurface",
                                         "eglReleaseThread",      "eglSetValidationEnabledANGLE",
                                         "eglSurfaceAttrib",      "eglSwapInterval",
                                         "eglTerminate",          "eglWaitGL",
                                         "eglWaitNative",         "eglWaitSyncKHR"};
    if (std::ranges::find(regular, name) == std::end(regular))
        return 0;
    const std::string symbol(name);
    const auto address = AngleEglProc(symbol.c_str());
    return address && !hostAbi ? Import(name, address) : address;
}
}
