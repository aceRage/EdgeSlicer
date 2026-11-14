#include "PluginHostClient.hpp"

#include "PluginHostProcess.hpp"
#include "PluginHostRpc.hpp"
#include "PluginIpcChannel.hpp"
#include "PluginIpcProtocol.hpp"

#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Utils.hpp>
#include <slic3r/GUI/GUI.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginTrust.hpp>
#include <slic3r/plugin/pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp>

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <wx/app.h>
#include <wx/thread.h>

namespace Slic3r::plugin_ipc {
namespace {

std::atomic<bool>        g_force_in_process{false};
std::atomic<int>         g_execute_timeout_ms{k_default_timeout_ms};
std::atomic<std::uint64_t> g_spawn_count{0};

nlohmann::json config_snapshot(const DynamicPrintConfig* config)
{
    nlohmann::json out = nlohmann::json::object();
    if (config == nullptr)
        return out;
    for (const std::string& key : config->keys()) {
        try {
            out[key] = config->opt_serialize(key);
        } catch (...) {
            // Skip options that cannot be serialized; the child treats missing keys as unset.
        }
    }
    return out;
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

} // namespace

struct PluginHostClient::Impl
{
    mutable std::mutex mutex;
    SpawnedHost        host;
    std::string        data_dir;
    std::string        resources_dir;
    std::unordered_set<std::string> loaded;
    std::unordered_set<std::string> active;
    std::atomic<bool>  stopping{false};
    std::uint64_t      next_id{1};

    bool ensure_started_locked(std::string& error)
    {
        if (stopping.load(std::memory_order_acquire)) {
            error = "plugin host is shutting down";
            return false;
        }
        const std::string current_data = Slic3r::data_dir();
        const std::string current_res  = Slic3r::resources_dir();
        if (host.valid() && (current_data != data_dir || current_res != resources_dir)) {
            kill_host(host);
            close_host(host);
            loaded.clear();
        }
        if (host.valid())
            return true;
        if (!spawn_host(host, error))
            return false;
        g_spawn_count.fetch_add(1, std::memory_order_relaxed);
        data_dir      = current_data;
        resources_dir = current_res;

        nlohmann::json hello = {
            {"type", k_type_hello},
            {"protocol_version", k_protocol_version},
            {"data_dir", data_dir},
            {"resources_dir", resources_dir},
        };
        if (!host.channel.write_json(hello, k_handshake_timeout_ms, error)) {
            kill_host(host);
            close_host(host);
            return false;
        }
        nlohmann::json reply;
        if (!host.channel.read_json(reply, k_handshake_timeout_ms, error)) {
            kill_host(host);
            close_host(host);
            return false;
        }
        if (reply.value("type", std::string()) != k_type_hello_ok) {
            error = "plugin host handshake failed";
            kill_host(host);
            close_host(host);
            return false;
        }
        int remote = reply.value("protocol_version", 0);
        if (!handshake_versions_compatible(remote, error)) {
            kill_host(host);
            close_host(host);
            return false;
        }
        return true;
    }

    void reset_child_locked()
    {
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
            {"plugin_key", plugin_key},
        };
        if (!host.channel.write_json(msg, g_execute_timeout_ms.load(), error))
            return false;
        nlohmann::json reply;
        if (!host.channel.read_json(reply, g_execute_timeout_ms.load(), error))
            return false;
        if (reply.value("type", std::string()) == k_type_load_ok) {
            loaded.insert(plugin_key);
            return true;
        }
        error = reply.value("message", std::string("plugin host load failed"));
        return false;
    }

    bool pump_until_result(const std::string& plugin_key,
                           const std::string& capability,
                           int timeout_ms,
                           nlohmann::json& result,
                           std::string& error)
    {
        const auto deadline = timeout_ms < 0
                                  ? std::chrono::steady_clock::time_point::max()
                                  : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            int wait_ms = -1;
            if (timeout_ms >= 0) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0) {
                    error = "plugin host timed out";
                    return false;
                }
                wait_ms = static_cast<int>(std::min<std::int64_t>(left.count(), 1'000'000'000));
            }
            nlohmann::json msg;
            if (!host.channel.read_json(msg, wait_ms, error))
                return false;
            const std::string type = msg.value("type", std::string());
            if (type == k_type_host_call) {
                HostCall call;
                call.plugin_key = msg.value("plugin_key", plugin_key);
                call.capability = msg.value("capability", capability);
                call.method     = msg.value("method", std::string());
                call.args       = msg.value("args", nlohmann::json::object());
                const HostResult hr = handle_host_call(call);
                nlohmann::json reply = {
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
                const std::string event_name = msg.value("event", std::string());
                const bool        is_write   = msg.value("is_write", false);
                std::vector<std::string> targets;
                if (msg.contains("targets") && msg["targets"].is_array()) {
                    for (const auto& t : msg["targets"])
                        if (t.is_string())
                            targets.push_back(t.get<std::string>());
                }
                bool allowed = true;
                std::string reason;
                {
                    ScopedPluginAuditContext audit(plugin_key, capability);
                    for (const std::string& target : targets) {
                        const AuditDecision d =
                            PluginAuditManager::instance().check_path_access(boost::filesystem::path(target), is_write);
                        if (!d.allowed) {
                            allowed = false;
                            reason  = d.reason;
                            break;
                        }
                    }
                    if (allowed && targets.empty())
                        allowed = false, reason = "audit_check missing targets";
                }
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
            if (type == k_type_result) {
                result = std::move(msg);
                return true;
            }
            if (type == k_type_error) {
                error = msg.value("message", std::string("plugin host error"));
                return false;
            }
            error = "unexpected plugin host message: " + type;
            return false;
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
        m_impl = std::make_unique<Impl>();
    return *m_impl;
}

PluginHostClient::~PluginHostClient()
{
    try {
        shutdown();
    } catch (...) {}
}

void PluginHostClient::set_force_in_process_for_testing(bool force)
{
    g_force_in_process.store(force, std::memory_order_release);
}

void PluginHostClient::set_execute_timeout_for_testing(int timeout_ms)
{
    g_execute_timeout_ms.store(timeout_ms > 0 ? timeout_ms : k_default_timeout_ms, std::memory_order_release);
}

void PluginHostClient::reset_spawn_count_for_testing()
{
    g_spawn_count.store(0, std::memory_order_release);
}

std::uint64_t PluginHostClient::spawn_count() { return g_spawn_count.load(std::memory_order_acquire); }

bool PluginHostClient::should_use_host()
{
    if (in_child_process())
        return false;
    if (g_force_in_process.load(std::memory_order_acquire))
        return false;
    if (const char* env = boost::nowide::getenv("EDGE_PLUGIN_HOST_IN_PROCESS")) {
        if (env[0] != '\0' && env[0] != '0')
            return false;
    }
    return helper_exists();
}

bool PluginHostClient::start_script(const std::string& plugin_key,
                                    const std::string& capability_name,
                                    std::function<void(const ScriptCompletion&)> on_done)
{
    Impl& i = impl();
    if (i.stopping.load(std::memory_order_acquire))
        return false;

    {
        std::lock_guard<std::mutex> lock(i.mutex);
        i.active.insert(plugin_key);
    }

    try {
        std::thread([&i, plugin_key, capability_name, on_done]() {
            ScriptCompletion completion;
            std::string      error;
            const int        timeout_ms = g_execute_timeout_ms.load(std::memory_order_acquire);
            bool             ok         = false;
            {
                std::unique_lock<std::mutex> lock(i.mutex);
                if (!i.ensure_started_locked(error) || !i.load_plugin_locked(plugin_key, error)) {
                    completion.error = error;
                    i.active.erase(plugin_key);
                    lock.unlock();
                    deliver_completion(on_done, completion);
                    return;
                }
                ScopedPluginAuditContext audit(plugin_key, capability_name);
                nlohmann::json msg = {
                    {"type", k_type_execute},
                    {"id", i.next_id++},
                    {"kind", k_kind_script},
                    {"plugin_key", plugin_key},
                    {"capability_name", capability_name},
                    {"timeout_ms", timeout_ms},
                };
                if (!i.host.channel.write_json(msg, timeout_ms, error)) {
                    completion.error = error;
                } else {
                    nlohmann::json result;
                    lock.unlock();
                    ok = i.pump_until_result(plugin_key, capability_name, timeout_ms, result, error);
                    lock.lock();
                    if (!ok)
                        completion.error = error;
                    else {
                        completion.result.status  = static_cast<PluginResult>(result.value("status", 0));
                        completion.result.message = result.value("message", std::string());
                        completion.result.data    = result.value("data", std::string());
                        completion.cancelled      = result.value("cancelled", false);
                    }
                }
                if (!ok && (error == "plugin host timed out" || error == "closed pipe" ||
                            error.find("truncated") != std::string::npos ||
                            error.find("plugin host timed out") != std::string::npos)) {
                    i.reset_child_locked();
                    if (completion.error.empty())
                        completion.error = error.empty() ? "plugin host failed" : error;
                }
                i.active.erase(plugin_key);
            }
            deliver_completion(on_done, completion);
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(i.mutex);
        i.active.erase(plugin_key);
        return false;
    }
    return true;
}

ExecutionResult PluginHostClient::execute_post_process(const std::string& plugin_key,
                                                       const std::string& capability_name,
                                                       const SlicingPipelineContext& ctx,
                                                       std::string& error)
{
    Impl& i = impl();
    std::lock_guard<std::mutex> lock(i.mutex);
    i.active.insert(plugin_key);
    const int timeout_ms = g_execute_timeout_ms.load(std::memory_order_acquire);
    ExecutionResult result;
    if (!i.ensure_started_locked(error) || !i.load_plugin_locked(plugin_key, error)) {
        i.active.erase(plugin_key);
        return ExecutionResult::failure(PluginResult::FatalError, error);
    }

    ScopedPluginAuditContext audit(plugin_key, capability_name);
    if (!ctx.gcode_path.empty())
        PluginAuditManager::instance().add_scoped_allowed_root(boost::filesystem::path(ctx.gcode_path).parent_path());

    nlohmann::json msg = {
        {"type", k_type_execute},
        {"id", i.next_id++},
        {"kind", k_kind_post_process},
        {"plugin_key", plugin_key},
        {"capability_name", capability_name},
        {"timeout_ms", timeout_ms},
        {"gcode_path", ctx.gcode_path},
        {"host", ctx.host},
        {"output_name", ctx.output_name},
        {"write_root", ctx.gcode_path.empty() ? std::string()
                                              : boost::filesystem::path(ctx.gcode_path).parent_path().string()},
        {"config", config_snapshot(ctx.full_config)},
        {"orca_version", ctx.orca_version},
    };
    if (!i.host.channel.write_json(msg, timeout_ms, error)) {
        i.reset_child_locked();
        i.active.erase(plugin_key);
        return ExecutionResult::failure(PluginResult::FatalError, error);
    }
    nlohmann::json reply;
    if (!i.pump_until_result(plugin_key, capability_name, timeout_ms, reply, error)) {
        i.reset_child_locked();
        i.active.erase(plugin_key);
        return ExecutionResult::failure(PluginResult::FatalError, error);
    }
    i.active.erase(plugin_key);
    result.status  = static_cast<PluginResult>(reply.value("status", 0));
    result.message = reply.value("message", std::string());
    result.data    = reply.value("data", std::string());
    return result;
}

void PluginHostClient::cancel(const std::string& plugin_key)
{
    (void) plugin_key;
    // Sync execute in the child is not interruptible over the data channel (nested host_call
    // replies share it). Hung children are killed by the execute timeout; unload abandons the
    // wait bound the same way ScriptAsyncRuntime does.
}

bool PluginHostClient::has_active_run(const std::string& plugin_key) const
{
    if (!m_impl)
        return false;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->active.count(plugin_key) != 0;
}

bool PluginHostClient::kill_child_for_testing()
{
    Impl& i = impl();
    std::lock_guard<std::mutex> lock(i.mutex);
    if (!i.host.valid())
        return false;
    kill_host(i.host);
    return true;
}

std::int64_t PluginHostClient::child_pid_for_testing() const
{
    if (!m_impl)
        return 0;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->host.pid;
}

void PluginHostClient::shutdown()
{
    if (!m_impl)
        return;
    m_impl->stopping.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->host.valid()) {
        std::string error;
        nlohmann::json msg = {{"type", k_type_shutdown}};
        (void) m_impl->host.channel.write_json(msg, 1000, error);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        kill_host(m_impl->host);
        close_host(m_impl->host);
    }
    m_impl->loaded.clear();
    m_impl->active.clear();
    m_impl->stopping.store(false, std::memory_order_release);
}

} // namespace Slic3r::plugin_ipc
