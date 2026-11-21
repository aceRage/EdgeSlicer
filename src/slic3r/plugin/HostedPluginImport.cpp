#include "HostedPluginImport.hpp"

#include "PluginDescriptor.hpp"
#include "PluginFsUtils.hpp"
#include "PluginLoader.hpp"
#include "PluginManager.hpp"
#include "PluginPreferences.hpp"
#include "ipc/PluginHostClient.hpp"
#include "pluginTypes/script/ScriptPluginCapability.hpp"
#include "pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

#include "slic3r/plugin/EdgePythonInclude.hpp"
#include <pybind11/gil.h>

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <utility>

namespace Slic3r::plugin_import {
namespace {

std::atomic<bool> g_in_process_for_testing{false};

const char* const k_runs_in_helper = "This plugin runs in the sandboxed plugin helper (edge-plugin-host).";

class HostedScriptCapability final : public ScriptPluginCapability, public HostedCapability
{
public:
    explicit HostedScriptCapability(plugin_ipc::HostedCapabilityInfo info) : HostedCapability(std::move(info)) {}

    std::string    get_name() const override { return m_info.name; }
    bool           has_config_ui() const override { return m_info.has_config_ui && !m_info.config_ui.empty(); }
    std::string    get_config_ui() const override { return m_info.config_ui; }
    nlohmann::json get_default_config() const override { return m_info.default_config; }

    // Never reached in a release build: launch_script_capability sends a hosted script to the
    // helper (PluginHostClient::start_script). Refuse rather than run anything here.
    ExecutionResult execute() override { return ExecutionResult::failure(PluginResult::FatalError, k_runs_in_helper); }
};

class HostedPipelineCapability final : public SlicingPipelinePluginCapability, public HostedCapability
{
public:
    explicit HostedPipelineCapability(plugin_ipc::HostedCapabilityInfo info) : HostedCapability(std::move(info)) {}

    std::string    get_name() const override { return m_info.name; }
    bool           has_config_ui() const override { return m_info.has_config_ui && !m_info.config_ui.empty(); }
    std::string    get_config_ui() const override { return m_info.config_ui; }
    nlohmann::json get_default_config() const override { return m_info.default_config; }

    ExecutionResult execute(SlicingPipelineContext& ctx) override
    {
        if (ctx.step == SlicingPipelineStepPlugin::psGCodePostProcess) {
            // The post-process dispatcher already routes hosted plugins to the helper; this keeps a
            // direct call on the same path. Never wait on the helper holding the GIL.
            std::string     error;
            ExecutionResult result;
            const auto      run = [&]() {
                result = plugin_ipc::PluginHostClient::instance().execute_post_process(audit_plugin_key(), name(), ctx, error);
            };
            if (Py_IsInitialized() && PyGILState_Check()) {
                pybind11::gil_scoped_release release;
                run();
            } else {
                run();
            }
            if (!error.empty() && result.status == PluginResult::Success)
                result = ExecutionResult::failure(PluginResult::FatalError, error);
            return result;
        }
        // D2 / D3: the early steps hand the plugin the live Print, which cannot cross the process
        // boundary. A hosted plugin is skipped there (it still runs at psGCodePostProcess).
        const std::string message = GUI::format(_u8L("Slicing pipeline plugin '%1%' runs in the sandboxed plugin helper, so it is "
                                                     "skipped on the early slicing steps. Only a plugin signed by a known publisher, "
                                                     "with \"Allow plugins on the early slicing steps\" on, runs there."),
                                                name());
        bool expected = false;
        if (m_skip_reported.compare_exchange_strong(expected, true))
            PluginManager::instance().report_capability_skip(message);
        return ExecutionResult::skipped(message);
    }

private:
    std::atomic<bool> m_skip_reported{false};
};

class HostedGenericCapability final : public PluginCapabilityInterface, public HostedCapability
{
public:
    explicit HostedGenericCapability(plugin_ipc::HostedCapabilityInfo info) : HostedCapability(std::move(info)) {}

    std::string          get_name() const override { return m_info.name; }
    PluginCapabilityType get_type() const override { return m_info.type; }
    bool                 has_config_ui() const override { return m_info.has_config_ui && !m_info.config_ui.empty(); }
    std::string          get_config_ui() const override { return m_info.config_ui; }
    nlohmann::json       get_default_config() const override { return m_info.default_config; }
};

} // namespace

bool early_steps_allowed(plugin_trust::TrustKind trust, bool shipped_publisher, bool in_process_steps_pref)
{
    return trust == plugin_trust::TrustKind::Signed && shipped_publisher && in_process_steps_pref;
}

ImportPlacement decide_import_placement(const ImportPolicyInput& input)
{
    if (input.in_helper_process)
        return ImportPlacement::InProcess;
    if (input.in_process_for_testing)
        return ImportPlacement::InProcess;
    if (early_steps_allowed(input.trust, input.shipped_publisher, input.in_process_steps_pref))
        return ImportPlacement::InProcess;
    return ImportPlacement::Helper;
}

ImportPlacement placement_for(const plugin_trust::TrustState& trust)
{
    ImportPolicyInput input;
    input.in_helper_process      = plugin_ipc::in_child_process();
    input.in_process_for_testing = in_process_import_for_testing() || !plugin_ipc::PluginHostClient::should_use_host();
    input.trust                  = trust.kind;
    // D2: a user-added publisher key is no better than a local pin here.
    input.shipped_publisher      = trust.kind == plugin_trust::TrustKind::Signed && trust.shipped_publisher;
    input.in_process_steps_pref  = plugin_prefs::enabled(plugin_prefs::k_in_process_steps);
    return decide_import_placement(input);
}

const char* placement_name(ImportPlacement placement)
{
    return placement == ImportPlacement::InProcess ? "in-process" : "helper";
}

void set_in_process_import_for_testing(bool in_process) { g_in_process_for_testing.store(in_process, std::memory_order_release); }

bool in_process_import_for_testing() { return g_in_process_for_testing.load(std::memory_order_acquire); }

std::shared_ptr<PluginCapabilityInterface> make_hosted_capability(const plugin_ipc::HostedCapabilityInfo& info)
{
    switch (info.type) {
    case PluginCapabilityType::Script: return std::make_shared<HostedScriptCapability>(info);
    case PluginCapabilityType::SlicingPipeline: return std::make_shared<HostedPipelineCapability>(info);
    default: return std::make_shared<HostedGenericCapability>(info);
    }
}

const HostedCapability* as_hosted(const PluginCapabilityInterface* capability)
{
    return dynamic_cast<const HostedCapability*>(capability);
}

bool build_hosted_plugin(const PluginDescriptor&                              descriptor,
                         const std::vector<plugin_ipc::HostedCapabilityInfo>& capabilities,
                         const std::vector<std::string>&                      capabilities_to_enable,
                         Plugin&                                              out,
                         std::string&                                         error)
{
    out = Plugin{};
    if (capabilities.empty()) {
        error = _u8L("Plugin module did not register any capabilities");
        return false;
    }

    // Same enable rule as an in-process load (plugin_loader::load): an empty request restores the
    // sidecar state (or enables by default); an explicit request overrides it for that capability.
    PluginInstallState install_state;
    bool               have_install_state = false;
    {
        std::lock_guard<std::mutex> state_lock(install_state_mutex());
        have_install_state = !descriptor.plugin_root.empty() &&
                             read_install_state(boost::filesystem::path(descriptor.plugin_root), install_state);
    }

    std::set<std::pair<int, std::string>>                   seen;
    std::vector<std::shared_ptr<PluginCapabilityInterface>> built;
    built.reserve(capabilities.size());
    for (const plugin_ipc::HostedCapabilityInfo& info : capabilities) {
        if (info.name.empty() || info.type == PluginCapabilityType::Unknown) {
            error = _u8L("Plugin capability materialization failed");
            return false;
        }
        if (!seen.emplace(static_cast<int>(info.type), info.name).second) {
            // TRN %1% is the capability name, %2% the capability type (e.g. Script).
            error = GUI::format(_u8L("Plugin declares duplicate capability '%1%' for type %2%"), info.name,
                                PluginConfig::capability_type_label(info.type));
            return false;
        }
        const bool explicitly_requested = std::find(capabilities_to_enable.begin(), capabilities_to_enable.end(), info.name) !=
                                          capabilities_to_enable.end();
        bool enabled = capabilities_to_enable.empty() || explicitly_requested;
        if (have_install_state && !explicitly_requested) {
            for (const auto& [cap_name, cap_enabled] : install_state.capabilities) {
                if (cap_name == info.name) {
                    enabled = cap_enabled;
                    break;
                }
            }
        }

        std::shared_ptr<PluginCapabilityInterface> cap = make_hosted_capability(info);
        cap->set_audit_plugin_key(descriptor.plugin_key);
        cap->set_resolved_identity(info.name, info.type);
        cap->set_enabled(enabled);
        cap->set_config_ui_available(info.has_config_ui && !info.config_ui.empty());
        built.push_back(std::move(cap));
    }

    out.descriptor = descriptor;
    out.descriptor.clear_error();
    out.capabilities = std::move(built);
    out.hosted       = true;
    return true;
}

bool load_hosted(const PluginDescriptor&                          descriptor,
                 bool                                             skip_deps,
                 const std::vector<std::string>&                  capabilities_to_enable,
                 const std::function<std::string(const Plugin&)>& registry_precheck,
                 Plugin&                                          out,
                 std::string&                                     error)
{
    error.clear();
    out = Plugin{};
    try {
        if (!descriptor.is_metadata_valid()) {
            error = GUI::format(_u8L("Plugin manifest is invalid: %1%"), descriptor.plugin_key);
            if (descriptor.has_error())
                error += " - " + descriptor.normalized_error();
            return false;
        }

        // Dependencies are wheels only, pinned and hashed (H6): installing them runs no plugin code.
        if (!skip_deps && !descriptor.dependencies.empty()) {
            std::string pkg_error;
            if (!plugin_loader::install_packages(descriptor.dependencies, pkg_error)) {
                error = GUI::format(_u8L("Failed to install plugin dependencies: %1%"), pkg_error);
                return false;
            }
        }

        std::vector<plugin_ipc::HostedCapabilityInfo> capabilities;
        std::string                                   host_error;
        BOOST_LOG_TRIVIAL(info) << "[plugin_import] importing plugin=" << descriptor.plugin_key << " in edge-plugin-host";
        if (!plugin_ipc::PluginHostClient::instance().load_in_helper(descriptor.plugin_key, capabilities, host_error)) {
            // TRN %1% is the reason the helper gave.
            error = GUI::format(_u8L("The plugin could not be loaded in the sandboxed plugin helper: %1%"), host_error);
            return false;
        }

        Plugin plugin;
        if (!build_hosted_plugin(descriptor, capabilities, capabilities_to_enable, plugin, error)) {
            plugin_ipc::PluginHostClient::instance().unload(descriptor.plugin_key);
            return false;
        }
        if (registry_precheck) {
            std::string precheck_error = registry_precheck(plugin);
            if (!precheck_error.empty()) {
                plugin_ipc::PluginHostClient::instance().unload(descriptor.plugin_key);
                error = std::move(precheck_error);
                return false;
            }
        }
        out = std::move(plugin);
        BOOST_LOG_TRIVIAL(info) << "[plugin_import] SUCCESS plugin=" << descriptor.plugin_key << " capabilities=" << out.capabilities.size()
                                << " (hosted)";
        return true;
    } catch (const std::exception& ex) {
        out   = Plugin{};
        error = GUI::format(_u8L("Unexpected plugin load failure: %1%"), ex.what());
        return false;
    } catch (...) {
        out   = Plugin{};
        error = _u8L("Unexpected plugin load failure");
        return false;
    }
}

} // namespace Slic3r::plugin_import
