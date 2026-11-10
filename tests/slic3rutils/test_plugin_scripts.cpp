#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <catch2/catch.hpp>

#include "plugin_test_utils.hpp"

#include <slic3r/plugin/OrcaApiVersion.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonPluginBridge.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/Utils/HomePluginsLogic.hpp>

#include <pybind11/embed.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <string>

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace py = pybind11;

namespace {

struct ScopedPluginManager
{
    ScopedDataDir python_data_dir{"plugin-python"};
    bool          initialized = false;

    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

const char* const SYNC_PLUGIN_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
# requires-orca-api = "1.0.0"
#
# [tool.orcaslicer.plugin]
# name = "Sync Script"
# description = "Synchronous execute fixture"
# author = "tests"
# version = "1.0"
# type = "script"
# ///
import orca

class Echo(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Echo"

    def execute(self):
        return orca.ExecutionResult.success("sync-ok")

@orca.plugin
class EchoPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(Echo)
)PY";

const char* const ASYNC_PLUGIN_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Async Script"
# description = "async def execute fixture"
# author = "tests"
# version = "1.0"
# type = "script"
# ///
import orca

class Echo(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Echo"

    async def execute(self):
        return orca.ExecutionResult.success("async-ok")

@orca.plugin
class EchoPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(Echo)
)PY";

const char* const FUTURE_API_PLUGIN_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
# requires-orca-api = "99.0.0"
#
# [tool.orcaslicer.plugin]
# name = "Future API"
# description = "Requires a newer orca API than Edge advertises"
# author = "tests"
# version = "1.0"
# type = "script"
# ///
import orca

class Echo(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Echo"

    def execute(self):
        return orca.ExecutionResult.success()

@orca.plugin
class EchoPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(Echo)
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
    manager.load_plugin(plugin_key, /*skip_deps=*/true);
    return manager.wait_for_plugin_load(plugin_key, std::chrono::seconds(120), error);
}

ScriptCompletion wait_for_script(PluginManager& manager, const std::string& plugin_key, const std::string& cap_name)
{
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;
    ScriptCompletion        completion;

    auto launch = manager.launch_script_capability(plugin_key, cap_name, [&](const ScriptCompletion& c) {
        std::lock_guard<std::mutex> lock(mutex);
        completion = c;
        done       = true;
        cv.notify_all();
    });
    REQUIRE(launch.launched_async);

    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(60), [&] { return done; }));
    return completion;
}

} // namespace

TEST_CASE("Advertised orca API version refuses a newer requirement", "[PluginScripts]")
{
    CHECK(std::string(k_orca_api_version) == "1.0.0");

    std::string error;
    CHECK(orca_api_version_supported("", error));
    CHECK(error.empty());
    CHECK(orca_api_version_supported("1.0.0", error));
    CHECK(orca_api_version_supported("1.0", error));
    CHECK(orca_api_version_supported(">=1.0.0", error));

    error.clear();
    CHECK_FALSE(orca_api_version_supported("99.0.0", error));
    CHECK(error.find("99.0.0") != std::string::npos);
    CHECK(error.find(k_orca_api_version) != std::string::npos);

    error.clear();
    CHECK_FALSE(orca_api_version_supported("not-a-version", error));
}

TEST_CASE("A synchronous script execute runs off the caller thread", "[PluginScripts][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("plugin-scripts-sync");
    write_plugin(data_dir_guard, "Sync_Script", SYNC_PLUGIN_SOURCE);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    PluginDescriptor descriptor;
    REQUIRE(manager.try_get_valid_plugin_descriptor("Sync_Script", descriptor));
    CHECK(descriptor.required_orca_api == "1.0.0");

    std::string error;
    REQUIRE(load_and_wait(manager, "Sync_Script", error));
    INFO("load error: " << error);
    REQUIRE(manager.is_plugin_loaded("Sync_Script"));

    const ScriptCompletion completion = wait_for_script(manager, "Sync_Script", "Echo");
    CHECK(completion.error.empty());
    CHECK_FALSE(completion.cancelled);
    CHECK(completion.result.status == PluginResult::Success);
    CHECK(completion.result.message == "sync-ok");
}

TEST_CASE("An async def execute runs off the UI thread", "[PluginScripts][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("plugin-scripts-async");
    write_plugin(data_dir_guard, "Async_Script", ASYNC_PLUGIN_SOURCE);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    REQUIRE(load_and_wait(manager, "Async_Script", error));
    INFO("load error: " << error);
    REQUIRE(manager.is_plugin_loaded("Async_Script"));

    const ScriptCompletion completion = wait_for_script(manager, "Async_Script", "Echo");
    CHECK(completion.error.empty());
    CHECK_FALSE(completion.cancelled);
    CHECK(completion.result.status == PluginResult::Success);
    CHECK(completion.result.message == "async-ok");
}

TEST_CASE("Unported host APIs raise a clear Edge-unavailable error", "[PluginScripts][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    (void) PythonPluginBridge::instance();
    py::module_ orca = py::module_::import("orca");
    py::object  host = orca.attr("host");

    auto getattr_unavailable = [](py::object obj, const char* name) {
        try {
            (void) py::getattr(obj, name);
            FAIL(std::string("attribute ") + name + " unexpectedly resolved");
        } catch (const py::error_already_set& error) {
            CHECK(error.matches(PyExc_RuntimeError));
            CHECK(std::string(error.what()).find("not available in this Edge build") != std::string::npos);
        }
    };

    getattr_unavailable(orca.attr("pages"), "PageBase");
    getattr_unavailable(orca.attr("printer_connection"), "PrinterAgentBase");
    getattr_unavailable(orca.attr("resource_texture"), "ResourceTextureBase");

    try {
        host.attr("slicing").attr("Print")();
        FAIL("orca.host.slicing.Print unexpectedly succeeded");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_RuntimeError));
        CHECK(std::string(error.what()).find("not available in this Edge build") != std::string::npos);
    }

    try {
        host.attr("preset").attr("bundles").attr("reload_local")("test-bundle");
        FAIL("preset bundle reload unexpectedly succeeded");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_RuntimeError));
        CHECK(std::string(error.what()).find("not available in this Edge build") != std::string::npos);
    }
}

TEST_CASE("A plugin that asks for a newer orca API is refused", "[PluginScripts][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("plugin-scripts-future-api");
    write_plugin(data_dir_guard, "Future_API", FUTURE_API_PLUGIN_SOURCE);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    PluginDescriptor descriptor;
    REQUIRE(manager.try_get_plugin_descriptor("Future_API", descriptor));
    CHECK(descriptor.metadata_valid);
    CHECK(descriptor.required_orca_api == "99.0.0");
    REQUIRE(descriptor.has_error());
    CHECK(descriptor.error.find("99.0.0") != std::string::npos);
    CHECK(descriptor.error.find(k_orca_api_version) != std::string::npos);

    std::string error;
    CHECK_FALSE(load_and_wait(manager, "Future_API", error));
    CHECK(error.find("99.0.0") != std::string::npos);
    CHECK_FALSE(manager.is_plugin_loaded("Future_API"));
}

TEST_CASE("Home plugin_run only accepts a plugin_key the host listed", "[PluginScripts][HomePlugins]")
{
    using HomeTab::is_listed_plugin_key;
    const std::vector<std::string> listed{"hello_script", "other"};
    CHECK(is_listed_plugin_key("hello_script", listed));
    CHECK_FALSE(is_listed_plugin_key("missing", listed));
    CHECK_FALSE(is_listed_plugin_key("", listed));
    CHECK_FALSE(is_listed_plugin_key("/tmp/orca_plugins/hello", listed));
    CHECK_FALSE(is_listed_plugin_key("../plugins/ultranet", listed));
}
