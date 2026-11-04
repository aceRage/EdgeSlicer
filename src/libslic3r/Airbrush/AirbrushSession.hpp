#ifndef slic3r_Airbrush_AirbrushSession_hpp_
#define slic3r_Airbrush_AirbrushSession_hpp_

// Airbrush P2 (tests/design_airbrush_paint.md, section 5 row P2; D8, D9, D10): the non-GUI half of
// the Airbrush tool in the colour-painting gizmo. Everything here is plain logic the gizmo drives
// and the unit tests pin:
//
//   * DabEmitter turns a held mouse button into dabs: spaced dabs along a drag, and dwell dabs at a
//     fixed rate while the cursor holds still, so holding builds up as an airbrush does (D9).
//   * The overlay: while strokes wait for their bake, the gizmo draws the field itself over the
//     model as small cell-sized quads, grouped in chunks so a dab only rebuilds what it touched.
//   * The bake job: everything the background bake needs, captured by value on the UI thread, and
//     a pure function that runs it off that thread. The result is written back as ONE change - field
//     and paint together - so an undo snapshot never holds a field without its paint.
//   * The cost job (D8): swaps, purge and swap time of a whole object, off the UI thread.
//   * The mix ladder from the project's existing mixed filaments (creating them is P4).

#include "AirbrushBake.hpp"
#include "AirbrushField.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

class MixedFilamentManager;
class ModelVolume;
class TriangleMesh;

namespace Airbrush {

// ---- brush settings -------------------------------------------------------------------------

// The tool's ranges and defaults. The grain defaults come from P0 (AirbrushBake.hpp).
static constexpr float AIRBRUSH_RADIUS_MIN       = 0.5f;
static constexpr float AIRBRUSH_RADIUS_MAX       = 25.f;
static constexpr float AIRBRUSH_RADIUS_DEFAULT   = 4.f;
static constexpr float AIRBRUSH_HARDNESS_DEFAULT = 0.3f;
static constexpr float AIRBRUSH_FLOW_MIN         = 0.01f;
static constexpr float AIRBRUSH_FLOW_DEFAULT     = 0.2f;
static constexpr float AIRBRUSH_GRAIN_MAX        = 3.f;
// The "Printer Nozzle Diameter" override (P3, item B).
static constexpr float AIRBRUSH_NOZZLE_MIN       = 0.1f;
static constexpr float AIRBRUSH_NOZZLE_MAX       = 2.f;
// Dwell dabs per second while the button is held still, and the spacing of drag dabs as a fraction
// of the radius.
static constexpr float AIRBRUSH_DAB_RATE_HZ      = 15.f;
static constexpr float AIRBRUSH_DAB_SPACING      = 0.25f;

struct BrushSettings
{
    float    radius_mm  = AIRBRUSH_RADIUS_DEFAULT;
    float    hardness   = AIRBRUSH_HARDNESS_DEFAULT;
    float    flow       = AIRBRUSH_FLOW_DEFAULT;
    float    grain_mm   = AIRBRUSH_DEFAULT_GRAIN_MM;
    bool     mix_steps  = true;
    uint32_t seed       = 0;
    // "Printer Nozzle Diameter" (P3, item B): 0 = Auto (the selected printer's nozzle), else the
    // nozzle the user paints for. It sets the grain floors the panel shows and the top floor of the
    // bake; slicing always uses the real nozzle.
    float    nozzle_mm  = 0.f;

    // Clamp every value into its range. The grain goes down to AIRBRUSH_MIN_GRAIN_MM (0.1 mm) for
    // every nozzle (P3); what the nozzle prints cleanly is a hint, and a finer top is the bake's
    // business (tone fallback at the nozzle's top floor).
    void sanitize();
    // "radius=4;hardness=0.3;..." with the classic locale, for the app config.
    std::string to_string() const;
    static BrushSettings from_string(const std::string &s);
    bool operator==(const BrushSettings &r) const { return to_string() == r.to_string(); }
};

// The bake parameters a volume gets from the brush settings. `base_filament` is the volume's own
// filament (the "A" end of every mix ladder).
// `printer_nozzle_mm` is the selected printer's nozzle (used when the brush's nozzle is Auto; <= 0
// means 0.4). A 0.4 mm nozzle gives the P0 floors exactly.
AirbrushBakeParams bake_params_from(const BrushSettings &brush, int base_filament, float printer_nozzle_mm = 0.f);

// ---- dabs from a held button (D9) -----------------------------------------------------------

// A point on the surface in mesh space and the direction the brush faces it from (back to the
// viewer), which keeps a dab off the far side of a thin wall.
struct SurfaceHit
{
    Vec3f pos    = Vec3f::Zero();
    Vec3f facing = Vec3f::Zero();
};

struct DabEmitterParams
{
    // Drag dabs every `spacing * radius` along the path.
    float spacing = AIRBRUSH_DAB_SPACING;
    // Dwell dabs per second while the cursor holds still.
    float rate_hz = AIRBRUSH_DAB_RATE_HZ;
    // A stalled UI (a long frame) never releases more than this many dwell dabs at once.
    int   max_dwell_burst = 4;
    // A jump longer than this many radii (the cursor crossed a gap in the model) is not filled in.
    float max_gap_radii = 6.f;
};

// Times are seconds on any monotonic clock. Not thread-safe; one per stroke.
class DabEmitter
{
public:
    explicit DabEmitter(const DabEmitterParams &params = {}) : m_params(params) {}

    // The press: one dab where it landed.
    std::vector<SurfaceHit> press(const SurfaceHit &hit, double t, float radius);
    // The cursor moved to `hit`: dabs every spacing along the straight segment from the last dab.
    // Moving resets the dwell clock whenever it lays a dab.
    std::vector<SurfaceHit> move(const SurfaceHit &hit, double t, float radius);
    // A timer tick with the button held: dwell dabs at the last position, rate_hz per second.
    std::vector<SurfaceHit> hold(double t);

    bool   active() const { return m_active; }
    size_t emitted() const { return m_emitted; }
    void   release() { m_active = false; }

private:
    DabEmitterParams m_params;
    bool             m_active    = false;
    SurfaceHit       m_last;              // the last dab laid
    SurfaceHit       m_cursor;            // where the cursor is now
    float            m_carry     = 0.f;   // path length since the last drag dab
    double           m_last_dab_t = 0.;   // time of the last dab, for the dwell clock
    size_t           m_emitted   = 0;
};

// ---- the overlay while strokes wait for their bake ------------------------------------------

// Chunks are 32 cells (4.8 mm) on a side, per surface class.
static constexpr int AIRBRUSH_OVERLAY_CHUNK_SHIFT = 5;
uint64_t overlay_chunk_of(uint64_t cell_key);

// Coverage is drawn in AIRBRUSH_OVERLAY_LEVELS steps, so a chunk needs few draw calls.
static constexpr int AIRBRUSH_OVERLAY_LEVELS = 4;

// One draw call of a chunk: quads of one slot at one coverage level.
struct OverlayBucket
{
    // 0-based slot of the field, or -1 for "background" (an erased cell: the part's own colour).
    int                slot  = -1;
    // 1..AIRBRUSH_OVERLAY_LEVELS for a slot (coverage, rounded up); 0 for background.
    int                level = 0;
    // Four corners per quad, in mesh space; triangles (0,1,2) and (0,2,3).
    std::vector<Vec3f> corners;
};

// The dominant slot of a cell and its coverage level, as the overlay draws it.
void overlay_style(const Weights &w, size_t nslots, int &slot, int &level);

// Quads for `cells` (keys of one chunk; a key no longer in the field draws as background). Each quad
// is the cell's outer face along its surface class, so it always sits just outside the surface.
// Buckets come out sorted by (slot, level).
std::vector<OverlayBucket> build_overlay_chunk(const AirbrushField &field, const std::vector<uint64_t> &cells);

// ---- the mix ladder from existing mixed filaments (P4 creates them) -------------------------

// A two-component mixed filament (LayerCycle or Simple) of `a` and `b` whose share of `b` is within
// half a step of step/steps; the closest one wins. 0 when the project has none.
int find_ladder_state(const MixedFilamentManager &mixed, size_t num_physical, int a, int b, int step, int steps);
// The same as a LadderLookup over a copy of `mixed`, safe to hand to a background job.
LadderLookup ladder_from_mixed(const MixedFilamentManager &mixed, size_t num_physical);

// ---- the bake job ---------------------------------------------------------------------------

struct BakeJob
{
    std::shared_ptr<const TriangleMesh>     mesh;
    std::shared_ptr<const AirbrushField>    field;     // the working field, copied at release
    TriangleSelector::TriangleSplittingData current;   // the volume's paint when the job started
    AirbrushBakeParams                      params;
    LadderLookup                            ladder;
    // Re-bake over paint edited elsewhere (or over a record from another mesh), replacing it on the
    // airbrush's own facets. False: such a job stops with ForeignEdit and changes nothing.
    bool                                    overwrite = false;
};

struct BakeJobResult
{
    enum class Status { Ok, ForeignEdit, StaleRecord, Failed };
    Status                                  status = Status::Failed;
    std::string                             error;
    // The field as committed: the job's field with the new record, params and anchor.
    std::shared_ptr<AirbrushField>          field;
    TriangleSelector::TriangleSplittingData painting;
    AirbrushBakeStats                       stats;
    double                                  bake_ms = 0.;
};

// Runs on any thread; touches nothing but its argument.
BakeJobResult run_bake_job(const BakeJob &job);

// Writes a finished bake into the volume: the field and its paint as one change.
void commit_bake(ModelVolume &volume, const BakeJobResult &result);

// ---- the cost job (D8) ----------------------------------------------------------------------

struct CostJob
{
    struct Part
    {
        std::shared_ptr<const TriangleMesh>     mesh;
        TriangleSelector::TriangleSplittingData paint;
        Transform3d                             trafo = Transform3d::Identity();   // mesh -> bed
        int                                     base_filament = 1;
    };
    std::vector<Part>  parts;
    AirbrushCostParams params;
};

struct CostJobResult
{
    AirbrushCost        cost;
    AirbrushCostWarning warning;
    double              ms = 0.;
};

CostJobResult run_cost_job(const CostJob &job);

} // namespace Airbrush
} // namespace Slic3r

#endif // slic3r_Airbrush_AirbrushSession_hpp_
