#ifndef slic3r_Showcase_ShowcaseMaterialGlsl_hpp_
#define slic3r_Showcase_ShowcaseMaterialGlsl_hpp_

// Showcase realism 1b (tests/design_showcase_realism.md 2.2): one copy of the material GLSL for
// the mesh and the toolpaths.
//
// resources/shaders/140/showcase_material.fs holds the material model (lights, environment, GGX,
// finishes, the output encoding) between two marker lines:
//
//     // ---- BEGIN SHOWCASE_MATERIAL_COMMON ----
//     ...
//     // ---- END SHOWCASE_MATERIAL_COMMON ----
//
// The toolpaths take that block plus resources/shaders/140/showcase_toolpath.glsl as the prelude
// of libvgcode's smooth-path fragment shader (libvgcode::Viewer::set_fragment_prelude). These
// functions cut the block out and check the assembled prelude, so a change to the mesh's shader
// reaches the toolpaths and a name clash is caught by the unit tests, not by a driver.
//
// GL-free.

#include <string>
#include <vector>

namespace Slic3r {
namespace Showcase {

// The marker name of the shared block in showcase_material.fs.
extern const char *const MATERIAL_GLSL_BLOCK;

// The lines strictly between "// ---- BEGIN <name> ----" and "// ---- END <name> ----" (each marker
// alone on its line, leading blanks allowed), with CR LF turned into LF. False, with `error`
// set, when a marker is missing, repeated, or the END comes first.
bool extract_glsl_block(const std::string &source, const std::string &name, std::string &out, std::string *error = nullptr);

// The toolpaths' prelude: the shared block of `material_fs` (the text of showcase_material.fs)
// followed by `toolpath_glsl` (showcase_toolpath.glsl). Empty, with `error` set, when the block is
// missing or the result fails toolpath_prelude_problems().
std::string toolpath_prelude(const std::string &material_fs, const std::string &toolpath_glsl, std::string *error = nullptr);

// What is wrong with a prelude for libvgcode's segments fragment shader: a #version line, a
// missing external_lit() or external_output(), or a global the segments shader also declares
// (libvgcode_fragment_globals()). Empty = fine.
std::vector<std::string> toolpath_prelude_problems(const std::string &prelude);

// The global names libvgcode's segments fragment shader declares (src/libvgcode/src/Shaders.hpp),
// which a prelude must leave alone.
const std::vector<std::string> &libvgcode_fragment_globals();

// The names a GLSL text declares at global scope: uniforms, ins, outs, constants and functions
// (a declaration starting at column 0). Good enough for the shaders here, not a GLSL parser.
std::vector<std::string> glsl_global_names(const std::string &source);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseMaterialGlsl_hpp_
