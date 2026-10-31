#ifndef slic3r_Airbrush_AirbrushBake_hpp_
#define slic3r_Airbrush_AirbrushBake_hpp_

// Airbrush P1 (tests/design_airbrush_paint.md, section 3.3; decisions D4-D8, D15, D16): turning the
// smooth colour field into ordinary `paint_color` MMU paint, and what that paint will cost.
//
// The bake is the only place grain exists. For every facet it subdivides to a leaf a quarter of the
// grain across, groups the leaves into GRAIN CELLS (a g x g square in the plane of the facet's
// dominant axis), averages the field over each cell and decides the whole cell at once, so a speck
// is never smaller than one grain cell. The decision, per cell:
//
//   * Low band (walls and slopes only, D4): the field smoothed over low_pass_mm picks the nearest
//     step of a mix ladder (mix_steps steps, 25% each by default) between the part's own filament
//     and the dominant slot - a mixed filament the slicer prints as layer alternation.
//   * High band: what the ladder step does not explain is dithered against an ordered blue-noise
//     threshold (D6) into pure specks of each slot, or of the background. Stable: one cell's
//     decision depends only on the field near it, the seed and the cell's position.
//   * Tops and bottoms (|normal.z| >= flat_cos) never get mix steps: only the last layer shows,
//     so a step there prints as whichever layer happened to be last (P0 photo 03). They get specks
//     only, at a grain of at least min_grain_top_mm (0.8 mm, P0). A finer request there falls back
//     to TONE - each 0.8 mm cell takes whichever of the background and the slots covers most of it -
//     never to islands too small to print (they broke the white top skin on the P0 card).
//   * Walls and slopes clamp the grain to min_grain_wall_mm (0.6 mm, P0).
//
// Merging (D16): leaves where the field decides "background" keep the paint that was there before
// the airbrush. The field's BakeRecord remembers which triangles the airbrush wrote and what they
// carried before, so a re-bake starts from that "under" paint and never from its own old specks.
// Triangles the airbrush does not touch are copied bit for bit.

#include "AirbrushField.hpp"

#include <functional>
#include <string>

namespace Slic3r {

class ModelVolume;

namespace Airbrush {

// Above about this coverage specks stop reading as separate dots and read as a tone (P0, walls and
// slopes at 0.12 mm layers). A hint for the UI; the bake does not change behaviour at it.
static constexpr float AIRBRUSH_TONE_COVERAGE = 0.35f;
inline bool coverage_reads_as_tone(float coverage) { return coverage > AIRBRUSH_TONE_COVERAGE; }

// P0 defaults (D5 retune, 2026-10-30).
static constexpr float AIRBRUSH_DEFAULT_GRAIN_MM  = 1.0f;
static constexpr float AIRBRUSH_MIN_GRAIN_WALL_MM = 0.6f;
static constexpr float AIRBRUSH_MIN_GRAIN_TOP_MM  = 0.8f;
static constexpr int   AIRBRUSH_DEFAULT_MIX_STEPS = 4;
// The most leaves one bake may evaluate; past it the leaf size grows (the grain is unaffected).
static constexpr size_t AIRBRUSH_MAX_LEAVES = 8000000;
static constexpr int    AIRBRUSH_MAX_DEPTH  = 7;

struct AirbrushBakeParams
{
    float    grain_mm          = AIRBRUSH_DEFAULT_GRAIN_MM;
    float    min_grain_wall_mm = AIRBRUSH_MIN_GRAIN_WALL_MM;
    float    min_grain_top_mm  = AIRBRUSH_MIN_GRAIN_TOP_MM;
    // A facet with |normal.z| >= flat_cos (30 degrees from horizontal by default) is a top or a
    // bottom: specks only, top grain floor.
    float    flat_cos          = 0.866f;
    // Mix ladder steps between the part's filament and a slot; < 2 disables the ladder.
    int      mix_steps         = AIRBRUSH_DEFAULT_MIX_STEPS;
    // Radius of the low band (the tone the ladder renders), in mm.
    float    low_pass_mm       = 1.5f;
    // Grain cells covered less than this are background.
    float    min_coverage      = 0.02f;
    // One seed per volume; "reroll" changes it.
    uint32_t seed              = 0;
    // The volume's own filament (1-based), the "A" end of every mix ladder. 0 disables the ladder.
    int      base_filament     = 0;

    std::string to_string() const;
    static bool from_string(const std::string &s, AirbrushBakeParams &out);
    bool        operator==(const AirbrushBakeParams &r) const { return to_string() == r.to_string(); }
};

// The grain a facet actually gets.
struct EffectiveGrain
{
    float grain_mm      = AIRBRUSH_DEFAULT_GRAIN_MM;
    // True when a top/bottom asked for a grain below its floor: the cells are min_grain_top_mm and
    // decide by majority instead of by dithering.
    bool  tone_fallback = false;
};
EffectiveGrain effective_grain(const AirbrushBakeParams &params, bool flat);

// The mix ladder: the state id (a mixed filament) that prints `step` of `steps` of filament_b over
// filament_a, or <= 0 when there is none. P1 asks; creating the mixed filaments is P4.
using LadderLookup = std::function<int(int filament_a, int filament_b, int step, int steps)>;

struct AirbrushBakeStats
{
    size_t leaves_evaluated   = 0;
    size_t leaves_airbrushed  = 0;   // leaves the field decided (not background)
    size_t leaves_speck       = 0;   // ... as a pure slot filament
    size_t leaves_mix         = 0;   // ... as a mix ladder step
    size_t triangles_owned    = 0;
    size_t grain_cells        = 0;
    size_t grain_cells_tone   = 0;   // cells above AIRBRUSH_TONE_COVERAGE
    size_t mix_on_flat_leaves = 0;   // always 0; counted so the tests can pin the rule
    bool   top_tone_fallback  = false;
    float  grain_wall_mm      = 0.f;
    float  grain_top_mm       = 0.f;
};

struct AirbrushBakeResult
{
    bool                                    ok = false;
    std::string                             error;
    TriangleSelector::TriangleSplittingData painting;
    BakeRecord                              record;
    AirbrushBakeStats                       stats;
};

// Bake `field` over `its`. `current` is the volume's painting now; where the field's record is
// valid for this mesh, the triangles it owns are first put back to their "under" paint. A record
// made on a mesh with another facet count is refused (ok = false): the old specks in `current`
// could not be told apart from real paint, so the caller must go through
// airbrush_rebake_after_mesh_edit() instead.
AirbrushBakeResult airbrush_bake(const indexed_triangle_set                     &its,
                                 const AirbrushField                            &field,
                                 const TriangleSelector::TriangleSplittingData &current,
                                 const AirbrushBakeParams                       &params,
                                 const LadderLookup                             &ladder = {});

// What to do when the volume's paint was edited since the last bake by something that does not know
// about the field (design 3.7: another slicer, or the plain brush on an airbrushed facet).
enum class OnForeignPaintEdit {
    // Do nothing and return false; the GUI asks the user first (P2).
    Refuse,
    // Re-bake from the field: the edits on the airbrush's own facets are replaced by the paint that
    // was there before the airbrush; edits elsewhere are kept.
    Overwrite,
};

// The convenience the GUI (P2) and the tests use: bake volume.airbrush_field into
// volume.mmu_segmentation_facets and store the new record and params on a copy of the field.
// False (and nothing changed) when the volume has no field, the bake failed, or the paint was edited
// elsewhere and `on_edit` is Refuse.
bool airbrush_bake_volume(ModelVolume &volume, const AirbrushBakeParams &params, const LadderLookup &ladder = {},
                          AirbrushBakeStats *stats = nullptr, OnForeignPaintEdit on_edit = OnForeignPaintEdit::Refuse);

// Is the volume's painting still the one the field's last bake produced (D16)? False when someone
// edited the paint without the airbrush, or the field was never baked.
bool airbrush_paint_in_sync(const ModelVolume &volume);

// D15: the volume's mesh changed. `old_its` / `old_paint` are the mesh and painting before the edit,
// `old_to_new` maps old mesh space to the new one (identity for an edit that kept positions: repair,
// remesh, simplify, a cut). Moves the field with the mesh, carries the paint that was there before the
// airbrush onto the new mesh (TriangleSelector::remap_painting) and re-bakes on top of it.
bool airbrush_rebake_after_mesh_edit(ModelVolume &volume, const indexed_triangle_set &old_its,
                                     const TriangleSelector::TriangleSplittingData &old_paint, const Transform3d &old_to_new,
                                     const AirbrushBakeParams &params, const LadderLookup &ladder = {}, AirbrushBakeStats *stats = nullptr);

// ---- the cost estimate (D8) ---------------------------------------------------------------------

struct AirbrushCostParams
{
    double first_layer_height = 0.2;
    double layer_height       = 0.2;
    // The volume's own filament (1-based): present on every layer the part spans.
    int    base_filament      = 1;
    // Purge per change, mm^3, before the multiplier (the profile's flush volume) and its multiplier.
    double flush_mm3          = 280.;
    double flush_multiplier   = 1.;
    double density_g_cm3      = 1.24;
    double swap_seconds       = 60.;
    // Which physical filament a painted state prints on a layer (a mixed filament alternates its
    // components). Default: the state itself.
    std::function<int(int state, int layer)> resolve;
};

struct AirbrushCost
{
    int    layers           = 0;
    int    multi_tool_layers = 0;   // layers printing more than one filament
    int    swaps            = 0;
    double purge_mm3        = 0.;
    double purge_g          = 0.;
    double swap_seconds     = 0.;
    // Per layer, the filaments printed there (ascending).
    std::vector<std::vector<int>> filaments_per_layer;
};

// Swaps, purge and swap time for a painted volume printed alone. `trafo` places the mesh on the bed
// (z = 0 is the bed). A swap is counted whenever the next filament needed differs from the one
// loaded: each layer starts with the filament the previous layer ended on when it needs it, which is
// what the slicer's tool ordering does (P0: one change per two-colour layer, never two).
AirbrushCost airbrush_estimate_cost(const indexed_triangle_set &its, const TriangleSelector::TriangleSplittingData &painting,
                                    const AirbrushCostParams &params, const Transform3d &trafo = Transform3d::Identity());

// The swap model on its own: `per_layer[l]` lists the filaments layer l prints (any order, duplicates
// allowed). airbrush_estimate_cost() ends here; the P2 cost meter also uses it to merge parts.
AirbrushCost airbrush_cost_from_layers(std::vector<std::vector<int>> per_layer, const AirbrushCostParams &params);

// Several parts of one object printed together: the union of their filaments on each layer, then one
// swap count. All parts must have been estimated with the same layer heights and the same bed z.
AirbrushCost airbrush_merge_costs(const std::vector<AirbrushCost> &parts, const AirbrushCostParams &params);

// D8: the cost meter warns (never caps) above these.
static constexpr double AIRBRUSH_WARN_PURGE_G      = 50.;
static constexpr double AIRBRUSH_WARN_SWAP_SECONDS = 2. * 3600.;
struct AirbrushCostWarning
{
    bool purge = false;
    bool time  = false;
    bool any() const { return purge || time; }
};
AirbrushCostWarning airbrush_cost_warning(const AirbrushCost &cost, double purge_g_limit = AIRBRUSH_WARN_PURGE_G,
                                          double swap_seconds_limit = AIRBRUSH_WARN_SWAP_SECONDS);

} // namespace Airbrush
} // namespace Slic3r

#endif // slic3r_Airbrush_AirbrushBake_hpp_
