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
static constexpr int    COUNT_CAP       = 100;

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

} // namespace fill_bed
} // namespace Slic3r

#endif // slic3r_FillBedPack_hpp_
