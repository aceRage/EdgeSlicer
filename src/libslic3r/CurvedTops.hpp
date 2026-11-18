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
// by layer, no raft, no variable layer height, on the printers whose head file records a curved
// print. See tests/design_curved_tops.md for how it works.
// P2 (part of it): supports and side stabilizers no longer refuse by themselves; the block is planned
// after them and refused only when they reach into the curved layers within the head's reach. A
// surface rising more than the head allows is curved over its upper part and printed flat below.

#include <memory>
#include <string>
#include <vector>

#include "ExPolygon.hpp"
#include "Flow.hpp"
#include "PrintConfig.hpp"

struct indexed_triangle_set;

namespace Slic3r {

class Print;
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
// Added to the head's reach when supports are checked against the curved block (design R2: nozzle
// radius + 0.5 mm).
static constexpr double CURVED_TOPS_REACH_MARGIN_MM     = 0.5;
// Travels in and out of the block run this far above the highest curved point.
static constexpr double CURVED_TOPS_SAFE_LIFT_MM        = 1.0;
// Curved tops on any printer: the head's slope limit when the user entered the nozzle protrusion on a printer
// with no head file and left the max surface angle automatic (design 2.2: "otherwise 10 deg").
static constexpr double CURVED_TOPS_USER_AUTO_ANGLE_DEG = 10.0;

// Curved tops on any printer (design 3.4, D3): the user accepts the risk of head values that no curved print has
// checked (an EdgeSlicer estimate, or their own nozzle protrusion) once per printer preset and head values. The
// acceptance is kept by the app (app settings, never a preset, a project or G-code) and handed to the slicer here;
// the key says what was accepted (curved_tops_acknowledgement_key), so a changed value asks again. The CLI has
// --curved-tops-accept-unverified instead (set_curved_tops_accept_unverified). Thread safe.
// App settings section the GUI keeps the accepted keys in (key = the acknowledgement key, value = when).
static constexpr const char *CURVED_TOPS_ACK_SECTION = "curved_tops_acknowledged";
void set_curved_tops_acknowledgements(const std::vector<std::string> &keys);
void add_curved_tops_acknowledgement(const std::string &key);
bool curved_tops_acknowledged(const std::string &key);
void set_curved_tops_accept_unverified(bool on);
bool curved_tops_accept_unverified();
// "<printer preset>|<printer model>|<head id>|lowest <mm>|rise <mm>|angle <deg>|contact <mm>": the head values in force
// for this printer config (resolved head, user values). Empty when there is no head at all.
std::string curved_tops_acknowledgement_key(const PrintConfig &config, const std::string &printer_preset);

// P3: developer overrides (curved_tops_dev_* process keys). They exist so the owner can find the real
// limits by test prints; they act only while the app's Developer mode is on (the GUI sets it from the
// app config, the CLI from --curved-tops-dev-overrides). With it off every override a project or preset
// carries is ignored, and the curved-tops message says so. Thread safe (an atomic).
void set_curved_tops_developer_mode(bool on);
bool curved_tops_developer_mode();

// The overrides in force for one object. Default-constructed = today's limits, no override.
struct CurvedTopsDevOverrides
{
    // The config sets at least one curved_tops_dev_* key off its default.
    bool   carried { false };
    // Carried and Developer mode is on: the values below are in force. When false they are the
    // built-in values, whatever the config says.
    bool   active { false };

    double angle_ceiling_deg   { CURVED_TOPS_MAX_ANGLE_CEILING };
    double head_lowest_part_mm { 0. }; // 0: from the head data
    double clearance_margin_mm { CURVED_TOPS_CLEARANCE_MARGIN_MM };
    double flank_margin_deg    { CURVED_TOPS_FLANK_MARGIN_DEG };
    double land_tolerance_mm   { CURVED_TOPS_LAND_TOLERANCE_MM };
    double reach_margin_mm     { CURVED_TOPS_REACH_MARGIN_MM };
    double speed_cap_mm_s      { 40.0 }; // CURVED_TOPS_MAX_SPEED_MM_S (ContourZ.hpp)
    double wipe_speed_cap_mm_s { 40.0 };
    double safe_lift_mm        { CURVED_TOPS_SAFE_LIFT_MM };
    bool   contact_warning_only { false };
    bool   allow_multiple_objects { false };
    bool   allow_by_object { false };
    bool   allow_multi_filament { false };
    bool   allow_supports_in_reach { false };
    bool   allow_material_beside { false };
    bool   allow_z_contouring { false };
    bool   allow_raft { false };
    bool   allow_variable_layer_height { false };
    bool   allow_tall_skirt { false };
    bool   allow_unverified_head { false };
    // P4: the nozzle contact tolerance applies on a head whose file records no nozzle contact print (and
    // above the record), up to the setting.
    bool   nozzle_tolerance_unrecorded { false };

    // "label value" of every override the config sets, in key order (used for the warning, the
    // G-code comment and the "ignored" note).
    std::vector<std::string> carried_list;
    // Values outside their documented range (clamped) or not a number (built-in used), one note each.
    std::vector<std::string> clamp_notes;

    // For the guard record: empty when nothing is carried, "ignored" when carried but not active,
    // else every value in force.
    std::string record() const;
    // "Curved tops developer overrides active: ..." (empty when not active).
    std::string active_message() const;
    // The note for a project whose overrides were ignored (empty unless carried and not active).
    std::string ignored_note() const;
};

// The overrides in force for an object with this config (developer_mode: the gate; the first form
// reads curved_tops_developer_mode()).
CurvedTopsDevOverrides curved_tops_dev_overrides(const PrintObjectConfig &config);
CurvedTopsDevOverrides curved_tops_dev_overrides(const PrintObjectConfig &config, bool developer_mode);

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
    // P4: deepest nozzle contact printed cleanly (0 = not recorded).
    double      verified_nozzle_contact_mm { 0. };
    std::string verified_on;
    // Where the numbers come from, for messages; why not usable, when not.
    std::string source;
    std::string refusal;
    // Curved tops on any printer: the head carries the user's own values (printer settings).
    bool        user_values { false };
    // The numbers are usable but no curved print checked them: curved tops runs only once the user accepted that
    // (acknowledged), or with the developer override.
    bool        needs_acknowledgement { false };
    bool        acknowledged { false };
    std::string acknowledgement_key;
    // The numbers the limits were derived with (built-in, or developer overrides).
    double      angle_ceiling_deg { CURVED_TOPS_MAX_ANGLE_CEILING };
    double      clearance_margin_mm { CURVED_TOPS_CLEARANCE_MARGIN_MM };
};

// The limits for a head on a printer model (head may be null: no data). `dev` (default: none) may
// widen them: angle ceiling, lowest head part, margins, a head nobody verified or no head data at all
// (then the lowest head part override is required).
// `acknowledged`: the user accepted the risk of a head no curved print has checked (see above).
CurvedTopsHeadLimits curved_tops_head_limits(const std::shared_ptr<const HeadClearance> &head, const std::string &printer_model,
                                             const CurvedTopsDevOverrides &dev = CurvedTopsDevOverrides(), bool acknowledged = false);
// Same, through resolve_head_clearance() (file and user values), acknowledged when the key for (config, printer_preset)
// was accepted or the CLI accepts every unverified head.
CurvedTopsHeadLimits curved_tops_head_limits(const PrintConfig &config, const CurvedTopsDevOverrides &dev = CurvedTopsDevOverrides(),
                                             const std::string &printer_preset = std::string());
// Same, for a print: its printer preset name comes from the full config (printer_settings_id).
CurvedTopsHeadLimits curved_tops_head_limits(const Print &print, const CurvedTopsDevOverrides &dev = CurvedTopsDevOverrides());
// The printer preset name a print was configured with (printer_settings_id; empty when unknown).
std::string curved_tops_printer_preset(const Print &print);

// The user's maximum angle, limited by the head and the absolute ceiling (head.angle_ceiling_deg).
double curved_tops_effective_max_angle(double user_max_deg, const CurvedTopsHeadLimits &head);

// How far from the nozzle axis (mm, in XY) any part of the head reaches below `height` above the tip:
// the nozzle outline where its underside is lower than `height`, and every head box whose bottom is
// lower than it, taken as the circle round the axis through its farthest corner. +infinity when a box
// spanning the whole X range (a gantry) is that low. Used to decide whether support material standing
// `height` above the lowest curved tip is close enough to the curved surface to be met.
double curved_tops_head_reach_mm(const HeadClearance &head, double height);

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
// curved_tops_enabled is set. For such an object Print::process() runs that step after the support
// step, so the object's support layers (supports, side stabilizers) are final here and are checked
// against the block. A refusal leaves the object planar and says why.
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

// ---- P4: the patch boundary, the fill patterns, the nozzle's contact tolerance ----

// The edge of a curved patch is where the surface reaches the maximum angle. The surface's slope is taken
// per vertex (the area-weighted normal of the facets round it that are within CURVED_TOPS_CREASE_DEG of
// each other, so a sharp edge such as a cap's rim on a vertical wall does not tilt it) and interpolated
// across each facet, and every facet is cut where that interpolated slope reaches the limit, instead of
// being taken whole or not at all. The edge then runs smoothly across the facets rather than along their
// sides. A facet steeper than the limit by more than CURVED_TOPS_EDGE_ANGLE_TOLERANCE_DEG never
// contributes; one within it may contribute the part of it on the shallow side of the line (so the patch
// may reach onto facets up to that much steeper than the limit, along its edge only). The outline is then
// opened by CURVED_TOPS_EDGE_OPENING_MM (slivers narrower than twice that go; nothing is added) and
// simplified with a deviation of at most CURVED_TOPS_EDGE_SIMPLIFY_MM (the only step that may move the
// edge outward, by at most that much in XY).
static constexpr double CURVED_TOPS_CREASE_DEG               = 30.0;
static constexpr double CURVED_TOPS_EDGE_ANGLE_TOLERANCE_DEG = 2.0;
static constexpr double CURVED_TOPS_EDGE_OPENING_MM          = 0.2;
static constexpr double CURVED_TOPS_EDGE_SIMPLIFY_MM         = 0.05;
// Slack added to the nozzle contact tolerance in the head check for the G-code's 0.001 mm rounding and the
// height map's float sums.
static constexpr double CURVED_TOPS_NOZZLE_TOLERANCE_SLACK_MM = 0.005;

// The nozzle contact tolerance in force for an object (review M2): the setting, capped by the deepest nozzle
// contact the print head's file records from a clean print on this printer model, and 0 (the strict check) on
// a head with no such record. The developer override nozzle_tolerance_unrecorded lifts the cap. `note` says
// when the setting is capped or not applied, or applied by the override (empty otherwise).
struct CurvedTopsNozzleTolerance
{
    double      asked { 0. };  // the setting, clamped to 0..1
    double      used { 0. };   // what the planner and the head check use
    double      record { 0. }; // the head's printed record (0 = none)
    std::string note;
};
CurvedTopsNozzleTolerance curved_tops_nozzle_tolerance(const PrintObjectConfig &config, const CurvedTopsHeadLimits &head,
                                                       const CurvedTopsDevOverrides &dev);

// One top-facing facet's part inside the patch (print frame: Z = mesh Z - ground).
struct CurvedTopsFacet
{
    Polygon xy;               // the part of the facet inside the patch, projected, CCW, scaled
    double  z_min { 0. }, z_max { 0. };
    Point   probe;            // a point inside xy
    double  slope_deg { 0. }; // the facet's own slope
};

struct CurvedTopsCandidates
{
    std::vector<CurvedTopsFacet> facets;
    // The patch outlines: the union of the facets' parts, healed, opened and simplified.
    ExPolygons                   pieces;
};

// The candidate surfaces of a mesh: every facet part that faces up within max_angle_deg (see above) and has
// nothing above it (a ray up from `probe` misses the mesh). `ground`: print Z = mesh Z - ground.
// smooth_edge false gives P3's footprint (whole facets under the limit), for comparisons.
CurvedTopsCandidates curved_tops_candidate_surfaces(const indexed_triangle_set &its, const sla::IndexedMesh &imesh, double max_angle_deg,
                                                    double ground, bool smooth_edge = true);

// One path of a curved layer's fill, before Z is put on it.
struct CurvedTopsFillPath
{
    Points points;
    float  width { 0.f };
    double mm3_per_mm { 0. };
    bool   wall { false }; // the layer's wall loop (curved_tops_layer_paths)
    bool   gap { false };  // P5: a gap-fill bead (curved_tops_layer_paths)
};

// P5: how many paths of a layer start somewhere else than where the path before ended (each one is a retract, a lift
// to the safe Z, a level travel and a drop in the curved block). The first path counts.
size_t curved_tops_path_starts(const std::vector<CurvedTopsFillPath> &paths);

// The fill of curved layer k (0 = lowest) inside `area` (the region inside the wall loop, as the planar
// solid fill would get it), for the pattern:
//  - Monotonic: lines at 45 degrees (k even) or 135 (k odd), each laid next to the one before in one
//    direction (the monotonic filler of the planar top surfaces);
//  - Concentric: loops following the outline inward (Arachne, variable width where the loops close up),
//    each closed loop starting at a seam spread along its length from layer to layer and loop to loop (see
//    curved_tops_seam_index), so the starts do not line up; a hole Arachne leaves in the middle (the apex
//    of a dome) that is too small for a loop gets one short line through it;
//  - Spiral (P5): the same insets of the outline, taken at one line spacing each, joined into one continuous
//    path running inward (see curved_tops_layer_paths for why inward): over each turn the path moves from one
//    inset to the next a little at a time, so there is no seam and no step between the rings. Where the insets
//    split (a narrow neck) or a hole's insets meet the outline's, the spiral ends and each part gets its own.
//    Here (no wall loop to lead in from) each spiral starts with a full turn on its outer inset.
// pcfg / ocfg: for the Arachne parameters (nozzle diameter). *failed (when given) is set when the filler threw
// or left a region of more than 1 mm2 with no path at all: the caller must not print that layer half built.
std::vector<CurvedTopsFillPath> curved_tops_fill(const ExPolygon &area, CurvedTopsPattern pattern, const Flow &solid_flow, int k,
                                                 const PrintConfig &pcfg, const PrintObjectConfig &ocfg, bool *failed = nullptr);

// Every path of curved layer k over the patch part `fp`: the wall loop (seams spread along it), the fill of the
// region inside it in the pattern (curved_tops_fill), then gap fill: variable-width beads along the medial axis
// of whatever the two leave open that is at least curved_tops_min_gap_width() wide (the spine of a narrowing
// limb, wedges at sharp loop corners, the strip between wall and fill), and one short line through a compact
// hole with no axis (a dome's apex). *failed as for curved_tops_fill.
// Spiral: each wall loop leads straight into the spiral inside it (one path, no travel between them): the spiral's
// first turn starts where the wall loop ends and runs from the wall to the first inset with its flow rising from
// nothing, the turns follow, and the last turn runs on the innermost inset with its flow falling to nothing, so
// both ends neither overlap nor leave a gap. Inward because the curved tops are domes and ridges: the nozzle then
// moves from the low edge up, the bead it lays next to is the one just laid below it, and the end point lies in
// the middle, where the centre's gap fill covers it, rather than at the patch edge.
// Gap fill (P5): Concentric and Spiral always; Monotonic only with a nozzle contact tolerance in force
// (nozzle_tolerance > 0): its beads squeezed into the wedges at the line ends touch the neighbouring lines by about
// 0.1 mm, which only the tolerance allows (with none, the strict head check refuses them; P4's behaviour is kept). With a
// tolerance the Monotonic lines also reach CURVED_TOPS_MONOTONIC_OVERLAP of a spacing over the wall loop's inner edge.
// Gap beads are joined to the path before them where they start near its end, and a bead that would print on its own
// is left out below CURVED_TOPS_GAP_MIN_ISOLATED_MM (owner, P5 prints: isolated short beads were dabs).
// *stats (when given) adds up what the gap fill did.
struct CurvedTopsGapStats
{
    size_t candidates { 0 };            // gap beads found
    double candidate_mm3 { 0. };        // their volume
    size_t attached { 0 };              // printed in one motion with the path before them (no travel)
    size_t isolated { 0 };              // printed on their own (a travel before them)
    size_t dropped { 0 };               // too short to print on their own: left out
    double kept_mm3 { 0. };             // volume printed, connecting pieces included
    double shortest_isolated_mm { 0. }; // the shortest bead printed on its own (0: none)
};
// A gap bead starting within this many line spacings of where the path before it ends is joined to it.
static constexpr double CURVED_TOPS_GAP_ATTACH_SPACINGS = 2.0;
// The connecting piece's flow, as a fraction of the fill's (it runs over beads already laid).
static constexpr double CURVED_TOPS_GAP_CONNECTOR_FLOW = 0.15;
// A gap bead printed on its own is at least this long.
static constexpr double CURVED_TOPS_GAP_MIN_ISOLATED_MM = 0.5;
std::vector<CurvedTopsFillPath> curved_tops_layer_paths(const ExPolygon &fp, CurvedTopsPattern pattern, const Flow &wall_flow,
                                                        const Flow &solid_flow, int k, const PrintConfig &pcfg,
                                                        const PrintObjectConfig &ocfg, bool *failed = nullptr,
                                                        double nozzle_tolerance = 0., CurvedTopsGapStats *stats = nullptr);
// P5: with a nozzle contact tolerance in force, the Monotonic fill overlaps the wall loop by this fraction of a spacing.
static constexpr double CURVED_TOPS_MONOTONIC_OVERLAP = 0.25;
// Narrowest gap a bead is laid in: a quarter of the nozzle diameter.
double curved_tops_min_gap_width(const PrintConfig &pcfg);
// The area (mm2) of `fp` the paths leave open, slivers narrower than min_width not counted (each path covers its
// centre line grown by half its spacing, from its flow at `height`).
double curved_tops_uncovered_mm2(const ExPolygon &fp, const std::vector<CurvedTopsFillPath> &paths, double height, double min_width);

// The point of `loop` (closed, first point not repeated) where curved layer k's loop number `index` starts. The
// loop is measured from a fixed anchor (its vertex with the smallest X, then Y), and the start is the vertex
// nearest the fraction frac((3k + index) * 0.618...) of its length: consecutive layers and loops are spread
// along the whole loop, also on a long narrow shape, where a direction from the centre would land on its two
// ends. (`centre` is kept for the callers; it no longer matters.)
size_t curved_tops_seam_index(const Points &loop, const Point &centre, int k, int index);

// True when a setting alone (not the mesh or the slice) makes apply_curved_tops() refuse this object.
// Print::apply() leaves a refused object (planar, walls not cut) alone while this holds.
bool curved_tops_blocked_by_settings(const PrintObject &object, int multi_filament = -1);

} // namespace Slic3r

#endif // slic3r_CurvedTops_hpp_
