#include <catch2/catch.hpp>
#include <libslic3r/CutUtils.hpp>
#include <libslic3r/CurvedCut.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/TacJoint.hpp>

using namespace Slic3r;

namespace {
ModelObject* tac_cube(Model& model)
{
    auto mesh = its_make_cube(30., 30., 30.);
    for (auto& v : mesh.vertices)
        v -= Vec3f::Constant(15.f);
    auto* object = model.add_object();
    object->name = "TAC test";
    object->add_volume(TriangleMesh(mesh));
    object->add_instance();
    object->cut_id.init();
    return object;
}
CutConnector tac_connector()
{
    CutConnector c;
    c.attribs = CutConnectorAttributes(CutConnectorType::TacJoint, CutConnectorStyle::Prism, CutConnectorShape::Circle);
    c.radius  = 2.7f;
    c.height  = 11.f;
    return c;
}
BoundingBoxf3 tac_box(const ModelVolume& v, const Transform3d& frame = Transform3d::Identity())
{
    return v.mesh().transformed_bounding_box(frame * v.get_matrix());
}
const ModelVolume* tac_hole(const ModelObject& o)
{
    for (const auto* v : o.volumes)
        if (v->is_negative_volume() && v->cut_info.is_tac_joint())
            return v;
    return nullptr;
}
ModelObjectCutAttributes tac_attrs()
{
    return ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::CreateDowels;
}
} // namespace

TEST_CASE("TAC Joint planar cut retains both reference sockets and a printable connector", "[TACJoint][TACJointCut]")
{
    Model      model;
    auto*      object = tac_cube(model);
    const auto c      = tac_connector();
    add_tac_joint_volume(object, c, "TAC Joint");
    Cut         cut(object, 0, Transform3d::Identity(), tac_attrs());
    const auto& result = cut.perform_with_plane();
    REQUIRE(result.size() == 3);
    const auto* upper = tac_hole(*result[0]);
    const auto* lower = tac_hole(*result[1]);
    REQUIRE(upper != nullptr);
    REQUIRE(lower != nullptr);
    const auto expected = TriangleMesh(tac_joint_cutter_mesh(c.tac_joint)).bounding_box();
    REQUIRE(tac_box(*upper).max.z() == Approx(expected.max.z()).margin(1e-4));
    REQUIRE(tac_box(*lower).min.z() == Approx(expected.min.z()).margin(1e-4));
    REQUIRE(tac_box(*upper).min.z() <= 0.);
    REQUIRE(tac_box(*lower).max.z() >= 0.);
    REQUIRE(its_num_open_edges(upper->mesh().its) == 0);
    REQUIRE(its_num_open_edges(lower->mesh().its) == 0);
    const auto* pin = result[2];
    REQUIRE(pin->volumes.size() == 1);
    REQUIRE(pin->volumes[0]->is_model_part());
    REQUIRE(pin->volumes[0]->cut_info.tac_joint == c.tac_joint);
    const auto box = tac_box(*pin->volumes[0], pin->instances[0]->get_matrix());
    REQUIRE(box.min.z() == Approx(0.).margin(1e-4));
    REQUIRE(box.size().z() == Approx(3.34).margin(1e-3));
    REQUIRE(box.max.x() > 15.);
}

TEST_CASE("TAC Joint rotation offset and kerf preserve the assembled connector pose", "[TACJoint][TACJointCut]")
{
    Model model;
    auto* object         = tac_cube(model);
    auto  c              = tac_connector();
    c.tac_joint.offset   = 1.25f;
    c.z_angle            = float(1.5 * M_PI);
    c.pos                = Vec3d(2., -1., 0.);
    const auto* placed   = add_tac_joint_volume(object, c, "TAC Joint");
    const auto  expected = TriangleMesh(tac_joint_connector_mesh(c.tac_joint)).transformed_bounding_box(placed->get_matrix());
    Cut         cut(object, 0, Transform3d::Identity(), tac_attrs());
    const auto& result = cut.perform_with_plane(1.0);
    REQUIRE(result.size() == 3);
    REQUIRE(tac_hole(*result[0]) != nullptr);
    REQUIRE(tac_hole(*result[1]) != nullptr);
    REQUIRE(tac_box(*tac_hole(*result[0])).min.z() <= .5);
    REQUIRE(tac_box(*tac_hole(*result[1])).max.z() >= -.5);
    const auto* pin = result[2];
    REQUIRE(pin->instances[0]->is_assemble_initialized());
    const auto assembled = tac_box(*pin->volumes[0], pin->instances[0]->get_assemble_transformation().get_matrix());
    REQUIRE((assembled.min - expected.min).norm() < 1e-4);
    REQUIRE((assembled.max - expected.max).norm() < 1e-4);
}

TEST_CASE("TAC Joint rejects lost engagement and can retry the same cut", "[TACJoint][TACJointCut]")
{
    Model model;
    auto* object = tac_cube(model);
    add_tac_joint_volume(object, tac_connector(), "TAC Joint");
    Cut cut(object, 0, Transform3d::Identity(), tac_attrs());
    REQUIRE_THROWS_AS(cut.perform_with_plane(12.), std::invalid_argument);
    REQUIRE(object->volumes.size() == 2);
    REQUIRE(cut.perform_with_plane().size() == 3);
}

TEST_CASE("TAC Joint keeps a separate connector when halves are kept as parts", "[TACJoint][TACJointCut]")
{
    Model model;
    auto* object = tac_cube(model);
    add_tac_joint_volume(object, tac_connector(), "TAC Joint");
    Cut         cut(object, 0, Transform3d::Identity(), tac_attrs() | ModelObjectCutAttribute::KeepAsParts);
    const auto& result = cut.perform_with_plane();
    REQUIRE(result.size() == 2);
    size_t sockets = 0;
    for (const auto* volume : result[0]->volumes)
        sockets += volume->is_negative_volume() && volume->cut_info.is_tac_joint();
    REQUIRE(sockets == 2);
    REQUIRE(result[1]->volumes.size() == 1);
    REQUIRE(result[1]->volumes[0]->is_model_part());
}

TEST_CASE("TAC Joint refuses unsupported cut routes before mutating the cut", "[TACJoint][TACJointCut]")
{
    Model model;
    auto* object = tac_cube(model);
    add_tac_joint_volume(object, tac_connector(), "TAC Joint");
    Cut cut(object, 0, Transform3d::Identity(), tac_attrs());
    REQUIRE_THROWS_AS(cut.perform_by_contour(object, {}, 1), std::invalid_argument);
    REQUIRE_THROWS_AS(cut.perform_with_groove(Cut::Groove{}, Transform3d::Identity(), 1, 0.f, 15.f), std::invalid_argument);
    REQUIRE(cut.perform_with_plane().size() == 3);
}

TEST_CASE("TAC Joint on a tilted plane survives print placement and source assembly transforms", "[TACJoint][TACJointCut]")
{
    Model                          model;
    auto*                          object = tac_cube(model);
    const Geometry::Transformation instance(Geometry::translation_transform(Vec3d(30., 40., 20.)) *
                                            Geometry::rotation_transform(Vec3d(.1, .2, .3)) *
                                            Geometry::scale_transform(Vec3d(1.2, 1.1, .9)));
    const Transform3d assembly = Geometry::translation_transform(Vec3d(-20., 5., 9.)) * Geometry::rotation_transform(Vec3d(.3, -.4, -.5));
    object->instances[0]->set_transformation(instance);
    object->instances[0]->set_assemble_from_transform(assembly);
    const Transform3d plane = Geometry::translation_transform(Vec3d(0., 0., 1.)) * Geometry::rotation_transform(Vec3d(.2, -.3, 0.));
    auto              c     = tac_connector();
    c.rotation_m            = Geometry::rotation_transform(Vec3d(.2, -.3, 0.));
    c.pos                   = plane * Vec3d(2., 1., 0.);
    c.z_angle               = .7f;
    c.tac_joint.offset      = -.8f;
    auto*      placed       = add_tac_joint_volume(object, c, "TAC Joint");
    const auto expected     = TriangleMesh(tac_joint_connector_mesh(c.tac_joint))
                              .transformed_bounding_box(assembly * instance.get_matrix_no_offset().inverse() * placed->get_matrix());
    Cut         cut(object, 0, plane, tac_attrs() | ModelObjectCutAttribute::PlaceOnCutUpper | ModelObjectCutAttribute::FlipLower);
    const auto& result = cut.perform_with_plane(.4);
    REQUIRE(result.size() == 3);
    REQUIRE(tac_hole(*result[0]) != nullptr);
    REQUIRE(tac_hole(*result[1]) != nullptr);
    const auto* pin    = result[2];
    const auto  actual = tac_box(*pin->volumes[0], pin->instances[0]->get_assemble_transformation().get_matrix());
    REQUIRE((actual.min - expected.min).norm() < 1e-4);
    REQUIRE((actual.max - expected.max).norm() < 1e-4);
    const auto local_upper = tac_box(*tac_hole(*result[0]), plane.inverse());
    const auto local_lower = tac_box(*tac_hole(*result[1]), plane.inverse());
    REQUIRE(local_upper.min.z() == Approx(.18).margin(1e-4));
    REQUIRE(local_lower.max.z() == Approx(-.18).margin(1e-4));
}

// TAC Joint on a curved sheet, the way the Pin works there (2026-10-31; more in test_cut_connector_surface.cpp).
TEST_CASE("TAC Joint on a curved sheet keeps full sockets on the sheet frame and a printable connector", "[TACJoint][TACJointCut][CurvedCut]")
{
    Model          model;
    auto*          object = tac_cube(model);
    CurvedCutSheet sheet(5);
    sheet.set_half_size(40.0);
    sheet.at(2, 2) = 6.0;
    REQUIRE(!sheet.is_flat());
    const double x = 4., y = -2.;
    auto         c = tac_connector();
    c.pos          = Vec3d(x, y, sheet.evaluate_local(x, y));
    c.rotation_m   = curved_cut_sheet_frame(sheet, x, y);
    const auto* placed = add_tac_joint_volume(object, c, "TAC Joint");
    const Vec3d axis   = placed->get_matrix().linear() * Vec3d::UnitZ();
    REQUIRE(std::abs(axis.z()) < 0.9999); // the sheet really tilts the connector
    const auto expected = TriangleMesh(tac_joint_connector_mesh(c.tac_joint)).transformed_bounding_box(placed->get_matrix());

    Cut         cut(object, 0, Transform3d::Identity(), tac_attrs());
    const auto& result = cut.perform_with_curved_sheet(sheet);
    REQUIRE(result.size() == 3);
    const auto* upper = tac_hole(*result[0]);
    const auto* lower = tac_hole(*result[1]);
    REQUIRE(upper != nullptr);
    REQUIRE(lower != nullptr);
    // Each socket is the whole cutter, standing on the sheet's normal at the connector.
    const double cutter_volume = double(its_volume(tac_joint_cutter_mesh(c.tac_joint)));
    REQUIRE(double(its_volume(upper->mesh().its)) == Approx(cutter_volume).epsilon(1e-4));
    REQUIRE(double(its_volume(lower->mesh().its)) == Approx(cutter_volume).epsilon(1e-4));
    REQUIRE(its_num_open_edges(upper->mesh().its) == 0);
    const auto* pin = result[2];
    REQUIRE(pin->volumes.size() == 1);
    REQUIRE(pin->volumes[0]->is_model_part());
    const auto box = tac_box(*pin->volumes[0], pin->instances[0]->get_matrix());
    REQUIRE(box.min.z() == Approx(0.).margin(1e-4));
    REQUIRE(box.size().z() == Approx(3.34).margin(1e-3));
    REQUIRE(pin->instances[0]->is_assemble_initialized());
    const auto assembled = tac_box(*pin->volumes[0], pin->instances[0]->get_assemble_transformation().get_matrix());
    REQUIRE((assembled.min - expected.min).norm() < 1e-3);
    REQUIRE((assembled.max - expected.max).norm() < 1e-3);
}

TEST_CASE("TAC Joint on a curved sheet still refuses lost engagement before cutting", "[TACJoint][TACJointCut][CurvedCut]")
{
    Model          model;
    auto*          object = tac_cube(model);
    CurvedCutSheet sheet(5);
    sheet.set_half_size(40.0);
    sheet.at(2, 2) = 6.0;
    auto c         = tac_connector();
    c.pos          = Vec3d(0., 0., sheet.evaluate_local(0., 0.));
    c.rotation_m   = curved_cut_sheet_frame(sheet, 0., 0.);
    add_tac_joint_volume(object, c, "TAC Joint");
    Cut cut(object, 0, Transform3d::Identity(), tac_attrs());
    REQUIRE_THROWS_AS(cut.perform_with_curved_sheet(sheet, 12.), std::invalid_argument);
    REQUIRE(object->volumes.size() == 2);
}
