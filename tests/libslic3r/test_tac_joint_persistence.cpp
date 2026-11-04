#include <catch2/catch.hpp>

#include <libslic3r/CutRecipe.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Preset.hpp>
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

// Dropping a new member from copy/equality or cereal must break these assertions:
// the actual undo payloads must retain parameters and notice a one-field edit.
TEST_CASE("TAC connector definitions and volume undo payloads retain every parameter", "[TACJoint][TACPersistence]")
{
    const CutConnector original = tac_connector();
    CutConnector       copied(original);
    REQUIRE(copied == original);
    copied.tac_joint.offset = 0.f;
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
    for (bool processed : {false, true}) {
        Model        source;
        ModelObject* o = fixture(source);
        // Before a cut even the object cut_id is unset; both export gates must admit TAC.
        if (!processed)
            o->cut_id = CutObjectBase{};
        ModelVolume* v        = o->add_volume(TriangleMesh(its_make_cube(4., 3., 11.)), ModelVolumeType::NEGATIVE_VOLUME);
        v->name               = "TAC connector";
        v->cut_info           = ModelVolume::CutInfo(CutConnectorType::TacJoint, 0.f, 0.f, processed);
        v->cut_info.tac_joint = tac_connector().tac_joint;
        Model loaded;
        round_trip(source, loaded, processed ? "processed" : "placed");
        REQUIRE(loaded.objects.size() == 1);
        REQUIRE(loaded.objects.front()->volumes.size() == 2);
        const auto& info = loaded.objects.front()->volumes.back()->cut_info;
        REQUIRE(info.is_tac_joint());
        REQUIRE(info.is_processed == processed);
        REQUIRE(info.tac_joint == tac_connector().tac_joint);
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
    REQUIRE(r.version >= 7);
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
    for (int mode = 0; mode < 7; ++mode) {
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

TEST_CASE("Untrusted TAC XML is rejected while baked project geometry remains available", "[TACJoint][TACPersistence]")
{
    const std::pair<std::string, std::string> corrupt[] = {{"tac_joint_version", "999"},
                                                           {"tac_joint_version", "1.5"},
                                                           {"tac_joint_version", "nan"},
                                                           {"tac_joint_version", "inf"},
                                                           {"tac_joint_version", "1e100"},
                                                           {"tac_joint_version", "invalid"},
                                                           {"tac_joint_scale", "nan"},
                                                           {"tac_joint_scale", "0"},
                                                           {"tac_joint_scale", "1e100"},
                                                           {"tac_joint_clearance", "-1"},
                                                           {"tac_joint_offset", "inf"},
                                                           {"tac_joint_end_clearance", "invalid"},
                                                           {"type", "999"}};
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
