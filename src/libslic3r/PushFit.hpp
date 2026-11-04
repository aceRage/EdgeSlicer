#ifndef libslic3r_PushFit_hpp_
#define libslic3r_PushFit_hpp_

// Push-fit "Pin" connector: a separate printed pin, pushed into a hole in each cut half.
// Design: tests/design_pushfit_connector.md (branch design/pushfit-connector).
//
// This file is pure geometry. It has no Model dependency, so the tests drive it directly; the
// cut (CutUtils), the 3MF and the gizmo wire it in later.
//
// THE CONNECTOR FRAME (the same as Flexi): the cut surface is z == 0, the pin axis is +Z (the
// connector normal), the UPPER half is z > 0 and the LOWER half is z < 0. With a kerf the
// halves' cut faces sit at z = +kerf_hi and z = -kerf_lo, and the pin spans the kerf.
// The pin's flats are perpendicular to the frame's Y axis, and its slit is the slab |x| < w/2,
// so the slit is perpendicular to the flats (the prongs stand side by side when the pin lies on
// a flat). The connector's z_angle turns all of this about the axis; that is the caller's job.
//
// All lengths are in mm.

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r.h"
#include "Point.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

enum class PushFitCutout : int { None = 0, Flat = 1, Slot = 2, FlatAndSlot = 3 };

inline bool pushfit_cutout_has_flat(PushFitCutout c) { return c == PushFitCutout::Flat || c == PushFitCutout::FlatAndSlot; }
inline bool pushfit_cutout_has_slot(PushFitCutout c) { return c == PushFitCutout::Slot || c == PushFitCutout::FlatAndSlot; }

// What the user asked for. Auto lies the pin on a flat when the flat is good enough to print on
// (see pushfit_can_lie) and stands it upright otherwise; Upright is the override.
enum class PushFitPose : int { Auto = 0, Upright = 1 };
// What the pin is actually built in.
enum class PushFitPrintPose : int { Lying = 0, Upright = 1 };

enum class PushFitSide : int { Upper = 0, Lower = 1 };

enum class PushFitPreset : int { S = 0, M = 1, L = 2, Custom = 3 };

struct PushFitParams
{
    // body
    float diameter{ 6.0f };         // D, nominal pin diameter
    float length_upper{ 7.0f };     // Lu, engagement into the upper half, from that half's cut face
    float length_lower{ 7.0f };     // Ll, engagement into the lower half
    float end_chamfer{ 0.5f };      // 45 degree lead-in at each pin end, axial = radial
    // lip: one per side, mirrored about the cut plane
    bool  lip{ true };
    float lip_height{ 0.25f };      // radial, beyond D/2
    float lip_width{ 1.0f };        // axial length of the lip base (crest = width - 2 x height)
    float lip_position{ 0.6f };     // lip centre as a fraction of the side's length, from the cut face
    // cutout
    PushFitCutout cutout{ PushFitCutout::FlatAndSlot };
    float flat_depth{ 0.9f };       // f, chord depth of the printing flat (0.15 D)
    bool  flat_both_sides{ true };  // a second flat opposite the first
    float slot_width{ 1.0f };       // w, slit width, through the pin
    float slot_length{ 4.0f };      // Ls, slit length from each pin tip (round end included)
    // holes
    float clearance_upper{ 0.10f }; // radial clearance, upper hole
    float clearance_lower{ 0.10f }; // radial clearance, lower hole
    float depth_clearance{ 0.4f };  // extra depth per hole beyond the pin end
    bool  hole_groove{ false };     // matching groove for the lip (detent) vs a plain hole
    float mouth_chamfer{ 0.4f };    // 45 degree flare at each hole mouth
    bool  pointed_end{ true };      // the hole ends in a 90 degree cone instead of a flat bridge
    // print
    PushFitPose pose{ PushFitPose::Auto };
    int   preset{ int(PushFitPreset::M) }; // UI only, stored so re-edit shows it

    float length(PushFitSide s) const { return s == PushFitSide::Upper ? length_upper : length_lower; }
    float clearance(PushFitSide s) const { return s == PushFitSide::Upper ? clearance_upper : clearance_lower; }

    bool operator==(const PushFitParams &o) const
    {
        return diameter == o.diameter && length_upper == o.length_upper && length_lower == o.length_lower &&
               end_chamfer == o.end_chamfer && lip == o.lip && lip_height == o.lip_height && lip_width == o.lip_width &&
               lip_position == o.lip_position && cutout == o.cutout && flat_depth == o.flat_depth &&
               flat_both_sides == o.flat_both_sides && slot_width == o.slot_width && slot_length == o.slot_length &&
               clearance_upper == o.clearance_upper && clearance_lower == o.clearance_lower &&
               depth_clearance == o.depth_clearance && hole_groove == o.hole_groove && mouth_chamfer == o.mouth_chamfer &&
               pointed_end == o.pointed_end && pose == o.pose && preset == o.preset;
    }
    bool operator!=(const PushFitParams &o) const { return !(*this == o); }

    // Same-binary cereal only (undo/redo), like the other connector parameter blocks.
    template<class Archive> void serialize(Archive &ar)
    {
        ar(diameter, length_upper, length_lower, end_chamfer, lip, lip_height, lip_width, lip_position, cutout,
           flat_depth, flat_both_sides, slot_width, slot_length, clearance_upper, clearance_lower, depth_clearance,
           hole_groove, mouth_chamfer, pointed_end, pose, preset);
    }
};

// ------------------------------------------------------------------ presets and sizing

// The S / M / L table (design 3.2). Fills the sized fields - diameter, both lengths, lip height
// and width, flat depth, slot width and length, both clearances, depth clearance and mouth
// chamfer - and the preset tag; every on/off choice (lip, cutout, groove, pose ...) is kept.
// Custom returns `p` unchanged.
PushFitParams pushfit_apply_preset(PushFitParams p, PushFitPreset preset);

struct PushFitAutoSize
{
    PushFitParams params;
    bool          fits{ true };   // false: nothing fitted, S was used anyway (warn the user)
};
// Pick the largest preset whose D <= 0.5 x inscribed_radius (the inscribed radius of the cut
// section at the connector) AND whose L + depth clearance + a 1.2 mm wall fits the half's
// thickness along the normal on both sides. A thickness <= 0 means "unknown" and is not checked.
PushFitAutoSize pushfit_auto_size(const PushFitParams &p, double inscribed_radius,
                                  double thickness_upper, double thickness_lower);

// ------------------------------------------------------------------ print pose

// The steepest overhang (degrees from vertical) of a round pin of diameter D lying on a flat of
// chord depth f: asin(1 - 2f/D). 90 when there is no flat.
double pushfit_lying_overhang_deg(const PushFitParams &p);
// The width of the printing flat (the chord), 0 without a flat.
double pushfit_flat_width(const PushFitParams &p);
// THE POSE RULE (owner, 2026-10-24): "print laying down if one of the pin sides is flat enough to
// do so". The pin lies on a flat when it has one, the flat keeps every overhang at or below 45
// degrees (f >= D (1 - sin 45) / 2, about 0.146 D) and the flat is a stable footprint (at least
// half the diameter wide, and 1.5 mm). Otherwise it prints upright. `why` gets the reason when
// the answer is no.
bool pushfit_can_lie(const PushFitParams &p, std::string *why = nullptr);
PushFitPrintPose pushfit_resolve_pose(const PushFitParams &p);

// ------------------------------------------------------------------ geometry

constexpr int    PUSHFIT_SECTORS          = 96;
// The hole volume starts this far OUTSIDE its half's cut face, so the hole opens on layer 1
// (OrcaSlicer #14512).
constexpr double PUSHFIT_HOLE_MOUTH_LEAD  = 0.2;
// The wall Auto sizing keeps behind a hole's end.
constexpr double PUSHFIT_MIN_WALL         = 1.2;

// The pin's (r, z) profile in the connector frame, before flats and slit. Exposed for tests and
// for the panel's side-profile inset.
std::vector<Vec2d> pushfit_pin_profile(const PushFitParams &p, double kerf_lo = 0., double kerf_hi = 0.);
// The hole's (r, z) profile in the connector frame. `face` is the distance of that half's cut
// face from z == 0 (the kerf on that side); the lower hole comes back already mirrored (z < 0).
std::vector<Vec2d> pushfit_hole_profile(const PushFitParams &p, PushFitSide side, double face = 0.);

enum class PushFitMeshFrame : int {
    Connector = 0,  // the connector frame (preview, overlap checks)
    PrintPose = 1,  // the resolved print pose: min z == 0, centred on x == y == 0
};

struct PushFitPinOptions
{
    double           kerf_lo{ 0. };
    double           kerf_hi{ 0. };
    PushFitMeshFrame frame{ PushFitMeshFrame::PrintPose };
    int              sectors{ PUSHFIT_SECTORS };
    // Test hook: pretend the slit boolean failed, to exercise the "no slit" fallback.
    bool             simulate_slot_boolean_failure{ false };
};

struct PushFitPinResult
{
    indexed_triangle_set mesh;
    PushFitPrintPose     pose{ PushFitPrintPose::Lying };
    bool                 slot_requested{ false };
    bool                 slot_made{ false };       // false with slot_requested: the boolean failed, no slit
    bool                 plain_fallback{ false };  // the checked mesh was bad, a plain flatted pin was used
    // In PrintPose frame: the transform that takes connector-frame points to the returned mesh.
    Transform3d          connector_to_mesh{ Transform3d::Identity() };
};

// The pin body: revolved profile with lips and end chamfers, the flats subtracted as half-space
// slabs (boolean chain: Manifold, mcut; cut_mesh() as the last resort), then the slit subtracted
// with a second boolean. If the slit boolean fails the pin comes back without a slit
// (slot_made == false); the cut never fails because of it.
PushFitPinResult pushfit_pin_mesh(const PushFitParams &p, const PushFitPinOptions &opt = {});

// The hole (negative volume) for one half, in the connector frame, in mm: round, radius
// D/2 + clearance_side, starting PUSHFIT_HOLE_MOUTH_LEAD outside the face, a 45 degree mouth
// flare, an optional lip groove, depth L_side + depth_clearance and a 90 degree cone (or a flat
// end). Not a scaled copy of the pin and never cut by the plane.
indexed_triangle_set pushfit_hole_mesh(const PushFitParams &p, PushFitSide side, double face = 0.,
                                       int sectors = PUSHFIT_SECTORS);

// The connector-frame pin for the gizmo preview (no kerf).
indexed_triangle_set pushfit_preview_body(const PushFitParams &p);

// ------------------------------------------------------------------ validation

// A blocking problem as a user-facing sentence, or "" when the pin can be built. `nozzle` sets
// the minimum prong width (two lines of 1.125 x nozzle).
std::string pushfit_validate(const PushFitParams &p, double nozzle_diameter = 0.4);

// Non-blocking warnings. A thickness <= 0 is "unknown" and not checked.
std::vector<std::string> pushfit_warnings(const PushFitParams &p, double nozzle_diameter = 0.4,
                                          double thickness_upper = 0., double thickness_lower = 0.);

// ------------------------------------------------------------------ storage

// Every field clamped to a range the geometry accepts, a non-finite value replaced by its
// default and the enums range-checked. For values read from a file (untrusted). Does not make
// an invalid combination valid - pushfit_validate() still decides that - it only bounds each
// value so nothing downstream sees a NaN, a negative length or an out-of-range enum.
PushFitParams pushfit_sanitize(PushFitParams p);

// The 3MF attributes of a pin: every field as ("pushfit_<field>", value), in a fixed order.
// Booleans and enums are written as integers. cut_information.xml and cut_recipe.xml share it.
std::vector<std::pair<std::string, double>> pushfit_attributes(const PushFitParams &p);

// The inverse: `get(name)` returns the attribute's value or nothing. A missing attribute keeps
// its default and the result goes through pushfit_sanitize().
PushFitParams pushfit_from_attributes(const std::function<std::optional<double>(const std::string &)> &get);

} // namespace Slic3r

#endif // libslic3r_PushFit_hpp_
