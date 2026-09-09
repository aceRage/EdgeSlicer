#ifndef libslic3r_FuzzySkin_hpp_
#define libslic3r_FuzzySkin_hpp_

#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/PerimeterGenerator.hpp"

namespace Slic3r::Feature::FuzzySkin {

// Fuzzy skin over overhangs (docs/superpowers/specs/2026-09-09-fuzzy-skin-overhang-research.md).
// "support" is the lower layer grown by half a wall width - exactly the polygons the perimeter
// generator already builds to decide erOverhangPerimeter - or nullptr when the caller has no such
// region (option off, first layer, no lower layer, overhang detection off). nullptr, and
// cfg.skip_overhangs == false, both mean "behave exactly as before".
void fuzzy_polyline(Points& poly, bool closed, coordf_t slice_z, const FuzzySkinConfig& cfg, const Polygons* support = nullptr);

void fuzzy_extrusion_line(Arachne::ExtrusionJunctions& ext_lines, coordf_t slice_z, const FuzzySkinConfig& cfg, const Polygons* support = nullptr);

void group_region_by_fuzzify(PerimeterGenerator& g);

bool should_fuzzify(const FuzzySkinConfig& config, int layer_id, size_t loop_idx, bool is_contour);

Polygon apply_fuzzy_skin(const Polygon& polygon, const PerimeterGenerator& perimeter_generator, size_t loop_idx, bool is_contour);
void    apply_fuzzy_skin(Arachne::ExtrusionLine* extrusion, const PerimeterGenerator& perimeter_generator, bool is_contour);

} // namespace Slic3r::Feature::FuzzySkin

#endif // libslic3r_FuzzySkin_hpp_
