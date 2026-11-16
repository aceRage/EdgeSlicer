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
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginTrust.hpp>
#include <slic3r/plugin/pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <wx/app.h>
#include <wx/thread.h>

namespace Slic3r::plugin_ipc {
namespace {

std::atomic<bool>          g_force_in_process{false};
std::atomic<int>           g_execute_timeout_ms{k_default_timeout_ms};
std::atomic<std::uint64_t> g_spawn_count{0};
std::atomic<bool>          g_logged_fallback{false};

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
}

struct Job
{
    enum class Kind { Script, PostProcess, Unload };
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
        if (job.kind == Job::Kind::Unload)
            return;
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
        } else if (job.post_promise) {
            job.post_promise->set_value({ExecutionResult::failure(PluginResult::FatalError, error), error});
        }
    }

    bool prompt_unsandboxed(std::unique_lock<std::mutex>& io, const std::string& plugin_key, const std::string& reason, std::string& error)
    {
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

    bool ensure_started_locked(std::unique_lock<std::mutex>& io, const SandboxSpec& spec, std::string& error)
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

#ifndef __APPLE__
        // macOS: the parent is never in the helper's App Sandbox, so probe_os_sandbox() is
        // always false there and would prompt every user before spawn. Handshake is the
        // source of truth (child reports kind=app-sandbox via sandbox_check).
        if (!os_sandbox_available() && !sandbox_env_disable_requested()) {
            if (!prompt_unsandboxed(io, spec.plugin_key, probe_os_sandbox().reason, error))
                return false;
        }
#endif

        if (!spawn_host(host, spec, error))
            return false;
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
            reset_child_locked();
            return false;
        }
        nlohmann::json reply;
        if (!host.channel.read_json(reply, k_handshake_timeout_ms, error)) {
            reset_child_locked();
            return false;
        }
        if (reply.value("type", std::string()) != k_type_hello_ok) {
            error = "plugin host handshake failed";
            reset_child_locked();
            return false;
        }
        int remote = reply.value("protocol_version", 0);
        if (!handshake_versions_compatible(remote, error)) {
            reset_child_locked();
            return false;
        }
        sandbox_report      = sandbox_report_from_json(reply.contains("sandbox") ? reply["sandbox"] : nlohmann::json::object());
        if (sandbox_report.degraded)
            BOOST_LOG_TRIVIAL(warning) << "plugin host OS sandbox is degraded: " << sandbox_report.detail;
        const bool shipping = sandbox_report.applied && sandbox_kind_is_shipping(sandbox_report.kind);
        if (!shipping && !sandbox_env_disable_requested()) {
            const std::string reason = sandbox_report.detail.empty() ? std::string("plugin host reported no shipping OS sandbox") :
                                                                       sandbox_report.detail;
            if (!prompt_unsandboxed(io, spec.plugin_key, reason, error)) {
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

    bool load_plugin_locked(const std::string& plugin_key, std::string& error)
    {
        if (loaded.count(plugin_key))
            return true;
        nlohmann::json msg = {
            {"type", k_type_load},
            {"id", next_id.fetch_add(1, std::memory_order_relaxed)},
        };
        attach_plugin_identity(msg, plugin_key);
        const std::uint64_t id = msg["id"].get<std::uint64_t>();
        if (!host.channel.write_json(msg, g_execute_timeout_ms.load(), error)) {
            reset_child_locked();
            return false;
        }
        nlohmann::json reply;
        if (!host.channel.read_json(reply, g_execute_timeout_ms.load(), error)) {
            reset_child_locked();
            return false;
        }
        const auto got = reply.value("id", static_cast<std::uint64_t>(0));
        if (got != id) {
            error = "plugin host request id mismatch";
            reset_child_locked();
            return false;
        }
        if (reply.value("type", std::string()) == k_type_load_ok) {
            loaded.insert(plugin_key);
            return true;
        }
        error = reply.value("message", std::string("plugin host load failed"));
        reset_child_locked();
        return false;
    }

    bool pump_until_result(const std::string& plugin_key,
                           const std::string& capability,
                           std::uint64_t      expected_id,
                           int                timeout_ms,
                           nlohmann::json&    result,
                           std::string&       error)
    {
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
            if (!host.channel.read_json(msg, wait_ms, error)) {
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
            const std::string type = msg.value("type", std::string());
            if (type == k_type_host_call) {
                HostCall call;
                call.plugin_key        = msg.value("plugin_key", plugin_key);
                call.capability        = msg.value("capability", capability);
                call.method            = msg.value("method", std::string());
                call.args              = msg.value("args", nlohmann::json::object());
                const HostResult hr    = handle_host_call(call);
                nlohmann::json   reply = {
                    {"type", k_type_host_result},
                    {"id", msg.value("id", 0)},
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
                        const AuditDecision d = PluginAuditManager::instance().check_path_access(boost::filesystem::path(target), is_write);
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
                    {"id", msg.value("id", 0)},
                    {"ok", allowed},
                    {"reason", reason},
                };
                if (!host.channel.write_json(reply, wait_ms, error))
                    return false;
                continue;
            }
            if (type == k_type_result || type == k_type_error) {
                const auto got = msg.value("id", static_cast<std::uint64_t>(0));
                if (expected_id != 0 && got != expected_id) {
                    error = "plugin host request id mismatch";
                    return false;
                }
                if (type == k_type_result) {
                    result = std::move(msg);
                    return true;
                }
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
        if (!host.channel.read_json(reply, 5000, error)) {
            reset_child_locked();
            return;
        }
        const auto got = reply.value("id", static_cast<std::uint64_t>(0));
        if (got != id)
            reset_child_locked();
    }

    void run_job(Job& job)
    {
        if (job.kind == Job::Kind::Unload) {
            run_unload(job);
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
        std::unique_lock<std::mutex> io(io_mutex);
        const SandboxSpec            spec = make_sandbox_spec(job.plugin_key, job.write_root);
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

        ScopedPluginAuditContext audit(job.plugin_key, job.capability_name);
        // Post-process G-code is inlined over IPC. The helper stages a scratch copy under
        // plugin_data; grant that folder to the parent audit (the RPC check runs here).
        if (job.kind == Job::Kind::PostProcess) {
            try {
                PluginAuditManager::instance().add_scoped_allowed_root(
                    boost::filesystem::path(PluginManager::instance().get_storage_dir(job.plugin_key)));
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
            }
            io.unlock();
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
                if (job && job->kind != Job::Kind::Unload) {
                    running_plugin_key = job->plugin_key;
                    running_request_id = job->request_id;
                }
                if (stopping.load(std::memory_order_acquire)) {
                    lock.unlock();
                    fail_job(*job, "plugin host is shutting down", false);
                    continue;
                }
            }
            run_job(*job);
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

std::uint64_t PluginHostClient::spawn_count() { return g_spawn_count.load(std::memory_order_acquire); }

bool PluginHostClient::should_use_host()
{
    if (in_child_process())
        return false;
    if (g_force_in_process.load(std::memory_order_acquire))
        return false;
    if (helper_env_overrides_allowed()) {
        if (const char* env = boost::nowide::getenv("EDGE_PLUGIN_HOST_IN_PROCESS")) {
            if (env[0] != '\0' && env[0] != '0') {
                log_host_fallback_once("EDGE_PLUGIN_HOST_IN_PROCESS is set");
                return false;
            }
        }
    }
    if (!helper_exists()) {
        log_host_fallback_once("edge-plugin-host is missing");
        return false;
    }
    return true;
}

bool PluginHostClient::host_fallback_active()
{
    if (in_child_process() || g_force_in_process.load(std::memory_order_acquire))
        return false;
    if (helper_env_overrides_allowed()) {
        if (const char* env = boost::nowide::getenv("EDGE_PLUGIN_HOST_IN_PROCESS")) {
            if (env[0] != '\0' && env[0] != '0')
                return true;
        }
    }
    return !helper_exists();
}

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
    job->post_promise = std::make_shared<std::promise<std::pair<ExecutionResult, std::string>>>();
    job->request_id   = i.next_id.fetch_add(1, std::memory_order_relaxed);
    job->msg          = {
        {"type", k_type_execute},
        {"id", job->request_id},
        {"kind", k_kind_post_process},
        {"capability_name", capability_name},
        {"timeout_ms", job->timeout_ms},
        {"host", ctx.host},
        {"output_name", ctx.output_name},
        {"config", config_snapshot(ctx.full_config)},
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

void PluginHostClient::unload(const std::string& plugin_key)
{
    Impl& i         = impl();
    auto  job       = std::make_shared<Job>();
    job->kind       = Job::Kind::Unload;
    job->plugin_key = plugin_key;
    {
        std::lock_guard<std::mutex> lock(i.state_mutex);
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
    for (auto& job : leftover)
        m_impl->fail_job(*job, "plugin host is shutting down", false);
    m_impl->stopping.store(false, std::memory_order_release);
    clear_sandbox_unavailable_prompt_abort();
}

} // namespace Slic3r::plugin_ipc
