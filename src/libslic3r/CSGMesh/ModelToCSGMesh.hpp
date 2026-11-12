#ifndef MODELTOCSGMESH_HPP
#define MODELTOCSGMESH_HPP

#include "CSGMesh.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/SLA/Hollowing.hpp"
#include "libslic3r/MeshSplitImpl.hpp"

namespace Slic3r { namespace csg {

// Flags to select which parts to export from Model into a csg part collection.
// These flags can be chained with the | operator
enum ModelParts {
    mpartsPositive = 1,   // Include positive parts
    mpartsNegative = 2,   // Include negative parts
    mpartsDrillHoles = 4, // Include drill holes
    mpartsDoSplits = 8,   // Split each splitable mesh and export as a union of csg parts
};

template<class OutIt>
bool model_to_csgmesh(const ModelObject &mo,
                      const Transform3d &trafo, // Applies to all exported parts
                      OutIt              out,   // Output iterator
                      // values of ModelParts OR-ed
                      int                parts_to_include = mpartsPositive
                      )
{
    bool do_positives  = parts_to_include & mpartsPositive;
    bool do_negatives  = parts_to_include & mpartsNegative;
    bool do_drillholes = parts_to_include & mpartsDrillHoles;
    bool do_splits     = parts_to_include & mpartsDoSplits;
    bool has_splitable_volume = false;

    for (const ModelVolume *vol : mo.volumes) {
        if (vol && vol->mesh_ptr() &&
            ((do_positives && vol->is_model_part()) ||
             (do_negatives && vol->is_negative_volume()))) {

            if (do_splits && its_is_splittable(vol->mesh().its)) {
                CSGPart part_begin{{}, vol->is_model_part() ? CSGType::Union : CSGType::Difference};
                part_begin.stack_operation = CSGStackOp::Push;
                *out = std::move(part_begin);
                ++out;

                // A shell wound inwards is a void (the hollow of a hollow part): it is taken away from
                // the shells around it, flipped outward, rather than unioned inside out. Largest first,
                // so each shell comes after every shell that encloses it: an island in a void is added
                // back after the void is taken away.
                std::vector<std::pair<float, indexed_triangle_set>> shells;
                its_split(vol->mesh().its, SplitOutputFn{[&shells](indexed_triangle_set &&its) {
                              if (!its.empty()) {
                                  const float volume = its_volume(its);
                                  shells.emplace_back(volume, std::move(its));
                              }
                          }});
                std::stable_sort(shells.begin(), shells.end(),
                                 [](const auto &a, const auto &b) { return std::abs(a.first) > std::abs(b.first); });
                for (auto &[volume, its] : shells) {
                    const bool is_void = volume < 0.f;
                    if (is_void)
                        its_flip_triangles(its);
                    CSGPart part{std::make_unique<indexed_triangle_set>(std::move(its)),
                                 is_void ? CSGType::Difference : CSGType::Union,
                                 (trafo * vol->get_matrix()).cast<float>()};
                    part.name = vol->name;
                    *out = std::move(part);
                    ++out;
                }

                CSGPart part_end{{}};
                part_end.stack_operation = CSGStackOp::Pop;
                *out = std::move(part_end);
                ++out;
                has_splitable_volume = true;
            } else {
                CSGPart part{&(vol->mesh().its),
                             vol->is_model_part() ? CSGType::Union : CSGType::Difference,
                             (trafo * vol->get_matrix()).cast<float>()};
                part.name = vol->name;
                *out = std::move(part);
                ++out;
            }
        }
    }

    //if (do_drillholes) {
    //    sla::DrainHoles drainholes = sla::transformed_drainhole_points(mo, trafo);

    //    for (const sla::DrainHole &dhole : drainholes) {
    //        CSGPart part{std::make_unique<const indexed_triangle_set>(
    //                         dhole.to_mesh()),
    //                     CSGType::Difference};

    //        *out = std::move(part);
    //        ++out;
    //    }
    //}

    return has_splitable_volume;
}

}} // namespace Slic3r::csg

#endif // MODELTOCSGMESH_HPP
