#pragma once

#include <nlohmann/json.hpp>

#include <boost/filesystem/path.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
struct PluginPermissions;
class PluginDescriptor;
} // namespace Slic3r

namespace Slic3r::plugin_ipc {

// Kind string reported in hello_ok.sandbox.kind. "none" means the OS sandbox was not applied.
inline constexpr const char* k_sandbox_kind_none             = "none";
inline constexpr const char* k_sandbox_kind_landlock_seccomp = "landlock+seccomp";
inline constexpr const char* k_sandbox_kind_appcontainer     = "appcontainer";
inline constexpr const char* k_sandbox_kind_app_sandbox      = "app-sandbox";
inline constexpr const char* k_sandbox_kind_seatbelt_probe   = "seatbelt-probe";

// Test-only env (honoured only when helper env overrides are allowed, same as Phase 7).
inline constexpr const char* k_env_sandbox_disable           = "EDGE_PLUGIN_SANDBOX";
inline constexpr const char* k_env_sandbox_force_unavailable = "EDGE_PLUGIN_SANDBOX_FORCE_UNAVAILABLE";
inline constexpr const char* k_env_sandbox_seatbelt_probe    = "EDGE_PLUGIN_SANDBOX_SEATBELT_PROBE";
inline constexpr const char* k_env_sandbox_bwrap             = "EDGE_PLUGIN_SANDBOX_BWRAP";

struct SandboxPath
{
    std::string path;
    bool        write = false;
};

// What the child should lock down to. Built on the Edge side from p4 grants plus the
// bundled Python tree, the plugin dir, and the post-process G-code folder.
struct SandboxSpec
{
    std::string              plugin_key;
    std::string              python_home;
    std::string              plugin_root;
    std::string              write_root; // exported G-code folder; empty for scripts
    std::vector<SandboxPath> extra_paths;
    bool                     allow_network = false;
    bool                     allow_exec    = false;
    // Parent-gated EDGE_PLUGIN_SANDBOX_SEATBELT_PROBE. Carried in hello so the child can apply
    // the labelled probe without honouring raw env (that would be reachable in release).
    bool dev_seatbelt_probe = false;
};

struct SandboxReport
{
    bool        applied = false;
    std::string kind    = k_sandbox_kind_none;
    std::string detail;
    int         landlock_abi = 0;
    bool        network      = false;
    bool        exec         = false;
};

struct SandboxProbe
{
    std::string os_name;
    bool        available = false;
    std::string reason; // why available is false, or a short note when true
    int         landlock_abi   = 0;
    bool        landlock       = false;
    bool        seccomp        = false;
    bool        appcontainer   = false;
    bool        app_sandbox    = false;
    bool        inside_flatpak = false;
    bool        bwrap_on_path  = false;
};

nlohmann::json sandbox_spec_to_json(const SandboxSpec& spec);
bool           sandbox_spec_from_json(const nlohmann::json& json, SandboxSpec& spec, std::string& error);
nlohmann::json sandbox_report_to_json(const SandboxReport& report);
SandboxReport  sandbox_report_from_json(const nlohmann::json& json);

bool sandbox_specs_equal(const SandboxSpec& a, const SandboxSpec& b);

// Probe this OS. Landlock ABI 0 on Linux means unavailable (fail closed, never a silent skip).
SandboxProbe probe_os_sandbox();
bool         os_sandbox_available();
std::string  expected_sandbox_kind();

// True inside a Flatpak sandbox (/.flatpak-info or FLATPAK_ID). Nested bwrap is never used.
bool running_inside_flatpak();

// Build the spec for a plugin execute: python home, plugin dir, G-code folder, p4 grants.
SandboxSpec make_sandbox_spec(const std::string& plugin_key, const std::string& write_root = {});
SandboxSpec make_sandbox_spec(const PluginDescriptor& descriptor, const PluginPermissions& permissions, const std::string& write_root);

// Child: apply the OS sandbox. Fail closed (return false) rather than skip. Called after
// CPython starts (so encodings/libpython are already mapped) and before load/execute so a
// plugin import cannot observe the unsandboxed filesystem. Landlock/seccomp are inherited.
bool apply_os_sandbox(const SandboxSpec& spec, SandboxReport& report, std::string& error);

// Parent: Windows AppContainer + ACLs around CreateProcess. No-op on POSIX (child-applied).
// Returns false when the platform requires a parent-applied sandbox and it cannot be set up.
bool  prepare_parent_sandbox(const SandboxSpec& spec, std::string& error);
void* parent_sandbox_security_capabilities(); // Windows STARTUPINFOEX blob; nullptr elsewhere
void  close_parent_sandbox();

#ifdef _WIN32
// Named-pipe SECURITY_ATTRIBUTES that lets the AppContainer SID connect. nullptr when unused.
void* parent_sandbox_pipe_security_attributes();
#endif

// Test-only: EDGE_PLUGIN_SANDBOX=0 / FORCE_UNAVAILABLE, same gate as EDGE_PLUGIN_HOST.
bool sandbox_env_disable_requested();
bool sandbox_env_force_unavailable();
bool sandbox_env_seatbelt_probe();
bool sandbox_env_bwrap_requested();

// Fail-closed prompt. Default button is Don't run. Production never stores an override unless
// the user explicitly chooses Run anyway. Tests install a seam; an unset seam denies.
enum class SandboxUnavailableChoice { DontRun, RunUnsandboxed };

using SandboxUnavailablePrompt = std::function<SandboxUnavailableChoice(const std::string& plugin_key, const std::string& reason)>;
void set_sandbox_unavailable_prompt_for_testing(SandboxUnavailablePrompt prompt);

// Returns true when the workload may proceed. applied_out is false when the user (or a stored
// override / test env) allowed an unsandboxed run. Default is Don't run.
bool allow_unsandboxed_run(const std::string& plugin_key, const std::string& reason, bool& applied_out, std::string& error);

std::string sandbox_unavailable_message(const std::string& plugin_key, const std::string& reason);

} // namespace Slic3r::plugin_ipc
