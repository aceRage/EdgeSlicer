// D3 (owner, 2026-11-20): plugin import runs in the sandboxed helper (edge-plugin-host), not in
// EdgeSlicer. These cases check the import-in-helper path, the strict parsing of what the helper
// reports (the helper is untrusted input), and that a hostile, broken or missing helper fails the
// load without crashing or hanging EdgeSlicer. Only fixture plugins written here run.

#include <catch2/catch.hpp>

#include "plugin_test_utils.hpp"

#include <slic3r/plugin/HostedPluginImport.hpp>
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PluginPreferences.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/ipc/PluginHostClient.hpp>
#include <slic3r/plugin/ipc/PluginHostProcess.hpp>
#include <slic3r/plugin/ipc/PluginHostRpc.hpp>
#include <slic3r/plugin/ipc/PluginIpcProtocol.hpp>
#include <slic3r/plugin/pluginTypes/script/ScriptPluginCapability.hpp>
#include <slic3r/plugin/pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp>

#include <libslic3r/GCode/PostProcessor.hpp>
#include <libslic3r/PrintConfig.hpp>

#include "slic3r/plugin/EdgePythonInclude.hpp"
#include <pybind11/embed.h>

#include <boost/filesystem.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace py = pybind11;
using json   = nlohmann::json;
using plugin_ipc::HostedCapabilityInfo;
using plugin_ipc::PluginHostClient;

namespace {

struct ScopedPluginManager
{
    ScopedDataDir python_data_dir{"plugin-import-python"};
    bool          initialized = false;

    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

// The test run imports fixtures in-process (slic3rutils_tests_main.cpp); these cases use the
// production placement, the helper.
struct HelperImport
{
    HelperImport() { plugin_import::set_in_process_import_for_testing(false); }
    ~HelperImport() { plugin_import::set_in_process_import_for_testing(true); }
};

struct ReplyFilter
{
    explicit ReplyFilter(std::function<void(json&)> filter) { PluginHostClient::set_reply_filter_for_testing(std::move(filter)); }
    ~ReplyFilter() { PluginHostClient::set_reply_filter_for_testing({}); }
};

const char* const D3_PLUGIN_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
# requires-orca-api = "1.0.0"
#
# [tool.orcaslicer.plugin]
# name = "D3 Hosted"
# description = "Import-in-helper fixture"
# author = "tests"
# version = "1.0"
# type = "script"
# ///
import builtins, os, orca

# Proves where the import ran: EdgeSlicer checks its own interpreter for this marker.
builtins.EDGE_D3_IMPORTED_PID = os.getpid()

STAMP = "; EDGE_D3_HOSTED_STAMP\n"

class Hello(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Hello"
    def execute(self):
        return orca.ExecutionResult.success("pid=%d" % os.getpid())

class Stamp(orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self):
        return "D3Stamp"
    def has_config_ui(self):
        return True
    def get_config_ui(self):
        return "<p>d3-config-ui</p>"
    def get_default_config(self):
        return {"depth": 2}
    def execute(self, ctx):
        if ctx.step != orca.slicing.Step.psGCodePostProcess:
            return orca.ExecutionResult.success()
        with open(ctx.gcode_path, "a", encoding="utf-8", newline="") as f:
            f.write(STAMP)
        return orca.ExecutionResult.success("stamped")

@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(Hello)
        orca.register_capability(Stamp)
)PY";

const char* const D3_PERMISSION_SOURCE_PREFIX = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "D3 Permission"
# type = "script"
# ///
import orca

class Hello(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Hello"
    def execute(self):
        return orca.ExecutionResult.success("ok")

@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.request_permissions(fs_read=[r'''__GRANT__'''])
        orca.register_capability(Hello)
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
    manager.load_plugin(plugin_key, /*skip_deps=*/true);
    return manager.wait_for_plugin_load(plugin_key, std::chrono::seconds(150), error);
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
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(90), [&] { return done; }));
    return completion;
}

bool parent_imported_fixture()
{
    PythonGILState gil;
    if (!gil)
        return false;
    return py::hasattr(py::module_::import("builtins"), "EDGE_D3_IMPORTED_PID");
}

json one_capability(const std::string& name, const std::string& type)
{
    return json{{"name", name}, {"type", type}};
}

json load_ok_with(json capabilities)
{
    return json{{"type", "load_ok"}, {"id", 7}, {"capabilities", std::move(capabilities)}};
}

} // namespace

// ---- Protocol: what EdgeSlicer accepts from the helper ---------------------------------------

TEST_CASE("D3: a well-formed capability list round-trips and parses", "[PluginImport][D3][protocol]")
{
    std::vector<HostedCapabilityInfo> sent(2);
    sent[0].name               = "Hello";
    sent[0].type               = PluginCapabilityType::Script;
    sent[1].name               = "Stamp";
    sent[1].type               = PluginCapabilityType::SlicingPipeline;
    sent[1].has_config_ui      = true;
    sent[1].config_ui          = "<p>ui</p>";
    sent[1].default_config     = json{{"depth", 2}};
    sent[1].lifecycle_events   = true;
    const json msg             = load_ok_with(plugin_ipc::hosted_capabilities_to_json(sent));

    std::string why;
    REQUIRE(plugin_ipc::validate_child_message(msg, why));
    std::vector<HostedCapabilityInfo> got;
    REQUIRE(plugin_ipc::parse_hosted_capabilities(msg, got, why));
    REQUIRE(got.size() == 2);
    CHECK(got[0].name == "Hello");
    CHECK(got[0].type == PluginCapabilityType::Script);
    CHECK_FALSE(got[0].has_config_ui);
    CHECK(got[1].type == PluginCapabilityType::SlicingPipeline);
    CHECK(got[1].config_ui == "<p>ui</p>");
    CHECK(got[1].default_config.value("depth", 0) == 2);
    CHECK(got[1].lifecycle_events);

    // An execute-time load_ok (no list) is still a valid message.
    CHECK(plugin_ipc::validate_child_message(json{{"type", "load_ok"}, {"id", 3}}, why));
}

TEST_CASE("D3: a malformed or hostile capability list is refused, never thrown", "[PluginImport][D3][protocol]")
{
    json too_many = json::array();
    for (std::size_t i = 0; i <= plugin_ipc::k_max_capabilities; ++i)
        too_many.push_back(one_capability("Cap" + std::to_string(i), "script"));
    json huge_ui                  = one_capability("Ui", "script");
    huge_ui["has_config_ui"]      = true;
    huge_ui["config_ui"]          = std::string(plugin_ipc::k_max_config_ui_bytes + 1, 'x');
    json long_name                = one_capability(std::string(plugin_ipc::k_max_capability_name_bytes + 1, 'n'), "script");
    json bad_defaults             = one_capability("D", "script");
    bad_defaults["default_config"] = json::array({1, 2});
    json bad_flag                 = one_capability("F", "script");
    bad_flag["has_config_ui"]     = "yes";
    json bad_ui_type              = one_capability("U", "script");
    bad_ui_type["config_ui"]      = 42;

    const std::vector<std::pair<const char*, json>> cases = {
        {"list is a string", load_ok_with("not-a-list")},
        {"list is an object", load_ok_with(json{{"name", "x"}})},
        {"empty list", load_ok_with(json::array())},
        {"too many", load_ok_with(too_many)},
        {"entry is a number", load_ok_with(json::array({5}))},
        {"name missing", load_ok_with(json::array({json{{"type", "script"}}}))},
        {"name is a number", load_ok_with(json::array({json{{"name", 5}, {"type", "script"}}}))},
        {"name empty", load_ok_with(json::array({one_capability("", "script")}))},
        {"name with ;", load_ok_with(json::array({one_capability("a;b", "script")}))},
        {"name with a control char", load_ok_with(json::array({one_capability(std::string("a\nb"), "script")}))},
        {"name too long", load_ok_with(json::array({long_name}))},
        {"type missing", load_ok_with(json::array({json{{"name", "x"}}}))},
        {"type unknown", load_ok_with(json::array({one_capability("x", "bogus")}))},
        {"type is a list", load_ok_with(json::array({json{{"name", "x"}, {"type", json::array({"script"})}}}))},
        {"duplicate", load_ok_with(json::array({one_capability("x", "script"), one_capability("x", "script")}))},
        {"config ui too large", load_ok_with(json::array({huge_ui}))},
        {"config ui not a string", load_ok_with(json::array({bad_ui_type}))},
        {"default config not an object", load_ok_with(json::array({bad_defaults}))},
        {"flag not a bool", load_ok_with(json::array({bad_flag}))},
    };
    for (const auto& [label, msg] : cases) {
        DYNAMIC_SECTION(label)
        {
            std::string                       why;
            std::vector<HostedCapabilityInfo> out;
            bool                              parsed = true;
            bool                              valid  = true;
            REQUIRE_NOTHROW(parsed = plugin_ipc::parse_hosted_capabilities(msg, out, why));
            CHECK_FALSE(parsed);
            CHECK(out.empty());
            CHECK_FALSE(why.empty());
            REQUIRE_NOTHROW(valid = plugin_ipc::validate_child_message(msg, why));
            CHECK_FALSE(valid);
        }
    }

    // load_ok with a non-integer id is refused before the list is looked at.
    std::string why;
    CHECK_FALSE(plugin_ipc::validate_child_message(json{{"type", "load_ok"}, {"id", "seven"}}, why));
    std::vector<HostedCapabilityInfo> out;
    CHECK_FALSE(plugin_ipc::parse_hosted_capabilities(json::array(), out, why));
}

TEST_CASE("D3: forwarded permission requests are parsed strictly", "[PluginImport][D3][protocol]")
{
    PluginPermissions perms;
    perms.fs_read      = {"/tmp/a/*"};
    perms.network_http = {"https://example.invalid"};
    perms.threading    = true;
    PluginPermissions back;
    std::string       why;
    REQUIRE(plugin_ipc::permissions_from_json(plugin_ipc::permissions_to_json(perms), back, why));
    CHECK(back.fs_read == perms.fs_read);
    CHECK(back.network_http == perms.network_http);
    CHECK(back.threading);
    CHECK_FALSE(back.full_access);

    json too_many = json::array();
    for (std::size_t i = 0; i <= plugin_ipc::k_max_permission_entries; ++i)
        too_many.push_back("/p" + std::to_string(i));
    const std::vector<std::pair<const char*, json>> bad = {
        {"not an object", json::array()},
        {"unknown bucket", json{{"secrets", json::array()}}},
        {"bucket is a string", json{{"fs_read", "/etc/*"}}},
        {"entry is a number", json{{"fs_read", json::array({1})}}},
        {"entry too long", json{{"fs_read", json::array({std::string(plugin_ipc::k_max_short_string_bytes + 1, 'p')})}}},
        {"too many entries", json{{"fs_read", too_many}}},
        {"full_access not a bool", json{{"full_access", "true"}}},
    };
    for (const auto& [label, value] : bad) {
        DYNAMIC_SECTION(label)
        {
            PluginPermissions out;
            bool              ok = true;
            REQUIRE_NOTHROW(ok = plugin_ipc::permissions_from_json(value, out, why));
            CHECK_FALSE(ok);
            CHECK(out.fs_read.empty());
            CHECK_FALSE(out.full_access);
        }
    }
}

// ---- The hosted registry entry -------------------------------------------------------------

TEST_CASE("D3: a hosted plugin is built from the helper's report as typed proxies", "[PluginImport][D3]")
{
    PluginDescriptor descriptor;
    descriptor.plugin_key     = "D3_Proxy";
    descriptor.name           = "D3 Proxy";
    descriptor.metadata_valid = true;

    std::vector<HostedCapabilityInfo> infos(3);
    infos[0].name           = "Hello";
    infos[0].type           = PluginCapabilityType::Script;
    infos[1].name           = "Stamp";
    infos[1].type           = PluginCapabilityType::SlicingPipeline;
    infos[1].has_config_ui  = true;
    infos[1].config_ui      = "<p>ui</p>";
    infos[1].default_config = json{{"depth", 2}};
    infos[2].name           = "Panel";
    infos[2].type           = PluginCapabilityType::Pages;

    Plugin      plugin;
    std::string error;
    REQUIRE(plugin_import::build_hosted_plugin(descriptor, infos, {"Hello"}, plugin, error));
    CHECK(plugin.hosted);
    CHECK(plugin.is_loaded());
    CHECK(plugin.module == nullptr);
    REQUIRE(plugin.capabilities.size() == 3);

    const auto& hello = plugin.capabilities[0];
    CHECK(std::dynamic_pointer_cast<ScriptPluginCapability>(hello) != nullptr);
    CHECK(plugin_import::as_hosted(hello.get()) != nullptr);
    CHECK(hello->name() == "Hello");
    CHECK(hello->audit_plugin_key() == "D3_Proxy");
    CHECK(hello->is_enabled());
    CHECK_FALSE(hello->config_ui_available());

    const auto& stamp = plugin.capabilities[1];
    CHECK(std::dynamic_pointer_cast<SlicingPipelinePluginCapability>(stamp) != nullptr);
    CHECK(stamp->type() == PluginCapabilityType::SlicingPipeline);
    CHECK_FALSE(stamp->is_enabled()); // an explicit request enables only what it names
    CHECK(stamp->config_ui_available());
    CHECK(stamp->get_config_ui() == "<p>ui</p>");
    CHECK(stamp->get_default_config().value("depth", 0) == 2);

    CHECK(plugin.capabilities[2]->type() == PluginCapabilityType::Pages);

    // A release-build script proxy never runs anything in EdgeSlicer.
    auto script = std::dynamic_pointer_cast<ScriptPluginCapability>(hello);
    CHECK(script->execute().status == PluginResult::FatalError);

    std::vector<HostedCapabilityInfo> dup(2);
    dup[0].name = dup[1].name = "Twice";
    dup[0].type = dup[1].type = PluginCapabilityType::Script;
    Plugin rejected;
    CHECK_FALSE(plugin_import::build_hosted_plugin(descriptor, dup, {}, rejected, error));
    CHECK_FALSE(rejected.hosted);
    CHECK(rejected.capabilities.empty());
    CHECK_FALSE(plugin_import::build_hosted_plugin(descriptor, {}, {}, rejected, error));
}

TEST_CASE("D3: a hosted slicing-pipeline plugin is skipped on the early steps without IPC", "[PluginImport][D3][D2]")
{
    HostedCapabilityInfo info;
    info.name = "Fuzzy";
    info.type = PluginCapabilityType::SlicingPipeline;
    auto cap  = plugin_import::make_hosted_capability(info);
    cap->set_resolved_identity(info.name, info.type);
    cap->set_audit_plugin_key("D3_Early");
    auto pipeline = std::dynamic_pointer_cast<SlicingPipelinePluginCapability>(cap);
    REQUIRE(pipeline);

    PluginHostClient::reset_spawn_count_for_testing();
    for (const SlicingPipelineStepPlugin step : {SlicingPipelineStepPlugin::posSlice, SlicingPipelineStepPlugin::posPerimeters,
                                                 SlicingPipelineStepPlugin::psSkirtBrim}) {
        SlicingPipelineContext ctx;
        ctx.step                  = step;
        const ExecutionResult res = pipeline->execute(ctx);
        CHECK(res.status == PluginResult::Skipped);
        CHECK(res.message.find("helper") != std::string::npos);
    }
    CHECK(PluginHostClient::spawn_count() == 0);
}

TEST_CASE("D3: unsigned plugins are imported in the helper, whatever the preferences say", "[PluginImport][D3][D2]")
{
    using plugin_import::ImportPlacement;
    using plugin_trust::TrustKind;
    plugin_import::ImportPolicyInput in;

    in.trust = TrustKind::ApprovedLocally;
    for (const bool pref : {false, true}) {
        in.in_process_steps_pref = pref;
        CHECK(plugin_import::decide_import_placement(in) == ImportPlacement::Helper);
        CHECK_FALSE(plugin_import::early_steps_allowed(in.trust, in.shipped_publisher, pref));
    }

    // The helper itself always imports what it was sent.
    in.in_helper_process = true;
    CHECK(plugin_import::decide_import_placement(in) == ImportPlacement::InProcess);
    in.in_helper_process = false;

    // Test seam only.
    in.in_process_for_testing = true;
    CHECK(plugin_import::decide_import_placement(in) == ImportPlacement::InProcess);
    in.in_process_for_testing = false;

    CHECK(std::string(plugin_import::placement_name(ImportPlacement::Helper)) == "helper");
}

// ---- End to end through the real helper ----------------------------------------------------

TEST_CASE("D3: a plugin is imported in the helper, never in EdgeSlicer, and runs from there", "[PluginImport][D3][PluginIpc][Python]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    HelperImport  helper_import;
    ScopedDataDir data_dir_guard("plugin-import-d3");
    write_plugin(data_dir_guard, "D3_Hosted", D3_PLUGIN_SOURCE);
    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    PluginHostClient::reset_spawn_count_for_testing();
    std::string error;
    REQUIRE(load_and_wait(manager, "D3_Hosted", error));
    CHECK(manager.is_plugin_loaded("D3_Hosted"));
    CHECK(PluginHostClient::spawn_count() >= 1);
    CHECK_FALSE(parent_imported_fixture());

    const auto caps = manager.get_plugin_capabilities("D3_Hosted", PluginCapabilityType::Unknown, false);
    REQUIRE(caps.size() == 2);
    for (const auto& cap : caps)
        CHECK(plugin_import::as_hosted(cap.get()) != nullptr);
    auto stamp = manager.get_plugin_capability({PluginCapabilityType::SlicingPipeline, "D3Stamp", "D3_Hosted"}, false);
    REQUIRE(stamp);
    CHECK(stamp->config_ui_available());
    CHECK(stamp->get_config_ui() == "<p>d3-config-ui</p>");
    CHECK(stamp->get_default_config().value("depth", 0) == 2);

    const ScriptCompletion hello = wait_for_script(manager, "D3_Hosted", "Hello");
    INFO(hello.error);
    CHECK(hello.error.empty());
    CHECK(hello.result.message.rfind("pid=", 0) == 0);
    CHECK(hello.result.message != "pid=" + std::to_string(get_current_pid()));

    ScopedTemporaryDir gcode_dir("d3-gcode");
    const fs::path     gcode = gcode_dir.path() / "box.gcode";
    {
        std::ofstream out(gcode.string(), std::ios::binary);
        out << "G1 X1\n";
    }
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"D3Stamp"}));
    config.set_key_value("plugins", new ConfigOptionStrings({std::string("D3_Hosted;;D3Stamp")}));
    std::string path   = gcode.string();
    std::string output = gcode.string();
    REQUIRE(run_post_process_scripts(path, false, "File", output, config));
    std::ifstream in(gcode.string(), std::ios::binary);
    const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(body == "G1 X1\n; EDGE_D3_HOSTED_STAMP\n");

    manager.unload_plugin("D3_Hosted");
    CHECK_FALSE(manager.is_plugin_loaded("D3_Hosted"));
    CHECK_FALSE(parent_imported_fixture());
}

TEST_CASE("D3: a load-time permission request is relayed to EdgeSlicer and answered there", "[PluginImport][D3][PluginIpc][Python]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    HelperImport  helper_import;
    ScopedDataDir data_dir_guard("plugin-import-d3-perm");
    const fs::path grant_dir = data_dir_guard.dir / "d3-grant";
    fs::create_directories(grant_dir);
    std::string source = D3_PERMISSION_SOURCE_PREFIX;
    const auto  pos    = source.find("__GRANT__");
    REQUIRE(pos != std::string::npos);
    source.replace(pos, 9, grant_dir.generic_string() + "/*");

    struct PromptReset
    {
        ~PromptReset() { PluginAuditManager::set_permission_message_prompt_for_testing({}); }
    } prompt_reset;
    std::atomic<int>  asked{0};
    std::atomic<bool> answer{false};
    PluginAuditManager::set_permission_message_prompt_for_testing([&](const std::string&) {
        ++asked;
        return answer.load();
    });

    PluginManager& manager = PluginManager::instance();

    write_plugin(data_dir_guard, "D3_Denied", source);
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    CHECK_FALSE(load_and_wait(manager, "D3_Denied", error));
    CHECK(asked.load() >= 1);
    CHECK_FALSE(manager.is_plugin_loaded("D3_Denied"));
    CHECK_FALSE(error.empty());

    asked  = 0;
    answer = true;
    write_plugin(data_dir_guard, "D3_Granted", source);
    manager.discover_plugins(/*async=*/false, /*clear=*/false);
    error.clear();
    REQUIRE(load_and_wait(manager, "D3_Granted", error));
    CHECK(asked.load() >= 1);
    PluginInstallState state;
    REQUIRE(manager.get_install_state("D3_Granted", state));
    CHECK_FALSE(state.permissions.fs_read.empty());
    CHECK_FALSE(parent_imported_fixture());

    // Outside an import EdgeSlicer refuses the same request.
    {
        ScopedPluginAuditContext audit("D3_Granted", "Hello");
        plugin_ipc::HostCall     call;
        call.plugin_key = "D3_Granted";
        call.method     = plugin_ipc::k_method_request_permissions;
        call.args       = json{{"fs_read", json::array({"/anything/*"})}};
        const auto r    = plugin_ipc::handle_host_call(call);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("loaded") != std::string::npos);
        call.during_load = true;
        call.args        = json{{"fs_read", "/not-a-list"}};
        const auto bad   = plugin_ipc::handle_host_call(call);
        CHECK_FALSE(bad.ok);
        CHECK(bad.error.find("bad permission request") != std::string::npos);
    }
    manager.unload_plugin("D3_Granted");
}

TEST_CASE("D3: a hostile helper reply fails the load and EdgeSlicer carries on", "[PluginImport][D3][PluginIpc][Python][slow]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    HelperImport  helper_import;
    ScopedDataDir data_dir_guard("plugin-import-d3-hostile");
    write_plugin(data_dir_guard, "D3_Hosted", D3_PLUGIN_SOURCE);
    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    json many = json::array();
    for (int i = 0; i < 1000; ++i)
        many.push_back(one_capability("C" + std::to_string(i), "script"));
    const std::vector<std::pair<const char*, std::function<void(json&)>>> hostile = {
        {"list is a string", [](json& m) { m["capabilities"] = "x"; }},
        {"list dropped", [](json& m) { m.erase("capabilities"); }},
        {"a thousand capabilities", [many](json& m) { m["capabilities"] = many; }},
        {"name is a number", [](json& m) { m["capabilities"][0]["name"] = 12; }},
        {"name with ;", [](json& m) { m["capabilities"][0]["name"] = "a;b"; }},
        {"unknown type", [](json& m) { m["capabilities"][0]["type"] = "printer-agent-x"; }},
        {"id is a string", [](json& m) { m["id"] = "seven"; }},
        {"id of another request", [](json& m) { m["id"] = 999999; }},
        {"wrong reply type", [](json& m) { m["type"] = "result"; }},
        {"huge config ui", [](json& m) {
             m["capabilities"][0]["has_config_ui"] = true;
             m["capabilities"][0]["config_ui"]     = std::string(plugin_ipc::k_max_config_ui_bytes + 1, 'u');
         }},
    };
    for (const auto& [label, mutate] : hostile) {
        DYNAMIC_SECTION(label)
        {
            const auto  filter_fn = mutate;
            ReplyFilter filter([filter_fn](json& msg) {
                if (msg.is_object() && msg.value("type", std::string()) == "load_ok" && msg.contains("capabilities"))
                    filter_fn(msg);
            });
            std::string error;
            const auto  started = std::chrono::steady_clock::now();
            CHECK_FALSE(load_and_wait(manager, "D3_Hosted", error));
            CHECK_FALSE(error.empty());
            CHECK_FALSE(manager.is_plugin_loaded("D3_Hosted"));
            CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(90));
        }
    }

    // The same plugin loads once the helper behaves.
    std::string error;
    REQUIRE(load_and_wait(manager, "D3_Hosted", error));
    CHECK(manager.is_plugin_loaded("D3_Hosted"));
    CHECK_FALSE(parent_imported_fixture());
    manager.unload_plugin("D3_Hosted");
}

TEST_CASE("D3: a missing or dying helper fails the load with a clear message", "[PluginImport][D3][PluginIpc][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    struct ResetOverrides
    {
        ~ResetOverrides() { plugin_ipc::set_allow_helper_env_overrides_for_testing(false); }
    } reset;
    plugin_ipc::set_allow_helper_env_overrides_for_testing(true);

    HelperImport  helper_import;
    ScopedDataDir data_dir_guard("plugin-import-d3-nohelper");
    write_plugin(data_dir_guard, "D3_Hosted", D3_PLUGIN_SOURCE);
    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    {
        ScopedTemporaryDir nowhere("plugin-import-no-helper");
        ScopedEnvVar       host_env("EDGE_PLUGIN_HOST", (nowhere.path() / "edge-plugin-host-missing").string());
        std::string        error;
        CHECK_FALSE(load_and_wait(manager, "D3_Hosted", error));
        INFO(error);
        CHECK(error.find("missing") != std::string::npos);
        CHECK_FALSE(manager.is_plugin_loaded("D3_Hosted"));
    }

    // fake-plugin-host exits at once (code 42): the spawn fails fast, no 15 s hang.
    const fs::path fake = plugin_ipc::helper_path().parent_path() /
#ifdef _WIN32
                          "fake-plugin-host.exe";
#else
                          "fake-plugin-host";
#endif
    if (fs::exists(fake)) {
        ScopedEnvVar host_env("EDGE_PLUGIN_HOST", fake.string());
        std::string  error;
        const auto   started = std::chrono::steady_clock::now();
        CHECK_FALSE(load_and_wait(manager, "D3_Hosted", error));
        CHECK_FALSE(error.empty());
        CHECK_FALSE(manager.is_plugin_loaded("D3_Hosted"));
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(60));
    }
    CHECK_FALSE(parent_imported_fixture());
}

// ---- D2: only a shipped publisher key unlocks the in-process mode --------------------------

namespace {

// RFC 8032 test vector 1, TEST ONLY (tests/data/plugins/trust/TEST_ONLY.md). Never shipped.
const char* const k_rfc_seed1 = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
const char* const k_rfc_pub1  = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";

PluginDescriptor d2_descriptor(const fs::path& package)
{
    PluginDescriptor d;
    d.plugin_key        = "d2_echo";
    d.name              = "D2 Echo";
    d.version           = "1.0.0";
    d.display_types     = {"script"};
    d.required_orca_api = "1.0.0";
    d.entry_path        = package.string();
    d.metadata_valid    = true;
    return d;
}

void sign_with_rfc_key(const fs::path& package, const PluginDescriptor& d)
{
    plugin_trust::Signature sig;
    std::string             error;
    sig.hash = plugin_trust::sha256_plugin_tree(package, error);
    REQUIRE_FALSE(sig.hash.empty());
    sig.metadata = plugin_trust::metadata_from_descriptor(d);
    REQUIRE(plugin_trust::public_key_from_seed(k_rfc_seed1, sig.public_key, error));
    REQUIRE(plugin_trust::sign(k_rfc_seed1, plugin_trust::canonical_payload(sig.hash, sig.metadata), sig.signature, error));
    sig.publisher = "D2 Test";
    REQUIRE(plugin_trust::write_signature_file(package, sig, error));
}

plugin_import::ImportPlacement placement_with_pref(const plugin_trust::TrustState& state, bool pref)
{
    plugin_import::ImportPolicyInput in;
    in.trust                 = state.kind;
    in.shipped_publisher     = state.shipped_publisher;
    in.in_process_steps_pref = pref;
    return plugin_import::decide_import_placement(in);
}

} // namespace

TEST_CASE("D2: a pinned or user-key plugin stays in the helper; a shipped key with the preference may run in-process",
          "[PluginImport][D2][PluginTrust]")
{
    using plugin_import::ImportPlacement;
    ScopedDataDir      data("plugin-import-d2");
    ScopedResourcesDir resources("plugin-import-d2-res");
    const fs::path     package = data.dir / "d2_echo.py";
    {
        std::ofstream out(package.string(), std::ios::binary);
        out << "import orca\n";
    }
    const PluginDescriptor d = d2_descriptor(package);
    std::string            error;

    SECTION("local pin only")
    {
        const std::string hash = plugin_trust::sha256_plugin_tree(package, error);
        REQUIRE(plugin_trust::pin_hash(d.plugin_key, hash, error));
        const plugin_trust::TrustState state = plugin_trust::evaluate(d);
        REQUIRE(state.kind == plugin_trust::TrustKind::ApprovedLocally);
        CHECK_FALSE(state.shipped_publisher);
        CHECK(placement_with_pref(state, false) == ImportPlacement::Helper);
        CHECK(placement_with_pref(state, true) == ImportPlacement::Helper);
        CHECK_FALSE(plugin_import::early_steps_allowed(state.kind, state.shipped_publisher, true));
    }

    SECTION("signed by a user-added publisher")
    {
        sign_with_rfc_key(package, d);
        REQUIRE(plugin_trust::add_user_publisher({"D2 Test", k_rfc_pub1, "user"}, error));
        const plugin_trust::TrustState state = plugin_trust::evaluate(d);
        REQUIRE(state.kind == plugin_trust::TrustKind::Signed);
        CHECK_FALSE(state.shipped_publisher);
        CHECK(placement_with_pref(state, true) == ImportPlacement::Helper);
        CHECK_FALSE(plugin_import::early_steps_allowed(state.kind, state.shipped_publisher, true));
    }

    SECTION("signed by a shipped publisher key")
    {
        sign_with_rfc_key(package, d);
        const fs::path keys = resources.dir / "python_plugins" / "trust_keys.json";
        fs::create_directories(keys.parent_path());
        {
            std::ofstream out(keys.string(), std::ios::binary);
            out << json{{"version", 1}, {"publishers", json::array({json{{"name", "D2 Shipped"}, {"public_key", k_rfc_pub1}}})}}.dump();
        }
        const plugin_trust::TrustState state = plugin_trust::evaluate(d);
        REQUIRE(state.kind == plugin_trust::TrustKind::Signed);
        CHECK(state.shipped_publisher);
        CHECK(placement_with_pref(state, false) == ImportPlacement::Helper);
        CHECK(placement_with_pref(state, true) == ImportPlacement::InProcess);
        CHECK(plugin_import::early_steps_allowed(state.kind, state.shipped_publisher, true));
    }
}

TEST_CASE("D2: the live placement keeps a pinned plugin in the helper with every preference set", "[PluginImport][D2]")
{
    HelperImport helper_import;
    struct PrefReset
    {
        ~PrefReset()
        {
            plugin_prefs::set_override_for_testing(plugin_prefs::k_in_process_steps, 1);
            plugin_prefs::set_override_for_testing(plugin_prefs::k_allow_unsigned_plugins, -1);
        }
    } reset;
    plugin_prefs::set_override_for_testing(plugin_prefs::k_in_process_steps, 1);
    plugin_prefs::set_override_for_testing(plugin_prefs::k_allow_unsigned_plugins, 1);

    plugin_trust::TrustState pinned;
    pinned.kind = plugin_trust::TrustKind::ApprovedLocally;
    CHECK(plugin_import::placement_for(pinned) == plugin_import::ImportPlacement::Helper);

    plugin_trust::TrustState user_signed;
    user_signed.kind = plugin_trust::TrustKind::Signed;
    CHECK(plugin_import::placement_for(user_signed) == plugin_import::ImportPlacement::Helper);

    plugin_trust::TrustState shipped = user_signed;
    shipped.shipped_publisher        = true;
    CHECK(plugin_import::placement_for(shipped) == plugin_import::ImportPlacement::InProcess);
}
