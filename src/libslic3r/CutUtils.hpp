#ifndef slic3r_CutUtils_hpp_
#define slic3r_CutUtils_hpp_

#include "enum_bitmask.hpp"
#include "Point.hpp"
#include "Model.hpp"
#include "CurvedCut.hpp"
#include "DrawCut.hpp"
#include "TriangleSelector.hpp"

#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace Slic3r {

using ModelObjectPtrs = std::vector<ModelObject*>;

class AABBMesh;

// ---------------------------------------------------------------------------
// CONNECTORS ON A CURVED OR DRAWN CUT (owner, 2026-10-31).
//
// A connector stands on its own plane: the plane through its point, square to
// its axis. On a flat cut that plane IS the cut. On a curved sheet or a drawn
// strip it is only the tangent plane at the connector's centre, and the real
// cut surface falls away from it (or rises) towards the rim of the connector's
// footprint. A connector is only allowed where that deviation stays within
// CutConnectorMaxSurfaceDeviation; the gizmo marks the others and Cut refuses
// them before anything is cut.
//
// Half a millimetre: about two layers. Over that the halves no longer close
// round a Pin or a TAC Joint without visibly losing engagement on one side. For
// a footprint of radius r it allows a surface radius down to about r^2 / 1 mm
// (a TAC Joint, r ~ 2.9 mm: ~8.5 mm; a 6 mm Pin's hole, r ~ 3.5 mm: ~12 mm).
static constexpr double CutConnectorMaxSurfaceDeviation = 0.5;

// The cut surface a connector stands on, asked two questions in the CUT PLANE's
// frame: which side of the cut a point is on, and where a line meets the surface.
// One per cut kind, so the fit below does not care which kind it is.
class CutSurfaceProbe
{
public:
    enum class Kind { Plane, Curved, Drawn };

    // The flat cut (z == 0 in the plane frame), with its kerf.
    static CutSurfaceProbe plane(double thickness = 0.0, CutThicknessOffset offset = CutThicknessOffset::Centred);
    // The curved sheet, with its kerf (offset along the plane normal, the way curved_cut_split() offsets it).
    static CutSurfaceProbe curved(const CurvedCutSheet& sheet, double thickness = 0.0, CutThicknessOffset offset = CutThicknessOffset::Centred);
    // The drawn strip. `mesh` is the object in the plane frame (it decides a wrap from a plug, as
    // in draw_cut_split()); the kerf comes from params.thickness. Builds the cutter solids.
    static CutSurfaceProbe drawn(const DrawCutStroke& stroke, const DrawCutParams& params, const indexed_triangle_set& mesh);
    // The drawn strip from a cutter the caller already built at face offset 0 (the gizmo's pick
    // cutter, in the plane frame). No kerf faces: is_upper / is_lower use the mid surface.
    static CutSurfaceProbe drawn_from_cutter(indexed_triangle_set cutter, bool closed);

    Kind kind() const { return m_kind; }
    bool valid() const;
    double thickness() const { return m_face_hi - m_face_lo; }

    // Which side of the MID surface (the cut with no kerf) `p` is on.
    bool is_upper_mid(const Vec3d& p) const;
    // Inside the material the upper / lower half keeps (past that half's own kerf face).
    bool is_upper(const Vec3d& p) const;
    bool is_lower(const Vec3d& p) const;
    // Where the line p + t * dir meets the mid surface, the crossing nearest t = 0 within
    // [-reach, reach]. `dir` must be a unit vector. False when there is none.
    bool surface_along(const Vec3d& p, const Vec3d& dir, double reach, double& t) const;

private:
    Kind               m_kind{ Kind::Plane };
    double             m_face_lo{ 0.0 };
    double             m_face_hi{ 0.0 };
    CurvedCutSheet     m_sheet;
    bool               m_closed{ false };
    std::shared_ptr<const indexed_triangle_set> m_cutter_mid;
    std::shared_ptr<const indexed_triangle_set> m_cutter_lo;
    std::shared_ptr<const indexed_triangle_set> m_cutter_hi;
    std::shared_ptr<const AABBMesh>             m_cutter_tree;
};

// How a connector sits on the cut surface. `frame` is the connector's frame in the cut plane's
// frame (no scale): its origin and its +Z axis.
struct CutConnectorFit
{
    // The axis meets the surface near the origin and the two sides of it are told apart.
    bool   on_surface{ false };
    // +Z of the frame points into the UPPER half. When false the connector has to be turned over
    // (Cut does that for the kinds that straddle the cut; the gizmo builds it the right way up).
    bool   upper_along_z{ true };
    // Where the axis meets the surface, along +Z from the origin, mm.
    double centre_offset{ 0.0 };
    // The largest distance, along the axis, between the cut surface and the connector's own plane
    // anywhere over its footprint. Infinite when part of the footprint misses the surface.
    double deviation{ 0.0 };
    // The two halves' faces along the axis turned INTO the upper half, from the origin, mm.
    double face_lo{ 0.0 };
    double face_hi{ 0.0 };

    bool fits() const { return on_surface && deviation <= CutConnectorMaxSurfaceDeviation; }
};
// `axial_reach` is how far along its axis the origin may sit from the surface (a Plug's origin is
// the middle of its body, half its height above the face).
CutConnectorFit cut_connector_fit(const CutSurfaceProbe& probe, const Transform3d& frame, double footprint_radius,
                                  double axial_reach = 0.0);

// The radius of the circle round a connector's footprint, in its own frame, mm: the TAC Joint's
// cutter, the Pin's hole with its mouth flare, the plain kinds' radius and radius tolerance.
// Flexi joints answer 0 (they take their own path and their own patch check).
double cut_connector_footprint_radius(const CutConnector& connector);

// Inside / outside a closed mesh (the object, in whatever frame the caller uses), by ray parity.
// Built once, asked many times. An empty mesh contains nothing.
class CutSolidTest
{
public:
    CutSolidTest() = default;
    explicit CutSolidTest(indexed_triangle_set solid);
    bool empty() const { return !m_tree; }
    bool contains(const Vec3d& p) const;

private:
    std::shared_ptr<const indexed_triangle_set> m_its;
    std::shared_ptr<const AABBMesh>             m_tree;
};

// THE CROSS-SECTION of a curved or drawn cut: the part of the cut `surface` that lies inside
// `object` (both in the same frame). Long triangles are split first, so the section follows the
// object's outline to within about `max_edge` mm. This is what the gizmo shows and picks on
// while connectors are placed: a connector belongs where the cut meets the part, never on the
// parts of the sheet or the drawn strip that hang outside it.
indexed_triangle_set cut_surface_section(const indexed_triangle_set& surface, const CutSolidTest& object, double max_edge);

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

// TAC Joint meshes already have their physical dimensions and clearance. Never apply the
// generic connector radius/height scaling to these reference meshes.
ModelVolume* add_tac_joint_volume(ModelObject* mo, const CutConnector& connector, const std::string& name);

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
