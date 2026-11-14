#include "PluginHostChild.hpp"

#include "PluginIpcChannel.hpp"
#include "PluginIpcProtocol.hpp"

#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Utils.hpp>
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PyPluginTrampoline.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/pluginTypes/script/ScriptPluginCapability.hpp>
#include <slic3r/plugin/pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp>

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <pybind11/gil.h>

namespace Slic3r::plugin_ipc {
namespace {

Channel*              g_channel  = nullptr;
std::recursive_mutex* g_io_mutex = nullptr;
std::atomic<std::uint64_t>* g_next_id = nullptr;

nlohmann::json rpc_roundtrip(nlohmann::json request)
{
    if (g_channel == nullptr || g_io_mutex == nullptr || g_next_id == nullptr)
        throw std::runtime_error("plugin host RPC is not available");
    const std::uint64_t id = g_next_id->fetch_add(1, std::memory_order_relaxed);
    request["id"]          = id;
    std::string error;
    nlohmann::json reply;
    {
        pybind11::gil_scoped_release release;
        std::lock_guard<std::recursive_mutex> lock(*g_io_mutex);
        if (!g_channel->write_json(request, k_default_timeout_ms, error))
            throw std::runtime_error(error.empty() ? "host RPC write failed" : error);
        if (!g_channel->read_json(reply, k_default_timeout_ms, error))
            throw std::runtime_error(error.empty() ? "host RPC read failed" : error);
    }
    return reply;
}

nlohmann::json execution_result_json(const ExecutionResult& result, bool cancelled)
{
    return nlohmann::json{
        {"type", k_type_result},
        {"status", static_cast<int>(result.status)},
        {"message", result.message},
        {"data", result.data},
        {"cancelled", cancelled},
    };
}

bool load_plugin(const nlohmann::json& msg, std::string& error)
{
    const std::string plugin_key = msg.value("plugin_key", std::string());
    if (plugin_key.empty()) {
        error = "plugin_key missing";
        return false;
    }
    PluginManager& manager = PluginManager::instance();
    // Edge already resolved the entry. Do not scan the plugin directory: a helper .whl sitting
    // next to the .py entry is a hashed dependency, and find_installed_plugin_entry would treat
    // it as a second plugin and mark the package invalid.
    if (msg.contains("entry_path") && msg["entry_path"].is_string() && !msg["entry_path"].get<std::string>().empty()) {
        PluginDescriptor descriptor;
        descriptor.plugin_key        = plugin_key;
        descriptor.entry_path        = msg.value("entry_path", std::string());
        descriptor.plugin_root       = msg.value("plugin_root", std::string());
        descriptor.entry_package     = msg.value("entry_package", std::string());
        descriptor.name              = msg.value("name", plugin_key);
        descriptor.version           = msg.value("version", std::string());
        descriptor.installed_version = msg.value("installed_version", std::string());
        descriptor.required_orca_api = msg.value("required_orca_api", std::string());
        descriptor.author            = msg.value("author", std::string());
        descriptor.description       = msg.value("description", std::string());
        descriptor.metadata_valid    = true;
        descriptor.enabled           = true;
        manager.adopt_parent_descriptor(std::move(descriptor));
    }
    PluginDescriptor descriptor;
    if (!manager.try_get_plugin_descriptor(plugin_key, descriptor)) {
        error = "plugin not discovered: " + plugin_key;
        return false;
    }
    if (manager.is_plugin_loaded(plugin_key))
        return true;
    manager.load_plugin(plugin_key, /*skip_deps=*/true);
    return manager.wait_for_plugin_load(plugin_key, std::chrono::seconds(120), error);
}

ScriptCompletion run_script(const std::string& plugin_key, const std::string& capability_name)
{
    ScriptCompletion completion;
    auto cap = PluginManager::instance().get_plugin_capability(
        {PluginCapabilityType::Script, capability_name, plugin_key});
    if (!cap) {
        completion.error = "script capability not found";
        return completion;
    }

    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;
    const bool started = ScriptAsyncRuntime::instance().start(plugin_key, capability_name, cap,
                                                              [&](const ScriptCompletion& c) {
                                                                  std::lock_guard<std::mutex> lock(mutex);
                                                                  completion = c;
                                                                  done       = true;
                                                                  cv.notify_all();
                                                              });
    if (!started) {
        completion.error = "failed to start script in plugin host";
        return completion;
    }
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return done; });
    return completion;
}

ExecutionResult run_post_process(const nlohmann::json& msg, std::string& error)
{
    const std::string plugin_key      = msg.value("plugin_key", std::string());
    const std::string capability_name = msg.value("capability_name", std::string());
    auto cap = PluginManager::instance().get_plugin_capability(
        {PluginCapabilityType::SlicingPipeline, capability_name, plugin_key});
    auto pipeline = std::dynamic_pointer_cast<SlicingPipelinePluginCapability>(cap);
    if (!pipeline) {
        error = "post-process capability not found";
        return ExecutionResult::failure(PluginResult::FatalError, error);
    }

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    if (msg.contains("config") && msg["config"].is_object()) {
        ConfigSubstitutionContext subst{ForwardCompatibilitySubstitutionRule::EnableSilent};
        for (auto it = msg["config"].begin(); it != msg["config"].end(); ++it) {
            if (!config.has(it.key()) || !it.value().is_string())
                continue;
            try {
                config.set_deserialize(it.key(), it.value().get<std::string>(), subst);
            } catch (...) {}
        }
    }

    SlicingPipelineContext ctx;
    ctx.orca_version = msg.value("orca_version", std::string());
    ctx.step         = SlicingPipelineStepPlugin::psGCodePostProcess;
    ctx.gcode_path   = msg.value("gcode_path", std::string());
    ctx.host         = msg.value("host", std::string());
    ctx.output_name  = msg.value("output_name", std::string());
    ctx.full_config  = &config;
    return pipeline->execute(ctx);
}

} // namespace

nlohmann::json call_host(const std::string& method, const nlohmann::json& args)
{
    nlohmann::json request = {
        {"type", k_type_host_call},
        {"method", method},
        {"plugin_key", PluginAuditManager::instance().current_plugin()},
        {"capability", PluginAuditManager::instance().current_capability()},
        {"args", args},
    };
    const nlohmann::json reply = rpc_roundtrip(std::move(request));
    if (reply.value("type", std::string()) != k_type_host_result)
        throw std::runtime_error("unexpected host RPC reply");
    if (!reply.value("ok", false))
        throw std::runtime_error(reply.value("error", std::string("host RPC denied")));
    return reply.value("value", nlohmann::json::object());
}

AuditDecision rpc_audit_check(const std::string& event_name,
                              const std::vector<std::string>& targets,
                              bool is_write,
                              const std::string& event_type)
{
    nlohmann::json request = {
        {"type", k_type_audit_check},
        {"event", event_name},
        {"event_type", event_type},
        {"is_write", is_write},
        {"targets", targets},
        {"plugin_key", PluginAuditManager::instance().current_plugin()},
        {"capability", PluginAuditManager::instance().current_capability()},
    };
    try {
        const nlohmann::json reply = rpc_roundtrip(std::move(request));
        AuditDecision        d;
        d.allowed = reply.value("ok", false);
        d.reason  = reply.value("reason", std::string());
        return d;
    } catch (const std::exception& ex) {
        return {false, ex.what()};
    }
}

int run_child(Channel& channel)
{
    g_channel = &channel;
    std::recursive_mutex io_mutex;
    std::atomic<std::uint64_t> next_id{1};
    g_io_mutex = &io_mutex;
    g_next_id  = &next_id;

    std::string error;
    nlohmann::json hello;
    if (!channel.read_json(hello, k_handshake_timeout_ms, error)) {
        BOOST_LOG_TRIVIAL(error) << "edge-plugin-host handshake read failed: " << error;
        return 2;
    }
    if (hello.value("type", std::string()) != k_type_hello) {
        BOOST_LOG_TRIVIAL(error) << "edge-plugin-host expected hello";
        return 2;
    }
    int remote = hello.value("protocol_version", 0);
    if (!handshake_versions_compatible(remote, error)) {
        nlohmann::json err = {{"type", k_type_error}, {"message", error}};
        (void) channel.write_json(err, k_handshake_timeout_ms, error);
        return 3;
    }
    if (hello.contains("data_dir") && hello["data_dir"].is_string())
        Slic3r::set_data_dir(hello["data_dir"].get<std::string>());
    if (hello.contains("resources_dir") && hello["resources_dir"].is_string())
        Slic3r::set_resources_dir(hello["resources_dir"].get<std::string>());

    if (!PluginManager::instance().initialize(/*install_hooks=*/false)) {
        nlohmann::json err = {{"type", k_type_error},
                              {"message", "failed to initialize plugin manager: " +
                                              PythonInterpreter::instance().last_error()}};
        (void) channel.write_json(err, k_handshake_timeout_ms, error);
        return 4;
    }

    nlohmann::json hello_ok = {{"type", k_type_hello_ok}, {"protocol_version", k_protocol_version}};
    if (!channel.write_json(hello_ok, k_handshake_timeout_ms, error))
        return 2;

    for (;;) {
        nlohmann::json msg;
        if (!channel.read_json(msg, -1, error)) {
            BOOST_LOG_TRIVIAL(info) << "edge-plugin-host channel closed: " << error;
            break;
        }
        const std::string type = msg.value("type", std::string());
        if (type == k_type_shutdown) {
            nlohmann::json ok = {{"type", k_type_shutdown_ok}};
            (void) channel.write_json(ok, 1000, error);
            break;
        }
        if (type == k_type_load) {
            std::string load_error;
            nlohmann::json reply;
            if (load_plugin(msg, load_error))
                reply = {{"type", k_type_load_ok}};
            else
                reply = {{"type", k_type_error}, {"message", load_error}};
            if (!channel.write_json(reply, k_default_timeout_ms, error))
                break;
            continue;
        }
        if (type == k_type_unload) {
            PluginManager::instance().unload_plugin(msg.value("plugin_key", std::string()));
            nlohmann::json reply = {{"type", k_type_unload_ok}};
            if (!channel.write_json(reply, k_default_timeout_ms, error))
                break;
            continue;
        }
        if (type == k_type_execute) {
            const std::string kind       = msg.value("kind", std::string());
            const std::string plugin_key = msg.value("plugin_key", std::string());
            std::string       load_error;
            nlohmann::json    reply;
            if (!load_plugin(msg, load_error)) {
                reply = {{"type", k_type_error}, {"message", load_error}};
            } else if (kind == k_kind_post_process) {
                try {
                    std::string exec_error;
                    const ExecutionResult result = run_post_process(msg, exec_error);
                    if (!exec_error.empty() && result.status == PluginResult::Success)
                        reply = {{"type", k_type_error}, {"message", exec_error}};
                    else
                        reply = execution_result_json(result, false);
                } catch (const std::exception& ex) {
                    reply = {{"type", k_type_error}, {"message", ex.what()}};
                }
            } else {
                const ScriptCompletion completion =
                    run_script(plugin_key, msg.value("capability_name", std::string()));
                if (!completion.error.empty() && !completion.cancelled)
                    reply = {{"type", k_type_error}, {"message", completion.error}};
                else
                    reply = execution_result_json(completion.result, completion.cancelled);
            }
            if (!channel.write_json(reply, k_default_timeout_ms, error))
                break;
            continue;
        }
        nlohmann::json unknown = {{"type", k_type_error}, {"message", "unknown message type: " + type}};
        if (!channel.write_json(unknown, k_default_timeout_ms, error))
            break;
    }

    PluginManager::instance().shutdown();
    g_channel  = nullptr;
    g_io_mutex = nullptr;
    g_next_id  = nullptr;
    return 0;
}

} // namespace Slic3r::plugin_ipc
