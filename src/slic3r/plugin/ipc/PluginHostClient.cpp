#include "PluginHostClient.hpp"

#include "PluginHostProcess.hpp"
#include "PluginHostRpc.hpp"
#include "PluginIpcChannel.hpp"
#include "PluginIpcProtocol.hpp"
#include "PluginSandbox.hpp"

#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Utils.hpp>
#include <slic3r/GUI/GUI.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/plugin/HostedPluginImport.hpp>
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginTrust.hpp>
#include <slic3r/plugin/pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <wx/app.h>
#include <wx/thread.h>

namespace Slic3r::plugin_ipc {
namespace {

std::atomic<bool>          g_force_in_process{false};
std::atomic<int>           g_execute_timeout_ms{k_default_timeout_ms};
std::atomic<int>           g_event_timeout_ms{k_event_timeout_ms};
std::atomic<std::uint64_t> g_spawn_count{0};
std::atomic<bool>          g_logged_fallback{false};
std::function<void()>      g_job_hook_for_testing;
std::mutex                 g_job_hook_mutex;
// Test seam (D3): rewrites every message read from the helper before it is validated, to play a
// hostile or broken helper against the real parent code.
std::function<void(nlohmann::json&)> g_reply_filter_for_testing;
std::mutex                           g_reply_filter_mutex;

void log_host_fallback_once(const char* reason)
{
    bool expected = false;
    if (!g_logged_fallback.compare_exchange_strong(expected, true, std::memory_order_relaxed))
        return;
    BOOST_LOG_TRIVIAL(warning) << "Plugin host fallback: " << reason << "; scripts and post-process run in-process";
}

void deliver_completion(std::function<void(const ScriptCompletion&)> on_done, const ScriptCompletion& completion)
{
    if (!on_done)
        return;
    if (wxTheApp == nullptr || wxIsMainThread())
        on_done(completion);
    else
        GUI::wxGetApp().CallAfter([on_done, completion]() { on_done(completion); });
}

void attach_plugin_identity(nlohmann::json& msg, const std::string& plugin_key)
{
    msg["plugin_key"] = plugin_key;
    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor(plugin_key, descriptor))
        return;
    msg["entry_path"]        = descriptor.entry_path;
    msg["plugin_root"]       = descriptor.plugin_root;
    msg["entry_package"]     = descriptor.entry_package;
    msg["name"]              = descriptor.name;
    msg["version"]           = descriptor.version;
    msg["installed_version"] = descriptor.installed_version;
    msg["required_orca_api"] = descriptor.required_orca_api;
    msg["author"]            = descriptor.author;
    msg["description"]       = descriptor.description;
    // H5: the helper cannot read trust.json; send the tree hash this side verified instead.
    const plugin_trust::TrustState trust = plugin_trust::evaluate(descriptor);
    if (trust.allowed())
        msg["verified_sha256"] = trust.sha256;
}

using HostedLoadOutcome = std::pair<std::vector<HostedCapabilityInfo>, std::string>;

struct Job
{
    // Events: send the plugin's queued lifecycle events (Impl::event_queues) as one batch.
    enum class Kind { Script, PostProcess, Unload, Load, Events };
    Kind                                                                   kind{Kind::Script};
    std::string                                                            plugin_key;
    std::string                                                            capability_name;
    std::string                                                            write_root;
    std::string                                                            gcode_path;
    nlohmann::json                                                         msg;
    int                                                                    timeout_ms{k_default_timeout_ms};
    std::uint64_t                                                          request_id{0};
    std::function<void(const ScriptCompletion&)>                           on_done;
    std::shared_ptr<std::promise<std::pair<ExecutionResult, std::string>>> post_promise;
    std::shared_ptr<std::promise<HostedLoadOutcome>>                       load_promise;
    // Set once the caller has been told the outcome, so an error path never reports twice.
    bool                                                                   finished{false};
};

} // namespace

nlohmann::json PluginHostClient::config_snapshot(const DynamicPrintConfig* config)
{
    nlohmann::json out = nlohmann::json::object();
    if (config == nullptr)
        return out;
    for (const std::string& key : config->keys()) {
        if (is_gcode_full_config_banned(key))
            continue;
        try {
            out[key] = config->opt_serialize(key);
        } catch (...) {}
    }
    return out;
}

int PluginHostClient::execute_timeout_ms(bool post_process, const std::string& gcode_path)
{
    const int configured = g_execute_timeout_ms.load(std::memory_order_acquire);
    if (!post_process || configured != k_default_timeout_ms || gcode_path.empty())
        return configured;
    boost::system::error_code ec;
    const auto                sz = boost::filesystem::file_size(gcode_path, ec);
    if (ec)
        return configured;
    const std::uint64_t mb    = (sz + 1024ull * 1024ull - 1ull) / (1024ull * 1024ull);
    const std::uint64_t extra = mb * 2000ull;
    const std::uint64_t total = static_cast<std::uint64_t>(configured) + extra;
    return static_cast<int>(std::min<std::uint64_t>(total, k_max_timeout_ms));
}

struct PluginHostClient::Impl
{
    // state_mutex: queue, active, loaded, worker, cancel flags. Never held across pump.
    // io_mutex: spawn, channel, reset. Held from writing a request through the matching reply.
    std::mutex                       state_mutex;
    std::mutex                       io_mutex;
    std::condition_variable          cv;
    std::deque<std::shared_ptr<Job>> queue;
    std::thread                      worker;
    SpawnedHost                      host;
    std::string                      data_dir;
    std::string                      resources_dir;
    std::unordered_set<std::string>  loaded;
    std::unordered_set<std::string>  active;
    std::atomic<bool>                stopping{false};
    std::atomic<bool>                cancel_requested{false};
    std::atomic<std::int64_t>        live_pid{0};
#ifdef _WIN32
    std::atomic<void*> live_process{nullptr};
#endif
    std::string                cancel_plugin_key;
    std::string                running_plugin_key;
    std::uint64_t              cancel_request_id{0};
    std::uint64_t              running_request_id{0};
    std::atomic<std::uint64_t> next_id{1};
    SandboxSpec                sandbox_spec;
    SandboxReport              sandbox_report;

    // Lifecycle events, under state_mutex. One bounded FIFO per plugin; at most one Events job per
    // plugin waits in `queue` (drain_queued). events_outstanding counts Events jobs queued or
    // running, for wait_lifecycle_events_idle_for_testing.
    struct PendingEvents
    {
        std::deque<LifecycleEventSnapshot> events;
        bool                               drain_queued{false};
        bool                               overflow_logged{false};
        std::uint64_t                      dropped{0};
    };
    std::unordered_map<std::string, PendingEvents> event_queues;
    std::size_t                                    events_outstanding{0};
    std::condition_variable                        events_idle_cv;

    void event_job_done()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (events_outstanding > 0)
            --events_outstanding;
        if (events_outstanding == 0)
            events_idle_cv.notify_all();
    }

    bool is_cancelled_locked(std::uint64_t request_id) const
    {
        return cancel_requested.load(std::memory_order_acquire) && request_id != 0 && cancel_request_id == request_id;
    }

    void consume_cancel_locked(std::uint64_t request_id)
    {
        if (request_id != 0 && cancel_request_id == request_id) {
            cancel_requested.store(false, std::memory_order_release);
            cancel_plugin_key.clear();
            cancel_request_id = 0;
        }
    }

    void ensure_worker_locked()
    {
        if (worker.joinable())
            return;
        worker = std::thread([this] { worker_loop(); });
    }

    void kill_live()
    {
#ifdef _WIN32
        void* h = live_process.load(std::memory_order_acquire);
        if (h)
            kill_host_handle(h);
#else
        const std::int64_t pid = live_pid.load(std::memory_order_acquire);
        if (pid > 0)
            kill_host_pid(pid);
#endif
    }

    void fail_job(Job& job, const std::string& error, bool cancelled)
    {
        if (job.kind == Job::Kind::Unload || job.kind == Job::Kind::Events || job.finished)
            return;
        job.finished = true;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            active.erase(job.plugin_key);
            if (running_request_id == job.request_id) {
                running_plugin_key.clear();
                running_request_id = 0;
            }
            if (cancelled)
                consume_cancel_locked(job.request_id);
        }
        if (job.kind == Job::Kind::Script) {
            ScriptCompletion completion;
            completion.error     = error;
            completion.cancelled = cancelled;
            deliver_completion(job.on_done, completion);
        } else if (job.kind == Job::Kind::Load) {
            if (job.load_promise)
                job.load_promise->set_value({{}, error.empty() ? std::string("plugin host load failed") : error});
        } else if (job.post_promise) {
            job.post_promise->set_value({ExecutionResult::failure(PluginResult::FatalError, error), error});
        }
    }

    // H2: every message from the helper goes through the schema check. A violation is reported
    // like a transport failure, and every caller then kills and resets the helper.
    bool read_child(nlohmann::json& msg, int timeout_ms, std::string& error)
    {
        if (!host.channel.read_json(msg, timeout_ms, error))
            return false;
        {
            std::function<void(nlohmann::json&)> filter;
            {
                std::lock_guard<std::mutex> lock(g_reply_filter_mutex);
                filter = g_reply_filter_for_testing;
            }
            if (filter)
                filter(msg);
        }
        std::string why;
        if (!validate_child_message(msg, why)) {
            error = "plugin host protocol violation: " + why;
            BOOST_LOG_TRIVIAL(error) << error;
            return false;
        }
        return true;
    }

    bool prompt_unsandboxed(std::unique_lock<std::mutex>& io,
                            const std::string&            plugin_key,
                            const std::string&            reason,
                            std::string&                  error,
                            bool                          allow_prompt = true)
    {
        if (!allow_prompt) {
            // Background work (lifecycle events) never shows the dialog; it runs only on a choice
            // the user already made for these files.
            if (unsandboxed_run_already_allowed(plugin_key))
                return true;
            error = "the OS sandbox is unavailable and the plugin was not allowed to run without it";
            return false;
        }
        io.unlock();
        bool       applied = false;
        const bool ok      = allow_unsandboxed_run(plugin_key, reason, applied, error);
        io.lock();
        if (stopping.load(std::memory_order_acquire)) {
            error = "plugin host is shutting down";
            return false;
        }
        return ok;
    }

    bool ensure_started_locked(std::unique_lock<std::mutex>& io, const SandboxSpec& spec, std::string& error, bool allow_prompt = true)
    {
        if (stopping.load(std::memory_order_acquire)) {
            error = "plugin host is shutting down";
            return false;
        }
        const std::string current_data = Slic3r::data_dir();
        const std::string current_res  = Slic3r::resources_dir();
        if (host.valid() && (current_data != data_dir || current_res != resources_dir || !sandbox_specs_equal(sandbox_spec, spec)))
            reset_child_locked();
        if (host.valid())
            return true;

        // H4: fail closed. A missing or unsigned helper stops the run here with a clear message
        // (before any "Run anyway" prompt: that override is for a missing OS sandbox only).
        {
            std::string why;
            if (!helper_runnable(why)) {
                error = why;
                BOOST_LOG_TRIVIAL(error) << error;
                return false;
            }
        }

#ifndef __APPLE__
        // macOS: the parent is never in the helper's App Sandbox, so probe_os_sandbox() is
        // always false there and would prompt every user before spawn. Handshake is the
        // source of truth (child reports kind=app-sandbox via sandbox_check).
        if (!os_sandbox_available() && !sandbox_env_disable_requested()) {
            if (!prompt_unsandboxed(io, spec.plugin_key, probe_os_sandbox().reason, error, allow_prompt))
                return false;
        }
#endif

        if (!spawn_host(host, spec, error)) {
            error = "The plugin helper (edge-plugin-host) could not start, so the plugin did not run: " + error;
            return false;
        }
        g_spawn_count.fetch_add(1, std::memory_order_relaxed);
        live_pid.store(host.pid, std::memory_order_release);
#ifdef _WIN32
        live_process.store(host.process, std::memory_order_release);
#endif
        data_dir      = current_data;
        resources_dir = current_res;
        sandbox_spec  = spec;

        nlohmann::json hello = {
            {"type", k_type_hello},           {"protocol_version", k_protocol_version}, {"data_dir", data_dir},
            {"resources_dir", resources_dir}, {"sandbox", sandbox_spec_to_json(spec)},
        };
        if (!host.channel.write_json(hello, k_handshake_timeout_ms, error)) {
            enrich_host_error(host, error);
            reset_child_locked();
            return false;
        }
        nlohmann::json reply;
        if (!read_child(reply, k_handshake_timeout_ms, error)) {
            enrich_host_error(host, error);
            reset_child_locked();
            return false;
        }
        if (reply["type"].get<std::string>() != k_type_hello_ok) {
            error = "plugin host handshake failed";
            reset_child_locked();
            return false;
        }
        const int remote = static_cast<int>(reply["protocol_version"].get<std::int64_t>());
        if (!handshake_versions_compatible(remote, error)) {
            reset_child_locked();
            return false;
        }
        sandbox_report = sandbox_report_from_json(reply.contains("sandbox") ? reply["sandbox"] : nlohmann::json::object());
        if (sandbox_report.degraded)
            BOOST_LOG_TRIVIAL(warning) << "plugin host OS sandbox is degraded: " << sandbox_report.detail;
        // M8: do not take the child's word for it.
        {
            std::string unconfirmed;
            if (!verify_child_sandbox(host, sandbox_report, unconfirmed)) {
                BOOST_LOG_TRIVIAL(error) << "plugin host sandbox not confirmed by EdgeSlicer: " << unconfirmed;
                sandbox_report.applied = false;
                sandbox_report.kind    = k_sandbox_kind_none;
                sandbox_report.detail  = unconfirmed;
            }
        }
        // M2: Landlock below ABI 3 leaves truncate() unhandled: not a shipping sandbox.
        const bool weak_landlock = sandbox_report.kind == k_sandbox_kind_landlock_seccomp && sandbox_report.landlock_abi < 3;
        const bool shipping = sandbox_report.applied && sandbox_kind_is_shipping(sandbox_report.kind) && !weak_landlock;
        if (!shipping && !sandbox_env_disable_requested()) {
            const std::string reason = sandbox_report.detail.empty() ? std::string("plugin host reported no shipping OS sandbox") :
                                                                       sandbox_report.detail;
            if (!prompt_unsandboxed(io, spec.plugin_key, reason, error, allow_prompt)) {
                reset_child_locked();
                return false;
            }
        }
        return true;
    }

    void reset_child_locked()
    {
        // Drop the published pid/handle before reap so a recycled pid (or a
        // reused HANDLE) cannot be killed by a concurrent cancel.
        live_pid.store(0, std::memory_order_release);
#ifdef _WIN32
        live_process.store(nullptr, std::memory_order_release);
#endif
        if (host.valid()) {
            kill_host(host);
            close_host(host);
        }
        loaded.clear();
    }

    // Imports plugin_key in the helper (once per helper process). With `capabilities`, the import
    // is always asked for again (the helper answers from its already-loaded copy) and the
    // capability list the helper returns is parsed strictly into it (D3). The helper may relay
    // the plugin's load-time permission request and audit checks while EdgeSlicer waits.
    bool load_plugin_locked(const std::string& plugin_key, std::string& error, std::vector<HostedCapabilityInfo>* capabilities = nullptr)
    {
        if (capabilities == nullptr && loaded.count(plugin_key))
            return true;
        nlohmann::json msg = {
            {"type", k_type_load},
            {"id", next_id.fetch_add(1, std::memory_order_relaxed)},
        };
        attach_plugin_identity(msg, plugin_key);
        const std::uint64_t id         = msg["id"].get<std::uint64_t>();
        const int           timeout_ms = g_execute_timeout_ms.load();
        if (!host.channel.write_json(msg, timeout_ms, error)) {
            reset_child_locked();
            return false;
        }
        nlohmann::json reply;
        if (!pump_until_result(plugin_key, std::string(), id, timeout_ms, reply, error, k_type_load_ok, /*during_load=*/true)) {
            if (error.empty())
                error = "plugin host load failed";
            reset_child_locked();
            return false;
        }
        if (capabilities != nullptr) {
            std::string why;
            if (!parse_hosted_capabilities(reply, *capabilities, why)) {
                error = "plugin host protocol violation: " + why;
                BOOST_LOG_TRIVIAL(error) << error;
                capabilities->clear();
                reset_child_locked();
                return false;
            }
        }
        loaded.insert(plugin_key);
        return true;
    }

    bool pump_until_result(const std::string& plugin_key,
                           const std::string& capability,
                           std::uint64_t      expected_id,
                           int                timeout_ms,
                           nlohmann::json&    result,
                           std::string&       error,
                           const char*        ok_type           = k_type_result,
                           bool               during_load       = false,
                           bool*              child_error_reply = nullptr)
    {
        if (child_error_reply != nullptr)
            *child_error_reply = false;
        const auto deadline = timeout_ms < 0 ? (std::chrono::steady_clock::time_point::max)() :
                                               std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (is_cancelled_locked(expected_id)) {
                    error = "cancelled";
                    return false;
                }
            }
            int wait_ms = -1;
            if (timeout_ms >= 0) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0) {
                    error = "plugin host timed out";
                    return false;
                }
                wait_ms = static_cast<int>(std::min<std::int64_t>(left.count(), k_pump_poll_ms));
            } else {
                wait_ms = k_pump_poll_ms;
            }
            nlohmann::json msg;
            if (!read_child(msg, wait_ms, error)) {
                if (error.rfind("plugin host protocol violation", 0) == 0)
                    return false;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    if (is_cancelled_locked(expected_id)) {
                        error = "cancelled";
                        return false;
                    }
                }
                const bool wait_expired = error == "truncated IPC message" || error == "IPC write timed out" ||
                                          error.find("timed out") != std::string::npos;
                if (wait_expired && timeout_ms >= 0 && std::chrono::steady_clock::now() < deadline) {
                    error.clear();
                    continue;
                }
                if (timeout_ms >= 0) {
                    const bool deadline_hit = std::chrono::steady_clock::now() >= deadline;
                    if (deadline_hit || wait_expired)
                        error = "plugin host timed out";
                }
                return false;
            }
            const std::string type = msg["type"].get<std::string>();
            if (type == k_type_host_call) {
                HostCall call;
                call.plugin_key        = msg.value("plugin_key", plugin_key);
                call.capability        = msg.value("capability", capability);
                call.method            = msg.value("method", std::string());
                call.args              = msg.value("args", nlohmann::json::object());
                call.during_load       = during_load;
                const HostResult hr    = handle_host_call(call);
                nlohmann::json   reply = {
                    {"type", k_type_host_result},
                    {"id", msg.value("id", static_cast<std::uint64_t>(0))},
                    {"ok", hr.ok},
                };
                if (hr.ok)
                    reply["value"] = hr.value;
                else
                    reply["error"] = hr.error;
                if (!host.channel.write_json(reply, wait_ms, error))
                    return false;
                continue;
            }
            if (type == k_type_audit_check) {
                const std::string        event_name = msg.value("event", std::string());
                const bool               is_write   = msg.value("is_write", false);
                std::vector<std::string> targets;
                if (msg.contains("targets") && msg["targets"].is_array()) {
                    for (const auto& t : msg["targets"])
                        if (t.is_string())
                            targets.push_back(t.get<std::string>());
                }
                bool        allowed = true;
                std::string reason;
                if (PluginAuditManager::instance().current_plugin() != plugin_key) {
                    allowed = false;
                    reason  = "audit_check plugin_key does not match the active Edge audit context";
                } else if (targets.empty()) {
                    allowed = false;
                    reason  = "audit_check missing targets";
                } else {
                    for (const std::string& target : targets) {
                        const AuditDecision d =
                            PluginAuditManager::instance().check_hosted_path_access(boost::filesystem::path(target), is_write);
                        if (!d.allowed) {
                            allowed = false;
                            reason  = d.reason;
                            break;
                        }
                    }
                }
                (void) event_name;
                nlohmann::json reply = {
                    {"type", k_type_audit_result},
                    {"id", msg.value("id", static_cast<std::uint64_t>(0))},
                    {"ok", allowed},
                    {"reason", reason},
                };
                if (!host.channel.write_json(reply, wait_ms, error))
                    return false;
                continue;
            }
            if (type == ok_type || type == k_type_error) {
                const auto got = msg.value("id", static_cast<std::uint64_t>(0));
                if (expected_id != 0 && got != expected_id) {
                    error = "plugin host request id mismatch";
                    return false;
                }
                if (type == ok_type) {
                    result = std::move(msg);
                    return true;
                }
                // A well-formed error reply to this very request: the channel is still in step.
                if (child_error_reply != nullptr && got == expected_id)
                    *child_error_reply = true;
                error = msg.value("message", std::string("plugin host error"));
                return false;
            }
            error = "unexpected plugin host message: " + type;
            return false;
        }
    }

    void run_unload(Job& job)
    {
        std::unique_lock<std::mutex> io(io_mutex);
        loaded.erase(job.plugin_key);
        if (!host.valid())
            return;
        std::string    error;
        nlohmann::json msg = {
            {"type", k_type_unload},
            {"id", next_id.fetch_add(1, std::memory_order_relaxed)},
            {"plugin_key", job.plugin_key},
        };
        const std::uint64_t id = msg["id"].get<std::uint64_t>();
        if (!host.channel.write_json(msg, 5000, error)) {
            reset_child_locked();
            return;
        }
        nlohmann::json reply;
        if (!read_child(reply, 5000, error)) {
            reset_child_locked();
            return;
        }
        const auto got = reply.value("id", static_cast<std::uint64_t>(0));
        if (got != id)
            reset_child_locked();
    }

    // Sends plugin_key's queued lifecycle events as one batch, in the order they were fired.
    // Never prompts. Anything but the matching lifecycle_event_ok (or a well-formed error reply
    // to this batch) resets the helper, so the channel never goes out of step. The batch is then
    // lost: events are fire-and-forget, and nothing in EdgeSlicer waits for them.
    void run_events(Job& job)
    {
        std::deque<LifecycleEventSnapshot> batch;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            const auto                  it = event_queues.find(job.plugin_key);
            if (it == event_queues.end())
                return;
            batch.swap(it->second.events);
            it->second.drain_queued    = false;
            it->second.overflow_logged = false;
        }
        if (batch.empty() || stopping.load(std::memory_order_acquire))
            return;

        // Only a plugin that is still loaded, through its enabled listeners.
        const std::vector<std::string> listeners = PluginHostClient::lifecycle_listeners(job.plugin_key);
        if (listeners.empty())
            return;

        ScopedPluginAuditContext audit(job.plugin_key, std::string());
        nlohmann::json           events   = nlohmann::json::array();
        std::size_t              filtered = 0;
        for (const LifecycleEventSnapshot& event : batch) {
            if (PluginHostClient::lifecycle_event_visible_to(job.plugin_key, event))
                events.push_back(lifecycle_event_to_json(event));
            else
                ++filtered;
        }
        if (filtered > 0)
            BOOST_LOG_TRIVIAL(info) << "plugin " << job.plugin_key << ": " << filtered
                                    << " lifecycle event(s) not sent: they name a path its permissions do not cover";
        if (events.empty())
            return;

        // The plugin's own storage folder (host.plugin.storage()), as for a script run.
        try {
            PluginAuditManager::instance().add_scoped_allowed_root(
                boost::filesystem::path(PluginManager::instance().get_storage_dir(job.plugin_key)));
        } catch (...) {}

        std::unique_lock<std::mutex> io(io_mutex);
        const SandboxSpec            spec       = make_sandbox_spec(job.plugin_key, std::string());
        const int                    timeout_ms = g_event_timeout_ms.load(std::memory_order_acquire);
        std::string                  error;
        for (int attempt = 0; attempt < 2; ++attempt) {
            error.clear();
            if (!ensure_started_locked(io, spec, error, /*allow_prompt=*/false))
                break;
            // A failed import or write means the helper never saw this batch: one more try on a
            // fresh helper (load_plugin_locked and the reset below both drop the old one).
            if (!load_plugin_locked(job.plugin_key, error))
                continue;
            nlohmann::json msg = {
                {"type", k_type_event},
                {"id", next_id.fetch_add(1, std::memory_order_relaxed)},
                {"capabilities", listeners},
                {"events", events},
            };
            attach_plugin_identity(msg, job.plugin_key);
            const std::uint64_t id = msg["id"].get<std::uint64_t>();
            if (!host.channel.write_json(msg, timeout_ms, error)) {
                reset_child_locked();
                continue;
            }
            // From here at most once: the helper may already have run some handlers.
            nlohmann::json reply;
            bool           child_error = false;
            if (pump_until_result(job.plugin_key, std::string(), id, timeout_ms, reply, error, k_type_event_ok, false, &child_error)) {
                const auto failed = reply.value("failed", static_cast<std::uint64_t>(0));
                if (failed > 0)
                    BOOST_LOG_TRIVIAL(warning) << "plugin " << job.plugin_key << ": " << failed << " on_lifecycle_event call(s) raised";
                return;
            }
            if (!child_error) {
                enrich_host_error(host, error);
                reset_child_locked();
            }
            break;
        }
        BOOST_LOG_TRIVIAL(warning) << "plugin " << job.plugin_key << ": " << events.size()
                                   << " lifecycle event(s) not delivered: " << error;
    }

    void run_job(Job& job)
    {
        if (job.kind == Job::Kind::Unload) {
            run_unload(job);
            return;
        }
        if (job.kind == Job::Kind::Events) {
            run_events(job);
            return;
        }
        std::string error;
        bool        cancelled_before_io = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            running_plugin_key  = job.plugin_key;
            running_request_id  = job.request_id;
            cancelled_before_io = is_cancelled_locked(job.request_id);
        }
        if (cancelled_before_io) {
            fail_job(job, "cancelled", true);
            return;
        }
        // The helper's host calls and audit checks (during the import too) are answered for this
        // plugin only: set its audit context before anything is sent.
        ScopedPluginAuditContext     audit(job.plugin_key, job.capability_name);
        std::unique_lock<std::mutex> io(io_mutex);
        const SandboxSpec            spec = make_sandbox_spec(job.plugin_key, job.write_root);
        if (job.kind == Job::Kind::Load) {
            std::vector<HostedCapabilityInfo> capabilities;
            if (!ensure_started_locked(io, spec, error) || !load_plugin_locked(job.plugin_key, error, &capabilities)) {
                io.unlock();
                fail_job(job, error, false);
                return;
            }
            io.unlock();
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                active.erase(job.plugin_key);
                if (running_request_id == job.request_id) {
                    running_plugin_key.clear();
                    running_request_id = 0;
                }
            }
            job.finished = true;
            if (job.load_promise)
                job.load_promise->set_value({std::move(capabilities), std::string()});
            return;
        }
        if (!ensure_started_locked(io, spec, error) || !load_plugin_locked(job.plugin_key, error)) {
            io.unlock();
            fail_job(job, error, false);
            return;
        }
        bool cancelled_after_load = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            cancelled_after_load = is_cancelled_locked(job.request_id);
        }
        if (cancelled_after_load) {
            io.unlock();
            fail_job(job, "cancelled", true);
            return;
        }

        // Post-process G-code is inlined over IPC. The helper stages a scratch copy under
        // plugin_data; grant that folder to the parent audit (the RPC check runs here) and
        // pin writes there so a write outside plugin_data names ctx.gcode_path.
        struct WritePin
        {
            bool armed = false;
            ~WritePin()
            {
                if (armed)
                    PluginAuditManager::instance().clear_write_pin();
            }
        } write_pin;
        if (job.kind == Job::Kind::PostProcess) {
            try {
                const auto storage = boost::filesystem::path(PluginManager::instance().get_storage_dir(job.plugin_key));
                PluginAuditManager::instance().add_scoped_allowed_root(storage);
                PluginAuditManager::instance().pin_writes_to(storage,
                                                             "write outside plugin_data; hosted post-process G-code is ctx.gcode_path");
                write_pin.armed = true;
            } catch (...) {}
        }

        if (!host.channel.write_json(job.msg, job.timeout_ms, error)) {
            reset_child_locked();
            io.unlock();
            fail_job(job, error, false);
            return;
        }

        nlohmann::json reply;
        const bool     ok        = pump_until_result(job.plugin_key, job.capability_name, job.request_id, job.timeout_ms, reply, error);
        bool           cancelled = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            cancelled = is_cancelled_locked(job.request_id);
        }
        if (!ok) {
            if (host_exceeded_job_memory_limit(host))
                error = "plugin host exceeded the 1 GiB job memory limit";
            else
                enrich_host_error(host, error);
            reset_child_locked();
            if (cancelled &&
                (error == "closed pipe" || error.find("timed out") != std::string::npos || error.find("cancelled") != std::string::npos))
                error = "cancelled";
            io.unlock();
            fail_job(job, error, cancelled);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            active.erase(job.plugin_key);
            if (running_request_id == job.request_id) {
                running_plugin_key.clear();
                running_request_id = 0;
            }
            if (is_cancelled_locked(job.request_id))
                consume_cancel_locked(job.request_id);
        }
        if (job.kind == Job::Kind::Script) {
            ScriptCompletion completion;
            completion.result.status  = static_cast<PluginResult>(reply.value("status", 0));
            job.finished              = true;
            completion.result.message = reply.value("message", std::string());
            completion.result.data    = reply.value("data", std::string());
            completion.cancelled      = reply.value("cancelled", false) || cancelled;
            io.unlock();
            deliver_completion(job.on_done, completion);
        } else {
            ExecutionResult result;
            result.status  = static_cast<PluginResult>(reply.value("status", 0));
            result.message = reply.value("message", std::string());
            result.data    = reply.value("data", std::string());
            if (!job.gcode_path.empty() && reply.contains("gcode") && reply["gcode"].is_string()) {
                boost::nowide::ofstream out(job.gcode_path, std::ios::binary | std::ios::trunc);
                if (!out) {
                    io.unlock();
                    fail_job(job, "failed to write post-process G-code", false);
                    return;
                }
                const std::string body = reply["gcode"].get<std::string>();
                out.write(body.data(), static_cast<std::streamsize>(body.size()));
                out.close();
#ifdef _WIN32
                // Parent write of hosted G-code that left plugin_data. A copy/move of a
                // helper-created file would keep a Low mandatory label; strip it (copy-replace
                // when WRITE_OWNER is missing).
                win_clear_low_integrity_label(job.gcode_path);
#endif
            }
            io.unlock();
            job.finished = true;
            if (job.post_promise)
                job.post_promise->set_value({result, std::string()});
        }
    }

    void worker_loop()
    {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(state_mutex);
                cv.wait(lock, [&] { return stopping.load(std::memory_order_acquire) || !queue.empty(); });
                if (stopping.load(std::memory_order_acquire) && queue.empty())
                    break;
                if (queue.empty())
                    continue;
                job = std::move(queue.front());
                queue.pop_front();
                // An event batch is never a cancel target (cancel() binds to a script or
                // post-process request id).
                if (job && job->kind != Job::Kind::Unload && job->kind != Job::Kind::Events) {
                    running_plugin_key = job->plugin_key;
                    running_request_id = job->request_id;
                }
                if (stopping.load(std::memory_order_acquire)) {
                    lock.unlock();
                    fail_job(*job, "plugin host is shutting down", false);
                    if (job->kind == Job::Kind::Events)
                        event_job_done();
                    continue;
                }
            }
            run_job_contained(*job);
            if (job->kind == Job::Kind::Events)
                event_job_done();
        }
    }

    // H2: nothing that comes back from the helper may end the app. Any exception from a job
    // (a malformed reply that slipped past the schema check, a host RPC that threw, an
    // allocation failure) kills and resets the helper and fails this run with the reason.
    void run_job_contained(Job& job)
    {
        std::string failure;
        try {
            {
                std::function<void()> hook;
                {
                    std::lock_guard<std::mutex> lock(g_job_hook_mutex);
                    hook = g_job_hook_for_testing;
                }
                if (hook)
                    hook();
            }
            run_job(job);
            return;
        } catch (const std::exception& ex) {
            failure = ex.what();
        } catch (...) {
            failure = "unknown error";
        }
        BOOST_LOG_TRIVIAL(error) << "plugin host job for " << job.plugin_key << " failed: " << failure;
        try {
            std::unique_lock<std::mutex> io(io_mutex, std::try_to_lock);
            if (!io.owns_lock())
                kill_live();
            else
                reset_child_locked();
        } catch (...) {}
        try {
            fail_job(job, "plugin host error: " + failure, false);
        } catch (...) {}
        if (job.kind == Job::Kind::Events) {
            // The next post queues a fresh drain for whatever is still waiting.
            std::lock_guard<std::mutex> lock(state_mutex);
            const auto                  it = event_queues.find(job.plugin_key);
            if (it != event_queues.end())
                it->second.drain_queued = false;
        }
    }
};

PluginHostClient& PluginHostClient::instance()
{
    static PluginHostClient inst;
    return inst;
}

PluginHostClient::Impl& PluginHostClient::impl()
{
    if (!m_impl)
        m_impl = std::make_shared<Impl>();
    return *m_impl;
}

PluginHostClient::~PluginHostClient()
{
    try {
        shutdown();
    } catch (...) {}
}

void PluginHostClient::set_force_in_process_for_testing(bool force) { g_force_in_process.store(force, std::memory_order_release); }

void PluginHostClient::set_execute_timeout_for_testing(int timeout_ms)
{
    g_execute_timeout_ms.store(timeout_ms > 0 ? timeout_ms : k_default_timeout_ms, std::memory_order_release);
}

void PluginHostClient::set_allow_env_overrides_for_testing(bool allow) { set_allow_helper_env_overrides_for_testing(allow); }

void PluginHostClient::reset_spawn_count_for_testing() { g_spawn_count.store(0, std::memory_order_release); }

void PluginHostClient::set_job_hook_for_testing(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lock(g_job_hook_mutex);
    g_job_hook_for_testing = std::move(hook);
}

std::uint64_t PluginHostClient::spawn_count() { return g_spawn_count.load(std::memory_order_acquire); }

bool PluginHostClient::should_use_host()
{
    // The helper itself runs the plugin code it was sent.
    if (in_child_process())
        return false;
    // Tests only: set_force_in_process_for_testing, or EDGE_PLUGIN_HOST_IN_PROCESS once a test
    // enabled helper env overrides. Release code never sets either.
    if (g_force_in_process.load(std::memory_order_acquire))
        return false;
    if (helper_env_overrides_allowed()) {
        if (const char* env = boost::nowide::getenv("EDGE_PLUGIN_HOST_IN_PROCESS")) {
            if (env[0] != '\0' && env[0] != '0') {
                log_host_fallback_once("EDGE_PLUGIN_HOST_IN_PROCESS is set (test override)");
                return false;
            }
        }
    }
    // H4: always the helper. A missing, unsigned or unstartable helper fails the run (fail
    // closed, see host_unavailable_reason()); plugin code never falls back to running inside
    // EdgeSlicer.
    return true;
}

std::string PluginHostClient::host_unavailable_reason()
{
    if (in_child_process())
        return {};
    std::string reason;
    if (!helper_runnable(reason))
        return reason;
    return {};
}

bool PluginHostClient::host_fallback_active() { return !host_unavailable_reason().empty(); }

bool PluginHostClient::start_script(const std::string&                           plugin_key,
                                    const std::string&                           capability_name,
                                    std::function<void(const ScriptCompletion&)> on_done)
{
    Impl& i              = impl();
    auto  job            = std::make_shared<Job>();
    job->kind            = Job::Kind::Script;
    job->plugin_key      = plugin_key;
    job->capability_name = capability_name;
    job->timeout_ms      = execute_timeout_ms(false, {});
    job->on_done         = std::move(on_done);
    job->request_id      = i.next_id.fetch_add(1, std::memory_order_relaxed);
    job->msg             = {
        {"type", k_type_execute},        {"id", job->request_id}, {"kind", k_kind_script}, {"capability_name", capability_name},
        {"timeout_ms", job->timeout_ms},
    };
    attach_plugin_identity(job->msg, plugin_key);
    {
        std::lock_guard<std::mutex> lock(i.state_mutex);
        if (i.stopping.load(std::memory_order_acquire))
            return false;
        i.active.insert(plugin_key);
        i.queue.push_back(job);
        i.ensure_worker_locked();
        i.cv.notify_one();
    }
    return true;
}

ExecutionResult PluginHostClient::execute_post_process(const std::string&            plugin_key,
                                                       const std::string&            capability_name,
                                                       const SlicingPipelineContext& ctx,
                                                       std::string&                  error)
{
    Impl& i              = impl();
    auto  job            = std::make_shared<Job>();
    job->kind            = Job::Kind::PostProcess;
    job->plugin_key      = plugin_key;
    job->capability_name = capability_name;
    job->timeout_ms      = execute_timeout_ms(true, ctx.gcode_path);
    job->gcode_path      = ctx.gcode_path;
    job->post_promise    = std::make_shared<std::promise<std::pair<ExecutionResult, std::string>>>();
    job->request_id      = i.next_id.fetch_add(1, std::memory_order_relaxed);
    job->msg             = {
        {"type", k_type_execute},           {"id", job->request_id},
        {"kind", k_kind_post_process},      {"capability_name", capability_name},
        {"timeout_ms", job->timeout_ms},    {"host", ctx.host},
        {"output_name", ctx.output_name},   {"config", config_snapshot(ctx.full_config)},
        {"orca_version", ctx.orca_version},
    };
    if (!ctx.gcode_path.empty()) {
        boost::system::error_code ec;
        const auto                sz = boost::filesystem::file_size(ctx.gcode_path, ec);
        if (ec) {
            error = "failed to read G-code for plugin host: " + ec.message();
            return ExecutionResult::failure(PluginResult::FatalError, error);
        }
        constexpr uint32_t k_gcode_ipc_headroom = 1024u * 1024u;
        if (sz > static_cast<uintmax_t>(k_max_payload_bytes - k_gcode_ipc_headroom)) {
            error = "G-code file exceeds the 16 MiB plugin-host IPC payload";
            return ExecutionResult::failure(PluginResult::FatalError, error);
        }
        boost::nowide::ifstream in(ctx.gcode_path, std::ios::binary);
        if (!in) {
            error = "failed to open G-code for plugin host";
            return ExecutionResult::failure(PluginResult::FatalError, error);
        }
        std::string body;
        body.resize(static_cast<size_t>(sz));
        if (sz > 0 && !in.read(body.data(), static_cast<std::streamsize>(sz))) {
            error = "failed to read G-code for plugin host";
            return ExecutionResult::failure(PluginResult::FatalError, error);
        }
        job->msg["gcode"] = std::move(body);
    }
    attach_plugin_identity(job->msg, plugin_key);
    auto future = job->post_promise->get_future();
    {
        std::lock_guard<std::mutex> lock(i.state_mutex);
        if (i.stopping.load(std::memory_order_acquire)) {
            error = "plugin host is shutting down";
            return ExecutionResult::failure(PluginResult::FatalError, error);
        }
        i.active.insert(plugin_key);
        i.queue.push_back(job);
        i.ensure_worker_locked();
        i.cv.notify_one();
    }
    try {
        auto [result, err] = future.get();
        error              = err;
        return result;
    } catch (const std::exception& ex) {
        error = ex.what();
        return ExecutionResult::failure(PluginResult::FatalError, error);
    }
}

bool PluginHostClient::load_in_helper(const std::string& plugin_key, std::vector<HostedCapabilityInfo>& capabilities, std::string& error)
{
    capabilities.clear();
    error.clear();
    if (plugin_key.empty()) {
        error = "plugin_key missing";
        return false;
    }
    Impl& i         = impl();
    auto  job       = std::make_shared<Job>();
    job->kind       = Job::Kind::Load;
    job->plugin_key = plugin_key;
    job->timeout_ms = execute_timeout_ms(false, {});
    job->request_id = i.next_id.fetch_add(1, std::memory_order_relaxed);
    job->load_promise = std::make_shared<std::promise<HostedLoadOutcome>>();
    auto future       = job->load_promise->get_future();
    {
        std::lock_guard<std::mutex> lock(i.state_mutex);
        if (i.stopping.load(std::memory_order_acquire)) {
            error = "plugin host is shutting down";
            return false;
        }
        i.queue.push_back(job);
        i.ensure_worker_locked();
        i.cv.notify_one();
    }
    try {
        HostedLoadOutcome outcome = future.get();
        if (!outcome.second.empty()) {
            error = std::move(outcome.second);
            return false;
        }
        capabilities = std::move(outcome.first);
        if (capabilities.empty()) {
            error = "the plugin registered no capabilities";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

void PluginHostClient::post_lifecycle_event(const std::string& plugin_key, const LifecycleEventSnapshot& snapshot)
{
    try {
        if (plugin_key.empty())
            return;
        Impl&                       i = impl();
        std::lock_guard<std::mutex> lock(i.state_mutex);
        if (i.stopping.load(std::memory_order_acquire))
            return;
        Impl::PendingEvents& pending = i.event_queues[plugin_key];
        // Drop policy: the oldest waiting event goes, so a listener sees the latest state. One
        // log line per overflow; the next batch sent re-arms it.
        while (pending.events.size() >= k_max_event_queue) {
            pending.events.pop_front();
            ++pending.dropped;
            if (!pending.overflow_logged) {
                pending.overflow_logged = true;
                BOOST_LOG_TRIVIAL(warning) << "plugin " << plugin_key << ": lifecycle event queue full (" << k_max_event_queue
                                           << "), dropping the oldest events until the helper catches up";
            }
        }
        pending.events.push_back(snapshot);
        if (pending.drain_queued)
            return;
        auto job        = std::make_shared<Job>();
        job->kind       = Job::Kind::Events;
        job->plugin_key = plugin_key;
        job->request_id = 0; // never a cancel target
        pending.drain_queued = true;
        ++i.events_outstanding;
        i.queue.push_back(std::move(job));
        i.ensure_worker_locked();
        i.cv.notify_one();
    } catch (...) {
        // Never let an event reach the caller as an exception (allocation failure, thread start).
    }
}

std::vector<std::string> PluginHostClient::lifecycle_listeners(const std::string& plugin_key)
{
    std::vector<std::string> names;
    PluginManager&           manager = PluginManager::instance();
    if (plugin_key.empty() || !manager.is_plugin_loaded(plugin_key))
        return names;
    for (const auto& cap : manager.get_plugin_capabilities(plugin_key, PluginCapabilityType::Unknown, /*only_enabled=*/true)) {
        if (!cap || !cap->is_enabled())
            continue;
        const plugin_import::HostedCapability* hosted = plugin_import::as_hosted(cap.get());
        if (hosted != nullptr && hosted->hosted_info().lifecycle_events)
            names.push_back(cap->name());
    }
    return names;
}

bool PluginHostClient::lifecycle_event_visible_to(const std::string& plugin_key, const LifecycleEventSnapshot& snapshot)
{
    const std::vector<std::string> paths = lifecycle_snapshot_paths(snapshot);
    if (paths.empty())
        return true;
    if (plugin_key.empty())
        return false;
    // The audit checks below answer for the plugin in the current audit context.
    ScopedPluginAuditContext scope(plugin_key, std::string());
    bool              full_access = false;
    PluginInstallState state;
    if (PluginManager::instance().get_install_state(plugin_key, state))
        full_access = state.permissions.full_access;
    PluginAuditManager& audit = PluginAuditManager::instance();
    for (const std::string& raw : paths) {
        std::string text = raw;
        if (text.size() >= 5 && (text.compare(0, 5, "file:") == 0 || text.compare(0, 5, "FILE:") == 0)) {
            text.erase(0, 5);
            // file:///C:/x -> C:/x, file:///home/x -> /home/x
            if (text.size() >= 2 && text.compare(0, 2, "//") == 0)
                text.erase(0, 2);
            if (text.size() >= 3 && text[0] == '/' && std::isalpha(static_cast<unsigned char>(text[1])) && text[2] == ':')
                text.erase(0, 1);
        }
        boost::filesystem::path path;
        try {
            path = boost::filesystem::path(text);
        } catch (...) {
            return false;
        }
        // Secret stores (hub tokens, access codes, sign-in data, app config) never, whatever the grants.
        if (audit.is_denied_path(path))
            return false;
        if (full_access)
            continue;
        if (!audit.check_hosted_path_access(path, /*is_write=*/false).allowed)
            return false;
    }
    return true;
}

std::size_t PluginHostClient::pending_lifecycle_events(const std::string& plugin_key) const
{
    if (!m_impl)
        return 0;
    std::lock_guard<std::mutex> lock(m_impl->state_mutex);
    const auto                  it = m_impl->event_queues.find(plugin_key);
    return it == m_impl->event_queues.end() ? 0 : it->second.events.size();
}

std::vector<std::string> PluginHostClient::pending_lifecycle_event_names_for_testing(const std::string& plugin_key) const
{
    std::vector<std::string> names;
    if (!m_impl)
        return names;
    std::lock_guard<std::mutex> lock(m_impl->state_mutex);
    const auto                  it = m_impl->event_queues.find(plugin_key);
    if (it != m_impl->event_queues.end())
        for (const LifecycleEventSnapshot& e : it->second.events)
            names.push_back(e.name);
    return names;
}

std::uint64_t PluginHostClient::lifecycle_events_dropped(const std::string& plugin_key) const
{
    if (!m_impl)
        return 0;
    std::lock_guard<std::mutex> lock(m_impl->state_mutex);
    const auto                  it = m_impl->event_queues.find(plugin_key);
    return it == m_impl->event_queues.end() ? 0 : it->second.dropped;
}

bool PluginHostClient::wait_lifecycle_events_idle_for_testing(std::chrono::milliseconds timeout)
{
    Impl&                        i = impl();
    std::unique_lock<std::mutex> lock(i.state_mutex);
    return i.events_idle_cv.wait_for(lock, timeout, [&i] { return i.events_outstanding == 0; });
}

void PluginHostClient::set_event_timeout_for_testing(int timeout_ms)
{
    g_event_timeout_ms.store(timeout_ms > 0 ? timeout_ms : k_event_timeout_ms, std::memory_order_release);
}

void PluginHostClient::set_reply_filter_for_testing(std::function<void(nlohmann::json&)> filter)
{
    std::lock_guard<std::mutex> lock(g_reply_filter_mutex);
    g_reply_filter_for_testing = std::move(filter);
}

void PluginHostClient::unload(const std::string& plugin_key)
{
    Impl& i         = impl();
    auto  job       = std::make_shared<Job>();
    job->kind       = Job::Kind::Unload;
    job->plugin_key = plugin_key;
    {
        std::lock_guard<std::mutex> lock(i.state_mutex);
        // Events still waiting for an unloaded plugin are never sent.
        i.event_queues.erase(plugin_key);
        if (plugin_key.empty() || i.active.count(plugin_key))
            return;
        if (i.stopping.load(std::memory_order_acquire))
            return;
        i.queue.push_back(job);
        i.ensure_worker_locked();
        i.cv.notify_one();
    }
}

void PluginHostClient::cancel(const std::string& plugin_key)
{
    Impl&         i          = impl();
    std::uint64_t request_id = 0;
    {
        std::lock_guard<std::mutex> lock(i.state_mutex);
        if (!i.active.count(plugin_key))
            return;
        if (i.running_plugin_key == plugin_key)
            request_id = i.running_request_id;
        if (request_id == 0) {
            for (const auto& job : i.queue) {
                if (job && job->plugin_key == plugin_key && job->request_id != 0) {
                    request_id = job->request_id;
                    break;
                }
            }
        }
        if (request_id == 0)
            return;
        i.cancel_requested.store(true, std::memory_order_release);
        i.cancel_plugin_key = plugin_key;
        i.cancel_request_id = request_id;
    }
    // Do not write a cancel frame: during a pump try_lock always fails, and an idle
    // id-less cancel reply would poison the next job as an id mismatch. Kill by pid
    // (or the stored process handle on Windows) after the grace period. Bind the
    // kill to this request id so a stale cancel cannot hit the next job that reuses
    // the same plugin_key (ASan made that race visible).
    std::thread([plugin_key, request_id, impl = m_impl]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(k_cancel_grace_ms));
        if (!impl)
            return;
        // Hold state_mutex across kill so run_job cannot start a new request_id
        // on the same helper between the check and SIGKILL.
        std::lock_guard<std::mutex> lock(impl->state_mutex);
        if (!impl->cancel_requested.load(std::memory_order_acquire))
            return;
        if (impl->cancel_request_id != request_id)
            return;
        if (impl->running_request_id != request_id)
            return;
        if (impl->running_plugin_key != plugin_key)
            return;
        impl->kill_live();
    }).detach();
}

bool PluginHostClient::has_active_run(const std::string& plugin_key) const
{
    if (!m_impl)
        return false;
    std::lock_guard<std::mutex> lock(m_impl->state_mutex);
    return m_impl->active.count(plugin_key) != 0;
}

bool PluginHostClient::kill_child_for_testing()
{
    Impl& i = impl();
#ifdef _WIN32
    if (i.live_process.load(std::memory_order_acquire) == nullptr)
        return false;
#else
    if (i.live_pid.load(std::memory_order_acquire) <= 0)
        return false;
#endif
    i.kill_live();
    return true;
}

std::int64_t PluginHostClient::child_pid_for_testing() const
{
    if (!m_impl)
        return 0;
    return m_impl->live_pid.load(std::memory_order_acquire);
}

SandboxReport PluginHostClient::last_sandbox_report() const
{
    if (!m_impl)
        return {};
    std::lock_guard<std::mutex> io(m_impl->io_mutex);
    return m_impl->sandbox_report;
}

void PluginHostClient::shutdown()
{
    if (!m_impl)
        return;
    m_impl->stopping.store(true, std::memory_order_release);
    abort_sandbox_unavailable_prompt();
    m_impl->cv.notify_all();
    // Never wait on io_mutex here: the worker holds it for the whole pump. Kill by
    // pid (or the stored process handle on Windows) so a hung execute unblocks,
    // then join, then close leftovers.
    m_impl->kill_live();
    if (m_impl->worker.joinable())
        m_impl->worker.join();
    std::deque<std::shared_ptr<Job>> leftover;
    {
        std::lock_guard<std::mutex> lock(m_impl->state_mutex);
        leftover.swap(m_impl->queue);
        m_impl->event_queues.clear();
        m_impl->active.clear();
        m_impl->running_plugin_key.clear();
        m_impl->running_request_id = 0;
        m_impl->cancel_requested.store(false, std::memory_order_release);
        m_impl->cancel_plugin_key.clear();
        m_impl->cancel_request_id = 0;
    }
    {
        std::lock_guard<std::mutex> io(m_impl->io_mutex);
        if (m_impl->host.valid()) {
            std::string    error;
            nlohmann::json msg = {{"type", k_type_shutdown}};
            (void) m_impl->host.channel.write_json(msg, 200, error);
            m_impl->reset_child_locked();
        } else {
            m_impl->loaded.clear();
        }
    }
    for (auto& job : leftover) {
        m_impl->fail_job(*job, "plugin host is shutting down", false);
        if (job->kind == Job::Kind::Events)
            m_impl->event_job_done();
    }
    m_impl->stopping.store(false, std::memory_order_release);
    clear_sandbox_unavailable_prompt_abort();
}

} // namespace Slic3r::plugin_ipc
