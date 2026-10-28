// Showcase realism 1b: the material GLSL shared by the mesh and the toolpaths
// (libslic3r/Showcase/ShowcaseMaterialGlsl, resources/shaders/140/showcase_material.fs and
// showcase_toolpath.glsl, libvgcode's segments fragment shader in src/libvgcode/src/Shaders.hpp).

#include <catch2/catch.hpp>

#include "libslic3r/Showcase/ShowcaseMaterialGlsl.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

using namespace Slic3r::Showcase;

namespace {

std::string repo_file(const std::string &relative)
{
    const std::string path = std::string(TEST_DATA_DIR) + "/../../" + relative;
    std::ifstream     in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The GLSL of a `static const char* <name> =` string in Shaders.hpp: its quoted lines, unescaped.
std::string libvgcode_shader(const std::string &shaders_hpp, const std::string &name)
{
    std::istringstream in(shaders_hpp);
    std::string        line, out;
    bool               inside = false;
    const std::string  start  = "static const char* " + name + " =";
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!inside) {
            inside = line.rfind(start, 0) == 0;
            continue;
        }
        const size_t q0 = line.find('"');
        if (q0 != 0) {
            if (line.find(';') == 0)
                break;
            continue;   // a C++ comment between the string's lines
        }
        const size_t q1 = line.rfind('"');
        std::string  s  = line.substr(q0 + 1, q1 - q0 - 1);
        std::string  u;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                const char c = s[++i];
                u += c == 'n' ? '\n' : c;
            } else
                u += s[i];
        }
        out += u;
        if (line.size() > q1 + 1 && line.find(';', q1) != std::string::npos)
            break;
    }
    return out;
}

bool contains(const std::vector<std::string> &v, const std::string &s) { return std::find(v.begin(), v.end(), s) != v.end(); }

} // namespace

TEST_CASE("GLSL block extraction", "[Showcase][ShowcaseGlsl]")
{
    std::string out, error;
    SECTION("the lines between the markers, CR LF as LF")
    {
        const std::string src = "a\r\n  // ---- BEGIN X ----\r\nb\r\nc\r\n// ---- END X ----\r\nd\r\n";
        REQUIRE(extract_glsl_block(src, "X", out, &error));
        CHECK(out == "b\nc\n");
    }
    SECTION("an empty block is fine")
    {
        REQUIRE(extract_glsl_block("// ---- BEGIN X ----\n// ---- END X ----\n", "X", out, &error));
        CHECK(out.empty());
    }
    SECTION("missing, repeated or reversed markers")
    {
        CHECK_FALSE(extract_glsl_block("a\nb\n", "X", out, &error));
        CHECK_THAT(error, Catch::Matchers::Contains("no BEGIN"));
        CHECK_FALSE(extract_glsl_block("// ---- BEGIN X ----\nb\n", "X", out, &error));
        CHECK_THAT(error, Catch::Matchers::Contains("no END"));
        CHECK_FALSE(extract_glsl_block("// ---- END X ----\n// ---- BEGIN X ----\n", "X", out, &error));
        CHECK_FALSE(extract_glsl_block("// ---- BEGIN X ----\n// ---- END X ----\n// ---- BEGIN X ----\n// ---- END X ----\n", "X", out, &error));
        CHECK(out.empty());
    }
    SECTION("another block's markers do not count")
    {
        CHECK_FALSE(extract_glsl_block("// ---- BEGIN Y ----\nb\n// ---- END Y ----\n", "X", out, &error));
    }
}

TEST_CASE("GLSL global names", "[Showcase][ShowcaseGlsl]")
{
    const std::string src = "#version 150\n"
                            "const float PI = 3.14;\n"
                            "uniform vec3 light_dir[3];\n"
                            "uniform float a, b[2], c = 1.0;\n"
                            "flat in vec4 seg_info;\n"
                            "struct Hit { float t; };\n"
                            "vec3 f(vec3 x) { return x; }\n"
                            "float g(float x,\n"
                            "        float y)\n"
                            "{\n"
                            "    const float inner = 1.0;\n"
                            "    float h = 2.0; // vec3 not_a_function(\n"
                            "    return inner;\n"
                            "}\n";
    const std::vector<std::string> names = glsl_global_names(src);
    for (const char *n : { "PI", "light_dir", "a", "b", "c", "seg_info", "Hit", "f", "g" })
        CHECK(contains(names, n));
    CHECK_FALSE(contains(names, "inner"));
    CHECK_FALSE(contains(names, "h"));
    CHECK_FALSE(contains(names, "x"));
    CHECK_FALSE(contains(names, "not_a_function"));
}

TEST_CASE("Toolpath prelude problems", "[Showcase][ShowcaseGlsl]")
{
    const std::string ok = "vec3 external_lit(vec3 n, vec3 v, vec3 t, vec3 b, vec3 p, float g, float s, int l) { return b; }\n"
                           "vec4 external_output(vec3 c) { return vec4(c, 1.0); }\n";
    CHECK(toolpath_prelude_problems(ok).empty());
    CHECK(toolpath_prelude_problems("#version 150\n" + ok).size() == 1);
    CHECK(toolpath_prelude_problems("vec4 external_output(vec3 c) { return vec4(c, 1.0); }\n").size() == 1);
    // A name libvgcode's shader has too.
    const std::vector<std::string> clash = toolpath_prelude_problems("uniform float exposure;\n" + ok);
    REQUIRE(clash.size() == 1);
    CHECK_THAT(clash.front(), Catch::Matchers::Contains("exposure"));
    CHECK(toolpath_prelude_problems("uniform float q;\nuniform float q;\n" + ok).size() == 1);
}

TEST_CASE("The shipped toolpath prelude assembles cleanly", "[Showcase][ShowcaseGlsl]")
{
    const std::string material = repo_file("resources/shaders/140/showcase_material.fs");
    const std::string glue     = repo_file("resources/shaders/140/showcase_toolpath.glsl");
    REQUIRE_FALSE(material.empty());
    REQUIRE_FALSE(glue.empty());

    std::string block, error;
    REQUIRE(extract_glsl_block(material, MATERIAL_GLSL_BLOCK, block, &error));
    // The material model the mesh uses is all in the block: the toolpaths get the same one.
    const std::vector<std::string> block_names = glsl_global_names(block);
    for (const char *n : { "shade_material", "material_output", "env_specular", "env_diffuse", "ggx_aniso", "tone_curve_map",
                           "light_dir", "env_sh", "hdr_output" })
        CHECK(contains(block_names, n));

    const std::string prelude = toolpath_prelude(material, glue, &error);
    INFO(error);
    REQUIRE_FALSE(prelude.empty());
    CHECK(toolpath_prelude_problems(prelude).empty());

    SECTION("the mesh shader declares nothing twice")
    {
        const std::vector<std::string> names = glsl_global_names(material);
        for (size_t i = 0; i < names.size(); ++i)
            for (size_t j = i + 1; j < names.size(); ++j) {
                INFO(names[i]);
                CHECK(names[i] != names[j]);
            }
    }
}

TEST_CASE("libvgcode's segments fragment shader names are all reserved", "[Showcase][ShowcaseGlsl]")
{
    // If libvgcode's shader gains a global, libvgcode_fragment_globals() has to learn it, or a
    // prelude could clash with it on the user's driver instead of in this test.
    const std::string shaders = repo_file("src/libvgcode/src/Shaders.hpp");
    REQUIRE_FALSE(shaders.empty());
    const std::string fragment = libvgcode_shader(shaders, "Segments_Fragment_Shader");
    REQUIRE(fragment.find("void main()") != std::string::npos);
    REQUIRE(fragment.find("#ifdef EXTERNAL_LIGHTING") != std::string::npos);
    const std::vector<std::string> &reserved = libvgcode_fragment_globals();
    for (const std::string &n : glsl_global_names(fragment)) {
        INFO(n);
        CHECK(contains(reserved, n));
    }
    // And the prelude defines nothing the shader does.
    std::string       error;
    const std::string prelude = toolpath_prelude(repo_file("resources/shaders/140/showcase_material.fs"),
                                                 repo_file("resources/shaders/140/showcase_toolpath.glsl"), &error);
    REQUIRE_FALSE(prelude.empty());
    const std::vector<std::string> shader_names = glsl_global_names(fragment);
    for (const std::string &n : glsl_global_names(prelude)) {
        INFO(n);
        CHECK_FALSE(contains(shader_names, n));
    }
}
