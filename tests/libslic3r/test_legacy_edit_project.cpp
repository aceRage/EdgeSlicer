#include <catch2/catch.hpp>

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

// The Edit gizmo (and Round all edges) wrote the result back as an ordinary
// ModelVolume mesh. Old projects therefore contain no MeshEdit session and no
// extra zip keys — only a triangle mesh. This helper builds that shape.

void one_edited_cube(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "edit-gizmo-cube";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
    volume->name        = "pushed-face";
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

bool zip_has_entry_substr(const std::string &zip_path, const std::string &needle)
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
            found = boost::algorithm::icontains(stat.m_filename, needle);
    }
    close_zip_reader(&archive);
    return found;
}

} // namespace

TEST_CASE("A project saved after an Edit-gizmo mesh edit still opens as a mesh", "[LegacyMeshProject][3mf]")
{
    prepare_temp_dir();
    Model src;
    one_edited_cube(src);
    REQUIRE(src.objects.size() == 1);
    REQUIRE(src.objects.front()->volumes.size() == 1);
    const double src_volume = src.objects.front()->volumes.front()->mesh().volume();
    REQUIRE(src_volume > 0.);

    ScopedTemporaryFile file(".3mf");
    REQUIRE(store_project(file.string(), src));
    // The removed tools never wrote a sidecar. A load must not depend on one.
    REQUIRE_FALSE(zip_has_entry_substr(file.string(), "meshedit"));
    REQUIRE_FALSE(zip_has_entry_substr(file.string(), "mesh_edit"));
    REQUIRE_FALSE(zip_has_entry_substr(file.string(), "bevel"));

    Model loaded;
    REQUIRE(load_project(file.string(), loaded));
    REQUIRE(loaded.objects.size() == 1);
    REQUIRE(loaded.objects.front()->volumes.size() == 1);
    const ModelVolume *vol = loaded.objects.front()->volumes.front();
    REQUIRE(vol->is_model_part());
    REQUIRE_FALSE(vol->mesh().empty());
    REQUIRE(vol->mesh().its.indices.size() >= 12);
    REQUIRE(vol->mesh().volume() > 0.);
    REQUIRE_THAT(vol->mesh().volume(), Catch::Matchers::WithinAbs(src_volume, 1.0));
}
