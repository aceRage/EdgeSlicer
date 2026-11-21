// H2 regression tests for the findings of tests/pr_review_2026-11-18_plugin_stack.md that the
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

// ---- H2 -------------------------------------------------------------------------------------

TEST_CASE("H2: malformed, mistyped or oversized helper messages are refused without throwing", "[PluginLanding][H2][PluginIpc]")
{
    using nlohmann::json;
    const std::string long_message(plugin_ipc::k_max_message_bytes + 1, 'x');
    const std::vector<std::pair<const char*, json>> bad = {
        {"not an object", json::array({1, 2})},
        {"no type", json{{"id", 1}}},
        {"type is a number", json{{"type", 5}}},
        {"unknown type", json{{"type", "launch_missiles"}}},
        {"result id as string", json{{"type", "result"}, {"id", "7"}}},
        {"result id negative", json{{"type", "result"}, {"id", -1}}},
        {"result id float", json{{"type", "result"}, {"id", 1.5}}},
        {"result id bool", json{{"type", "result"}, {"id", true}}},
        {"result id null", json{{"type", "result"}, {"id", nullptr}}},
        {"result missing id", json{{"type", "result"}}},
        {"status as string", json{{"type", "result"}, {"id", 1}, {"status", "0"}}},
        {"status out of range", json{{"type", "result"}, {"id", 1}, {"status", 9}}},
        {"status negative", json{{"type", "result"}, {"id", 1}, {"status", -2}}},
        {"status huge", json{{"type", "result"}, {"id", 1}, {"status", 1e300}}},
        {"message as object", json{{"type", "result"}, {"id", 1}, {"message", json::object()}}},
        {"message too long", json{{"type", "result"}, {"id", 1}, {"message", long_message}}},
        {"cancelled as string", json{{"type", "result"}, {"id", 1}, {"cancelled", "yes"}}},
        {"gcode as number", json{{"type", "result"}, {"id", 1}, {"gcode", 12}}},
        {"error message as array", json{{"type", "error"}, {"message", json::array()}}},
        {"host_call args as array", json{{"type", "host_call"}, {"id", 1}, {"method", "host.model"}, {"args", json::array()}}},
        {"host_call method as number", json{{"type", "host_call"}, {"id", 1}, {"method", 3}}},
        {"host_call plugin_key as number", json{{"type", "host_call"}, {"id", 1}, {"method", "host.model"}, {"plugin_key", 3}}},
        {"audit targets as string", json{{"type", "audit_check"}, {"id", 1}, {"targets", "/etc"}}},
        {"audit targets with numbers", json{{"type", "audit_check"}, {"id", 1}, {"targets", json::array({1, 2})}}},
        {"audit targets too many", json{{"type", "audit_check"}, {"id", 1}, {"targets", json(std::vector<std::string>(100, "/x"))}}},
        {"audit is_write as string", json{{"type", "audit_check"}, {"id", 1}, {"is_write", "true"}, {"targets", json::array({"/x"})}}},
        {"load_ok id as string", json{{"type", "load_ok"}, {"id", "1"}}},
        {"hello_ok version as string", json{{"type", "hello_ok"}, {"protocol_version", "1"}}},
        {"hello_ok sandbox as string", json{{"type", "hello_ok"}, {"protocol_version", 1}, {"sandbox", "applied"}}},
        {"hello_ok applied as string", json{{"type", "hello_ok"}, {"protocol_version", 1}, {"sandbox", {{"applied", "true"}}}}},
        {"hello_ok abi as string", json{{"type", "hello_ok"}, {"protocol_version", 1}, {"sandbox", {{"landlock_abi", "6"}}}}},
    };
    for (const auto& [name, msg] : bad) {
        INFO(name);
        std::string error;
        bool        ok = true;
        CHECK_NOTHROW(ok = plugin_ipc::validate_child_message(msg, error));
        CHECK_FALSE(ok);
        CHECK_FALSE(error.empty());
    }

    const std::vector<json> good = {
        json{{"type", "result"}, {"id", 3}, {"status", 0}, {"message", "ok"}, {"data", ""}, {"cancelled", false}, {"gcode", "G1"}},
        json{{"type", "error"}, {"message", "boom"}},
        json{{"type", "host_call"}, {"id", 9}, {"method", "host.model"}, {"plugin_key", "A"}, {"args", json::object()}},
        json{{"type", "audit_check"}, {"id", 9}, {"event", "open"}, {"is_write", false}, {"targets", json::array({"/a"})}},
        json{{"type", "load_ok"}, {"id", 2}},
        json{{"type", "hello_ok"}, {"protocol_version", 1}, {"sandbox", {{"applied", true}, {"kind", "landlock"}, {"landlock_abi", 6}}}},
    };
    for (const json& msg : good) {
        INFO(msg.dump());
        std::string error;
        CHECK(plugin_ipc::validate_child_message(msg, error));
    }
}

TEST_CASE("H2: out-of-range numbers and deep nesting in a payload are refused, not thrown", "[PluginLanding][H2][PluginIpc]")
{
    auto decode = [](const std::string& text, std::string& error) {
        nlohmann::json out;
        bool           ok = true;
        CHECK_NOTHROW(ok = plugin_ipc::decode_payload(reinterpret_cast<const uint8_t*>(text.data()), uint32_t(text.size()), out, error));
        return ok;
    };
    std::string error;
    CHECK_FALSE(decode(R"({"type":"result","id":1,"status":1e400})", error));
    CHECK(error.find("garbage") != std::string::npos);

    std::string deep = R"({"type":"result","id":1,"data":)";
    for (int i = 0; i < 5000; ++i)
        deep += "[";
    for (int i = 0; i < 5000; ++i)
        deep += "]";
    deep += "}";
    error.clear();
    CHECK_FALSE(decode(deep, error));
    CHECK(error.find("deep") != std::string::npos);

    std::string shallow = R"({"type":"result","id":1,"x":)";
    for (int i = 0; i < 10; ++i)
        shallow += "[";
    for (int i = 0; i < 10; ++i)
        shallow += "]";
    shallow += "}";
    error.clear();
    CHECK(decode(shallow, error));

    // A sandbox report with wrong types is "not applied", never an exception.
    plugin_ipc::SandboxReport report;
    CHECK_NOTHROW(report = plugin_ipc::sandbox_report_from_json(nlohmann::json{{"applied", "yes"}, {"landlock_abi", "6"}}));
    CHECK_FALSE(report.applied);
}

TEST_CASE("H2: an exception on the plugin-host worker fails the run instead of ending the app", "[PluginLanding][H2][PluginIpc]")
{
    struct ResetHook
    {
        ~ResetHook() { PluginHostClient::set_job_hook_for_testing({}); }
    } reset;
    // The exception nlohmann throws for a wrongly typed field, as the old pump did.
    PluginHostClient::set_job_hook_for_testing([] { (void) nlohmann::json("not a number").get<int>(); });

    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;
    ScriptCompletion        completion;
    REQUIRE(PluginHostClient::instance().start_script("H2_NoSuchPlugin", "Cap", [&](const ScriptCompletion& c) {
        std::lock_guard<std::mutex> lock(mutex);
        completion = c;
        done       = true;
        cv.notify_all();
    }));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(30), [&] { return done; }));
    }
    CHECK(completion.error.find("plugin host error") != std::string::npos);
    CHECK_FALSE(PluginHostClient::instance().has_active_run("H2_NoSuchPlugin"));
}

