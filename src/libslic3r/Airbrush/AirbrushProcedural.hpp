#ifndef slic3r_Airbrush_AirbrushProcedural_hpp_
#define slic3r_Airbrush_AirbrushProcedural_hpp_

// Airbrush P3 (tests/design_airbrush_paint.md, section 3.4 and row P3; D11): the Gradient tool and
// the procedural patterns. Both write into the colour field (AirbrushField::fill()), exactly where a
// brush would, and are then baked like a stroke - so grain, seed, mix steps and the cost meter all
// apply unchanged, and a fill is one undo step.
//
// Every fill is a scalar t(p) over the surface fed through a RAMP of N stops (2..AIRBRUSH_MAX_STOPS),
// each a filament (or 0 = the part's own colour, i.e. no spray):
//   * Linear   t = position along start -> end
//   * Radial   t = surface distance from the start / surface distance of the end, OVER THE SURFACE
//              (geodesic_distances(): Dijkstra over the mesh edges, exact on the flat patch round
//              the seed; an approximation that never jumps across a slot)
//   * Along Z  t = world height between the start and end heights (`up` is the world Z axis in mesh
//              space, so a scaled or rotated part still grades along the print direction)
//   * Noise    fBm value noise (stone, skin mottling)        t = 0.5 + contrast * (fbm - 0.5)
//   * Speckle  Worley spots (granite)                         t = 1 inside a spot, 0 outside
//   * Camo     low-frequency fBm cut into one hard band per stop
//   * Marble   sin(height / scale + contrast * fbm) veins along `up`
// The patterns are pure functions of mesh-space position and the seed: no UVs, no seams, and the same
// recipe on the same mesh always gives the same field (the determinism tests pin that).
//
// The recipe is a small string (AirbrushFillRecipe::to_string()) stored on the field, so a pattern
// can be re-applied (D11: baked into the field, parameters kept).

#include "AirbrushField.hpp"

#include <string>
#include <vector>

namespace Slic3r {
namespace Airbrush {

static constexpr size_t AIRBRUSH_MAX_STOPS = 6;

struct GradientStop
{
    // 1-based filament id (physical or mixed), or 0 = the part's own colour (nothing sprayed).
    int   filament = 0;
    // 0..1 along the ramp.
    float pos      = 0.f;
    bool  operator==(const GradientStop &r) const { return filament == r.filament && pos == r.pos; }
};

enum class FillKind : int {
    Linear = 0,
    Radial,
    AlongZ,
    Noise,
    Speckle,
    Camo,
    Marble,
    Count
};
const char *fill_kind_name(FillKind kind);
inline bool fill_kind_is_gradient(FillKind k) { return k == FillKind::Linear || k == FillKind::Radial || k == FillKind::AlongZ; }

struct AirbrushFillRecipe
{
    FillKind                  kind = FillKind::Linear;
    std::vector<GradientStop> stops;
    // Gradients, mesh space: the press and release points of the drag.
    Vec3f                     start = Vec3f::Zero();
    Vec3f                     end   = Vec3f::Zero();
    // World Z in mesh space (the third row of the mesh -> world linear map). Along Z and Marble.
    Vec3f                     up    = Vec3f::UnitZ();
    // Patterns: feature size in mesh mm, contrast, fBm octaves, speck amount (Speckle: share of the
    // surface covered, 0..1) and seed.
    float                     scale_mm = 8.f;
    float                     contrast = 1.f;
    int                       octaves  = 4;
    float                     amount   = 0.3f;
    uint32_t                  seed     = 1;
    // How much the fill replaces what is there (1) or glazes over it.
    float                     opacity  = 1.f;
    // Where it goes (hand test 2026-10-31): the whole part, or the brush area - the brush radius
    // round the drag from start to end (a click: a circle round it), soft at the rim by `hardness`;
    // for Radial the circle of the drag's radius round the centre. The brush area only takes
    // facets facing `facing` (the direction back to the viewer), as a spray does. Mesh mm.
    enum class Area : int { WholePart = 0, Brush = 1 };
    Area                      area         = Area::WholePart;
    float                     brush_radius = 4.f;
    float                     hardness     = 0.5f;
    Vec3f                     facing       = Vec3f::Zero();

    // Stops sorted by position, clamped to 0..1, at most AIRBRUSH_MAX_STOPS, filaments in range.
    // False when fewer than two stops remain.
    bool        sanitize();
    std::string to_string() const;
    static bool from_string(const std::string &s, AirbrushFillRecipe &out);
    bool        operator==(const AirbrushFillRecipe &r) const { return to_string() == r.to_string(); }
};

// Two stops, `a` at 0 and `b` at 1.
std::vector<GradientStop> two_stops(int a, int b);
// `n` stops evenly spread over the given filaments (camo).
std::vector<GradientStop> even_stops(const std::vector<int> &filaments);

// ---- the scalar functions (pure, exposed for the tests) ---------------------------------------

// Value noise in 0..1 (trilinear, smoothstep-faded lattice of hashed values).
float value_noise(const Vec3f &p, uint32_t seed);
// Fractal sum of `octaves` value noises, each twice the frequency and half the amplitude, in 0..1.
float fbm_noise(const Vec3f &p, int octaves, uint32_t seed);
// Distance from `p` to the nearest Worley feature point (one per unit cell), in cell units.
float worley_f1(const Vec3f &p, uint32_t seed);
// t(p) of a pattern (Noise, Speckle, Camo, Marble) at a mesh-space point. Gradients need the mesh:
// use make_fill_function().
float pattern_value(const AirbrushFillRecipe &recipe, const Vec3f &p);

// Coverage per stop index for ramp position t: two neighbouring stops share it linearly, or (hard)
// the nearer stop takes it all. Index = stop index.
std::vector<float> ramp_weights(const std::vector<GradientStop> &stops, float t, bool hard);

// Surface distance from `seed` to every vertex of `its`: Dijkstra over the mesh edges, started from
// the flat patch of facets round the seed (straight-line distances there, so a flat face is exact).
// `seed_patch`, when given, receives one flag per facet: in the flat patch round the seed.
std::vector<float> geodesic_distances(const indexed_triangle_set &its, const Vec3f &seed, std::vector<char> *seed_patch = nullptr);
// The surface distance of a point on `facet`, from the vertex distances.
float geodesic_at(const indexed_triangle_set &its, const std::vector<float> &vertex_dist, const std::vector<char> &seed_patch,
                  const Vec3f &seed, size_t facet, const Vec3f &p);

// ---- applying a recipe ------------------------------------------------------------------------

// The brush area as a weight 0..1 at a point (1 for the whole part): the dab falloff of the distance
// to the drag segment (to the centre for Radial). Facing is not part of it (see apply_fill()).
// `radial_radius`: a Radial gradient's extent over the surface (make_fill_function()).
float fill_area_weight(const AirbrushFillRecipe &recipe, const Vec3f &p, float radial_radius = 0.f);

// Builds the fill function: adds the stops' filaments to the field's slots (false, with `error`,
// when that would pass AIRBRUSH_MAX_SLOTS or the recipe is not usable).
// `radial_radius`, when given, receives a Radial gradient's extent: the surface distance from the
// start to the end of the drag (the last stop sits there).
bool make_fill_function(AirbrushField &field, const indexed_triangle_set &its, const AirbrushFillRecipe &recipe,
                        AirbrushField::FillFunction &out, std::string *error = nullptr, float *radial_radius = nullptr);

// make_fill_function() + AirbrushField::fill() + store the recipe on the field. Returns the number of
// cells changed, or -1 (with `error`) when the recipe could not be applied.
// `base`: blend from this field instead of `field` (a live preview into an empty_like() field).
long apply_fill(AirbrushField &field, const AirbrushSurface &surface, const AirbrushFillRecipe &recipe,
                std::vector<uint64_t> *changed_keys = nullptr, std::string *error = nullptr, const AirbrushField *base = nullptr);

} // namespace Airbrush
} // namespace Slic3r

#endif // slic3r_Airbrush_AirbrushProcedural_hpp_
