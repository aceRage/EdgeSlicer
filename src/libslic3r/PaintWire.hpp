#ifndef slic3r_PaintWire_hpp_
#define slic3r_PaintWire_hpp_

// Paint wire (pen colouring: spike S0, then paint_v1 apply, H2): the hub's receiver for paint made by
// the phone app.
//
// The app paints with its own model - an adaptive 4-way midpoint quadtree per original triangle,
// spec in EdgeSlicerApp docs/paint_wire_v0.md - and never reads or writes this slicer's bitstream.
// A 4-way midpoint split is TriangleSelector's three-side split with special side 0, so the tree
// converts to ordinary MMU painting exactly: what the phone showed is what the PC gets.

#include "TriangleMesh.hpp"
#include "TriangleSelector.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// The deepest tree the wire accepts (4^10 leaves per original triangle).
constexpr int PAINT_WIRE_MAX_DEPTH = 10;
// Pre-order byte that marks a split node; every other byte is a leaf state.
constexpr uint8_t PAINT_WIRE_SPLIT = 0xFF;

// One changed original triangle: its index and its tree bytes.
using PaintWireTree = std::pair<int, std::vector<uint8_t>>;

// Check one tree: depth, normal form (no split whose four children are equal leaves), no trailing
// bytes. Returns false with `error` set.
bool paint_wire_check_tree(const std::vector<uint8_t> &bytes, std::string &error);

// The painting with `trees` written over `existing`: a listed triangle is replaced by its tree, an
// unlisted one keeps its bits from `existing` exactly. Triangles must be ascending, unique and
// below `n_original_triangles`. Returns false with `error` set and leaves `out` empty.
bool paint_wire_apply(size_t                                         n_original_triangles,
                      const std::vector<PaintWireTree>              &trees,
                      const TriangleSelector::TriangleSplittingData &existing,
                      TriangleSelector::TriangleSplittingData       &out,
                      std::string                                   &error);

// What the app needs to draw the PC's current painting: per state, the sub-triangles in mesh
// space and the original triangle each came from. State 0 (unpainted) is not listed.
struct PaintWireDisplay
{
    int                       state = 0;
    std::vector<Vec3f>        vertices;   // 3 per sub-triangle
    std::vector<int>          source;     // 1 per sub-triangle
};
std::vector<PaintWireDisplay> paint_wire_display(const TriangleMesh                            &mesh,
                                                 const TriangleSelector::TriangleSplittingData &painting);

// One original triangle's subtree in `data`: its bits from TriangleSelector::serialize(), empty when
// the triangle is one unpainted leaf (serialize() leaves those out). Two paintings of a triangle are
// the same exactly when these are equal: paint_v1's apply compares what the tablet saw with what the
// PC has now, triangle by triangle.
std::vector<bool> paint_wire_triangle_bits(const TriangleSelector::TriangleSplittingData &data, int triangle);

// The highest state any tree in `trees` uses (0 when they are all unpainted); -1 with `error` set for a
// tree that does not decode.
int paint_wire_max_state(const std::vector<PaintWireTree> &trees, std::string &error);

} // namespace Slic3r

#endif // slic3r_PaintWire_hpp_
