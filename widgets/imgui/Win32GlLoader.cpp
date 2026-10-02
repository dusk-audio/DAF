#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <windows.h>

#include "Win32GlLoader.h"

#include <cstdio>

daf::glloader::GlProc<PFNGLACTIVETEXTUREPROC> glActiveTexture { "glActiveTexture" };
daf::glloader::GlProc<PFNGLATTACHSHADERPROC> glAttachShader { "glAttachShader" };
daf::glloader::GlProc<PFNGLBINDBUFFERPROC> glBindBuffer { "glBindBuffer" };
daf::glloader::GlProc<PFNGLBINDSAMPLERPROC> glBindSampler { "glBindSampler" };
daf::glloader::GlProc<PFNGLBINDVERTEXARRAYPROC> glBindVertexArray { "glBindVertexArray" };
daf::glloader::GlProc<PFNGLBLENDEQUATIONPROC> glBlendEquation { "glBlendEquation" };
daf::glloader::GlProc<PFNGLBLENDEQUATIONSEPARATEPROC> glBlendEquationSeparate { "glBlendEquationSeparate" };
daf::glloader::GlProc<PFNGLBLENDFUNCSEPARATEPROC> glBlendFuncSeparate { "glBlendFuncSeparate" };
daf::glloader::GlProc<PFNGLBUFFERDATAPROC> glBufferData { "glBufferData" };
daf::glloader::GlProc<PFNGLBUFFERSUBDATAPROC> glBufferSubData { "glBufferSubData" };
daf::glloader::GlProc<PFNGLCOMPILESHADERPROC> glCompileShader { "glCompileShader" };
daf::glloader::GlProc<PFNGLCREATEPROGRAMPROC> glCreateProgram { "glCreateProgram" };
daf::glloader::GlProc<PFNGLCREATESHADERPROC> glCreateShader { "glCreateShader" };
daf::glloader::GlProc<PFNGLDELETEBUFFERSPROC> glDeleteBuffers { "glDeleteBuffers" };
daf::glloader::GlProc<PFNGLDELETEPROGRAMPROC> glDeleteProgram { "glDeleteProgram" };
daf::glloader::GlProc<PFNGLDELETESHADERPROC> glDeleteShader { "glDeleteShader" };
daf::glloader::GlProc<PFNGLDELETEVERTEXARRAYSPROC> glDeleteVertexArrays { "glDeleteVertexArrays" };
daf::glloader::GlProc<PFNGLDETACHSHADERPROC> glDetachShader { "glDetachShader" };
daf::glloader::GlProc<PFNGLDRAWELEMENTSBASEVERTEXPROC> glDrawElementsBaseVertex { "glDrawElementsBaseVertex" };
daf::glloader::GlProc<PFNGLENABLEVERTEXATTRIBARRAYPROC> glEnableVertexAttribArray { "glEnableVertexAttribArray" };
daf::glloader::GlProc<PFNGLGENBUFFERSPROC> glGenBuffers { "glGenBuffers" };
daf::glloader::GlProc<PFNGLGENVERTEXARRAYSPROC> glGenVertexArrays { "glGenVertexArrays" };
daf::glloader::GlProc<PFNGLGETATTRIBLOCATIONPROC> glGetAttribLocation { "glGetAttribLocation" };
daf::glloader::GlProc<PFNGLGETPROGRAMINFOLOGPROC> glGetProgramInfoLog { "glGetProgramInfoLog" };
daf::glloader::GlProc<PFNGLGETPROGRAMIVPROC> glGetProgramiv { "glGetProgramiv" };
daf::glloader::GlProc<PFNGLGETSHADERINFOLOGPROC> glGetShaderInfoLog { "glGetShaderInfoLog" };
daf::glloader::GlProc<PFNGLGETSHADERIVPROC> glGetShaderiv { "glGetShaderiv" };
daf::glloader::GlProc<PFNGLGETSTRINGIPROC> glGetStringi { "glGetStringi" };
daf::glloader::GlProc<PFNGLGETUNIFORMLOCATIONPROC> glGetUniformLocation { "glGetUniformLocation" };
daf::glloader::GlProc<PFNGLISPROGRAMPROC> glIsProgram { "glIsProgram" };
daf::glloader::GlProc<PFNGLLINKPROGRAMPROC> glLinkProgram { "glLinkProgram" };
daf::glloader::GlProc<PFNGLSHADERSOURCEPROC> glShaderSource { "glShaderSource" };
daf::glloader::GlProc<PFNGLUNIFORM1IPROC> glUniform1i { "glUniform1i" };
daf::glloader::GlProc<PFNGLUNIFORMMATRIX4FVPROC> glUniformMatrix4fv { "glUniformMatrix4fv" };
daf::glloader::GlProc<PFNGLUSEPROGRAMPROC> glUseProgram { "glUseProgram" };
daf::glloader::GlProc<PFNGLVERTEXATTRIBPOINTERPROC> glVertexAttribPointer { "glVertexAttribPointer" };

namespace daf { namespace glloader {

ProcPtr procAddress(const char* const name)
{
    const auto address = ::wglGetProcAddress(name);
    const std::intptr_t value = reinterpret_cast<std::intptr_t>(address);
    if (value == 0 || value == 1 || value == 2 || value == 3 || value == -1)
    {
        char message[192] {};
        std::snprintf (message, sizeof message,
                       "[DAF/widgets] OpenGL entry point unavailable: %s\n",
                       name);
        std::fputs (message, stderr);
        ::OutputDebugStringA (message);
        return nullptr;
    }
    return reinterpret_cast<ProcPtr> (address);
}

bool hasCurrentContext()
{
    return ::wglGetCurrentContext() != nullptr;
}

void reportNoContext(const char* const name)
{
    char message[192] {};
    std::snprintf (message, sizeof message,
                   "[DAF/widgets] OpenGL call without a current context: %s\n",
                   name);
    std::fputs (message, stderr);
    ::OutputDebugStringA (message);
}

} }

#endif
