#include "AngleState.h"
#include "GlSurface.h"

#include <windows.h>
#include <d3d11_4.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace stone::windows::angle_backend {
thread_local Angle angle;
thread_local bool current = false;
thread_local bool withheld = false;
}

using namespace stone::windows::angle_backend;

namespace {
namespace fs = std::filesystem;

fs::path AngleDirectory()
{
    // Service and package-management launches need not use the installation
    // directory as their working directory.
    std::wstring executable(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!size || size >= executable.size())
        return {};
    executable.resize(size);
    const auto directory = fs::path(executable).parent_path();
    return directory.is_absolute() ? directory : fs::path{};
}

bool LoadAngle(bool webView)
{
    if (!WindowsAngleExport(true, "eglGetDisplay"))
        return false;
#define LOAD(module, field, name)                                                                                      \
    angle.field =                                                                                                      \
        reinterpret_cast<decltype(angle.field)>(WindowsAngleExport(std::string_view(#module) == "egl", name));         \
    if (!angle.field)                                                                                                  \
    return false
    LOAD(egl, GetDisplay, "eglGetDisplay");
    LOAD(egl, Initialize, "eglInitialize");
    LOAD(egl, BindAPI, "eglBindAPI");
    LOAD(egl, ChooseConfig, "eglChooseConfig");
    LOAD(egl, CreateContext, "eglCreateContext");
    LOAD(egl, CreatePbufferSurface, "eglCreatePbufferSurface");
    LOAD(egl, MakeCurrent, "eglMakeCurrent");
    LOAD(egl, DestroySurface, "eglDestroySurface");
    LOAD(egl, DestroyContext, "eglDestroyContext");
    LOAD(egl, Terminate, "eglTerminate");
    LOAD(egl, GetError, "eglGetError");
    LOAD(gles, glViewport, "glViewport");
    LOAD(gles, glClear, "glClear");
    LOAD(gles, glClearColor, "glClearColor");
    LOAD(gles, glEnable, "glEnable");
    LOAD(gles, glDisable, "glDisable");
    LOAD(gles, glScissor, "glScissor");
    LOAD(gles, glGetString, "glGetString");
    LOAD(gles, glGetError, "glGetError");
#undef LOAD
    angle.GetPlatformDisplay =
        reinterpret_cast<decltype(angle.GetPlatformDisplay)>(WindowsAngleExport(true, "eglGetPlatformDisplay"));
    if (webView) {
        // Chromium can share its resource contexts with the current render context.
        angle.display = WindowsAngleGuestDisplay();
    }
    else if (angle.GetPlatformDisplay) {
        // Guest eglTerminate must not invalidate the Java renderer's display.
        const EGLAttrib attributes[]{EGL_PLATFORM_ANGLE_DISPLAY_KEY_ANGLE, 1, EGL_NONE};
        angle.display = angle.GetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attributes);
        angle.ownsDisplay = angle.display != EGL_NO_DISPLAY;
    }
    if (angle.display == EGL_NO_DISPLAY) {
        angle.GetError();
        angle.display = angle.GetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (angle.display == EGL_NO_DISPLAY || !InitializeWindowsAngleDisplay(angle.display, nullptr, nullptr) ||
        !angle.BindAPI(EGL_OPENGL_ES_API))
        return false;
    const EGLint attributes[]{EGL_SURFACE_TYPE,
                              EGL_PBUFFER_BIT,
                              EGL_RENDERABLE_TYPE,
                              webView ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT,
                              EGL_RED_SIZE,
                              8,
                              EGL_GREEN_SIZE,
                              8,
                              EGL_BLUE_SIZE,
                              8,
                              EGL_ALPHA_SIZE,
                              8,
                              EGL_DEPTH_SIZE,
                              16,
                              EGL_STENCIL_SIZE,
                              8,
                              EGL_NONE};
    EGLint count = 0;
    if (!angle.ChooseConfig(angle.display, attributes, &angle.config, 1, &count) || !count)
        return false;
    const EGLint contextAttributes[]{EGL_CONTEXT_CLIENT_VERSION, webView ? 3 : 2, EGL_NONE};
    angle.context = angle.CreateContext(angle.display, angle.config, EGL_NO_CONTEXT, contextAttributes);
    return angle.context != EGL_NO_CONTEXT;
}

}

uintptr_t WindowsAngleExport(bool egl, const char* name)
{
    struct Modules {
        HMODULE egl = nullptr;
        HMODULE gles = nullptr;
    };

    // Guest thunks cache export addresses for the lifetime of the process.
    static const auto modules = [] {
        Modules result;
        const auto dir = AngleDirectory();
        if (!dir.empty()) {
            constexpr DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
            result.gles = LoadLibraryExW((dir / L"libGLESv2.dll").c_str(), nullptr, flags);
            result.egl = LoadLibraryExW((dir / L"libEGL.dll").c_str(), nullptr, flags);
        }
        if (!result.egl || !result.gles) {
            std::fprintf(stderr, "[win32] OpenGL ES needs ANGLE libEGL.dll and libGLESv2.dll beside the executable\n");
        }
        return result;
    }();
    const auto module = egl ? modules.egl : modules.gles;
    return module ? reinterpret_cast<uintptr_t>(GetProcAddress(module, name)) : 0;
}

void* WindowsAngleGuestDisplay()
{
    static const auto getPlatform =
        reinterpret_cast<decltype(&eglGetPlatformDisplay)>(WindowsAngleExport(true, "eglGetPlatformDisplay"));
    static const auto getDisplay =
        reinterpret_cast<decltype(&eglGetDisplay)>(WindowsAngleExport(true, "eglGetDisplay"));
    static const auto display = [&]() -> EGLDisplay {
        if (getPlatform) {
            // A returned display handle does not prove the backend can start.
            // Prefer Windows D3D11, then try the other compiled ANGLE backends.
            for (const EGLAttrib backend : {EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE, EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,
                                            EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE}) {
                const EGLAttrib attributes[]{EGL_PLATFORM_ANGLE_TYPE_ANGLE, backend, EGL_NONE};
                const auto candidate = getPlatform(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attributes);
                if (candidate != EGL_NO_DISPLAY && InitializeWindowsAngleDisplay(candidate, nullptr, nullptr))
                    return candidate;
            }
            const EGLAttrib software[]{EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
                                       EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE,
                                       EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE, EGL_NONE};
            const auto candidate = getPlatform(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, software);
            if (candidate != EGL_NO_DISPLAY && InitializeWindowsAngleDisplay(candidate, nullptr, nullptr))
                return candidate;
        }
        return getDisplay ? getDisplay(EGL_DEFAULT_DISPLAY) : EGL_NO_DISPLAY;
    }();
    return display;
}

unsigned int InitializeWindowsAngleDisplay(void* display, int* major, int* minor)
{
    static std::mutex initializeMutex;
    const std::lock_guard lock(initializeMutex);
    const auto initialize = reinterpret_cast<decltype(&eglInitialize)>(WindowsAngleExport(true, "eglInitialize"));
    if (!initialize || !initialize(display, major, minor))
        return EGL_FALSE;
    const auto lookup = reinterpret_cast<decltype(&eglGetProcAddress)>(WindowsAngleExport(true, "eglGetProcAddress"));
    if (!lookup)
        return EGL_TRUE;
    const auto displayAttribute = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(lookup("eglQueryDisplayAttribEXT"));
    const auto deviceAttribute = reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXTPROC>(lookup("eglQueryDeviceAttribEXT"));
    EGLAttrib handle = 0;
    EGLAttrib device = 0;
    if (displayAttribute && deviceAttribute && displayAttribute(display, EGL_DEVICE_EXT, &handle) &&
        deviceAttribute(reinterpret_cast<EGLDeviceEXT>(handle), EGL_D3D11_DEVICE_ANGLE, &device) && device) {
        ID3D11Multithread* multithread = nullptr;
        auto* d3d = reinterpret_cast<ID3D11Device*>(device);
        if (SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&multithread)))) {
            if (!multithread->GetMultithreadProtected())
                multithread->SetMultithreadProtected(TRUE);
            multithread->Release();
        }
    }
    const auto error = reinterpret_cast<decltype(&eglGetError)>(WindowsAngleExport(true, "eglGetError"));
    if (error)
        error();
    return EGL_TRUE;
}

namespace {
bool BeginGlFrame(int width, int height, bool webView)
{
    if (!angle.attempted) {
        angle.attempted = true;
        if (!LoadAngle(webView)) {
            std::fprintf(stderr, "[win32] could not initialize ANGLE (EGL error 0x%X)\n",
                         angle.GetError ? angle.GetError() : 0);
            CloseWindowsGl();
        }
    }
    if (angle.context == EGL_NO_CONTEXT)
        return false;
    if (angle.surface == EGL_NO_SURFACE || width != angle.width || height != angle.height) {
        const EGLint attributes[]{EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
        auto surface = angle.CreatePbufferSurface(angle.display, angle.config, attributes);
        if (surface == EGL_NO_SURFACE || !angle.MakeCurrent(angle.display, surface, surface, angle.context)) {
            std::fprintf(stderr, "[win32] could not create the %dx%d EGL surface (0x%X)\n", width, height,
                         angle.GetError());
            if (surface != EGL_NO_SURFACE)
                angle.DestroySurface(angle.display, surface);
            if (!webView || angle.surface == EGL_NO_SURFACE)
                CloseWindowsGl();
            return false;
        }
        if (angle.surface != EGL_NO_SURFACE)
            angle.DestroySurface(angle.display, angle.surface);
        angle.surface = surface;
        angle.width = width;
        angle.height = height;
        const auto renderer = angle.glGetString(GL_RENDERER);
        std::fprintf(stderr, "[win32] ANGLE surface %dx%d (%s)\n", width, height,
                     renderer ? reinterpret_cast<const char*>(renderer) : "unknown renderer");
    }
    else if (!angle.MakeCurrent(angle.display, angle.surface, angle.surface, angle.context)) {
        std::fprintf(stderr, "[win32] could not make the EGL context current (0x%X)\n", angle.GetError());
        if (!webView)
            CloseWindowsGl();
        return false;
    }
    current = true;
    withheld = false;
    return true;
}
}

bool BeginWindowsGlFrame(int width, int height)
{
    return BeginGlFrame(width, height, false);
}

bool BeginWindowsWebViewGlFrame(int width, int height)
{
    return BeginGlFrame(width, height, true);
}

bool CaptureWindowsGlFrame(std::vector<jint>& pixels, int width, int height)
{
    static const auto get = reinterpret_cast<decltype(&glGetIntegerv)>(WindowsAngleExport(false, "glGetIntegerv"));
    static const auto getString = reinterpret_cast<decltype(&glGetString)>(WindowsAngleExport(false, "glGetString"));
    static const auto bindBuffer = reinterpret_cast<decltype(&glBindBuffer)>(WindowsAngleExport(false, "glBindBuffer"));
    static const auto store = reinterpret_cast<decltype(&glPixelStorei)>(WindowsAngleExport(false, "glPixelStorei"));
    static const auto read = reinterpret_cast<decltype(&glReadPixels)>(WindowsAngleExport(false, "glReadPixels"));
    if (!get || !store || !read || width <= 0 || height <= 0 || width > 8192 || height > 8192)
        return false;
    GLint pack = 4;
    GLint framebuffer = 0;
    get(GL_PACK_ALIGNMENT, &pack);
    get(GL_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer)
        return false;
    const auto version = getString ? reinterpret_cast<const char*>(getString(GL_VERSION)) : nullptr;
    const bool es3 = version && std::string_view(version).starts_with("OpenGL ES 3");
    if (es3) {
        get(GL_READ_FRAMEBUFFER_BINDING, &framebuffer);
        if (framebuffer || !bindBuffer)
            return false;
    }
    pixels.resize(static_cast<size_t>(width) * height);
    constexpr std::array<GLenum, 3> packKeys{GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS};
    std::array<GLint, packKeys.size()> packValues{};
    GLint packBuffer = 0;
    if (es3) {
        get(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
        if (packBuffer)
            bindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        for (size_t i = 0; i < packKeys.size(); ++i) {
            get(packKeys[i], &packValues[i]);
            if (packValues[i])
                store(packKeys[i], 0);
        }
    }
    store(GL_PACK_ALIGNMENT, 4);
    read(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    store(GL_PACK_ALIGNMENT, pack);
    if (es3) {
        for (size_t i = 0; i < packKeys.size(); ++i)
            if (packValues[i])
                store(packKeys[i], packValues[i]);
        if (packBuffer)
            bindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
    }
    // Convert bottom-up RGBA into top-down ARGB for the Win32 compositor.
    const auto argb = [](jint pixel) {
        const auto rgba = static_cast<unsigned>(pixel);
        return static_cast<jint>((rgba & 0xff00ff00u) | ((rgba & 0xffu) << 16) | ((rgba >> 16) & 0xffu));
    };
    for (int y = 0; y < (height + 1) / 2; ++y) {
        const size_t top = static_cast<size_t>(y) * width;
        const size_t bottom = static_cast<size_t>(height - y - 1) * width;
        for (int x = 0; x < width; ++x) {
            const auto pixel = pixels[top + x];
            pixels[top + x] = argb(pixels[bottom + x]);
            pixels[bottom + x] = argb(pixel);
        }
    }
    return true;
}

bool ReadWindowsGlFrame(std::vector<jint>& pixels, int width, int height)
{
    if (!current)
        return false;
    const bool captured = !withheld && CaptureWindowsGlFrame(pixels, width, height);
    angle.MakeCurrent(angle.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    current = false;
    return captured;
}

void CloseWindowsGl()
{
    current = false;
    if (angle.display != EGL_NO_DISPLAY) {
        angle.MakeCurrent(angle.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (angle.surface != EGL_NO_SURFACE)
            angle.DestroySurface(angle.display, angle.surface);
        if (angle.context != EGL_NO_CONTEXT)
            angle.DestroyContext(angle.display, angle.context);
        if (angle.ownsDisplay)
            angle.Terminate(angle.display);
    }
    angle = {};
    angle.attempted = true;
}
