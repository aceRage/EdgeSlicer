// Port of OrcaSlicer #16514 ("Tests: add unit tests for cut connectors covering plug, dowel, and
// snap", tests only, on top of #16020), adapted to this tree's Catch2 v2 (catch.hpp, Approx
// instead of WithinAbs). EdgeSlicer adds the last test case: upstream's cases only push
// connectors onto ModelObject::cut_connectors, which the Cut itself never reads, so none of them
// can see #16020's fix. That one builds the Dowel volume the way the gizmo does and measures both
// holes.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

namespace {

ModelObject* make_cube_object(Model& model)
{
    TriangleMesh mesh = make_cube(40.0, 40.0, 40.0);
    mesh.translate(Vec3f(-20.f, -20.f, -20.f));
    ModelObject* obj = model.add_object();
    obj->add_volume(mesh);
    obj->add_instance();
    return obj;
}

Transform3d plane_cut_matrix()
{
    return Transform3d::Identity();
}

CutConnector make_connector(CutConnectorType type, float height, float height_tolerance, float radius = 3.0f)
{
    CutConnector c;
    c.pos              = Vec3d(0.0, 0.0, 0.0);
    c.rotation_m       = Transform3d::Identity();
    c.radius           = radius;
    c.height           = height;
    c.radius_tolerance = 0.f;
    c.height_tolerance = height_tolerance;
    c.z_angle          = 0.f;
    c.attribs          = CutConnectorAttributes(type, CutConnectorStyle::Prism, CutConnectorShape::Circle);
    return c;
}

} // namespace

TEST_CASE("Cut with Dowel connector produces two parts", "[CutConnectors]")
{
    Model model;
    ModelObject* obj = make_cube_object(model);
    obj->cut_connectors.push_back(make_connector(CutConnectorType::Dowel, 10.f, 0.f));

    const ModelObjectCutAttributes attrs =
        ModelObjectCutAttribute::KeepUpper |
        ModelObjectCutAttribute::KeepLower |
        ModelObjectCutAttribute::CreateDowels;

    Cut cut(obj, 0, plane_cut_matrix(), attrs);
    const ModelObjectPtrs& result = cut.perform_with_plane();

    REQUIRE(result.size() >= 2);
}

TEST_CASE("Cut with Plug connector produces two parts", "[CutConnectors]")
{
    Model model;
    ModelObject* obj = make_cube_object(model);
    obj->cut_connectors.push_back(make_connector(CutConnectorType::Plug, 10.f, 0.f));

    const ModelObjectCutAttributes attrs =
        ModelObjectCutAttribute::KeepUpper |
        ModelObjectCutAttribute::KeepLower;

    Cut cut(obj, 0, plane_cut_matrix(), attrs);
    const ModelObjectPtrs& result = cut.perform_with_plane();

    REQUIRE(result.size() >= 2);
}

TEST_CASE("Cut with Snap connector produces two parts", "[CutConnectors]")
{
    Model model;
    ModelObject* obj = make_cube_object(model);
    obj->cut_connectors.push_back(make_connector(CutConnectorType::Snap, 10.f, 0.f));

    const ModelObjectCutAttributes attrs =
        ModelObjectCutAttribute::KeepUpper |
        ModelObjectCutAttribute::KeepLower;

    Cut cut(obj, 0, plane_cut_matrix(), attrs);
    const ModelObjectPtrs& result = cut.perform_with_plane();

    REQUIRE(result.size() >= 2);
}

TEST_CASE("Cut with no connectors produces upper and lower parts", "[CutConnectors]")
{
    Model model;
    ModelObject* obj = make_cube_object(model);

    const ModelObjectCutAttributes attrs =
        ModelObjectCutAttribute::KeepUpper |
        ModelObjectCutAttribute::KeepLower;

    Cut cut(obj, 0, plane_cut_matrix(), attrs);
    const ModelObjectPtrs& result = cut.perform_with_plane();

    REQUIRE(result.size() >= 2);

    bool has_upper = false;
    bool has_lower = false;
    for (const ModelObject* part : result) {
        for (const ModelVolume* vol : part->volumes) {
            // Object coordinates (the volume may be re-centred on load).
            const Transform3d m = vol->get_matrix();
            for (const Vec3f& v : vol->mesh().its.vertices) {
                const Vec3d p = m * v.cast<double>();
                if (p.z() > 1e-6) has_upper = true;
                if (p.z() < -1e-6) has_lower = true;
            }
        }
    }
    CHECK(has_upper);
    CHECK(has_lower);
}

TEST_CASE("Cut connector height_tolerance field stores assigned value", "[CutConnectors]")
{
    const float tolerance = GENERATE(0.f, 0.1f, 0.2f, 0.35f, 0.5f);
    DYNAMIC_SECTION("height_tolerance = " << tolerance)
    {
        CutConnector c = make_connector(CutConnectorType::Dowel, 10.f, tolerance);
        CHECK(double(c.height_tolerance) == Approx(double(tolerance)).margin(1e-6));
        CHECK(double(c.height) == Approx(10.0).margin(1e-6));
    }
}

TEST_CASE("CutConnector default construction has valid fields", "[CutConnectors]")
{
    CutConnector c;
    CHECK(c.radius > 0.f);
    CHECK(c.height > 0.f);
    CHECK(c.height_tolerance >= 0.f);
    CHECK(c.radius_tolerance >= 0.f);
}

TEST_CASE("CutConnector copy constructor preserves all fields", "[CutConnectors]")
{
    CutConnector orig = make_connector(CutConnectorType::Plug, 8.f, 0.15f, 4.f);
    CutConnector copy = orig;

    CHECK(double(copy.height) == Approx(8.0).margin(1e-6));
    CHECK(double(copy.height_tolerance) == Approx(0.15).margin(1e-6));
    CHECK(double(copy.radius) == Approx(4.0).margin(1e-6));
    CHECK(copy.attribs.type == CutConnectorType::Plug);
    CHECK(copy.attribs.shape == CutConnectorShape::Circle);

    // EdgeSlicer: the push-fit Pin's parameter block is copied too.
    CutConnector pin = make_connector(CutConnectorType::PushFit, 12.f, 0.f);
    pin.pushfit.diameter     = 8.f;
    pin.pushfit.length_lower = 4.5f;
    const CutConnector pin_copy = pin;
    CHECK(pin_copy.pushfit == pin.pushfit);
}

TEST_CASE("CutConnector fields match between two identically constructed connectors", "[CutConnectors]")
{
    CutConnector a = make_connector(CutConnectorType::Dowel, 10.f, 0.1f);
    CutConnector b = make_connector(CutConnectorType::Dowel, 10.f, 0.1f);
    CutConnector c = make_connector(CutConnectorType::Plug,  10.f, 0.1f);

    CHECK(double(a.height) == Approx(double(b.height)).margin(1e-6));
    CHECK(double(a.height_tolerance) == Approx(double(b.height_tolerance)).margin(1e-6));
    CHECK(double(a.radius) == Approx(double(b.radius)).margin(1e-6));
    CHECK(a.attribs.type == b.attribs.type);

    CHECK(a.attribs.type != c.attribs.type);
}

TEST_CASE("Issue #14512 regression: plane cut at z=0 produces non-empty upper and lower parts", "[CutConnectors][Regression]")
{
    Model model;
    ModelObject* obj = make_cube_object(model);
    obj->cut_connectors.push_back(make_connector(CutConnectorType::Dowel, 10.f, 0.2f));

    const ModelObjectCutAttributes attrs =
        ModelObjectCutAttribute::KeepUpper |
        ModelObjectCutAttribute::KeepLower |
        ModelObjectCutAttribute::CreateDowels;

    Cut cut(obj, 0, plane_cut_matrix(), attrs);
    const ModelObjectPtrs& result = cut.perform_with_plane();

    REQUIRE(result.size() >= 2);
    for (const ModelObject* part : result) {
        bool has_volumes = !part->volumes.empty();
        CHECK(has_volumes);
        if (has_volumes)
            CHECK_FALSE(part->volumes.front()->mesh().its.vertices.empty());
    }
}

// EdgeSlicer: #16020 itself. The Dowel volume is built the way GLGizmoCut3D::apply_cut_connectors
// builds it (a unit cylinder, centred, scaled by (r, r, h) with h = 2 x the Depth field), so the
// cut really applies the tolerance. Each hole must then be depth + tolerance/2 deep from the cut
// face - before #16020 the upper one got the whole tolerance and the lower one none.
TEST_CASE("Dowel depth tolerance is split evenly between the two holes (Orca #16020)", "[CutConnectors][Regression]")
{
    const double depth     = 5.0;   // the panel's per-side Depth
    const double tolerance = 1.0;
    Model model;
    ModelObject* obj = make_cube_object(model);
    ModelVolume* v = obj->add_volume(TriangleMesh(its_make_cylinder(1.0, 1.0)), ModelVolumeType::NEGATIVE_VOLUME);
    v->set_transformation(Geometry::scale_transform(Vec3d(3.0, 3.0, 2.0 * depth)));
    v->cut_info = ModelVolume::CutInfo(CutConnectorType::Dowel, 0.f, float(tolerance));

    Cut cut(obj, 0, plane_cut_matrix(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::CreateDowels);
    const ModelObjectPtrs& result = cut.perform_with_plane();
    REQUIRE(result.size() == 3);

    auto hole_extent = [](const ModelObject* o) {
        BoundingBoxf3 bb;
        for (const ModelVolume* vol : o->volumes)
            if (vol->type() == ModelVolumeType::NEGATIVE_VOLUME) {
                const Transform3d m = vol->get_matrix();
                for (const Vec3f& p : vol->mesh().its.vertices)
                    bb.merge(m * p.cast<double>());
            }
        return bb;
    };
    const BoundingBoxf3 up = hole_extent(result[0]);
    const BoundingBoxf3 lo = hole_extent(result[1]);
    REQUIRE(up.defined);
    REQUIRE(lo.defined);
    // Orca #16893: each hole keeps its full depth and reaches CONNECTOR_HOLE_OVERLAP out of its face.
    const double up_depth = up.max.z();
    const double lo_depth = -lo.min.z();
    CHECK(up_depth == Approx(depth + 0.5 * tolerance).margin(1e-3));
    CHECK(lo_depth == Approx(depth + 0.5 * tolerance).margin(1e-3));
    CHECK(up.min.z() == Approx(-CONNECTOR_HOLE_OVERLAP).margin(1e-3));
    CHECK(lo.max.z() == Approx(CONNECTOR_HOLE_OVERLAP).margin(1e-3));
    // The pin itself is not lengthened.
    const ModelObject* dowel = result[2];
    BoundingBoxf3 pin;
    for (const ModelVolume* vol : dowel->volumes)
        for (const Vec3f& p : vol->mesh().its.vertices)
            pin.merge(vol->get_matrix() * p.cast<double>());
    CHECK(pin.size().z() == Approx(2.0 * depth).margin(1e-3));
}

// Orca #16893: a plug's hole grows by the tolerance away from the cut face and by the overlap through
// it, so it spans from CONNECTOR_HOLE_OVERLAP below the face to depth + tolerance above it; the peg on
// the other half is not changed.
TEST_CASE("Plug hole breaks through the cut face and keeps its full depth (Orca #16893)", "[CutConnectors][Regression]")
{
    const double depth     = 5.0;
    const double tolerance = 0.4;
    Model model;
    ModelObject* obj = make_cube_object(model);
    ModelVolume* v = obj->add_volume(TriangleMesh(its_make_cylinder(1.0, 1.0)), ModelVolumeType::NEGATIVE_VOLUME);
    // A plug sits on one side of the plane: centre at half its height.
    v->set_transformation(Geometry::translation_transform(Vec3d(0., 0., 0.5 * depth)) * Geometry::scale_transform(Vec3d(3.0, 3.0, depth)));
    v->cut_info = ModelVolume::CutInfo(CutConnectorType::Plug, 0.f, float(tolerance));

    Cut cut(obj, 0, plane_cut_matrix(), ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& result = cut.perform_with_plane();
    REQUIRE(result.size() == 2);

    auto extent = [](const ModelObject* o, ModelVolumeType type) {
        BoundingBoxf3 bb;
        for (const ModelVolume* vol : o->volumes)
            if (vol->cut_info.is_connector && vol->type() == type) {
                const Transform3d m = vol->get_matrix();
                for (const Vec3f& p : vol->mesh().its.vertices)
                    bb.merge(m * p.cast<double>());
            }
        return bb;
    };
    const BoundingBoxf3 hole = extent(result[0], ModelVolumeType::NEGATIVE_VOLUME);
    const BoundingBoxf3 peg  = extent(result[1], ModelVolumeType::MODEL_PART);
    REQUIRE(hole.defined);
    REQUIRE(peg.defined);
    CHECK(hole.min.z() == Approx(-CONNECTOR_HOLE_OVERLAP).margin(1e-3));
    CHECK(hole.max.z() == Approx(depth + tolerance).margin(1e-3));
    CHECK(peg.min.z() == Approx(0.).margin(1e-3));
    CHECK(peg.max.z() == Approx(depth).margin(1e-3));
}

// ---------------------------------------------------------------------------
// The Cut gizmo's connector decisions (GLGizmoCut3D::apply_cut_connectors(),
// has_connector_conflict(), the dovetail's first depth), tested without the GUI.
// ---------------------------------------------------------------------------
namespace {

// The gizmo's unit shape for a Plug / Dowel prism with `sectors` sides (get_connector_mesh()).
indexed_triangle_set unit_prism(int sectors) { return its_make_cylinder(1.0, 1.0, 2. * PI / sectors); }

CutConnector shaped_connector(CutConnectorShape shape, float z_angle_deg)
{
    CutConnector c;
    c.pos     = Vec3d(3., 4., 0.);
    c.radius  = 5.f;
    c.height  = 6.f;
    c.z_angle = float(Geometry::deg2rad(double(z_angle_deg)));
    c.attribs = CutConnectorAttributes(CutConnectorType::Plug, CutConnectorStyle::Prism, shape);
    return c;
}

// The corners of the shape the gizmo DRAWS for `c` (render_connectors(): translation, the frame, the
// Rotation, radius x radius x height applied to the unit shape), seen from above.
std::vector<Vec2d> drawn_corners(const CutConnector& c, int sectors)
{
    const Transform3d m = Geometry::translation_transform(c.pos) * c.rotation_m *
                          Geometry::rotation_transform(-double(c.z_angle) * Vec3d::UnitZ()) *
                          Geometry::scale_transform(Vec3d(c.radius, c.radius, c.height));
    std::vector<Vec2d> out;
    for (const Vec3f& v : unit_prism(sectors).vertices)
        if (v.head<2>().norm() > 0.5f && std::abs(v.z()) < 1e-6f) // the rim at the bottom, not the axis
            out.push_back((m * v.cast<double>()).head<2>());
    return out;
}

bool near_one_of(const Vec2d& p, const std::vector<Vec2d>& pts, double eps = 1e-4)
{
    return std::any_of(pts.begin(), pts.end(), [&](const Vec2d& q) { return (p - q).norm() < eps; });
}

} // namespace

TEST_CASE("Cut connector volume: a triangle lands where it is drawn", "[CutConnectors][Regression]")
{
    // A triangle's box is not centred on its axis (its corners reach 1 one way, 0.5 the other).
    // Centring the volume's mesh on that box moved the connector a quarter of its radius off the
    // point placed.
    for (const float z_angle : { 0.f, 30.f }) {
        CAPTURE(z_angle);
        const CutConnector c = shaped_connector(CutConnectorShape::Triangle, z_angle);
        Model        model;
        ModelObject* mo = model.add_object();
        mo->add_volume(make_cube(40., 40., 40.));
        mo->add_instance();
        ModelVolume* v = add_cut_connector_volume(*mo, unit_prism(3), c);
        REQUIRE(v->type() == ModelVolumeType::NEGATIVE_VOLUME);

        TriangleMesh m = v->mesh();
        m.transform(v->get_matrix());
        const std::vector<Vec2d> drawn = drawn_corners(c, 3);
        REQUIRE(drawn.size() == 3);
        for (const Vec3f& p : m.its.vertices)
            if ((p.head<2>().cast<double>() - c.pos.head<2>()).norm() > 0.5 * c.radius)
                CHECK(near_one_of(p.head<2>().cast<double>(), drawn));
        // The height is centred on the connector's position, as apply_connectors_in_model() expects.
        const BoundingBoxf3 bb = m.bounding_box();
        CHECK(bb.min.z() == Approx(c.pos.z() - 0.5 * c.height));
        CHECK(bb.max.z() == Approx(c.pos.z() + 0.5 * c.height));
    }
}

TEST_CASE("Cut connector outline: tested turned by its Rotation", "[CutConnectors][Regression]")
{
    // The outline tested against the cut contour ignored the Rotation: a turned triangle or square
    // that pokes out of the part was tested in its unturned place, and passed.
    for (const auto& [shape, sectors] : { std::pair{ CutConnectorShape::Square, 4 }, std::pair{ CutConnectorShape::Triangle, 3 } })
        for (const float z_angle : { 0.f, 45.f, 100.f }) {
            CAPTURE(sectors, z_angle);
            CutConnector c = shaped_connector(shape, z_angle);
            c.pos          = Vec3d::Zero(); // the outline is in the connector's own frame
            const std::vector<Vec3f> outline = cut_connector_outline(c);
            REQUIRE(outline.size() == size_t(sectors));
            const std::vector<Vec2d> drawn = drawn_corners(c, sectors);
            for (const Vec3f& p : outline)
                CHECK(near_one_of(p.head<2>().cast<double>(), drawn));
        }
    // A circle is still 60 points at its radius.
    const std::vector<Vec3f> circle = cut_connector_outline(shaped_connector(CutConnectorShape::Circle, 20.f));
    CHECK(circle.size() == 60);
    for (const Vec3f& p : circle)
        CHECK(p.norm() == Approx(5.f));
}

TEST_CASE("Cut dovetail: the first depth comes from the object", "[CutConnectors][Regression]")
{
    // It came from the grabber size, which is fixed on screen (32 px / zoom): the groove first
    // offered depended on how far the camera was zoomed out when the tool opened.
    CHECK(cut_groove_default_depth(BoundingBoxf3(Vec3d::Zero(), Vec3d(40., 40., 40.))) == Approx(2.f));
    CHECK(cut_groove_default_depth(BoundingBoxf3(Vec3d(-100., -50., 0.), Vec3d(100., 50., 60.))) == Approx(6.f));
    // Never under 1 mm.
    CHECK(cut_groove_default_depth(BoundingBoxf3(Vec3d::Zero(), Vec3d(5., 5., 5.))) == Approx(1.f));
}
