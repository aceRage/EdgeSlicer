#ifndef slic3r_CurvedTops_hpp_
#define slic3r_CurvedTops_hpp_

// EdgeSlicer: curved (non-planar) top surfaces. Experimental, off by default (curved_tops_enabled).
// The print head comes from the head clearance model (HeadClearance.hpp, resolve_head_clearance()),
// the one source the Z contouring collision check uses too. No head data = no curved tops.
//
// The idea and the surface selection rules (top-facing facets under a maximum angle, grouped
// into surfaces, rejected when too small or taller than the nozzle clearance, deferred conformal
// layers printed on top of a lowered planar body) follow fich-l/OrcaSlicer-Nonplanar v0.1
// (AGPL-3.0), itself after Daniel Ahlers' non-planar Slic3r work, with ZimengXiong/PancakeSlicer's
// port (AGPL-3.0, engine/src/libslic3r/Nonplanar/CurvedTops*.cpp) read as a reference. No code
// was copied: both sources sit on a Polyline3 geometry this fork does not have, so this is a
// re-implementation on top of EdgeSlicer's Z contouring (ContourZ.hpp), whose per-point Z
// samples, emitter and guards it reuses.
//
// Scope (P1 of tests/design_curved_tops_any_printer.md): one object, one instance, one filament,
// by layer, no raft, no supports, no variable layer height, on the printers whose head file records
// a curved print. See tests/design_curved_tops.md for how it works.

#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

class PrintConfig;
class PrintObject;
class TriangleMesh;
struct HeadClearance;
namespace sla { class IndexedMesh; }

// Margin taken off the head's lowest part: a curved surface may rise at most (lowest part - margin).
// The spike's "2.5 mm measured, 2.0 mm used". A process key in P2 (curved_tops_clearance_margin).
static constexpr double CURVED_TOPS_CLEARANCE_MARGIN_MM = 0.5;
// Taken off the nozzle flank's angle for the head's angle cap.
static constexpr double CURVED_TOPS_FLANK_MARGIN_DEG    = 5.0;
// Absolute ceiling for the surface angle, whatever the user or the head says. The design's D8
// allows 30; the lead kept #1021's 20 for P1 (2026-11-16): a hardening phase must not loosen a
// limit, and raising it waits for a steeper verified print.
static constexpr double CURVED_TOPS_MAX_ANGLE_CEILING   = 20.0;
// A nozzle profile point this close above the tip is part of the tip land (the collision check's
// default tolerance, HeadCollisionParams::tolerance).
static constexpr double CURVED_TOPS_LAND_TOLERANCE_MM   = 0.1;

// What the print head allows a curved top, derived from the resolved head model. A conformal layer
// is printed after the planar body below it, so the nozzle tip runs over a surface that rises
// around it:
//  - allowed_rise_mm: a surface may rise at most this much (lowest head part above the tip minus
//    the margin), otherwise the head body would meet the high part of the surface while the tip
//    prints the low part;
//  - max_slope_deg: the steepest slope the nozzle flank clears, minus a margin, at most CURVED_TOPS_MAX_ANGLE_CEILING (20).
struct CurvedTopsHeadLimits
{
    std::shared_ptr<const HeadClearance> head;
    // A head model exists for this printer.
    bool        known { false };
    // Curved tops may run: known, with usable numbers, and (in P1, until the acknowledgement of P2)
    // the head file records a curved print on this printer model.
    bool        usable { false };
    double      allowed_rise_mm { 0. };
    double      max_slope_deg { 0. };
    // Verified on hardware for this printer model (0 = not verified).
    double      verified_rise_mm { 0. };
    double      verified_angle_deg { 0. };
    std::string verified_on;
    // Where the numbers come from, for messages; why not usable, when not.
    std::string source;
    std::string refusal;
};

// The limits for a head on a printer model (head may be null: no data).
CurvedTopsHeadLimits curved_tops_head_limits(const std::shared_ptr<const HeadClearance> &head, const std::string &printer_model);
// Same, through resolve_head_clearance().
CurvedTopsHeadLimits curved_tops_head_limits(const PrintConfig &config);

// The user's maximum angle, limited by the head and the absolute ceiling.
double curved_tops_effective_max_angle(double user_max_deg, const CurvedTopsHeadLimits &head);

// Flow multiplier for a bead whose real height is `gap` while it was planned for `height`:
// the first conformal layer fills whatever the stepped planar body below left, between 1 and 2
// layer heights.
double curved_tops_gap_flow(double gap, double height);

// What apply_curved_tops() did. Exactly one of `refusal` (the object stays planar) and `summary`
// (the block was built) is set when curved tops was asked for.
struct CurvedTopsResult
{
    bool                     applied { false };
    // Why the object is printed planar; empty when applied.
    std::string              refusal;
    // One line about the block: surfaces, rise and slope used against what is allowed, head data.
    std::string              summary;
    // Further notes for the user (surfaces left flat, beyond what has been printed, not ironed).
    std::vector<std::string> notes;
};

// Clips the planar extrusions that sit inside the curved band and appends the conformal layers
// to the object's top layer. Called from PrintObject::contour_z() after Z contouring, only when
// curved_tops_enabled is set. A refusal leaves the object planar and says why.
CurvedTopsResult apply_curved_tops(PrintObject &object, const TriangleMesh &mesh, const sla::IndexedMesh &imesh);

// True when the object's top layer carries a curved-tops block.
bool has_curved_tops_block(const PrintObject &object);

// Every value a guard of apply_curved_tops() reads (print, object, the other objects and copies),
// as one string. PrintObject::contour_z() records it; Print::apply() compares it after each change
// and regenerates the object's walls (and so re-plans the curved block) when it differs, because
// most of these settings do not invalidate posContouring by themselves. Each value is reduced to what
// decides a verdict (alone on the plate or not, one filament or more...), so changes that cannot change
// the outcome do not re-slice. multi_filament: Print::extruders(true).size() > 1 when the caller has it
// (computed once per apply), -1 to compute it here.
std::string curved_tops_guard_inputs(const PrintObject &object, int multi_filament = -1);

// True when a setting alone (not the mesh or the slice) makes apply_curved_tops() refuse this object.
// Print::apply() leaves a refused object (planar, walls not cut) alone while this holds.
bool curved_tops_blocked_by_settings(const PrintObject &object, int multi_filament = -1);

} // namespace Slic3r

#endif // slic3r_CurvedTops_hpp_
