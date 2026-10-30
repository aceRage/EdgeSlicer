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
    // The 0.05 mm preview nudge moves each piece towards the other half; take it out.
    const double up_depth = up.max.z() + 0.05;
    const double lo_depth = -(lo.min.z() - 0.05);
    CHECK(up_depth == Approx(depth + 0.5 * tolerance).margin(1e-3));
    CHECK(lo_depth == Approx(depth + 0.5 * tolerance).margin(1e-3));
    // The pin itself is not lengthened.
    const ModelObject* dowel = result[2];
    BoundingBoxf3 pin;
    for (const ModelVolume* vol : dowel->volumes)
        for (const Vec3f& p : vol->mesh().its.vertices)
            pin.merge(vol->get_matrix() * p.cast<double>());
    CHECK(pin.size().z() == Approx(2.0 * depth).margin(1e-3));
}
