#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <catch2/catch.hpp>

#include "plugin_test_utils.hpp"

#include <slic3r/plugin/OrcaApiVersion.hpp>
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonPluginBridge.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/plugin/host/PluginHostBindings.hpp>
#include <slic3r/Utils/HomePluginsLogic.hpp>

#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <pybind11/embed.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

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

// install_hook() records data_dir() once, at interpreter init. Tests then point data_dir()
// at a throwaway tree, so plugin-owned files would sit outside that original allowed root
// unless the current data_dir is granted here.
void allow_current_data_dir_writes()
{
    PluginAuditManager::instance().add_global_allowed_root(data_dir());
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
    CHECK(orca_api_version_supported("<2", error));
    CHECK(orca_api_version_supported("<2.0.0", error));
    CHECK(orca_api_version_supported("<=1.0.0", error));

    error.clear();
    CHECK_FALSE(orca_api_version_supported("1.0.0-beta", error));
    CHECK(error.find("pre-release") != std::string::npos);

    error.clear();
    CHECK_FALSE(orca_api_version_supported(">2", error));

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

static bool wait_for_file(const fs::path& path, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fs::exists(path))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return fs::exists(path);
}

TEST_CASE("A script on a Run worker holds a model snapshot, not the live Plater model", "[PluginScripts][Python][PluginHost]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("plugin-scripts-model-snap");
    allow_current_data_dir_writes();
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / "Hold_Model";
    const fs::path held       = plugin_dir / "held";
    const fs::path go         = plugin_dir / "go";

    const std::string source =
        "# /// script\n"
        "# requires-python = \">=3.12\"\n"
        "#\n"
        "# [tool.orcaslicer.plugin]\n"
        "# name = \"Hold Model\"\n"
        "# type = \"script\"\n"
        "# ///\n"
        "import orca, time, os\n"
        "HOLD = r'''" + held.string() + "'''\n"
        "GO = r'''" + go.string() + "'''\n"
        "class Hold(orca.script.ScriptPluginCapabilityBase):\n"
        "    def get_name(self):\n"
        "        return 'Hold'\n"
        "    def execute(self):\n"
        "        m = orca.host.model()\n"
        "        obj = m.object(0)\n"
        "        name_before = obj.name\n"
        "        bb = m.bounding_box_approx()\n"
        "        open(HOLD, 'w').write('1')\n"
        "        while not os.path.exists(GO):\n"
        "            time.sleep(0.02)\n"
        "        return orca.ExecutionResult.success('%s:%s:%s' % (name_before, obj.name, m.object_count()))\n"
        "@orca.plugin\n"
        "class Pkg(orca.base):\n"
        "    def register_capabilities(self):\n"
        "        orca.register_capability(Hold)\n";

    write_plugin(data_dir_guard, "Hold_Model", source);

    Model live;
    ModelObject* object = live.add_object();
    object->name = "before";
    object->add_volume(make_cube(10.0, 10.0, 10.0));
    object->add_instance();
    host_bindings::set_live_model_for_testing(&live);
    struct LiveModelGuard
    {
        ~LiveModelGuard() { host_bindings::set_live_model_for_testing(nullptr); }
    } live_guard;

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Hold_Model", error));
    INFO("load error: " << error);

    const auto launch_start = std::chrono::steady_clock::now();
    const ScriptCompletion completion = [&] {
        std::mutex              mutex;
        std::condition_variable cv;
        bool                    done = false;
        ScriptCompletion        result;
        auto launch = manager.launch_script_capability("Hold_Model", "Hold", [&](const ScriptCompletion& c) {
            std::lock_guard<std::mutex> lock(mutex);
            result = c;
            done   = true;
            cv.notify_all();
        });
        REQUIRE(launch.launched_async);
        REQUIRE(wait_for_file(held, std::chrono::seconds(10)));
        live.delete_object(size_t(0));
        ModelObject* replacement = live.add_object();
        replacement->name = "after";
        replacement->add_volume(make_cube(4.0, 4.0, 4.0));
        replacement->add_instance();
        { std::ofstream out(go.string()); out << "1"; }
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(30), [&] { return done; }));
        return result;
    }();
    (void) launch_start;

    CHECK(completion.error.empty());
    CHECK_FALSE(completion.cancelled);
    CHECK(completion.result.status == PluginResult::Success);
    CHECK(completion.result.message == "before:before:1");
}

TEST_CASE("A plugin that patches asyncio.new_event_loop does not get unscoped loop construction",
          "[PluginScripts][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("plugin-scripts-asyncio-patch");
    const std::string source = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Patch Loop"
# type = "script"
# ///
import asyncio, orca

flags = {'new_loop': False, 'policy': False}
_real = asyncio.new_event_loop
def evil_new():
    flags['new_loop'] = True
    return _real()
asyncio.new_event_loop = evil_new

class EvilPolicy(asyncio.DefaultEventLoopPolicy):
    def new_event_loop(self):
        flags['policy'] = True
        return super().new_event_loop()
asyncio.set_event_loop_policy(EvilPolicy())

class Echo(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return 'Echo'
    async def execute(self):
        return orca.ExecutionResult.success('%s:%s' % (flags['new_loop'], flags['policy']))

@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(Echo)
)PY";

    write_plugin(data_dir_guard, "Patch_Loop", source);
    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Patch_Loop", error));
    INFO("load error: " << error);

    const ScriptCompletion completion = wait_for_script(manager, "Patch_Loop", "Echo");
    CHECK(completion.error.empty());
    CHECK_FALSE(completion.cancelled);
    CHECK(completion.result.status == PluginResult::Success);
    CHECK(completion.result.message == "False:False");
}

TEST_CASE("Unload abandons a sleeping script after the wait bound", "[PluginScripts][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("plugin-scripts-wait-bound");
    allow_current_data_dir_writes();
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / "Sleep_Forever";
    const fs::path started    = plugin_dir / "started";
    const fs::path stop       = plugin_dir / "stop";
    const fs::path done       = plugin_dir / "done";
    const std::string source =
        "# /// script\n"
        "# requires-python = \">=3.12\"\n"
        "#\n"
        "# [tool.orcaslicer.plugin]\n"
        "# name = \"Sleep Forever\"\n"
        "# type = \"script\"\n"
        "# ///\n"
        "import orca, time, os\n"
        "STARTED = r'''" + started.string() + "'''\n"
        "STOP = r'''" + stop.string() + "'''\n"
        "DONE = r'''" + done.string() + "'''\n"
        "class Sleep(orca.script.ScriptPluginCapabilityBase):\n"
        "    def get_name(self):\n"
        "        return 'Sleep'\n"
        "    def execute(self):\n"
        "        open(STARTED, 'w').write('1')\n"
        "        while not os.path.exists(STOP):\n"
        "            time.sleep(0.05)\n"
        "        open(DONE, 'w').write('1')\n"
        "        return orca.ExecutionResult.success('stopped')\n"
        "@orca.plugin\n"
        "class Pkg(orca.base):\n"
        "    def register_capabilities(self):\n"
        "        orca.register_capability(Sleep)\n";

    write_plugin(data_dir_guard, "Sleep_Forever", source);
    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Sleep_Forever", error));
    INFO("load error: " << error);

    auto launch = manager.launch_script_capability("Sleep_Forever", "Sleep", [](const ScriptCompletion&) {});
    REQUIRE(launch.launched_async);
    REQUIRE(wait_for_file(started, std::chrono::seconds(10)));

    ScriptAsyncRuntime::set_wait_bound_for_testing(std::chrono::milliseconds(250));
    struct WaitBoundGuard
    {
        ~WaitBoundGuard() { ScriptAsyncRuntime::set_wait_bound_for_testing(std::chrono::milliseconds(8000)); }
    } wait_guard;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(manager.unload_plugin("Sleep_Forever"));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed < std::chrono::seconds(3));
    // Unload returned without waiting for the sleeper. Let it observe STOP so the worker
    // can exit before the interpreter shuts down, otherwise a detached Python thread
    // tears down Boost.Log TLS after main and aborts the process.
    { std::ofstream out(stop.string()); out << "1"; }
    CHECK(wait_for_file(done, std::chrono::seconds(5)));
}

TEST_CASE("Event-loop socket permit does not unblock writes or subprocess", "[PluginScripts][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    ScopedPluginAuditContext      audit_scope("permit-test-plugin", "cap");
    ScopedEventLoopSocketPermit   socket_permit;
    py::dict                      locals;
    py::exec(R"PY(
wrote = False
spawned = False
write_err = ''
spawn_err = ''
try:
    open('/tmp/edgeslicer-permit-should-not-write', 'w').write('x')
    wrote = True
except Exception as e:
    write_err = type(e).__name__
try:
    import subprocess
    subprocess.Popen(['true'])
    spawned = True
except Exception as e:
    spawn_err = type(e).__name__
)PY",
             py::globals(), locals);

    CHECK_FALSE(locals["wrote"].cast<bool>());
    CHECK_FALSE(locals["spawned"].cast<bool>());
    CHECK(locals["write_err"].cast<std::string>() == "PermissionError");
    CHECK(locals["spawn_err"].cast<std::string>() == "PermissionError");
}
