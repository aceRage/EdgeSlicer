#ifndef slic3r_GCode_HeadCollisionCheck_hpp_
#define slic3r_GCode_HeadCollisionCheck_hpp_

// EdgeSlicer: check-only print-head collision pass over the processed G-code moves.
//
// Replays the toolpath (GCodeProcessorResult::moves) in print order into a height map of the
// material already laid down, and sweeps the head described by a HeadClearance along every travel,
// wipe and extrusion move. A move "hits" where already printed material rises into the head
// volume - under the nozzle cone or into a box - by more than a tolerance. It never changes the
// G-code: the result is reported as a slicing warning.
//
// Meant for Z contouring (ZAA), whose moves dip and rise inside a layer, and for the travels that
// cross contoured material. Planar printing cannot trigger it: material never rises above the tip
// of the move that is laying the current layer, so the coarse tile test rejects every sample.
//
// Idea after PancakeSlicer's G-code clearance verifier (GCode/ClearanceVerifier.cpp, AGPL-3.0,
// https://github.com/ZimengXiong/PancakeSlicer); this is a smaller, independent implementation.

#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "GCodeProcessor.hpp"

namespace Slic3r {

struct HeadClearance;

struct HeadCollisionHit
{
    unsigned int layer_id { 0 };   // 0-based, as GCodeProcessorResult::MoveVertex::layer_id
    unsigned int gcode_id { 0 };   // G-code line of the move
    Vec3f        position { Vec3f::Zero() }; // tip position where the deepest contact of the move is
    float        depth { 0.f };    // how far material reaches into the head volume, mm
    bool         travel { false }; // travel / wipe (true) or extrusion (false)
    std::string  part;             // "nozzle" or the box name
};

// One head part's contacts over the whole check (curved tops P3).
struct HeadCollisionPartStat
{
    std::string part;           // "nozzle tip" (underside within the tolerance of the tip), "nozzle flank", or a box name
    size_t      hit_moves { 0 }; // moves where this part touches
    float       max_depth { 0.f };
};

struct HeadCollisionReport
{
    bool   ran { false };
    size_t moves_checked { 0 };
    size_t hit_moves { 0 };  // moves with at least one contact
    float  max_depth { 0.f };
    // The first contact of the first hit moves, in print order, at most HeadCollisionParams::max_reported.
    std::vector<HeadCollisionHit> hits;
    // Every head part in order (nozzle tip, nozzle flank, then the boxes), with zero counts where it never touches.
    std::vector<HeadCollisionPartStat> parts;
    // Only with HeadCollisionParams::measure_clearance_from_layer: over every move on those layers, the
    // smallest gap between a head box's bottom and the material already printed under it (negative =
    // the box is in material), which box, and the tip position. +infinity when nothing was under a box.
    double                        min_box_clearance { std::numeric_limits<double>::infinity() };
    std::string                   min_box_clearance_part;
    Vec3f                         min_box_clearance_position { Vec3f::Zero() };
    // Curved tops P4, only with HeadCollisionParams::nozzle_contact_tolerance > 0: moves whose only contact is
    // the nozzle (tip or flank), no deeper than that tolerance. They are not hits (hit_moves, max_depth, hits
    // leave them out); they are counted here as information. Every head part's own counts (parts) still
    // include them.
    size_t tolerated_moves { 0 };
    float  tolerated_max_depth { 0.f };
};

struct HeadCollisionParams
{
    // Contact shallower than this is ignored: neighbouring beads of one layer legitimately touch the
    // nozzle land, and ZAA moves its Z in steps of about this size.
    double tolerance { 0.1 };
    // Height map cell (mm). 0 = automatic: 0.1 mm, coarser on a very large print area.
    double cell { 0. };
    size_t max_cells { 4000000 };
    size_t max_reported { 20 };
    // Curved tops: the steepest slope (degrees) of the surface the curved block actually follows. For
    // extrusion moves on layers from extrusion_surface_slope_min_layer on (the block's layer), material
    // within the nozzle's tip land - where the underside is no more than `tolerance` above the tip -
    // rising no more than this slope from the tip is the surface being followed (the neighbouring bead
    // of a sloped line), not a contact. Travels, wipes, the cone beyond the land and the boxes are never
    // relaxed. 0 (the default, and always for Z contouring only prints) = off.
    double       extrusion_surface_slope_deg { 0. };
    unsigned int extrusion_surface_slope_min_layer { 0 };
    // Curved tops: also measure HeadCollisionReport::min_box_clearance over the moves on layers from
    // this one on. The default (the largest value) measures nothing and changes nothing.
    unsigned int measure_clearance_from_layer { std::numeric_limits<unsigned int>::max() };
    // Curved tops P4: the nozzle's own contact tolerance (curved_tops_nozzle_tolerance). On layers from
    // nozzle_contact_min_layer on, an extrusion, a wipe, or a vertical travel (a descent or lift at one XY
    // point) whose contacts are all the nozzle tip or flank, each no deeper than this, against material standing
    // no more than this above the tip (so the cone never ploughs into a taller wall), is tolerated (counted
    // in HeadCollisionReport::tolerated_moves) instead of being a hit. A contact by any other head part (sock,
    // heater block, ducts, boxes) makes the move a hit as before, and so does any horizontal travel. 0 (the
    // default, and always for Z contouring) = off: the check is exactly what it was.
    double       nozzle_contact_tolerance { 0. };
    unsigned int nozzle_contact_min_layer { std::numeric_limits<unsigned int>::max() };
};

HeadCollisionReport check_head_collisions(const std::vector<GCodeProcessorResult::MoveVertex> &moves,
                                          const HeadClearance                                  &head,
                                          const HeadCollisionParams                            &params = {},
                                          const std::function<void()>                          &throw_if_canceled = {});

} // namespace Slic3r

#endif // slic3r_GCode_HeadCollisionCheck_hpp_
