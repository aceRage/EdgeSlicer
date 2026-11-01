#include <catch2/catch.hpp>

#include <libslic3r/CutRecipe.hpp>
#include <libslic3r/CutUtils.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PushFit.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Utils.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace Slic3r;

// Push-fit "Pin" connector, PR 2 (tests/design_pushfit_connector.md): the cut (T5), the 3MF and
// the recipe (T6), re-cutting from the recipe (T7), and C1 - every cut result records where it
// sits when assembled (tests/design_showcase_assembly_buildup.md, D8).

namespace {

constexpr double CUBE = 30.0;

indexed_triangle_set centred_cube()
{
    indexed_triangle_set its = its_make_cube(CUBE, CUBE, CUBE);
    for (Vec3f& v : its.vertices)
        v -= Vec3f(float(0.5 * CUBE), float(0.5 * CUBE), float(0.5 * CUBE));
    return its;
}

ModelObject* make_cube(Model& model, const Geometry::Transformation& inst = Geometry::Transformation())
{
    ModelObject* mo = model.add_object();
    mo->name        = "cube";
    mo->add_volume(TriangleMesh(centred_cube()))->name = "part";
    mo->add_instance()->set_transformation(inst);
    mo->cut_id.init();
    return mo;
}

PushFitParams pin_params(float lu = 7.f, float ll = 5.f)
{
    PushFitParams p = pushfit_apply_preset(PushFitParams{}, PushFitPreset::M);
    p.length_upper  = lu;
    p.length_lower  = ll;
    return p;
}

CutConnector pin_connector(const PushFitParams& p, const Vec3d& pos = Vec3d::Zero(),
                           const Transform3d& rot = Transform3d::Identity())
{
    CutConnector c;
    c.pos        = pos;
    c.rotation_m = rot;
    c.radius     = 0.5f * p.diameter;
    c.height     = p.length_upper + p.length_lower;
    c.attribs    = CutConnectorAttributes(CutConnectorType::PushFit, CutConnectorStyle::Prism, CutConnectorShape::Circle);
    c.pushfit    = p;
    return c;
}

// A Dowel connector volume exactly as the gizmo makes one (unit cylinder, scaled).
void add_dowel_volume(ModelObject* mo, const Vec3d& pos, float r, float h)
{
    ModelVolume* v = mo->add_volume(TriangleMesh(its_make_cylinder(1.0, 1.0)), ModelVolumeType::NEGATIVE_VOLUME);
    v->set_transformation(Geometry::translation_transform(pos) *
                          Geometry::scale_transform(Vec3d(double(r), double(r), double(h))));
    v->cut_info = ModelVolume::CutInfo(CutConnectorType::Dowel, 0.f, 0.f);
    v->name     = "Connector-dowel";
}

ModelObjectCutAttributes ATTRS_ALL()
{
    return ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::CreateDowels;
}

BoundingBoxf3 volume_bbox(const ModelVolume* v, const Transform3d& pre = Transform3d::Identity())
{
    BoundingBoxf3 bb;
    const Transform3d m = pre * v->get_matrix();
    for (const Vec3f& p : v->mesh().its.vertices)
        bb.merge(m * p.cast<double>());
    return bb;
}

// Model parts only, through `pre` (an instance matrix, or an assembled pose).
BoundingBoxf3 parts_bbox(const ModelObject* o, const Transform3d& pre)
{
    BoundingBoxf3 bb;
    for (const ModelVolume* v : o->volumes)
        if (v->is_model_part())
            bb.merge(volume_bbox(v, pre));
    return bb;
}

const ModelVolume* pin_hole(const ModelObject* o)
{
    const ModelVolume* found = nullptr;
    int                n     = 0;
    for (const ModelVolume* v : o->volumes)
        if (v->type() == ModelVolumeType::NEGATIVE_VOLUME && v->cut_info.is_pushfit()) {
            found = v;
            ++n;
        }
    return n == 1 ? found : nullptr;
}

const ModelObject* find_named(const ModelObjectPtrs& res, const std::string& key)
{
    for (const ModelObject* o : res)
        if (o->name.find(key) != std::string::npos)
            return o;
    return nullptr;
}

bool same_box(const BoundingBoxf3& a, const BoundingBoxf3& b, double eps = 1e-3)
{
    return a.defined && b.defined && (a.min - b.min).norm() < eps && (a.max - b.max).norm() < eps;
}

std::string tmp_file(const std::string& name)
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(root);
    Slic3r::set_temporary_dir(root.string());
    return (root / name).string();
}

void round_trip(Model& src, Model& dst, const std::string& file)
{
    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    StoreParams        sp;
    sp.path     = file.c_str();
    sp.model    = &src;
    sp.config   = &store_config;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(sp));

    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset*>      project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(file.c_str(), &dst_config, &ctxt, &dst, &plate_data, &project_presets, &is_bbl_3mf,
                         &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                             LoadStrategy::Silence));
    release_PlateData_list(plate_data);
    if (!std::getenv("SNORCA_PUSHFIT_KEEP"))
        boost::filesystem::remove(file);
}

} // namespace

// ------------------------------------------------------------------------ T5 the cut

TEST_CASE("A flat cut with a Pin gives two halves with their own holes and a pin beside them", "[PushFit][PushFitCut]")
{
    const PushFitParams p = pin_params(7.f, 5.f);   // asymmetric on purpose
    Model model;
    ModelObject* mo = make_cube(model, Geometry::Transformation(Geometry::translation_transform(Vec3d(0., 0., 15.))));
    add_pushfit_volume(mo, pin_connector(p), "Connector-1");

    Cut cut(mo, 0, Transform3d::Identity(), ATTRS_ALL());
    const ModelObjectPtrs& res = cut.perform_with_plane();
    REQUIRE(res.size() == 3);
    REQUIRE(cut.pin_slot_failures() == 0);

    const double rh = 0.5 * double(p.diameter) + double(p.clearance_upper);
    const double dc = double(p.depth_clearance);

    // Upper half: one Pin hole, from 0.2 mm outside its face to Lu + depth clearance + the cone.
    const ModelVolume* up = pin_hole(res[0]);
    REQUIRE(up != nullptr);
    REQUIRE(up->cut_info.is_processed);
    REQUIRE(up->cut_info.pushfit == p);
    BoundingBoxf3 bb = volume_bbox(up);
    REQUIRE(bb.min.z() == Approx(-PUSHFIT_HOLE_MOUTH_LEAD).margin(1e-3));
    REQUIRE(bb.max.z() == Approx(7. + dc + rh).margin(1e-3));
    // Lower half: its own depth, Ll, not the upper one's.
    const ModelVolume* lo = pin_hole(res[1]);
    REQUIRE(lo != nullptr);
    bb = volume_bbox(lo);
    REQUIRE(bb.max.z() == Approx(PUSHFIT_HOLE_MOUTH_LEAD).margin(1e-3));
    REQUIRE(bb.min.z() == Approx(-(5. + dc + rh)).margin(1e-3));
    // The halves keep their material.
    REQUIRE(parts_bbox(res[0], Transform3d::Identity()).min.z() == Approx(0.).margin(1e-3));
    REQUIRE(parts_bbox(res[1], Transform3d::Identity()).max.z() == Approx(0.).margin(1e-3));

    // The pin: an object of its own, one connector part, lying, in a row beside the cube.
    const ModelObject* pin = find_named(res, "-Pin-1");
    REQUIRE(pin != nullptr);
    REQUIRE(pin->volumes.size() == 1);
    REQUIRE(pin->volumes.front()->is_model_part());
    REQUIRE(pin->volumes.front()->cut_info.is_pushfit());
    REQUIRE(its_num_open_edges(pin->volumes.front()->mesh().its) == 0);
    const BoundingBoxf3 world = parts_bbox(pin, pin->instances.front()->get_matrix());
    REQUIRE(world.min.z() == Approx(0.).margin(1e-3));
    REQUIRE(world.size().x() == Approx(12.).margin(1e-3));                 // lying along X
    REQUIRE(world.size().z() == Approx(double(p.diameter - 2.f * p.flat_depth)).margin(1e-3));
    REQUIRE(world.min.x() == Approx(0.5 * CUBE + 5.).margin(1e-3));        // 5 mm beside the cube
}

TEST_CASE("A Pin across a kerf: each hole is measured from its own face, the pin spans the kerf", "[PushFit][PushFitCut]")
{
    const PushFitParams p = pin_params(7.f, 5.f);
    Model model;
    ModelObject* mo = make_cube(model);
    add_pushfit_volume(mo, pin_connector(p), "Connector-1");

    Cut cut(mo, 0, Transform3d::Identity(), ATTRS_ALL());
    const ModelObjectPtrs& res = cut.perform_with_plane(0.5, CutThicknessOffset::Centred);
    REQUIRE(res.size() == 3);
    const double rh = 0.5 * double(p.diameter) + double(p.clearance_upper);
    const double dc = double(p.depth_clearance);

    BoundingBoxf3 bb = volume_bbox(pin_hole(res[0]));
    REQUIRE(bb.min.z() == Approx(0.25 - PUSHFIT_HOLE_MOUTH_LEAD).margin(1e-3));
    REQUIRE(bb.max.z() == Approx(0.25 + 7. + dc + rh).margin(1e-3));
    bb = volume_bbox(pin_hole(res[1]));
    REQUIRE(bb.min.z() == Approx(-(0.25 + 5. + dc + rh)).margin(1e-3));

    const ModelObject* pin = find_named(res, "-Pin-");
    REQUIRE(pin != nullptr);
    REQUIRE(parts_bbox(pin, Transform3d::Identity()).size().x() == Approx(12.5).margin(1e-3));
}

TEST_CASE("A tilted Pin's holes follow the connector axis", "[PushFit][PushFitCut]")
{
    const PushFitParams p   = pin_params(6.f, 6.f);
    const Transform3d   rot = Geometry::rotation_transform(Vec3d(0.3, 0., 0.));
    const Vec3d         ax  = rot * Vec3d::UnitZ();
    Model model;
    ModelObject* mo = make_cube(model);
    add_pushfit_volume(mo, pin_connector(p, Vec3d(2., -1., 0.), rot), "Connector-1");

    Cut cut(mo, 0, Transform3d::Identity(), ATTRS_ALL());
    const ModelObjectPtrs& res = cut.perform_with_plane();
    REQUIRE(res.size() == 3);
    // The vertex centroid of a revolved hole lies on its axis.
    const std::vector<std::pair<const ModelObject*, double>> halves = { { res[0], 1. }, { res[1], -1. } };
    for (const auto& [obj, sign] : halves) {
        const ModelVolume* h = pin_hole(obj);
        REQUIRE(h != nullptr);
        Vec3d c = Vec3d::Zero();
        const Transform3d m = h->get_matrix();
        for (const Vec3f& v : h->mesh().its.vertices)
            c += m * v.cast<double>();
        c /= double(h->mesh().its.vertices.size());
        const Vec3d d = c - Vec3d(2., -1., 0.);
        REQUIRE(sign * d.dot(ax) > 2.);
        REQUIRE((d - d.dot(ax) * ax).norm() < 1e-3);
    }
}

TEST_CASE("Without CreateDowels a Pin makes the holes and no pin object", "[PushFit][PushFitCut]")
{
    Model model;
    ModelObject* mo = make_cube(model);
    add_pushfit_volume(mo, pin_connector(pin_params()), "Connector-1");
    Cut cut(mo, 0, Transform3d::Identity(), ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);
    REQUIRE(pin_hole(res[0]) != nullptr);
    REQUIRE(pin_hole(res[1]) != nullptr);
}

TEST_CASE("Several Pins are laid out in a row", "[PushFit][PushFitCut]")
{
    const PushFitParams p = pin_params(6.f, 6.f);
    Model model;
    ModelObject* mo = make_cube(model);
    add_pushfit_volume(mo, pin_connector(p, Vec3d(-7., 0., 0.)), "Connector-1");
    add_pushfit_volume(mo, pin_connector(p, Vec3d(7., 0., 0.)), "Connector-2");
    Cut cut(mo, 0, Transform3d::Identity(), ATTRS_ALL());
    const ModelObjectPtrs& res = cut.perform_with_plane();
    REQUIRE(res.size() == 4);
    const ModelObject* a = find_named(res, "-Pin-1");
    const ModelObject* b = find_named(res, "-Pin-2");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    const BoundingBoxf3 ba = parts_bbox(a, a->instances.front()->get_matrix());
    const BoundingBoxf3 bb = parts_bbox(b, b->instances.front()->get_matrix());
    REQUIRE(bb.min.x() - ba.max.x() == Approx(3.).margin(1e-3));
    REQUIRE(ba.min.y() == Approx(bb.min.y()).margin(1e-3));
}

// ------------------------------------------------------------------------ C1 assembled poses

TEST_CASE("C1: cut results sit at their assembled poses", "[PushFit][CutAssembly]")
{
    const Geometry::Transformation inst(Geometry::translation_transform(Vec3d(50., 20., 15.)) *
                                        Geometry::rotation_transform(Vec3d(0., 0., 0.5)));
    const Geometry::Transformation assembled(Geometry::translation_transform(Vec3d(200., -40., 10.)) *
                                             Geometry::rotation_transform(Vec3d(0., 0., -0.8)));
    const PushFitParams p = pin_params(6.f, 6.f);
    // Connectors live in the halves' frame: the object frame turned by the instance.
    const Vec3d pin_pos(5., 3., 0.);
    const Vec3d dowel_pos(-6., -4., 0.);

    for (bool initialised : { true, false }) {
        INFO("assembled pose initialised: " << initialised);
        Model model;
        ModelObject* mo = make_cube(model, inst);
        if (initialised)
            mo->instances.front()->set_assemble_transformation(assembled);
        const Transform3d A  = initialised ? assembled.get_matrix() : inst.get_matrix();
        const Transform3d M0 = inst.get_matrix_no_offset();
        const BoundingBoxf3 src = parts_bbox(mo, A);

        add_pushfit_volume(mo, pin_connector(p, pin_pos), "Connector-1");
        add_dowel_volume(mo, dowel_pos, 2.f, 10.f);

        // Place-on-cut and flip change the halves' plate poses, never their assembled ones.
        Cut cut(mo, 0, Transform3d::Identity(), ATTRS_ALL() | ModelObjectCutAttribute::PlaceOnCutLower);
        const ModelObjectPtrs& res = cut.perform_with_plane();
        REQUIRE(res.size() == 4);

        // The halves at their assembled poses rebuild the uncut cube.
        BoundingBoxf3 halves;
        for (int i = 0; i < 2; ++i) {
            REQUIRE(res[i]->instances.front()->is_assemble_initialized());
            halves.merge(parts_bbox(res[i], res[i]->instances.front()->get_assemble_transformation().get_matrix()));
        }
        REQUIRE(same_box(halves, src));
        // ... and the upper one is the top half of it.
        const BoundingBoxf3 top = parts_bbox(res[0], res[0]->instances.front()->get_assemble_transformation().get_matrix());
        REQUIRE(top.min.z() == Approx(src.center().z()).margin(1e-3));
        REQUIRE(top.max.z() == Approx(src.max.z()).margin(1e-3));

        // The pin and the dowel stand in their holes: centred on the connector, axis along the
        // cut normal.
        const ModelObject* pin   = find_named(res, "-Pin-");
        const ModelObject* dowel = find_named(res, "-Dowel-");
        REQUIRE(pin != nullptr);
        REQUIRE(dowel != nullptr);
        const Transform3d to_assembled = A * M0.inverse();
        const BoundingBoxf3 pb = parts_bbox(pin, pin->instances.front()->get_assemble_transformation().get_matrix());
        REQUIRE((pb.center() - to_assembled * pin_pos).norm() < 1e-3);
        REQUIRE(pb.size().z() == Approx(12.).margin(1e-3));
        const BoundingBoxf3 db = parts_bbox(dowel, dowel->instances.front()->get_assemble_transformation().get_matrix());
        REQUIRE((db.center() - to_assembled * dowel_pos).norm() < 1e-3);
        REQUIRE(db.size().z() == Approx(10.).margin(1e-3));
        // ... while on the plate the pin lies beside the object.
        REQUIRE(parts_bbox(pin, pin->instances.front()->get_matrix()).size().x() == Approx(12.).margin(1e-3));
    }
}

TEST_CASE("C1: a keep-as-parts cut stays assembled", "[PushFit][CutAssembly]")
{
    const Geometry::Transformation assembled(Geometry::translation_transform(Vec3d(-30., 12., 4.)));
    Model model;
    ModelObject* mo = make_cube(model);
    mo->instances.front()->set_assemble_transformation(assembled);
    const BoundingBoxf3 src = parts_bbox(mo, assembled.get_matrix());
    Cut cut(mo, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepAsParts);
    const ModelObjectPtrs& res = cut.perform_with_plane();
    REQUIRE(res.size() == 1);
    REQUIRE(same_box(parts_bbox(res[0], res[0]->instances.front()->get_assemble_transformation().get_matrix()), src));
}

// ------------------------------------------------------------------------ T6 3MF and recipe

TEST_CASE("Pin parameters survive a project 3MF round trip", "[PushFit][PushFitCut]")
{
    PushFitParams p = pin_params(7.5f, 4.5f);
    p.clearance_upper = 0.15f;
    p.clearance_lower = 0.05f;
    p.hole_groove     = true;
    p.cutout          = PushFitCutout::Flat;
    p.pose            = PushFitPose::Upright;
    p.preset          = int(PushFitPreset::Custom);
    p.lip_position    = 0.55f;
    REQUIRE(pushfit_validate(p).empty());

    Model model;
    ModelObject* mo = make_cube(model);
    add_pushfit_volume(mo, pin_connector(p), "Connector-1");
    Cut cut(mo, 0, Transform3d::Identity(), ATTRS_ALL());
    const ModelObjectPtrs& res = cut.perform_with_plane();
    REQUIRE(res.size() == 3);

    Model out;
    for (const ModelObject* o : res)
        out.add_object(*o);
    Model back;
    round_trip(out, back, tmp_file("edgeslicer_pushfit_roundtrip.3mf"));
    REQUIRE(back.objects.size() == 3);

    int pins = 0;
    for (const ModelObject* o : back.objects)
        for (const ModelVolume* v : o->volumes)
            if (v->cut_info.is_pushfit()) {
                ++pins;
                REQUIRE(v->cut_info.is_processed);
                // The whole struct, so a field added later without an attribute is caught here.
                REQUIRE(v->cut_info.pushfit == p);
            }
    REQUIRE(pins == 3);   // two holes and the pin
}

TEST_CASE("Pin attributes read from a file are clamped", "[PushFit][PushFitCut]")
{
    std::map<std::string, double> crafted = {
        { "pushfit_diameter", 1e9 },       { "pushfit_length_upper", -4. },
        { "pushfit_clearance_lower", 7. }, { "pushfit_lip_height", std::numeric_limits<double>::quiet_NaN() },
        { "pushfit_cutout", 42. },         { "pushfit_pose", -3. },
        { "pushfit_preset", 1e12 },        { "pushfit_mouth_chamfer", 99. },
    };
    const PushFitParams q = pushfit_from_attributes([&crafted](const std::string& n) -> std::optional<double> {
        auto it = crafted.find(n);
        return it == crafted.end() ? std::nullopt : std::optional<double>(it->second);
    });
    const PushFitParams def;
    REQUIRE(q.diameter == 50.f);
    REQUIRE(q.length_upper == 2.f);
    REQUIRE(q.clearance_lower == 1.f);
    REQUIRE(q.lip_height == def.lip_height);
    REQUIRE(q.cutout == def.cutout);
    REQUIRE(q.pose == def.pose);
    REQUIRE(q.preset == def.preset);
    REQUIRE(q.mouth_chamfer == Approx(0.25f * q.diameter));
    // Absent attributes keep their defaults, and a full set round-trips exactly.
    REQUIRE(pushfit_from_attributes([](const std::string&) { return std::optional<double>(); }) == def);
    const PushFitParams p = pin_params(8.f, 3.f);
    const auto attrs = pushfit_attributes(p);
    REQUIRE(pushfit_from_attributes([&attrs](const std::string& n) -> std::optional<double> {
                for (const auto& [k, v] : attrs)
                    if (k == n)
                        return v;
                return std::nullopt;
            }) == p);
}

TEST_CASE("A cut recipe with a Pin survives a 3MF round trip", "[PushFit][PushFitCut]")
{
    PushFitParams p = pin_params(6.5f, 5.5f);
    p.slot_length   = 3.5f;
    CutConnector c  = pin_connector(p, Vec3d(1., 2., 0.));
    c.z_angle       = 0.4f;

    CutRecipe r;
    r.version = CutRecipeVersion;
    r.kind    = CutRecipeKind::Plane;
    cut_recipe_connectors_from_model({ c }, r.connectors);
    r.mesh = TriangleMesh(centred_cube());
    r.mesh_hash = cut_recipe_mesh_hash(cut_recipe_mesh_to_blob(r.mesh));
    REQUIRE(CutRecipeVersion >= 6);

    Model src;
    ModelObject* mo = make_cube(src);
    mo->cut_recipe  = r;
    Model back;
    round_trip(src, back, tmp_file("edgeslicer_pushfit_recipe.3mf"));
    REQUIRE(back.objects.size() == 1);
    REQUIRE(back.objects.front()->cut_recipe.has_value());
    const CutRecipe& out = *back.objects.front()->cut_recipe;
    REQUIRE(out.version == CutRecipeVersion);
    REQUIRE(out.connectors.size() == 1);
    REQUIRE(out.connectors.front().type == int(CutConnectorType::PushFit));
    REQUIRE(out.connectors.front().pushfit == p);

    std::vector<CutConnector> model_connectors;
    cut_recipe_connectors_to_model(out.connectors, model_connectors);
    REQUIRE(model_connectors.size() == 1);
    REQUIRE(model_connectors.front().attribs.type == CutConnectorType::PushFit);
    REQUIRE(model_connectors.front().pushfit == p);
    REQUIRE(model_connectors.front().z_angle == Approx(0.4f));
}

TEST_CASE("A mirrored Pin keeps its type and turns its flats the other way", "[PushFit][PushFitCut]")
{
    CutRecipe r;
    r.version = CutRecipeVersion;
    r.kind    = CutRecipeKind::Plane;
    CutConnector c = pin_connector(pin_params(), Vec3d(4., 0., 0.));
    c.z_angle      = 0.6f;
    cut_recipe_connectors_from_model({ c }, r.connectors);
    const CutRecipe m = cut_recipe_mirrored(r, CutMirrorAxis::X, Vec3d::Zero());
    REQUIRE(m.connectors.front().type == int(CutConnectorType::PushFit));
    REQUIRE(m.connectors.front().z_angle == Approx(-0.6f));
    REQUIRE(m.connectors.front().pushfit == c.pushfit);
    REQUIRE(cut_recipe_mirrored(m, CutMirrorAxis::X, Vec3d::Zero()) == r);
}

// ------------------------------------------------------------------------ T7 re-cut from the recipe

TEST_CASE("Re-cutting from the recipe with a new diameter gives one new pin of that diameter", "[PushFit][PushFitCut]")
{
    PushFitParams p = pin_params(6.f, 6.f);
    std::vector<CutRecipeConnector> stored;
    cut_recipe_connectors_from_model({ pin_connector(p) }, stored);

    // The re-edit: the recipe's connectors come back, the user picks L, the cut runs again.
    auto recut = [&stored](float diameter, Model& model) {
        std::vector<CutConnector> cs;
        cut_recipe_connectors_to_model(stored, cs);
        cs.front().pushfit = pushfit_apply_preset(cs.front().pushfit, diameter > 7.f ? PushFitPreset::L : PushFitPreset::M);
        ModelObject* mo = make_cube(model);
        for (const CutConnector& c : cs)
            add_pushfit_volume(mo, c, "Connector-1");
        Cut cut(mo, 0, Transform3d::Identity(), ATTRS_ALL());
        ModelObjectPtrs res = cut.perform_with_plane();
        std::vector<float> pins;
        for (const ModelObject* o : res)
            if (o->name.find("-Pin-") != std::string::npos)
                pins.push_back(o->volumes.front()->cut_info.pushfit.diameter);
        return pins;
    };
    Model m1, m2;
    const std::vector<float> first  = recut(6.f, m1);
    const std::vector<float> second = recut(8.f, m2);
    REQUIRE(first == std::vector<float>{ 6.f });
    REQUIRE(second == std::vector<float>{ 8.f });
}
