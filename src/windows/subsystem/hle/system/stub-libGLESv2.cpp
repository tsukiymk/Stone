#include "abi/AndroidLibraryImports.h"
#include "../../../host/graphics/GlSurface.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace stone::windows {
uintptr_t AndroidFrameworkGlImport(std::string_view name, uintptr_t address);

namespace {
constexpr std::string_view UnbridgedDebug = "GL_KHR_debug";

uintptr_t FindAngleGl(const char* name)
{
    if (const auto address = WindowsAngleExport(false, name))
        return address;
    static const auto lookup =
        reinterpret_cast<decltype(&eglGetProcAddress)>(WindowsAngleExport(true, "eglGetProcAddress"));
    if (lookup)
        if (const auto address = lookup(name))
            return reinterpret_cast<uintptr_t>(address);
    // GLES 1 fixed-point and promoted extension entry points have identical
    // prototypes to their core spellings. ANGLE exports several only as core.
    const std::string_view symbol(name);
    if (symbol.ends_with("OES")) {
        const std::string core(symbol.substr(0, symbol.size() - 3));
        if (const auto address = WindowsAngleExport(false, core.c_str()))
            return address;
        if (lookup)
            return reinterpret_cast<uintptr_t>(lookup(core.c_str()));
    }
    return 0;
}

template <typename Function> Function AngleGl(const char* name)
{
    return reinterpret_cast<Function>(FindAngleGl(name));
}

const GLubyte* GlString(GLenum name)
{
    static const auto query = AngleGl<decltype(&glGetString)>("glGetString");
    const auto value = query(name);
    if (!value || name != GL_EXTENSIONS)
        return value;
    const std::string_view list(reinterpret_cast<const char*>(value));
    if (list.find(UnbridgedDebug) == list.npos)
        return value;
    static std::mutex mutex;
    static std::unordered_map<std::string, std::string> filtered;
    try {
        const std::lock_guard lock(mutex);
        std::string key(list);
        if (const auto found = filtered.find(key); found != filtered.end())
            return reinterpret_cast<const GLubyte*>(found->second.c_str());
        std::string result;
        auto remaining = list;
        while (!remaining.empty()) {
            const auto end = remaining.find(' ');
            const auto extension = remaining.substr(0, end);
            if (!extension.empty() && extension != UnbridgedDebug) {
                if (!result.empty())
                    result += ' ';
                result += extension;
            }
            if (end == remaining.npos)
                break;
            remaining.remove_prefix(end + 1);
        }
        const auto stored = filtered.emplace(std::move(key), std::move(result));
        return reinterpret_cast<const GLubyte*>(stored.first->second.c_str());
    }
    catch (const std::bad_alloc&) {
        return nullptr;
    }
}

GLuint FindDebugExtension(GLuint count)
{
    static const auto query = AngleGl<decltype(&glGetStringi)>("glGetStringi");
    if (query) {
        for (GLuint i = 0; i < count; ++i) {
            const auto value = query(GL_EXTENSIONS, i);
            if (value && reinterpret_cast<const char*>(value) == UnbridgedDebug)
                return i;
        }
    }
    return count;
}

void GlInteger(GLenum name, GLint* value)
{
    static const auto query = AngleGl<decltype(&glGetIntegerv)>("glGetIntegerv");
    if (name != GL_NUM_EXTENSIONS || !value) {
        query(name, value);
        return;
    }
    GLint count = -1;
    query(name, &count);
    if (count < 0)
        return;
    const auto size = static_cast<GLuint>(count);
    *value = count - (FindDebugExtension(size) != size ? 1 : 0);
}

const GLubyte* GlIndexedString(GLenum name, GLuint index)
{
    static const auto query = AngleGl<decltype(&glGetStringi)>("glGetStringi");
    if (name != GL_EXTENSIONS)
        return query(name, index);
    static const auto get = AngleGl<decltype(&glGetIntegerv)>("glGetIntegerv");
    GLint count = 0;
    get(GL_NUM_EXTENSIONS, &count);
    if (count > 0 && index < static_cast<GLuint>(count) && FindDebugExtension(static_cast<GLuint>(count)) <= index)
        ++index;
    return query(name, index);
}
}

// Shared ANGLE resolver for libGLESv1_CM.so, libGLESv2.so and libGLESv3.so.
uintptr_t Libgles(std::string_view name, bool hostAbi)
{
    if (!name.starts_with("gl") || name.starts_with("glGetProc"))
        return 0;
    // AOSP's framework-only Bounds wrappers discard the final count argument.
    // Their remaining arguments are exactly the corresponding GLES prototype.
    if (name == "glColorPointerBounds" || name == "glNormalPointerBounds" || name == "glTexCoordPointerBounds" ||
        name == "glVertexPointerBounds" || name == "glPointSizePointerOESBounds" ||
        name == "glMatrixIndexPointerOESBounds" || name == "glWeightPointerOESBounds")
        name.remove_suffix(6);
    const std::string symbol(name);
    const auto address = FindAngleGl(symbol.c_str());
    if (!address)
        return 0;
    if (hostAbi)
        return address;
    // ANGLE's native driver callback crosses into guest code and needs a
    // separate ABI bridge. Keep both extension enumeration APIs consistent.
    if (!IsArm64TranslationEnabled()) {
        if (name == "glGetString")
            return Import(name, &GlString);
        if (name == "glGetStringi")
            return Import(name, &GlIndexedString);
        if (name == "glGetIntegerv")
            return Import(name, &GlInteger);
    }
    constexpr std::string_view arm64Calls[]{
        "glClearBufferfi",
        "glCompressedTexImage3D",
        "glCompressedTexSubImage3D",
        "glCopyTexImage2D",
        "glCopyTexSubImage3D",
        "glGetTransformFeedbackVarying",
        "glInvalidateSubFramebuffer",
        "glSamplerParameterf",
        "glTexImage3D",
        "glTexStorage3DMultisample",
        "glTexSubImage3D",
        "glVertexAttrib2f",
        "glVertexAttrib3f",
        "glVertexAttrib4f",
    };
    if (IsArm64TranslationEnabled() && std::ranges::find(arm64Calls, name) != std::end(arm64Calls))
        return Import(name, address);
    if (name == "glTexSubImage3D" || name == "glCompressedTexSubImage3D")
        return Import(name, address, AndroidToWindowsWideEleven);
    if (name == "glTexImage2D" || name == "glTexSubImage2D" || name == "glReadPixels" ||
        name == "glCompressedTexImage2D" || name == "glCompressedTexSubImage2D" || name == "glCopyTexSubImage2D" ||
        name == "glTexImage3D" || name == "glCompressedTexImage3D" || name == "glCopyTexImage2D" ||
        name == "glCopyTexSubImage3D" || name == "glGetTransformFeedbackVarying" ||
        name == "glDrawElementsInstancedBaseVertexBaseInstanceEXT" ||
        name == "glDrawElementsInstancedBaseVertexBaseInstanceANGLE" ||
        name == "glMultiDrawElementsInstancedBaseVertexBaseInstanceANGLE" || name == "glGetDebugMessageLogKHR")
        return Import(name, address, AndroidToWindowsWide);
    if (name == "glUniform2f" || name == "glUniform3f" || name == "glUniform4f" || name == "glUniform1f" ||
        name == "glAlphaFunc" || name == "glVertexAttrib1f" || name == "glVertexAttrib2f" ||
        name == "glVertexAttrib3f" || name == "glVertexAttrib4f")
        return Import(name, address, AndroidToWindowsIntFirst);
    if (name == "glSampleCoverage")
        return Import(name, address, AndroidToWindowsFloatFirst);
    if (name == "glTexParameterf") {
        using Function = void(GL_APIENTRYP)(GLenum, GLenum, GLfloat);
        static const auto function = reinterpret_cast<Function>(address);
        return Import(
            name,
            +[](float value, GLenum target, GLenum parameter) {
                function(target, parameter, value);
            },
            AndroidToWindowsFloatFirst);
    }
    if (name == "glSamplerParameterf") {
        using Function = void(GL_APIENTRYP)(GLuint, GLenum, GLfloat);
        static const auto function = reinterpret_cast<Function>(address);
        return Import(
            name,
            +[](float value, GLuint sampler, GLenum parameter) {
                function(sampler, parameter, value);
            },
            AndroidToWindowsFloatFirst);
    }
    if (name == "glClearBufferfi") {
        using Function = void(GL_APIENTRYP)(GLenum, GLint, GLfloat, GLint);
        static const auto function = reinterpret_cast<Function>(address);
        return Import(
            name,
            +[](float depth, GLenum buffer, GLint drawBuffer, GLint stencil) {
                function(buffer, drawBuffer, depth, stencil);
            },
            AndroidToWindowsFloatFirst);
    }
    if (name == "glBlitFramebuffer" || name == "glGetActiveUniform" || name == "glGetActiveAttrib")
        return Import(name, address, AndroidToWindowsWide);
    // Limit resolution to GLES calls with a known ABI. Unrecognized exports
    // must not be passed to the integer-only thunk.
    constexpr std::string_view regular[]{
        "glActiveTexture",
        "glAttachShader",
        "glBeginQuery",
        "glBeginTransformFeedback",
        "glBindAttribLocation",
        "glBindBuffer",
        "glBindBufferBase",
        "glBindBufferRange",
        "glBindFragDataLocationEXT",
        "glBindFragDataLocationIndexedEXT",
        "glBindFramebuffer",
        "glBindRenderbuffer",
        "glBindSampler",
        "glBindTexture",
        "glBindTransformFeedback",
        "glBindUniformLocationCHROMIUM",
        "glBindVertexArray",
        "glBindVertexArrayOES",
        "glBlendBarrierKHR",
        "glBlendColor",
        "glBlendEquation",
        "glBlendEquationSeparate",
        "glBlendFunc",
        "glBlendFuncSeparate",
        "glBufferData",
        "glBufferSubData",
        "glCheckFramebufferStatus",
        "glClear",
        "glClearBufferfv",
        "glClearBufferiv",
        "glClearBufferuiv",
        "glClearColor",
        "glClearDepthf",
        "glClearStencil",
        "glClientWaitSync",
        "glClipControlEXT",
        "glColorMask",
        "glCompileShader",
        "glCopyBufferSubData",
        "glCreateProgram",
        "glCreateShader",
        "glCullFace",
        "glDebugMessageControlKHR",
        "glDebugMessageInsertKHR",
        "glDeleteBuffers",
        "glDeleteFencesNV",
        "glDeleteFramebuffers",
        "glDeleteProgram",
        "glDeleteQueries",
        "glDeleteRenderbuffers",
        "glDeleteSamplers",
        "glDeleteShader",
        "glDeleteSync",
        "glDeleteTextures",
        "glDeleteTransformFeedbacks",
        "glDeleteVertexArrays",
        "glDeleteVertexArraysOES",
        "glDepthFunc",
        "glDepthMask",
        "glDepthRangef",
        "glDetachShader",
        "glDisable",
        "glDisableVertexAttribArray",
        "glDiscardFramebufferEXT",
        "glDispatchCompute",
        "glDrawArrays",
        "glDrawArraysIndirect",
        "glDrawArraysInstanced",
        "glDrawArraysInstancedBaseInstanceANGLE",
        "glDrawArraysInstancedBaseInstanceEXT",
        "glDrawBuffers",
        "glDrawElements",
        "glDrawElementsIndirect",
        "glDrawElementsInstanced",
        "glDrawRangeElements",
        "glEGLImageTargetRenderbufferStorageOES",
        "glEGLImageTargetTexture2DOES",
        "glEnable",
        "glEnableVertexAttribArray",
        "glEndQuery",
        "glEndTransformFeedback",
        "glFenceSync",
        "glFinish",
        "glFinishFenceNV",
        "glFlush",
        "glFlushMappedBufferRange",
        "glFramebufferRenderbuffer",
        "glFramebufferTexture2D",
        "glFramebufferTextureLayer",
        "glFrontFace",
        "glGenBuffers",
        "glGenFencesNV",
        "glGenFramebuffers",
        "glGenQueries",
        "glGenRenderbuffers",
        "glGenSamplers",
        "glGenTextures",
        "glGenTransformFeedbacks",
        "glGenVertexArrays",
        "glGenVertexArraysOES",
        "glGenerateMipmap",
        "glGetActiveAttrib",
        "glGetActiveUniformBlockName",
        "glGetActiveUniformBlockiv",
        "glGetActiveUniformsiv",
        "glGetAttachedShaders",
        "glGetAttribLocation",
        "glGetBooleanv",
        "glGetBufferParameteri64v",
        "glGetBufferParameteriv",
        "glGetBufferPointerv",
        "glGetError",
        "glGetFloatv",
        "glGetFragDataLocation",
        "glGetFramebufferAttachmentParameteriv",
        "glGetGraphicsResetStatusEXT",
        "glGetInteger64i_v",
        "glGetInteger64v",
        "glGetIntegeri_v",
        "glGetIntegerv",
        "glGetInternalformativ",
        "glGetMultisamplefv",
        "glGetObjectLabel",
        "glGetObjectLabelEXT",
        "glGetObjectLabelKHR",
        "glGetProgramBinary",
        "glGetProgramInfoLog",
        "glGetProgramiv",
        "glGetQueryObjectivEXT",
        "glGetQueryObjecti64vEXT",
        "glGetQueryObjectuiv",
        "glGetQueryObjectui64vEXT",
        "glGetQueryiv",
        "glGetRenderbufferParameteriv",
        "glGetSamplerParameterfv",
        "glGetSamplerParameteriv",
        "glGetShaderInfoLog",
        "glGetShaderPrecisionFormat",
        "glGetShaderSource",
        "glGetShaderiv",
        "glGetString",
        "glGetStringi",
        "glGetSynciv",
        "glGetTexLevelParameterfv",
        "glGetTexLevelParameteriv",
        "glGetTexParameterfv",
        "glGetTexParameteriv",
        "glGetUniformBlockIndex",
        "glGetUniformIndices",
        "glGetUniformLocation",
        "glGetUniformfv",
        "glGetUniformiv",
        "glGetUniformuiv",
        "glGetVertexAttribIiv",
        "glGetVertexAttribIuiv",
        "glGetVertexAttribPointerv",
        "glGetVertexAttribfv",
        "glGetVertexAttribiv",
        "glHint",
        "glInsertEventMarkerEXT",
        "glInvalidateFramebuffer",
        "glInvalidateSubFramebuffer",
        "glIsBuffer",
        "glIsEnabled",
        "glIsFenceNV",
        "glIsFramebuffer",
        "glIsProgram",
        "glIsQuery",
        "glIsRenderbuffer",
        "glIsSampler",
        "glIsShader",
        "glIsSync",
        "glIsTexture",
        "glIsTransformFeedback",
        "glIsVertexArray",
        "glLabelObjectEXT",
        "glLineWidth",
        "glLinkProgram",
        "glMapBufferOES",
        "glMapBufferRange",
        "glMemoryBarrier",
        "glMemoryBarrierByRegion",
        "glMultiDrawArraysIndirectEXT",
        "glMultiDrawArraysInstancedBaseInstanceANGLE",
        "glMultiDrawElementsIndirectEXT",
        "glObjectLabel",
        "glObjectLabelKHR",
        "glPauseTransformFeedback",
        "glPixelStorei",
        "glPolygonOffset",
        "glPopDebugGroupKHR",
        "glPopGroupMarkerEXT",
        "glProgramBinary",
        "glProgramParameteri",
        "glPushDebugGroupKHR",
        "glPushGroupMarkerEXT",
        "glQueryCounterEXT",
        "glReadBuffer",
        "glReleaseShaderCompiler",
        "glRenderbufferStorage",
        "glRenderbufferStorageMultisample",
        "glRequestExtensionANGLE",
        "glResumeTransformFeedback",
        "glSampleMaski",
        "glSamplerParameterfv",
        "glSamplerParameteri",
        "glSamplerParameteriv",
        "glScissor",
        "glShaderBinary",
        "glSetFenceNV",
        "glShaderSource",
        "glStencilFunc",
        "glStencilFuncSeparate",
        "glStencilMask",
        "glStencilMaskSeparate",
        "glStencilOp",
        "glStencilOpSeparate",
        "glTexBuffer",
        "glTexBufferOES",
        "glTexBufferRange",
        "glTexBufferRangeOES",
        "glTexParameterfv",
        "glTexParameteri",
        "glTexParameteriv",
        "glTexStorage2D",
        "glTexStorage2DEXT",
        "glTexStorage2DMultisample",
        "glTexStorage3D",
        "glTexStorage3DEXT",
        "glTransformFeedbackVaryings",
        "glUniform1fv",
        "glUniform1i",
        "glUniform1iv",
        "glUniform1ui",
        "glUniform1uiv",
        "glUniform2fv",
        "glUniform2i",
        "glUniform2iv",
        "glUniform2ui",
        "glUniform2uiv",
        "glUniform3fv",
        "glUniform3i",
        "glUniform3iv",
        "glUniform3ui",
        "glUniform3uiv",
        "glUniform4fv",
        "glUniform4i",
        "glUniform4iv",
        "glUniform4ui",
        "glUniform4uiv",
        "glUniformBlockBinding",
        "glUniformMatrix2fv",
        "glUniformMatrix2x3fv",
        "glUniformMatrix2x4fv",
        "glUniformMatrix3fv",
        "glUniformMatrix3x2fv",
        "glUniformMatrix3x4fv",
        "glUniformMatrix4fv",
        "glUniformMatrix4x2fv",
        "glUniformMatrix4x3fv",
        "glUnmapBuffer",
        "glUseProgram",
        "glValidateProgram",
        "glVertexAttrib1fv",
        "glVertexAttrib2fv",
        "glVertexAttrib3fv",
        "glVertexAttrib4fv",
        "glVertexAttribDivisor",
        "glVertexAttribDivisorANGLE",
        "glVertexAttribI4i",
        "glVertexAttribI4iv",
        "glVertexAttribI4ui",
        "glVertexAttribI4uiv",
        "glVertexAttribIPointer",
        "glVertexAttribPointer",
        "glTestFenceNV",
        "glViewport",
        "glWaitSync",
        "glWindowRectanglesEXT",
    };
    if (std::ranges::find(regular, name) != std::end(regular))
        return Import(name, address);
    // These are known Khronos prototypes. ARM64 dispatch still validates and
    // implements each operation separately; unsupported calls fail by name.
    return AndroidFrameworkGlImport(name, address);
}
}
