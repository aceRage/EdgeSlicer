#ifndef slic3r_AdaptiveLayerMerge_hpp_
#define slic3r_AdaptiveLayerMerge_hpp_

// Mixed-layer-height band planner, ported from BambuStudio v02.09.01.70 src/libslic3r/AdaptiveLayerMerge.{hpp,cpp}
// (AGPL-3.0). Pure planning functions plus the patch record a planned band is stored as. Since step 3 the planner
// runs from PrintObject::make_perimeters() on plates whose used filaments print through more than one nozzle
// diameter; Layer::make_perimeters() and PrintObject::combine_adaptive_infill() consume the patches. Supports
// (step 4) and the Bambu 3MF keys (step 5) still ignore them.

#include "ExPolygon.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Slic3r {

class Layer;
class PrintConfig;
class PrintObject;

struct AdaptiveLayerHeightRange
{
    double min_height { 0. };
    double max_height { 0. };

    bool contains(double height) const;
};

// One planned merge: a region extrudes `area()` once, on layer `top_idx`, at `merged_height`, and extrudes
// nothing on the layers below it in the span.
struct AdaptiveMergePatch
{
    // A region may carry multiple disjoint patches on one group-top layer. They keep the nozzle's common
    // top_idx but may start at different bottom_idx values after local Z-conflict clipping.
    size_t     bottom_idx { 0 };
    size_t     top_idx { 0 };
    // Only set on the copy owned by the group-top LayerRegion. Layer::id() may be offset against the
    // PrintObject layer index (raft layers, bottom layers dropped by fix_slicing_errors), so the role
    // has to be stored explicitly instead of being derived from top_idx.
    bool       group_top { false };
    double     merged_height { 0. };

    // The retained contour is extruded on the group top and marks the swallowed footprint on lower layers.
    const ExPolygons &area() const
    {
        static const ExPolygons empty;
        return m_area ? *m_area : empty;
    }

    void set_area(ExPolygons area) { m_area = std::make_shared<const ExPolygons>(std::move(area)); }

    // Geometry that could not be assigned to a printable slab. It is intentionally separate from area():
    // the patch must suppress fill in this footprint without generating walls or claiming Z occupancy.
    const ExPolygons &suppressed_area() const
    {
        static const ExPolygons empty;
        return m_suppressed_area ? *m_suppressed_area : empty;
    }

    void set_suppressed_area(ExPolygons area)
    {
        m_suppressed_area = std::make_shared<const ExPolygons>(std::move(area));
    }

    bool has_suppressed_area() const { return bool(m_suppressed_area); }

private:
    // All copies stored across a merge span share the same immutable geometry.
    std::shared_ptr<const ExPolygons> m_area;
    std::shared_ptr<const ExPolygons> m_suppressed_area;
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

// The footprint of a layer whose orphans were suppressed: sliced, but extruded by no region. What rests on it
// is not supported there.
ExPolygons adaptive_suppressed_orphan_area(const Layer &layer);

// Id convention (kept deliberately, documented rather than unified): the vector-taking gates below
// (has_multiple_used_nozzle_diameters, adaptive_band_reference_nozzle) take 0-based filament indices, as Bambu's
// API and Print::extruders() do. adaptive_layer_height_range_for_filament takes a 1-based filament id, because it
// forwards straight into Edge's nozzle_diameter_for_filament / physical_extruder_for_filament, which are 1-based
// across Edge. Unifying would mean changing either Bambu's ported API or Edge-wide helpers, so it is left as is.
//
// True when the filaments handed in do not all share one nozzle diameter. filament_ids are 0 based; pass the
// filaments the print actually uses. Diameters are resolved through Edge's nozzle_diameter_for_filament(), which
// honors filament_nozzle_assignment (per-nozzle P2), nozzle_diameter_override (P7) and rack slots (P4/P9), so the
// gate cannot disagree with the flows behind it.
bool has_multiple_used_nozzle_diameters(const PrintConfig &print_config, const std::vector<unsigned int> &filament_ids);

// How many base layers one nozzle merges into a single extrusion. The stack is sliced at the user's
// layer_height; min_nozzle_diameter is only the ratio's denominator (how many of those base layers a
// bigger nozzle covers per extrusion), not a grid. A 0.8 nozzle next to a 0.2 reference keeps one
// contour out of every four at equal layer_height. The ratio is corrected when the resulting thickness
// falls outside what the nozzle can actually extrude. Returns 1 when the nozzle has to print every
// layer on its own.
size_t adaptive_layer_band_size(double nozzle_diameter, double min_nozzle_diameter, double base_layer_height);
// The same, walked into `range` instead of the plain diameter window: the live planner passes the per-nozzle clamp
// (adaptive_layer_height_range_for_nozzle) so a band never exceeds the configured min/max layer height of its nozzle.
size_t adaptive_layer_band_size(double nozzle_diameter, double min_nozzle_diameter, double base_layer_height,
                                const AdaptiveLayerHeightRange &range);

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
// segment before it and the segment is re-cut from the base anchor, so eight layers with band_size five come out
// as 5 + 3 (a full band from the base, then the short tail as its own band; Bambu's upstream comment says 4 + 4,
// which the code does not do). Returns an empty plan when band_size is 1, i.e. when this nozzle merges nothing.
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

// The band reference nozzle: the smallest nonzero diameter among the given filaments (0-based ids).
// It is the ratio denominator for adaptive_layer_band_size(), not the height the stack is sliced at.
// 0 when none resolves to a positive diameter.
double adaptive_band_reference_nozzle(const PrintConfig &print_config, const std::vector<unsigned int> &filament_ids);

// The printable window of the nozzle 1-based `filament_id` prints with: Bambu's [0.2d, 0.7d] window, narrowed by
// the min_layer_height / max_layer_height of the PHYSICAL extruder the filament maps to
// (physical_extruder_for_filament, so filament_nozzle_assignment is honored and slots past the nozzle count do not
// fall back to extruder 0 via get_at() - the pitfall Bambu's comment above warns about). An unmapped mixed
// virtual filament id still falls back to nozzle 0, so that failure mode is a silently wrong nozzle rather
// than a crash. Zero or negative limits are ignored. If the two windows do not overlap the Bambu window is
// returned unchanged.
//
// Consistency with Orca #17000 (equal_layering): after #17000 the prime tower no longer requires objects to share
// min/max_layer_height; it only requires one base layer grid. The per-nozzle limits that equal_layering stopped
// comparing are enforced here instead, per band, so every nozzle keeps its own window while all of them stay on the
// one shared grid the wipe tower syncs to.
AdaptiveLayerHeightRange adaptive_layer_height_range_for_filament(const PrintConfig &print_config, unsigned int filament_id);

// The per-nozzle clamp the live planner sizes every band with. The window of the nozzle
// that prints 1-based `filament_id`, narrowed by min_layer_height / max_layer_height as follows:
//   * extruder-sized vectors (the normal layout): the slot of the physical extruder the filament maps to, and only
//     when that extruder's preset nozzle IS the resolved diameter. A rack slot of another size (H3C P4/P9) is a
//     different nozzle, so the preset's pair does not apply to it.
//   * filament-sized vectors (Preset::normalize under single_extruder_multi_material): the tightest pair over the
//     filament asked about and every filament of `used_filament_ids` (0-based) on the same nozzle - an AMS whose
//     filaments 1 and 5 share a nozzle with different limits gets the larger minimum and the smaller maximum.
//   * a slot past the end of the vector is ignored, never read through get_at()'s values.front() fallback.
// adaptive_layer_height_range_for_filament() is this with no other filament on the nozzle.
AdaptiveLayerHeightRange adaptive_layer_height_range_for_nozzle(const PrintConfig               &print_config,
                                                                unsigned int                     filament_id,
                                                                const std::vector<unsigned int> &used_filament_ids);

// Test accessor for PrintObject::compute_adaptive_merge_groups(). The planner runs from PrintObject::make_perimeters()
// since step 3; this lets a unit test re-run it on a sliced object without the perimeter step.
struct AdaptiveMergePlanAccess
{
    static void compute(PrintObject &object);
};

} // namespace Slic3r

#endif
