#include "../../support/test-suite.h"

#include "abi/AndroidLibraryImports.h"
#include "abi/TypedAndroidImport.h"
#include "hle/system/stub-libvulkan.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

namespace stone::windows::test {
void TestGraphics(Suite& suite)
{
    suite.Run("typed-import-independent-register-banks-and-stack-spills", [] {
        const auto host = +[](int a, double b, int c, float d, int e, double f, int g, float h, int i, double j, int k,
                              float l, int m, double n, int o, float p, double q, float r, const int* end) -> double {
            return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j + 11 * k + 12 * l +
                   13 * m + 14 * n + 15 * o + 16 * p + 17 * q + 18 * r + 19 * (*end);
        };
        const auto address =
            ImportAndroidScalars<decltype(host)>("Stone typed import spill test", reinterpret_cast<uintptr_t>(host));
        const auto call = Guest<decltype(host)>(address, "(IDIFIDIFIDIFIDIFDFL)D");
        const int end = 19;
        STONE_CHECK(call(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, &end) == 2470.0);
        const auto negative = +[](int value) -> int {
            return -value;
        };
        const auto negate = Guest<int (*)(int)>(ImportAndroidScalars<decltype(negative)>(
                                                    "Stone typed signed return", reinterpret_cast<uintptr_t>(negative)),
                                                "(I)I");
        STONE_CHECK(negate(42) == -42);
    });
    suite.Run("libegl-extension-filtering-and-one-shot-error", [] {
        const auto query = Guest<const char* (*)(EGLDisplay, EGLint)>(Libegl("eglQueryString"), "(LI)L");
        const auto extensions = query(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        STONE_CHECK(extensions);
        const std::string_view advertised(extensions);
        STONE_CHECK(advertised.find("EGL_KHR_debug") == advertised.npos);
        STONE_CHECK(advertised.starts_with("EGL_EXT_buffer_age EGL_KHR_surfaceless_context"));
        const auto fence = Guest<EGLint (*)(EGLDisplay, EGLSyncKHR)>(Libegl("eglDupNativeFenceFDANDROID"), "(LL)I");
        const auto error = Guest<EGLint (*)()>(Libegl("eglGetError"), "()I");
        STONE_CHECK(fence(EGL_NO_DISPLAY, EGL_NO_SYNC_KHR) == EGL_NO_NATIVE_FENCE_FD_ANDROID);
        STONE_CHECK(error() == EGL_BAD_PARAMETER);
        STONE_CHECK(error() == EGL_SUCCESS);
    });
    suite.Run("libegl-angle-procedure-lookup-and-validation-state", [] {
        const auto lookup = Guest<uintptr_t (*)(const char*)>(Libegl("eglGetProcAddress"), "(L)J");
        STONE_CHECK(lookup("eglStoneUnknownCommand") == 0);
        const auto address = lookup("eglSetValidationEnabledANGLE");
        STONE_CHECK(address);
        const auto set = Guest<void (*)(EGLBoolean)>(address, "(I)V");
        const auto state = reinterpret_cast<unsigned (*)()>(
            GetProcAddress(GetModuleHandleW(L"libEGL.dll"), "StoneTestEglValidationEnabled"));
        STONE_CHECK(state && state() == EGL_TRUE);
        set(EGL_FALSE);
        STONE_CHECK(state() == EGL_FALSE);
        set(EGL_TRUE);
        STONE_CHECK(state() == EGL_TRUE);
    });
    suite.Run("libglesv2-consistent-string-index-and-count-queries", [] {
        const auto get = Guest<const char* (*)(GLenum)>(Libgles("glGetString"), "(I)L");
        const auto indexed = Guest<const char* (*)(GLenum, GLuint)>(Libgles("glGetStringi"), "(II)L");
        const auto integer = Guest<void (*)(GLenum, GLint*)>(Libgles("glGetIntegerv"), "(IL)V");
        const auto raw = Host<const char* (*)(GLenum)>(Libgles("glGetString", true));
        STONE_CHECK(std::string_view(raw(GL_EXTENSIONS)).find("GL_KHR_debug") != std::string_view::npos);
        const auto extensions = get(GL_EXTENSIONS);
        STONE_CHECK(extensions &&
                    std::string_view(extensions) == "GL_EXT_texture_format_BGRA8888 GL_EXT_texture_storage");
        const auto first = indexed(GL_EXTENSIONS, 0);
        const auto second = indexed(GL_EXTENSIONS, 1);
        STONE_CHECK(first && std::string_view(first) == "GL_EXT_texture_format_BGRA8888");
        STONE_CHECK(second && std::string_view(second) == "GL_EXT_texture_storage");
        GLint count = -1;
        integer(GL_NUM_EXTENSIONS, &count);
        STONE_CHECK(count == 2);
    });
    suite.Run("libglesv2-khr-graphics-reset-status", [] {
        const auto reset = Guest<GLenum (*)()>(Libgles("glGetGraphicsResetStatusKHR"), "()I");
        STONE_CHECK(reset() == 0x8253);
    });
    suite.Run("libvulkan-loader-capability-and-unknown-symbol", [] {
        STONE_CHECK(Libvulkan("vkStoneUnknownCommand") == 0);
        STONE_CHECK(static_cast<bool>(Libvulkan("vkCreateInstance")) == WindowsArm64VulkanAvailable());
    });
}
}
