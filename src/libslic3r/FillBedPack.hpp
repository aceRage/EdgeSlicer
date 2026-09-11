#ifndef slic3r_FillBedPack_hpp_
#define slic3r_FillBedPack_hpp_

#include "Point.hpp"

namespace Slic3r {

// The pure arithmetic behind "Fill bed with copies": how many copies of one template can be
// expected to fit, and where the user's own edge margins move the bed outline to.
//
// Both live here rather than in src/slic3r/GUI/Jobs/FillBedJob.cpp so the dialog's live
// "Estimated copies" label, the job's clone count and the unit tests all use one implementation,
// and so the tests need neither wx nor a Plater.
namespace fill_bed {

// How many more clones than the estimate to hand the packer, so that a slightly optimistic pack
// is never starved of items. The surplus costs only packer time: anything that lands on a
// virtual bed is discarded by FillBedJob::finalize().
static constexpr double COUNT_OVERSHOOT = 1.3;
// The packer is roughly O(n^2); this is the ceiling that keeps a fill of a tiny part on a large
// bed from taking minutes. It is 100 rather than the 150 the spec suggested because
// FillBedJob::process() still hands anything ABOVE 100 items to the old bounding-box grid
// branch - which knows nothing about the gap, the edge margins, rotation, or the other
// objects on the plate. Phase 2 replaces that branch with a proper grid; until then the cap
// keeps every fill on the NFP path the dialog actually drives.
// The Compact (NFP) layout's ceiling. Above it the fill switches to the Grid layout, which is
// O(cells) rather than O(n^2) and has no ceiling beyond the bed itself. 150 is the number the
// spec suggested; phase 1 held it at 100 only because anything above that fell through to the
// old ad-hoc bounding-box branch, which knew nothing about the gap, the margins, rotation or
// the other objects on the plate. That branch is gone - grid_pack() replaces it - so the cap
// can go back up.
static constexpr int    COUNT_CAP       = 150;

// The number of copies of a `bbox_w` x `bbox_h` (mm) template that tile an area of `free_area`
// (mm^2) when neighbours are `gap` mm apart.
//
// The gap is SHARED between neighbours, so the footprint of one copy in a tiling is
// (w + gap) * (h + gap), not the area of the fully inflated outline - which is what the old
// area-ratio estimate used, and why it came out systematically low.
//
// Returns 0 for a degenerate template or a non-positive free area.
int estimate_count(double bbox_w, double bbox_h, double gap, double free_area);

// estimate_count() with the overshoot and the cap applied - the number of clones to create.
int estimate_count_with_overshoot(double bbox_w, double bbox_h, double gap, double free_area);

// The user's fill-only edge margins, applied to a bed outline that has ALREADY been shrunk by
// arrange's own margin (get_shrink_bedpts, i.e. bed_shrink_x/y plus the skirt distance).
//
// `edge_margin` applies to all four sides and `front_margin` to the front (min-Y) side only;
// the effective front margin is max(front_margin, edge_margin), so the front override can never
// place a copy closer to the edge than the general margin would.
//
// Each side only moves by the amount the arrange shrink has not already covered
// (margin - already_shrunk_*, clamped at 0), so margins of 0 leave the outline exactly as
// arrange produced it and the dialog can never widen the printable area.
//
// For a non-rectangular bed the move follows the sign of each point's offset from the centre,
// the same rule get_shrink_bedpts uses; the front override then moves only the points on the
// front half.
//
// All distances are in mm; `bedpts` is in scaled coordinates.
Points shrink_bed_per_side(const Points &bedpts,
                           double        edge_margin,
                           double        front_margin,
                           bool          front_margin_enabled,
                           double        already_shrunk_x,
                           double        already_shrunk_y);

// ---------------------------------------------------------------------------------------------
// The Grid layout - strategy (b) of the spec.
// ---------------------------------------------------------------------------------------------

// How the copies are laid out. Persisted in AppConfig as fill_bed/layout.
enum class Layout : int {
    // Today's path: the NFP packer, which nestles copies into whatever pocket it finds. Denser
    // for awkward outlines, but irregular-looking and O(n^2), so it is capped at COUNT_CAP.
    Compact = 0,
    // Bounding-box tiling of the template, tried at 0 and 90 degrees, centred on the free bed
    // area. Deterministic, instant, regular - what a bed of cubes should look like.
    Grid    = 1,
};

// One obstacle the grid must not land on, in scaled coordinates: an exclusion region, the wipe
// tower, or an object already on the plate. `outline` is the obstacle's own outline; `inflation`
// is the clearance that must be kept around it (half-gap for a real object, the exclusion gap
// for a virtual one), so a cell is rejected when its own inflated hull touches this one.
struct GridObstacle {
    Polygon outline;
    coord_t inflation = 0;
};

// One placement the grid found.
struct GridCell {
    // Where the template's ORIGIN goes, in scaled coordinates - i.e. what to write into
    // ArrangePolygon::translation.
    Point  translation{0, 0};
    // The rotation applied to the template, in radians: 0 or pi/2.
    double rotation = 0.;
};

// Tile `bed` with copies of `tmpl`.
//
// `tmpl` is the template outline in scaled coordinates, positioned exactly as the item is (so
// its own offset from the origin is preserved - the returned translation is relative to that,
// the same convention ArrangePolygon::translation uses).
//
// `gap` (scaled) is the clear distance between neighbouring copies, shared between them: the
// step is the template's bounding box plus one gap. Each copy is treated as its bounding box
// inflated by gap/2 when it is tested against the obstacles, which is the same "half each"
// rule the NFP path's ArrangePolygon::inflation uses.
//
// `allow_rotation` decides whether 90 degrees is tried at all; when it is, the orientation that
// yields more cells wins (ties go to 0 degrees, so an unrotated layout is never disturbed for
// nothing).
//
// `bed` is the outline the copies must stay inside - already shrunk by arrange's own margin AND
// by the user's per-side margins (shrink_bed_per_side), so this function adds no margin of its
// own. A cell is kept only when its whole gap-free bounding box is inside the bed.
//
// `obstacles` are the things already on the plate. A cell whose inflated box intersects one is
// dropped; the rest of the grid keeps its positions, so the layout still reads as a grid with
// holes in it rather than as a reflow.
//
// `max_cells` caps the result (0 = no cap); cells are produced row by row from the front-left,
// so a cap keeps a contiguous block.
//
// Rows and columns are centred on the bed's bounding box, so the leftover margin is split
// evenly and the layout looks deliberate.
std::vector<GridCell> grid_pack(const ExPolygon              &tmpl,
                                const Points                 &bed,
                                coord_t                       gap,
                                bool                          allow_rotation,
                                const std::vector<GridObstacle> &obstacles,
                                size_t                        max_cells = 0);

// How many cells grid_pack() would return, without building them. Used by the dialog's live
// estimate, which for Grid is exact rather than a tiling guess.
size_t grid_count(const ExPolygon              &tmpl,
                  const Points                 &bed,
                  coord_t                       gap,
                  bool                          allow_rotation,
                  const std::vector<GridObstacle> &obstacles);

} // namespace fill_bed
} // namespace Slic3r

#endif // slic3r_FillBedPack_hpp_
