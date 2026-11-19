#include <catch2/catch.hpp>

#include <libslic3r/CutRecipe.hpp>
#include <libslic3r/CutUtils.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/TriangleSelector.hpp>
#include <libslic3r/Utils.hpp>
#include <libslic3r/miniz_extension.hpp>
#include <cereal/archives/binary.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <limits>
#include <sstream>

using namespace Slic3r;

namespace {
CutConnector tac_connector()
{
    CutConnector c;
    c.attribs.type            = CutConnectorType::TacJoint;
    c.pos                     = Vec3d(2., 3., 0.);
    c.z_angle                 = 1.25f;
    c.tac_joint.scale         = 1.5f;
    c.tac_joint.clearance     = 0.25f;
    c.tac_joint.end_clearance = 0.45f;
    c.tac_joint.offset        = -0.75f;
    c.tac_joint.attachment    = TacJointAttachment::Lower;
    return c;
}

ModelObject* fixture(Model& model)
{
    ModelObject* o                                                  = model.add_object();
    o->name                                                         = "TAC persistence fixture";
    o->add_volume(TriangleMesh(its_make_cube(30., 30., 30.)))->name = "body";
    o->add_instance();
    o->cut_id.init();
    return o;
}

void rewrite_metadata(const std::string&                file,
                      const std::string&                entry,
                      const std::string&                attribute,
                      const std::optional<std::string>& value)
{
    mz_zip_archive reader{}, writer{};
    REQUIRE(open_zip_reader(&reader, file));
    const std::string patched = file + ".patched";
    REQUIRE(open_zip_writer(&writer, patched));
    bool found = false;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&reader); ++i) {
        mz_zip_archive_file_stat stat;
        REQUIRE(mz_zip_reader_file_stat(&reader, i, &stat));
        if (entry != stat.m_filename) {
            REQUIRE(mz_zip_writer_add_from_zip_reader(&writer, &reader, i));
            continue;
        }
        std::string xml;
        REQUIRE(read_zip_entry_capped(reader, i, 1024 * 1024, xml));
        const std::string key   = " " + attribute + "=\"";
        const size_t      start = xml.find(key);
        REQUIRE(start != std::string::npos);
        const size_t end = xml.find('"', start + key.size());
        REQUIRE(end != std::string::npos);
        xml.replace(start, end - start + 1, value ? key + *value + '"' : "");
        REQUIRE(mz_zip_writer_add_mem(&writer, stat.m_filename, xml.data(), xml.size(), MZ_DEFAULT_COMPRESSION));
        found = true;
    }
    REQUIRE(found);
    // close_zip_writer releases the writer/file only; emit the central directory
    // before replacing the original so the next rewrite/import sees a complete ZIP.
    REQUIRE(mz_zip_writer_finalize_archive(&writer));
    REQUIRE(close_zip_writer(&writer));
    REQUIRE(close_zip_reader(&reader));
    boost::filesystem::remove(file);
    boost::filesystem::rename(patched, file);
}

void round_trip(Model& source, Model& result, const std::string& name, const std::function<void(const std::string&)>& patch = {})
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(root);
    Slic3r::set_temporary_dir(root.string());
    const std::string  file   = (root / ("edgeslicer_tac_" + name + ".3mf")).string();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    StoreParams        sp;
    sp.path     = file.c_str();
    sp.model    = &source;
    sp.config   = &config;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(sp));
    if (patch)
        patch(file);
    DynamicPrintConfig        loaded_config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::EnableSilent};
    PlateDataPtrs             plates;
    std::vector<Preset*>      presets;
    bool                      bbl = false;
    Semver                    version;
    REQUIRE(load_bbs_3mf(file.c_str(), &loaded_config, &ctxt, &result, &plates, &presets, &bbl, &version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
    release_PlateData_list(plates);
    boost::filesystem::remove(file);
}

CutRecipe recipe(const CutConnector& c)
{
    CutRecipe r;
    r.mesh = TriangleMesh(its_make_cube(30., 30., 30.));
    cut_recipe_connectors_from_model({c}, r.connectors);
    return r;
}
} // namespace

TEST_CASE("An editable TAC cut retains its mesh application choice in 3MF and undo", "[TACJoint][TACApplyMesh][TACPersistence]")
{
    const bool apply = GENERATE(false, true);
    Model      source;
    auto*      object         = fixture(source);
    auto       original       = recipe(tac_connector());
    original.apply_tac_joints = apply;
    object->cut_recipe        = original;
    Model loaded;
    round_trip(source, loaded, "apply_mesh");
    REQUIRE(loaded.objects.front()->cut_recipe.has_value());
    REQUIRE(loaded.objects.front()->cut_recipe->apply_tac_joints == apply);
    std::stringstream stream;
    {
        cereal::BinaryOutputArchive ar(stream);
        ar(original);
    }
    CutRecipe restored;
    {
        cereal::BinaryInputArchive ar(stream);
        ar(restored);
    }
    REQUIRE(restored.apply_tac_joints == apply);
    REQUIRE(restored == original);
    restored.apply_tac_joints = !apply;
    REQUIRE(restored != original);
}

TEST_CASE("A legacy TAC recipe leaves automatic mesh application disabled", "[TACJoint][TACApplyMesh][TACPersistence]")
{
    Model source;
    auto* object                = fixture(source);
    object->cut_recipe          = recipe(tac_connector());
    object->cut_recipe->version = 8;
    Model loaded;
    round_trip(source, loaded, "legacy_apply_mesh",
               [&](const std::string& path) { rewrite_metadata(path, "Metadata/cut_recipe.xml", "apply_tac_joints", std::nullopt); });
    REQUIRE(loaded.objects.front()->cut_recipe.has_value());
    REQUIRE(loaded.objects.front()->cut_recipe->valid());
    REQUIRE_FALSE(loaded.objects.front()->cut_recipe->apply_tac_joints);
}

TEST_CASE("A saved TAC recipe rebuilds both full halves with real sockets", "[TACJoint][TACApplyMesh][TACPersistence][TACReedit]")
{
    const size_t selected_half = GENERATE(size_t(0), size_t(1));
    auto         cube          = its_make_cube(30., 30., 30.);
    for (auto& p : cube.vertices)
        p -= Vec3f::Constant(15.f);
    Model source;
    auto* object = source.add_object();
    object->name = "TAC recipe source";
    object->add_volume(TriangleMesh(cube));
    object->add_instance();
    object->cut_id.init();
    auto connector      = tac_connector();
    connector.tac_joint = TacJointParams{};
    connector.pos       = Vec3d::Zero();
    connector.z_angle   = 0.f;
    connector.radius    = 2.7f;
    connector.height    = 11.f;
    CutRecipe original;
    original.mesh = TriangleMesh(cube);
    cut_recipe_connectors_from_model({connector}, original.connectors);
    add_tac_joint_volume(object, connector, "TAC Joint");
    const auto attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::CreateDowels;
    Cut        first(object, 0, Transform3d::Identity(), attributes);
    const auto& first_result = first.perform_with_plane();
    REQUIRE(first_result.size() == 3);
    Model saved;
    for (size_t i = 0; i < first_result.size(); ++i) {
        auto* half = saved.add_object(*first_result[i]);
        if (i < 2) {
            half->cut_recipe = original;
            cut_recipe_record_half_frame(*half);
        }
    }
    Model loaded;
    round_trip(saved, loaded, "reedit_apply_both");
    REQUIRE(loaded.objects.size() == 3);
    REQUIRE(loaded.objects[selected_half]->has_cut_recipe());
    CutRecipe edited        = *loaded.objects[selected_half]->cut_recipe;
    edited.apply_tac_joints = true;
    // Edit cut uses the saved original, not the already shortened selected half.
    REQUIRE(edited.mesh.bounding_box().size().z() == Approx(30.));
    Model proxy_model;
    auto* proxy = proxy_model.add_object();
    proxy->add_volume(TriangleMesh(edited.mesh), ModelVolumeType::MODEL_PART, false);
    proxy->add_instance();
    cut_recipe_connectors_to_model(edited.connectors, proxy->cut_connectors);
    REQUIRE(proxy->cut_connectors.size() == 1);
    add_tac_joint_volume(proxy, proxy->cut_connectors.front(), "TAC Joint");
    Cut         recut(proxy, 0, Transform3d::Identity(), attributes | ModelObjectCutAttribute::ApplyTacJoints);
    const auto& result = recut.perform_with_plane();
    REQUIRE(result.size() == 3);
    for (size_t half = 0; half < 2; ++half) {
        REQUIRE(result[half]->volumes.size() == 1);
        REQUIRE(result[half]->volumes.front()->is_model_part());
        auto body = result[half]->raw_mesh();
        body.transform(result[half]->instances.front()->get_transformation().get_matrix(), true);
        REQUIRE(body.bounding_box().size().z() == Approx(15.));
        REQUIRE(its_num_open_edges(body.its) == 0);
        REQUIRE(its_mass_properties(body.its).volume < 13499.);
        REQUIRE(its_mass_properties(body.its).volume > 13200.);
        // An independent probe inside each expected socket must hit no material.
        TriangleMesh probe(its_make_cube(.2, .2, .2));
        probe.translate(-.1f, -.1f, half == 0 ? 2.f : -2.f);
        std::vector<TriangleMesh> intersections;
        REQUIRE(MeshBoolean::mfd::make_boolean(body, probe, intersections, "INTERSECTION"));
        double occupied = 0.;
        for (const auto& piece : intersections)
            occupied += std::abs(its_mass_properties(piece.its).volume);
        REQUIRE(occupied < 1e-9);
        auto assembled = result[half]->raw_mesh();
        assembled.transform(result[half]->instances.front()->get_assemble_transformation().get_matrix(), true);
        // Removing the last socket volume must keep the body in its source frame.
        REQUIRE((assembled.bounding_box().min - body.bounding_box().min).norm() < 1e-5);
        REQUIRE((assembled.bounding_box().max - body.bounding_box().max).norm() < 1e-5);
    }
    const auto pin = result.back()->raw_mesh();
    REQUIRE(pin.bounding_box().size().x() == Approx(4.09).margin(1e-4));
    REQUIRE(pin.bounding_box().size().y() == Approx(11.).margin(1e-4));
    REQUIRE(pin.bounding_box().size().z() == Approx(3.34).margin(1e-4));
    REQUIRE(its_mass_properties(pin.its).volume == Approx(128.85036).epsilon(1e-5));
}

TEST_CASE("Applied TAC halves retain their source frames and painting after reload",
          "[TACJoint][TACApplyMesh][TACPersistence][TACReedit][CutPaint]")
{
    const auto attachment = GENERATE(TacJointAttachment::Separate, TacJointAttachment::Upper, TacJointAttachment::Lower);
    auto       cube       = its_make_cube(30., 30., 30.);
    for (auto& p : cube.vertices)
        p -= Vec3f::Constant(15.f);
    Model source;
    auto* object = source.add_object();
    auto* body   = object->add_volume(TriangleMesh(cube));
    body->set_offset(Vec3d(3., -2., 2.));
    object->add_instance();
    object->cut_id.init();
    TriangleSelector painting(body->mesh());
    for (size_t i = 0; i < body->mesh().its.indices.size(); ++i)
        if (its_face_normal(body->mesh().its, int(i)).x() > .9f)
            painting.set_facet(int(i), EnforcerBlockerType::Extruder2);
    body->mmu_segmentation_facets.set_data(painting.serialize());
    auto connector                 = tac_connector();
    connector.tac_joint            = TacJointParams{};
    connector.tac_joint.attachment = attachment;
    CutRecipe original;
    original.mesh                = object->raw_mesh();
    original.apply_tac_joints    = true;
    original.instance_linear_set = true;
    cut_recipe_connectors_from_model({connector}, original.connectors);
    add_tac_joint_volume(object, connector, "TAC Joint");
    Cut         cut(object, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::CreateDowels |
                ModelObjectCutAttribute::ApplyTacJoints | ModelObjectCutAttribute::KeepPaint);
    const auto& result = cut.perform_with_plane();
    Model       saved;
    for (size_t half = 0; half < 2; ++half) {
        auto* copy       = saved.add_object(*result[half]);
        copy->cut_recipe = original;
        cut_recipe_record_half_frame(*copy);
    }
    Model loaded;
    round_trip(saved, loaded, "applied_source_frames");
    REQUIRE(loaded.objects.size() == 2);
    for (const auto* half : loaded.objects) {
        REQUIRE(half->volumes.size() == 1);
        REQUIRE(half->has_cut_recipe());
        const auto& r = *half->cut_recipe;
        REQUIRE(r.half_part_matrices.size() == 1);
        auto in_source = half->volumes.front()->mesh();
        in_source.transform(r.half_part_matrices.front(), true);
        CHECK(in_source.bounding_box().min.x() == Approx(-12.));
        CHECK(in_source.bounding_box().max.x() == Approx(18.));
        // Use the same placement contract as Edit cut: recover the original
        // object's pose from the selected half and its recorded source frame.
        const Transform3d proxy_pose = half->instances.front()->get_matrix() * half->volumes.front()->get_matrix() *
                                       r.half_part_matrices.front().inverse();
        auto reopened = r.mesh;
        reopened.transform(proxy_pose, true);
        CHECK((reopened.bounding_box().min - original.mesh.bounding_box().min).norm() < 1e-4);
        CHECK((reopened.bounding_box().max - original.mesh.bounding_box().max).norm() < 1e-4);
    }
    Model      proxy;
    auto*      uncut = proxy.add_object()->add_volume(TriangleMesh(original.mesh), ModelVolumeType::MODEL_PART, false);
    const auto carry = carry_cut_painting_to_uncut({loaded.objects[0], loaded.objects[1]}, Transform3d::Identity(), *uncut);
    REQUIRE(carry.had_paint);
    REQUIRE(carry.carried);
    const auto painted = uncut->mmu_segmentation_facets.get_facets(*uncut, EnforcerBlockerType::Extruder2);
    double     area    = 0.;
    for (const auto& f : painted.indices)
        area += .5 * (painted.vertices[f[1]] - painted.vertices[f[0]]).cross(painted.vertices[f[2]] - painted.vertices[f[0]]).norm();
    REQUIRE(area == Approx(900.).epsilon(.01));
    for (const auto& p : painted.vertices)
        REQUIRE(p.x() == Approx(18.).margin(1e-3));
}

// Dropping a new member from copy/equality or cereal must break these assertions:
// the actual undo payloads must retain parameters and notice a one-field edit.
TEST_CASE("TAC connector definitions and volume undo payloads retain every parameter", "[TACJoint][TACPersistence]")
{
    const CutConnector original = tac_connector();
    CutConnector       copied(original);
    REQUIRE(copied == original);
    copied.tac_joint.offset = 0.f;
    REQUIRE(copied != original);
    copied                      = original;
    copied.tac_joint.attachment = TacJointAttachment::Upper;
    REQUIRE(copied != original);
    ModelVolume::CutInfo info(CutConnectorType::TacJoint, 0.f, 0.f, true);
    info.tac_joint                  = original.tac_joint;
    const CutRecipe    r            = recipe(original);
    CutRecipeConnector changed      = r.connectors.front();
    changed.tac_joint.end_clearance = 0.3f;
    REQUIRE(changed != r.connectors.front());

    std::stringstream stream;
    {
        cereal::BinaryOutputArchive ar(stream);
        ar(original, info, r.connectors.front());
    }
    CutConnector         connector_back;
    ModelVolume::CutInfo info_back;
    CutRecipeConnector   recipe_back;
    {
        cereal::BinaryInputArchive ar(stream);
        ar(connector_back, info_back, recipe_back);
    }
    REQUIRE(connector_back == original);
    REQUIRE(info_back.is_tac_joint());
    REQUIRE(info_back.tac_joint == original.tac_joint);
    REQUIRE(recipe_back == r.connectors.front());
    std::vector<CutConnector> restored;
    cut_recipe_connectors_to_model({recipe_back}, restored);
    REQUIRE(restored.size() == 1);
    REQUIRE(restored.front() == original);
}

TEST_CASE("TAC placed and processed connector volume metadata round-trips through 3MF", "[TACJoint][TACPersistence]")
{
    const auto attachment = GENERATE(TacJointAttachment::Separate, TacJointAttachment::Upper, TacJointAttachment::Lower);
    const auto type       = GENERATE(ModelVolumeType::NEGATIVE_VOLUME, ModelVolumeType::MODEL_PART);
    for (bool processed : {false, true}) {
        if (!processed && type == ModelVolumeType::MODEL_PART)
            continue;
        Model        source;
        ModelObject* o = fixture(source);
        // Before a cut even the object cut_id is unset; both export gates must admit TAC.
        if (!processed)
            o->cut_id = CutObjectBase{};
        ModelVolume* v                   = o->add_volume(TriangleMesh(its_make_cube(4., 3., 11.)), type);
        v->name               = "TAC connector";
        v->cut_info           = ModelVolume::CutInfo(CutConnectorType::TacJoint, 0.f, 0.f, processed);
        v->cut_info.tac_joint = tac_connector().tac_joint;
        v->cut_info.tac_joint.attachment = attachment;
        const auto expected              = v->cut_info.tac_joint;
        Model loaded;
        round_trip(source, loaded, processed ? "processed" : "placed");
        REQUIRE(loaded.objects.size() == 1);
        REQUIRE(loaded.objects.front()->volumes.size() == 2);
        const auto& info = loaded.objects.front()->volumes.back()->cut_info;
        REQUIRE(info.is_tac_joint());
        REQUIRE(info.is_processed == processed);
        REQUIRE(info.tac_joint == expected);
        REQUIRE(loaded.objects.front()->volumes.back()->type() == type);
    }
}

TEST_CASE("A TAC recipe retains its type, pose and parameters in a project", "[TACJoint][TACPersistence]")
{
    const CutConnector connector = tac_connector();
    Model              source;
    fixture(source)->cut_recipe = recipe(connector);
    Model loaded;
    round_trip(source, loaded, "recipe");
    REQUIRE(loaded.objects.size() == 1);
    REQUIRE(loaded.objects.front()->cut_recipe.has_value());
    const CutRecipe& r = *loaded.objects.front()->cut_recipe;
    REQUIRE(r.version == 9);
    std::vector<CutConnector> connectors;
    cut_recipe_connectors_to_model(r.connectors, connectors);
    REQUIRE(connectors.size() == 1);
    REQUIRE(connectors.front() == connector);
    REQUIRE(is_separate_pin_connector_type(connectors.front().attribs.type));
}

TEST_CASE("Mirroring refuses an asymmetric TAC recipe without changing it", "[TACJoint][TACPersistence][CutMirror]")
{
    CutRecipe       r      = recipe(tac_connector());
    const CutRecipe before = r;
    for (const auto axis : {CutMirrorAxis::X, CutMirrorAxis::Y, CutMirrorAxis::Z}) {
        REQUIRE_THROWS_AS(cut_recipe_mirrored(r, axis, Vec3d(1., 2., 3.)), std::invalid_argument);
        REQUIRE(r == before);
    }
}

TEST_CASE("Invalid or future TAC definitions never become editable recipes", "[TACJoint][TACPersistence]")
{
    for (int mode = 0; mode < 8; ++mode) {
        CutConnector c = tac_connector();
        if (mode == 0)
            c.tac_joint.version = 999;
        if (mode == 1)
            c.tac_joint.scale = std::numeric_limits<float>::quiet_NaN();
        if (mode == 2)
            c.tac_joint.scale = 0.f;
        if (mode == 3)
            c.tac_joint.clearance = -1.f;
        if (mode == 4)
            c.z_angle = std::numeric_limits<float>::infinity();
        if (mode == 5)
            c.pos.x() = std::numeric_limits<double>::quiet_NaN();
        if (mode == 6)
            c.rotation_m(0, 0) = std::numeric_limits<double>::infinity();
        if (mode == 7)
            c.tac_joint.attachment = TacJointAttachment(999);
        CutRecipe r = recipe(c);
        REQUIRE_FALSE(r.valid());
        std::vector<CutConnector> connectors;
        REQUIRE_THROWS(cut_recipe_connectors_to_model(r.connectors, connectors));
    }
}

TEST_CASE("A legacy recipe loads with its original connector behavior", "[TACJoint][TACPersistence]")
{
    CutConnector c;
    c.attribs.type = CutConnectorType::Dowel;
    CutRecipe r    = recipe(c);
    r.version      = 6;
    Model source;
    fixture(source)->cut_recipe = r;
    Model loaded;
    round_trip(source, loaded, "legacy");
    REQUIRE(loaded.objects.front()->cut_recipe.has_value());
    std::vector<CutConnector> connectors;
    cut_recipe_connectors_to_model(loaded.objects.front()->cut_recipe->connectors, connectors);
    REQUIRE(connectors.size() == 1);
    REQUIRE(connectors.front().attribs.type == CutConnectorType::Dowel);
    REQUIRE(connectors.front().tac_joint == TacJointParams{});
}

TEST_CASE("A version 7 TAC project without attachment retains separate connectors", "[TACJoint][TACPersistence]")
{
    CutConnector c = tac_connector();
    Model        source;
    auto*        object         = fixture(source);
    object->cut_recipe          = recipe(c);
    object->cut_recipe->version = 7;
    auto* volume                = object->add_volume(TriangleMesh(its_make_cube(4., 3., 11.)), ModelVolumeType::NEGATIVE_VOLUME);
    volume->cut_info            = ModelVolume::CutInfo(CutConnectorType::TacJoint, 0.f, 0.f, true);
    volume->cut_info.tac_joint  = c.tac_joint;
    Model loaded;
    round_trip(source, loaded, "legacy_tac", [&](const std::string& file) {
        rewrite_metadata(file, "Metadata/cut_recipe.xml", "tac_joint_attachment", std::nullopt);
        rewrite_metadata(file, "Metadata/cut_information.xml", "tac_joint_attachment", std::nullopt);
    });
    c.tac_joint.attachment = TacJointAttachment::Separate;
    REQUIRE(loaded.objects.front()->cut_recipe.has_value());
    REQUIRE(loaded.objects.front()->cut_recipe->connectors.front().tac_joint == c.tac_joint);
    REQUIRE(loaded.objects.front()->volumes.back()->cut_info.tac_joint == c.tac_joint);
}

TEST_CASE("Untrusted TAC XML is rejected while baked project geometry remains available", "[TACJoint][TACPersistence]")
{
    const std::pair<std::string, std::string> corrupt[] = {{"tac_joint_version", "999"},        {"tac_joint_version", "1.5"},
                                                           {"tac_joint_version", "nan"},        {"tac_joint_version", "inf"},
                                                           {"tac_joint_version", "1e100"},      {"tac_joint_version", "invalid"},
                                                           {"tac_joint_scale", "nan"},          {"tac_joint_scale", "0"},
                                                           {"tac_joint_scale", "1e100"},        {"tac_joint_clearance", "-1"},
                                                           {"tac_joint_offset", "inf"},         {"tac_joint_end_clearance", "invalid"},
                                                           {"tac_joint_attachment", "3"},       {"tac_joint_attachment", "-1"},
                                                           {"tac_joint_attachment", "1.5"},     {"tac_joint_attachment", "nan"},
                                                           {"tac_joint_attachment", "invalid"}, {"type", "999"}};
    for (const auto& [attribute, value] : corrupt) {
        INFO(attribute << "=" << value);
        Model        source;
        ModelObject* o        = fixture(source);
        o->cut_recipe         = recipe(tac_connector());
        ModelVolume* v        = o->add_volume(TriangleMesh(its_make_cube(4., 3., 11.)), ModelVolumeType::NEGATIVE_VOLUME);
        v->cut_info           = ModelVolume::CutInfo(CutConnectorType::TacJoint, 0.f, 0.f, true);
        v->cut_info.tac_joint = tac_connector().tac_joint;
        Model loaded;
        round_trip(source, loaded, "invalid_xml", [&](const std::string& file) {
            rewrite_metadata(file, "Metadata/cut_recipe.xml", attribute, value);
            rewrite_metadata(file, "Metadata/cut_information.xml", attribute, value);
        });
        REQUIRE(loaded.objects.size() == 1);
        REQUIRE(loaded.objects.front()->volumes.size() == 2);
        REQUIRE_FALSE(loaded.objects.front()->cut_recipe.has_value());
        REQUIRE_FALSE(loaded.objects.front()->volumes.back()->cut_info.is_connector);
    }
}

TEST_CASE("A TAC file missing optional parameters uses documented defaults", "[TACJoint][TACPersistence]")
{
    Model        source;
    CutConnector c;
    c.attribs.type              = CutConnectorType::TacJoint;
    fixture(source)->cut_recipe = recipe(c);
    Model loaded;
    round_trip(source, loaded, "missing", [&](const std::string& file) {
        rewrite_metadata(file, "Metadata/cut_recipe.xml", "tac_joint_scale", std::nullopt);
        rewrite_metadata(file, "Metadata/cut_recipe.xml", "tac_joint_clearance", std::nullopt);
        rewrite_metadata(file, "Metadata/cut_recipe.xml", "tac_joint_end_clearance", std::nullopt);
        rewrite_metadata(file, "Metadata/cut_recipe.xml", "tac_joint_offset", std::nullopt);
        rewrite_metadata(file, "Metadata/cut_recipe.xml", "tac_joint_attachment", std::nullopt);
    });
    REQUIRE(loaded.objects.front()->cut_recipe.has_value());
    REQUIRE(loaded.objects.front()->cut_recipe->connectors.front().tac_joint == TacJointParams{});
}

TEST_CASE("Malformed TAC recipe poses are refused instead of defaulting to a different cut", "[TACJoint][TACPersistence]")
{
    const std::pair<std::string, std::string> corrupt[] = {{"z_angle", "nan"},
                                                           {"x", "inf"},
                                                           {"rotation", "1 0 0 0"},
                                                           {"rotation", "nan 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"}};
    for (const auto& [attribute, value] : corrupt) {
        INFO(attribute << "=" << value);
        Model source;
        fixture(source)->cut_recipe = recipe(tac_connector());
        Model loaded;
        round_trip(source, loaded, "invalid_pose",
                   [&](const std::string& file) { rewrite_metadata(file, "Metadata/cut_recipe.xml", attribute, value); });
        REQUIRE(loaded.objects.size() == 1);
        REQUIRE_FALSE(loaded.objects.front()->cut_recipe.has_value());
    }
}
