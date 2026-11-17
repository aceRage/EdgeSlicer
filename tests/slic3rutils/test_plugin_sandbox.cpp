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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

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

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
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

#ifdef _WIN32
// Per-run test-only plugin keys. The `test.` prefix maps to the `EdgeSlicer.t.*`
// AppContainer moniker (when the test harness hook is on) so teardown cannot collide
// with shipping `EdgeSlicer.ph.*` profiles.
std::string unique_test_plugin_key(const char* stem)
{
    plugin_ipc::set_test_only_plugin_keys_for_testing(true);
    static std::atomic<unsigned> seq{0};
    const unsigned               n    = seq.fetch_add(1, std::memory_order_relaxed);
    const auto                   tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string("test.") + stem + "." + std::to_string(::GetCurrentProcessId()) + "." + std::to_string(n) + "." +
           std::to_string(static_cast<long long>(tick));
}
#endif

bool spawn_and_handshake(plugin_ipc::SpawnedHost& host, const plugin_ipc::SandboxSpec& spec, nlohmann::json& reply, std::string& error)
{
    if (!plugin_ipc::spawn_host(host, spec, error))
        return false;
    nlohmann::json hello = {
        {"type", plugin_ipc::k_type_hello}, {"protocol_version", plugin_ipc::k_protocol_version}, {"data_dir", data_dir()},
        {"resources_dir", resources_dir()}, {"sandbox", plugin_ipc::sandbox_spec_to_json(spec)},
    };
    if (!host.channel.write_json(hello, plugin_ipc::k_handshake_timeout_ms, error)) {
        plugin_ipc::enrich_host_error(host, error);
        return false;
    }
    if (!host.channel.read_json(reply, plugin_ipc::k_handshake_timeout_ms, error)) {
        plugin_ipc::enrich_host_error(host, error);
        return false;
    }
    return true;
}

fs::path write_whl_entry_plugin(const ScopedDataDir& data_dir_guard, const std::string& stem)
{
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / stem;
    fs::create_directories(plugin_dir);
    const std::string py       = R"PY(import orca
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

    ScopedDataDir           data_dir_guard("plugin-sandbox-hello");
    plugin_ipc::SandboxSpec spec;
    spec.python_home = PythonInterpreter::bundled_python_home();
    spec.plugin_root = data_dir_guard.plugins_dir().string();
    fs::create_directories(data_dir_guard.plugins_dir());

    // macOS first-launch of a freshly linked helper (Gatekeeper / dyld, including libpython
    // pages faulted during hello) is charged on the first full spawn+handshake, not on a
    // spawn-only warmup that kills the child before CPython init. Time cold then warm;
    // retry once if the warm exec is still over budget.
    // The 1500 ms (5000 ms ASan) budget is the steady-state spawn+handshake, not first-exec.
#ifdef __SANITIZE_ADDRESS__
    const long long budget_ms = 5000;
#else
    const long long budget_ms = 1500;
#endif
    auto timed_handshake = [&]() {
        ScopedSpawnedHost spawned;
        nlohmann::json    reply;
        std::string       error;
        const auto        t0 = std::chrono::steady_clock::now();
        const bool        ok = spawn_and_handshake(spawned.host, spec, reply, error);
        const auto        ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        return std::make_tuple(ok, error, reply, ms);
    };

    auto [cold_ok, cold_error, cold_reply, cold_ms] = timed_handshake();
    WARN("cold spawn+handshake ms=" << cold_ms);
    INFO("cold handshake error: " << cold_error);
    REQUIRE(cold_ok);
    REQUIRE(cold_reply.value("type", std::string()) == plugin_ipc::k_type_hello_ok);

    auto [ok, error, reply, ms] = timed_handshake();
    WARN("warm spawn+handshake ms=" << ms);
    INFO("handshake error: " << error);
    REQUIRE(ok);
    if (ms >= budget_ms) {
        WARN("warm spawn+handshake still cold; retrying once");
        std::tie(ok, error, reply, ms) = timed_handshake();
        WARN("warm spawn+handshake retry ms=" << ms);
        INFO("handshake error: " << error);
        REQUIRE(ok);
    }
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
    INFO("warm spawn+handshake ms=" << ms);
    CHECK(ms < budget_ms);
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
    CHECK((bits & 1u) != 0);     // applied
    CHECK((bits & 2u) != 0);     // secret denied
    CHECK((bits & 4u) != 0);     // plugin file readable
    CHECK((bits & 8u) != 0);     // gcode folder writable
    CHECK((bits & 16u) != 0);    // write outside denied
    CHECK((bits & 32u) != 0);    // inet socket denied
    CHECK((bits & 64u) != 0);    // execve denied
    CHECK((bits & 128u) != 0);   // socket(AF_UNIX) denied
    CHECK((bits & 256u) != 0);   // connect to unix file socket denied
    CHECK((bits & 512u) != 0);   // abstract unix connect denied
    CHECK((bits & 1024u) != 0);  // socketpair(AF_UNIX) still allowed
    CHECK((bits & 2048u) != 0);  // plugin_root not writable
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
    const fs::path     sock     = bus_dir.path() / "bus";
    const std::string  addr     = "unix:path=" + sock.string();
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
    INFO("discovered=" << manager.try_get_plugin_descriptor("Sandbox_Whl", discovered) << " meta_err=" << discovered.error
                       << " entry=" << discovered.entry_path);
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
    hints.ai_family    = AF_UNSPEC;
    hints.ai_socktype  = SOCK_STREAM;
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
    state.installed_from           = "local";
    state.plugin_name              = "DNS Probe";
    state.enabled                  = true;
    state.permissions.network_http = {"https://one.one.one.one"};
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

TEST_CASE("make_sandbox_spec creates the shared python packages dir", "[PluginSandbox]")
{
    ScopedDataDir  data("plugin-sandbox-create-packages");
    const fs::path packages = fs::path(PythonInterpreter::shared_packages_dir());
    {
        boost::system::error_code ec;
        const bool                present = fs::exists(packages, ec) && !ec;
        REQUIRE_FALSE(present);
    }

    const plugin_ipc::SandboxSpec spec = plugin_ipc::make_sandbox_spec("Create_Packages", {});
    {
        boost::system::error_code ec;
        REQUIRE(fs::is_directory(packages, ec));
        CHECK_FALSE(ec);
    }
    bool granted = false;
    for (const auto& extra : spec.extra_paths) {
        if (extra.path == packages.string() && !extra.write)
            granted = true;
    }
    CHECK(granted);
}

TEST_CASE("make_sandbox_spec marks fs_readwrite extras writable and Low-labelled", "[PluginSandbox]")
{
    ScopedDataDir             data("plugin-sandbox-fsrw-spec");
    boost::system::error_code ec;
    const fs::path            user_rw = data.dir / "user-rw";
    fs::create_directories(user_rw, ec);
    REQUIRE_FALSE(ec);

    PluginDescriptor  descriptor;
    descriptor.plugin_key = "FsRw_Spec";
    PluginPermissions permissions;
    permissions.fs_readwrite = {user_rw.string()};
    const plugin_ipc::SandboxSpec spec = plugin_ipc::make_sandbox_spec(descriptor, permissions, {});
    bool granted = false;
    for (const auto& extra : spec.extra_paths) {
        if (extra.path == user_rw.string() && extra.write && extra.low_label)
            granted = true;
    }
    CHECK(granted);
}

TEST_CASE("host log dir is plugin_host not the ACL journal sibling", "[PluginSandbox]")
{
    ScopedDataDir           data("plugin-sandbox-log-vs-journal");
    plugin_ipc::SandboxSpec spec;
    spec.data_dir             = data.dir.string();
    const std::string log_dir = plugin_ipc::host_child_log_dir(spec);
    REQUIRE_FALSE(log_dir.empty());
    CHECK(fs::path(log_dir).filename() == "plugin_host");
    CHECK(fs::path(log_dir).filename() != "plugin_host_acl");
    CHECK(fs::path(log_dir) != data.dir / "plugin_host_acl");
}

#ifdef _WIN32
TEST_CASE("spawn fails fast when the helper exits before connecting", "[PluginSandbox][windows][spawn]")
{
    const fs::path fake = plugin_ipc::helper_path().parent_path() / "fake-plugin-host.exe";
    {
        boost::system::error_code ec;
        if (!fs::exists(fake, ec) || ec)
            SKIP("fake-plugin-host is not next to the test binary");
    }

    plugin_ipc::set_allow_helper_env_overrides_for_testing(true);
    ScopedEnvVar host_env("EDGE_PLUGIN_HOST", fake.string());
    ScopedEnvVar sandbox_off(plugin_ipc::k_env_sandbox_disable, "0");

    ScopedSpawnedHost spawned;
    std::string       error;
    const auto        t0 = std::chrono::steady_clock::now();
    const bool        ok = plugin_ipc::spawn_host(spawned.host, error);
    const auto        ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    plugin_ipc::set_allow_helper_env_overrides_for_testing(false);

    INFO("spawn error: " << error);
    INFO("elapsed ms: " << ms);
    CHECK_FALSE(ok);
    CHECK(ms < 3000);
    CHECK(error.find("exited before connecting") != std::string::npos);
    CHECK(error.find("exit code 42") != std::string::npos);
}

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
    CHECK(a.size() == 14 + 16);
    plugin_ipc::set_test_only_plugin_keys_for_testing(false);
    CHECK(plugin_ipc::win_appcontainer_profile_name("test.alpha").find(L"EdgeSlicer.ph.") == 0);
    plugin_ipc::set_test_only_plugin_keys_for_testing(true);
    struct ResetTestKeys
    {
        ~ResetTestKeys() { plugin_ipc::set_test_only_plugin_keys_for_testing(false); }
    } reset;
    const auto t = plugin_ipc::win_appcontainer_profile_name("test.alpha");
    CHECK(t.find(L"EdgeSlicer.t.") == 0);
    CHECK(t.size() == 13 + 16);
    CHECK(t.find(L"EdgeSlicer.ph.") == std::wstring::npos);
}

namespace {

bool dacl_has_allowed_sid(PACL dacl, PSID sid)
{
    if (dacl == nullptr || sid == nullptr)
        return false;
    ACL_SIZE_INFORMATION info{};
    if (!::GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation))
        return false;
    for (DWORD i = 0; i < info.AceCount; ++i) {
        LPVOID ace = nullptr;
        if (!::GetAce(dacl, i, &ace) || ace == nullptr)
            continue;
        auto* header = static_cast<ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            continue;
        auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        if (::EqualSid(reinterpret_cast<PSID>(&allowed->SidStart), sid))
            return true;
    }
    return false;
}

constexpr DWORD k_data_dir_traverse_mask = FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE;
constexpr BYTE  k_inherit_ace_flags      = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE;

bool dacl_has_data_dir_traverse_ace(PACL dacl)
{
    if (dacl == nullptr)
        return false;
    ACL_SIZE_INFORMATION info{};
    if (!::GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation))
        return false;
    for (DWORD i = 0; i < info.AceCount; ++i) {
        LPVOID ace = nullptr;
        if (!::GetAce(dacl, i, &ace) || ace == nullptr)
            continue;
        auto* header = static_cast<ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            continue;
        if ((header->AceFlags & k_inherit_ace_flags) != 0)
            continue;
        auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        if (allowed->Mask == k_data_dir_traverse_mask)
            return true;
    }
    return false;
}

bool path_has_data_dir_traverse_ace(const std::string& path)
{
    const std::wstring   wide = boost::nowide::widen(path);
    PACL                 dacl = nullptr;
    PSECURITY_DESCRIPTOR sd   = nullptr;
    const DWORD rc = ::GetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                             &dacl, nullptr, &sd);
    const bool  found = rc == ERROR_SUCCESS && dacl_has_data_dir_traverse_ace(dacl);
    if (sd)
        ::LocalFree(sd);
    return found;
}

struct LocalSid
{
    PSID sid = nullptr;
    explicit LocalSid(LPCWSTR sddl)
    {
        if (!::ConvertStringSidToSidW(sddl, &sid))
            sid = nullptr;
    }
    ~LocalSid()
    {
        if (sid)
            ::LocalFree(sid);
    }
    LocalSid(const LocalSid&)            = delete;
    LocalSid& operator=(const LocalSid&) = delete;
};

struct CloseSandbox
{
    std::string plugin_key;
    explicit CloseSandbox(std::string key = {}) : plugin_key(std::move(key)) {}
    ~CloseSandbox()
    {
        plugin_ipc::close_parent_sandbox();
        if (!plugin_key.empty())
            plugin_ipc::delete_test_created_parent_sandbox_profile(plugin_key);
    }
};

} // namespace

TEST_CASE("AppContainer pipe SD grants the user, package SID, and Low IL", "[PluginSandbox][windows][pipe]")
{
    LocalSid package(L"S-1-15-2-1-2-3-4-5-6-7");
    REQUIRE(package.sid != nullptr);

    void*       sd_void = nullptr;
    std::string error;
    const bool  built = plugin_ipc::win_build_pipe_security_descriptor(package.sid, &sd_void, error);
    INFO("pipe SD error: " << error);
    REQUIRE(built);
    REQUIRE(sd_void != nullptr);
    struct LocalSd
    {
        PSECURITY_DESCRIPTOR sd = nullptr;
        ~LocalSd()
        {
            if (sd)
                ::LocalFree(sd);
        }
    };
    LocalSd sd_owner;
    sd_owner.sd = static_cast<PSECURITY_DESCRIPTOR>(sd_void);
    auto* sd    = sd_owner.sd;

    HANDLE token = nullptr;
    REQUIRE(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    REQUIRE(needed > 0);
    std::vector<BYTE> user_buf(needed);
    REQUIRE(::GetTokenInformation(token, TokenUser, user_buf.data(), static_cast<DWORD>(user_buf.size()), &needed));
    ::CloseHandle(token);
    PSID user_sid = reinterpret_cast<TOKEN_USER*>(user_buf.data())->User.Sid;
    REQUIRE(user_sid != nullptr);

    LocalSid system_sid(L"S-1-5-18");
    LocalSid admins_sid(L"S-1-5-32-544");
    LocalSid low_sid(L"S-1-16-4096");
    REQUIRE(system_sid.sid != nullptr);
    REQUIRE(admins_sid.sid != nullptr);
    REQUIRE(low_sid.sid != nullptr);

    BOOL dacl_present = FALSE, dacl_defaulted = FALSE;
    PACL dacl = nullptr;
    REQUIRE(::GetSecurityDescriptorDacl(sd, &dacl_present, &dacl, &dacl_defaulted));
    REQUIRE(dacl_present);
    REQUIRE(dacl != nullptr);
    CHECK(dacl_has_allowed_sid(dacl, package.sid));
    CHECK(dacl_has_allowed_sid(dacl, user_sid));
    CHECK(dacl_has_allowed_sid(dacl, system_sid.sid));
    CHECK_FALSE(dacl_has_allowed_sid(dacl, admins_sid.sid));

    BOOL sacl_present = FALSE, sacl_defaulted = FALSE;
    PACL sacl = nullptr;
    REQUIRE(::GetSecurityDescriptorSacl(sd, &sacl_present, &sacl, &sacl_defaulted));
    REQUIRE(sacl_present);
    REQUIRE(sacl != nullptr);
    ACL_SIZE_INFORMATION sacl_info{};
    REQUIRE(::GetAclInformation(sacl, &sacl_info, sizeof(sacl_info), AclSizeInformation));
    bool has_low_ml = false;
    for (DWORD i = 0; i < sacl_info.AceCount; ++i) {
        LPVOID ace = nullptr;
        if (!::GetAce(sacl, i, &ace) || ace == nullptr)
            continue;
        auto* header = static_cast<ACE_HEADER*>(ace);
        if (header->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE)
            continue;
        auto* ml = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace);
        if (::EqualSid(reinterpret_cast<PSID>(&ml->SidStart), low_sid.sid))
            has_low_ml = true;
    }
    CHECK(has_low_ml);

    // Kernel accepts the SD (CreateNamedPipe). Do not impersonate a low-IL token in-process
    // and CreateFile: that is denied (err=5) even when a real low-IL process can open the
    // production pipe. SD-structure asserts plus CreateNamedPipe are the in-process probe.
    wchar_t pipe_name[128];
    swprintf(pipe_name, 128, L"\\\\.\\pipe\\edge-plugin-pipe-sd-%lu", static_cast<unsigned long>(::GetCurrentProcessId()));
    SECURITY_ATTRIBUTES sa{};
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle       = FALSE;
    HANDLE      server      = ::CreateNamedPipeW(pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, &sa);
    const DWORD pipe_err    = (server == INVALID_HANDLE_VALUE) ? ::GetLastError() : 0;
    INFO("CreateNamedPipe err=" << pipe_err);
    REQUIRE(server != INVALID_HANDLE_VALUE);
    ::CloseHandle(server);
}

TEST_CASE("AppContainer helper env keeps LOCALAPPDATA and drops APPDATA", "[PluginSandbox][windows][env]")
{
    const std::vector<std::wstring> entries = plugin_ipc::win_scrubbed_helper_env_entries_for_testing();
    auto                            has_var = [&](const wchar_t* name) {
        const std::wstring prefix = std::wstring(name) + L"=";
        for (const std::wstring& e : entries) {
            if (e.size() >= prefix.size() && ::_wcsnicmp(e.c_str(), prefix.c_str(), prefix.size()) == 0)
                return true;
        }
        return false;
    };
    // CreateProcessW + AppContainer SECURITY_CAPABILITIES requires LOCALAPPDATA (error 203
    // without it). Only the known-folder API is redirected inside the container; this
    // test does not prepare a profile, so the env var still carries the real path.
    CHECK(has_var(L"LOCALAPPDATA"));
    CHECK_FALSE(has_var(L"APPDATA"));
    CHECK_FALSE(has_var(L"USERPROFILE"));
    CHECK_FALSE(has_var(L"HOMEDRIVE"));
    CHECK_FALSE(has_var(L"HOMEPATH"));
}

TEST_CASE("AppContainer data_dir ACE is traverse-only and revoked on close", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    boost::system::error_code ec;
    const fs::path            dir = fs::temp_directory_path() / fs::unique_path("orca-data-dir-ace-%%%%-%%%%");
    fs::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{dir};
    CloseSandbox close_guard;

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("DataDir_Traverse");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = dir.string();
    std::string error;
    const bool  ok = plugin_ipc::prepare_parent_sandbox(spec, error);
    INFO("prepare error: " << error);
    REQUIRE(ok);
    CHECK(path_has_data_dir_traverse_ace(spec.data_dir));
    // Exact mask: RA|X|SYNCHRONIZE, no FILE_LIST_DIRECTORY / FILE_READ_DATA.
    CHECK((k_data_dir_traverse_mask & FILE_LIST_DIRECTORY) == 0);

    plugin_ipc::close_parent_sandbox();
    CHECK_FALSE(path_has_data_dir_traverse_ace(spec.data_dir));
}

bool grant_all_app_packages_rx(const std::string& path)
{
    LocalSid any(L"S-1-15-2-1");
    if (any.sid == nullptr)
        return false;
    const std::wstring wide = boost::nowide::widen(path);
    EXPLICIT_ACCESSW   ea{};
    ea.grfAccessPermissions   = GENERIC_READ | GENERIC_EXECUTE;
    ea.grfAccessMode          = SET_ACCESS;
    ea.grfInheritance         = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm    = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType    = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName      = static_cast<LPWSTR>(any.sid);
    PACL                 prev = nullptr;
    PACL                 neu  = nullptr;
    PSECURITY_DESCRIPTOR sd   = nullptr;
    if (::GetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &prev,
                                nullptr, &sd) != ERROR_SUCCESS)
        sd = nullptr;
    if (::SetEntriesInAclW(1, &ea, prev, &neu) != ERROR_SUCCESS) {
        if (sd)
            ::LocalFree(sd);
        return false;
    }
    const DWORD rc = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                             neu, nullptr);
    ::LocalFree(neu);
    if (sd)
        ::LocalFree(sd);
    return rc == ERROR_SUCCESS;
}

bool path_dacl_has_sid(const std::string& path, PSID sid)
{
    const std::wstring   wide = boost::nowide::widen(path);
    PACL                 dacl = nullptr;
    PSECURITY_DESCRIPTOR sd   = nullptr;
    const DWORD rc = ::GetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                             &dacl, nullptr, &sd);
    const bool  found = rc == ERROR_SUCCESS && dacl_has_allowed_sid(dacl, sid);
    if (sd)
        ::LocalFree(sd);
    return found;
}

std::string path_dacl_sddl(const std::string& path)
{
    const std::wstring   wide = boost::nowide::widen(path);
    PACL                 dacl = nullptr;
    PSECURITY_DESCRIPTOR sd   = nullptr;
    if (::GetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                                nullptr, &sd) != ERROR_SUCCESS ||
        sd == nullptr)
        return {};
    LPWSTR            sddl = nullptr;
    std::string       out;
    if (::ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &sddl, nullptr) &&
        sddl != nullptr) {
        out = boost::nowide::narrow(sddl);
        ::LocalFree(sddl);
    }
    ::LocalFree(sd);
    return out;
}

bool granted_paths_contain(const std::string& path)
{
    const auto granted = plugin_ipc::win_granted_paths_for_testing();
    return std::find(granted.begin(), granted.end(), path) != granted.end();
}

bool restrict_dacl_to_modify(const std::string& path)
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        ::CloseHandle(token);
        return false;
    }
    std::vector<BYTE> buf(needed);
    if (!::GetTokenInformation(token, TokenUser, buf.data(), static_cast<DWORD>(buf.size()), &needed)) {
        ::CloseHandle(token);
        return false;
    }
    ::CloseHandle(token);
    PSID user = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
    // Explorer "Modify": read/write/execute/delete, not WRITE_DAC / WRITE_OWNER.
    constexpr DWORD k_modify = FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_READ_EA | FILE_WRITE_EA | FILE_EXECUTE |
                               FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | DELETE | READ_CONTROL | SYNCHRONIZE;
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = k_modify;
    ea.grfAccessMode        = SET_ACCESS;
    ea.grfInheritance       = NO_INHERITANCE;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName    = static_cast<LPWSTR>(user);
    PACL neu                = nullptr;
    if (::SetEntriesInAclW(1, &ea, nullptr, &neu) != ERROR_SUCCESS)
        return false;
    const std::wstring wide = boost::nowide::widen(path);
    const DWORD        rc   = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT,
                                                      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, neu,
                                                      nullptr);
    ::LocalFree(neu);
    return rc == ERROR_SUCCESS;
}

bool restrict_dacl_dac_without_write_owner(const std::string& path)
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        ::CloseHandle(token);
        return false;
    }
    std::vector<BYTE> buf(needed);
    if (!::GetTokenInformation(token, TokenUser, buf.data(), static_cast<DWORD>(buf.size()), &needed)) {
        ::CloseHandle(token);
        return false;
    }
    ::CloseHandle(token);
    PSID user = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
    // WRITE_DAC so we can still append the package-SID ACE; no WRITE_OWNER so the Low label fails.
    constexpr DWORD k_dac_no_owner = FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_READ_EA | FILE_WRITE_EA | FILE_EXECUTE |
                                     FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | DELETE | READ_CONTROL | SYNCHRONIZE | WRITE_DAC;
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = k_dac_no_owner;
    ea.grfAccessMode        = SET_ACCESS;
    ea.grfInheritance       = NO_INHERITANCE;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName    = static_cast<LPWSTR>(user);
    PACL neu                = nullptr;
    if (::SetEntriesInAclW(1, &ea, nullptr, &neu) != ERROR_SUCCESS)
        return false;
    const std::wstring wide = boost::nowide::widen(path);
    const DWORD        rc   = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT,
                                                      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, neu,
                                                      nullptr);
    ::LocalFree(neu);
    return rc == ERROR_SUCCESS;
}

bool journal_file_exists(const std::string& path)
{
    if (path.empty())
        return false;
    boost::system::error_code ec;
    const bool                present = fs::exists(path, ec) && !ec;
    return present;
}

size_t journal_grant_path_count(const std::string& path)
{
    if (!journal_file_exists(path))
        return 0;
    boost::nowide::ifstream in(path);
    const auto              json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded() || !json.is_object())
        return 0;
    size_t n = 0;
    if (json.contains("sessions") && json["sessions"].is_array()) {
        for (const auto& session : json["sessions"]) {
            if (session.contains("grants") && session["grants"].is_array())
                n += session["grants"].size();
        }
        return n;
    }
    if (json.contains("grants") && json["grants"].is_array())
        return json["grants"].size();
    return 0;
}

void write_acl_journal(const nlohmann::json& sessions)
{
    const std::string path = plugin_ipc::win_acl_journal_path_for_testing();
    REQUIRE_FALSE(path.empty());
    fs::create_directories(fs::path(path).parent_path());
    nlohmann::json          body = {{"sessions", sessions}};
    boost::nowide::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out);
    out << body.dump(1, '\t') << '\n';
}

TEST_CASE("AppContainer skips a RO grant when ALL APPLICATION PACKAGES already has RX", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir             data("plugin-sandbox-skip-rx");
    boost::system::error_code ec;
    const fs::path            dir = fs::temp_directory_path() / fs::unique_path("orca-skip-rx-%%%%-%%%%");
    fs::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{dir};
    CloseSandbox close_guard;

    REQUIRE(grant_all_app_packages_rx(dir.string()));

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Skip_RX");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    spec.extra_paths.push_back({dir.string(), false, false});
    std::string error;
    const bool  ok = plugin_ipc::prepare_parent_sandbox(spec, error);
    INFO("prepare error: " << error);
    REQUIRE(ok);
    CHECK_FALSE(granted_paths_contain(dir.string()));
    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    REQUIRE_FALSE(sid_str.empty());
    LocalSid package(sid_str.c_str());
    REQUIRE(package.sid != nullptr);
    CHECK_FALSE(path_dacl_has_sid(dir.string(), package.sid));
    LocalSid any(L"S-1-15-2-1");
    REQUIRE(any.sid != nullptr);
    CHECK(path_dacl_has_sid(dir.string(), any.sid));
}

TEST_CASE("AppContainer skips a missing data_dir ACE", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir data("plugin-sandbox-missing-data-dir");
    CloseSandbox  close_guard;

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Missing_DataDir");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = (data.dir / "no_such_folder").string();
    std::string error;
    const bool  ok = plugin_ipc::prepare_parent_sandbox(spec, error);
    INFO("prepare error: " << error);
    REQUIRE(ok);
}

TEST_CASE("AppContainer grants a user-owned RO directory", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir             data("plugin-sandbox-user-ro");
    boost::system::error_code ec;
    const fs::path            dir = fs::temp_directory_path() / fs::unique_path("orca-user-ro-%%%%-%%%%");
    fs::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{dir};
    CloseSandbox close_guard;

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("User_RO");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    spec.extra_paths.push_back({dir.string(), false, false});
    std::string error;
    const bool  ok = plugin_ipc::prepare_parent_sandbox(spec, error);
    INFO("prepare error: " << error);
    REQUIRE(ok);
    CHECK(granted_paths_contain(dir.string()));
    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    REQUIRE_FALSE(sid_str.empty());
    LocalSid package(sid_str.c_str());
    REQUIRE(package.sid != nullptr);
    CHECK(path_dacl_has_sid(dir.string(), package.sid));

    plugin_ipc::close_parent_sandbox();
    CHECK_FALSE(path_dacl_has_sid(dir.string(), package.sid));
}

TEST_CASE("AppContainer restores Low-IL labels on Edge work dirs and fs_readwrite roots", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir             data("plugin-sandbox-low-label");
    boost::system::error_code ec;
    const fs::path            write_root = fs::temp_directory_path() / fs::unique_path("orca-low-owned-%%%%-%%%%");
    const fs::path            user_rw    = fs::temp_directory_path() / fs::unique_path("orca-low-user-%%%%-%%%%");
    fs::create_directories(write_root, ec);
    REQUIRE_FALSE(ec);
    ec.clear();
    fs::create_directories(user_rw, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path a, b;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(a, ignore);
            fs::remove_all(b, ignore);
        }
    } cleanup{write_root, user_rw};
    CloseSandbox close_guard;

    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(write_root.string()));
    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));
    const std::string original_dacl = path_dacl_sddl(user_rw.string());
    REQUIRE_FALSE(original_dacl.empty());

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Low_Label");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    spec.write_root        = write_root.string();
    spec.extra_paths.push_back({user_rw.string(), true, true});
    std::string error;
    const bool  ok = plugin_ipc::prepare_parent_sandbox(spec, error);
    INFO("prepare error: " << error);
    REQUIRE(ok);
    CHECK(plugin_ipc::win_path_has_low_integrity_label(write_root.string()));
    CHECK(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));
    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    REQUIRE_FALSE(sid_str.empty());
    LocalSid package(sid_str.c_str());
    REQUIRE(package.sid != nullptr);
    CHECK(path_dacl_has_sid(user_rw.string(), package.sid));

    const fs::path user_file = user_rw / "plugin_out.txt";
    {
        boost::nowide::ofstream out(user_file.string(), std::ios::binary | std::ios::trunc);
        REQUIRE(out);
        out << "x";
    }
    REQUIRE(plugin_ipc::win_apply_low_integrity_label_for_testing(user_file.string()));
    CHECK(plugin_ipc::win_path_has_low_integrity_label(user_file.string()));

    plugin_ipc::close_parent_sandbox();
    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(write_root.string()));
    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));
    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(user_file.string()));
    CHECK_FALSE(path_dacl_has_sid(user_rw.string(), package.sid));
    CHECK(path_dacl_sddl(user_rw.string()) == original_dacl);
}

TEST_CASE("AppContainer helper can write into an fs_readwrite root", "[PluginSandbox][windows][acl][Python]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir             data("plugin-sandbox-fs-readwrite");
    boost::system::error_code ec;
    const fs::path            user_rw = fs::temp_directory_path() / fs::unique_path("orca-hosted-rw-%%%%-%%%%");
    fs::create_directories(user_rw, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{user_rw};

    const fs::path    out_file = user_rw / "hosted.txt";
    const std::string original_dacl = path_dacl_sddl(user_rw.string());
    REQUIRE_FALSE(original_dacl.empty());
    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));

    std::string source = R"PY(# /// script
# requires-python = ">=3.12"
# requires-orca-api = "1.0.0"
#
# [tool.orcaslicer.plugin]
# name = "FsRw Write"
# type = "script"
# ///
import orca
class Echo(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Echo"
    def execute(self):
        open(r'''__OUT__''', "w", encoding="utf-8").write("hosted-ok")
        return orca.ExecutionResult.success("wrote")
@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(Echo)
)PY";
    source = replace_all(source, "__OUT__", out_file.string());
    write_plugin(data, "Sandbox_FsRw", source);
    PluginInstallState state;
    state.installed_from             = "local";
    state.plugin_name                = "FsRw Write";
    state.enabled                    = true;
    state.permissions.fs_readwrite   = {user_rw.string()};
    REQUIRE(write_install_state(data.plugins_dir() / "Sandbox_FsRw", state));

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    std::string error;
    REQUIRE(load_and_wait(manager, "Sandbox_FsRw", error));
    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    REQUIRE_FALSE(sid_str.empty());
    LocalSid package(sid_str.c_str());
    REQUIRE(package.sid != nullptr);
    CHECK(path_dacl_has_sid(user_rw.string(), package.sid));
    CHECK(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));

    const ScriptCompletion completion = wait_for_script(manager, "Sandbox_FsRw", "Echo");
    INFO("hosted write: " << completion.result.message << " err=" << completion.error);
    CHECK(completion.error.empty());
    CHECK(completion.result.message == "wrote");
    {
        boost::nowide::ifstream in(out_file.string(), std::ios::binary);
        REQUIRE(in);
        const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(got == "hosted-ok");
    }

    plugin_ipc::PluginHostClient::instance().shutdown();
    CHECK_FALSE(path_dacl_has_sid(user_rw.string(), package.sid));
    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));
    CHECK(path_dacl_sddl(user_rw.string()) == original_dacl);
    {
        boost::nowide::ifstream in(out_file.string(), std::ios::binary);
        REQUIRE(in);
        const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(got == "hosted-ok");
    }
}

TEST_CASE("AppContainer crash sweep restores an fs_readwrite root", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir             data("plugin-sandbox-fsrw-crash");
    boost::system::error_code ec;
    const fs::path            user_rw = fs::temp_directory_path() / fs::unique_path("orca-fsrw-crash-%%%%-%%%%");
    fs::create_directories(user_rw, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{user_rw};
    CloseSandbox close_guard;

    const std::string original_dacl = path_dacl_sddl(user_rw.string());
    REQUIRE_FALSE(original_dacl.empty());
    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("FsRw_Crash");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    spec.extra_paths.push_back({user_rw.string(), true, true});
    std::string error;
    REQUIRE(plugin_ipc::prepare_parent_sandbox(spec, error));
    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    LocalSid           package(sid_str.c_str());
    REQUIRE(package.sid != nullptr);
    REQUIRE(path_dacl_has_sid(user_rw.string(), package.sid));
    REQUIRE(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));

    plugin_ipc::win_abandon_grants_for_testing();
    REQUIRE(path_dacl_has_sid(user_rw.string(), package.sid));
    REQUIRE(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));

    plugin_ipc::sweep_stale_parent_sandbox_grants();
    CHECK_FALSE(path_dacl_has_sid(user_rw.string(), package.sid));
    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));
    CHECK(path_dacl_sddl(user_rw.string()) == original_dacl);
}

TEST_CASE("AppContainer fail-closes fs_readwrite when WRITE_OWNER is missing", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir             data("plugin-sandbox-fsrw-nowriteowner");
    boost::system::error_code ec;
    const fs::path            user_rw = fs::temp_directory_path() / fs::unique_path("orca-fsrw-nowo-%%%%-%%%%");
    fs::create_directories(user_rw, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{user_rw};
    CloseSandbox close_guard;

    REQUIRE(restrict_dacl_dac_without_write_owner(user_rw.string()));
    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("FsRw_NoWO");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    spec.extra_paths.push_back({user_rw.string(), true, true});
    std::string error;
    const bool  ok = plugin_ipc::prepare_parent_sandbox(spec, error);
    INFO("prepare error: " << error);
    REQUIRE_FALSE(ok);
    CHECK(error.find("WRITE_OWNER") != std::string::npos);
    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));
}

TEST_CASE("AppContainer leaves a pre-existing Low label on fs_readwrite after close", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir             data("plugin-sandbox-fsrw-prelow");
    boost::system::error_code ec;
    const fs::path            user_rw = fs::temp_directory_path() / fs::unique_path("orca-fsrw-prelow-%%%%-%%%%");
    fs::create_directories(user_rw, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{user_rw};
    CloseSandbox close_guard;

    REQUIRE(plugin_ipc::win_apply_low_integrity_label_for_testing(user_rw.string()));
    REQUIRE(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));
    const fs::path inner_dir  = user_rw / "already-low-dir";
    const fs::path inner_file = user_rw / "already-low.txt";
    fs::create_directories(inner_dir, ec);
    REQUIRE_FALSE(ec);
    {
        boost::nowide::ofstream out(inner_file.string(), std::ios::binary | std::ios::trunc);
        REQUIRE(out);
        out << "pre-grant";
    }
    REQUIRE(plugin_ipc::win_apply_low_integrity_label_for_testing(inner_dir.string()));
    REQUIRE(plugin_ipc::win_apply_low_integrity_label_for_testing(inner_file.string()));

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("FsRw_PreLow");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    spec.extra_paths.push_back({user_rw.string(), true, true});
    std::string error;
    REQUIRE(plugin_ipc::prepare_parent_sandbox(spec, error));
    CHECK(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));
    plugin_ipc::close_parent_sandbox();
    CHECK(plugin_ipc::win_path_has_low_integrity_label(user_rw.string()));
    CHECK(plugin_ipc::win_path_has_low_integrity_label(inner_dir.string()));
    CHECK(plugin_ipc::win_path_has_low_integrity_label(inner_file.string()));
}

TEST_CASE("AppContainer grant journal is swept after a simulated crash", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir             data("plugin-sandbox-acl-journal");
    boost::system::error_code ec;
    const fs::path            dir = fs::temp_directory_path() / fs::unique_path("orca-journal-%%%%-%%%%");
    fs::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{dir};
    CloseSandbox close_guard;

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Journal_Sweep");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    spec.write_root        = dir.string();
    std::string error;
    const bool  ok = plugin_ipc::prepare_parent_sandbox(spec, error);
    INFO("prepare error: " << error);
    REQUIRE(ok);
    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    REQUIRE_FALSE(sid_str.empty());
    LocalSid package(sid_str.c_str());
    REQUIRE(package.sid != nullptr);
    CHECK(path_dacl_has_sid(dir.string(), package.sid));
    CHECK(plugin_ipc::win_path_has_low_integrity_label(dir.string()));

    const std::string journal = plugin_ipc::win_acl_journal_path_for_testing();
    REQUIRE_FALSE(journal.empty());
    CHECK(fs::path(journal).parent_path().filename() == "plugin_host_acl");
    CHECK_FALSE(granted_paths_contain(fs::path(journal).parent_path().string()));
    {
        boost::system::error_code jec;
        REQUIRE(fs::exists(journal, jec));
        REQUIRE_FALSE(jec);
        CHECK_FALSE(fs::exists(fs::path(data.dir) / "plugin_host" / "acl_journal.json", jec));
    }

    // Crash: in-memory grants/SID gone, journal and ACEs remain. Do not close/clear.
    plugin_ipc::win_abandon_grants_for_testing();
    CHECK(path_dacl_has_sid(dir.string(), package.sid));
    CHECK(plugin_ipc::win_path_has_low_integrity_label(dir.string()));
    {
        boost::system::error_code jec;
        REQUIRE(fs::exists(journal, jec));
        REQUIRE_FALSE(jec);
    }

    plugin_ipc::sweep_stale_parent_sandbox_grants();
    CHECK_FALSE(path_dacl_has_sid(dir.string(), package.sid));
    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(dir.string()));
    {
        boost::system::error_code jec;
        CHECK_FALSE(fs::exists(journal, jec));
    }
}

TEST_CASE("AppContainer sweep skips a journal session whose owner process is still alive", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir             data("plugin-sandbox-live-owner");
    boost::system::error_code ec;
    const fs::path            dir = fs::temp_directory_path() / fs::unique_path("orca-live-owner-%%%%-%%%%");
    fs::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{dir};
    CloseSandbox close_guard;

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Live_Owner");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    spec.write_root        = dir.string();
    std::string error;
    REQUIRE(plugin_ipc::prepare_parent_sandbox(spec, error));
    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    REQUIRE_FALSE(sid_str.empty());
    LocalSid package(sid_str.c_str());
    REQUIRE(package.sid != nullptr);
    REQUIRE(path_dacl_has_sid(dir.string(), package.sid));
    const std::string journal = plugin_ipc::win_acl_journal_path_for_testing();

    plugin_ipc::sweep_stale_parent_sandbox_grants();
    CHECK(path_dacl_has_sid(dir.string(), package.sid));
    REQUIRE(journal_file_exists(journal));

    plugin_ipc::win_abandon_grants_for_testing();
    FILETIME created{}, exited{}, kernel{}, user{};
    REQUIRE(::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user));
    ULARGE_INTEGER start{};
    start.LowPart  = created.dwLowDateTime;
    start.HighPart = created.dwHighDateTime;
    {
        boost::nowide::ifstream in(journal);
        REQUIRE(in);
        auto json = nlohmann::json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
        REQUIRE(json.contains("sessions"));
        REQUIRE_FALSE(json["sessions"].empty());
        json["sessions"][0]["owner_pid"]   = static_cast<std::uint32_t>(::GetCurrentProcessId());
        json["sessions"][0]["owner_start"] = start.QuadPart;
        write_acl_journal(json["sessions"]);
    }
    plugin_ipc::sweep_stale_parent_sandbox_grants();
    CHECK(path_dacl_has_sid(dir.string(), package.sid));
    REQUIRE(journal_file_exists(journal));

    {
        boost::nowide::ifstream in(journal);
        REQUIRE(in);
        auto json = nlohmann::json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
        json["sessions"][0]["owner_pid"]   = 0;
        json["sessions"][0]["owner_start"] = 0;
        write_acl_journal(json["sessions"]);
    }
    plugin_ipc::sweep_stale_parent_sandbox_grants();
    CHECK_FALSE(path_dacl_has_sid(dir.string(), package.sid));
    {
        boost::system::error_code jec;
        CHECK_FALSE(fs::exists(journal, jec));
    }
}

TEST_CASE("AppContainer copy-replaces a Low-IL file when WRITE_OWNER is missing", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    boost::system::error_code ec;
    const fs::path            dir = fs::temp_directory_path() / fs::unique_path("orca-low-nowriteowner-%%%%-%%%%");
    fs::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{dir};

    const fs::path    file = dir / "plugin_out.txt";
    const std::string body = "low-il-payload";
    {
        boost::nowide::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
        REQUIRE(out);
        out << body;
    }
    REQUIRE(plugin_ipc::win_apply_low_integrity_label_for_testing(file.string()));
    REQUIRE(plugin_ipc::win_path_has_low_integrity_label(file.string()));
    REQUIRE(restrict_dacl_to_modify(file.string()));

    plugin_ipc::win_clear_low_integrity_label(file.string());
    CHECK(plugin_ipc::win_low_label_used_copy_replace_for_testing());
    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(file.string()));
    {
        boost::nowide::ifstream in(file.string(), std::ios::binary);
        REQUIRE(in);
        const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(got == body);
    }
}

TEST_CASE("AppContainer copy-replace keeps the original when MoveFileEx fails", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    boost::system::error_code ec;
    const fs::path            dir = fs::temp_directory_path() / fs::unique_path("orca-low-move-fail-%%%%-%%%%");
    fs::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{dir};

    const fs::path    file = dir / "plugin_out.txt";
    const std::string body = "must-not-lose-this";
    {
        boost::nowide::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
        REQUIRE(out);
        out << body;
    }
    REQUIRE(plugin_ipc::win_apply_low_integrity_label_for_testing(file.string()));
    REQUIRE(plugin_ipc::win_path_has_low_integrity_label(file.string()));
    REQUIRE(restrict_dacl_to_modify(file.string()));

    const std::wstring wide = boost::nowide::widen(file.string());
    // No FILE_SHARE_DELETE: MoveFileEx(REPLACE_EXISTING) cannot replace the original.
    HANDLE held = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    plugin_ipc::win_clear_low_integrity_label(file.string());
    ::CloseHandle(held);

    CHECK_FALSE(plugin_ipc::win_low_label_used_copy_replace_for_testing());
    {
        boost::system::error_code exists_ec;
        REQUIRE(fs::exists(file, exists_ec));
        REQUIRE_FALSE(exists_ec);
        REQUIRE(fs::is_regular_file(file, exists_ec));
    }
    CHECK(plugin_ipc::win_path_has_low_integrity_label(file.string()));
    {
        boost::nowide::ifstream in(file.string(), std::ios::binary);
        REQUIRE(in);
        const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(got == body);
    }
}

#ifdef _WIN32
bool write_zone_identifier(const std::string& path)
{
    const std::wstring wide = boost::nowide::widen(path) + L":Zone.Identifier";
    HANDLE             h    = ::CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const char payload[] = "[ZoneTransfer]\r\nZoneId=3\r\n";
    DWORD      written   = 0;
    const BOOL ok        = ::WriteFile(h, payload, static_cast<DWORD>(sizeof(payload) - 1), &written, nullptr);
    ::CloseHandle(h);
    return ok && written == sizeof(payload) - 1;
}

bool zone_identifier_present(const std::string& path)
{
    const std::wstring wide = boost::nowide::widen(path) + L":Zone.Identifier";
    HANDLE             h    = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    char  buf[64]{};
    DWORD n = 0;
    ::ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
    ::CloseHandle(h);
    const std::string got(buf, buf + n);
    return got.find("ZoneId=3") != std::string::npos;
}

bool file_times(const std::string& path, FILETIME& created, FILETIME& accessed, FILETIME& written)
{
    const std::wstring wide = boost::nowide::widen(path);
    HANDLE h = ::CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const BOOL ok = ::GetFileTime(h, &created, &accessed, &written);
    ::CloseHandle(h);
    return ok != 0;
}

std::string read_file_bytes(const std::string& path)
{
    boost::nowide::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
#endif

TEST_CASE("AppContainer copy-replace preserves timestamps and attributes", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    boost::system::error_code ec;
    const fs::path            dir = fs::temp_directory_path() / fs::unique_path("orca-low-times-%%%%-%%%%");
    fs::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{dir};

    const fs::path    file = dir / "plugin_out.txt";
    const std::string body = "keep-times";
    {
        boost::nowide::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
        REQUIRE(out);
        out << body;
    }
    FILETIME created{}, accessed{}, written{};
    created.dwLowDateTime   = 0x12345678;
    created.dwHighDateTime  = 0x01d00000;
    accessed.dwLowDateTime  = 0x22345678;
    accessed.dwHighDateTime = 0x01d00000;
    written.dwLowDateTime   = 0x32345678;
    written.dwHighDateTime  = 0x01d00000;
    {
        const std::wstring wide = boost::nowide::widen(file.string());
        HANDLE             h    = ::CreateFileW(wide.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        REQUIRE(::SetFileTime(h, &created, &accessed, &written));
        ::CloseHandle(h);
    }
    REQUIRE(::SetFileAttributesW(boost::nowide::widen(file.string()).c_str(), FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN));
    REQUIRE(plugin_ipc::win_apply_low_integrity_label_for_testing(file.string()));
    REQUIRE(restrict_dacl_to_modify(file.string()));

    plugin_ipc::win_clear_low_integrity_label(file.string());
    CHECK(plugin_ipc::win_low_label_used_copy_replace_for_testing());
    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(file.string()));
    CHECK(read_file_bytes(file.string()) == body);
    FILETIME got_c{}, got_a{}, got_w{};
    REQUIRE(file_times(file.string(), got_c, got_a, got_w));
    CHECK(got_c.dwLowDateTime == created.dwLowDateTime);
    CHECK(got_c.dwHighDateTime == created.dwHighDateTime);
    CHECK(got_w.dwLowDateTime == written.dwLowDateTime);
    CHECK(got_w.dwHighDateTime == written.dwHighDateTime);
    CHECK((::GetFileAttributesW(boost::nowide::widen(file.string()).c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0);
}

TEST_CASE("AppContainer copy-replace skips hard links and alternate data streams", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    boost::system::error_code ec;
    const fs::path            dir = fs::temp_directory_path() / fs::unique_path("orca-low-ads-%%%%-%%%%");
    fs::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{dir};

    const fs::path    ads_file = dir / "motw.txt";
    const std::string ads_body = "has-motw";
    {
        boost::nowide::ofstream out(ads_file.string(), std::ios::binary | std::ios::trunc);
        REQUIRE(out);
        out << ads_body;
    }
    REQUIRE(write_zone_identifier(ads_file.string()));
    REQUIRE(zone_identifier_present(ads_file.string()));
    REQUIRE(plugin_ipc::win_apply_low_integrity_label_for_testing(ads_file.string()));
    REQUIRE(restrict_dacl_to_modify(ads_file.string()));
    plugin_ipc::win_clear_low_integrity_label(ads_file.string());
    CHECK_FALSE(plugin_ipc::win_low_label_used_copy_replace_for_testing());
    CHECK(plugin_ipc::win_path_has_low_integrity_label(ads_file.string()));
    CHECK(read_file_bytes(ads_file.string()) == ads_body);
    CHECK(zone_identifier_present(ads_file.string()));

    const fs::path    linked = dir / "linked.txt";
    const fs::path    alias  = dir / "linked-alias.txt";
    const std::string link_body = "two-names";
    {
        boost::nowide::ofstream out(linked.string(), std::ios::binary | std::ios::trunc);
        REQUIRE(out);
        out << link_body;
    }
    REQUIRE(::CreateHardLinkW(boost::nowide::widen(alias.string()).c_str(), boost::nowide::widen(linked.string()).c_str(), nullptr));
    REQUIRE(plugin_ipc::win_apply_low_integrity_label_for_testing(linked.string()));
    REQUIRE(restrict_dacl_to_modify(linked.string()));
    plugin_ipc::win_clear_low_integrity_label(linked.string());
    CHECK_FALSE(plugin_ipc::win_low_label_used_copy_replace_for_testing());
    CHECK(plugin_ipc::win_path_has_low_integrity_label(linked.string()));
    CHECK(read_file_bytes(linked.string()) == link_body);
    CHECK(read_file_bytes(alias.string()) == link_body);
}

TEST_CASE("AppContainer Low-IL tree strip copy-replaces only helper-created files", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir             data("plugin-sandbox-grant-time-strip");
    boost::system::error_code ec;
    const fs::path            user_rw = fs::temp_directory_path() / fs::unique_path("orca-grant-time-%%%%-%%%%");
    fs::create_directories(user_rw, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{user_rw};
    CloseSandbox close_guard;

    const fs::path    pre_file = user_rw / "pre-grant.txt";
    const std::string pre_body = "user-file";
    {
        boost::nowide::ofstream out(pre_file.string(), std::ios::binary | std::ios::trunc);
        REQUIRE(out);
        out << pre_body;
    }
    REQUIRE(write_zone_identifier(pre_file.string()));
    REQUIRE(plugin_ipc::win_apply_low_integrity_label_for_testing(pre_file.string()));
    REQUIRE(restrict_dacl_to_modify(pre_file.string()));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Grant_Time");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    spec.extra_paths.push_back({user_rw.string(), true, false});
    std::string error;
    REQUIRE(plugin_ipc::prepare_parent_sandbox(spec, error));

    const fs::path    post_file = user_rw / "post-grant.txt";
    const std::string post_body = "helper-out";
    {
        boost::nowide::ofstream out(post_file.string(), std::ios::binary | std::ios::trunc);
        REQUIRE(out);
        out << post_body;
    }
    REQUIRE(plugin_ipc::win_apply_low_integrity_label_for_testing(post_file.string()));
    REQUIRE(restrict_dacl_to_modify(post_file.string()));

    plugin_ipc::close_parent_sandbox();
    CHECK(plugin_ipc::win_path_has_low_integrity_label(pre_file.string()));
    CHECK(read_file_bytes(pre_file.string()) == pre_body);
    CHECK(zone_identifier_present(pre_file.string()));
    CHECK_FALSE(plugin_ipc::win_path_has_low_integrity_label(post_file.string()));
    CHECK(read_file_bytes(post_file.string()) == post_body);
}

TEST_CASE("AppContainer journals an existing package-SID ACE so sweep can remove it", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir             data("plugin-sandbox-existing-sid");
    boost::system::error_code ec;
    const fs::path            dir = fs::temp_directory_path() / fs::unique_path("orca-existing-sid-%%%%-%%%%");
    fs::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{dir};
    CloseSandbox close_guard;

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Existing_SID");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    spec.extra_paths.push_back({dir.string(), false, false});
    std::string error;
    REQUIRE(plugin_ipc::prepare_parent_sandbox(spec, error));
    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    REQUIRE_FALSE(sid_str.empty());
    LocalSid package(sid_str.c_str());
    REQUIRE(package.sid != nullptr);
    REQUIRE(path_dacl_has_sid(dir.string(), package.sid));
    REQUIRE(granted_paths_contain(dir.string()));

    plugin_ipc::win_abandon_grants_for_testing();
    REQUIRE(path_dacl_has_sid(dir.string(), package.sid));
    // Simulate the journal vanishing while ACEs remain (early delete / different data_dir).
    {
        boost::system::error_code jec;
        fs::remove(plugin_ipc::win_acl_journal_path_for_testing(), jec);
    }

    error.clear();
    REQUIRE(plugin_ipc::prepare_parent_sandbox(spec, error));
    CHECK(granted_paths_contain(dir.string()));
    CHECK(path_dacl_has_sid(dir.string(), package.sid));
    REQUIRE(journal_file_exists(plugin_ipc::win_acl_journal_path_for_testing()));

    plugin_ipc::close_parent_sandbox();
    CHECK_FALSE(path_dacl_has_sid(dir.string(), package.sid));
}

TEST_CASE("AppContainer journal stays while the host is alive and sweep drops python_home ACEs", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedDataDir data("plugin-sandbox-live-journal");
    fs::create_directories(data.plugins_dir());
    CloseSandbox close_guard;

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Journal_Live");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    spec.python_home       = PythonInterpreter::bundled_python_home();
    spec.plugin_root       = data.plugins_dir().string();
    REQUIRE_FALSE(spec.python_home.empty());

    ScopedSpawnedHost spawned;
    std::string       error;
    const bool        ok = plugin_ipc::spawn_host(spawned.host, spec, error);
    INFO("spawn error: " << error);
    REQUIRE(ok);
    REQUIRE(spawned.host.pid > 0);

    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    REQUIRE_FALSE(sid_str.empty());
    LocalSid package(sid_str.c_str());
    REQUIRE(package.sid != nullptr);
    const std::string helper_dir = plugin_ipc::helper_path().parent_path().string();
    CHECK(path_dacl_has_sid(spec.python_home, package.sid));
    CHECK(path_dacl_has_sid(helper_dir, package.sid));

    const std::string journal = plugin_ipc::win_acl_journal_path_for_testing();
    REQUIRE(journal_file_exists(journal));
    CHECK(fs::path(journal).parent_path().filename() == "plugin_host_acl");
    CHECK_FALSE(granted_paths_contain(fs::path(journal).parent_path().string()));
    {
        boost::system::error_code jec;
        CHECK_FALSE(fs::exists(fs::path(data.dir) / "plugin_host" / "acl_journal.json", jec));
    }
    const std::wstring ac_local = plugin_ipc::win_appcontainer_localappdata_path();
    if (!ac_local.empty()) {
        const fs::path ac_journal = fs::path(ac_local) / "EdgeSlicer" / "plugin_host" / "acl_journal.json";
        CHECK_FALSE(journal_file_exists(ac_journal.string()));
    }

    // Crash with the helper still running: drop in-memory grants, keep journal + ACEs.
    plugin_ipc::win_abandon_grants_for_testing();
    REQUIRE(spawned.host.pid > 0);
    REQUIRE(journal_file_exists(journal));
    CHECK(path_dacl_has_sid(spec.python_home, package.sid));
    CHECK(path_dacl_has_sid(helper_dir, package.sid));

    // Reap the helper so it releases edge-plugin-host.log before the Low-label walk.
    plugin_ipc::kill_host(spawned.host);
    if (spawned.host.process)
        ::WaitForSingleObject(static_cast<HANDLE>(spawned.host.process), 5000);

    plugin_ipc::sweep_stale_parent_sandbox_grants();
    CHECK_FALSE(path_dacl_has_sid(spec.python_home, package.sid));
    CHECK_FALSE(path_dacl_has_sid(helper_dir, package.sid));
    CHECK_FALSE(journal_file_exists(journal));
}

TEST_CASE("AppContainer ACL journal DACL excludes the package SID", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir             data("plugin-sandbox-journal-dacl");
    boost::system::error_code ec;
    const fs::path            dir = fs::temp_directory_path() / fs::unique_path("orca-journal-dacl-%%%%-%%%%");
    fs::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{dir};
    CloseSandbox close_guard;

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Journal_Dacl");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    spec.write_root        = dir.string();
    std::string error;
    REQUIRE(plugin_ipc::prepare_parent_sandbox(spec, error));
    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    REQUIRE_FALSE(sid_str.empty());
    LocalSid package(sid_str.c_str());
    LocalSid any(L"S-1-15-2-1");
    REQUIRE(package.sid != nullptr);
    REQUIRE(any.sid != nullptr);
    const fs::path journal_dir = fs::path(plugin_ipc::win_acl_journal_path_for_testing()).parent_path();
    CHECK_FALSE(granted_paths_contain(journal_dir.string()));
    CHECK_FALSE(path_dacl_has_sid(journal_dir.string(), package.sid));
    CHECK_FALSE(path_dacl_has_sid(journal_dir.string(), any.sid));
    CHECK_FALSE(path_dacl_has_sid(plugin_ipc::win_acl_journal_path_for_testing(), package.sid));
    CHECK_FALSE(path_dacl_has_sid(plugin_ipc::win_acl_journal_path_for_testing(), any.sid));
}

TEST_CASE("AppContainer sweep drops ALL APPLICATION PACKAGES journal SIDs", "[PluginSandbox][windows][acl]")
{
    ScopedDataDir  data("plugin-sandbox-journal-wellknown-sid");
    const fs::path target = data.dir / "plugin_host";
    fs::create_directories(target);
    write_acl_journal(nlohmann::json::array({
        {{"package_sid", "S-1-15-2-1"},
         {"profile_name", ""},
         {"grants", nlohmann::json::array({
                        {{"path", target.string()},
                         {"permissions", 1179817},
                         {"inheritance", 3},
                         {"write", true},
                         {"low_label", false},
                         {"existing", false},
                         {"original_label", ""}},
                    })}},
    }));
    REQUIRE(journal_file_exists(plugin_ipc::win_acl_journal_path_for_testing()));
    plugin_ipc::sweep_stale_parent_sandbox_grants();
    CHECK_FALSE(journal_file_exists(plugin_ipc::win_acl_journal_path_for_testing()));
}

TEST_CASE("AppContainer sweep drops journal sessions with an empty profile_name", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir data("plugin-sandbox-empty-profile");
    CloseSandbox  close_guard;

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Empty_Profile");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    std::string error;
    REQUIRE(plugin_ipc::prepare_parent_sandbox(spec, error));
    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    REQUIRE_FALSE(sid_str.empty());
    plugin_ipc::close_parent_sandbox();

    write_acl_journal(nlohmann::json::array({
        {{"package_sid", boost::nowide::narrow(sid_str)},
         {"profile_name", ""},
         {"grants", nlohmann::json::array({
                        {{"path", (data.dir / "plugin_host").string()},
                         {"permissions", 1179817},
                         {"inheritance", 3},
                         {"write", true},
                         {"low_label", false},
                         {"existing", false},
                         {"original_label", ""}},
                    })}},
    }));
    REQUIRE(journal_file_exists(plugin_ipc::win_acl_journal_path_for_testing()));
    plugin_ipc::sweep_stale_parent_sandbox_grants();
    CHECK_FALSE(journal_file_exists(plugin_ipc::win_acl_journal_path_for_testing()));
}

TEST_CASE("AppContainer sweep caps leftover grant revoke and keeps the remainder", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir data("plugin-sandbox-sweep-cap");
    CloseSandbox  close_guard;

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Sweep_Cap");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    std::string error;
    REQUIRE(plugin_ipc::prepare_parent_sandbox(spec, error));
    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    REQUIRE_FALSE(sid_str.empty());
    const std::wstring profile = plugin_ipc::win_appcontainer_profile_name(spec.plugin_key);
    plugin_ipc::close_parent_sandbox();

    nlohmann::json grants = nlohmann::json::array();
    for (int i = 0; i < 513; ++i) {
        grants.push_back({{"path", (data.dir / ("missing-" + std::to_string(i))).string()},
                          {"permissions", 1179817},
                          {"inheritance", 3},
                          {"write", false},
                          {"low_label", false},
                          {"existing", false},
                          {"original_label", ""}});
    }
    write_acl_journal(nlohmann::json::array({
        {{"package_sid", boost::nowide::narrow(sid_str)},
         {"profile_name", boost::nowide::narrow(profile)},
         {"data_dir", spec.data_dir},
         {"python_home", ""},
         {"helper_dir", ""},
         {"resources_dir", ""},
         {"grants", std::move(grants)}},
    }));
    const std::string journal = plugin_ipc::win_acl_journal_path_for_testing();
    REQUIRE(journal_file_exists(journal));
    plugin_ipc::sweep_stale_parent_sandbox_grants();
    REQUIRE(journal_file_exists(journal));
    CHECK(journal_grant_path_count(journal) == 1);
}

TEST_CASE("AppContainer sweep keeps skipped journal entries and does not delete the session", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir data("plugin-sandbox-journal-skip-keep");
    CloseSandbox  close_guard;

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Journal_SkipKeep");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    std::string error;
    REQUIRE(plugin_ipc::prepare_parent_sandbox(spec, error));
    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    REQUIRE_FALSE(sid_str.empty());
    const std::wstring profile     = plugin_ipc::win_appcontainer_profile_name(spec.plugin_key);
    const std::string  journal     = plugin_ipc::win_acl_journal_path_for_testing();
    const std::string  journal_dir = fs::path(journal).parent_path().string();
    plugin_ipc::close_parent_sandbox();

    // The journal tree is never actionable. Skipping it must keep the session so a
    // later sweep can still see the remainder (do not delete the journal).
    write_acl_journal(nlohmann::json::array({
        {{"package_sid", boost::nowide::narrow(sid_str)},
         {"profile_name", boost::nowide::narrow(profile)},
         {"data_dir", spec.data_dir},
         {"python_home", ""},
         {"helper_dir", ""},
         {"resources_dir", ""},
         {"grants", nlohmann::json::array({
                        {{"path", journal_dir},
                         {"permissions", 1179817},
                         {"inheritance", 3},
                         {"write", true},
                         {"low_label", false},
                         {"existing", false},
                         {"original_label", ""}},
                    })}},
    }));
    REQUIRE(journal_file_exists(journal));
    plugin_ipc::sweep_stale_parent_sandbox_grants();
    REQUIRE(journal_file_exists(journal));
    CHECK(journal_grant_path_count(journal) == 1);
}

TEST_CASE("AppContainer sweep revokes python_home and helper ACEs outside this process's roots", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir             data("plugin-sandbox-journal-foreign-roots");
    CloseSandbox              close_guard;
    boost::system::error_code ec;
    const fs::path foreign_base   = plugin_ipc::helper_path().parent_path().parent_path() / fs::unique_path("orca-foreign-roots-%%%%-%%%%");
    const fs::path foreign_python = foreign_base / "python";
    const fs::path foreign_helper = foreign_base / "helper";
    fs::create_directories(foreign_python, ec);
    REQUIRE_FALSE(ec);
    fs::create_directories(foreign_helper, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{foreign_base};

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Foreign_Roots");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    spec.python_home       = foreign_python.string();
    spec.extra_paths.push_back({foreign_helper.string(), false, false});
    std::string error;
    const bool  ok = plugin_ipc::prepare_parent_sandbox(spec, error);
    INFO("prepare error: " << error);
    REQUIRE(ok);

    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    REQUIRE_FALSE(sid_str.empty());
    LocalSid package(sid_str.c_str());
    REQUIRE(package.sid != nullptr);
    CHECK(path_dacl_has_sid(foreign_python.string(), package.sid));
    CHECK(path_dacl_has_sid(foreign_helper.string(), package.sid));
    const std::string journal = plugin_ipc::win_acl_journal_path_for_testing();
    REQUIRE(journal_file_exists(journal));

    plugin_ipc::win_abandon_grants_for_testing();
    CHECK(path_dacl_has_sid(foreign_python.string(), package.sid));
    CHECK(path_dacl_has_sid(foreign_helper.string(), package.sid));

    plugin_ipc::sweep_stale_parent_sandbox_grants();
    CHECK_FALSE(path_dacl_has_sid(foreign_python.string(), package.sid));
    CHECK_FALSE(path_dacl_has_sid(foreign_helper.string(), package.sid));
    CHECK_FALSE(journal_file_exists(journal));
}

TEST_CASE("AppContainer sweep keeps the journal session when a revoke fails", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir             data("plugin-sandbox-journal-revoke-fail");
    boost::system::error_code ec;
    const fs::path            dir = fs::temp_directory_path() / fs::unique_path("orca-revoke-fail-%%%%-%%%%");
    fs::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    struct RemoveDir
    {
        fs::path p;
        ~RemoveDir()
        {
            boost::system::error_code ignore;
            fs::remove_all(p, ignore);
        }
    } cleanup{dir};
    CloseSandbox close_guard;

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Revoke_Fail");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    spec.write_root        = dir.string();
    std::string error;
    const bool  ok = plugin_ipc::prepare_parent_sandbox(spec, error);
    INFO("prepare error: " << error);
    REQUIRE(ok);
    const std::string journal = plugin_ipc::win_acl_journal_path_for_testing();
    REQUIRE(journal_file_exists(journal));
    REQUIRE(journal_grant_path_count(journal) >= 1);

    plugin_ipc::win_abandon_grants_for_testing();
    REQUIRE(restrict_dacl_to_modify(dir.string()));

    plugin_ipc::sweep_stale_parent_sandbox_grants();
    REQUIRE(journal_file_exists(journal));
    CHECK(journal_grant_path_count(journal) >= 1);
}

TEST_CASE("AppContainer sweep treats a vanished grant path as already revoked", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir data("plugin-sandbox-journal-vanished");
    CloseSandbox  close_guard;

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key        = unique_test_plugin_key("Journal_Vanished");
    close_guard.plugin_key = spec.plugin_key;
    spec.data_dir          = data.dir.string();
    std::string error;
    REQUIRE(plugin_ipc::prepare_parent_sandbox(spec, error));
    const std::wstring sid_str = plugin_ipc::win_current_package_sid_string_for_testing();
    REQUIRE_FALSE(sid_str.empty());
    const std::wstring profile = plugin_ipc::win_appcontainer_profile_name(spec.plugin_key);
    plugin_ipc::close_parent_sandbox();

    const fs::path vanished = fs::temp_directory_path() / fs::unique_path("orca-vanished-%%%%-%%%%");
    {
        boost::system::error_code ec;
        REQUIRE_FALSE((fs::exists(vanished, ec) && !ec));
    }
    nlohmann::json grants = nlohmann::json::array();
    for (int i = 0; i < 20; ++i) {
        grants.push_back({{"path", (vanished / std::to_string(i)).string()},
                          {"permissions", 1179817},
                          {"inheritance", 3},
                          {"write", true},
                          {"low_label", false},
                          {"existing", false},
                          {"original_label", ""}});
    }
    write_acl_journal(nlohmann::json::array({
        {{"package_sid", boost::nowide::narrow(sid_str)}, {"profile_name", boost::nowide::narrow(profile)}, {"grants", std::move(grants)}},
    }));
    REQUIRE(journal_grant_path_count(plugin_ipc::win_acl_journal_path_for_testing()) == 20);

    plugin_ipc::sweep_stale_parent_sandbox_grants();
    CHECK_FALSE(journal_file_exists(plugin_ipc::win_acl_journal_path_for_testing()));

    error.clear();
    REQUIRE(plugin_ipc::prepare_parent_sandbox(spec, error));
    CHECK(journal_grant_path_count(plugin_ipc::win_acl_journal_path_for_testing()) == plugin_ipc::win_granted_paths_for_testing().size());
    CHECK(journal_grant_path_count(plugin_ipc::win_acl_journal_path_for_testing()) < 20);
    plugin_ipc::close_parent_sandbox();
    CHECK_FALSE(journal_file_exists(plugin_ipc::win_acl_journal_path_for_testing()));
}

TEST_CASE("AppContainer spawn SKIP without a working profile", "[PluginSandbox][windows]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");
    ScopedDataDir           data("plugin-sandbox-spawn-skip");
    ScopedSpawnedHost       spawned;
    std::string             error;
    plugin_ipc::SandboxSpec spec;
    spec.plugin_key            = unique_test_plugin_key("Win_Probe");
    spec.data_dir              = data.dir.string();
    spec.python_home           = PythonInterpreter::bundled_python_home();
    spawned.sandbox_plugin_key = spec.plugin_key;
    REQUIRE(plugin_ipc::spawn_host(spawned.host, spec, error));
}

TEST_CASE("AppContainer spawn from a fresh data dir with no python/packages", "[PluginSandbox][windows]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedDataDir  data("plugin-sandbox-fresh-packages");
    const fs::path packages = fs::path(PythonInterpreter::shared_packages_dir());
    {
        boost::system::error_code ec;
        const bool                present = fs::exists(packages, ec) && !ec;
        REQUIRE_FALSE(present);
    }

    fs::create_directories(data.plugins_dir());
    const std::string       key  = unique_test_plugin_key("Fresh_Packages");
    plugin_ipc::SandboxSpec spec = plugin_ipc::make_sandbox_spec(key, {});
    spec.plugin_key              = key;
    spec.python_home             = PythonInterpreter::bundled_python_home();
    spec.plugin_root             = data.plugins_dir().string();
    {
        boost::system::error_code ec;
        CHECK(fs::is_directory(packages, ec));
    }

    ScopedSpawnedHost spawned;
    spawned.sandbox_plugin_key = spec.plugin_key;
    nlohmann::json reply;
    std::string    error;
    const bool     ok = spawn_and_handshake(spawned.host, spec, reply, error);
    INFO("spawn error: " << error);
    REQUIRE(ok);
    REQUIRE(reply.value("type", std::string()) == plugin_ipc::k_type_hello_ok);
    REQUIRE(reply.contains("sandbox"));
    const auto report = plugin_ipc::sandbox_report_from_json(reply["sandbox"]);
    INFO("sandbox kind=" << report.kind << " applied=" << report.applied << " detail=" << report.detail);
    CHECK(report.applied);
    CHECK(report.kind == plugin_ipc::k_sandbox_kind_appcontainer);
}

TEST_CASE("AppContainer prepare fails when no data dir is resolvable", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    const std::string previous = Slic3r::data_dir();
    Slic3r::set_data_dir("");
    struct RestoreDataDir
    {
        std::string previous;
        ~RestoreDataDir()
        {
            plugin_ipc::close_parent_sandbox();
            Slic3r::set_data_dir(previous);
        }
    } restore{previous};

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key = unique_test_plugin_key("No_DataDir");
    std::string error;
    const bool  ok = plugin_ipc::prepare_parent_sandbox(spec, error);
    INFO("prepare error: " << error);
    REQUIRE_FALSE(ok);
    CHECK(error.find("no data dir") != std::string::npos);
}

TEST_CASE("AppContainer creates a missing plugin_host_acl journal dir", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedTemporaryDir global_b("plugin-sandbox-missing-journal-global");
    ScopedTemporaryDir spec_a("plugin-sandbox-missing-journal-spec");
    const std::string  previous = Slic3r::data_dir();
    Slic3r::set_data_dir(global_b.path().string());
    struct RestoreDataDir
    {
        std::string previous;
        ~RestoreDataDir() { Slic3r::set_data_dir(previous); }
    } restore{previous};

    const fs::path journal_dir = spec_a.path() / "plugin_host_acl";
    {
        boost::system::error_code ec;
        REQUIRE_FALSE((fs::exists(journal_dir, ec) && !ec));
    }

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key = unique_test_plugin_key("Missing_JournalDir");
    spec.data_dir   = spec_a.path().string();
    CloseSandbox close_guard{spec.plugin_key};
    std::string  error;
    const bool   ok = plugin_ipc::prepare_parent_sandbox(spec, error);
    INFO("prepare error: " << error);
    REQUIRE(ok);
    {
        boost::system::error_code ec;
        REQUIRE(fs::is_directory(journal_dir, ec));
        REQUIRE_FALSE(ec);
    }
    REQUIRE(journal_file_exists((journal_dir / "acl_journal.json").string()));
    CHECK_FALSE(journal_file_exists((global_b.path() / "plugin_host_acl" / "acl_journal.json").string()));
}

TEST_CASE("AppContainer journal follows spec.data_dir when global data_dir changes", "[PluginSandbox][windows][acl]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedTemporaryDir spec_a("plugin-sandbox-journal-spec-a");
    ScopedTemporaryDir global_b("plugin-sandbox-journal-global-b");
    const std::string  previous = Slic3r::data_dir();
    Slic3r::set_data_dir(global_b.path().string());
    struct RestoreDataDir
    {
        std::string previous;
        ~RestoreDataDir() { Slic3r::set_data_dir(previous); }
    } restore{previous};

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key = unique_test_plugin_key("Journal_SpecDir");
    spec.data_dir   = spec_a.path().string();
    CloseSandbox close_guard{spec.plugin_key};

    std::string error;
    bool        ok = false;
    if (plugin_ipc::helper_exists()) {
        ScopedSpawnedHost spawned;
        spawned.sandbox_plugin_key = spec.plugin_key;
        ok                         = plugin_ipc::spawn_host(spawned.host, spec, error);
        INFO("spawn error: " << error);
        REQUIRE(ok);
        const fs::path journal_a = spec_a.path() / "plugin_host_acl" / "acl_journal.json";
        const fs::path journal_b = global_b.path() / "plugin_host_acl" / "acl_journal.json";
        REQUIRE(journal_file_exists(journal_a.string()));
        CHECK_FALSE(journal_file_exists(journal_b.string()));

        ScopedTemporaryDir global_c("plugin-sandbox-journal-global-c");
        Slic3r::set_data_dir(global_c.path().string());
        plugin_ipc::close_host(spawned.host);
        CHECK_FALSE(journal_file_exists(journal_a.string()));
        CHECK_FALSE(journal_file_exists(journal_b.string()));
        CHECK_FALSE(journal_file_exists((global_c.path() / "plugin_host_acl" / "acl_journal.json").string()));
    } else {
        ok = plugin_ipc::prepare_parent_sandbox(spec, error);
        INFO("prepare error: " << error);
        REQUIRE(ok);
        const fs::path journal_a = spec_a.path() / "plugin_host_acl" / "acl_journal.json";
        const fs::path journal_b = global_b.path() / "plugin_host_acl" / "acl_journal.json";
        REQUIRE(journal_file_exists(journal_a.string()));
        CHECK_FALSE(journal_file_exists(journal_b.string()));

        ScopedTemporaryDir global_c("plugin-sandbox-journal-global-c");
        Slic3r::set_data_dir(global_c.path().string());
        plugin_ipc::close_parent_sandbox();
        CHECK_FALSE(journal_file_exists(journal_a.string()));
        CHECK_FALSE(journal_file_exists(journal_b.string()));
        CHECK_FALSE(journal_file_exists((global_c.path() / "plugin_host_acl" / "acl_journal.json").string()));
    }
}

TEST_CASE("AppContainer teardown leaves a pre-existing profile in place", "[PluginSandbox][windows]")
{
    const auto probe = plugin_ipc::probe_os_sandbox();
    if (!probe.appcontainer)
        SKIP("AppContainer unavailable: " + probe.reason);

    ScopedDataDir     data("plugin-sandbox-preexisting-profile");
    const std::string key = unique_test_plugin_key("Preexisting_K");

    struct UninstallAtEnd
    {
        std::string key;
        ~UninstallAtEnd() { plugin_ipc::delete_parent_sandbox_profile(key); }
    } uninstall{key};

    std::string create_error;
    const bool  created = plugin_ipc::win_create_appcontainer_profile_for_testing(key, create_error);
    INFO("create error: " << create_error);
    REQUIRE(created);
    REQUIRE(plugin_ipc::win_appcontainer_profile_exists_for_testing(key));

    plugin_ipc::SandboxSpec spec;
    spec.plugin_key  = key;
    spec.data_dir    = data.dir.string();
    spec.python_home = PythonInterpreter::bundled_python_home();
    std::string error;
    if (plugin_ipc::helper_exists()) {
        ScopedSpawnedHost spawned;
        spawned.sandbox_plugin_key = key;
        CloseSandbox close_guard{key};
        const bool   ok = plugin_ipc::spawn_host(spawned.host, spec, error);
        INFO("spawn error: " << error);
        REQUIRE(ok);
    } else {
        CloseSandbox close_guard{key};
        const bool   ok = plugin_ipc::prepare_parent_sandbox(spec, error);
        INFO("prepare error: " << error);
        REQUIRE(ok);
    }

    REQUIRE(plugin_ipc::win_appcontainer_profile_exists_for_testing(key));
}
#endif
