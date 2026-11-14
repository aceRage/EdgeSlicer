#pragma once

#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/plugin/PluginManager.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
struct SlicingPipelineContext;
class DynamicPrintConfig;
}

namespace Slic3r::plugin_ipc {

class PluginHostClient
{
public:
    static PluginHostClient& instance();

    PluginHostClient(const PluginHostClient&)            = delete;
    PluginHostClient& operator=(const PluginHostClient&) = delete;

    // True when script / post-process should run in edge-plugin-host.
    static bool should_use_host();
    // True when the helper is missing (or a test env override forced in-process) so Edge
    // falls back to in-process execute. The Home card surfaces this.
    static bool host_fallback_active();
    static void set_force_in_process_for_testing(bool force);
    static void set_execute_timeout_for_testing(int timeout_ms);
    static void set_allow_env_overrides_for_testing(bool allow);
    static void reset_spawn_count_for_testing();
    static std::uint64_t spawn_count();

    // Snapshot of print config for a post-process execute. Omits the same banned keys
    // append_full_config skips (print_host credentials, visual-only, ...).
    static nlohmann::json config_snapshot(const DynamicPrintConfig* config);
    static int            execute_timeout_ms(bool post_process, const std::string& gcode_path);

    // Starts a Script capability on a worker that talks to the child. Same completion
    // contract as ScriptAsyncRuntime::start.
    bool start_script(const std::string& plugin_key,
                      const std::string& capability_name,
                      std::function<void(const ScriptCompletion&)> on_done);

    ExecutionResult execute_post_process(const std::string& plugin_key,
                                         const std::string& capability_name,
                                         const SlicingPipelineContext& ctx,
                                         std::string& error);

    void cancel(const std::string& plugin_key);
    void unload(const std::string& plugin_key);
    bool has_active_run(const std::string& plugin_key) const;

    // SIGKILL / TerminateProcess the child. The next execute respawns.
    bool kill_child_for_testing();
    std::int64_t child_pid_for_testing() const;

    void shutdown();

private:
    PluginHostClient() = default;
    ~PluginHostClient();

    struct Impl;
    std::shared_ptr<Impl> m_impl;
    Impl&                 impl();
};

} // namespace Slic3r::plugin_ipc
