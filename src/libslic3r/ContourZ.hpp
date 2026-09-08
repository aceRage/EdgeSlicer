#ifndef slic3r_ContourZ_hpp_
#define slic3r_ContourZ_hpp_

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Point.hpp"

namespace Slic3r {

// EdgeSlicer: Z contouring, a.k.a. Z anti-aliasing ("ZAA").
//
// Ported from OrcaSlicer's src/libslic3r/ContourZ.cpp (PR #12736, merged 2026-05-02, plus the
// follow-up fixes #13450/#13452/#13508/#13510/#13766 which are already folded into upstream main).
// See docs/superpowers/specs/2026-09-07-z-contouring-port.md.
//
// THE MINIMAL VARIANT
// -------------------
// Upstream changed ExtrusionPath::polyline from the 2-D Polyline to a Polyline3 so that every
// extrusion point carries its own Z. This fork sits on the pre-ZAA (Orca 2.3.x) geometry and
// keeps the 2-D Polyline. Instead, a contoured ExtrusionPath holds a shared, immutable
// ContourZSamples: the very same resampled points the contouring pass produced, each with the
// Z delta that was raycast for it.
//
// Keeping the samples in a SEPARATE, IMMUTABLE, SHARED object (rather than a parallel vector
// indexed by polyline point) is what makes the minimal variant safe. Between posContouring and
// G-code emission the polyline is still reordered, rotated, split at the seam and clipped
// (ExtrusionLoop::split_at / split_at_vertex / clip_end, ExtrusionPath::reverse). All of those
// either preserve the point values or introduce at most one interpolated point. Because the
// lookup is keyed on the point's COORDINATES and not on its index, none of them can desynchronise
// it, and none of them needed to be touched.
struct ContourZSamples
{
    // Scaled XY, in the order the contouring pass produced them. These are exactly the points the
    // contoured ExtrusionPath's polyline was given, so an exact lookup hits for every point that
    // survived to the G-code emitter unchanged.
    Points             points;
    // Z delta in mm, relative to the path's OWN base Z, i.e. relative to
    //     print_z + path.z_offset * path.height
    // (offset_layers raises odd walls by half a layer; the contour rides on top of that base -
    // see the clamp in ContourZ.cpp and the emitter in GCode.cpp).
    std::vector<float> z;

    // Build the coordinate index. Must be called once, after points/z are filled.
    void build_index();

    // Z delta for p. Exact hit on an original sample; otherwise the value is interpolated along
    // the nearest sample segment (this only happens for the single point a seam split or a
    // clip_end introduces).
    float z_at(const Point &p) const;

    bool   empty() const { return this->points.empty(); }
    size_t size() const { return this->points.size(); }

private:
    struct KeyHash
    {
        std::size_t operator()(const std::pair<int64_t, int64_t> &k) const noexcept
        {
            std::size_t h = std::hash<int64_t>()(k.first);
            h ^= std::hash<int64_t>()(k.second) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<std::pair<int64_t, int64_t>, float, KeyHash> m_index;
};

using ContourZSamplesPtr = std::shared_ptr<const ContourZSamples>;

// ---------------------------------------------------------------------------------------------
// The tuning constants of the pass. All of them exist because of a measured artifact on a real
// slice; see docs/superpowers/specs/2026-09-07-z-contouring-port.md, "The review".
// ---------------------------------------------------------------------------------------------

// Resampling resolution along a path, mm. Upstream's value.
static constexpr double ZAA_SAMPLE_RESOLUTION_MM = 0.1;

// Upstream's tolerance above max_up inside which a sample still counts as a top surface. Above
// max_up upstream pins the delta to exactly max_up and then drops it to 0 the moment the mesh is
// one micron further up, i.e. a full max_up (50 um by default) step between two neighbouring
// samples. Here the delta fades from max_up to 0 across the same tolerance instead.
static constexpr double ZAA_TOP_TOLERANCE_MM = 0.03;

// The slope band above zaa_minimize_perimeter_height over which the -half_width*sin(slope)
// adjustment fades in (EdgeSlicer's guard for the still-open upstream bug OrcaSlicer#13552).
// Upstream applies it as a hard step. The fade is a smoothstep, so both the value and its
// derivative are continuous: the adjustment cannot change the wall's Z faster than the mesh's
// own slope changes. Measured on a 30 mm hemisphere, a 5 degree band still let the outer wall's
// per-layer bead height jump 39 um where the surface slope crossed the threshold; 20 degrees
// spreads the same 0.12 mm over enough layers that the jump falls under 10 um.
static constexpr double ZAA_SLOPE_RAMP_DEGREES = 20.0;

// Half width, in samples, of the symmetric moving average applied to a path's delta profile
// before it is written out. A straight ramp is a fixed point of a moving average, so the mesh
// following is untouched; what it removes is the sample-to-sample noise the slope term and the
// mesh facets inject, which is what reads as fuzzy skin on the outer wall.
static constexpr int ZAA_SMOOTH_RADIUS_SAMPLES = 3;

// How far a sample may sit off the chord between its neighbours and still be dropped as
// collinear. Upstream uses EPSILON (0.1 um), which was fine for a raw, piecewise-linear
// raycast profile but keeps nearly every sample of a smoothed one - the wedge's G-code grew
// 44 %. One micron is under the printer's Z resolution and holds the emitted profile to within
// a micron of the smoothed one.
static constexpr double ZAA_COLLAPSE_TOLERANCE_MM = 0.001;

// A path whose whole profile stays inside this band is left uncontoured. Below this the contour
// is under the printer's Z resolution but the path would still lose arc fitting and gain a Z
// word on every move. Applied per path, never per sample, so it can never introduce a step.
static constexpr double ZAA_MIN_PATH_DELTA_MM = 0.010;

// Everything the per-sample decision needs. Deliberately free of ExtrusionRole / LayerRegion so
// that the decision is a pure function of numbers and can be unit tested on its own
// (tests/libslic3r/test_contour_z.cpp).
struct ContourZSampleInput
{
    // Role, reduced to the two facts the rules care about.
    bool is_perimeter = false;
    bool is_ironing   = false;

    // The upward raycast from the slicing plane into the object mesh.
    bool   hit          = false;
    double hit_distance = 0.0;
    Vec3d  hit_normal   = Vec3d(0.0, 0.0, 1.0);

    // Layer geometry.
    double print_z = 0.0;
    double slice_z = 0.0;
    double height  = 0.2;

    // Config.
    double min_z                         = 0.05; // zaa_min_z
    double minimize_perimeter_height_deg = 0.0;  // zaa_minimize_perimeter_height

    // Path geometry. z_offset_mm is offset_layers' shift in mm, i.e. path.z_offset * height; the
    // returned delta is relative to print_z + z_offset_mm, NOT to print_z.
    double half_width  = 0.2;
    double z_offset_mm = 0.0;
};

// The Z delta for one sample, in mm, relative to the path's own base Z. See ContourZ.cpp.
double contour_z_sample_delta(const ContourZSampleInput &in);

// Symmetric moving average over a path's delta profile, window radius in samples, shrinking at
// the two ends. Every output is a convex combination of inputs, so a profile that satisfied the
// clamp still satisfies it and a profile that was everywhere <= 0 (a perimeter) stays <= 0.
void contour_z_smooth_profile(std::vector<double> &d, int radius);

// The single height correction the G-code emitter applies to the flow of a contoured segment.
// The local layer height is (height + z_diff); ironing keeps its configured flow.
inline double contour_z_extrusion_ratio(bool is_ironing, double height, double z_diff)
{
    if (is_ironing || height <= 1e-9)
        return 1.0;
    return (height + z_diff) / height;
}

} // namespace Slic3r

#endif // slic3r_ContourZ_hpp_
