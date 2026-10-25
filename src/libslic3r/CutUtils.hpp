#ifndef slic3r_CutUtils_hpp_
#define slic3r_CutUtils_hpp_

#include "enum_bitmask.hpp"
#include "Point.hpp"
#include "Model.hpp"
#include "CurvedCut.hpp"
#include "DrawCut.hpp"
#include "TriangleSelector.hpp"

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

// OD11 PR D (owner D2 = a, 2026-10-24): "Edit cut" rebuilds the uncut part from the recipe's bare
// mesh, so without this a re-cut painted part came back unpainted. Carries the support / seam /
// colour / fuzzy-skin painting of the halves - as they are now, so paint added after the cut
// comes too - back onto `uncut`.
//
// THE FRAMES. A half's model part, through its own volume matrix, is in the cut frame: the cut
// instance's linear transform (`instance_linear`, recorded in the recipe) times the object frame
// the recipe's mesh is in. That survives a project save and reload (the volume matrix is stored),
// moving or turning the half on the bed (that is the instance), Flip and Place on cut (also the
// instance). It does not survive moving a part inside a half, or an unrecorded instance_linear
// on a cut made from a rotated or scaled instance - so the carry checks before it paints: the
// painted points must land on the uncut surface. If they do not, nothing is painted and
// `carried` is false (the caller says so: fallback (c), "dropped").
struct CutPaintCarry
{
    bool had_paint{ false }; // some half had painting to carry
    bool carried{ false };   // ... and it was put on `uncut`
};
CutPaintCarry carry_cut_painting_to_uncut(const std::vector<const ModelObject*>& halves, const Transform3d& instance_linear,
                                          ModelVolume& uncut);
// Records `half`'s model-part volume matrices into its own cut recipe (CutRecipe::half_part_matrices),
// right after the cut, while they still ARE the cut frame. No-op without a recipe.
void cut_recipe_record_half_frame(ModelObject& half);

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
