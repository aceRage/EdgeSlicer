///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak, Vojtěch Bubník @bubnikv
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/

#include "OpenGLUtils.hpp"

#include <iostream>
#include <assert.h>
#include <cctype>
#include <stdio.h>
#include <cstring>
#include <string>
#include <cstdlib>
#include <set>
#include <utility>

namespace libvgcode {

#ifdef HAS_GLSAFE
void glAssertRecentCallImpl(const char* file_name, unsigned int line, const char* function_name)
{
    const GLenum err = glGetError();
    if (err == GL_NO_ERROR)
        return;
    const char* sErr = 0;
    switch (err) {
    case GL_INVALID_ENUM:      { sErr = "Invalid Enum"; break; }
    case GL_INVALID_VALUE:     { sErr = "Invalid Value"; break; }
    // be aware that GL_INVALID_OPERATION is generated if glGetError is executed between the execution of glBegin / glEnd 
    case GL_INVALID_OPERATION: { sErr = "Invalid Operation"; break; }
    case GL_OUT_OF_MEMORY:     { sErr = "Out Of Memory"; break; }
    case GL_INVALID_FRAMEBUFFER_OPERATION: { sErr = "Invalid framebuffer operation"; break; }
#if !defined(ENABLE_OPENGL_ES)
    case GL_STACK_OVERFLOW:    { sErr = "Stack Overflow"; break; }
    case GL_STACK_UNDERFLOW:   { sErr = "Stack Underflow"; break; }
#endif // ENABLE_OPENGL_ES
    default:                   { sErr = "Unknown"; break; }
    }
    std::cout << "OpenGL error in " << file_name << ":" << line << ", function " << function_name << "() : " << (int)err << " - " << sErr << "\n";
    assert(false);
}
#else
static bool read_gl_debug_env()
{
    const char* value = ::getenv("EDGESLICER_GL_DEBUG");
    return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0 && std::strcmp(value, "off") != 0;
}
bool s_gl_debug_calls = read_gl_debug_env();

void glReportRecentCallImpl(const char* file_name, unsigned int line, const char* function_name)
{
    const GLenum err = glGetError();
    if (err == GL_NO_ERROR)
        return;
    // Drain the rest, then report each call site once (a per-frame error must not flood stderr).
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}
    static std::set<std::pair<std::string, unsigned int>> logged;
    static int reports_left = 100;
    if (reports_left <= 0 || !logged.insert({ std::string(file_name), line }).second)
        return;
    --reports_left;
    char code[16];
    snprintf(code, sizeof(code), "0x%04X", (unsigned int)err);
    std::cerr << "EdgeSlicer OpenGL debug (libvgcode): GL error " << code << " at " << file_name << ":" << line << " (" << function_name
              << "()), raised by that call or an unchecked GL call just before it\n";
}
#endif // HAS_GLSAFE

DrawCheckHook s_draw_check_hook = nullptr;

void set_sampler_units(unsigned int program, std::initializer_list<std::pair<const char*, int>> units)
{
    if (program == 0)
        return;
    GLint previous = 0;
    glsafe(glGetIntegerv(GL_CURRENT_PROGRAM, &previous));
    glsafe(glUseProgram(program));
    for (const auto& [name, unit] : units) {
        const GLint location = glGetUniformLocation(program, name);
        if (location >= 0)
            glsafe(glUniform1i(location, unit));
    }
    glsafe(glUseProgram(static_cast<GLuint>(previous)));
}

static const char* OPENGL_ES_PREFIXES[] = { "OpenGL ES-CM ", "OpenGL ES-CL ", "OpenGL ES ", nullptr };

bool OpenGLWrapper::s_valid_context = false;
#ifdef ENABLE_OPENGL_ES
int OpenGLWrapper::s_max_texture_size = 0;
#endif // ENABLE_OPENGL_ES

bool OpenGLWrapper::load_opengl(const std::string& context_version)
{
    s_valid_context = false;

    const char* version = context_version.c_str();
    for (int i = 0; OPENGL_ES_PREFIXES[i] != nullptr; ++i) {
        const size_t length = strlen(OPENGL_ES_PREFIXES[i]);
        if (strncmp(version, OPENGL_ES_PREFIXES[i], length) == 0) {
            version += length;
            break;
        }
    }

    GLint major = 0;
    GLint minor = 0;
#ifdef _MSC_VER
    const int res = sscanf_s(version, "%d.%d", &major, &minor);
#else
    const int res = sscanf(version, "%d.%d", &major, &minor);
#endif // _MSC_VER
    if (res != 2)
        return false;

#ifdef ENABLE_OPENGL_ES
    s_valid_context = major > 3 || (major == 3 && minor >= 0);
    const int glad_res = gladLoaderLoadGLES2();
#else
    s_valid_context = major > 3 || (major == 3 && minor >= 2);
    const int glad_res = gladLoaderLoadGL();
#endif // ENABLE_OPENGL_ES

    if (glad_res == 0)
        return false;

#ifdef ENABLE_OPENGL_ES
    glsafe(glGetIntegerv(GL_MAX_TEXTURE_SIZE, &s_max_texture_size));
#endif // ENABLE_OPENGL_ES

    return s_valid_context;
}

void OpenGLWrapper::unload_opengl()
{
#ifdef ENABLE_OPENGL_ES
    gladLoaderUnloadGLES2();
#else
    // EDGE: desktop GL shares the application's GLAD loader (src/glad, OrcaSlicer #13197), so
    // unloading it here would drop the GL entry points every other renderer still uses. The
    // application owns the loader; leave it loaded.
#endif // ENABLE_OPENGL_ES
}

} // namespace libvgcode
