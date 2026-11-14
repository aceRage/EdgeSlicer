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

const char* const KILL_MARKER_SOURCE_PREFIX = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Ipc Kill"
# type = "script"
# ///
import time, os, orca
MARKER = r'''MARK'''
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

ScriptCompletion wait_for_script(PluginManager& manager, const std::string& plugin_key, const std::string& cap_name,
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
    nlohmann::json msg = {{"type", "hello"}, {"protocol_version", plugin_ipc::k_protocol_version}};
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
    CHECK_FALSE(plugin_ipc::decode_payload(reinterpret_cast<const uint8_t*>("xxxx"),
                                           plugin_ipc::k_max_payload_bytes + 1, decoded, error));
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

    std::string error;
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

    const ScriptCompletion host     = run_once(false);
    const ScriptCompletion in_proc  = run_once(true);
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
    const auto     pos    = source.find("MARK");
    REQUIRE(pos != std::string::npos);
    source.replace(pos, 4, marker.string());
    write_plugin(data_dir_guard, "Ipc_Kill", source);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Ipc_Kill", error));

    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;
    ScriptCompletion        first;
    auto launch = manager.launch_script_capability("Ipc_Kill", "Hold", [&](const ScriptCompletion& c) {
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
    plugin_ipc::HostCall unknown;
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
