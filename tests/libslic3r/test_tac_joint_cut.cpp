#include <catch2/catch.hpp>
#include <libslic3r/CutUtils.hpp>
#include <libslic3r/CurvedCut.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/CSGMesh/ModelToCSGMesh.hpp>
#include <libslic3r/CSGMesh/PerformCSGMeshBooleans.hpp>
#include <libslic3r/TacJoint.hpp>
#include <libslic3r/TacJointBoolean.hpp>
#include <libslic3r/Format/STL.hpp>
#include <boost/filesystem.hpp>

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

TEST_CASE("TAC Joint sockets can be baked by Mesh Boolean and STL export", "[TACJoint][TACJointCut][TACJointBoolean][slow]")
{
    Model model;
    auto* object       = tac_cube(model);
    auto  c            = tac_connector();
    c.tac_joint.offset = GENERATE(0.f, 1.25f, -.8f);
    c.tac_joint.scale  = GENERATE(.5f, 1.f, 2.f);
    c.z_angle          = GENERATE(0.f, .7f);
    const double kerf  = GENERATE(0., .4);
    CAPTURE(c.tac_joint.offset, c.tac_joint.scale, c.z_angle, kerf);
    // The source cutter and both generated sockets must pass the same CGAL
    // preflight used by the object's Mesh Boolean action and STL export.
    REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(TriangleMesh(tac_joint_cutter_mesh(c.tac_joint))));
    add_tac_joint_volume(object, c, "TAC Joint");
    Cut         cut(object, 0, Transform3d::Identity(), tac_attrs());
    const auto& result = cut.perform_with_plane(kerf);
    REQUIRE(result.size() == 3);
    for (size_t half = 0; half < 2; ++half) {
        CAPTURE(half);
        std::vector<csg::CSGPart> parts;
        csg::model_to_csgmesh(*result[half], Transform3d::Identity(), std::back_inserter(parts), csg::mpartsPositive | csg::mpartsNegative);
        const auto status = csg::check_csgmesh_booleans(range(parts));
        CAPTURE(std::get<1>(status));
        REQUIRE(std::get<0>(status) == csg::BooleanFailReason::OK);
        for (bool manual_boolean : {true, false}) {
            CAPTURE(manual_boolean);
            if (!manual_boolean) {
                parts.clear();
                csg::model_to_csgmesh(*result[half], Transform3d::Identity(), std::back_inserter(parts),
                                      csg::mpartsPositive | csg::mpartsNegative | csg::mpartsDoSplits);
                REQUIRE(std::get<0>(csg::check_csgmesh_booleans(range(parts))) == csg::BooleanFailReason::OK);
            }
            // Both actions share the robust TAC backend after their usual preflight.
            REQUIRE(has_tac_joint_volumes(*result[half]));
            TriangleMesh mesh = bake_tac_joint_object(*result[half]);
            REQUIRE_FALSE(mesh.empty());
            REQUIRE(its_num_open_edges(mesh.its) == 0);
            REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(mesh));
            // Each 30 x 30 half loses material to its socket, not only to the kerf.
            REQUIRE(mesh.volume() < 30. * 30. * (15. - .5 * kerf) - 1.);
            REQUIRE(mesh.volume() > 30. * 30. * (15. - .5 * kerf) - 300. * std::pow(c.tac_joint.scale, 3));
            const auto file = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tac-boolean-%%%%-%%%%.stl");
            REQUIRE(store_stl(file.string().c_str(), &mesh, true));
            Model      imported;
            const bool loaded = load_stl(file.string().c_str(), &imported);
            boost::filesystem::remove(file);
            REQUIRE(loaded);
            REQUIRE(imported.objects.size() == 1);
            auto restored = imported.objects.front()->raw_mesh();
            REQUIRE(its_num_open_edges(restored.its) == 0);
            // STL import caches ADMesh's float volume estimate. Compare the actual
            // triangles with the same double-precision calculation on both sides.
            REQUIRE(its_mass_properties(restored.its).volume == Approx(its_mass_properties(mesh.its).volume).epsilon(1e-6));
        }
    }
}

TEST_CASE("An attached TAC Joint belongs to one half and leaves only the opposite socket", "[TACJoint][TACJointCut][TACAttachment]")
{
    const int owner = GENERATE(1, 2);
    CAPTURE(owner);
    Model model;
    auto* object       = tac_cube(model);
    auto  c            = tac_connector();
    c.tac_joint        = tac_joint_from_attributes([owner](const std::string& name) -> std::optional<double> {
        return name == "tac_joint_attachment" ? std::optional<double>(owner) : std::nullopt;
    });
    c.tac_joint.offset = GENERATE(0.f, 1.25f, -.8f);
    c.z_angle          = GENERATE(0.f, .7f);
    const double kerf  = GENERATE(0., .4);
    CAPTURE(c.tac_joint.offset, c.z_angle, kerf);
    add_tac_joint_volume(object, c, "TAC Joint");
    Cut         cut(object, 0, Transform3d::Identity(), tac_attrs());
    const auto& result = cut.perform_with_plane(kerf);
    REQUIRE(result.size() == 2);
    REQUIRE(tac_hole(*result[owner - 1]) == nullptr);
    REQUIRE(tac_hole(*result[2 - owner]) != nullptr);
    size_t attached = 0;
    for (const auto* v : result[owner - 1]->volumes)
        if (v->is_model_part() && v->cut_info.is_tac_joint()) {
            ++attached;
            const auto box = tac_box(*v);
            REQUIRE(box.min.z() == Approx(-5.5 + c.tac_joint.offset).margin(1e-4));
            REQUIRE(box.max.z() == Approx(5.5 + c.tac_joint.offset).margin(1e-4));
        }
    REQUIRE(attached == 1);
    // Manual Mesh Boolean must produce one continuous body with an integral pin.
    std::vector<csg::CSGPart> parts;
    csg::model_to_csgmesh(*result[owner - 1], Transform3d::Identity(), std::back_inserter(parts), csg::mpartsPositive | csg::mpartsNegative);
    REQUIRE(std::get<0>(csg::check_csgmesh_booleans(range(parts))) == csg::BooleanFailReason::OK);
    TriangleMesh mesh = bake_tac_joint_object(*result[owner - 1]);
    REQUIRE(its_num_open_edges(mesh.its) == 0);
    REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(mesh));
    REQUIRE(mesh.split().size() == 1);
    REQUIRE(mesh.volume() > 30. * 30. * (15. - .5 * kerf) + 1.);
    REQUIRE(mesh.volume() < 30. * 30. * (15. - .5 * kerf) + 300.);
}

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
    // The separate printable pin also takes the TAC STL path. A union of its
    // single positive volume must preserve the reference geometry.
    const auto baked_pin = bake_tac_joint_object(*pin);
    REQUIRE(its_num_open_edges(baked_pin.its) == 0);
    REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(baked_pin));
    REQUIRE(its_mass_properties(baked_pin.its).volume == Approx(its_mass_properties(pin->raw_mesh().its).volume).epsilon(1e-6));
}

TEST_CASE("Attached TAC ownership follows retained halves even without CreateDowels", "[TACJoint][TACAttachment]")
{
    const auto attachment = GENERATE(TacJointAttachment::Upper, TacJointAttachment::Lower);
    const bool keep_upper = GENERATE(true, false);
    Model      model;
    auto*      object      = tac_cube(model);
    auto       c           = tac_connector();
    c.tac_joint.attachment = attachment;
    add_tac_joint_volume(object, c, "TAC Joint");
    Cut cut(object, 0, Transform3d::Identity(), keep_upper ? ModelObjectCutAttribute::KeepUpper : ModelObjectCutAttribute::KeepLower);
    const auto& result = cut.perform_with_plane();
    REQUIRE(result.size() == 1);
    const bool owns = keep_upper == (attachment == TacJointAttachment::Upper);
    REQUIRE((tac_hole(*result.front()) == nullptr) == owns);
    size_t pins = 0;
    for (const auto* v : result.front()->volumes)
        pins += v->is_model_part() && v->cut_info.is_tac_joint();
    REQUIRE(pins == (owns ? 1 : 0));
}

TEST_CASE("An attached TAC refuses object-wide sockets from KeepAsParts", "[TACJoint][TACAttachment]")
{
    const auto attachment = GENERATE(TacJointAttachment::Upper, TacJointAttachment::Lower);
    Model      model;
    auto*      object      = tac_cube(model);
    auto       c           = tac_connector();
    c.tac_joint.attachment = attachment;
    add_tac_joint_volume(object, c, "TAC Joint");
    Cut cut(object, 0, Transform3d::Identity(), tac_attrs() | ModelObjectCutAttribute::KeepAsParts);
    REQUIRE_THROWS_AS(cut.perform_with_plane(), std::invalid_argument);
    CurvedCutSheet sheet(5);
    sheet.set_half_size(40.);
    REQUIRE_THROWS_AS(cut.perform_with_curved_sheet(sheet), std::invalid_argument);
    REQUIRE(object->volumes.size() == 2);
    Cut retry(object, 0, Transform3d::Identity(), tac_attrs());
    REQUIRE(retry.perform_with_plane().size() == 2);
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
    c.tac_joint.attachment  = GENERATE(TacJointAttachment::Separate, TacJointAttachment::Upper, TacJointAttachment::Lower);
    auto*      placed       = add_tac_joint_volume(object, c, "TAC Joint");
    const auto expected     = TriangleMesh(tac_joint_connector_mesh(c.tac_joint))
                              .transformed_bounding_box(assembly * instance.get_matrix_no_offset().inverse() * placed->get_matrix());
    Cut         cut(object, 0, plane, tac_attrs() | ModelObjectCutAttribute::PlaceOnCutUpper | ModelObjectCutAttribute::FlipLower);
    const auto& result = cut.perform_with_plane(.4);
    const bool  separate = c.tac_joint.attachment == TacJointAttachment::Separate;
    REQUIRE(result.size() == (separate ? 3 : 2));
    const auto*        pin        = result[separate ? 2 : (c.tac_joint.attachment == TacJointAttachment::Upper ? 0 : 1)];
    const ModelVolume* pin_volume = nullptr;
    for (const auto* v : pin->volumes)
        if (v->is_model_part() && v->cut_info.is_tac_joint())
            pin_volume = v;
    REQUIRE(pin_volume != nullptr);
    const auto actual = tac_box(*pin_volume, pin->instances[0]->get_assemble_transformation().get_matrix());
    REQUIRE((actual.min - expected.min).norm() < 1e-4);
    REQUIRE((actual.max - expected.max).norm() < 1e-4);
    if (c.tac_joint.attachment != TacJointAttachment::Upper) {
        REQUIRE(tac_hole(*result[0]) != nullptr);
        REQUIRE(tac_box(*tac_hole(*result[0]), plane.inverse()).min.z() == Approx(.18).margin(1e-4));
    }
    if (c.tac_joint.attachment != TacJointAttachment::Lower) {
        REQUIRE(tac_hole(*result[1]) != nullptr);
        REQUIRE(tac_box(*tac_hole(*result[1]), plane.inverse()).max.z() == Approx(-.18).margin(1e-4));
    }
}

TEST_CASE("TAC placement is independent for several connectors in one cut", "[TACJoint][TACAttachment]")
{
    Model model;
    auto* object = tac_cube(model);
    for (int mode = 0; mode < 3; ++mode) {
        auto c                 = tac_connector();
        c.pos                  = Vec3d(8. * (mode - 1), 0., 0.);
        c.z_angle              = .3f * mode;
        c.tac_joint.attachment = TacJointAttachment(mode);
        add_tac_joint_volume(object, c, "TAC Joint-" + std::to_string(mode));
    }
    Cut         cut(object, 0, Transform3d::Identity(), tac_attrs());
    const auto& result = cut.perform_with_plane();
    REQUIRE(result.size() == 3);
    for (size_t half = 0; half < 2; ++half) {
        size_t holes = 0, pins = 0;
        for (const auto* v : result[half]->volumes) {
            holes += v->is_negative_volume();
            pins += v->is_model_part() && v->cut_info.is_tac_joint();
        }
        REQUIRE(holes == 2);
        REQUIRE(pins == 1);
        std::vector<csg::CSGPart> parts;
        csg::model_to_csgmesh(*result[half], Transform3d::Identity(), std::back_inserter(parts), csg::mpartsPositive | csg::mpartsNegative);
        REQUIRE(std::get<0>(csg::check_csgmesh_booleans(range(parts))) == csg::BooleanFailReason::OK);
        auto mesh = bake_tac_joint_object(*result[half]);
        REQUIRE(its_num_open_edges(mesh.its) == 0);
        REQUIRE(mesh.split().size() == 1);
        REQUIRE(mesh.volume() < 13499.);
    }
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
