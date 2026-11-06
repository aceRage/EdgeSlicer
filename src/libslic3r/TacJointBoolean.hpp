#ifndef slic3r_TacJointBoolean_hpp_
#define slic3r_TacJointBoolean_hpp_

#include "MeshBoolean.hpp"
#include "Model.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace Slic3r {

inline bool has_tac_joint_volumes(const ModelObject& object)
{
    return std::any_of(object.volumes.begin(), object.volumes.end(), [](const ModelVolume* volume) {
        return volume->cut_info.is_tac_joint() && (volume->is_model_part() || volume->is_negative_volume());
    });
}

// Shared by manual Mesh Boolean and STL export after the normal input validation.
// The reference profile has small, nearly coplanar facets. MCUT/CGAL may produce
// self-intersecting float meshes at their intersections even when the inputs pass
// preflight. Keep TAC objects on Manifold through both union and subtraction.
inline TriangleMesh bake_tac_joint_object(const ModelObject& object)
{
    const auto clean = [](TriangleMesh& mesh) {
        // Conversion to float may collapse distinct intersection vertices onto
        // exactly the same point. Weld only identical coordinates and remove the
        // resulting zero-edge faces; do not move the profile or change its fit.
        its_merge_vertices(mesh.its);
        its_remove_degenerate_faces(mesh.its);
        its_compactify_vertices(mesh.its);
        mesh = TriangleMesh(std::move(mesh.its));
    };
    std::vector<indexed_triangle_set> positives;
    for (const ModelVolume* volume : object.volumes)
        if (volume->is_model_part()) {
            TriangleMesh mesh(volume->mesh());
            mesh.transform(volume->get_matrix(), true);
            positives.push_back(std::move(mesh.its));
        }
    indexed_triangle_set united;
    if (!MeshBoolean::mfd::union_all(positives, united))
        throw std::runtime_error("Unable to unite TAC Joint object parts.");
    TriangleMesh result(std::move(united));
    clean(result);
    for (const ModelVolume* volume : object.volumes)
        if (volume->is_negative_volume() && !result.empty()) {
            TriangleMesh cutter(volume->mesh());
            cutter.transform(volume->get_matrix(), true);
            std::vector<TriangleMesh> pieces;
            if (!MeshBoolean::mfd::make_boolean(result, cutter, pieces, "A_NOT_B"))
                throw std::runtime_error("Unable to subtract a TAC Joint object socket.");
            result = TriangleMesh{};
            for (const auto& piece : pieces)
                result.merge(piece);
            clean(result);
        }
    return result;
}

} // namespace Slic3r

#endif
