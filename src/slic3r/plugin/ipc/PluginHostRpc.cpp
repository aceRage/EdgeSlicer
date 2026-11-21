#include "PluginHostRpc.hpp"

#include "PluginIpcProtocol.hpp"

#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PluginTrust.hpp>
#include <slic3r/plugin/host/PluginHostBindings.hpp>
#include <slic3r/plugin/OrcaApiVersion.hpp>

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace Slic3r::plugin_ipc {
namespace {

const std::unordered_set<std::string>& allowed_methods()
{
    static const std::unordered_set<std::string> k = {
        "host.model",
        "host.app_info",
        "host.plugin.storage",
    };
    return k;
}

HostResult deny(std::string message)
{
    HostResult r;
    r.ok    = false;
    r.error = std::move(message);
    return r;
}

bool trust_allows(const std::string& plugin_key, std::string& error)
{
    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor(plugin_key, descriptor)) {
        error = "plugin is not registered";
        return false;
    }
    const auto state = plugin_trust::evaluate(descriptor);
    if (!state.allowed()) {
        error = plugin_trust::refuse_untrusted_message();
        return false;
    }
    return true;
}

} // namespace

namespace {
nlohmann::json snapshot_model_json() { return host_bindings::snapshot_model_json(); }
nlohmann::json snapshot_app_info_json() { return host_bindings::snapshot_app_info_json(); }
} // namespace

std::unique_ptr<Model> model_from_snapshot_json(const nlohmann::json& snapshot)
{
    auto model = std::make_unique<Model>();
    if (!snapshot.contains("objects") || !snapshot["objects"].is_array())
        return model;

    for (const auto& obj_json : snapshot["objects"]) {
        ModelObject* object = model->add_object();
        object->name        = obj_json.value("name", std::string());
        double dx = 10.0, dy = 10.0, dz = 10.0;
        if (obj_json.contains("bbox_min") && obj_json.contains("bbox_max") &&
            obj_json["bbox_min"].is_array() && obj_json["bbox_max"].is_array() &&
            obj_json["bbox_min"].size() >= 3 && obj_json["bbox_max"].size() >= 3) {
            dx = std::max(1.0, obj_json["bbox_max"][0].get<double>() - obj_json["bbox_min"][0].get<double>());
            dy = std::max(1.0, obj_json["bbox_max"][1].get<double>() - obj_json["bbox_min"][1].get<double>());
            dz = std::max(1.0, obj_json["bbox_max"][2].get<double>() - obj_json["bbox_min"][2].get<double>());
        }
        object->add_volume(make_cube(dx, dy, dz));
        object->add_instance();
        if (obj_json.contains("bbox_min") && obj_json["bbox_min"].is_array() && obj_json["bbox_min"].size() >= 3) {
            object->instances.front()->set_offset(Vec3d(obj_json["bbox_min"][0].get<double>(),
                                                        obj_json["bbox_min"][1].get<double>(),
                                                        obj_json["bbox_min"][2].get<double>()));
        }
    }
    return model;
}

HostResult handle_host_call(const HostCall& call)
{
    if (call.plugin_key.empty())
        return deny("host_call missing plugin_key");
    if (call.method.empty())
        return deny("host_call missing method");
    if (allowed_methods().count(call.method) == 0)
        return deny("unknown host method: " + call.method);

    const std::string current = PluginAuditManager::instance().current_plugin();
    if (current.empty() || current != call.plugin_key)
        return deny("host_call plugin_key does not match the active Edge audit context");

    std::string trust_error;
    if (!trust_allows(call.plugin_key, trust_error))
        return deny(trust_error);

    try {
        HostResult ok;
        ok.ok = true;
        if (call.method == "host.model") {
            ok.value = snapshot_model_json();
            return ok;
        }
        if (call.method == "host.app_info") {
            ok.value = snapshot_app_info_json();
            return ok;
        }
        if (call.method == "host.plugin.storage") {
            const std::string path = PluginManager::instance().get_storage_dir(call.plugin_key);
            if (PluginAuditManager::instance().is_ultranet_plugins_path(boost::filesystem::path(path)))
                return deny("plugin storage must not be the UltraNet plugins folder");
            ok.value = nlohmann::json{{"path", path}};
            return ok;
        }
        return deny("unknown host method: " + call.method);
    } catch (const std::exception& ex) {
        return deny(ex.what());
    }
}

} // namespace Slic3r::plugin_ipc
