#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>

// Private context state shared with the JNI entry adapter on the same thread.
namespace stone::windows::angle_backend {
struct Angle {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    int width = 0;
    int height = 0;
    bool attempted = false;
    bool ownsDisplay = false;

#define EGL_FUNCTION(name) decltype(&egl##name) name = nullptr
    EGL_FUNCTION(GetDisplay);
    EGL_FUNCTION(GetPlatformDisplay);
    EGL_FUNCTION(Initialize);
    EGL_FUNCTION(BindAPI);
    EGL_FUNCTION(ChooseConfig);
    EGL_FUNCTION(CreateContext);
    EGL_FUNCTION(CreatePbufferSurface);
    EGL_FUNCTION(MakeCurrent);
    EGL_FUNCTION(DestroySurface);
    EGL_FUNCTION(DestroyContext);
    EGL_FUNCTION(Terminate);
    EGL_FUNCTION(GetError);
#undef EGL_FUNCTION

#define GL_FUNCTION(name) decltype(&gl##name) gl##name = nullptr
    GL_FUNCTION(Viewport);
    GL_FUNCTION(Clear);
    GL_FUNCTION(ClearColor);
    GL_FUNCTION(Enable);
    GL_FUNCTION(Disable);
    GL_FUNCTION(Scissor);
    GL_FUNCTION(GetString);
    GL_FUNCTION(GetError);
#undef GL_FUNCTION
};

extern thread_local Angle angle;
extern thread_local bool current;
extern thread_local bool withheld;

}
