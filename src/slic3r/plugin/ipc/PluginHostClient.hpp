#pragma once

#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/plugin/PluginManager.hpp>

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
    static void set_force_in_process_for_testing(bool force);
    static void set_execute_timeout_for_testing(int timeout_ms);
    static void reset_spawn_count_for_testing();
    static std::uint64_t spawn_count();

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
    std::unique_ptr<Impl> m_impl;
    Impl&                 impl();
};

} // namespace Slic3r::plugin_ipc
