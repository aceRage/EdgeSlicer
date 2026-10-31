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

// tests/data/legacy_round_cube_p4.3mf is a REAL pre-P5 project. It was written by
// the P4 tree (a415f9bf57 + its review fixes, which still had the Edit gizmo,
// MeshEdit and MeshRound): a 20 mm cube put through round_with_options() with
// round_by_voxels at r = 2 mm (all other RoundOptions at their defaults), stored
// with store_bbs_3mf() the same way the GUI saves a project. The mesh tools wrote
// their result back as an ordinary ModelVolume mesh, so the file holds no
// mesh-edit session and no extra zip keys - only a triangle mesh. P5 removed the
// Edit gizmo; this checks such a project still opens with the same geometry.
const char *FIXTURE = "legacy_round_cube_p4.3mf";

// Measured on the fixture as loaded: 37460 triangles in 3D/3dmodel.model, and
// TriangleMesh::volume() = 7829.14 mm^3 (the unrounded cube is 8000).
constexpr size_t FIXTURE_TRIANGLES = 37460;
constexpr double FIXTURE_VOLUME    = 7829.14;

void prepare_temp_dir()
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
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

TEST_CASE("A project saved by the pre-P5 mesh tools still opens as a mesh", "[LegacyMeshProject][3mf]")
{
    prepare_temp_dir();
    const std::string path = (boost::filesystem::path(TEST_DATA_DIR) / FIXTURE).string();
    REQUIRE(boost::filesystem::exists(path));
    // The removed tools never wrote a sidecar. A load must not depend on one.
    REQUIRE_FALSE(zip_has_entry_substr(path, "meshedit"));
    REQUIRE_FALSE(zip_has_entry_substr(path, "mesh_edit"));
    REQUIRE_FALSE(zip_has_entry_substr(path, "bevel"));

    Model loaded;
    REQUIRE(load_project(path, loaded));
    REQUIRE(loaded.objects.size() == 1);
    REQUIRE(loaded.objects.front()->volumes.size() == 1);
    const ModelVolume *vol = loaded.objects.front()->volumes.front();
    REQUIRE(vol->is_model_part());
    REQUIRE(vol->mesh().its.indices.size() == FIXTURE_TRIANGLES);
    const double v = TriangleMesh(vol->mesh()).volume();
    REQUIRE_THAT(v, Catch::Matchers::WithinAbs(FIXTURE_VOLUME, 0.5));
    // Still the rounded shape, not a plain cube.
    REQUIRE(v < 8000. - 100.);
}
