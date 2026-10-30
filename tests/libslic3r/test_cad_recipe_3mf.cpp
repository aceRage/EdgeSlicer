#include <catch2/catch.hpp>

#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include "test_utils.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

using namespace Slic3r;

namespace {

void one_cube(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
}

DynamicPrintConfig project_config()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    return cfg;
}

void prepare_temp_dir()
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
}

bool store_project(const std::string &path, Model &model)
{
    DynamicPrintConfig cfg = project_config();
    StoreParams        sp;
    sp.path     = path;
    sp.model    = &model;
    sp.config   = &cfg;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    return store_bbs_3mf(sp);
}

bool load_project(const std::string &path, Model &model)
{
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::EnableSilent};
    PlateDataPtrs             plates;
    std::vector<Preset *>     presets;
    bool                      is_bbl = false;
    Semver                    version;
    const bool ok = load_bbs_3mf(path.c_str(), &config, &ctxt, &model, &plates, &presets, &is_bbl, &version, nullptr,
                                 LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                                     LoadStrategy::Silence);
    release_PlateData_list(plates);
    for (Preset *p : presets)
        delete p;
    return ok;
}

bool zip_has_entry(const std::string &zip_path, const std::string &entry)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return false;
    bool found = false;
    const mz_uint n = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < n && !found; ++i) {
        mz_zip_archive_file_stat stat;
        if (mz_zip_reader_file_stat(&archive, i, &stat))
            found = boost::algorithm::iequals(stat.m_filename, entry);
    }
    close_zip_reader(&archive);
    return found;
}

std::string zip_entry(const std::string &zip_path, const std::string &entry_name)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return {};
    size_t size = 0;
    void  *data = mz_zip_reader_extract_file_to_heap(&archive, entry_name.c_str(), &size, 0);
    std::string result;
    if (data != nullptr) {
        result.assign(static_cast<const char *>(data), size);
        mz_free(data);
    }
    close_zip_reader(&archive);
    return result;
}

bool clone_zip_with_entry(const std::string &src, const std::string &dst, const std::string &name, const std::string &data)
{
    mz_zip_archive in;
    mz_zip_archive out;
    mz_zip_zero_struct(&in);
    mz_zip_zero_struct(&out);
    if (!open_zip_reader(&in, src))
        return false;
    if (!open_zip_writer(&out, dst)) {
        close_zip_reader(&in);
        return false;
    }
    const mz_uint n = mz_zip_reader_get_num_files(&in);
    for (mz_uint i = 0; i < n; ++i) {
        if (!mz_zip_writer_add_from_zip_reader(&out, &in, i)) {
            close_zip_reader(&in);
            close_zip_writer(&out);
            return false;
        }
    }
    const bool added = mz_zip_writer_add_mem(&out, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    const bool fin   = added && mz_zip_writer_finalize_archive(&out);
    close_zip_reader(&in);
    close_zip_writer(&out);
    return fin;
}

std::string square_extrude_recipe()
{
    CadDocument doc;
    SketchProfile sp;
    sp.points = {Vec2d(-10, -10), Vec2d(10, -10), Vec2d(10, 10), Vec2d(-10, 10)};
    sp.closed = true;
    const int sk = doc.add_sketch_profile(sp, SketchPlane::XY(), "Square");
    REQUIRE(sk >= 0);
    REQUIRE(doc.add_extrude(sk, 5.0, false, BooleanMode::New, "Extrude") >= 0);
    const std::string blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());
    return blob;
}

// A Fillet with no body: cereal-valid, but recompute() throws "fillet needs a body".
std::string dangling_fillet_recipe()
{
    CadDocument doc;
    REQUIRE(doc.add_fillet(1.0, std::vector<int>{}, "DanglingFillet") >= 0);
    const std::string blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());
    REQUIRE(CadDocument::validate_recipe_bytes(blob));
    CadDocument rebuilt;
    REQUIRE_FALSE(rebuilt.deserialize_recipe(blob));
    return blob;
}

// An Import whose BRep payload is junk text: cereal-valid, BRepTools::Read would not build a shape.
std::string junk_import_recipe()
{
    CadDocument doc;
    CadFeature  f;
    f.type          = CadFeatureType::Import;
    f.name          = "JunkImport";
    f.imported_brep = "this is not a BRep";
    doc.features.push_back(f);
    const std::string blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());
    reset_brep_from_string_calls();
    REQUIRE(CadDocument::validate_recipe_bytes(blob));
    REQUIRE(brep_from_string_calls() == 0);
    return blob;
}

} // namespace

TEST_CASE("CAD recipe round-trips through a project 3MF", "[CadRecipe][3mf]")
{
    prepare_temp_dir();
    const std::string blob = square_extrude_recipe();

    Model src;
    one_cube(src);
    src.cad_recipe = blob;
    {
        Model copied = src;
        REQUIRE(copied.cad_recipe == blob);
        Model moved = std::move(copied);
        REQUIRE(moved.cad_recipe == blob);
        Model live;
        live.load_from(moved);
        REQUIRE(live.cad_recipe == blob);
    }

    ScopedTemporaryFile file(".3mf");
    REQUIRE(store_project(file.string(), src));
    REQUIRE(zip_has_entry(file.string(), "Metadata/orca_cad.bin"));
    REQUIRE_FALSE(zip_has_entry(file.string(), "Metadata/SnapOrca_cad.bin"));
    const std::string ctypes = zip_entry(file.string(), "[Content_Types].xml");
    REQUIRE_FALSE(ctypes.empty());
    REQUIRE(ctypes.find("orca_cad") == std::string::npos);
    REQUIRE(ctypes.find("Extension=\"bin\"") == std::string::npos);

    Model loaded;
    REQUIRE(load_project(file.string(), loaded));
    REQUIRE(loaded.objects.size() == 1);
    REQUIRE(loaded.cad_recipe == blob);

    CadDocument round;
    REQUIRE(round.deserialize_recipe(loaded.cad_recipe.bytes()));
    REQUIRE(round.features.size() >= 2);
}

TEST_CASE("A project with no CAD recipe stays recipe-free", "[CadRecipe][3mf]")
{
    prepare_temp_dir();
    Model src;
    one_cube(src);
    REQUIRE(src.cad_recipe.empty());

    ScopedTemporaryFile file(".3mf");
    REQUIRE(store_project(file.string(), src));
    REQUIRE_FALSE(zip_has_entry(file.string(), "Metadata/orca_cad.bin"));
    REQUIRE_FALSE(zip_has_entry(file.string(), "Metadata/SnapOrca_cad.bin"));

    Model loaded;
    REQUIRE(load_project(file.string(), loaded));
    REQUIRE(loaded.objects.size() == 1);
    REQUIRE(loaded.cad_recipe.empty());
}

TEST_CASE("A truncated or corrupt CAD recipe part fails cleanly and still loads the mesh", "[CadRecipe][3mf]")
{
    prepare_temp_dir();
    Model src;
    one_cube(src);
    ScopedTemporaryFile clean(".3mf");
    REQUIRE(store_project(clean.string(), src));

    const std::string valid = square_extrude_recipe();
    REQUIRE(valid.size() > 16);

    auto check_bad_part = [&](const std::string &entry, const std::string &payload) {
        ScopedTemporaryFile bad(".3mf");
        REQUIRE(clone_zip_with_entry(clean.string(), bad.string(), entry, payload));
        Model loaded;
        bool ok = false;
        REQUIRE_NOTHROW((ok = load_project(bad.string(), loaded)));
        REQUIRE(ok);
        REQUIRE(loaded.objects.size() == 1);
        REQUIRE_FALSE(loaded.objects.front()->volumes.empty());
        REQUIRE(loaded.cad_recipe.empty());
        REQUIRE_FALSE(CadDocument::validate_recipe_bytes(payload));
    };

    SECTION("garbage bytes under the current name") {
        check_bad_part("Metadata/orca_cad.bin", "this is not a valid cereal blob");
    }
    SECTION("truncated valid recipe") {
        check_bad_part("Metadata/orca_cad.bin", valid.substr(0, valid.size() / 2));
    }
    SECTION("legacy name, truncated") {
        check_bad_part("Metadata/SnapOrca_cad.bin", valid.substr(0, 8));
    }
}

TEST_CASE("A legacy SnapOrca_cad.bin recipe part still loads", "[CadRecipe][3mf]")
{
    prepare_temp_dir();
    Model src;
    one_cube(src);
    ScopedTemporaryFile clean(".3mf");
    REQUIRE(store_project(clean.string(), src));

    const std::string blob = square_extrude_recipe();
    ScopedTemporaryFile legacy(".3mf");
    REQUIRE(clone_zip_with_entry(clean.string(), legacy.string(), "Metadata/SnapOrca_cad.bin", blob));

    Model loaded;
    REQUIRE(load_project(legacy.string(), loaded));
    REQUIRE(loaded.objects.size() == 1);
    REQUIRE(loaded.cad_recipe == blob);
    CadDocument doc;
    REQUIRE(doc.deserialize_recipe(loaded.cad_recipe.bytes()));
}

TEST_CASE("orca_cad.bin wins over a legacy SnapOrca_cad.bin in the same archive", "[CadRecipe][3mf]")
{
    prepare_temp_dir();
    Model src;
    one_cube(src);
    ScopedTemporaryFile clean(".3mf");
    REQUIRE(store_project(clean.string(), src));

    const std::string current = square_extrude_recipe();
    const std::string legacy  = dangling_fillet_recipe();
    REQUIRE(current != legacy);

    auto load_both = [&](const std::string &first_name, const std::string &first_blob,
                         const std::string &second_name, const std::string &second_blob) {
        ScopedTemporaryFile mid(".3mf");
        ScopedTemporaryFile both(".3mf");
        REQUIRE(clone_zip_with_entry(clean.string(), mid.string(), first_name, first_blob));
        REQUIRE(clone_zip_with_entry(mid.string(), both.string(), second_name, second_blob));
        REQUIRE(zip_has_entry(both.string(), "Metadata/orca_cad.bin"));
        REQUIRE(zip_has_entry(both.string(), "Metadata/SnapOrca_cad.bin"));
        Model loaded;
        REQUIRE(load_project(both.string(), loaded));
        REQUIRE(loaded.cad_recipe == current);
    };

    SECTION("legacy listed first") {
        load_both("Metadata/SnapOrca_cad.bin", legacy, "Metadata/orca_cad.bin", current);
    }
    SECTION("current listed first") {
        load_both("Metadata/orca_cad.bin", current, "Metadata/SnapOrca_cad.bin", legacy);
    }
}

TEST_CASE("A structurally valid recipe that would not rebuild is kept byte-for-byte", "[CadRecipe][3mf]")
{
    prepare_temp_dir();
    const std::string blob = dangling_fillet_recipe();

    Model src;
    one_cube(src);
    src.cad_recipe = blob;
    ScopedTemporaryFile file(".3mf");
    REQUIRE(store_project(file.string(), src));
    REQUIRE(zip_entry(file.string(), "Metadata/orca_cad.bin") == blob);

    Model loaded;
    REQUIRE(load_project(file.string(), loaded));
    REQUIRE(loaded.objects.size() == 1);
    REQUIRE(loaded.cad_recipe == blob);

    ScopedTemporaryFile again(".3mf");
    REQUIRE(store_project(again.string(), loaded));
    REQUIRE(zip_entry(again.string(), "Metadata/orca_cad.bin") == blob);
}

TEST_CASE("An Import with junk BRep text is kept without calling BRepTools", "[CadRecipe][3mf]")
{
    prepare_temp_dir();
    const std::string blob = junk_import_recipe();

    Model src;
    one_cube(src);
    src.cad_recipe = blob;
    ScopedTemporaryFile file(".3mf");
    REQUIRE(store_project(file.string(), src));
    REQUIRE(zip_entry(file.string(), "Metadata/orca_cad.bin") == blob);

    reset_brep_from_string_calls();
    Model loaded;
    REQUIRE(load_project(file.string(), loaded));
    REQUIRE(loaded.objects.size() == 1);
    REQUIRE(loaded.cad_recipe == blob);
    REQUIRE(brep_from_string_calls() == 0);

    ScopedTemporaryFile again(".3mf");
    REQUIRE(store_project(again.string(), loaded));
    REQUIRE(zip_entry(again.string(), "Metadata/orca_cad.bin") == blob);
}

TEST_CASE("A sliced .gcode.3mf does not carry the CAD recipe", "[CadRecipe][3mf]")
{
    // F5: project 3MFs keep Metadata/orca_cad.bin; sliced handoff files do not.
    prepare_temp_dir();
    CadDocument doc;
    const int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 10, 0, "Sketch");
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
    const std::string blob = doc.serialize_recipe();

    Model src;
    one_cube(src);
    src.cad_recipe = blob;
    ScopedTemporaryFile file(".gcode.3mf");
    DynamicPrintConfig cfg = project_config();
    StoreParams        sp;
    sp.path     = file.string();
    sp.model    = &src;
    sp.config   = &cfg;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary | SaveStrategy::SkipStatic |
                  SaveStrategy::WithGcode;
    REQUIRE(store_bbs_3mf(sp));
    REQUIRE_FALSE(zip_has_entry(file.string(), "Metadata/orca_cad.bin"));
}
