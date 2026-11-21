// H1 regression tests for the findings of tests/pr_review_2026-11-18_plugin_stack.md that the
// landing branch (feature/plugins-landing) fixes: H1 start-up hooks, H2 helper message
// validation, H3 GIL-less UI marshal, H4 fail-closed helper, H5 secret stores.
// Only fixture plugins written by these tests run; nothing is downloaded.

#include <catch2/catch.hpp>

#include "plugin_test_utils.hpp"

#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/host/PluginHostUiMarshal.hpp>
#include <slic3r/plugin/ipc/PluginHostClient.hpp>
#include <slic3r/plugin/ipc/PluginHostProcess.hpp>
#include <slic3r/plugin/ipc/PluginIpcProtocol.hpp>
#include <slic3r/plugin/ipc/PluginSandbox.hpp>

#include "slic3r/plugin/EdgePythonInclude.hpp"
#include <pybind11/embed.h>

#include <boost/filesystem.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace py = pybind11;
using plugin_ipc::PluginHostClient;

namespace {

struct [[maybe_unused]] ScopedPluginManager
{
    ScopedDataDir python_data_dir{"plugin-landing-python"};
    bool          initialized = false;

    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

[[maybe_unused]] void write_text(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    out << text;
}

[[maybe_unused]] ScriptCompletion run_script_and_wait(const std::string& plugin_key, const std::string& cap, std::chrono::seconds timeout)
{
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;
    ScriptCompletion        completion;
    auto launch = PluginManager::instance().launch_script_capability(plugin_key, cap, [&](const ScriptCompletion& c) {
        std::lock_guard<std::mutex> lock(mutex);
        completion = c;
        done       = true;
        cv.notify_all();
    });
    REQUIRE(launch.launched_async);
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(cv.wait_for(lock, timeout, [&] { return done; }));
    return completion;
}

// A fixture plugin that reports, from wherever it runs, whether start-up hook code ran and
// whether a module from the shared packages folder can be imported.
[[maybe_unused]] const char* const H1_PROBE_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "H1 Probe"
# type = "script"
# ///
import builtins, sys, orca

class Probe(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Probe"
    def execute(self):
        planted = hasattr(builtins, "EDGE_H1_PLANTED") or "sitecustomize" in sys.modules
        try:
            import edge_h1_probe
            probe = edge_h1_probe.VALUE
        except Exception as ex:
            probe = "import failed: " + str(ex)
        return orca.ExecutionResult.success("planted=%s probe=%s no_site=%s" % (planted, probe, sys.flags.no_site))

@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(Probe)
)PY";

} // namespace

// ---- H1 -------------------------------------------------------------------------------------

TEST_CASE("H1: the app interpreter starts without site and adds the shared packages after the hook", "[PluginLanding][H1][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    CHECK(PythonInterpreter::instance().shared_packages_on_sys_path());
    py::gil_scoped_acquire gil;
    py::module_            sys = py::module_::import("sys");
    CHECK(sys.attr("flags").attr("no_site").cast<int>() == 1);
    py::dict modules = sys.attr("modules");
    CHECK_FALSE(modules.contains("sitecustomize"));
    CHECK_FALSE(modules.contains("usercustomize"));
    // The shared packages are appended: every stdlib entry comes first.
    py::list path = sys.attr("path");
    REQUIRE(py::len(path) > 0);
    const std::string packages = fs::path(PythonInterpreter::shared_packages_dir()).filename().string();
    bool              seen     = false;
    for (const py::handle entry : path) {
        const std::string e = py::str(entry).cast<std::string>();
        if (fs::path(e).filename().string() == packages)
            seen = true;
    }
    CHECK(seen);
}

TEST_CASE("H1: a sitecustomize in the shared packages never runs in the helper; packages import after the sandbox",
          "[PluginLanding][H1][PluginIpc][Python]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data("plugin-landing-h1-host");
    const fs::path packages = PythonInterpreter::shared_packages_dir();
    write_text(packages / "sitecustomize.py", "import builtins\nbuiltins.EDGE_H1_PLANTED = True\n");
    write_text(packages / "edge_h1_probe.py", "VALUE = 42\n");
    const fs::path plugin_dir = data.plugins_dir() / "H1_Probe";
    write_text(plugin_dir / "H1_Probe.py", H1_PROBE_SOURCE);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(approve_plugin_for_test("H1_Probe", error));
    manager.load_plugin("H1_Probe", /*skip_deps=*/true);
    REQUIRE(manager.wait_for_plugin_load("H1_Probe", std::chrono::seconds(120), error));

    const ScriptCompletion completion = run_script_and_wait("H1_Probe", "Probe", std::chrono::seconds(120));
    manager.unload_plugin("H1_Probe");
    INFO(completion.error);
    INFO(completion.result.message);
    REQUIRE(completion.error.empty());
    CHECK(completion.result.message.find("planted=False") != std::string::npos);
    CHECK(completion.result.message.find("probe=42") != std::string::npos);
    CHECK(completion.result.message.find("no_site=1") != std::string::npos);
}

