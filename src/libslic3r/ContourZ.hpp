#ifndef slic3r_ContourZ_hpp_
#define slic3r_ContourZ_hpp_

#include <cmath>
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

// ---------------------------------------------------------------------------------------------
// Constant-volumetric-flow speed scaling on contoured segments (zaa_speed_scaling).
//
// A contoured segment's bead is (height + d) tall while the role's feed rate F was chosen for a
// bead `height` tall. At 0.2 mm layers a segment can swing from the nominal 0.20 mm bead down to
// zaa_min_z (0.05 mm), a 4x drop in the material laid per mm at an unchanged F: the thin end
// starves and the Z axis is doing its fastest work exactly where the bead is thinnest. Holding
// the flow at what the role was tuned for at the nominal layer height means scaling F by the
// same height ratio the E was scaled by, so a quarter-height bead runs at a quarter speed.
// ---------------------------------------------------------------------------------------------

// Absolute floor for a scaled feed rate, mm/min (10 mm/s). A thin segment on a steep contour can
// ask for an arbitrarily small F; below roughly this the move stops being a print move and starts
// being a dwell that oozes, and the cooling/pressure-advance models downstream are not calibrated
// for it. Chosen as a fixed constant rather than a config key: it is a lower bound on sane
// behaviour, not a tuning knob, and every printer profile in the tree prints something at 10 mm/s.
static constexpr double ZAA_MIN_SPEED_MM_S = 10.0;

// Hysteresis band for the emitted F. Consecutive contoured segments whose local layer height is
// within this fraction of the height that set the current F keep that F.
//
// This matters more than it looks. The scaled F cannot be written into the movement G1 itself:
// CoolingBuffer only slows lines inside a ";_EXTRUDE_SET_SPEED" block and asserts that no G1
// inside one carries its own F (CoolingBuffer.cpp ~476), so an embedded F escapes the layer-time
// slowdown entirely - measured on the dome, contoured outer walls kept running at 9124 mm/min
// where cooling had slowed the rest of the layer to 2841. So each change costs its own `G1 F...`
// line, and without a deadband a smoothed contour profile - which changes by a micron or two per
// 0.1 mm sample - would emit one on almost every move for a correction under a percent.
//
// 5 % of the local height is under the quantisation the profile already carries (1 um on a 200 um
// layer is 0.5 %), and it bounds the flow error the hysteresis itself can introduce at exactly
// 5 %. Measured: it holds the F words to 2-3 % of contoured segments on the dome and 41-45 % on
// the wedge, whose contour is a continuous ramp.
static constexpr double ZAA_SPEED_HYSTERESIS = 0.05;

// The cooling marker a ZAA-scaled speed block carries, appended after ";_EXTRUDE_SET_SPEED".
//
// CoolingBuffer's layer-time slowdown has two regimes. When it only needs to stretch a layer a
// little it caps every adjustable line at a common ceiling feed rate (`slow_down_to_feedrate`),
// which is exactly the wrong operator for this feature: the whole point of zaa_speed_scaling is
// that the segments of one contoured path run at DIFFERENT speeds in a fixed ratio to their local
// bead height, and a common ceiling flattens that ratio to a constant. Measured on the dome at
// 0.2 mm: the 7029 contoured segments carry 205 distinct feed rates spanning 3825-9659 mm/min
// with cooling off, and collapse to 23 values spanning 1200-3453 with cooling on - whole layers
// pinned to a single F, which is the "the feature does not reach the G-code" symptom.
//
// A block carrying this marker is instead only ever slowed PROPORTIONALLY, by the same factor as
// the rest of its layer, so F_seg/h_seg stays constant while the layer still reaches its cooling
// target. See CoolingBuffer.cpp, CoolingLine::TYPE_ZAA_SCALED.
static constexpr const char *ZAA_COOLING_MARKER = ";_ZAA_SCALED";

// The factor a ZAA-scaled cooling block is slowed by when the layer-time slowdown is capping the
// ordinary adjustable lines at `cap_feedrate`.
//
// The ordinary rule is a CAP: every adjustable line above the ceiling is pinned to it. Applied to
// ZAA blocks that destroys the feature, because the whole point is that the segments of one
// contoured path run at different speeds in a fixed ratio to their local bead height.
//
// So ZAA blocks are scaled instead, by the ratio the cap represents for the layer: reference /
// cap, where `reference` is the fastest ordinary adjustable line - the speed the cap is really
// acting on. A ZAA block at any speed is divided by that same factor, so every F_seg/h_seg ratio
// is preserved exactly while the block contributes its share of the stretched layer time.
//
//   reference_feedrate  fastest ordinary (non-ZAA) adjustable feed rate on this extruder, mm/s;
//                       when a layer has none, the caller passes the fastest ZAA feed rate, which
//                       applies the same rule to the blocks themselves.
//   cap_feedrate        the ceiling the ordinary lines are being pinned to, mm/s.
//
// Returns 1.0 (no slowdown) when the cap is not actually biting.
inline double contour_z_cooling_factor(double reference_feedrate, double cap_feedrate)
{
    if (cap_feedrate <= 0.0 || reference_feedrate <= cap_feedrate)
        return 1.0;
    return reference_feedrate / cap_feedrate;
}

// The feed rate a ZAA-scaled block ends at once the layer-time slowdown has applied `factor`,
// never below the material's own minimum print speed. All speeds in the same unit.
inline double contour_z_cooled_feedrate(double f_seg, double factor, double min_speed)
{
    if (factor <= 1.0 || f_seg <= 0.0)
        return f_seg;
    return std::max(f_seg / factor, min_speed);
}

// The scaled feed rate for one contoured segment, mm/min.
//
//     F_seg = F_role * h_seg / h_nominal,  clamped to [floor, F_role] and then to the volumetric cap
//
// The segment's E has already been scaled by the same h_seg / h_nominal ratio, so scaling F by
// it too is what returns the extruder's melt rate - and the time the Z axis is given for each
// step - to what the role was tuned for at the nominal layer height. A half-height bead runs at
// half speed; a bead at the nominal height is untouched.
//
//   f_role_mm_min   the feed rate the role would have used, in mm/min (already carrying every
//                   dynamic slowdown this fork applies - overhang grading, small perimeters,
//                   resonance avoidance, the filament volumetric cap - because it is the F that
//                   was about to be emitted for this segment).
//   h_nominal       the path's nominal layer height, mm.
//   h_seg           the segment's local layer height, mm - height + the trapezoid mean of the two
//                   endpoints' contour deltas, i.e. exactly the height the E of this segment was
//                   computed from, so flow really is held constant.
//   max_vol_mm3_s   filament_max_volumetric_speed, mm3/s; <= 0 means no cap.
//   mm3_per_mm_seg  the segment's actual volumetric cross-section, mm3/mm, i.e. the path's
//                   _mm3_per_mm scaled by the same height ratio the E was scaled by.
//
// Never raises F above f_role_mm_min: a thick segment (d > 0, which only top surfaces get, and
// only inside the +zaa_min_z half of the band) would otherwise speed up past a role speed the
// user set deliberately. Only thin segments are slowed.
double contour_z_segment_feedrate(double f_role_mm_min,
                                  double h_nominal,
                                  double h_seg,
                                  double max_vol_mm3_s,
                                  double mm3_per_mm_seg);

// True when a segment whose local height is h_seg may keep an F that was set for a segment of
// local height h_ref. Both heights in mm. A non-positive h_ref means "no F set yet".
inline bool contour_z_speed_within_hysteresis(double h_ref, double h_seg)
{
    if (h_ref <= 0.0 || h_seg <= 0.0)
        return false;
    return std::abs(h_seg - h_ref) <= ZAA_SPEED_HYSTERESIS * h_ref;
}

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
