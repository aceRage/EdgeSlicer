// H5 regression tests for the findings of tests/pr_review_2026-11-18_plugin_stack.md that the
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

// ---- H5 -------------------------------------------------------------------------------------

TEST_CASE("H5: hub, relay, access-code, cloud, config and trust stores are denied to plugins", "[PluginLanding][H5][audit]")
{
    ScopedDataDir data("plugin-landing-h5");
    const fs::path d   = data.dir;
    const std::string conf = PluginAuditManager::default_denied_filenames().front();
    for (const fs::path& f : {d / "hub" / "hub.json", d / "hub" / "settings.json", d / "hub" / "printers.json", d / "hub" / "u1_lan_auth.json",
                              d / "plugins" / "bambu_networking.dll", d / "orca_plugins" / "trust.json", d / "plugin_host_acl" / "acl_journal.json",
                              d / "go2rtc.yaml", d / "simplyprint_oauth.json", d / conf, d / "user" / "default" / "print" / "mine.json",
                              d / "orca_plugins" / "Foo" / "Foo.py"})
        write_text(f, "x");

    PluginAuditManager& audit = PluginAuditManager::instance();
    for (const fs::path& secret : {d / "hub", d / "hub" / "hub.json", d / "hub" / "settings.json", d / "hub" / "u1_lan_auth.json",
                                   d / "hub" / "not_yet_written.json", d / "plugins" / "bambu_networking.dll", d / "orca_plugins" / "trust.json",
                                   d / "plugin_host_acl" / "acl_journal.json", d / "go2rtc.yaml", d / "simplyprint_oauth.json", d / conf,
                                   d / "HUB" / "hub.json"}) {
        INFO(secret.string());
        CHECK(audit.is_secret_store_path(secret));
        CHECK(audit.is_denied_path(secret));
    }
    for (const fs::path& fine : {d / "user" / "default" / "print" / "mine.json", d / "orca_plugins" / "Foo" / "Foo.py",
                                 d / "plugin_host" / "host.log", d / "hubby.json", d / "orca_plugins" / "plugin_data" / "Foo"}) {
        INFO(fine.string());
        CHECK_FALSE(audit.is_secret_store_path(fine));
    }

    // In a plugin context the audit refuses them before any allowed root (data_dir is one).
    {
        ScopedPluginAuditContext ctx("H5_Probe", "Cap");
        const AuditDecision      read  = audit.check_path_access(d / "hub" / "hub.json", false);
        const AuditDecision      write = audit.check_path_access(d / "orca_plugins" / "trust.json", true);
        CHECK_FALSE(read.allowed);
        CHECK_FALSE(write.allowed);
    }
}

TEST_CASE("H5: OS grants never reach a secret store; covering grants are narrowed", "[PluginLanding][H5][PluginSandbox]")
{
    ScopedTemporaryDir home("plugin-landing-h5-home");
    const fs::path     data_root = home.path() / "AppData" / "EdgeSlicer";
    ScopedTemporaryDir unused("plugin-landing-h5-unused");
    struct DataDirGuard
    {
        std::string previous = data_dir();
        ~DataDirGuard() { set_data_dir(previous); }
    } guard;
    fs::create_directories(data_root);
    set_data_dir(data_root.string());
    const std::string conf = PluginAuditManager::default_denied_filenames().front();
    write_text(data_root / "hub" / "hub.json", "x");
    write_text(data_root / conf, "x");
    write_text(data_root / "user" / "a.json", "x");
    write_text(data_root / "orca_plugins" / "trust.json", "x");
    write_text(data_root / "orca_plugins" / "Foo" / "Foo.py", "x");
    write_text(home.path() / "Documents" / "model.stl", "x");
    write_text(home.path() / "notes.txt", "x");

    plugin_ipc::SandboxSpec spec;
    spec.extra_paths = {
        {home.path().string(), false},                      // %USERPROFILE%/*: narrowed
        {data_root.string(), true},                         // {data_dir}/*: narrowed
        {(data_root / "hub").string(), false},              // a secret store: dropped
        {(data_root / conf).string(), false},               // the app config file: dropped
        {(home.path() / "Documents").string(), true},       // unrelated: kept
    };
    plugin_ipc::narrow_grants_around_secret_stores(spec);

    std::vector<std::string> granted;
    for (const auto& g : spec.extra_paths) {
        INFO(g.path);
        CHECK_FALSE(PluginAuditManager::grant_reaches_secret_store(g.path));
        granted.push_back(fs::path(g.path).lexically_normal().string());
    }
    auto has = [&](const fs::path& p) { return std::find(granted.begin(), granted.end(), p.lexically_normal().string()) != granted.end(); };
    CHECK(has(home.path() / "Documents"));
    CHECK(has(home.path() / "notes.txt"));
    CHECK(has(data_root / "user"));
    CHECK_FALSE(has(home.path()));
    CHECK_FALSE(has(home.path() / "AppData"));
    CHECK_FALSE(has(data_root));
    CHECK_FALSE(has(data_root / "hub"));
    CHECK_FALSE(has(data_root / "orca_plugins")); // holds trust.json

    // A plugin's own sandbox spec never grants trust.json any more, even read-only.
    PluginDescriptor descriptor;
    descriptor.plugin_key  = "H5_Spec";
    descriptor.plugin_root = (data_root / "orca_plugins" / "Foo").string();
    descriptor.entry_path  = (data_root / "orca_plugins" / "Foo" / "Foo.py").string();
    PluginPermissions perms;
    perms.fs_read      = {home.path().string() + "/*"};
    perms.fs_readwrite = {data_root.string() + "/*"};
    perms.full_access  = true;
    const plugin_ipc::SandboxSpec built = plugin_ipc::make_sandbox_spec(descriptor, perms, std::string());
    for (const auto& g : built.extra_paths) {
        INFO(g.path);
        CHECK_FALSE(PluginAuditManager::grant_reaches_secret_store(g.path));
    }
}
