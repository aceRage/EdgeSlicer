#ifndef slic3r_CurvedTops_hpp_
#define slic3r_CurvedTops_hpp_

// EdgeSlicer: curved (non-planar) top surfaces. Experimental, off by default (curved_tops_enabled),
// and only on printers whose head clearance is known (curved_tops_head_clearance()).
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
// Scope: one object, one instance, one nozzle, no raft, no variable layer height. See
// tests/design_curved_tops.md for how it works, the limits and what is still to do.

namespace Slic3r {

class PrintConfig;
class PrintObject;
class TriangleMesh;
namespace sla { class IndexedMesh; }

// What the print head allows. A conformal layer is printed after the planar body below it, so
// the nozzle tip runs over a surface that rises around it:
//  - nozzle_height_mm: how far the nozzle tip reaches below everything else on the head (heater
//    block, silicone sock, fan duct). A curved surface may not be taller than this, otherwise the
//    head body would meet the high part of the surface while the tip prints the low part.
//  - max_slope_deg: the steepest slope the nozzle cone itself clears.
struct CurvedTopsHeadClearance
{
    double      nozzle_height_mm = 1.5;
    double      max_slope_deg    = 12.0;
    const char *source           = "generic conservative default";
    // False for the generic default: apply_curved_tops() leaves the object planar on a printer
    // whose clearance nobody has given.
    bool        known            = false;
};

// Hard-coded per printer model for now: the Elegoo Centauri Carbon and the Bambu Lab X1 Carbon.
// Every other printer gets the generic default with known = false. The next step is to take these
// from the head-clearance model (HeadClearance.hpp, head_clearance_for_printer_model(), data in
// resources/head_clearance), which the Z contouring head collision check already uses.
CurvedTopsHeadClearance curved_tops_head_clearance(const PrintConfig &config);

// The user's maximum angle, limited by the head.
double curved_tops_effective_max_angle(double user_max_deg, const CurvedTopsHeadClearance &head);

// Flow multiplier for a bead whose real height is `gap` while it was planned for `height`:
// the first conformal layer fills whatever the stepped planar body below left, between 1 and 2
// layer heights.
double curved_tops_gap_flow(double gap, double height);

// Clips the planar extrusions that sit inside the curved band and appends the conformal layers
// to the object's top layer. Called from PrintObject::contour_z() after Z contouring, only when
// curved_tops_enabled is set. Returns false (and leaves the object planar) when a guard refuses.
bool apply_curved_tops(PrintObject &object, const TriangleMesh &mesh, const sla::IndexedMesh &imesh);

} // namespace Slic3r

#endif // slic3r_CurvedTops_hpp_
