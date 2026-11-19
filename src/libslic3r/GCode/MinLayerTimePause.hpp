#ifndef slic3r_GCode_MinLayerTimePause_hpp_
#define slic3r_GCode_MinLayerTimePause_hpp_

// Edge: minimum layer time pause (process option min_layer_time_pause).
//
// The filament's cooling slowdown stretches a short layer only down to its minimum print speed, so a very
// small layer (the tip of a thin spike) can still finish too early. With min_layer_time_pause > 0, the
// G-code generator ends every eligible layer with a park-and-return block between the markers below; the
// CoolingBuffer, which knows the layer time after its slowdown, either fills the dwell (G4 P<ms> for the
// time still missing) or removes the block. The block leaves the writer state as it found it, so removing it
// leaves the G-code as it would have been without it.

#include "../libslic3r.h"
#include "../BoundingBox.hpp"
#include "../Polygon.hpp"
#include "../PrintConfig.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
namespace MinLayerTimePause {

// "<START> <target seconds>" opens the block, <DWELL> stands where the G4 goes, <END> closes it.
// They never reach the exported G-code.
inline constexpr const char *START_TAG = ";_MIN_LAYER_TIME_PAUSE_START";
inline constexpr const char *DWELL_TAG = ";_MIN_LAYER_TIME_PAUSE_DWELL";
inline constexpr const char *END_TAG   = ";_MIN_LAYER_TIME_PAUSE_END";
// Comment written before the G4 of a pause that was kept, for tools reading the G-code.
inline constexpr const char *COMMENT_TAG = ";MIN_LAYER_TIME_PAUSE";
// A shortfall below this is not worth a park-and-return.
inline constexpr float MIN_PAUSE_S = 0.5f;

// Clearance kept from objects, the prime tower and excluded areas, and from the bed edge.
inline constexpr double OBJECT_MARGIN = 10.;
inline constexpr double EDGE_MARGIN   = 5.;

// What the park position is chosen from, in bed coordinates (mm).
struct Sites
{
    BoundingBoxf               bed;            // bounding box of the printable area
    Polygon                    bed_polygon;    // printable area (scaled); empty: the box alone
    std::vector<BoundingBoxf>  objects;        // footprint of every object instance
    std::optional<BoundingBoxf> tower;         // prime tower footprint (brim included)
    std::vector<BoundingBoxf>  exclusions;     // bed_exclude_area
};

// Where to park. `from` is the head position. `current_object` is the footprint of the object printed now
// when printing by object, else null: then "away" stays beside that object, as finished objects may be
// taller than the head. `reachable` tells whether the active extruder reaches a point (null: everywhere).
// Empty when no free spot was found.
std::optional<Vec2d> park_point(const Sites &sites, MinLayerTimePausePark mode, const Vec2d &custom, const Vec2d &from,
                                const BoundingBoxf *current_object, const std::function<bool(const Vec2d &)> &reachable);

// The G-code that stands for DWELL_TAG when the layer took `layer_time` seconds of `target`: the comment
// and G4 for the shortfall, or empty when there is none worth stopping for (the caller drops the block).
std::string dwell_gcode(float target, float layer_time);

} // namespace MinLayerTimePause
} // namespace Slic3r

#endif // slic3r_GCode_MinLayerTimePause_hpp_
