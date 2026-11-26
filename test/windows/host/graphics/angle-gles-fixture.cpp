#include <cstddef>

namespace {
constexpr const char* extensions[]{"GL_EXT_texture_format_BGRA8888", "GL_KHR_debug", "GL_EXT_texture_storage"};
}

extern "C" __declspec(dllexport) const char* glGetString(unsigned int name)
{
    return name == 0x1f03   ? "GL_EXT_texture_format_BGRA8888 GL_KHR_debug GL_EXT_texture_storage"
           : name == 0x1f00 ? "Stone CTest ANGLE fixture"
                            : nullptr;
}

extern "C" __declspec(dllexport) const char* glGetStringi(unsigned int name, unsigned int index)
{
    return name == 0x1f03 && index < 3 ? extensions[index] : nullptr;
}

extern "C" __declspec(dllexport) void glGetIntegerv(unsigned int name, int* value)
{
    if (name == 0x821d && value)
        *value = 3;
}
