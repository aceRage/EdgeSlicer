#include "PartMeshReplace.hpp"

#include "Model.hpp"
#include "TriangleMesh.hpp"
#include "BRep/CadBody.hpp"
#include "Airbrush/AirbrushBake.hpp"

namespace Slic3r {

bool replace_part_mesh(ModelVolume &volume, TriangleMesh &&mesh, std::shared_ptr<const BRep::CadBody> cad_body)
{
    ModelObject *object  = volume.get_object();
    const bool   sinking = object != nullptr && object->min_z() < SINKING_Z_THRESHOLD;

    const bool had_paint = !volume.supported_facets.empty() || !volume.seam_facets.empty() ||
                           !volume.mmu_segmentation_facets.empty() || !volume.fuzzy_skin_facets.empty();
    volume.supported_facets.reset();
    volume.seam_facets.reset();
    volume.mmu_segmentation_facets.reset();
    volume.fuzzy_skin_facets.reset();

    // Edge (Airbrush P3, A4): the paint goes, but the position-keyed colour field comes back on the
    // new mesh where it still has surface.
    const std::shared_ptr<const TriangleMesh> old_mesh = volume.mesh_ptr();
    volume.set_mesh(std::move(mesh));
    if (volume.airbrush_field) {
        const TriangleSelector::TriangleSplittingData none;
        const std::shared_ptr<const Airbrush::AirbrushField> field = volume.airbrush_field;
        Airbrush::airbrush_carry_field(volume, *field, old_mesh->its, none, Transform3d::Identity(), &none);
    }
    volume.calculate_convex_hull();
    volume.invalidate_convex_hull_2d();
    volume.set_new_unique_id();
    if (cad_body) {
        auto body       = std::make_shared<BRep::CadBody>(*cad_body);
        body->mesh      = BRep::mesh_fingerprint(volume.mesh().its);
        volume.cad_body = std::move(body);
    }
    if (object != nullptr) {
        object->invalidate_bounding_box();
        if (!sinking)
            object->ensure_on_bed();
    }
    return had_paint;
}

} // namespace Slic3r
