extern "C" __declspec(dllexport) const char* eglQueryString(const void*, int name)
{
    switch (name) {
    case 0x3055:
        return "EGL_EXT_buffer_age EGL_KHR_debug EGL_KHR_surfaceless_context";
    case 0x3053:
        return "Stone CTest ANGLE fixture";
    default:
        return nullptr;
    }
}

extern "C" __declspec(dllexport) unsigned int eglGetError()
{
    return 0x3000;
}

extern "C" __declspec(dllexport) void* eglGetProcAddress(const char*)
{
    return nullptr;
}
