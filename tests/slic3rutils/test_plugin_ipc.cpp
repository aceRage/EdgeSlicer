#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <catch2/catch.hpp>

#include "plugin_test_utils.hpp"

#include <slic3r/plugin/OrcaApiVersion.hpp>
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/plugin/ipc/PluginHostClient.hpp>
#include <slic3r/plugin/ipc/PluginHostProcess.hpp>
#include <slic3r/plugin/ipc/PluginHostRpc.hpp>
#include <slic3r/plugin/ipc/PluginIpcChannel.hpp>
#include <slic3r/plugin/ipc/PluginIpcProtocol.hpp>

#include <libslic3r/GCode/PostProcessor.hpp>
#include <libslic3r/PrintConfig.hpp>

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace Slic3r;
namespace fs = boost::filesystem;
using plugin_ipc::PluginHostClient;

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
# name = "Ipc Echo"
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
        return orca.ExecutionResult.success("ipc-ok")

@orca.plugin
class EchoPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(Echo)
)PY";

const char* const ABORT_PLUGIN_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Ipc Abort"
# type = "script"
# ///
import os, orca

class Boom(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Boom"
    def execute(self):
        os.abort()

@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(Boom)
)PY";

const char* const HANG_PLUGIN_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Ipc Hang"
# type = "script"
# ///
import time, orca

class Hang(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Hang"
    def execute(self):
        while True:
            time.sleep(0.2)

@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(Hang)
)PY";

const char* const SLEEP_OK_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Ipc Sleep Ok"
# type = "script"
# ///
import time, orca

class SleepOk(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "SleepOk"
    def execute(self):
        time.sleep(0.2)
        return orca.ExecutionResult.success("slept")

@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(SleepOk)
)PY";

const char* const KILL_MARKER_SOURCE_PREFIX = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Ipc Kill"
# type = "script"
# ///
import time, os, orca
MARKER = r'''__MARKER_PATH__'''
class Hold(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Hold"
    def execute(self):
        open(MARKER, "w").write("1")
        while True:
            time.sleep(0.05)
@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(Hold)
)PY";

const char* const STAMP_BLOCK = "; EDGE_PIPELINE_STAMP begin\n"
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
    return manager.wait_for_plugin_load(plugin_key, std::chrono::seconds(120), error);
}

ScriptCompletion wait_for_script(PluginManager&       manager,
                                 const std::string&   plugin_key,
                                 const std::string&   cap_name,
                                 std::chrono::seconds timeout = std::chrono::seconds(60))
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
    REQUIRE(cv.wait_for(lock, timeout, [&] { return done; }));
    return completion;
}

bool wait_for_file(const fs::path& path, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fs::exists(path))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return fs::exists(path);
}

std::string read_file(const fs::path& path)
{
    std::ifstream in(path.string(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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

} // namespace

TEST_CASE("IPC protocol encode/decode round-trips a hello object", "[PluginIpc][protocol]")
{
    nlohmann::json       msg = {{"type", "hello"}, {"protocol_version", plugin_ipc::k_protocol_version}};
    std::vector<uint8_t> frame;
    std::string          error;
    REQUIRE(plugin_ipc::encode_message(msg, frame, error));
    REQUIRE(frame.size() >= 4);
    const uint32_t size = plugin_ipc::read_le32(frame.data());
    CHECK(size == frame.size() - 4);
    nlohmann::json decoded;
    REQUIRE(plugin_ipc::decode_payload(frame.data() + 4, size, decoded, error));
    CHECK(decoded["type"] == "hello");
    CHECK(decoded["protocol_version"] == plugin_ipc::k_protocol_version);
}

TEST_CASE("IPC protocol rejects oversized, empty and garbage payloads", "[PluginIpc][protocol]")
{
    std::string    error;
    nlohmann::json decoded;
    CHECK_FALSE(plugin_ipc::decode_payload(nullptr, 0, decoded, error));
    CHECK(error.find("empty") != std::string::npos);

    error.clear();
    CHECK_FALSE(plugin_ipc::decode_payload(reinterpret_cast<const uint8_t*>("not-json"), 8, decoded, error));
    CHECK(error.find("garbage") != std::string::npos);

    error.clear();
    CHECK_FALSE(plugin_ipc::decode_payload(reinterpret_cast<const uint8_t*>("xxxx"), plugin_ipc::k_max_payload_bytes + 1, decoded, error));
    CHECK(error.find("oversized") != std::string::npos);

    error.clear();
    CHECK_FALSE(plugin_ipc::handshake_versions_compatible(99, error));
    CHECK(error.find("version") != std::string::npos);
    error.clear();
    CHECK(plugin_ipc::handshake_versions_compatible(plugin_ipc::k_protocol_version, error));
}

#ifndef _WIN32
TEST_CASE("IPC channel reports truncated and closed-pipe failures", "[PluginIpc][protocol]")
{
    int sv[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    auto a = plugin_ipc::Channel::from_posix_fd(sv[0]);
    auto b = plugin_ipc::Channel::from_posix_fd(sv[1]);

    std::string    error;
    nlohmann::json msg;
    CHECK_FALSE(a.read_json(msg, 50, error));
    CHECK(error.find("truncated") != std::string::npos);

    uint8_t header[4];
    plugin_ipc::write_le32(header, 8);
    REQUIRE(::write(sv[1], header, 4) == 4);
    REQUIRE(::write(sv[1], "abcd", 4) == 4);
    error.clear();
    CHECK_FALSE(a.read_json(msg, 50, error));
    CHECK(error.find("truncated") != std::string::npos);

    b.close();
    error.clear();
    nlohmann::json hello = {{"type", "hello"}};
    CHECK_FALSE(a.write_json(hello, 200, error));
    CHECK(error.find("closed") != std::string::npos);
}

TEST_CASE("IPC channel delivers the last frame when POLLHUP arrives with POLLIN", "[PluginIpc][protocol]")
{
    int sv[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    auto a = plugin_ipc::Channel::from_posix_fd(sv[0]);
    auto b = plugin_ipc::Channel::from_posix_fd(sv[1]);

    std::string    error;
    nlohmann::json hello = {{"type", "hello"}, {"protocol_version", 1}};
    REQUIRE(b.write_json(hello, 200, error));
    b.close();
    nlohmann::json got;
    REQUIRE(a.read_json(got, 200, error));
    CHECK(got["type"] == "hello");
}
#endif

TEST_CASE("The plugin host is not spawned when no plugin is enabled", "[PluginIpc][spawn]")
{
    PluginHostClient::reset_spawn_count_for_testing();
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    CHECK(PluginHostClient::spawn_count() == 0);

    ScopedDataDir data_dir_guard("plugin-ipc-no-spawn");
    write_plugin(data_dir_guard, "Ipc_Echo", SYNC_PLUGIN_SOURCE);
    PluginManager::instance().discover_plugins(/*async=*/false, /*clear=*/true);
    CHECK_FALSE(PluginManager::instance().is_plugin_loaded("Ipc_Echo"));
    CHECK(PluginHostClient::spawn_count() == 0);
}

TEST_CASE("A script execute through the host matches in-process", "[PluginIpc][Python][script]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    auto run_once = [](bool in_process) {
        PluginHostClient::set_force_in_process_for_testing(in_process);
        ScopedDataDir data_dir_guard(in_process ? "plugin-ipc-echo-inproc" : "plugin-ipc-echo-host");
        write_plugin(data_dir_guard, "Ipc_Echo", SYNC_PLUGIN_SOURCE);
        PluginManager& manager = PluginManager::instance();
        manager.discover_plugins(/*async=*/false, /*clear=*/true);
        std::string error;
        REQUIRE(load_and_wait(manager, "Ipc_Echo", error));
        const ScriptCompletion completion = wait_for_script(manager, "Ipc_Echo", "Echo");
        manager.unload_plugin("Ipc_Echo");
        PluginHostClient::set_force_in_process_for_testing(false);
        return completion;
    };

    const ScriptCompletion host    = run_once(false);
    const ScriptCompletion in_proc = run_once(true);
    CHECK(host.error.empty());
    CHECK(in_proc.error.empty());
    CHECK(host.result.message == in_proc.result.message);
    CHECK(host.result.message == "ipc-ok");
    CHECK(host.result.status == PluginResult::Success);
}

TEST_CASE("Killing the host mid-execute fails the run and a later run respawns", "[PluginIpc][Python][crash]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("plugin-ipc-kill");
    PluginAuditManager::instance().add_global_allowed_root(data_dir());
    const fs::path marker = data_dir_guard.plugins_dir() / "Ipc_Kill" / "running";
    std::string    source = KILL_MARKER_SOURCE_PREFIX;
    const auto     pos    = source.find("__MARKER_PATH__");
    REQUIRE(pos != std::string::npos);
    source.replace(pos, 15, marker.string());
    write_plugin(data_dir_guard, "Ipc_Kill", source);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Ipc_Kill", error));

    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;
    ScriptCompletion        first;
    auto                    launch = manager.launch_script_capability("Ipc_Kill", "Hold", [&](const ScriptCompletion& c) {
        std::lock_guard<std::mutex> lock(mutex);
        first = c;
        done  = true;
        cv.notify_all();
    });
    REQUIRE(launch.launched_async);
    REQUIRE(wait_for_file(marker, std::chrono::seconds(15)));
    REQUIRE(PluginHostClient::instance().child_pid_for_testing() > 0);
    REQUIRE(PluginHostClient::instance().kill_child_for_testing());
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(30), [&] { return done; }));
    }
    CHECK_FALSE(first.error.empty());

    write_plugin(data_dir_guard, "Ipc_Echo", SYNC_PLUGIN_SOURCE);
    manager.discover_plugins(/*async=*/false, /*clear=*/false);
    REQUIRE(load_and_wait(manager, "Ipc_Echo", error));
    const ScriptCompletion second = wait_for_script(manager, "Ipc_Echo", "Echo");
    CHECK(second.error.empty());
    CHECK(second.result.message == "ipc-ok");
}

TEST_CASE("os.abort in the child fails the run and Edge survives", "[PluginIpc][Python][crash]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("plugin-ipc-abort");
    write_plugin(data_dir_guard, "Ipc_Abort", ABORT_PLUGIN_SOURCE);
    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Ipc_Abort", error));
    const ScriptCompletion boom = wait_for_script(manager, "Ipc_Abort", "Boom");
    CHECK_FALSE(boom.error.empty());

    write_plugin(data_dir_guard, "Ipc_Echo", SYNC_PLUGIN_SOURCE);
    manager.discover_plugins(/*async=*/false, /*clear=*/false);
    REQUIRE(load_and_wait(manager, "Ipc_Echo", error));
    const ScriptCompletion second = wait_for_script(manager, "Ipc_Echo", "Echo");
    CHECK(second.error.empty());
    CHECK(second.result.message == "ipc-ok");
}

TEST_CASE("A hung child is killed by the execute timeout", "[PluginIpc][Python][timeout]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    PluginHostClient::set_execute_timeout_for_testing(2000);
    struct TimeoutGuard
    {
        ~TimeoutGuard() { PluginHostClient::set_execute_timeout_for_testing(plugin_ipc::k_default_timeout_ms); }
    } guard;

    ScopedDataDir data_dir_guard("plugin-ipc-hang");
    write_plugin(data_dir_guard, "Ipc_Hang", HANG_PLUGIN_SOURCE);
    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Ipc_Hang", error));
    const ScriptCompletion hung = wait_for_script(manager, "Ipc_Hang", "Hang", std::chrono::seconds(20));
    CHECK_FALSE(hung.error.empty());
    INFO("hang error: " << hung.error);
    CHECK(hung.error.find("timed out") != std::string::npos);

    write_plugin(data_dir_guard, "Ipc_Echo", SYNC_PLUGIN_SOURCE);
    manager.discover_plugins(/*async=*/false, /*clear=*/false);
    REQUIRE(load_and_wait(manager, "Ipc_Echo", error));
    const ScriptCompletion second = wait_for_script(manager, "Ipc_Echo", "Echo");
    CHECK(second.error.empty());
    CHECK(second.result.message == "ipc-ok");
}

TEST_CASE("Host RPC denies forged and unknown methods on the Edge side", "[PluginIpc][Python][rpc]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("plugin-ipc-rpc");
    write_plugin(data_dir_guard, "Ipc_Echo", SYNC_PLUGIN_SOURCE);
    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Ipc_Echo", error));

    plugin_ipc::HostCall forged;
    forged.plugin_key = "Ipc_Echo";
    forged.method     = "host.model";
    auto denied       = plugin_ipc::handle_host_call(forged);
    CHECK_FALSE(denied.ok);
    CHECK(denied.error.find("audit context") != std::string::npos);

    ScopedPluginAuditContext audit("Ipc_Echo", "Echo");
    plugin_ipc::HostCall     unknown;
    unknown.plugin_key = "Ipc_Echo";
    unknown.method     = "host.secret_key";
    denied             = plugin_ipc::handle_host_call(unknown);
    CHECK_FALSE(denied.ok);
    CHECK(denied.error.find("unknown") != std::string::npos);

    plugin_ipc::HostCall mismatch;
    mismatch.plugin_key = "SomeoneElse";
    mismatch.method     = "host.plugin.storage";
    denied              = plugin_ipc::handle_host_call(mismatch);
    CHECK_FALSE(denied.ok);

    plugin_ipc::HostCall storage;
    storage.plugin_key = "Ipc_Echo";
    storage.method     = "host.plugin.storage";
    const auto granted = plugin_ipc::handle_host_call(storage);
    REQUIRE(granted.ok);
    const std::string path = granted.value.at("path").get<std::string>();
    CHECK(path.find("plugin_data") != std::string::npos);
    CHECK_FALSE(PluginAuditManager::instance().is_ultranet_plugins_path(fs::path(path)));
}

TEST_CASE("psGCodePostProcess through IPC matches the in-process stamp bytes", "[PluginIpc][Python][post_process]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::string original = "G1 X10 Y10 E0.1\n; original trailer\n";

    auto stamp_once = [&](bool in_process) {
        PluginHostClient::set_force_in_process_for_testing(in_process);
        ScopedDataDir data_dir_guard(in_process ? "plugin-ipc-stamp-inproc" : "plugin-ipc-stamp-host");
        write_plugin(data_dir_guard, "PipelineStamp", STAMP_SOURCE);
        PluginManager& manager = PluginManager::instance();
        manager.discover_plugins(/*async=*/false, /*clear=*/true);
        std::string error;
        REQUIRE(load_and_wait(manager, "PipelineStamp", error));
        REQUIRE(manager.is_plugin_loaded("PipelineStamp"));

        ScopedTemporaryDir gcode_dir(in_process ? "ipc-stamp-inproc-gcode" : "ipc-stamp-host-gcode");
        const fs::path     gcode = gcode_dir.path() / "box.gcode";
        write_file(gcode, original);
        DynamicPrintConfig config = pipeline_config("Stamp", "PipelineStamp");
        std::string        path   = gcode.string();
        std::string        output = gcode.string();
        REQUIRE(run_post_process_scripts(path, false, "File", output, config));
        const std::string body = read_file(gcode);
        manager.unload_plugin("PipelineStamp");
        PluginHostClient::set_force_in_process_for_testing(false);
        return body;
    };

    const std::string host    = stamp_once(false);
    const std::string in_proc = stamp_once(true);
    CHECK(host == in_proc);
    CHECK(host == original + STAMP_BLOCK);
}

TEST_CASE("config_snapshot omits print-host secrets", "[PluginIpc][snapshot]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_key_value("print_host", new ConfigOptionString("http://secret.example"));
    cfg.set_key_value("printhost_apikey", new ConfigOptionString("api-key"));
    cfg.set_key_value("printhost_user", new ConfigOptionString("user"));
    cfg.set_key_value("printhost_password", new ConfigOptionString("password"));
    const nlohmann::json snap = PluginHostClient::config_snapshot(&cfg);
    CHECK_FALSE(snap.contains("print_host"));
    CHECK_FALSE(snap.contains("print_host_webui"));
    CHECK_FALSE(snap.contains("printhost_apikey"));
    CHECK_FALSE(snap.contains("printhost_user"));
    CHECK_FALSE(snap.contains("printhost_password"));
    CHECK_FALSE(snap.contains("printhost_cafile"));
    CHECK_FALSE(snap.contains("printhost_port"));
    CHECK_FALSE(snap.contains("compatible_printers"));
    CHECK_FALSE(snap.contains("compatible_prints"));
    CHECK_FALSE(snap.contains("filament_colour_type"));
}

TEST_CASE("post-process execute timeout scales with G-code size unless tests override it", "[PluginIpc][timeout]")
{
    CHECK(PluginHostClient::execute_timeout_ms(false, "") == plugin_ipc::k_default_timeout_ms);
    ScopedTemporaryDir dir("ipc-timeout-gcode");
    const fs::path     gcode = dir.path() / "big.gcode";
    {
        std::ofstream out(gcode.string(), std::ios::binary);
        std::string   chunk(1024 * 1024, 'G');
        out << chunk << chunk;
    }
    CHECK(PluginHostClient::execute_timeout_ms(true, gcode.string()) > plugin_ipc::k_default_timeout_ms);
    PluginHostClient::set_execute_timeout_for_testing(2000);
    CHECK(PluginHostClient::execute_timeout_ms(true, gcode.string()) == 2000);
    PluginHostClient::set_execute_timeout_for_testing(plugin_ipc::k_default_timeout_ms);
}

TEST_CASE("Two sequential script runs reuse the same host pid", "[PluginIpc][Python][spawn]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("plugin-ipc-seq-pid");
    write_plugin(data_dir_guard, "Ipc_Echo", SYNC_PLUGIN_SOURCE);
    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Ipc_Echo", error));

    const ScriptCompletion first = wait_for_script(manager, "Ipc_Echo", "Echo");
    CHECK(first.error.empty());
    const std::int64_t pid1 = PluginHostClient::instance().child_pid_for_testing();
    REQUIRE(pid1 > 0);

    const ScriptCompletion second = wait_for_script(manager, "Ipc_Echo", "Echo");
    CHECK(second.error.empty());
    const std::int64_t pid2 = PluginHostClient::instance().child_pid_for_testing();
    CHECK(pid2 == pid1);
}

#ifndef _WIN32
TEST_CASE("plugin host spawn plus handshake is not blocked on the exec-error pipe", "[PluginIpc][spawn]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedDataDir           data_dir_guard("plugin-ipc-spawn-fast");
    plugin_ipc::SpawnedHost host;
    std::string             error;
    const auto              t0 = std::chrono::steady_clock::now();
    REQUIRE(plugin_ipc::spawn_host(host, error));
    CHECK(error.empty());
    const auto spawn_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    // F_DUPFD used to drop CLOEXEC on the exec-error pipe, so poll(errp[0], 3000)
    // waited the full 3 s on every spawn. spawn_host itself must return promptly.
    CHECK(spawn_ms < 500);
#ifdef __linux__
    {
        // The exec-error pipe is moved to fd >= 11 and must not survive exec.
        // Inspect before hello so the helper is blocked in handshake read, not
        // yet in CPython (which opens its own pipes).
        const fs::path               fd_dir = fs::path("/proc") / std::to_string(host.pid) / "fd";
        boost::system::error_code    ec;
        fs::directory_iterator       it(fd_dir, ec);
        const fs::directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            const int n = std::atoi(it->path().filename().string().c_str());
            if (n < 11)
                continue;
            char          dest[128] = {};
            const ssize_t len       = ::readlink(it->path().c_str(), dest, sizeof(dest) - 1);
            if (len > 0 && std::strncmp(dest, "pipe:", 5) == 0)
                FAIL("helper still holds the exec-error pipe at fd " + std::to_string(n) + " -> " + dest);
        }
    }
#endif
    nlohmann::json hello = {
        {"type", plugin_ipc::k_type_hello},
        {"protocol_version", plugin_ipc::k_protocol_version},
        {"data_dir", data_dir()},
        {"resources_dir", resources_dir()},
    };
    REQUIRE(host.channel.write_json(hello, plugin_ipc::k_handshake_timeout_ms, error));
    nlohmann::json reply;
    REQUIRE(host.channel.read_json(reply, plugin_ipc::k_handshake_timeout_ms, error));
    CHECK(reply.value("type", std::string()) == plugin_ipc::k_type_hello_ok);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
#ifdef __SANITIZE_ADDRESS__
    CHECK(ms < 5000);
#else
    CHECK(ms < 1000);
#endif
    plugin_ipc::kill_host(host);
    plugin_ipc::close_host(host);
}
#endif

TEST_CASE("Cancel of a hung host run returns before the execute timeout", "[PluginIpc][Python][cancel]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    PluginHostClient::set_execute_timeout_for_testing(plugin_ipc::k_default_timeout_ms);
    ScopedDataDir data_dir_guard("plugin-ipc-cancel");
    PluginAuditManager::instance().add_global_allowed_root(data_dir());
    const fs::path marker = data_dir_guard.plugins_dir() / "Ipc_Hang" / "running";
    std::string    source = KILL_MARKER_SOURCE_PREFIX;
    const auto     pos    = source.find("__MARKER_PATH__");
    REQUIRE(pos != std::string::npos);
    source.replace(pos, 15, marker.string());
    write_plugin(data_dir_guard, "Ipc_Hang", source);
    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Ipc_Hang", error));

    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;
    ScriptCompletion        completion;
    auto                    launch = manager.launch_script_capability("Ipc_Hang", "Hold", [&](const ScriptCompletion& c) {
        std::lock_guard<std::mutex> lock(mutex);
        completion = c;
        done       = true;
        cv.notify_all();
    });
    REQUIRE(launch.launched_async);
    REQUIRE(wait_for_file(marker, std::chrono::seconds(15)));
    REQUIRE(PluginHostClient::instance().has_active_run("Ipc_Hang"));
    REQUIRE(PluginHostClient::instance().child_pid_for_testing() > 0);
    const auto start = std::chrono::steady_clock::now();
    PluginHostClient::instance().cancel("Ipc_Hang");
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(8), [&] { return done; }));
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(elapsed_ms < 5000);
    CHECK((completion.cancelled || !completion.error.empty()));
}

TEST_CASE("A cancel that races a successful run does not auto-cancel the next job", "[PluginIpc][Python][cancel]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("plugin-ipc-cancel-next");
    write_plugin(data_dir_guard, "Ipc_SleepOk", SLEEP_OK_SOURCE);
    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Ipc_SleepOk", error));

    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;
    ScriptCompletion        first;
    auto                    launch = manager.launch_script_capability("Ipc_SleepOk", "SleepOk", [&](const ScriptCompletion& c) {
        std::lock_guard<std::mutex> lock(mutex);
        first = c;
        done  = true;
        cv.notify_all();
    });
    REQUIRE(launch.launched_async);
    const auto start = std::chrono::steady_clock::now();
    while (!PluginHostClient::instance().has_active_run("Ipc_SleepOk")) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10))
            FAIL("SleepOk never became active");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    PluginHostClient::instance().cancel("Ipc_SleepOk");
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(15), [&] { return done; }));
    }

    const ScriptCompletion second = wait_for_script(manager, "Ipc_SleepOk", "SleepOk");
    CHECK(second.error.empty());
    CHECK_FALSE(second.cancelled);
    CHECK(second.result.message == "slept");
}

const char* const WRITE_PROBE_SOURCE_PREFIX = R"PY(# /// script
# requires-python = ">=3.12"
# requires-orca-api = "1.0.0"
#
# [tool.orcaslicer.plugin]
# name = "Ipc Write Probe"
# description = "Attempts a write used to assert post-process write-root isolation"
# author = "tests"
# version = "1.0"
# type = "script"
# ///
import os, time, orca

STARTED_PATH = "__STARTED_PATH__"
GO_PATH = "__GO_PATH__"
PROBE_PATH = "__PROBE_PATH__"

class Probe(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Probe"

    def execute(self):
        with open(STARTED_PATH, "w", encoding="utf-8") as f:
            f.write("1")
        for _ in range(400):
            if os.path.exists(GO_PATH):
                break
            time.sleep(0.05)
        try:
            with open(PROBE_PATH, "w", encoding="utf-8") as f:
                f.write("from-script")
            return orca.ExecutionResult.success("wrote")
        except Exception:
            return orca.ExecutionResult.success("denied")

@orca.plugin
class WriteProbePackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(Probe)
)PY";

const char* const WAIT_STAMP_SOURCE_PREFIX = R"PY(# /// script
# requires-python = ">=3.12"
# requires-orca-api = "1.0.0"
#
# [tool.orcaslicer.plugin]
# name = "Pipeline Stamp"
# description = "Waits for a GO flag, then appends a fixed comment block at psGCodePostProcess."
# author = "tests"
# version = "1.0.0"
# type = "slicing-pipeline"
# ///
import os, time, orca

STARTED_PATH = "__STARTED_PATH__"
GO_PATH = "__GO_PATH__"
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
        with open(STARTED_PATH, "w", encoding="utf-8") as f:
            f.write("1")
        for _ in range(400):
            if os.path.exists(GO_PATH):
                break
            time.sleep(0.05)
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

std::string replace_placeholder(std::string src, const std::string& placeholder, const std::string& value)
{
    const auto pos = src.find(placeholder);
    REQUIRE(pos != std::string::npos);
    src.replace(pos, placeholder.size(), value);
    return src;
}

TEST_CASE("Concurrent script and post-process each get their own result; script cannot use the post-process write root",
          "[PluginIpc][Python][concurrent]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("plugin-ipc-concurrent");
    PluginAuditManager::instance().add_global_allowed_root(data_dir());
    ScopedTemporaryDir gcode_dir("ipc-concurrent-gcode");
    const fs::path     gcode = gcode_dir.path() / "box.gcode";
    const fs::path     probe = gcode_dir.path() / "from_script.txt";
    // Coordination files live inside each plugin dir so Landlock write grants cover them.
    const fs::path script_started = data_dir_guard.plugins_dir() / "Ipc_Probe" / "script_started";
    const fs::path script_go      = data_dir_guard.plugins_dir() / "Ipc_Probe" / "script_go";
    const fs::path stamp_started  = data_dir_guard.plugins_dir() / "PipelineStamp" / "stamp_started";
    const fs::path stamp_go       = data_dir_guard.plugins_dir() / "PipelineStamp" / "stamp_go";
    write_file(gcode, "G1 X10 Y10 E0.1\n");

    std::string stamp_src = WAIT_STAMP_SOURCE_PREFIX;
    stamp_src             = replace_placeholder(stamp_src, "__STARTED_PATH__", stamp_started.string());
    stamp_src             = replace_placeholder(stamp_src, "__GO_PATH__", stamp_go.string());
    write_plugin(data_dir_guard, "PipelineStamp", stamp_src);

    std::string probe_src = WRITE_PROBE_SOURCE_PREFIX;
    probe_src             = replace_placeholder(probe_src, "__STARTED_PATH__", script_started.string());
    probe_src             = replace_placeholder(probe_src, "__GO_PATH__", script_go.string());
    probe_src             = replace_placeholder(probe_src, "__PROBE_PATH__", probe.string());
    write_plugin(data_dir_guard, "Ipc_Probe", probe_src);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "PipelineStamp", error));
    if (!load_and_wait(manager, "Ipc_Probe", error))
        FAIL("Ipc_Probe load failed: " + error);

    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;
    ScriptCompletion        script;
    auto                    launch = manager.launch_script_capability("Ipc_Probe", "Probe", [&](const ScriptCompletion& c) {
        std::lock_guard<std::mutex> lock(mutex);
        script = c;
        done   = true;
        cv.notify_all();
    });
    REQUIRE(launch.launched_async);
    REQUIRE(wait_for_file(script_started, std::chrono::seconds(15)));

    DynamicPrintConfig config  = pipeline_config("Stamp", "PipelineStamp");
    std::string        path    = gcode.string();
    std::string        output  = gcode.string();
    bool               post_ok = false;
    std::string        post_error;
    std::thread        post([&] {
        try {
            post_ok = run_post_process_scripts(path, false, "File", output, config);
        } catch (const std::exception& ex) {
            post_error = ex.what();
        }
    });

    const auto overlap_start = std::chrono::steady_clock::now();
    while (!(PluginHostClient::instance().has_active_run("Ipc_Probe") && PluginHostClient::instance().has_active_run("PipelineStamp"))) {
        if (std::chrono::steady_clock::now() - overlap_start > std::chrono::seconds(15)) {
            post.join();
            FAIL("both jobs never became active together");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    write_file(script_go, "1");
    write_file(stamp_go, "1");
    post.join();
    INFO("post_error=" << post_error);
    REQUIRE(post_error.empty());
    REQUIRE(post_ok);

    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(60), [&] { return done; }));
    }
    CHECK(script.error.empty());
    CHECK(script.result.message == "denied");
    CHECK_FALSE(fs::exists(probe));
    CHECK(read_file(gcode).find("EDGE_PIPELINE_STAMP") != std::string::npos);
}
