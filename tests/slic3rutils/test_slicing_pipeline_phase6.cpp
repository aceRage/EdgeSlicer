#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <catch2/catch.hpp>

#include "plugin_test_utils.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/PostProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PluginFsUtils.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PluginResolver.hpp"
#include "slic3r/plugin/PluginTrust.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <pybind11/embed.h>
#include <pybind11/pybind11.h>
#include <wx/event.h>

using namespace Slic3r;
namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

struct ScopedPluginManager
{
    ScopedDataDir python_data_dir{"plugin-python"};
    bool          initialized = false;

    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

const char* const STAMP_BLOCK =
    "; EDGE_PIPELINE_STAMP begin\n"
    "; EdgeSlicer pipeline plugin stamp v1\n"
    "; EDGE_PIPELINE_STAMP end\n";

const char* const STAMP_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
# requires-orca-api = "1.0.0"
#
# [tool.orcaslicer.plugin]
# name = "Pipeline Stamp"
# description = "Appends a fixed comment block at psGCodePostProcess."
# author = "tests"
# version = "1.0.0"
# type = "slicing-pipeline"
# ///
import orca

STAMP = (
    "; EDGE_PIPELINE_STAMP begin\n"
    "; EdgeSlicer pipeline plugin stamp v1\n"
    "; EDGE_PIPELINE_STAMP end\n"
)

class Stamp(orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self):
        return "Stamp"

    def execute(self, ctx):
        if ctx.step != orca.slicing.Step.psGCodePostProcess:
            return orca.ExecutionResult.success()
        if not ctx.gcode_path:
            return orca.ExecutionResult.failure(orca.PluginResult.FatalError, "missing gcode_path")
        with open(ctx.gcode_path, "a", encoding="utf-8") as f:
            f.write(STAMP)
        return orca.ExecutionResult.success("stamped")

@orca.plugin
class PipelineStampPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(Stamp)
)PY";

const char* const WRITE_PROBE_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
# requires-orca-api = "1.0.0"
#
# [tool.orcaslicer.plugin]
# name = "Write Probe"
# description = "Probes the psGCodePostProcess write root"
# author = "tests"
# version = "1.0.0"
# type = "slicing-pipeline"
# ///
import json
import os

import orca

class WriteProbe(orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self):
        return "WriteProbe"

    def execute(self, ctx):
        if ctx.step != orca.slicing.Step.psGCodePostProcess:
            return orca.ExecutionResult.success()
        targets_path = os.path.join(os.path.dirname(ctx.gcode_path), "probe_targets.json")
        with open(targets_path, "r", encoding="utf-8") as f:
            targets = json.load(f)
        results = {}
        for name, path in targets.items():
            try:
                with open(path, "w", encoding="utf-8") as out:
                    out.write("wrote:" + name)
                results[name] = "ok"
            except Exception as e:
                results[name] = type(e).__name__ + ":" + str(e)
        orca._write_probe = json.dumps(results, sort_keys=True)
        return orca.ExecutionResult.success("probed")

@orca.plugin
class WriteProbePackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(WriteProbe)
)PY";

const char* const BOOM_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
# requires-orca-api = "1.0.0"
#
# [tool.orcaslicer.plugin]
# name = "Boom"
# description = "Fails at psGCodePostProcess"
# author = "tests"
# version = "1.0.0"
# type = "slicing-pipeline"
# ///
import orca

class Boom(orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self):
        return "Boom"

    def execute(self, ctx):
        if ctx.step != orca.slicing.Step.psGCodePostProcess:
            return orca.ExecutionResult.success()
        return orca.ExecutionResult.failure(orca.PluginResult.FatalError, "boom")

@orca.plugin
class BoomPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(Boom)
)PY";

fs::path write_plugin(const ScopedDataDir& data_dir_guard, const std::string& stem, const std::string& source)
{
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / stem;
    fs::create_directories(plugin_dir);
    std::ofstream out((plugin_dir / (stem + ".py")).string(), std::ios::binary);
    out << source;
    out.close();
    return plugin_dir;
}

bool load_and_wait(PluginManager& manager, const std::string& plugin_key, std::string& error,
                   std::vector<std::string> capabilities_to_enable = {})
{
    if (!approve_plugin_for_test(plugin_key, error))
        return false;
    manager.load_plugin(plugin_key, /*skip_deps=*/true, std::move(capabilities_to_enable));
    return manager.wait_for_plugin_load(plugin_key, std::chrono::seconds(120), error);
}

void discover_and_load(PluginManager& manager, const ScopedDataDir& data_dir_guard, const std::string& stem,
                       const std::string& source, const std::vector<std::string>& caps)
{
    write_plugin(data_dir_guard, stem, source);
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, stem, error, caps));
    INFO("load error: " << error);
    REQUIRE(manager.is_plugin_loaded(stem));
}

std::string read_file(const fs::path& path)
{
    std::ifstream in(path.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& path, const std::string& contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    out << contents;
}

DynamicPrintConfig pipeline_config(const std::string& cap, const std::string& plugin_key)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({cap}));
    config.set_key_value("plugins", new ConfigOptionStrings({plugin_key + ";;" + cap}));
    return config;
}

bool run_post_process(const fs::path& gcode, const DynamicPrintConfig& config, std::exception_ptr& ex)
{
    std::string path        = gcode.string();
    std::string output_name = gcode.string();
    try {
        return run_post_process_scripts(path, false, "File", output_name, config);
    } catch (...) {
        ex = std::current_exception();
        return false;
    }
}

SlicingProcessCompletedEvent completed(SlicingProcessCompletedEvent::StatusType status, std::exception_ptr ex)
{
    return SlicingProcessCompletedEvent(wxEVT_NULL, 0, status, std::move(ex));
}

void init_print_cube(Print& print, Model& model, const DynamicPrintConfig& config_in)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.apply(config_in);
    ModelObject* object = model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(20, 20, 20));
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
}

} // namespace

TEST_CASE("Startup does not load a pipeline plugin whose sidecar is missing", "[slicing_pipeline][Python][startup]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("pipeline-startup");
    write_plugin(data_dir_guard, "PipelineStamp", STAMP_SOURCE);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    const std::vector<std::string> keys = manager.get_enabled_plugin_keys();
    CHECK(std::find(keys.begin(), keys.end(), "PipelineStamp") == keys.end());
    CHECK_FALSE(manager.is_plugin_loaded("PipelineStamp"));
}

TEST_CASE("Stamp fixture appends the exact comment block at psGCodePostProcess", "[slicing_pipeline][Python][stamp]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data_dir_guard("pipeline-stamp");
    PluginManager& manager = PluginManager::instance();
    discover_and_load(manager, data_dir_guard, "PipelineStamp", STAMP_SOURCE, {"Stamp"});

    const std::string original = "G1 X10 Y10 E0.1\n; original trailer\n";
    ScopedTemporaryDir gcode_dir("pipeline-stamp-gcode");
    const fs::path     gcode     = gcode_dir.path() / "box.gcode";
    write_file(gcode, original);

    DynamicPrintConfig config = pipeline_config("Stamp", "PipelineStamp");
    std::exception_ptr ex;
    REQUIRE(run_post_process(gcode, config, ex));
    REQUIRE_FALSE(ex);
    CHECK(read_file(gcode) == original + STAMP_BLOCK);

    manager.unload_plugin("PipelineStamp");
}

TEST_CASE("No pipeline plugin: post-process leaves G-code byte-identical", "[slicing_pipeline][Python][stamp]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::string original = "G1 X10 Y10 E0.1\n";
    ScopedTemporaryDir gcode_dir("pipeline-nop-gcode");
    const fs::path     gcode     = gcode_dir.path() / "box.gcode";
    write_file(gcode, original);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    std::exception_ptr ex;
    CHECK_FALSE(run_post_process(gcode, config, ex));
    REQUIRE_FALSE(ex);
    CHECK(read_file(gcode) == original);
}

TEST_CASE("An untrusted pipeline plugin fails the slice instead of running", "[slicing_pipeline][Python][trust]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("pipeline-untrusted");
    write_plugin(data_dir_guard, "PipelineStamp", STAMP_SOURCE);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    PluginDescriptor descriptor;
    REQUIRE(manager.try_get_plugin_descriptor("PipelineStamp", descriptor));
    CHECK_FALSE(plugin_trust::evaluate(descriptor).allowed());
    CHECK_FALSE(manager.is_plugin_loaded("PipelineStamp"));

    const std::string original = "G1 X10 Y10 E0.1\n";
    ScopedTemporaryDir gcode_dir("pipeline-untrusted-gcode");
    const fs::path     gcode     = gcode_dir.path() / "box.gcode";
    write_file(gcode, original);

    DynamicPrintConfig config = pipeline_config("Stamp", "PipelineStamp");
    std::exception_ptr ex;
    CHECK_FALSE(run_post_process(gcode, config, ex));
    REQUIRE(ex);
    CHECK_THROWS_AS(std::rethrow_exception(ex), ExportError);
    try {
        std::rethrow_exception(ex);
    } catch (const std::exception& err) {
        CHECK(std::string(err.what()).find("untrusted") != std::string::npos);
    }
    CHECK(read_file(gcode) == original);
}

TEST_CASE("psGCodePostProcess write root is the G-code folder and not UltraNet plugins",
          "[slicing_pipeline][Python][audit]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data_dir_guard("pipeline-write-root");
    PluginManager& manager = PluginManager::instance();
    discover_and_load(manager, data_dir_guard, "WriteProbe", WRITE_PROBE_SOURCE, {"WriteProbe"});

    ScopedTemporaryDir gcode_dir("pipeline-write-gcode");
    const fs::path     gcode     = gcode_dir.path() / "box.gcode";
    write_file(gcode, "G1 X0 Y0\n");

    const fs::path sidecar = gcode_dir.path() / "sidecar.txt";
    const fs::path ultra   = fs::path(data_dir()) / "plugins" / "escape.txt";
    fs::create_directories(ultra.parent_path());
    const fs::path outside = fs::temp_directory_path() / "edge-pipeline-outside.txt";
    boost::system::error_code ec;
    fs::remove(outside, ec);

    json targets = {{"gcode_folder", sidecar.string()}, {"ultranet", ultra.string()}, {"outside", outside.string()}};
    write_file(gcode_dir.path() / "probe_targets.json", targets.dump());

    // Production deny-first: a worker thread must not pop a Yes/No dialog. The test seam would
    // short-circuit that path, so leave it unset and run the plugin off the main thread.
    DynamicPrintConfig config = pipeline_config("WriteProbe", "WriteProbe");
    std::exception_ptr ex;
    std::thread worker([&] { run_post_process(gcode, config, ex); });
    worker.join();

    REQUIRE_FALSE(ex);
    CHECK(fs::exists(sidecar));
    CHECK(read_file(sidecar) == "wrote:gcode_folder");
    CHECK_FALSE(fs::exists(ultra));
    CHECK_FALSE(fs::exists(outside));

    std::string observed;
    {
        PythonGILState gil;
        REQUIRE(static_cast<bool>(gil));
        pybind11::module_ orca = pybind11::module_::import("orca");
        if (pybind11::hasattr(orca, "_write_probe"))
            observed = orca.attr("_write_probe").cast<std::string>();
    }
    INFO("write probe: " << observed);
    CHECK(observed.find("\"gcode_folder\": \"ok\"") != std::string::npos);
    CHECK(observed.find("ultranet") != std::string::npos);
    CHECK(observed.find("\"ultranet\": \"ok\"") == std::string::npos);
    CHECK(observed.find("\"outside\": \"ok\"") == std::string::npos);

    manager.unload_plugin("WriteProbe");
}

TEST_CASE("A missing pipeline plugin is skipped with a warning, not a silent run", "[slicing_pipeline][Python][missing]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("pipeline-missing");
    PluginManager::instance().discover_plugins(/*async=*/false, /*clear=*/true);

    Print print;
    Model model;
    auto  config = pipeline_config("GhostCap", "GhostPlugin");
    init_print_cube(print, model, config);
    REQUIRE_NOTHROW(print.process());

    Preset preset(Preset::TYPE_PRINT, "missing-plugin-print");
    preset.config = config;
    refresh_missing_plugins(Preset::TYPE_PRINT, preset.config.option<ConfigOptionStrings>("plugins"), &preset);
    REQUIRE(has_missing_plugins());
    const std::string warning = missing_plugins_warning_message();
    CHECK(warning.find("GhostPlugin") != std::string::npos);
    CHECK(warning.find("skip") != std::string::npos);
}

TEST_CASE("Unresolved plugin capabilities produce the #16929 save warning", "[slicing_pipeline][Python][missing]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"GhostCap"}));
    const auto unresolved = config.unresolved_plugin_capabilities();
    REQUIRE_FALSE(unresolved.empty());
    CHECK(unresolved.front().capability_name == "GhostCap");
    CHECK(unresolved.front().opt_key == "slicing_pipeline_plugin");
    const std::string text = unresolved_plugins_save_message(unresolved);
    CHECK(text.find("GhostCap") != std::string::npos);
    CHECK(text.find("Save anyway") != std::string::npos);
}

TEST_CASE("A pipeline plugin failure is an ExportError that does not invalidate the plate",
          "[slicing_pipeline][Python][retry]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data_dir_guard("pipeline-boom");
    PluginManager& manager = PluginManager::instance();
    discover_and_load(manager, data_dir_guard, "Boom", BOOM_SOURCE, {"Boom"});

    ScopedTemporaryDir gcode_dir("pipeline-boom-gcode");
    const fs::path     gcode     = gcode_dir.path() / "box.gcode";
    write_file(gcode, "G1 X10 Y10\n");

    DynamicPrintConfig config = pipeline_config("Boom", "Boom");
    std::exception_ptr ex;
    CHECK_FALSE(run_post_process(gcode, config, ex));
    REQUIRE(ex);
    CHECK_THROWS_AS(std::rethrow_exception(ex), ExportError);

    const SlicingProcessCompletedEvent evt = completed(SlicingProcessCompletedEvent::Error, ex);
    CHECK(evt.export_failed());
    CHECK_FALSE(evt.invalidate_plater());

    manager.unload_plugin("Boom");
}

TEST_CASE("slicing_pipeline_plugin and plugins survive a project 3MF round-trip", "[slicing_pipeline][3mf]")
{
    ScopedTemporaryDir tmp("pipeline-3mf");
    Slic3r::set_temporary_dir(tmp.path().string());

    Model src_model;
    ModelObject* src_object = src_model.add_object();
    src_object->name        = "cube";
    src_object->add_volume(make_cube(10., 10., 10.))->name = "cube";
    src_object->add_instance();
    src_object->ensure_on_bed();

    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    store_config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"Stamp"}));
    store_config.set_key_value("plugins", new ConfigOptionStrings({"PipelineStamp;;Stamp"}));

    const std::string test_file = (tmp.path() / "pipeline_caps.3mf").string();
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

    REQUIRE(dst_config.has("slicing_pipeline_plugin"));
    CHECK(dst_config.option<ConfigOptionStrings>("slicing_pipeline_plugin")->values ==
          std::vector<std::string>{"Stamp"});
    REQUIRE(dst_config.has("plugins"));
    CHECK(dst_config.option<ConfigOptionStrings>("plugins")->values ==
          std::vector<std::string>{"PipelineStamp;;Stamp"});
}

TEST_CASE("Loaded pipeline plugin fills the plugins manifest on save", "[slicing_pipeline][Python][manifest]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data_dir_guard("pipeline-manifest");
    PluginManager& manager = PluginManager::instance();
    discover_and_load(manager, data_dir_guard, "PipelineStamp", STAMP_SOURCE, {"Stamp"});

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"Stamp"}));
    config.update_plugin_manifest();
    const auto* plugins = config.option<ConfigOptionStrings>("plugins");
    REQUIRE(plugins != nullptr);
    REQUIRE_FALSE(plugins->values.empty());
    CHECK(plugins->values.front().find("Stamp") != std::string::npos);

    manager.unload_plugin("PipelineStamp");
}
