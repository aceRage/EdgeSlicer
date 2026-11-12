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

struct HeadCollisionReport
{
    bool   ran { false };
    size_t moves_checked { 0 };
    size_t hit_moves { 0 };  // moves with at least one contact
    float  max_depth { 0.f };
    // The first contact of the first hit moves, in print order, at most HeadCollisionParams::max_reported.
    std::vector<HeadCollisionHit> hits;
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
};

HeadCollisionReport check_head_collisions(const std::vector<GCodeProcessorResult::MoveVertex> &moves,
                                          const HeadClearance                                  &head,
                                          const HeadCollisionParams                            &params = {},
                                          const std::function<void()>                          &throw_if_canceled = {});

} // namespace Slic3r

#endif // slic3r_GCode_HeadCollisionCheck_hpp_
