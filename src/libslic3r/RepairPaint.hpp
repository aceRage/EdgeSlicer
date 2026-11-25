// Keep a part's painting when its mesh is rebuilt in place: Fix model (netfabb), Repair/Remesh,
// Round all edges, Quad remesh and Simplify. Every one of those gives the part new facets (new
// order, new count), and the painting is stored per facet, so until now each of them cleared the
// support, seam, colour and fuzzy-skin painting (Plater::clear_before_change_mesh).
//
// The painting is carried instead: the old part is kept aside, and once the new mesh is in place
// every new facet is resampled from the old surface (PaintTransfer: nearest point on the old mesh
// through its AABB tree, the facet normals must agree, samples at the corners, edge midpoints and
// centre, and a new facet that straddles a paint border is split until the border is sharp
// again). Runs in parallel (TBB).
//
// Note: the Paint Split PRs (feat/paint-split-1/-2) carry painting through their own split repair.
// Once both have merged, the two should share one helper.
#ifndef slic3r_RepairPaint_hpp_
#define slic3r_RepairPaint_hpp_

#include "PaintTransfer.hpp"
#include "TriangleMesh.hpp"

#include <memory>

namespace Slic3r {

class Model;
class ModelVolume;

namespace RepairPaint {

// How far (mesh mm) a rebuilt surface may sit from the old one and still take its painting, when
// the caller has no better figure: 2% of the part's bounding-box diagonal, at least 0.1 mm.
float auto_tolerance(const indexed_triangle_set &its);

// The painting a part had, kept aside before its mesh is replaced. Cheap: the old mesh is shared,
// not copied, and so is the painting.
class Saved
{
public:
    explicit Saved(const ModelVolume &volume);
    ~Saved();
    Saved(const Saved &) = delete;
    Saved &operator=(const Saved &) = delete;
    Saved(Saved &&) noexcept;
    Saved &operator=(Saved &&) noexcept;

    // Whether there is any support, seam, colour or fuzzy-skin painting to carry.
    bool has_painting() const { return m_painted; }
    // The old part (a copy that shares the old mesh), or null when there is nothing to carry.
    const ModelVolume *volume() const { return m_volume; }
    // Filament the real part printed with (the copy has no object config to look it up in).
    int filament() const { return m_filament; }

private:
    std::unique_ptr<Model> m_model;
    const ModelVolume     *m_volume   = nullptr;
    int                    m_filament = 1;
    bool                   m_painted  = false;
};

struct Result
{
    bool                 carried = false; // some painting was written
    PaintTransfer::Stats stats;
    long                 ms = 0;
};

// Repaints `volume`, whose mesh was just replaced by one rebuilt from `saved` IN THE SAME MESH
// FRAME (positions kept, volume matrix unchanged), from the painting it had before. `tolerance`
// (mesh mm) is how far the new surface may sit from the old one, 0 = auto_tolerance(). Nothing
// happens when `saved` has no painting (an unpainted part stays exactly as it is).
// An airbrushed part's colour is re-baked from its field over the new mesh
// (Airbrush::airbrush_after_mesh_replaced), as the repair paths did before; only when that bake
// gives up is the old colour painting resampled like the other layers.
Result restore(const Saved &saved, ModelVolume &volume, float tolerance = 0.f);

} // namespace RepairPaint
} // namespace Slic3r

#endif // slic3r_RepairPaint_hpp_
