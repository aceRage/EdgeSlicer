#ifndef slic3r_Airbrush_AirbrushField_hpp_
#define slic3r_Airbrush_AirbrushField_hpp_

// Airbrush P1 (tests/design_airbrush_paint.md, sections 3.1, 3.2 and 4; decisions D1-D3, D9, D15,
// D16): the colour field, the source of truth of an airbrushed part.
//
//   * Where it lives (D2): a sparse surface grid in the volume's own MESH space. A cell is
//     AIRBRUSH_CELL_MM (0.15 mm) on a side and is keyed by its integer (i, j, k) AND by the
//     dominant axis of the surface normal it was painted through (one of six), so the two faces of
//     a wall thinner than a cell never share a cell - paint cannot bleed through a thin wall.
//   * What it holds (D3): filament weights, not colours. Up to AIRBRUSH_MAX_SLOTS slots per field,
//     each slot a 1-based filament id (physical or mixed), each weight 0..255 = coverage of that
//     slot over whatever the part is underneath. The remainder (255 - sum) is "background".
//   * How paint goes on (D9): brush dabs, airbrush style. A dab of flow f covers fraction a = f *
//     falloff(d) of a cell with the slot's filament, "over" everything already there; holding still
//     builds up. A stroke may cap its own build-up (the opacity cap, off by default).
//   * The grain is NOT stored here. The bake (AirbrushBake.hpp) turns the smooth field into specks
//     and mix steps, so grain size, seed and ladder can all change without touching a stroke.
//   * Mesh edits (D15): the field is position-keyed, so it survives anything that keeps surface
//     points where they were (repair, remesh, simplify, a cut that does not move the halves) and is
//     resampled by transform() when the mesh itself is moved, scaled or mirrored. The bake then
//     re-runs; see AirbrushBake.hpp.
//   * Ownership (D16): the field carries the record of its last bake - which original triangles
//     the airbrush wrote and what paint was there before - so a re-bake replaces only its own
//     paint and a paint edit made elsewhere (another slicer, the plain brush) is detectable.
//
// No ObjectBase anywhere: the field hangs off ModelVolume as a shared_ptr<const>, exactly like
// ModelVolume::cad_body, and goes to the 3MF as Metadata/airbrush/field_<n>.bin.

#include "../libslic3r.h"
#include "../BoundingBox.hpp"
#include "../Point.hpp"
#include "../TriangleSelector.hpp"

#include <admesh/stl.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r {

class ModelVolume;

namespace Airbrush {

static constexpr float  AIRBRUSH_CELL_MM   = 0.15f;
static constexpr size_t AIRBRUSH_MAX_SLOTS = 8;

using Weights = std::array<uint8_t, AIRBRUSH_MAX_SLOTS>;

// The six surface classes: the dominant axis of a normal and its sign.
//   0 = +X  1 = -X  2 = +Y  3 = -Y  4 = +Z  5 = -Z
// A tie goes to the LOWER axis (X, then Y, then Z), so a 45 degree facet always lands somewhere
// definite. -1 for a degenerate normal.
int surface_class(const Vec3f &normal);
int surface_class(const Vec3d &normal);

// The record of the last bake (D16). Triangle indices refer to the mesh the bake ran on.
struct BakeRecord
{
    // Ascending original-triangle indices the airbrush bake wrote.
    std::vector<int>                        owned;
    // The paint those triangles carried before the airbrush wrote them: the "under" paint a re-bake
    // starts from, so specks never pile up on old specks. Only `owned` triangles appear in it.
    TriangleSelector::TriangleSplittingData under;
    // Hash of the volume's whole mmu painting right after the bake. A mismatch on load means the
    // paint was edited by something that does not know about the field.
    uint64_t                                paint_hash = 0;
    // Facet count of the mesh the bake ran on; a different count means `owned` is stale.
    uint32_t                                facets     = 0;

    bool valid() const { return facets != 0; }
    void clear() { *this = BakeRecord(); }
};

// One dab of the brush. Positions are in mesh space, lengths in mm.
struct Dab
{
    Vec3f center    = Vec3f::Zero();
    float radius    = 2.f;
    // 0..1: inside radius * hardness the dab is at full strength, then a smoothstep to 0 at radius.
    float hardness  = 0.5f;
    // 0..1: the fraction a single dab deposits at full strength.
    float flow      = 0.25f;
    // The surface normal at the hit point (or the direction back to the viewer). Facets facing away
    // from it take no paint, so a dab never sprays through to the far side of a thin wall or slot.
    // Zero: every facet in the sphere.
    Vec3f normal    = Vec3f::Zero();
};

// Brush settings that stay fixed for a stroke.
struct StrokeParams
{
    // 0-based slot index to spray, or -1 for the eraser (sprays "background").
    int      slot        = 0;
    // 0 = off (D9 default). Otherwise the most a single stroke may add to a cell, 0..1.
    float    opacity_cap = 0.f;
    // 0 = off. Otherwise each dab centre is jittered by up to scatter * radius on each axis, from a
    // stable hash of (seed, dab number within the stroke), so a stroke replays identically.
    float    scatter     = 0.f;
    uint32_t seed        = 0;
};

class AirbrushSurface;

class AirbrushField
{
public:
    struct Cell
    {
        Weights w {};
        uint32_t sum() const { uint32_t s = 0; for (uint8_t v : w) s += v; return s; }
        bool     empty() const { return sum() == 0; }
    };

    AirbrushField() = default;
    explicit AirbrushField(float cell_mm) : m_cell(cell_mm) {}

    float        cell_size() const { return m_cell; }
    const Vec3d &origin() const { return m_origin; }

    // Slots: 1-based filament ids, at most AIRBRUSH_MAX_SLOTS. add_slot() returns the slot index of
    // `filament_id`, adding it when new; -1 when the field is full or the id is not a filament.
    const std::vector<int> &slots() const { return m_slots; }
    int                     slot_of(int filament_id) const;
    int                     add_slot(int filament_id);

    // Cell keys: (i, j, k) as 20-bit two's-complement fields plus the 3-bit surface class, counted
    // from origin(). The origin moves with the mesh (translate(), attach_to_mesh()), so moving a
    // part moves its field exactly, cells and grain alike, with no resampling.
    static uint64_t make_key(int64_t i, int64_t j, int64_t k, int cls);
    static void     split_key(uint64_t key, int64_t &i, int64_t &j, int64_t &k, int &cls);
    uint64_t        key_of(const Vec3d &p, int cls) const;
    Vec3d           center_of(uint64_t key) const;

    // Direct access, mainly for the tests and the serializer.
    const std::unordered_map<uint64_t, Cell> &cells() const { return m_cells; }
    size_t                                    size() const { return m_cells.size(); }
    bool                                      empty() const { return m_cells.empty(); }
    void                                      set_cell(uint64_t key, const Weights &w);
    const Cell                               *find(uint64_t key) const;

    // The weights at a surface point painted through a facet with surface class `cls`. A point whose
    // own cell is empty takes the mean of the non-empty face neighbours of the same class (the
    // writer samples the surface at half a cell, so a leaf centroid can land in a cell the plane
    // only grazed). Returns false and zero weights when there is nothing there.
    bool sample(const Vec3d &p, int cls, Weights &out) const;

    // ---- painting (D9) --------------------------------------------------------------------------
    // A stroke is begin_stroke(), any number of dab() calls, end_stroke(). Within a stroke every
    // cell remembers its weights at the start and how much this stroke has covered it, so the
    // opacity cap is exact and rounding never drifts however many dabs land on one cell.
    void   begin_stroke(const StrokeParams &params);
    // Returns the number of cells the dab changed. `changed_keys`, when given, receives their keys (in
    // ascending order), so a viewer can redraw only what moved (the P2 overlay).
    size_t dab(const AirbrushSurface &surface, const Dab &dab, std::vector<uint64_t> *changed_keys = nullptr);
    void   end_stroke();
    bool   in_stroke() const { return m_stroke_active; }

    // ---- fills (P3: gradients and procedural patterns, AirbrushProcedural.hpp) ---------------
    // The coverage `fn` wants at a surface point, per slot index (0..1 each, summing to at most 1).
    // `facet` is the mesh triangle the point lies on.
    using Coverage     = std::array<float, AIRBRUSH_MAX_SLOTS>;
    using FillFunction = std::function<void(const Vec3f &p, size_t facet, int cls, Coverage &cov)>;
    // Moves every surface cell of the whole mesh toward what `fn` wants there: new = old + opacity
    // * (target - old), so opacity 1 replaces and a lower opacity glazes over strokes. Cells are
    // found exactly as dab() samples them (half a cell over every facet); `fn` runs once per cell,
    // at the first sample (facet order) that landed in it, so a fill is deterministic. Returns the
    // number of cells changed; `changed_keys` (ascending) as dab().
    size_t fill(const AirbrushSurface &surface, const FillFunction &fn, float opacity, std::vector<uint64_t> *changed_keys = nullptr);

    // ---- filament remap (P3, A1) ----------------------------------------------------------------
    // Every slot's filament id goes through `map` (1-based ids; an id the map sends outside
    // 1..ExtruderMax keeps its id). Slots that land on one filament are merged, their weights added.
    // Returns true when anything changed.
    bool remap_slots(const std::function<int(int)> &map);

    // The last gradient or pattern applied (AirbrushFillRecipe::to_string()), so "re-apply" can
    // repeat it (D11). Carried in the blob after the cells (minor version 1; older readers skip it).
    const std::string &fill_recipe() const { return m_fill_recipe; }
    void               set_fill_recipe(const std::string &s) { m_fill_recipe = s; }

    // ---- mesh edits (D15) -----------------------------------------------------------------------
    // Resample the field through `t` (mesh-space to new mesh-space). Positions move with the mesh,
    // surface classes follow the transformed normals, and a scale above 1 splats each old cell over
    // every new cell its image covers, so an enlarged field has no holes. Invalidates the bake
    // record (its triangle indices belong to the old mesh).
    void transform(const Transform3d &t);
    void translate(const Vec3d &d) { transform(Transform3d(Eigen::Translation3d(d))); }

    // The mesh the field was last attached to, as its bounding box. attach_to_mesh() compares it
    // with `its` and, when only the position changed (a 3MF loader re-centring the mesh, as
    // ModelVolume::cad_body's fingerprint handles), shifts the field to match. Returns the shift.
    Vec3d attach_to_mesh(const indexed_triangle_set &its);
    void  set_anchor(const indexed_triangle_set &its);
    const BoundingBoxf3 &anchor() const { return m_anchor; }

    // Bake parameters (AirbrushBakeParams::to_string()) and the last bake's record.
    const std::string &bake_params() const { return m_bake_params; }
    void               set_bake_params(const std::string &s) { m_bake_params = s; }
    const BakeRecord  &record() const { return m_record; }
    BakeRecord        &record() { return m_record; }

    // ---- persistence ----------------------------------------------------------------------------
    // Versioned binary blob (Metadata/airbrush/field_<n>.bin and the undo stack). from_blob() never
    // throws: anything malformed, truncated or of a newer major version yields nullptr.
    // `anchor`, when given, is written instead of the field's own (the 3MF writer passes the mesh's
    // box as written).
    std::string                                  to_blob(const BoundingBoxf3 *anchor = nullptr) const;
    static std::shared_ptr<AirbrushField>        from_blob(const std::string &blob);

    bool operator==(const AirbrushField &rhs) const;

private:
    float                              m_cell = AIRBRUSH_CELL_MM;
    Vec3d                              m_origin = Vec3d::Zero();

    void resample(const Transform3d &t);
    std::vector<int>                   m_slots;
    std::unordered_map<uint64_t, Cell> m_cells;
    BoundingBoxf3                      m_anchor;
    std::string                        m_bake_params;
    std::string                        m_fill_recipe;
    BakeRecord                         m_record;

    struct StrokeCell
    {
        Weights start {};
        float   alpha = 0.f;
    };
    bool                                     m_stroke_active = false;
    StrokeParams                             m_stroke;
    uint32_t                                 m_stroke_dabs = 0;
    std::unordered_map<uint64_t, StrokeCell> m_stroke_cells;
};

// Composite `alpha` of `slot` (or of background when slot < 0) over `start`, rounded. Exposed for
// the tests; dab() uses it.
Weights composite_over(const Weights &start, int slot, float alpha);

// The brush falloff, 1 inside radius * hardness, smoothstep to 0 at radius.
float dab_falloff(float d, float radius, float hardness);

// A mesh prepared for dabs: the triangles, their surface classes and an AABB tree, so a dab finds
// the facets inside its sphere without visiting the whole mesh. It keeps a REFERENCE to `its`:
// rebuild it whenever the volume's mesh is replaced (set_mesh), never keep it across that.
class AirbrushSurface
{
public:
    explicit AirbrushSurface(const indexed_triangle_set &its);
    ~AirbrushSurface();
    const indexed_triangle_set &its() const { return m_its; }
    const std::vector<int>     &classes() const { return m_classes; }
    std::vector<size_t>         facets_in_radius(const Vec3f &center, float radius) const;
    Vec3f                       facet_normal(size_t idx) const;

private:
    const indexed_triangle_set &m_its;
    std::vector<int>            m_classes;
    struct Tree;
    std::unique_ptr<Tree>       m_tree;
};

// Hash of a painting (FNV-1a over the triangle map and the bitstream), the BakeRecord's paint_hash.
uint64_t paint_hash(const TriangleSelector::TriangleSplittingData &data);

} // namespace Airbrush
} // namespace Slic3r

#endif // slic3r_Airbrush_AirbrushField_hpp_
