#ifndef slic3r_TacJoint_hpp_
#define slic3r_TacJoint_hpp_

#include "TriangleMesh.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

struct TacJointParams
{
    float scale         = 1.f;
    float clearance     = .12f;
    float end_clearance = .30f;
    float offset        = 0.f;
    int   version       = 1;

    bool operator==(const TacJointParams& other) const
    {
        return scale == other.scale && clearance == other.clearance && end_clearance == other.end_clearance && offset == other.offset &&
               version == other.version;
    }
    bool operator!=(const TacJointParams& other) const { return !(*this == other); }

    template<class Archive> void serialize(Archive& archive) { archive(scale, clearance, end_clearance, offset, version); }
};

struct TacJointValidation
{
    bool        valid;
    std::string message;
};

// kerf_lo / kerf_hi bound the removed slab in the connector frame. Both retained
// halves must have positive engagement. Values are in millimetres.
TacJointValidation tac_joint_validate(const TacJointParams&, double kerf_lo = 0, double kerf_hi = 0);

// Local Z is the 11 mm longitudinal axis. Both meshes include offset, but no
// gizmo rotation. Invalid scalar parameters throw std::invalid_argument. Both
// meshes remain available to preview an invalid fit/engagement; callers must
// pass tac_joint_validate before applying the cut.
indexed_triangle_set tac_joint_connector_mesh(const TacJointParams&);
indexed_triangle_set tac_joint_cutter_mesh(const TacJointParams&);
Transform3d          tac_joint_source_transform();

std::vector<std::pair<std::string, double>> tac_joint_attributes(const TacJointParams&);
// Missing attributes take defaults. Invalid values remain invalid and are
// rejected by tac_joint_validate; malformed versions become -1.
TacJointParams tac_joint_from_attributes(const std::function<std::optional<double>(const std::string&)>&);

} // namespace Slic3r

#endif
