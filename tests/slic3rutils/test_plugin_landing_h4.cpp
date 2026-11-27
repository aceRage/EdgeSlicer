// H4 regression tests for the findings of tests/pr_review_2026-11-18_plugin_stack.md that the
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

// ---- H4 -------------------------------------------------------------------------------------

TEST_CASE("H4: plugins always go to the helper, and a missing helper fails the run with a clear message",
          "[PluginLanding][H4][PluginIpc]")
{
    CHECK(plugin_ipc::helper_signature_policy_allows(false, false));
    CHECK(plugin_ipc::helper_signature_policy_allows(false, true));
    CHECK(plugin_ipc::helper_signature_policy_allows(true, true));
    CHECK_FALSE(plugin_ipc::helper_signature_policy_allows(true, false));

    struct ResetOverrides
    {
        ~ResetOverrides() { plugin_ipc::set_allow_helper_env_overrides_for_testing(false); }
    } reset;
    ScopedTemporaryDir nowhere("plugin-landing-no-helper");
    plugin_ipc::set_allow_helper_env_overrides_for_testing(true);
    ScopedEnvVar host_env("EDGE_PLUGIN_HOST", (nowhere.path() / "edge-plugin-host-missing").string());

    CHECK(PluginHostClient::should_use_host());
    const std::string reason = PluginHostClient::host_unavailable_reason();
    CHECK(reason.find("missing") != std::string::npos);
    CHECK(PluginHostClient::host_fallback_active());

    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;
    ScriptCompletion        completion;
    REQUIRE(PluginHostClient::instance().start_script("H4_NoHelper", "Cap", [&](const ScriptCompletion& c) {
        std::lock_guard<std::mutex> lock(mutex);
        completion = c;
        done       = true;
        cv.notify_all();
    }));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(30), [&] { return done; }));
    }
    INFO(completion.error);
    CHECK(completion.error.find("missing") != std::string::npos);
    CHECK(completion.result.message.empty());
}

