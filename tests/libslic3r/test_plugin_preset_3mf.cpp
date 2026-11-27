#include <edge_test_scratch.hpp>
#include <catch2/catch.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

TEST_CASE("slicing_pipeline_plugin and plugins survive a project 3MF round-trip", "[3mf][Config][plugins]")
{
    const boost::filesystem::path tmp_root = edge_test::snorca_tests_dir();
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

static const char *const k_plugin_json_keys[] = {
    "plugins",
    "slicing_pipeline_plugin",
    "print_plugin_config_overrides",
    "printer_plugin_config_overrides",
    "filament_plugin_config_overrides",
};

TEST_CASE("Empty plugin keys are omitted from JSON serialization", "[3mf][Config][plugins][gcode_dump]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    std::ostringstream ss;
    cfg.save_to_json(ss, "project_settings", "project", "0");
    const std::string json = ss.str();
    for (const char *key : k_plugin_json_keys)
        CHECK(json.find(std::string("\"") + key + "\"") == std::string::npos);
}

TEST_CASE("Non-empty plugin keys are kept in JSON serialization", "[3mf][Config][plugins][gcode_dump]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"Stamp"}));
    cfg.set_key_value("plugins", new ConfigOptionStrings({"PipelineStamp;;Stamp"}));
    cfg.set_key_value("print_plugin_config_overrides", new ConfigOptionString("[{\"name\":\"Stamp\"}]"));
    std::ostringstream ss;
    cfg.save_to_json(ss, "project_settings", "project", "0");
    const std::string json = ss.str();
    CHECK(json.find("\"slicing_pipeline_plugin\"") != std::string::npos);
    CHECK(json.find("\"plugins\"") != std::string::npos);
    CHECK(json.find("\"print_plugin_config_overrides\"") != std::string::npos);
    CHECK(json.find("Stamp") != std::string::npos);
}

namespace {

std::string plugin_test_zip_entry(const std::string &zip_path, const std::string &entry_name)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return {};
    size_t      size = 0;
    void       *data = mz_zip_reader_extract_file_to_heap(&archive, entry_name.c_str(), &size, 0);
    std::string result;
    if (data != nullptr) {
        result.assign(static_cast<const char *>(data), size);
        mz_free(data);
    }
    close_zip_reader(&archive);
    return result;
}

// Stores a one-cube project with `config` and returns Metadata/project_settings.config.
std::string stored_project_settings(DynamicPrintConfig &config, bool bambu_compat, const std::string &name)
{
    const boost::filesystem::path tmp_root = edge_test::snorca_tests_dir();
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());

    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(10., 10., 10.))->name = "cube";
    object->add_instance();
    object->ensure_on_bed();

    const std::string        path = (tmp_root / name).string();
    BambuExport::Report      report;
    StoreParams              store_params;
    store_params.path         = path.c_str();
    store_params.model        = &model;
    store_params.config       = &config;
    store_params.strategy     = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    store_params.bambu_compat = bambu_compat;
    store_params.bambu_report = bambu_compat ? &report : nullptr;
    if (!store_bbs_3mf(store_params))
        return {};
    std::string settings = plugin_test_zip_entry(path, "Metadata/project_settings.config");
    boost::filesystem::remove(path);
    return settings;
}

} // namespace

TEST_CASE("A project 3MF with no plugin selected carries no plugin keys", "[3mf][Config][plugins]")
{
    DynamicPrintConfig config   = DynamicPrintConfig::full_print_config();
    const std::string  settings = stored_project_settings(config, false, "plugin_keys_unset.3mf");
    REQUIRE_FALSE(settings.empty());
    CHECK(settings.find("\"layer_height\"") != std::string::npos);
    for (const char *key : k_plugin_json_keys)
        CHECK(settings.find(std::string("\"") + key + "\"") == std::string::npos);
}

TEST_CASE("Export Bambu 3MF drops the plugin keys even when they are set", "[3mf][Config][plugins][BambuExport]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"Stamp"}));
    config.set_key_value("plugins", new ConfigOptionStrings({"PipelineStamp;;Stamp"}));
    config.set_key_value("print_plugin_config_overrides", new ConfigOptionString("[{\"name\":\"Stamp\"}]"));
    config.set_key_value("printer_plugin_config_overrides", new ConfigOptionString("[{\"name\":\"P\"}]"));
    config.set_key_value("filament_plugin_config_overrides", new ConfigOptionString("[{\"name\":\"F\"}]"));

    // A normal save keeps them (the project names its plugin) ...
    const std::string ours = stored_project_settings(config, false, "plugin_keys_ours.3mf");
    REQUIRE_FALSE(ours.empty());
    CHECK(ours.find("\"slicing_pipeline_plugin\"") != std::string::npos);

    // ... a Bambu export never does: Bambu Studio has no such keys.
    const std::string bambu = stored_project_settings(config, true, "plugin_keys_bambu.3mf");
    REQUIRE_FALSE(bambu.empty());
    for (const char *key : k_plugin_json_keys) {
        INFO(key);
        CHECK(BambuExport::find_key(key) == nullptr);
        CHECK(bambu.find(std::string("\"") + key + "\"") == std::string::npos);
    }
    CHECK(bambu.find("Stamp") == std::string::npos);
}
