#ifndef slic3r_Showcase_ShowcaseTone_hpp_
#define slic3r_Showcase_ShowcaseTone_hpp_

// Showcase realism 1a (tests/design_showcase_realism.md 2.1): the tone curves of the post pass.
//
// The lit objects are drawn into a floating-point (RGBA16F) layer as linear light; one post pass
// (resources/shaders/140/showcase_post.fs) applies the exposure, the tone curve and the sRGB
// encoding, and lays the result over the backdrop, plate and floor. The GLSL there and in
// showcase_material.fs and showcase_background.fs (the floor's mirror pass and the environment
// backdrop still encode in the shader) mirrors the functions here; the unit tests check them.
//
// GL-free, so the unit tests can check the curves and the exact inverse.

#include <array>
#include <string>

namespace Slic3r {
namespace Showcase {

// D-R1 (owner, 2026-10-26): Khronos PBR Neutral stays the default, a filmic curve is an option.
// The JSON names (to_string) are stable; never translate them.
enum class ToneCurve : int { Neutral = 0, Filmic };

const char *to_string(ToneCurve v);
bool        from_string(const std::string &s, ToneCurve &v);

using Rgb = std::array<float, 3>;

// Khronos PBR Neutral: linear scene light to linear display light in [0, 1). Keeps a filament's
// colour under a white light: below ~0.76 it only lifts the darkest channel's toe.
Rgb tone_neutral(Rgb c);
// The curve has no inverse over the whole display range: its desaturation means a saturated colour
// brighter than ~0.76 is never an output (pure blue at 255 cannot be made). So anything already lit
// and encoded for the display (the toolpaths' Preview lighting, the backdrop, the floor) stays in
// the display layer instead of being pushed back through the curve.

// A filmic curve in the style of AgX: a log2 encoding over 16.5 stops, a sigmoid with a soft
// shoulder and toe, and the AgX inset / outset matrices that desaturate the brightest highlights
// towards white instead of clipping them per channel. Linear scene light to linear display light.
Rgb tone_filmic(Rgb c);

Rgb tone_map(ToneCurve curve, const Rgb &c);

// The sRGB transfer functions (IEC 61966-2-1), per channel.
float srgb_to_linear(float c);
float linear_to_srgb(float c);   // clamps to [0, 1]

// What an IEEE half float (the RGBA16F layer) keeps of `v`: round to nearest even, 10 mantissa bits.
float round_to_half(float v);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseTone_hpp_
