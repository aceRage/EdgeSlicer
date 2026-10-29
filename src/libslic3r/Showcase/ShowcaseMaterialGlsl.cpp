#include "ShowcaseMaterialGlsl.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace Slic3r {
namespace Showcase {

const char *const MATERIAL_GLSL_BLOCK = "SHOWCASE_MATERIAL_COMMON";

namespace {

std::string glsl_unix_newlines(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        if (!(s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n'))
            out += s[i];
    return out;
}

std::string glsl_trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char) s[b]))
        ++b;
    while (e > b && std::isspace((unsigned char) s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

bool glsl_is_ident_char(char c) { return std::isalnum((unsigned char) c) || c == '_'; }

// The identifiers of a line, in order (numbers and operators dropped).
std::vector<std::string> glsl_identifiers(const std::string &line)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        if (glsl_is_ident_char(line[i]) && !std::isdigit((unsigned char) line[i])) {
            size_t j = i;
            while (j < line.size() && glsl_is_ident_char(line[j]))
                ++j;
            out.push_back(line.substr(i, j - i));
            i = j;
        } else
            ++i;
    }
    return out;
}

} // namespace

bool extract_glsl_block(const std::string &source, const std::string &name, std::string &out, std::string *error)
{
    const std::string begin = "// ---- BEGIN " + name + " ----";
    const std::string end   = "// ---- END " + name + " ----";
    std::istringstream in(glsl_unix_newlines(source));
    std::string        line, body;
    int                begins = 0, ends = 0;
    bool               inside = false, ok = true;
    std::string        why;
    while (std::getline(in, line)) {
        const std::string t = glsl_trim(line);
        if (t == begin) {
            ++begins;
            if (inside || ends > 0) {
                ok  = false;
                why = "a second BEGIN marker";
            }
            inside = true;
            continue;
        }
        if (t == end) {
            ++ends;
            if (!inside) {
                ok  = false;
                why = begins == 0 ? "END before BEGIN" : "a second END marker";
            }
            inside = false;
            continue;
        }
        if (inside)
            body += line + "\n";
    }
    if (ok && begins == 0)
        ok = false, why = "no BEGIN marker";
    if (ok && (begins != 1 || ends != 1))
        ok = false, why = inside ? "no END marker" : "markers repeated";
    if (!ok) {
        if (error != nullptr)
            *error = "GLSL block " + name + ": " + why;
        out.clear();
        return false;
    }
    out = body;
    return true;
}

const std::vector<std::string> &libvgcode_fragment_globals()
{
    // Segments_Fragment_Shader in src/libvgcode/src/Shaders.hpp (uniforms, varyings, constants and
    // functions). Keep in step when that shader gains a name.
    static const std::vector<std::string> names = {
        "light_top_dir", "shadow_map", "shadow_light_vp", "shadow_intensity", "shadow_map_texel", "exposure", "saturation",
        "clipping_plane", "view_matrix", "projection_matrix", "camera_position", "position_tex", "height_width_angle_tex",
        "smooth_mode", "look_a", "look_b", "section", "seg_info", "seg_right", "seg_up", "seg_dir", "base_color",
        "s_light_top_diffuse", "s_light_top_specular", "s_light_top_shininess", "s_light_front_dir", "s_light_front_diffuse",
        "s_ambient", "s_emission", "UP", "LUMA", "color", "color_direct", "world_position", "shadow_normal", "segment_id",
        "cut_color_direct", "fragment_color", "shadow_shade", "find_cut", "smooth_normal", "smooth_lit", "main",
        // EDGE multi-colour P3: the base colour hook's varyings and the colour the smooth path lights.
        "seg_role", "extruded_volume", "bead_base",
    };
    return names;
}

std::vector<std::string> glsl_global_names(const std::string &source)
{
    static const std::set<std::string> qualifiers = { "uniform", "in", "out", "const", "flat", "smooth", "noperspective",
                                                      "highp", "mediump", "lowp", "centroid" };
    std::vector<std::string> names;
    std::istringstream       in(glsl_unix_newlines(source));
    std::string              line;
    int                      depth = 0;   // inside { }
    while (std::getline(in, line)) {
        const size_t comment = line.find("//");
        std::string  code    = comment == std::string::npos ? line : line.substr(0, comment);
        const bool   at_top  = depth == 0 && !code.empty() && !std::isspace((unsigned char) code[0]) && code[0] != '#';
        if (at_top) {
            const std::vector<std::string> ids = glsl_identifiers(code);
            size_t                          k   = 0;
            bool                            decl = false;
            while (k < ids.size() && qualifiers.count(ids[k]) > 0) {
                ++k;
                decl = true;
            }
            if (k < ids.size() && ids[k] == "struct") {
                if (k + 1 < ids.size())
                    names.push_back(ids[k + 1]);
            } else if (decl) {
                // qualifiers, the type, then one or more names (a, b[3], c = ...).
                if (k + 1 < ids.size()) {
                    names.push_back(ids[k + 1]);
                    // Further names of a comma list, at the top level of the declaration.
                    const size_t first = code.find(ids[k + 1], code.find(ids[k]) + ids[k].size());
                    int          paren = 0;
                    for (size_t i = first; i < code.size(); ++i) {
                        const char c = code[i];
                        if (c == '(' || c == '[')
                            ++paren;
                        else if (c == ')' || c == ']')
                            --paren;
                        else if (c == '=' && paren == 0)
                            break;
                        else if (c == ',' && paren == 0) {
                            size_t j = i + 1;
                            while (j < code.size() && std::isspace((unsigned char) code[j]))
                                ++j;
                            size_t e = j;
                            while (e < code.size() && glsl_is_ident_char(code[e]))
                                ++e;
                            if (e > j)
                                names.push_back(code.substr(j, e - j));
                        }
                    }
                }
            } else if (ids.size() >= 2) {
                // A function: "<type> <name>(".
                const size_t at = code.find(ids[1], code.find(ids[0]) + ids[0].size());
                size_t       p  = at + ids[1].size();
                while (p < code.size() && std::isspace((unsigned char) code[p]))
                    ++p;
                if (p < code.size() && code[p] == '(')
                    names.push_back(ids[1]);
            }
        }
        for (char c : code) {
            if (c == '{')
                ++depth;
            else if (c == '}')
                depth = std::max(0, depth - 1);
        }
    }
    return names;
}

std::vector<std::string> toolpath_prelude_problems(const std::string &prelude)
{
    std::vector<std::string> problems;
    const std::string        text = glsl_unix_newlines(prelude);
    {
        std::istringstream in(text);
        std::string        line;
        while (std::getline(in, line))
            if (glsl_trim(line).rfind("#version", 0) == 0) {
                problems.push_back("has a #version line (libvgcode keeps its own)");
                break;
            }
    }
    const std::vector<std::string> names = glsl_global_names(text);
    for (const char *needed : { "external_lit", "external_output" })
        if (std::find(names.begin(), names.end(), needed) == names.end())
            problems.push_back(std::string("does not define ") + needed + "()");
    const std::vector<std::string> &reserved = libvgcode_fragment_globals();
    std::set<std::string>           reported;
    for (const std::string &n : names)
        if (std::find(reserved.begin(), reserved.end(), n) != reserved.end() && reported.insert(n).second)
            problems.push_back("declares '" + n + "', which libvgcode's segments shader declares too");
    std::set<std::string> seen;
    for (const std::string &n : names)
        if (!seen.insert(n).second && reported.insert(n).second)
            problems.push_back("declares '" + n + "' twice");
    return problems;
}

std::string toolpath_prelude(const std::string &material_fs, const std::string &toolpath_glsl, std::string *error)
{
    std::string block;
    if (!extract_glsl_block(material_fs, MATERIAL_GLSL_BLOCK, block, error))
        return {};
    std::string prelude = "// showcase_material.fs, " + std::string(MATERIAL_GLSL_BLOCK) + "\n" + block +
                          "// showcase_toolpath.glsl\n" + glsl_unix_newlines(toolpath_glsl);
    if (!prelude.empty() && prelude.back() != '\n')
        prelude += '\n';
    const std::vector<std::string> problems = toolpath_prelude_problems(prelude);
    if (!problems.empty()) {
        if (error != nullptr) {
            std::string msg = "toolpath prelude:";
            for (const std::string &p : problems)
                msg += " " + p + ";";
            *error = msg;
        }
        return {};
    }
    return prelude;
}

} // namespace Showcase
} // namespace Slic3r
