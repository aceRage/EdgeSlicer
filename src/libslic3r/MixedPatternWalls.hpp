#pragma once

// EdgeSlicer, mixed filament wall patterns: too few walls notice.
//
// A mixed filament's manual pattern may hold comma groups, one per wall index: group 0 is the outer wall, the last
// group is the deepest wall and everything behind it (MixedFilamentManager::resolve_perimeter). A pattern of N groups
// therefore needs N walls. Where a part prints fewer, the deeper groups never print: their filaments still load on the
// prime tower, and the colour the pattern was built for does not appear.
//
// This module only OBSERVES, after the walls of an object are generated. It reports two cases per mixed filament:
//  - the region's Wall loops setting is below the number of groups;
//  - the setting is enough, but in thin areas of the outline fewer walls fit (see areas_with_fewer_walls).
// It changes no extrusion and no G-code.

#include "ExPolygon.hpp"
#include "Point.hpp"

#include <string>
#include <vector>

namespace Slic3r {

// The parts of `outline` (scaled) where wall number `walls` (1 = the outer wall) does not fit: the part the
// morphological opening by the depth of that wall's centreline removes, less pieces smaller than a square of two
// external line widths (corner rounding, Clipper slivers).
//   classic: wall i (0-based) exists where the outline is at least ext_width + 2 * i * spacing wide.
//   Arachne: can place a single centre line as the deepest wall, so half a spacing less is needed.
ExPolygons areas_with_fewer_walls(const ExPolygons &outline, coord_t ext_width, coord_t spacing, int walls, bool arachne);

// What one object found for one mixed filament.
struct MixedPatternWallShortfall
{
    unsigned int filament_id{0};   // the mixed filament's 1-based id, as shown in the filament list
    int          walls_needed{0};  // comma groups of its pattern
    int          walls_set{0};     // lowest Wall loops among the regions that use it and have too few; 0 = none
    size_t       thin_layers{0};   // layers with thin areas (regions whose setting is enough)
    double       thin_area{0.};    // mm2, summed over those layers
    double       first_thin_z{0.}; // mm, lowest such layer

    bool setting_too_low() const { return walls_set > 0; }
    // Thin areas are reported from this summed area on, so ordinary models with a few sharp tips stay quiet.
    bool thin_areas() const;
    bool any() const { return this->setting_too_low() || this->thin_areas(); }
};

// One slicing notice for an object, or empty when no entry has anything to say.
std::string mixed_pattern_walls_notice(const std::vector<MixedPatternWallShortfall> &found, const std::string &object_name);

} // namespace Slic3r
