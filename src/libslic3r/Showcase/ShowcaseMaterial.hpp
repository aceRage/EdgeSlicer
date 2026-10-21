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

private:
    std::vector<float> m_tops;
    bool               m_uniform = true;
};

// Uniform layers without a table: the first layer, then `layer_height` steps (mirrored in GLSL).
LayerSpan uniform_layer_at(float z, float first_layer_height, float layer_height);
// 0 at the bottom of the layer, 1 at its top.
float     layer_phase(const LayerSpan &span, float z);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseMaterial_hpp_
