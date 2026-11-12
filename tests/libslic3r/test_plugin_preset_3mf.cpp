#include <catch2/catch.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <string>
#include <vector>

using namespace Slic3r;

TEST_CASE("slicing_pipeline_plugin and plugins survive a project 3MF round-trip", "[3mf][Config][plugins]")
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());

    Model src_model;
    ModelObject* src_object = src_model.add_object();
    src_object->name        = "cube";
    src_object->add_volume(make_cube(10., 10., 10.))->name = "cube";
    src_object->add_instance();
    src_object->ensure_on_bed();

    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    store_config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"Stamp"}));
    store_config.set_key_value("plugins", new ConfigOptionStrings({"PipelineStamp;;Stamp"}));

    const std::string test_file = (tmp_root / "pipeline_caps.3mf").string();
    StoreParams       store_params;
    store_params.path     = test_file.c_str();
    store_params.model    = &src_model;
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store_params));

    Model                     dst_model;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::EnableSilent};
    PlateDataPtrs             plate_data;
    std::vector<Preset*>      project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &plate_data, &project_presets, &is_bbl_3mf,
                         &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                             LoadStrategy::Silence));
    release_PlateData_list(plate_data);
    boost::filesystem::remove(test_file);

    REQUIRE(dst_config.has("slicing_pipeline_plugin"));
    CHECK(dst_config.option<ConfigOptionStrings>("slicing_pipeline_plugin")->values ==
          std::vector<std::string>{"Stamp"});
    REQUIRE(dst_config.has("plugins"));
    CHECK(dst_config.option<ConfigOptionStrings>("plugins")->values ==
          std::vector<std::string>{"PipelineStamp;;Stamp"});
}
