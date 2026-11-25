// See RepairPaint.hpp.
#include "RepairPaint.hpp"

#include "Airbrush/AirbrushBake.hpp"
#include "BoundingBox.hpp"
#include "Model.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace RepairPaint {

float auto_tolerance(const indexed_triangle_set &its)
{
    BoundingBoxf3 bb;
    for (const stl_vertex &v : its.vertices)
        bb.merge(v.cast<double>());
    const float diag = bb.defined ? float(bb.size().norm()) : 0.f;
    return std::max(0.1f, 0.02f * diag);
}

Saved::Saved(const ModelVolume &volume)
{
    m_painted = PaintTransfer::has_painting(volume);
    // An airbrushed part is kept as well: its field is re-baked over the new mesh.
    if ((! m_painted && ! volume.airbrush_field) || volume.mesh().empty())
        return;
    m_filament = PaintTransfer::effective_filament(volume);
    // A private model holds the copy: Transfer reads its painting from a ModelVolume. The copy
    // shares the old mesh, and a model made here is never backed up.
    m_model        = std::make_unique<Model>();
    ModelObject *o = m_model->add_object();
    m_volume       = o->add_volume(volume);
}

Saved::~Saved() = default;
Saved::Saved(Saved &&) noexcept = default;
Saved &Saved::operator=(Saved &&) noexcept = default;

static Result carry(const Saved &saved, ModelVolume &volume, float tolerance, unsigned layers)
{
    Result out;
    const auto t0 = std::chrono::steady_clock::now();
    PaintTransfer::Source src;
    src.volume   = saved.volume();
    src.to_world = Transform3d::Identity(); // the new mesh is in the old one's frame
    // The unpainted skin stays unpainted: source and target print with the same filament.
    src.filament = saved.filament();
    PaintTransfer::Transfer transfer({ src });

    PaintTransfer::Options opts;
    opts.max_dist         = tolerance > 0.f ? tolerance : auto_tolerance(volume.mesh().its);
    opts.normal_agreement = true;
    // A border between two whole old facets is followed down to a quarter of an old edge (and no
    // coarser than 0.05 mm, the brush's own finest step), so it stays within one facet.
    {
        const indexed_triangle_set &old_its = saved.volume()->mesh().its;
        double sum = 0.;
        for (const Vec3i32 &f : old_its.indices)
            sum += (old_its.vertices[f[1]] - old_its.vertices[f[0]]).norm();
        const float mean_edge = old_its.indices.empty() ? 0.f : float(sum / double(old_its.indices.size()));
        opts.edge_limit       = std::clamp(0.25f * mean_edge, 0.005f, 0.05f);
    }
    transfer.set_options(opts);
    out.stats   = transfer.to(volume, Transform3d::Identity(), saved.filament(), nullptr, layers);
    out.carried = out.stats.layers > 0;
    out.ms      = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    return out;
}

Result restore(const Saved &saved, ModelVolume &volume, float tolerance)
{
    if (saved.volume() == nullptr || volume.mesh().empty())
        return {};
    const ModelVolume &old = *saved.volume();
    unsigned layers = PaintTransfer::ALL_LAYERS;
    if (volume.airbrush_field)
        layers &= ~unsigned(PaintTransfer::LAYER_COLOUR);
    Result out;
    if (saved.has_painting())
        out = carry(saved, volume, tolerance, layers);
    if (volume.airbrush_field) {
        const bool baked = Airbrush::airbrush_after_mesh_replaced(volume, old.mesh().its, old.mmu_segmentation_facets.get_data());
        if (! baked) {
            // The field gave up (it no longer covers the part): keep the colour it had baked.
            const Result c = carry(saved, volume, tolerance, PaintTransfer::LAYER_COLOUR);
            out.carried    = out.carried || c.carried;
            out.ms += c.ms;
        }
    }
    BOOST_LOG_TRIVIAL(info) << "RepairPaint: '" << volume.name << "' " << old.mesh().its.indices.size() << " -> "
                            << volume.mesh().its.indices.size() << " facets, " << out.stats.layers << " layer(s), "
                            << out.stats.samples << " samples, " << out.ms << " ms";
    return out;
}

} // namespace RepairPaint
} // namespace Slic3r
