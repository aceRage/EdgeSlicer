#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <catch2/catch.hpp>

#include "plugin_test_utils.hpp"

#include <slic3r/plugin/PluginDescriptor.hpp>
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

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
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
TEST_CASE("macOS Seatbelt probe uses named profiles and fail-closes without sandbox_init", "[PluginSandbox][macos-sdk]")
{
    CHECK(std::string(plugin_ipc::mac_seatbelt::named_profile(false)) == "no-internet");
    CHECK(std::string(plugin_ipc::mac_seatbelt::named_profile(true)) == "no-write");
    CHECK(std::string(plugin_ipc::mac_seatbelt::sbpl_profile(false)).find("(deny network*)") != std::string::npos);
    CHECK(std::string(plugin_ipc::mac_seatbelt::sbpl_profile(true)).find("(deny file-write*)") != std::string::npos);
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
    std::string   error;
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

    REQUIRE(plugin_trust::clear_unsandboxed("Sandbox_Probe", error));
    CHECK_FALSE(plugin_trust::is_unsandboxed("Sandbox_Probe"));
    plugin_ipc::set_sandbox_unavailable_prompt_for_testing({});
}

TEST_CASE("handshake reports the OS sandbox state", "[PluginSandbox][handshake][PluginIpc]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");

    ScopedDataDir       data_dir_guard("plugin-sandbox-hello");
    ScopedSpawnedHost   spawned;
    std::string         error;
    const auto          t0 = std::chrono::steady_clock::now();
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
        CHECK(report.landlock_abi == probe.landlock_abi);
    } else {
        CHECK_FALSE(report.applied);
        CHECK(report.kind == plugin_ipc::k_sandbox_kind_none);
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

    ScopedDataDir       data_dir_guard("plugin-sandbox-seatbelt");
    ScopedSpawnedHost   spawned;
    std::string         error;
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
    REQUIRE(spawned.host.channel.read_json(reply, plugin_ipc::k_handshake_timeout_ms, error));
    CHECK(reply.value("type", std::string()) == plugin_ipc::k_type_hello_ok);
    REQUIRE(reply.contains("sandbox"));
    const auto report = plugin_ipc::sandbox_report_from_json(reply["sandbox"]);
    INFO("sandbox kind=" << report.kind << " detail=" << report.detail);
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
    const fs::path     readable = plugin_dir.path() / "ok.txt";
    const fs::path     secret   = home_dir.path() / "secret.txt";
    const fs::path     gcode    = gcode_dir.path() / "out.gcode";
    const fs::path     outside  = home_dir.path() / "out.txt";
    std::ofstream(readable.string()) << "yes";
    std::ofstream(secret.string()) << "no";
    std::ofstream(gcode.string()) << "; gcode\n";

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
        plugin_ipc::SandboxReport report;
        std::string               error;
        unsigned char             bits = 0;
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
    REQUIRE(n == 1);
    INFO("landlock bits=" << static_cast<int>(bits));
    CHECK((bits & 1u) != 0);  // applied
    CHECK((bits & 2u) != 0);  // secret denied
    CHECK((bits & 4u) != 0);  // plugin file readable
    CHECK((bits & 8u) != 0);  // gcode folder writable
    CHECK((bits & 16u) != 0); // write outside denied
    CHECK((bits & 32u) != 0); // inet socket denied
    CHECK((bits & 64u) != 0); // execve denied
}
#endif
