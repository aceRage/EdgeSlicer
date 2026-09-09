#ifndef slic3r_CutUtils_hpp_
#define slic3r_CutUtils_hpp_

#include "enum_bitmask.hpp"
#include "Point.hpp"
#include "Model.hpp"
#include "CurvedCut.hpp"

#include <vector>

namespace Slic3r {

using ModelObjectPtrs = std::vector<ModelObject*>;

// Append the NEGATIVE_VOLUME that carries a flexi joint into `mo` before the cut runs.
// The volume's mesh is the male body; Cut regenerates both bodies from cut_info.flexi, so
// the mesh is only there for previews and for "does it intersect the contour" checks.
// Lives in libslic3r (not in the gizmo) so tests can drive the whole apply path headless.
ModelVolume* add_flexi_joint_volume(ModelObject* mo, const CutConnector& connector, const std::string& name);

// True when this object carries at least one unprocessed flexi joint connector.
bool has_flexi_joint(const ModelObject* mo);

enum class ModelObjectCutAttribute : int { KeepUpper, KeepLower, KeepAsParts, FlipUpper, FlipLower, PlaceOnCutUpper, PlaceOnCutLower, CreateDowels, InvalidateCutInfo };
using ModelObjectCutAttributes = enum_bitmask<ModelObjectCutAttribute>;
ENABLE_ENUM_BITMASK_OPERATORS(ModelObjectCutAttribute);


class Cut {

    Model                       m_model;
    int                         m_instance;
    const Transform3d           m_cut_matrix;
    ModelObjectCutAttributes    m_attributes;

    void post_process(ModelObject* object, ModelObjectPtrs& objects, bool keep, bool place_on_cut, bool flip);
    void post_process(ModelObject* upper_object, ModelObject* lower_object, ModelObjectPtrs& objects);
    void finalize(const ModelObjectPtrs& objects);

public:

    Cut(const ModelObject* object, int instance, const Transform3d& cut_matrix, 
        ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper |
                                              ModelObjectCutAttribute::KeepLower |
                                              ModelObjectCutAttribute::KeepAsParts );
    ~Cut() { m_model.clear_objects(); }

    struct Groove
    {
        float depth{ 0.f };
        float width{ 0.f };
        float flaps_angle{ 0.f };
        float angle{ 0.f };
        float depth_init{ 0.f };
        float width_init{ 0.f };
        float flaps_angle_init{ 0.f };
        float angle_init{ 0.f };
        float depth_tolerance{ 0.1f };
        float width_tolerance{ 0.1f };
    };

    struct Part
    {
        bool selected;
        bool is_modifier;
    };

    const ModelObjectPtrs& perform_with_plane();
    // Curved cut, phase 1: split by a height field z = f(u,v) over the cut plane
    // instead of by the plane itself. A sheet with every control point at zero IS
    // the plane, and this routes straight into perform_with_plane() in that case, so
    // a zero-displacement curved cut runs the same code path as today's flat cut and
    // its output is bit-identical. No connectors on a curved cut in phase 1.
    const ModelObjectPtrs& perform_with_curved_sheet(const CurvedCutSheet& sheet);
    // Flexi joint cut: one object, two watertight model parts, a real Manifold boolean.
    // perform_with_plane() dispatches here automatically when a flexi connector is present.
    const ModelObjectPtrs& perform_with_flexi_joints();
    const ModelObjectPtrs& perform_by_contour(std::vector<Part> parts, int dowels_count);
    const ModelObjectPtrs& perform_with_groove(const Groove& groove, const Transform3d& rotation_m, bool keep_as_parts = false);

}; // namespace Cut

} // namespace Slic3r

#endif /* slic3r_CutUtils_hpp_ */
