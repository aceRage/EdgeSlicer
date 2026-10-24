#ifndef slic3r_CutUtils_hpp_
#define slic3r_CutUtils_hpp_

#include "enum_bitmask.hpp"
#include "Point.hpp"
#include "Model.hpp"
#include "CurvedCut.hpp"
#include "DrawCut.hpp"
#include "TriangleSelector.hpp"

#include <map>
#include <optional>
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

// Append the NEGATIVE_VOLUME that carries a push-fit Pin into `mo` before the cut runs. Its mesh
// is the pin itself in mm (pushfit_preview_body), its transform the connector frame (pos,
// rotation_m, z_angle; no scale). The cut builds the pin object and both holes from
// cut_info.pushfit, so the mesh is only for previews and overlap checks.
ModelVolume* add_pushfit_volume(ModelObject* mo, const CutConnector& connector, const std::string& name);

// A connector that leaves the cut as an object of its own (a Dowel or a Pin), with the transform
// from its object frame to the frame the halves' meshes are built in (the source object's frame
// with the instance's rotation and scale applied, no offset). The assembled pose (C1) and the pin
// row are made from it.
struct CutSeparatePart
{
    ModelObject* object{ nullptr };
    Transform3d  part_to_cut_frame{ Transform3d::Identity() };
    bool         is_pin{ false };
    bool         pin_slot_failed{ false };
};

enum class ModelObjectCutAttribute : int { KeepUpper, KeepLower, KeepAsParts, FlipUpper, FlipLower, PlaceOnCutUpper, PlaceOnCutLower, CreateDowels, InvalidateCutInfo,
    // OrcaSlicer #13472: carry the support / seam / colour / fuzzy-skin painting of every model part
    // onto the halves (spatial remap, ModelVolume::restore_painting()). The gizmo always sets it
    // (owner, 2026-10-24: no preference); the contour / groove preview cuts leave it off.
    KeepPaint };
using ModelObjectCutAttributes = enum_bitmask<ModelObjectCutAttribute>;
ENABLE_ENUM_BITMASK_OPERATORS(ModelObjectCutAttribute);


class Cut {

    Model                       m_model;
    int                         m_instance;
    const Transform3d           m_cut_matrix;
    ModelObjectCutAttributes    m_attributes;
    // The cut thickness ("kerf") the current perform_with_plane() was asked for,
    // stashed so perform_with_flexi_joints() - which it dispatches into without
    // arguments - can widen the joint's gap by it. Zero for every other entry
    // point, which is the no-kerf behaviour.
    double                      m_kerf{ 0.0 };

    // C1, ASSEMBLED POSES (owner D8, tests/design_showcase_assembly_buildup.md). Taken from the
    // source object when the Cut is made: per instance, its assembled pose (or its plain
    // transformation when the assembled pose was never set - which is what the plater would
    // have initialised it to), and the inverse of the cut instance's offset-free matrix, which
    // takes the halves' mesh frame back to the source object's frame.
    std::vector<Transform3d>    m_src_assembly;
    Transform3d                 m_src_frame_inv{ Transform3d::Identity() };
    // Per instance, the source object's bounding box: the pin row starts beside it.
    std::vector<BoundingBoxf3>  m_src_bboxes;
    // The frame map of the objects that are not halves (dowels, pins), and the one used for the
    // halves (Identity, except perform_by_contour whose halves are not baked with the instance).
    std::map<const ModelObject*, Transform3d> m_part_to_cut_frame;
    Transform3d                 m_halves_to_cut_frame{ Transform3d::Identity() };
    int                         m_pin_slot_failures{ 0 };

    // Appends the dowels and pins to `objects`: dowels as before (instance reset), pins laid out
    // in a row beside the source object, lying in their print pose.
    void add_separate_parts(std::vector<CutSeparatePart>& parts, ModelObjectPtrs& objects);
    // Sets every instance's assembled pose so the part sits where it was in the source object's
    // assembled pose.
    void set_assembled_pose(ModelObject* object, const Transform3d& part_to_cut_frame) const;

    void post_process(ModelObject* object, ModelObjectPtrs& objects, bool keep, bool place_on_cut, bool flip);
    void post_process(ModelObject* upper_object, ModelObject* lower_object, ModelObjectPtrs& objects);
    using SavedPaintings = std::vector<std::optional<TriangleSelector::SavedPainting>>;
    // The painting of every model part of `object`, its mesh taken into the frame the halves' meshes
    // are built in (instance without offset, times the volume matrix). Empty without KeepPaint.
    SavedPaintings save_paintings(const ModelObject* object) const;
    void finalize(const ModelObjectPtrs& objects, const SavedPaintings& saved_paintings = {});

public:

    Cut(const ModelObject* object, int instance, const Transform3d& cut_matrix, 
        ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper |
                                              ModelObjectCutAttribute::KeepLower |
                                              ModelObjectCutAttribute::KeepAsParts );
    ~Cut() { m_model.clear_objects(); }

    // perform_by_contour runs a second Cut on a working copy of the object; it takes the
    // source's assembled poses and boxes from the outer one, since the copy has lost both.
    void inherit_source(const Cut& outer)
    {
        m_src_assembly  = outer.m_src_assembly;
        m_src_frame_inv = outer.m_src_frame_inv;
        m_src_bboxes    = outer.m_src_bboxes;
    }
    // How many pins came out without their slit because the boolean failed (the panel says so).
    int pin_slot_failures() const { return m_pin_slot_failures; }

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

    // `thickness` is the CUT THICKNESS ("kerf"), in mm along the cut normal: a
    // band of material centred on the cut surface is removed, so the upper half
    // keeps what is above +t/2 and the lower half what is below -t/2. Zero (the
    // default) is the original cut, bit-for-bit - the offset slice is only taken
    // when t > 0. See CurvedCut.hpp for the shared range and offset enum.
    const ModelObjectPtrs& perform_with_plane(double thickness = 0.0, CutThicknessOffset offset = CutThicknessOffset::Centred);
    // Curved cut, phase 1: split by a height field z = f(u,v) over the cut plane
    // instead of by the plane itself. A sheet with every control point at zero IS
    // the plane, and this routes straight into perform_with_plane() in that case, so
    // a zero-displacement curved cut runs the same code path as today's flat cut and
    // its output is bit-identical. No connectors on a curved cut in phase 1.
    const ModelObjectPtrs& perform_with_curved_sheet(const CurvedCutSheet& sheet, double thickness = 0.0, CutThicknessOffset offset = CutThicknessOffset::Centred);
    // Draw cut, phase 1: split by a RULED STRIP swept along a stroke the user drew
    // on the model's surface, instead of by the plane or by a height field. The
    // stroke is in the cut plane's own frame, the same frame the sheet lives in.
    //
    // The kerf rides in through `params.thickness` rather than as its own argument:
    // a drawn cut's band is offset along the STRIP's own normal (which varies along
    // the stroke), not along a single plane normal, so there is nothing for a
    // separate `thickness` argument to mean here.
    //
    // No connectors on a drawn cut in phase 1 (phase 2 adds the surface frame the
    // connector path would stand on). A flexi joint dispatches to
    // perform_with_flexi_joints() exactly as it does on a curved cut, for the same
    // reason: the joint's two segments are separated by its own gap between two
    // flat faces, so a flexi cut cannot also be a drawn one.
    const ModelObjectPtrs& perform_with_draw_stroke(const DrawCutStroke& stroke, const DrawCutParams& params);
    // Flexi joint cut: one object, two watertight model parts, a real Manifold boolean.
    // perform_with_plane() dispatches here automatically when a flexi connector is present.
    const ModelObjectPtrs& perform_with_flexi_joints();
    // `src_object` is the object as it was before PartSelection split it: its painting is what the
    // halves get back with KeepPaint (the split pieces carry none). May be null.
    const ModelObjectPtrs& perform_by_contour(const ModelObject* src_object, std::vector<Part> parts, int dowels_count);
    // OrcaSlicer #12318: `groove_count` dovetails side by side along the plane's X, `groove_gap` mm
    // apart; `plane_radius` is the gizmo radius the dovetail's outer width (and so the spacing) is
    // computed from - see calculate_groove_width(). Count 1 is the single dovetail of old.
    const ModelObjectPtrs& perform_with_groove(const Groove&      groove,
                                               const Transform3d& rotation_m,
                                               const int          groove_count,
                                               const float        groove_gap,
                                               const float        plane_radius,
                                               bool               keep_as_parts = false);

    static float calculate_groove_width(const Cut::Groove& groove, const float plane_radius);

}; // namespace Cut

} // namespace Slic3r

#endif /* slic3r_CutUtils_hpp_ */
