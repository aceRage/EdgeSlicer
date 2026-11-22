#pragma once

#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/plugin/PluginManager.hpp>

#include "PluginIpcProtocol.hpp"
#include "PluginSandbox.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
struct SlicingPipelineContext;
class DynamicPrintConfig;
} // namespace Slic3r

namespace Slic3r::plugin_ipc {

class PluginHostClient
{
public:
    static PluginHostClient& instance();

    PluginHostClient(const PluginHostClient&)            = delete;
    PluginHostClient& operator=(const PluginHostClient&) = delete;

    // True when script / post-process should run in edge-plugin-host: always, outside tests (H4).
    static bool should_use_host();
    // Why plugins cannot run now (the helper is missing, or not signed although the app is),
    // empty when they can. Plugin code then does not run at all; there is no in-process
    // fallback (H4).
    static std::string   host_unavailable_reason();
    // !host_unavailable_reason().empty(). The Home card shows the reason.
    static bool          host_fallback_active();
    static void          set_force_in_process_for_testing(bool force);
    static void          set_execute_timeout_for_testing(int timeout_ms);
    static void          set_allow_env_overrides_for_testing(bool allow);
    static void          reset_spawn_count_for_testing();
    // Runs at the start of every worker job; a test throws from it to check containment (H2).
    static void          set_job_hook_for_testing(std::function<void()> hook);
    // D3 tests: rewrites every message read from the helper before validation (a hostile helper).
    static void          set_reply_filter_for_testing(std::function<void(nlohmann::json&)> filter);
    static std::uint64_t spawn_count();

    // Snapshot of print config for a post-process execute. Omits the same banned keys
    // append_full_config skips (print_host credentials, visual-only, ...).
    static nlohmann::json config_snapshot(const DynamicPrintConfig* config);
    static int            execute_timeout_ms(bool post_process, const std::string& gcode_path);

    // Starts a Script capability on a worker that talks to the child. Same completion
    // contract as ScriptAsyncRuntime::start.
    bool start_script(const std::string&                           plugin_key,
                      const std::string&                           capability_name,
                      std::function<void(const ScriptCompletion&)> on_done);

    ExecutionResult execute_post_process(const std::string&            plugin_key,
                                         const std::string&            capability_name,
                                         const SlicingPipelineContext& ctx,
                                         std::string&                  error);

    // D3: imports plugin_key in edge-plugin-host (sandboxed) and returns the capabilities it
    // registered, strictly validated. Blocks the caller (never the UI thread: PluginManager calls
    // it from its load worker), bounded by the execute timeout. No plugin code runs in this
    // process. A malformed, hostile or dead helper fails the load and is reset; it never throws.
    bool load_in_helper(const std::string& plugin_key, std::vector<HostedCapabilityInfo>& capabilities, std::string& error);

    // Lifecycle events for a hosted plugin. Fire-and-forget: returns at once on any thread (it
    // only takes a short lock to queue the plain-data snapshot) and never throws. Per plugin at
    // most k_max_event_queue events wait; when full the OLDEST is dropped, with one log line per
    // overflow. The worker sends each plugin's queue as one batch, in order. At send time it
    // drops the batch for a plugin that is no longer loaded or has no enabled listener, and each
    // event naming a local path the plugin's grants do not cover (secret stores never pass, Full
    // access included). A dead, hung (k_event_timeout_ms) or misbehaving helper is reset and that
    // batch is lost; nothing waits for it. Never prompts: when the helper would need the "Run
    // anyway" prompt the batch is dropped.
    void post_lifecycle_event(const std::string& plugin_key, const LifecycleEventSnapshot& snapshot);
    // Names of the plugin's enabled, hosted capabilities that override on_lifecycle_event; empty
    // when the plugin is not loaded.
    static std::vector<std::string> lifecycle_listeners(const std::string& plugin_key);
    // Permission filter for one event (call with plugin_key's audit context set). False when a
    // path field names a denied/secret path, or a path the plugin's grants do not cover.
    static bool lifecycle_event_visible_to(const std::string& plugin_key, const LifecycleEventSnapshot& snapshot);

    std::size_t              pending_lifecycle_events(const std::string& plugin_key) const;
    std::vector<std::string> pending_lifecycle_event_names_for_testing(const std::string& plugin_key) const;
    std::uint64_t            lifecycle_events_dropped(const std::string& plugin_key) const;
    // Waits until no lifecycle batch is queued or being sent. False on timeout.
    bool                     wait_lifecycle_events_idle_for_testing(std::chrono::milliseconds timeout);
    static void              set_event_timeout_for_testing(int timeout_ms);

    // Cancels the in-flight request for plugin_key. Bound to that request id so a
    // later job with the same key is not cancelled.
    void cancel(const std::string& plugin_key);
    void unload(const std::string& plugin_key);
    bool has_active_run(const std::string& plugin_key) const;

    // SIGKILL / TerminateProcess the child. The next execute respawns.
    bool          kill_child_for_testing();
    std::int64_t  child_pid_for_testing() const;
    SandboxReport last_sandbox_report() const;

    void shutdown();

private:
    PluginHostClient() = default;
    ~PluginHostClient();

    struct Impl;
    std::shared_ptr<Impl> m_impl;
    Impl&                 impl();
};

} // namespace Slic3r::plugin_ipc
