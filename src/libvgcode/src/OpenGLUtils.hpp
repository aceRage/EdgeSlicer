///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak, Vojtěch Bubník @bubnikv
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_OPENGLUTILS_HPP
#define VGCODE_OPENGLUTILS_HPP

// OpenGL loader
#ifdef ENABLE_OPENGL_ES
#include <glad/gles2.h>
#else
#include <glad/gl.h>
#endif // ENABLE_OPENGL_ES

#include <initializer_list>
#include <string>
#include <utility>

namespace libvgcode {
#ifndef NDEBUG
#define HAS_GLSAFE
#endif // NDEBUG

#ifdef HAS_GLSAFE
extern void glAssertRecentCallImpl(const char* file_name, unsigned int line, const char* function_name);
inline void glAssertRecentCall() { glAssertRecentCallImpl(__FILE__, __LINE__, __FUNCTION__); }
#define glsafe(cmd) do { cmd; glAssertRecentCallImpl(__FILE__, __LINE__, __FUNCTION__); } while (false)
#define glcheck() do { glAssertRecentCallImpl(__FILE__, __LINE__, __FUNCTION__); } while (false)
#else
// EDGE: EDGESLICER_GL_DEBUG=1 in the environment checks glGetError() after every call in release
// builds too and prints each failing call site once to stderr (the app's own GL code logs the same
// way, see 3DScene.hpp). Off, one flag test per call.
extern bool s_gl_debug_calls;
extern void glReportRecentCallImpl(const char* file_name, unsigned int line, const char* function_name);
inline void glAssertRecentCall() { }
#define glsafe(cmd) do { cmd; if (::libvgcode::s_gl_debug_calls) ::libvgcode::glReportRecentCallImpl(__FILE__, __LINE__, __FUNCTION__); } while (false)
#define glcheck() do { if (::libvgcode::s_gl_debug_calls) ::libvgcode::glReportRecentCallImpl(__FILE__, __LINE__, __FUNCTION__); } while (false)
#endif // HAS_GLSAFE

// EDGE: a host hook called right before each libvgcode draw, with the draw's name; the program
// that draws is bound. EdgeSlicer sets it with EDGESLICER_GL_DEBUG=1 to check every sampler of that
// program against what is bound on its unit (Viewer::set_draw_check_hook()). nullptr: no check.
using DrawCheckHook = void (*)(const char* what);
extern DrawCheckHook s_draw_check_hook;
inline void check_draw(const char* what) { if (s_draw_check_hook != nullptr) s_draw_check_hook(what); }

// EDGE (core profile): sets sampler uniforms of `program` to fixed texture units right after
// linking. A sampler left at its default unit 0 shares it with the samplerBuffer bound there
// (position_tex): macOS then reports "unit 0 GLD_TEXTURE_INDEX_2D is unloadable" for a sampler2D.
// Restores the bound program. Names the linker dropped are skipped.
void set_sampler_units(unsigned int program, std::initializer_list<std::pair<const char*, int>> units);

class OpenGLWrapper
{
public:
    static bool load_opengl(const std::string& context_version);
    static void unload_opengl();
    static bool is_valid_context() { return s_valid_context; }
#ifdef ENABLE_OPENGL_ES
    static size_t max_texture_size() { return static_cast<size_t>(s_max_texture_size); }
#endif // ENABLE_OPENGL_ES

private:
    static bool s_valid_context;
#ifdef ENABLE_OPENGL_ES
    static int s_max_texture_size;
#endif // ENABLE_OPENGL_ES
};

} // namespace libvgcode

#endif // VGCODE_OPENGLUTILS_HPP
