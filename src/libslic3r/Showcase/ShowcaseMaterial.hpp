#ifndef slic3r_Showcase_ShowcaseMaterial_hpp_
#define slic3r_Showcase_ShowcaseMaterial_hpp_

// Showcase tab, phase S2: how a filament looks in a presentation render. The finish (matte, silk,
// gloss, transparent, ...) is inferred from the filament preset's name and type (D4: no new profile
// key) and can be overridden per filament in the scene. Also the light rig maths (colour
// temperature, the three-point lights around the world-fixed key, D14) and the procedural layer
// lines (design 2.6.2): where a fragment sits inside its layer, and how far the stripes fade on a
// far shot. The GLSL in resources/shaders/140/showcase_material.fs mirrors the functions marked so.
//
// GL- and wxWidgets-free, so the unit tests can check the inference and the maths.

#include <array>
#include <string>
#include <vector>

namespace Slic3r {
namespace Showcase {

// The look of one filament. The JSON names (to_string) are stable; never translate them.
enum class Finish : int { Standard = 0, Matte, Silk, Gloss, Transparent, Metallic, CarbonFiber };

const char *to_string(Finish v);
bool        from_string(const std::string &s, Finish &v);
// Realism 1c: JSON names kept back for finishes still to come, so nothing else takes them and an
// older build reads them as unknown (the inferred finish): "multicolour_silk", the dual / tri-colour
// coextruded silk (tests/design_multicolour_filament_view.md).
const std::vector<std::string> &reserved_finish_names();
std::vector<Finish> all_finishes();

// D4: the finish a filament preset most likely has, from its name (e.g. "Snapmaker PLA Silk @U1",
// "Generic PETG-CF") and its filament_type ("PLA", "PETG", "PLA-CF", ...). Whole words only, so a
// vendor called "MatterHackers" is not matte. Order of precedence: transparent, silk, metallic,
// carbon/glass fibre, matte, gloss (PETG family), standard.
Finish infer_finish(const std::string &preset_name, const std::string &filament_type);

// The parameters the material shader takes for a finish (design 2.5).
struct MaterialParams
{
    float roughness   = 0.55f;   // GGX roughness (perceptual), 0..1
    float f0          = 0.04f;   // specular reflectance at normal incidence
    float metal_tint  = 0.f;     // 0 = white highlights, 1 = highlights in the filament colour
    float anisotropy  = 0.f;     // 0 = round highlight, 1 = streaked across the layer lines (silk)
    float clearcoat   = 0.f;     // a second, sharper lobe (gloss)
    float alpha       = 1.f;     // < 1: drawn translucent, sorted per object (D15)
    float line_shade  = 0.5f;    // how much the groove between two layers darkens (matte hides less)
    float fiber_noise = 0.f;     // micro speckle of carbon / glass fibre
};
MaterialParams material_params(Finish finish);

// --- toolpath realism (S5, D13; design 2.6.3) --------------------------------------------------

// The finish's default Smoothness (design 2.6.3's table): matte 0.5, standard 0.6, silk 0.8, gloss
// 0.75, transparent 0.7, metallic 0.7, carbon fibre 0.45.
float default_toolpath_smoothness(Finish finish);

// What the toolpath shader does at a Smoothness (0 = Preview's crisp extrusions .. 1), the uniforms of
// libvgcode's smooth path (A + B + F of D13). At 0 everything is off: the shader takes its old path.
struct ToolpathLook
{
    float smoothness   = 0.f;   // 0..1
    // A: the bead's cross-section, a superellipse |x|^p + |y|^p = 1 (p 2 = round .. 6 = boxy, a bulge
    // that sits flatter on the bead below).
    float bulge        = 2.f;
    // B: top and bottom skin (solid infill, ironing): the line widened by this factor so neighbours
    // overlap (1 = as sliced), and its normal bent towards straight up/down (0..1).
    float skin_overlap = 1.f;
    float skin_flatten = 0.f;
    // The groove between two wall layers: how dark it stays (0 = none .. 1 = as crisp toolpaths).
    float groove       = 1.f;
    // The SSAO weight on skin (the creases there were the main artefact): 1 = full, 0 = none.
    float ssao_skin    = 1.f;
    // Silk: the highlight streaks along the extrusion (0..1).
    float anisotropy   = 0.f;
};

// `smoothness` < 0 = the finish's default. The finish shapes the defaults (silk overlaps more and
// streaks its highlight, matte keeps its grooves).
ToolpathLook toolpath_look(Finish finish, float smoothness);

// --- light rig ---------------------------------------------------------------------------------

// Linear RGB of a black body at `kelvin` (clamped to 1000..40000), largest channel 1. 6500 K is
// close to white, lower is warmer.
std::array<float, 3> color_temperature_rgb(float kelvin);

// --- procedural layer lines (design 2.6.2) -----------------------------------------------------

enum class LayerLines : int { Off = 0, Subtle, Real, Exaggerated };
const char *to_string(LayerLines v);
bool        from_string(const std::string &s, LayerLines &v);
// Normal tilt amplitude of the bead profile: 0, 0.15, 0.35, 0.7.
float layer_line_depth(LayerLines v);

// How visible the stripes are when one layer covers `layer_px` pixels on screen: 0 at 1 px and
// below, 1 at 2 px and above, smooth in between (0.5 at 1.5 px). Stripes thinner than that would
// only moire. Mirrored in GLSL as layer_fade(): layer_px = layer_height / fwidth(z).
float layer_line_fade(float layer_px);
// layer_px for a layer `layer_height` mm high where z changes by `dz_per_px` mm per pixel.
float layer_pixels(float layer_height, float dz_per_px);

// The object's layers as in generate_object_layers(): pairs of bottom / top Z, from Z = 0.
// A fragment's place inside its layer.
struct LayerSpan
{
    float bottom = 0.f;
    float top    = 0.f;
    float height() const { return top - bottom; }
};

class LayerTable
{
public:
    LayerTable() = default;
    // `boundaries` is generate_object_layers()' output: lo0, hi0, lo1, hi1, ...
    explicit LayerTable(const std::vector<double> &boundaries);

    bool  empty() const { return m_tops.empty(); }
    size_t size() const { return m_tops.size(); }
    const std::vector<float> &tops() const { return m_tops; }

    // Every layer above the first is as high as the first of them (within 1 um): the shader then
    // needs only first_layer_height() and layer_height(), no table.
    bool  uniform() const { return m_uniform; }
    float first_layer_height() const { return m_tops.empty() ? 0.f : m_tops.front(); }
    float layer_height() const { return m_tops.size() > 1 ? m_tops[1] - m_tops[0] : first_layer_height(); }

    // The layer containing z (binary search over the tops; mirrored in GLSL). Below 0 it is the
    // first layer, above the last the last one continued.
    LayerSpan layer_at(float z) const;
    // 1c: the index of that layer, 0 = the first (mirrored in GLSL as layer_index_at()).
    int       index_at(float z) const;

private:
    std::vector<float> m_tops;
    bool               m_uniform = true;
};

// Uniform layers without a table: the first layer, then `layer_height` steps (mirrored in GLSL).
LayerSpan uniform_layer_at(float z, float first_layer_height, float layer_height);
// 1c: the index of uniform_layer_at()'s layer, 0 = the first.
int       uniform_layer_index_at(float z, float first_layer_height, float layer_height);
// 0 at the bottom of the layer, 1 at its top.
float     layer_phase(const LayerSpan &span, float z);

// --- top and bottom skin direction (realism 1c, design 2.3) -------------------------------------
//
// Which way the lines of a top (or bottom) skin run, worked out from the print settings as
// Fill.cpp lays them, without a slice: the silk sheen's tangent on the mesh's flat faces (1c), and
// the top stripes of the multi-colour filament view (tests/design_multicolour_filament_view.md,
// P4). Angles are in degrees from +X towards +Y in the plate's frame; a line and its reverse are
// the same direction, so they are kept in [0, 180).

enum class SkinPattern : int {
    Isotropic = 0,   // no single direction the shader can model (Hilbert, Archimedean chords, ...)
    Lines,           // straight lines: rectilinear, monotonic, monotonic line, aligned rectilinear
    Concentric,      // rings around the face (concentric, spiral inset)
};

struct SkinTangent
{
    SkinPattern        pattern = SkinPattern::Isotropic;
    // Lines: layer i of the object (0 = its first layer) runs at angles_deg[i % size()].
    std::vector<float> angles_deg;
    // Lines: neighbouring lines run opposite ways (a zig-zag: rectilinear, monotonic); false when
    // every line runs the same way (monotonic line). The sheen does not care; P4's stripes do.
    bool               zigzag = true;

    // Lines: the direction of layer `layer` in degrees, [0, 180); 0 for the other patterns.
    float angle_at(int layer) const;
    // Lines: the unit direction (x, y) of layer `layer`; (0, 0) for the other patterns.
    std::array<float, 2> direction_at(int layer) const;
    // Concentric: the unit direction along the ring through (x, y) around (cx, cy), anticlockwise;
    // (0, 0) at the centre.
    static std::array<float, 2> ring_direction(float x, float y, float cx, float cy);
};

// The settings skin_tangent_mode() reads (the config's values for one surface).
struct SkinSettings
{
    std::string pattern                = "monotonicline";   // top_surface_pattern / bottom_surface_pattern, serialized
    double      solid_infill_direction = 45.;               // solid_infill_direction
    double      surface_direction      = -1.;               // top_layer_direction / bottom_layer_direction; < 0 = not set
    std::string rotate_template;                            // solid_infill_rotate_template
    bool        align_to_model         = false;             // align_infill_direction_to_model
    double      model_rotation_deg     = 0.;                // the object's rotation about Z (used when aligned)
};

// How the skin's lines run (Fill.cpp: a set top/bottom_layer_direction fixes the angle; else a plain
// rotate template ("0,90") gives each layer its absolute angle; else the solid infill direction,
// turned 90 degrees on odd layers for every line pattern but aligned rectilinear). A template in the
// rotate metalanguage ("+30", "45%", ...) is not modelled: Isotropic.
SkinTangent skin_tangent_mode(const SkinSettings &settings);

} // namespace Showcase

class DynamicPrintConfig;

namespace Showcase {

// The top (`top` true) or bottom skin's settings from an object's full print config.
SkinSettings skin_settings(const DynamicPrintConfig &config, bool top, double model_rotation_deg);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseMaterial_hpp_
