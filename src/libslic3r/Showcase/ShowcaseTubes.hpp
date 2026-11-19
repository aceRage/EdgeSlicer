#ifndef slic3r_Showcase_ShowcaseTubes_hpp_
#define slic3r_Showcase_ShowcaseTubes_hpp_

// Showcase realism phase 4 (tests/design_showcase_realism.md 0 and 5.2): real tube meshes of the
// sliced plate's extrusions, for export only (the Blender package). The live view never needs them.
//
//  * Each run of extrusions (one filament, one kind of line, no travel in between) becomes one tube: a
//    rounded bead cross-section (the line's width and layer height) swept along the moves, mitred at
//    the corners, capped at the ends. The bead hangs below the move's Z, which is the top of its layer.
//  * Level of detail: nearly straight runs are thinned first (points within TUBE_SIMPLIFY_MM of the
//    straight line are dropped); then the cross-section has 8, 6 or 4 sides, the most that keeps the
//    whole plate within the triangle budget. Past the budget even at 4 sides, the last runs in print
//    order are dropped and the result says so.
//  * Culling (decision D-R9): internal infill (sparse and internal solid) is left out unless asked for;
//    supports and the skirt / brim follow the scene's toolpath switches.
//  * Per vertex: the extrusion direction (the silk sheen's tangent) and the build fraction (0 = the
//    first move of the print .. 1 = the last) so Blender can grow the print.
//
// Millimetres, world space (the plate's origin already added by the caller). Deterministic. GL- and
// wxWidgets-free.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Slic3r {
namespace Showcase {

enum class TubeKind : uint8_t { Extrude = 0, Travel, Other };
enum class TubeRole : uint8_t { Wall = 0, Skin, SparseInfill, InternalSolid, Support, Skirt, Other };

// One move of the slice, as GCodeProcessor reports it: the END of the move, and what the move was.
struct TubeMove
{
    float    x = 0.f, y = 0.f, z = 0.f;
    float    width = 0.f, height = 0.f;   // mm; of an extrusion
    int      extruder = 0;                // 0-based
    TubeKind kind = TubeKind::Other;
    TubeRole role = TubeRole::Other;
};

constexpr float  TUBE_SIMPLIFY_MM       = 0.01f;
constexpr size_t TUBE_DEFAULT_TRIANGLES = 4000000;

struct TubeOptions
{
    int    sides          = 0;      // 0 = automatic (8, 6 or 4 to fit the budget), else 3..16
    size_t max_triangles  = TUBE_DEFAULT_TRIANGLES;
    bool   internals      = false;  // sparse and internal solid infill
    bool   supports       = true;
    bool   skirt          = true;
    float  simplify_mm    = TUBE_SIMPLIFY_MM;
    // The bead's cross-section: a superellipse |x|^p + |y|^p = 1 (2 = an ellipse, larger = boxier).
    float  bulge          = 3.f;
};

struct TubeMesh
{
    int                   extruder = 0;   // 0-based
    std::vector<float>    positions;      // x, y, z (mm)
    std::vector<float>    normals;        // x, y, z (unit)
    std::vector<float>    direction;      // x, y, z (unit, along the extrusion)
    std::vector<float>    build;          // 0..1 per vertex
    std::vector<uint32_t> indices;
    size_t vertex_count() const { return positions.size() / 3; }
    size_t triangle_count() const { return indices.size() / 3; }
};

struct TubeResult
{
    std::vector<TubeMesh> meshes;      // one per filament used, by extruder
    int    sides     = 0;
    size_t runs      = 0;              // runs made into tubes
    size_t points    = 0;              // their points after thinning
    size_t culled    = 0;              // extrusion moves left out by the culling rules
    size_t dropped   = 0;              // runs left out by the budget
    size_t triangles = 0;
};

TubeResult build_tubes(const std::vector<TubeMove> &moves, const TubeOptions &options = TubeOptions());

// The triangles a run of `points` points makes with `sides` sides (two caps included).
size_t tube_triangles(size_t points, int sides);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseTubes_hpp_
