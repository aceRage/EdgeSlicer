#ifndef slic3r_AdaptiveLayerMerge_hpp_
#define slic3r_AdaptiveLayerMerge_hpp_

// Mixed-layer-height band planner, ported from BambuStudio v02.09.01.70 src/libslic3r/AdaptiveLayerMerge.{hpp,cpp}
// (AGPL-3.0). Standalone step 1: pure planning functions only. Nothing in the slicer calls them yet, so G-code is
// unchanged. Left out for the later region-merge step: AdaptiveMergePatch and adaptive_suppressed_orphan_area(),
// which need a LayerRegion member Edge does not have yet.

#include "ExPolygon.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Slic3r {

class PrintConfig;

struct AdaptiveLayerHeightRange
{
    double min_height { 0. };
    double max_height { 0. };

    bool contains(double height) const;
};

// One band of the layer stack, [bottom_idx, top_idx] inclusive. A nozzle prints the band as a single
// extrusion of merged_height, placed on the top layer; the layers below it are dropped.
struct AdaptiveLayerBand
{
    // Ordinary bands are shaped by top_idx. The first band is special: callers use the true first-layer
    // contour and color, and top_idx only selects the physical Z where that snapshot is extruded.
    size_t bottom_idx { 0 };
    size_t top_idx { 0 };
    double merged_height { 0. };
};

AdaptiveLayerHeightRange adaptive_layer_height_range(double nozzle_diameter);

// Top gap between support and the object above it for a nozzle diameter. Only 0.2 / 0.4 / 0.6 / 0.8 ship;
// anything else takes the nearest one.
float support_top_z_for_nozzle(double nozzle_diameter);

// Keep only the pieces a nozzle of this diameter could really lay a bead into. Two contours polygonised
// apart - consecutive slices of a shrinking shape, a slice and the paint covering it - never coincide point
// for point, and what one has over the other is a fringe of degenerate, near zero area slivers. Offsetting a
// degenerate contour makes Clipper hand back islands orders of magnitude larger than what it was given, so
// the area test comes first and the erosion is read as a yes/no answer only: the returned pieces are always
// the originals, never the output of the offset.
ExPolygons adaptive_printable_pieces(ExPolygons pieces, double nozzle_diameter);

// Like adaptive_printable_pieces(), but also trims every kept piece down to the part a bead of this nozzle
// fits into. A sloped wall's fringe that widens at one corner passes the yes/no test as a whole, and its
// hairline tails would otherwise become a patch of their own and be walled as fragmented thin lines.
ExPolygons adaptive_printable_core(ExPolygons pieces, double nozzle_diameter);

// True when the filaments handed in do not all share one nozzle diameter. filament_ids are 0 based; pass the
// filaments the print actually uses. Diameters are resolved through Edge's nozzle_diameter_for_filament(), which
// honors filament_nozzle_assignment (per-nozzle P2), nozzle_diameter_override (P7) and rack slots (P4/P9), so the
// gate cannot disagree with the flows behind it.
bool has_multiple_used_nozzle_diameters(const PrintConfig &print_config, const std::vector<unsigned int> &filament_ids);

// How many base layers one nozzle merges into a single extrusion. The whole print is sliced at the layer
// height of its smallest nozzle, so a larger nozzle simply keeps one layer out of every diameter-ratio
// layers: a 0.8 nozzle next to a 0.2 one keeps one contour out of every four. The ratio is corrected when
// the resulting thickness falls outside what the nozzle can actually extrude. Returns 1 when the nozzle
// has to print every layer on its own.
size_t adaptive_layer_band_size(double nozzle_diameter, double min_nozzle_diameter, double base_layer_height);

// Split the complete layer stack into bands of band_size layers. The first band starts at the object base
// and is extruded on its top layer with the complete merged height, so it rests on the bed or on the
// object's raft/support substrate. Every nozzle shares that anchor, so bands of commensurate sizes nest
// into each other and their boundaries never straddle.
//
// band_tops optionally marks hard structural boundaries that a band must not straddle. Colour ownership
// changes are deliberately not such boundaries: every nozzle keeps its fixed ladder and the caller aligns
// ownership to the band's anchor contour. The marks are honored unless doing so would leave a segment the
// nozzle cannot extrude, in which case printability wins. Pass an empty vector for the normal fixed grid.
//
// Unlike a fixed stride, no layer is left out: a tail too short for a full band is folded back into the
// segment before it and the segment is re-cut, so eight layers with band_size five come out as 4 + 4 rather
// than 5 + an orphaned 3. Returns an empty plan when band_size is 1, i.e. when this nozzle merges nothing.
std::vector<AdaptiveLayerBand> build_adaptive_layer_bands(
    const std::vector<double>      &layer_heights,
    size_t                          band_size,
    const AdaptiveLayerHeightRange &height_range,
    const std::vector<uint8_t>     &band_tops = {});

// Tail recovery may pin a band top on a layer this nozzle never owns (the geometric stack top after
// a colour disappears). Pull top_idx down to the last layer in the band that still has material,
// and recompute merged_height. Returns false when no such layer exists, the leftover is a single
// step, or the shortened height is outside the printable window: the caller then abandons the band
// instead of falling back to the base layer height on every step.
//
// has_material is indexed by layer; it must cover [bottom_idx, top_idx].
bool pull_adaptive_band_top_to_material(
    AdaptiveLayerBand                &band,
    const std::vector<double>        &layer_heights,
    const AdaptiveLayerHeightRange   &height_range,
    const std::vector<unsigned char> &has_material);

// Cut a continuous Z span into printable groups as close as possible to preferred_band_size. Used by the
// wipe tower, which merges whole slabs and has no per-region geometry to decimate.
bool split_adaptive_layer_span(
    const std::vector<double>              &layer_heights,
    size_t                                  bottom_idx,
    size_t                                  top_idx,
    size_t                                  preferred_band_size,
    const AdaptiveLayerHeightRange         &height_range,
    std::vector<std::pair<size_t, size_t>> &groups);

// ---- EdgeSlicer additions (not in BambuStudio) ----------------------------------------------------------------

// The band reference nozzle: the smallest diameter among the used filaments (0 based ids), i.e. the nozzle whose
// layer height the whole stack is sliced at. 0 when none resolves to a positive diameter.
double adaptive_band_reference_nozzle(const PrintConfig &print_config, const std::vector<unsigned int> &filament_ids);

// The printable window of the nozzle 1-based `filament_id` prints with: Bambu's [0.2d, 0.7d] window, narrowed by
// the min_layer_height / max_layer_height of the PHYSICAL extruder the filament maps to
// (physical_extruder_for_filament, so filament_nozzle_assignment is honored and slots past the nozzle count do not
// fall back to extruder 0 - the get_at() pitfall Bambu's comment above warns about). Zero or negative limits are
// ignored. If the two windows do not overlap the Bambu window is returned unchanged.
//
// Consistency with Orca #17000 (equal_layering): after #17000 the prime tower no longer requires objects to share
// min/max_layer_height; it only requires one base layer grid. The per-nozzle limits that equal_layering stopped
// comparing are enforced here instead, per band, so every nozzle keeps its own window while all of them stay on the
// one shared grid the wipe tower syncs to.
AdaptiveLayerHeightRange adaptive_layer_height_range_for_filament(const PrintConfig &print_config, unsigned int filament_id);

} // namespace Slic3r

#endif
