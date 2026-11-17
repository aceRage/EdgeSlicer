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
    // Windows: apply a Low-IL mandatory label only on Edge-created work dirs (plugin_data,
    // host-log, write_root). User-chosen fs_readwrite folders stay at their original label.
    bool low_label = false;
};

// What the child should lock down to. Built on the Edge side from p4 grants plus the
// bundled Python tree, the plugin dir, and the post-process G-code folder.
struct SandboxSpec
{
    std::string plugin_key;
    std::string python_home;
    std::string plugin_root;
    std::string write_root; // extra writable path for tests; post-process G-code is inlined over IPC
    // Parent AppContainer grants a non-inherited traverse ACE on this folder (not list/read).
    std::string              data_dir;
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
    // True when the shipping sandbox is applied but weaker than the current policy
    // (Linux Landlock ABI < 3 has no truncate; ABI < 6 has no abstract-unix/signal scopes).
    // Fail-closed still requires applied.
    bool degraded = false;
};

// Fail-closed handshake: only these kinds skip the Don't-run prompt. seatbelt-probe
// is labelled isolation for tests and does not satisfy production.
inline bool sandbox_kind_is_shipping(const std::string& kind)
{
    return kind == k_sandbox_kind_landlock_seccomp || kind == k_sandbox_kind_appcontainer || kind == k_sandbox_kind_app_sandbox;
}

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

// Build the spec for a plugin execute: python home, plugin dir, extra writable test root, p4 grants.
SandboxSpec make_sandbox_spec(const std::string& plugin_key, const std::string& write_root = {});
SandboxSpec make_sandbox_spec(const PluginDescriptor& descriptor, const PluginPermissions& permissions, const std::string& write_root);

// Writable directory for the helper diagnostic log: write_root if set, else <data_dir>/plugin_host.
std::string host_child_log_dir(const SandboxSpec& spec);
std::string host_child_log_path(const SandboxSpec& spec);

// Child: apply the OS sandbox. Fail closed (return false) rather than skip. Called after
// CPython starts (so encodings/libpython are already mapped) and before load/execute so a
// plugin import cannot observe the unsandboxed filesystem. Landlock/seccomp are inherited.
bool apply_os_sandbox(const SandboxSpec& spec, SandboxReport& report, std::string& error);

// Parent: Windows AppContainer + ACLs around CreateProcess. No-op on POSIX (child-applied).
// Returns false when the platform requires a parent-applied sandbox and it cannot be set up.
bool  prepare_parent_sandbox(const SandboxSpec& spec, std::string& error);
void* parent_sandbox_security_capabilities(); // Windows STARTUPINFOEX blob; nullptr elsewhere
void  close_parent_sandbox();
// Windows: revoke package-SID ACEs and restore Low-IL labels listed in the grant journal
// left by a crash/kill, then delete the journal. No-op on POSIX, in the helper, and when
// no journal exists. Call on every **parent** process start that can spawn the helper
// (GUI PluginManager::initialize, CLI after data_dir is set, and prepare). The journal
// is parent-only at `{spec.data_dir or data_dir()}/plugin_host_acl/acl_journal.json` (not
// the helper-writable `plugin_host` log dir, and not `%LOCALAPPDATA%` / Packages\\*\\AC).
// The path is resolved once at grant time and stored on the session; close/revoke never
// re-reads the global. An explicit DACL excludes the package SID and ALL APPLICATION
// PACKAGES. Sweep accepts only derived `EdgeSlicer.ph.*` / test `EdgeSlicer.t.*`
// package SIDs. ACE revoke and Low-label restore on a recorded grant path are
// allowed for that session's python_home / helper / resources / data_dir even when
// those paths sit outside this process's current roots (moved install, portable
// copy, test binary vs GUI). Copy-replace of Low-IL files stays inside this
// process's known user-rw roots (current data_dir, remembered spec data_dirs,
// plugin dirs, python_home, helper, resources, temp). A session is rewritten
// with only the remainder and kept if any grant was skipped or failed; the
// journal is deleted only after every grant in every session is removed.
// Leftover package-SID ACEs are recorded even when RX is already present.
// ALL APPLICATION PACKAGES RX (Program Files) is not journaled. A missing journal
// directory is created; spawn fails only when no data dir is resolvable.
void sweep_stale_parent_sandbox_grants();
// Windows: DeleteAppContainerProfile for this exact plugin_key on **permanent uninstall**
// only. Never general cleanup, never close_host, never test teardown of a pre-existing
// profile. No-op elsewhere, if the profile is missing, or while a host still uses it.
void delete_parent_sandbox_profile(const std::string& plugin_key);
// Test-only: DeleteAppContainerProfile iff this process's CreateAppContainerProfile
// succeeded (not ERROR_ALREADY_EXISTS). No-op on POSIX and for pre-existing profiles.
void delete_test_created_parent_sandbox_profile(const std::string& plugin_key);
// Test harness: honour `test.` plugin keys as EdgeSlicer.t.* AppContainer monikers.
// Production never enables this; without it, `test.*` hashes to EdgeSlicer.ph.* like any other key.
void set_test_only_plugin_keys_for_testing(bool enable);

#ifdef _WIN32
// Named-pipe SECURITY_ATTRIBUTES that lets the AppContainer SID connect. nullptr when unused.
void* parent_sandbox_pipe_security_attributes();
// Fixed-length FNV-1a profile name (no truncation collisions). Tests cover the hash.
std::wstring win_appcontainer_profile_name(const std::string& plugin_key);
// Named-pipe SECURITY_DESCRIPTOR used for AppContainer IPC. DACL grants GENERIC_ALL to the
// package SID, the current process TokenUser, and SYSTEM (not BA: a non-elevated token has
// Administrators as deny-only, so the user/groups check fails). SACL is Low IL + no-write-up.
// Caller LocalFree's *out_sd. container_sid / out_sd are PSID / PSECURITY_DESCRIPTOR.
bool win_build_pipe_security_descriptor(void* container_sid, void** out_sd, std::string& error);
// Paths that received a package-SID ACE during the current prepare (skipped RO grants omitted).
std::vector<std::string> win_granted_paths_for_testing();
std::wstring             win_current_package_sid_string_for_testing();
// Drop in-memory grant state without revoking or deleting the journal (crash simulation).
void win_abandon_grants_for_testing();
bool win_path_has_low_integrity_label(const std::string& path);
bool win_apply_low_integrity_label_for_testing(const std::string& path);
// Restore the default (no Low ML ACE). Used after Edge writes hosted G-code out of plugin_data.
// If LABEL_SECURITY_INFORMATION fails (no WRITE_OWNER), copy-replaces a regular file as the user.
void        win_clear_low_integrity_label(const std::string& path);
std::string win_acl_journal_path_for_testing();
bool        win_low_label_used_copy_replace_for_testing();
// Packages\<family>\AC when the current AppContainer SID is known; empty otherwise.
std::wstring win_appcontainer_localappdata_path();
// Pre-create a profile without recording it as test-created (simulates a leftover).
bool win_create_appcontainer_profile_for_testing(const std::string& plugin_key, std::string& error);
bool win_appcontainer_profile_exists_for_testing(const std::string& plugin_key);
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

// Unblock allow_unsandboxed_run waiters (host shutdown). Tests can poll the flag.
void abort_sandbox_unavailable_prompt();
void clear_sandbox_unavailable_prompt_abort();
bool sandbox_unavailable_prompt_aborted();

// Returns true when the workload may proceed. applied_out is false when the user (or a stored
// override / test env) allowed an unsandboxed run. Default is Don't run.
bool allow_unsandboxed_run(const std::string& plugin_key, const std::string& reason, bool& applied_out, std::string& error);

std::string sandbox_unavailable_message(const std::string& plugin_key, const std::string& reason);

} // namespace Slic3r::plugin_ipc
