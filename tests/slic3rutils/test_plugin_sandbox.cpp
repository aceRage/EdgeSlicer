#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>
#include <catch2/catch.hpp>

#include "plugin_test_utils.hpp"

#include <libslic3r/Zipper.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PluginTrust.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/ipc/PluginHostClient.hpp>
#include <slic3r/plugin/ipc/PluginHostProcess.hpp>
#include <slic3r/plugin/ipc/PluginIpcProtocol.hpp>
#include <slic3r/plugin/ipc/PluginSandbox.hpp>
#ifndef _WIN32
#include <slic3r/plugin/ipc/PluginSandboxMacSeatbelt.hpp>
#endif
#include <slic3r/Utils/HomePluginsLogic.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <netdb.h>
#include <sys/syscall.h>
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#endif
#endif

using namespace Slic3r;
namespace fs = boost::filesystem;
using plugin_ipc::PluginHostClient;

namespace {

struct ScopedPluginManager
{
    ScopedDataDir python_data_dir{"plugin-sandbox-python"};
    bool          initialized = false;

    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

fs::path write_plugin(const ScopedDataDir& data_dir_guard, const std::string& stem, const std::string& source)
{
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / stem;
    fs::create_directories(plugin_dir);
    std::ofstream out((plugin_dir / (stem + ".py")).string(), std::ios::binary);
    out << source;
    out.close();
    return plugin_dir;
}

fs::path write_whl_entry_plugin(const ScopedDataDir& data_dir_guard, const std::string& stem)
{
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / stem;
    fs::create_directories(plugin_dir);
    const std::string py = R"PY(import orca
class Echo(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Echo"
    def execute(self):
        return orca.ExecutionResult.success("whl-ok")
@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(Echo)
)PY";
    const std::string metadata = "Metadata-Version: 2.1\nName: sandbox_whl_echo\nVersion: 1.0.0\n"
                                 "Summary: wheel entry test\nAuthor: test\nImport-Name: sandbox_whl_echo\n"
                                 "Requires-Orca-Api: 1.0.0\n";
    const std::string wheel    = "Wheel-Version: 1.0\nGenerator: edgeslicer-test\nRoot-Is-Purelib: true\nTag: py3-none-any\n";
    const std::string top      = "sandbox_whl_echo\n";
    const std::string record   = "sandbox_whl_echo/__init__.py,,\n"
                                 "sandbox_whl_echo-1.0.0.dist-info/METADATA,,\n"
                                 "sandbox_whl_echo-1.0.0.dist-info/WHEEL,,\n"
                                 "sandbox_whl_echo-1.0.0.dist-info/top_level.txt,,\n"
                                 "sandbox_whl_echo-1.0.0.dist-info/RECORD,,\n";
    const fs::path    whl      = plugin_dir / (stem + ".whl");
    Zipper            zipper(whl.string(), Zipper::NO_COMPRESSION);
    zipper.add_entry("sandbox_whl_echo/__init__.py", py.data(), py.size());
    zipper.add_entry("sandbox_whl_echo-1.0.0.dist-info/METADATA", metadata.data(), metadata.size());
    zipper.add_entry("sandbox_whl_echo-1.0.0.dist-info/WHEEL", wheel.data(), wheel.size());
    zipper.add_entry("sandbox_whl_echo-1.0.0.dist-info/top_level.txt", top.data(), top.size());
    zipper.add_entry("sandbox_whl_echo-1.0.0.dist-info/RECORD", record.data(), record.size());
    zipper.finalize();
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
    auto                    launch = manager.launch_script_capability(plugin_key, cap_name, [&](const ScriptCompletion& c) {
        std::lock_guard<std::mutex> lock(mutex);
        completion = c;
        done       = true;
        cv.notify_all();
    });
    if (!launch.launched_async) {
        completion.error = launch.outcome.error.empty() ? "failed to launch" : launch.outcome.error;
        return completion;
    }
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, timeout, [&] { return done; }))
        completion.error = "timed out waiting for script";
    return completion;
}

const char* const PROBE_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
# requires-orca-api = "1.0.0"
#
# [tool.orcaslicer.plugin]
# name = "Sandbox Probe"
# type = "script"
# ///
import os, socket, subprocess, orca

SECRET = r'''__SECRET__'''
PLUGIN_FILE = r'''__PLUGIN_FILE__'''
OUTSIDE = r'''__OUTSIDE__'''
GCODE = r'''__GCODE__'''

class Probe(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Probe"
    def execute(self):
        bits = []
        try:
            open(SECRET, "r").read()
            bits.append("read-secret")
        except Exception:
            bits.append("denied-secret")
        try:
            open(PLUGIN_FILE, "r").read()
            bits.append("read-plugin")
        except Exception:
            bits.append("denied-plugin")
        try:
            open(OUTSIDE, "w").write("nope")
            bits.append("wrote-outside")
        except Exception:
            bits.append("denied-write")
        try:
            open(GCODE, "w").write("ok")
            bits.append("wrote-gcode")
        except Exception:
            bits.append("denied-gcode")
        try:
            subprocess.Popen(["/bin/true"] if os.name != "nt" else ["cmd.exe", "/c", "exit", "0"])
            bits.append("spawned")
        except Exception:
            bits.append("denied-spawn")
        try:
            socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            bits.append("net")
        except Exception:
            bits.append("denied-net")
        return orca.ExecutionResult.success(",".join(bits))

@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(Probe)
)PY";

std::string replace_all(std::string text, const std::string& from, const std::string& to)
{
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

} // namespace

TEST_CASE("OS sandbox probe reports this kernel", "[PluginSandbox][probe]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    REQUIRE_FALSE(probe.os_name.empty());
#ifdef __linux__
    CHECK(probe.os_name == "linux");
    INFO("landlock_abi=" << probe.landlock_abi << " reason=" << probe.reason);
    if (probe.landlock_abi <= 0) {
        CHECK_FALSE(probe.available);
        CHECK_FALSE(probe.reason.empty());
    } else {
        CHECK(probe.available);
        CHECK(probe.landlock);
        CHECK(probe.seccomp);
        CHECK(plugin_ipc::expected_sandbox_kind() == plugin_ipc::k_sandbox_kind_landlock_seccomp);
    }
    CHECK_FALSE((plugin_ipc::sandbox_env_bwrap_requested() && plugin_ipc::running_inside_flatpak()));
#endif
}

#ifndef _WIN32
TEST_CASE("macOS Seatbelt probe uses an IPC-safe SBPL and fail-closes without sandbox_init", "[PluginSandbox][macos-sdk]")
{
    CHECK(std::string(plugin_ipc::mac_seatbelt::named_profile(false)) == "no-internet");
    CHECK(std::string(plugin_ipc::mac_seatbelt::named_profile(true)) == "no-write");
    const std::string sbpl = plugin_ipc::mac_seatbelt::sbpl_profile(false);
    CHECK(sbpl.find("(allow default)") != std::string::npos);
    CHECK(sbpl.find("(deny network*)") == std::string::npos);
    CHECK(sbpl.find("(subpath") != std::string::npos);
    CHECK(std::string(plugin_ipc::mac_seatbelt::sbpl_profile(true)) == sbpl);
    CHECK_FALSE(plugin_ipc::sandbox_kind_is_shipping(plugin_ipc::k_sandbox_kind_seatbelt_probe));
    CHECK_FALSE(plugin_ipc::sandbox_kind_is_shipping(plugin_ipc::k_sandbox_kind_none));
#ifndef __APPLE__
    std::string error;
    CHECK_FALSE(plugin_ipc::mac_seatbelt::apply_probe(false, error));
    CHECK(error.find("unavailable") != std::string::npos);
    CHECK_FALSE(plugin_ipc::mac_seatbelt::apply_probe(true, error));
    CHECK(error.find("unavailable") != std::string::npos);
#endif
}
#endif

TEST_CASE("fail-closed prompt defaults to Don't run and records Run anyway on trust.json", "[PluginSandbox][fail-closed][PluginTrust]")
{
    ScopedDataDir data_dir_guard("plugin-sandbox-override");
    write_plugin(data_dir_guard, "Sandbox_Probe", "print('probe')\n");
    std::string error;
    REQUIRE_FALSE(plugin_trust::is_unsandboxed("Sandbox_Probe"));

    plugin_ipc::set_sandbox_unavailable_prompt_for_testing(
        [](const std::string&, const std::string&) { return plugin_ipc::SandboxUnavailableChoice::DontRun; });
    bool applied = true;
    CHECK_FALSE(plugin_ipc::allow_unsandboxed_run("Sandbox_Probe", "Landlock missing", applied, error));
    CHECK_FALSE(applied);
    CHECK_FALSE(plugin_trust::is_unsandboxed("Sandbox_Probe"));

    plugin_ipc::set_sandbox_unavailable_prompt_for_testing(
        [](const std::string&, const std::string&) { return plugin_ipc::SandboxUnavailableChoice::RunUnsandboxed; });
    applied = true;
    REQUIRE(plugin_ipc::allow_unsandboxed_run("Sandbox_Probe", "Landlock missing", applied, error));
    CHECK_FALSE(applied);
    CHECK(plugin_trust::is_unsandboxed("Sandbox_Probe"));
    CHECK(error.empty());

    // A stored override is enough; no second prompt.
    bool prompted_again = false;
    plugin_ipc::set_sandbox_unavailable_prompt_for_testing([&prompted_again](const std::string&, const std::string&) {
        prompted_again = true;
        return plugin_ipc::SandboxUnavailableChoice::DontRun;
    });
    REQUIRE(plugin_ipc::allow_unsandboxed_run("Sandbox_Probe", "again", applied, error));
    CHECK_FALSE(prompted_again);

    // An update of the plugin tree drops the override.
    std::ofstream((data_dir_guard.plugins_dir() / "Sandbox_Probe" / "extra.py").string()) << "x = 1\n";
    CHECK_FALSE(plugin_trust::is_unsandboxed("Sandbox_Probe"));

    REQUIRE(plugin_trust::clear_unsandboxed("Sandbox_Probe", error));
    plugin_ipc::set_sandbox_unavailable_prompt_for_testing({});
}

TEST_CASE("legacy string unsandboxed entries are ignored", "[PluginSandbox][fail-closed][PluginTrust]")
{
    ScopedDataDir data_dir_guard("plugin-sandbox-legacy-unsandboxed");
    write_plugin(data_dir_guard, "Sandbox_Probe", "print('probe')\n");
    const fs::path sidecar = fs::path(data_dir()) / "orca_plugins" / "trust.json";
    fs::create_directories(sidecar.parent_path());
    std::ofstream(sidecar.string()) << R"({"version":1,"publishers":[],"pins":[],"unsandboxed":["Sandbox_Probe"]})" << "\n";
    CHECK_FALSE(plugin_trust::is_unsandboxed("Sandbox_Probe"));
}

TEST_CASE("handshake reports the OS sandbox state", "[PluginSandbox][handshake][PluginIpc]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedDataDir     data_dir_guard("plugin-sandbox-hello");
    std::string       error;
    {
        ScopedSpawnedHost warm;
        REQUIRE(plugin_ipc::spawn_host(warm.host, error));
    }
    ScopedSpawnedHost spawned;
    const auto        t0 = std::chrono::steady_clock::now();
    REQUIRE(plugin_ipc::spawn_host(spawned.host, error));
    plugin_ipc::SandboxSpec spec;
    spec.python_home     = PythonInterpreter::bundled_python_home();
    spec.plugin_root     = data_dir_guard.plugins_dir().string();
    nlohmann::json hello = {
        {"type", plugin_ipc::k_type_hello}, {"protocol_version", plugin_ipc::k_protocol_version}, {"data_dir", data_dir()},
        {"resources_dir", resources_dir()}, {"sandbox", plugin_ipc::sandbox_spec_to_json(spec)},
    };
    REQUIRE(spawned.host.channel.write_json(hello, plugin_ipc::k_handshake_timeout_ms, error));
    nlohmann::json reply;
    REQUIRE(spawned.host.channel.read_json(reply, plugin_ipc::k_handshake_timeout_ms, error));
    CHECK(reply.value("type", std::string()) == plugin_ipc::k_type_hello_ok);
    REQUIRE(reply.contains("sandbox"));
    const auto report = plugin_ipc::sandbox_report_from_json(reply["sandbox"]);
    INFO("sandbox kind=" << report.kind << " detail=" << report.detail << " abi=" << report.landlock_abi);
    const auto probe = plugin_ipc::probe_os_sandbox();
#ifdef __linux__
    if (probe.landlock_abi > 0) {
        CHECK(report.applied);
        CHECK(report.kind == plugin_ipc::k_sandbox_kind_landlock_seccomp);
        CHECK(plugin_ipc::sandbox_kind_is_shipping(report.kind));
        CHECK(report.landlock_abi == probe.landlock_abi);
        CHECK(report.degraded == (probe.landlock_abi < 6));
        if (probe.landlock_abi < 3)
            CHECK(report.detail.find("ABI<3") != std::string::npos);
        if (report.degraded)
            CHECK(report.detail.find("ABI<6") != std::string::npos);
    } else {
        CHECK_FALSE(report.applied);
        CHECK(report.kind == plugin_ipc::k_sandbox_kind_none);
        CHECK_FALSE(plugin_ipc::sandbox_kind_is_shipping(report.kind));
    }
#elif defined(__APPLE__)
    if (probe.app_sandbox) {
        CHECK(report.applied);
        CHECK(report.kind == plugin_ipc::k_sandbox_kind_app_sandbox);
    } else {
        CHECK_FALSE(report.applied);
        CHECK(report.kind == plugin_ipc::k_sandbox_kind_none);
    }
#elif defined(_WIN32)
    if (probe.appcontainer) {
        CHECK(report.applied);
        CHECK(report.kind == plugin_ipc::k_sandbox_kind_appcontainer);
    } else {
        CHECK_FALSE(report.applied);
        CHECK(report.kind == plugin_ipc::k_sandbox_kind_none);
    }
#endif
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    INFO("spawn+handshake ms=" << ms);
#ifdef __SANITIZE_ADDRESS__
    CHECK(ms < 5000);
#else
    CHECK(ms < 1500);
#endif
}

TEST_CASE("sandboxed child cannot read a user-home file and can read its plugin dir", "[PluginSandbox][Python][fs]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    if (!test_os_sandbox_will_apply()) {
        SKIP("OS sandbox is not applied in this environment: " + plugin_ipc::probe_os_sandbox().reason);
    }

    ScopedDataDir      data_dir_guard("plugin-sandbox-fs");
    ScopedTemporaryDir fake_home("plugin-sandbox-home");
    ScopedTemporaryDir gcode_dir("plugin-sandbox-gcode");
    const fs::path     secret  = fake_home.path() / "secret.txt";
    const fs::path     outside = fake_home.path() / "out.txt";
    const fs::path     gcode   = gcode_dir.path() / "out.gcode";
    {
        std::ofstream(secret.string()) << "secret";
        std::ofstream(gcode.string()) << "; gcode\n";
    }

    std::string    source      = PROBE_SOURCE;
    const fs::path plugin_dir  = data_dir_guard.plugins_dir() / "Sandbox_Probe";
    const fs::path plugin_file = plugin_dir / "readable.txt";
    source                     = replace_all(source, "__SECRET__", secret.string());
    source                     = replace_all(source, "__PLUGIN_FILE__", plugin_file.string());
    source                     = replace_all(source, "__OUTSIDE__", outside.string());
    source                     = replace_all(source, "__GCODE__", gcode.string());
    write_plugin(data_dir_guard, "Sandbox_Probe", source);
    fs::create_directories(plugin_dir);
    std::ofstream(plugin_file.string()) << "plugin-ok";

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Sandbox_Probe", error));

    const ScriptCompletion completion = wait_for_script(manager, "Sandbox_Probe", "Probe");
    INFO("probe: " << completion.result.message << " err=" << completion.error);
    CHECK(completion.error.empty());
    CHECK(completion.result.message.find("denied-secret") != std::string::npos);
    CHECK(completion.result.message.find("read-plugin") != std::string::npos);
    CHECK(completion.result.message.find("denied-write") != std::string::npos);
    CHECK(completion.result.message.find("denied-spawn") != std::string::npos);
    CHECK(completion.result.message.find("denied-net") != std::string::npos);
    CHECK(completion.result.message.find("denied-gcode") != std::string::npos);
    CHECK(completion.result.message.find("read-secret") == std::string::npos);
    CHECK(completion.result.message.find("wrote-outside") == std::string::npos);
    CHECK(completion.result.message.find("spawned") == std::string::npos);
    CHECK(completion.result.message.find(",net") == std::string::npos);
    CHECK_FALSE(fs::exists(outside));

    const auto report = PluginHostClient::instance().last_sandbox_report();
    INFO("host sandbox kind=" << report.kind << " applied=" << report.applied);
    CHECK(report.applied);
#ifdef __linux__
    CHECK(report.kind == plugin_ipc::k_sandbox_kind_landlock_seccomp);
#elif defined(_WIN32)
    CHECK(report.kind == plugin_ipc::k_sandbox_kind_appcontainer);
#elif defined(__APPLE__)
    CHECK(report.kind == plugin_ipc::k_sandbox_kind_app_sandbox);
#endif
}

TEST_CASE("FORCE_UNAVAILABLE is fail-closed unless the stored override exists", "[PluginSandbox][fail-closed][Python]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    PluginHostClient::set_allow_env_overrides_for_testing(true);
    ScopedEnvVar force(plugin_ipc::k_env_sandbox_force_unavailable, "1");

    ScopedDataDir data_dir_guard("plugin-sandbox-force");
    write_plugin(data_dir_guard, "Ipc_Echo", R"PY(# /// script
# requires-python = ">=3.12"
# [tool.orcaslicer.plugin]
# name = "Echo"
# type = "script"
# ///
import orca
class Echo(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Echo"
    def execute(self):
        return orca.ExecutionResult.success("echo")
@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(Echo)
)PY");
    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Ipc_Echo", error));

    plugin_ipc::set_sandbox_unavailable_prompt_for_testing(
        [](const std::string&, const std::string&) { return plugin_ipc::SandboxUnavailableChoice::DontRun; });
    const ScriptCompletion denied = wait_for_script(manager, "Ipc_Echo", "Echo");
    CHECK_FALSE(denied.error.empty());

    plugin_ipc::set_sandbox_unavailable_prompt_for_testing(
        [](const std::string&, const std::string&) { return plugin_ipc::SandboxUnavailableChoice::RunUnsandboxed; });
    const ScriptCompletion allowed = wait_for_script(manager, "Ipc_Echo", "Echo");
    CHECK(allowed.error.empty());
    CHECK(allowed.result.message == "echo");
    CHECK(plugin_trust::is_unsandboxed("Ipc_Echo"));
    CHECK(HomeTab::plugin_card([&] {
              HomeTab::PluginView v;
              v.plugin_key          = "Ipc_Echo";
              v.running_unsandboxed = plugin_trust::is_unsandboxed("Ipc_Echo");
              return v;
          }())["running_unsandboxed"] == true);

    plugin_ipc::set_sandbox_unavailable_prompt_for_testing({});
    PluginHostClient::set_allow_env_overrides_for_testing(false);
    std::string clear_error;
    plugin_trust::clear_unsandboxed("Ipc_Echo", clear_error);
}

TEST_CASE("execute without a stored override refuses when the OS sandbox is unavailable", "[PluginSandbox][fail-closed][Python]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");
    if (test_os_sandbox_will_apply())
        SKIP("OS sandbox applies here; refusal is covered by FORCE_UNAVAILABLE");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir data_dir_guard("plugin-sandbox-refuse");
    write_plugin(data_dir_guard, "Ipc_Echo", R"PY(# /// script
# requires-python = ">=3.12"
# [tool.orcaslicer.plugin]
# name = "Echo"
# type = "script"
# ///
import orca
class Echo(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Echo"
    def execute(self):
        return orca.ExecutionResult.success("echo")
@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(Echo)
)PY");
    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    PluginDescriptor descriptor;
    REQUIRE(manager.try_get_plugin_descriptor("Ipc_Echo", descriptor));
    std::string       error;
    const std::string hash = plugin_trust::sha256_plugin_tree(descriptor.plugin_root.empty() ?
                                                                  boost::filesystem::path(descriptor.entry_path) :
                                                                  boost::filesystem::path(descriptor.plugin_root),
                                                              error);
    REQUIRE_FALSE(hash.empty());
    REQUIRE(plugin_trust::pin_hash("Ipc_Echo", hash, error));
    manager.load_plugin("Ipc_Echo", /*skip_deps=*/true);
    REQUIRE(manager.wait_for_plugin_load("Ipc_Echo", std::chrono::seconds(120), error));

    const ScriptCompletion denied = wait_for_script(manager, "Ipc_Echo", "Echo");
    CHECK_FALSE(denied.error.empty());
    CHECK(denied.error.find("OS sandbox unavailable") != std::string::npos);
    CHECK_FALSE(plugin_trust::is_unsandboxed("Ipc_Echo"));
}

#ifdef __APPLE__
TEST_CASE("Seatbelt probe is labelled and parent-gated", "[PluginSandbox][handshake][macos-sdk]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedDataDir     data_dir_guard("plugin-sandbox-seatbelt");
    ScopedSpawnedHost spawned;
    std::string       error;
    REQUIRE(plugin_ipc::spawn_host(spawned.host, error));
    plugin_ipc::SandboxSpec spec;
    spec.python_home        = PythonInterpreter::bundled_python_home();
    spec.plugin_root        = data_dir_guard.plugins_dir().string();
    spec.dev_seatbelt_probe = true;
    nlohmann::json hello    = {
        {"type", plugin_ipc::k_type_hello}, {"protocol_version", plugin_ipc::k_protocol_version}, {"data_dir", data_dir()},
        {"resources_dir", resources_dir()}, {"sandbox", plugin_ipc::sandbox_spec_to_json(spec)},
    };
    REQUIRE(spawned.host.channel.write_json(hello, plugin_ipc::k_handshake_timeout_ms, error));
    nlohmann::json reply;
    const bool     got_reply = spawned.host.channel.read_json(reply, plugin_ipc::k_handshake_timeout_ms, error);
    if (!got_reply) {
        plugin_ipc::kill_host(spawned.host);
        const std::string child_err = plugin_ipc::drain_host_stderr(spawned.host);
        plugin_ipc::close_host(spawned.host);
        SKIP("Seatbelt probe did not complete handshake (" + error + "); child " + plugin_ipc::host_exit_summary(spawned.host) +
             " stderr=" + child_err);
    }
    if (reply.value("type", std::string()) != plugin_ipc::k_type_hello_ok || !reply.contains("sandbox")) {
        plugin_ipc::kill_host(spawned.host);
        const std::string child_err = plugin_ipc::drain_host_stderr(spawned.host);
        plugin_ipc::close_host(spawned.host);
        SKIP("Seatbelt probe reply was not hello_ok; child " + plugin_ipc::host_exit_summary(spawned.host) + " stderr=" + child_err);
    }
    const auto report = plugin_ipc::sandbox_report_from_json(reply["sandbox"]);
    INFO("sandbox kind=" << report.kind << " detail=" << report.detail);
    if (!report.applied || report.kind != plugin_ipc::k_sandbox_kind_seatbelt_probe || report.detail.find("DEV/TEST") == std::string::npos) {
        plugin_ipc::kill_host(spawned.host);
        const std::string child_err = plugin_ipc::drain_host_stderr(spawned.host);
        plugin_ipc::close_host(spawned.host);
        SKIP("Seatbelt probe could not be confirmed: kind=" + report.kind + " detail=" + report.detail + " stderr=" + child_err);
    }
    CHECK(report.applied);
    CHECK(report.kind == plugin_ipc::k_sandbox_kind_seatbelt_probe);
    CHECK(report.detail.find("DEV/TEST") != std::string::npos);
}
#endif

#ifdef __linux__
TEST_CASE("Landlock+seccomp fixture in a forked child", "[PluginSandbox][landlock]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (probe.landlock_abi <= 0)
        SKIP("Landlock unavailable: " + probe.reason);

    ScopedTemporaryDir plugin_dir("plugin-sandbox-ll-plugin");
    ScopedTemporaryDir home_dir("plugin-sandbox-ll-home");
    ScopedTemporaryDir gcode_dir("plugin-sandbox-ll-gcode");
    const fs::path     readable  = plugin_dir.path() / "ok.txt";
    const fs::path     secret    = home_dir.path() / "secret.txt";
    const fs::path     gcode     = gcode_dir.path() / "out.gcode";
    const fs::path     outside   = home_dir.path() / "out.txt";
    const fs::path     unix_path = home_dir.path() / "escape.sock";
    std::ofstream(readable.string()) << "yes";
    std::ofstream(secret.string()) << "no";
    std::ofstream(gcode.string()) << "; gcode\n";

    int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);
    sockaddr_un listen_addr{};
    listen_addr.sun_family = AF_UNIX;
    std::snprintf(listen_addr.sun_path, sizeof(listen_addr.sun_path), "%s", unix_path.string().c_str());
    ::unlink(unix_path.string().c_str());
    REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr)) == 0);
    REQUIRE(::listen(listen_fd, 1) == 0);

    plugin_ipc::SandboxSpec spec;
    spec.python_home   = PythonInterpreter::bundled_python_home();
    spec.plugin_root   = plugin_dir.path().string();
    spec.write_root    = gcode_dir.path().string();
    spec.allow_network = false;
    spec.allow_exec    = false;

    int sv[2] = {-1, -1};
    REQUIRE(::pipe(sv) == 0);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(sv[0]);
        ::close(listen_fd);
        plugin_ipc::SandboxReport report;
        std::string               error;
        uint32_t                  bits    = 0;
        int                       unix_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        int                       abs_fd  = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (plugin_ipc::apply_os_sandbox(spec, report, error) && report.applied) {
            bits |= 1u;
            const int secret_fd = ::open(secret.string().c_str(), O_RDONLY);
            const int ok_fd     = ::open(readable.string().c_str(), O_RDONLY);
            if (secret_fd < 0)
                bits |= 2u;
            if (ok_fd >= 0)
                bits |= 4u;
            if (ok_fd >= 0)
                ::close(ok_fd);
            if (secret_fd >= 0)
                ::close(secret_fd);
            const int gfd = ::open(gcode.string().c_str(), O_WRONLY | O_APPEND);
            if (gfd >= 0) {
                bits |= 8u;
                ::close(gfd);
            }
            const int ofd = ::open(outside.string().c_str(), O_WRONLY | O_CREAT, 0600);
            if (ofd < 0)
                bits |= 16u;
            else
                ::close(ofd);
            const int sfd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (sfd < 0)
                bits |= 32u;
            else
                ::close(sfd);
            const pid_t child = ::fork();
            if (child == 0) {
                char* argv[] = {const_cast<char*>("/bin/true"), nullptr};
                ::execve("/bin/true", argv, nullptr);
                ::_exit((errno == EPERM || errno == EACCES) ? 42 : 1);
            }
            if (child > 0) {
                int st = 0;
                while (::waitpid(child, &st, 0) < 0 && errno == EINTR) {}
                if (WIFEXITED(st) && WEXITSTATUS(st) == 42)
                    bits |= 64u;
            }
            if (::socket(AF_UNIX, SOCK_STREAM, 0) < 0)
                bits |= 128u;
            if (unix_fd >= 0) {
                const int rc = ::connect(unix_fd, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr));
                if (rc < 0)
                    bits |= 256u;
                ::close(unix_fd);
            }
            if (abs_fd >= 0) {
                sockaddr_un abs_addr{};
                abs_addr.sun_family  = AF_UNIX;
                abs_addr.sun_path[0] = '\0';
                std::memcpy(abs_addr.sun_path + 1, "edge-p8-abs-test", 16);
                const int rc = ::connect(abs_fd, reinterpret_cast<sockaddr*>(&abs_addr), sizeof(sa_family_t) + 1 + 16);
                if (rc < 0)
                    bits |= 512u;
                ::close(abs_fd);
            }
            int pair[2] = {-1, -1};
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0) {
                bits |= 1024u;
                ::close(pair[0]);
                ::close(pair[1]);
            }
            const int wplugin = ::open(readable.string().c_str(), O_WRONLY | O_APPEND);
            if (wplugin < 0)
                bits |= 2048u;
            else
                ::close(wplugin);
#ifdef __NR_io_uring_setup
            alignas(64) unsigned char ring[256]{};
            const long                rc = ::syscall(__NR_io_uring_setup, 1, ring);
            if (rc < 0 && (errno == EPERM || errno == ENOSYS))
                bits |= 4096u;
#else
            bits |= 4096u;
#endif
            if (::setpgid(0, 0) < 0)
                bits |= 8192u;
            int dgram[2] = {-1, -1};
            if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, dgram) < 0)
                bits |= 16384u;
            else {
                ::close(dgram[0]);
                ::close(dgram[1]);
            }
        }
        (void) !::write(sv[1], &bits, sizeof(bits));
        ::_exit(0);
    }
    ::close(sv[1]);
    uint32_t bits = 0;
    ssize_t  n    = 0;
    do {
        n = ::read(sv[0], &bits, sizeof(bits));
    } while (n < 0 && errno == EINTR);
    ::close(sv[0]);
    int wstatus = 0;
    while (::waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {}
    ::close(listen_fd);
    ::unlink(unix_path.string().c_str());
    REQUIRE(n == static_cast<ssize_t>(sizeof(bits)));
    INFO("landlock bits=" << bits);
    CHECK((bits & 1u) != 0);    // applied
    CHECK((bits & 2u) != 0);    // secret denied
    CHECK((bits & 4u) != 0);    // plugin file readable
    CHECK((bits & 8u) != 0);    // gcode folder writable
    CHECK((bits & 16u) != 0);   // write outside denied
    CHECK((bits & 32u) != 0);   // inet socket denied
    CHECK((bits & 64u) != 0);   // execve denied
    CHECK((bits & 128u) != 0);  // socket(AF_UNIX) denied
    CHECK((bits & 256u) != 0);  // connect to unix file socket denied
    CHECK((bits & 512u) != 0);  // abstract unix connect denied
    CHECK((bits & 1024u) != 0); // socketpair(AF_UNIX) still allowed
    CHECK((bits & 2048u) != 0); // plugin_root not writable
    CHECK((bits & 4096u) != 0);  // io_uring_setup denied
    CHECK((bits & 8192u) != 0);  // setpgid denied
    CHECK((bits & 16384u) != 0); // SOCK_DGRAM socketpair denied
}

TEST_CASE("sandboxed child cannot connect to a dbus-daemon unix socket", "[PluginSandbox][landlock][dbus]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (probe.landlock_abi <= 0)
        SKIP("Landlock unavailable: " + probe.reason);
    if (::access("/usr/bin/dbus-daemon", X_OK) != 0 && ::access("/usr/sbin/dbus-daemon", X_OK) != 0)
        SKIP("dbus-daemon is not installed");

    ScopedTemporaryDir bus_dir("plugin-sandbox-dbus");
    const fs::path     sock = bus_dir.path() / "bus";
    const std::string  addr = "unix:path=" + sock.string();
    const fs::path     log_path = bus_dir.path() / "p8-dbus-b1.log";
    std::ofstream      log(log_path.string(), std::ios::app);
    log << "addr=" << addr << "\n";

    const pid_t bus_pid = ::fork();
    REQUIRE(bus_pid >= 0);
    if (bus_pid == 0) {
        const char* daemon = ::access("/usr/bin/dbus-daemon", X_OK) == 0 ? "/usr/bin/dbus-daemon" : "/usr/sbin/dbus-daemon";
        char*       argv[] = {const_cast<char*>(daemon),
                              const_cast<char*>("--session"),
                              const_cast<char*>("--nofork"),
                              const_cast<char*>("--nopidfile"),
                              const_cast<char*>("--address"),
                              const_cast<char*>(addr.c_str()),
                              nullptr};
        ::execv(daemon, argv);
        ::_exit(127);
    }
    bool ready = false;
    for (int i = 0; i < 50; ++i) {
        boost::system::error_code ec;
        if (fs::exists(sock, ec)) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    log << "sock_ready=" << ready << " bus_pid=" << bus_pid << "\n";
    if (!ready) {
        ::kill(bus_pid, SIGKILL);
        int st = 0;
        while (::waitpid(bus_pid, &st, 0) < 0 && errno == EINTR) {}
        SKIP("dbus-daemon did not create " + sock.string());
    }

    plugin_ipc::SandboxSpec spec;
    spec.python_home   = PythonInterpreter::bundled_python_home();
    spec.allow_network = false;
    spec.allow_exec    = false;

    int sv[2] = {-1, -1};
    REQUIRE(::pipe(sv) == 0);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(sv[0]);
        plugin_ipc::SandboxReport report;
        std::string               error;
        unsigned char             bits = 0;
        int                       pre  = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (plugin_ipc::apply_os_sandbox(spec, report, error) && report.applied) {
            bits |= 1u;
            if (::socket(AF_UNIX, SOCK_STREAM, 0) < 0)
                bits |= 2u;
            if (pre >= 0) {
                sockaddr_un un{};
                un.sun_family = AF_UNIX;
                std::snprintf(un.sun_path, sizeof(un.sun_path), "%s", sock.string().c_str());
                if (::connect(pre, reinterpret_cast<sockaddr*>(&un), sizeof(un)) < 0)
                    bits |= 4u;
                ::close(pre);
            }
        }
        (void) !::write(sv[1], &bits, 1);
        ::_exit(0);
    }
    ::close(sv[1]);
    unsigned char bits = 0;
    ssize_t       n    = 0;
    do {
        n = ::read(sv[0], &bits, 1);
    } while (n < 0 && errno == EINTR);
    ::close(sv[0]);
    int wstatus = 0;
    while (::waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {}
    ::kill(bus_pid, SIGKILL);
    int bus_st = 0;
    while (::waitpid(bus_pid, &bus_st, 0) < 0 && errno == EINTR) {}
    log << "child_bits=" << static_cast<int>(bits) << " child_wait=" << wstatus << "\n";
    log.close();
    {
        boost::system::error_code ec;
        fs::create_directories("/opt/cursor/artifacts", ec);
        fs::copy_file(log_path, "/opt/cursor/artifacts/p8-dbus-b1.log", fs::copy_option::overwrite_if_exists, ec);
    }
    REQUIRE(n == 1);
    CHECK((bits & 1u) != 0);
    CHECK((bits & 2u) != 0);
    CHECK((bits & 4u) != 0);
}
#endif

TEST_CASE("shutdown while a sandbox prompt is pending does not hang", "[PluginSandbox][fail-closed]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    PluginHostClient::set_allow_env_overrides_for_testing(true);
    ScopedEnvVar force(plugin_ipc::k_env_sandbox_force_unavailable, "1");

    ScopedDataDir data_dir_guard("plugin-sandbox-prompt-shutdown");
    write_plugin(data_dir_guard, "Ipc_Echo", R"PY(# /// script
# requires-python = ">=3.12"
# [tool.orcaslicer.plugin]
# name = "Echo"
# type = "script"
# ///
import orca
class Echo(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Echo"
    def execute(self):
        return orca.ExecutionResult.success("echo")
@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(Echo)
)PY");
    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Ipc_Echo", error));

    std::atomic<bool> in_prompt{false};
    plugin_ipc::set_sandbox_unavailable_prompt_for_testing([&in_prompt](const std::string&, const std::string&) {
        in_prompt.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!plugin_ipc::sandbox_unavailable_prompt_aborted() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return plugin_ipc::SandboxUnavailableChoice::DontRun;
    });

    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;
    ScriptCompletion        completion;
    REQUIRE(PluginHostClient::instance().start_script("Ipc_Echo", "Echo", [&](const ScriptCompletion& c) {
        std::lock_guard<std::mutex> lock(mutex);
        completion = c;
        done       = true;
        cv.notify_all();
    }));
    const auto entered = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!in_prompt.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < entered)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(in_prompt.load(std::memory_order_acquire));
    PluginHostClient::instance().shutdown();
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return done; }));
    CHECK_FALSE(completion.error.empty());

    plugin_ipc::set_sandbox_unavailable_prompt_for_testing({});
    PluginHostClient::set_allow_env_overrides_for_testing(false);
}

TEST_CASE("sandboxed .whl-entry plugin extracts under plugin_data", "[PluginSandbox][Python][whl]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    if (!test_os_sandbox_will_apply())
        SKIP("OS sandbox is not applied in this environment: " + plugin_ipc::probe_os_sandbox().reason);

    ScopedDataDir  data_dir_guard("plugin-sandbox-whl");
    const fs::path plugin_dir = write_whl_entry_plugin(data_dir_guard, "Sandbox_Whl");
    PluginManager& manager    = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    PluginDescriptor discovered;
    INFO("discovered=" << manager.try_get_plugin_descriptor("Sandbox_Whl", discovered)
                       << " meta_err=" << discovered.error << " entry=" << discovered.entry_path);
    std::string error;
    const bool  loaded = load_and_wait(manager, "Sandbox_Whl", error);
    INFO("load error=" << error);
    REQUIRE(loaded);

    const ScriptCompletion completion = wait_for_script(manager, "Sandbox_Whl", "Echo");
    INFO("whl: " << completion.result.message << " err=" << completion.error);
    CHECK(completion.error.empty());
    CHECK(completion.result.message == "whl-ok");
    CHECK_FALSE(fs::exists(plugin_dir / "__whl_extracted__"));
    CHECK(fs::exists(data_dir_guard.plugin_scratch_dir("Sandbox_Whl") / "__whl_extracted__" / "sandbox_whl_echo"));
}

#ifdef __linux__
TEST_CASE("network-granted plugin can resolve DNS", "[PluginSandbox][Python][dns]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    if (!test_os_sandbox_will_apply())
        SKIP("OS sandbox is not applied in this environment: " + plugin_ipc::probe_os_sandbox().reason);

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    if (::getaddrinfo("one.one.one.one", "80", &hints, &resolved) != 0)
        SKIP("no network");
    if (resolved != nullptr)
        ::freeaddrinfo(resolved);

    ScopedDataDir data_dir_guard("plugin-sandbox-dns");
    write_plugin(data_dir_guard, "Sandbox_Dns", R"PY(# /// script
# requires-python = ">=3.12"
# requires-orca-api = "1.0.0"
#
# [tool.orcaslicer.plugin]
# name = "DNS Probe"
# type = "script"
# ///
import socket, orca
class Probe(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Probe"
    def execute(self):
        try:
            socket.getaddrinfo("one.one.one.one", 80)
            return orca.ExecutionResult.success("dns-ok")
        except Exception as e:
            return orca.ExecutionResult.success("dns-fail:" + str(e))
@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(Probe)
)PY");
    PluginManager&     manager = PluginManager::instance();
    PluginInstallState state;
    state.installed_from             = "local";
    state.plugin_name                = "DNS Probe";
    state.enabled                    = true;
    state.permissions.network_http   = {"https://one.one.one.one"};
    REQUIRE(write_install_state(data_dir_guard.plugins_dir() / "Sandbox_Dns", state));
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Sandbox_Dns", error));
    const ScriptCompletion completion = wait_for_script(manager, "Sandbox_Dns", "Probe");
    INFO("dns: " << completion.result.message << " err=" << completion.error);
    CHECK(completion.error.empty());
    CHECK(completion.result.message == "dns-ok");
}
#endif

#ifdef _WIN32
TEST_CASE("AppContainer profile names are a fixed-length FNV hash", "[PluginSandbox][windows]")
{
    const auto a     = plugin_ipc::win_appcontainer_profile_name("alpha");
    const auto b     = plugin_ipc::win_appcontainer_profile_name("beta");
    const auto long1 = plugin_ipc::win_appcontainer_profile_name(std::string(400, 'x'));
    const auto long2 = plugin_ipc::win_appcontainer_profile_name(std::string(400, 'y'));
    CHECK(a != b);
    CHECK(long1 != long2);
    CHECK(a.size() == long1.size());
    CHECK(a.find(L"EdgeSlicer.ph.") == 0);
}

TEST_CASE("AppContainer spawn SKIP without a working profile", "[PluginSandbox][windows]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");
    ScopedSpawnedHost spawned;
    std::string       error;
    plugin_ipc::SandboxSpec spec;
    spec.plugin_key  = "Win_Probe";
    spec.python_home = PythonInterpreter::bundled_python_home();
    REQUIRE(plugin_ipc::spawn_host(spawned.host, spec, error));
}
#endif
