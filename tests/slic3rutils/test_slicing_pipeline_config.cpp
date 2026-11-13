#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <catch2/catch.hpp>

#include "plugin_test_utils.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/plugin/PluginConfig.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"

#include <chrono>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>
#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

using namespace Slic3r;
namespace fs = boost::filesystem;
using json   = nlohmann::json;

// End-to-end coverage of a slicing-pipeline capability reading its own config: the loader seeds the
// store from the capability's get_default_config() hook, and the real dispatch
// (execute_capabilities_from_refs -> hook -> GIL -> trampoline) lets the plugin read back whatever
// the host has stored, through self.get_config().

namespace {

struct ScopedPluginManager
{
    ScopedDataDir python_data_dir{"plugin-python"};
    bool          initialized = false;

    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

const char* const CONFIG_PROBE_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
# requires-orca-api = "1.0.0"
#
# [tool.orcaslicer.plugin]
# name = "Config Probe"
# description = "Echoes its own config back to the test"
# author = "tests"
# version = "1.0"
# type = "slicing-pipeline"
# ///
import json

import orca

class ConfigEcho(orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self):
        return "ConfigEcho"

    def get_default_config(self):
        return {"alpha": "1.25", "beta": "hello"}

    def execute(self, ctx):
        if ctx.step != orca.slicing.Step.posSlice or ctx.object is None:
            return orca.ExecutionResult.success()
        try:
            text = repr(sorted(json.loads(self.get_config()).items()))
        except Exception as e:
            text = "config-error: " + repr(e)
        orca._probe_config = text
        return orca.ExecutionResult.success("config probed")

@orca.plugin
class ConfigProbePackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(ConfigEcho)
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

bool load_and_wait(PluginManager& manager, const std::string& plugin_key, std::string& error)
{
    if (!approve_plugin_for_test(plugin_key, error))
        return false;
    manager.load_plugin(plugin_key, /*skip_deps=*/true, {});
    return manager.wait_for_plugin_load(plugin_key, std::chrono::seconds(120), error);
}

void load_config_probe(PluginManager& manager, const ScopedDataDir& data_dir_guard)
{
    write_plugin(data_dir_guard, "ConfigProbe", CONFIG_PROBE_SOURCE);
    manager.get_config().load();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "ConfigProbe", error));
    INFO("load error: " << error);
    REQUIRE(manager.is_plugin_loaded("ConfigProbe"));
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

TEST_CASE("slicing-pipeline dispatch delivers the stored config to self.get_config()", "[slicing_pipeline][PluginConfig][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data_dir_guard("pipeline-config");
    PluginManager& manager = PluginManager::instance();
    load_config_probe(manager, data_dir_guard);

    const PluginCapabilityId id{PluginCapabilityType::SlicingPipeline, "ConfigEcho", "ConfigProbe"};

    const auto seeded = manager.get_config().get_config(id);
    REQUIRE(seeded);
    CHECK(seeded->config == json({{"alpha", "1.25"}, {"beta", "hello"}}));

    REQUIRE(manager.get_config().store_capability_config(id, json({{"alpha", "9.5"}, {"beta", "hello"}})));

    Print print;
    Model model;
    auto  config = DynamicPrintConfig::full_print_config();
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"ConfigEcho"}));
    config.set_key_value("plugins", new ConfigOptionStrings({"ConfigProbe;;ConfigEcho"}));
    init_print_cube(print, model, config);
    print.process();

    std::string observed = "<capability never executed>";
    {
        PythonGILState gil;
        REQUIRE(static_cast<bool>(gil));
        pybind11::module_ orca = pybind11::module_::import("orca");
        if (pybind11::hasattr(orca, "_probe_config"))
            observed = orca.attr("_probe_config").cast<std::string>();
    }
    INFO("config observed by Python: " << observed);
    CHECK(observed.find("'alpha', '9.5'") != std::string::npos);
    CHECK(observed.find("'beta', 'hello'") != std::string::npos);
    CHECK(observed.find("1.25") == std::string::npos);

    manager.unload_plugin("ConfigProbe");
}

TEST_CASE("slicing-pipeline dispatch runs a selected capability missing from the plugins manifest", "[slicing_pipeline][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data_dir_guard("pipeline-no-manifest");
    PluginManager& manager = PluginManager::instance();
    load_config_probe(manager, data_dir_guard);

    {
        PythonGILState gil;
        REQUIRE(static_cast<bool>(gil));
        pybind11::module_ orca = pybind11::module_::import("orca");
        if (pybind11::hasattr(orca, "_probe_config"))
            pybind11::delattr(orca, "_probe_config");
    }

    Print print;
    Model model;
    auto  config = DynamicPrintConfig::full_print_config();
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"ConfigEcho"}));
    config.set_key_value("plugins", new ConfigOptionStrings());
    init_print_cube(print, model, config);
    print.process();

    {
        PythonGILState gil;
        REQUIRE(static_cast<bool>(gil));
        CHECK(pybind11::hasattr(pybind11::module_::import("orca"), "_probe_config"));
    }

    manager.unload_plugin("ConfigProbe");
}
