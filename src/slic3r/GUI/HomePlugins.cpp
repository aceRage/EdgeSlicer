#include "HomePlugins.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "PluginsInspectorDialog.hpp"
#include "PluginsInspectorLogic.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/HomePluginsLogic.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"
#include "slic3r/plugin/PluginFsUtils.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PluginTrust.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <utility>

#include <wx/filedlg.h>

namespace Slic3r {
namespace GUI {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

static const char* plugin_file_filter()
{
    return "Python plugins (*.py;*.whl)|*.py;*.whl|Python files (*.py)|*.py|Python wheels (*.whl)|*.whl";
}

static bool is_plugin_package_file(const fs::path& path)
{
    std::string ext = path.extension().string();
    boost::algorithm::to_lower(ext);
    return ext == ".py" || ext == ".whl";
}

static HomeTab::PluginView view_of(PluginManager& mgr, const PluginDescriptor& d)
{
    HomeTab::PluginView v;
    v.plugin_key       = d.plugin_key;
    v.name             = d.name;
    v.version          = d.installed_version.empty() ? d.version : d.installed_version;
    v.author           = d.author;
    v.description      = d.description;
    v.source           = "local";
    v.loaded           = mgr.is_plugin_loaded(d.plugin_key);
    v.enabled          = d.enabled || v.loaded;
    v.loading          = mgr.is_plugin_load_in_progress(d.plugin_key);
    v.error            = d.normalized_error();
    if (v.error.empty())
        v.error = mgr.get_plugin_load_error(d.plugin_key);
    v.update_available = false; // catalog updates are Phase 11
    v.trust            = plugin_trust::evaluate(d).label;
    v.types            = d.display_types;

    if (v.loaded) {
        for (const auto& cap : mgr.get_plugin_capabilities(d.plugin_key, PluginCapabilityType::Unknown, false)) {
            if (!cap)
                continue;
            HomeTab::PluginCapabilityView cv;
            cv.name    = cap->name();
            cv.type    = plugin_capability_type_to_string(cap->type());
            cv.enabled = cap->is_enabled();
            v.capabilities.push_back(std::move(cv));
            if (cap->type() == PluginCapabilityType::SlicingPipeline)
                v.in_process_pipeline = true;
        }
    } else {
        PluginInstallState state;
        if (mgr.get_install_state(d.plugin_key, state))
            for (const auto& [name, on] : state.capabilities) {
                HomeTab::PluginCapabilityView cv;
                cv.name    = name;
                cv.enabled = on;
                v.capabilities.push_back(std::move(cv));
            }
        for (const std::string& t : v.types)
            if (t == "slicing-pipeline")
                v.in_process_pipeline = true;
    }
    return v;
}

HomePlugins::HomePlugins(wxWindow* parent, SendFn send)
    : m_parent(parent)
    , m_send(std::move(send))
{
    auto refresh = [this, alive = m_alive](const std::string&) {
        wxGetApp().CallAfter([this, alive]() {
            if (!*alive)
                return;
            if (wxTheApp == nullptr || wxGetApp().is_closing())
                return;
            send_state();
        });
    };
    PluginManager& mgr = PluginManager::instance();
    mgr.subscribe_on_load_callback(refresh);
    mgr.subscribe_on_unload_callback(refresh);
    mgr.subscribe_on_capability_load_callback([refresh](const PluginCapabilityId&) { refresh(std::string()); });
    mgr.subscribe_on_capability_unload_callback([refresh](const PluginCapabilityId&) { refresh(std::string()); });
}

HomePlugins::~HomePlugins() { *m_alive = false; }

bool HomePlugins::listed(const std::string& key) const { return HomeTab::is_listed_plugin_key(key, m_keys); }

bool HomePlugins::listed_capability(const std::string& key, const std::string& name, const std::string& type) const
{
    auto it = m_listed_caps.find(key);
    if (it == m_listed_caps.end())
        return false;
    return HomeTab::is_listed_capability(name, type, it->second);
}

void HomePlugins::notice(const std::string& text, bool error)
{
    m_send({ { "type", "plugin_notice" }, { "text", text }, { "error", error } });
}

void HomePlugins::stub(const std::string& text) { notice(text); }

void HomePlugins::send_state()
{
    PluginManager& mgr = PluginManager::instance();
    m_keys.clear();
    m_listed_caps.clear();
    std::vector<HomeTab::PluginView> items;
    if (mgr.is_discovery_in_progress()) {
        m_send(HomeTab::plugins_message({}, true, mgr.get_discovery_error()));
        return;
    }
    for (const PluginDescriptor& d : mgr.get_plugin_descriptors(true)) {
        if (d.plugin_key.empty() || !HomeTab::is_listed_plugin_key(d.plugin_key, { d.plugin_key }))
            continue;
        m_keys.push_back(d.plugin_key);
        HomeTab::PluginView view = view_of(mgr, d);
        m_listed_caps[d.plugin_key] = view.capabilities;
        auto it = m_run.find(d.plugin_key);
        if (it != m_run.end()) {
            view.run_state   = it->second.state;
            view.run_message = it->second.message;
        }
        items.push_back(std::move(view));
    }
    m_send(HomeTab::plugins_message(items, false, mgr.get_discovery_error()));
}

bool HomePlugins::handle(const json& msg)
{
    const std::string command = msg.value("command", std::string());
    if (command.rfind("plugin_", 0) != 0)
        return false;
    const std::string key  = msg.value("key", std::string());
    const std::string name = msg.value("name", std::string());
    const std::string type = msg.value("type", std::string());

    if (command == "plugin_state") {
        PluginManager::instance().rescan_plugins();
        send_state();
    } else if (command == "plugin_install")
        install();
    else if (command == "plugin_toggle") {
        if (listed(key))
            toggle(key, msg.value("enabled", false));
    } else if (command == "plugin_capability_toggle") {
        if (listed(key) && listed_capability(key, name, type))
            capability_toggle(key, name, type, msg.value("enabled", false));
    } else if (command == "plugin_reload") {
        if (listed(key))
            reload(key, msg.value("clear", false));
    } else if (command == "plugin_delete") {
        if (listed(key))
            remove(key);
    } else if (command == "plugin_reveal") {
        if (listed(key))
            reveal(key);
    } else if (command == "plugin_run") {
        if (listed(key))
            run(key);
    }
    else if (is_inspector_home_command(command)) {
        if (!listed(key))
            return true;
        PluginsInspectorDialog::show_for(m_parent, key, inspector_tab_from_command(command));
    }
    else if (command == "plugin_update" || command == "plugin_catalog")
        stub(_u8L("The Edge catalog lands in a later phase."));
    else
        BOOST_LOG_TRIVIAL(warning) << "HomePlugins: unknown command \"" << command << "\"";
    return true;
}

void HomePlugins::install()
{
    // Native picker; PluginManager copies into {data_dir}/orca_plugins, never {data_dir}/plugins.
    wxFileDialog dlg(m_parent, _L("Install a Python plugin"), from_u8(wxGetApp().app_config->get_last_dir()), wxEmptyString,
                     plugin_file_filter(), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const fs::path path = into_path(dlg.GetPath());
    if (!is_plugin_package_file(path)) {
        notice(_u8L("Only a .py or .whl file can be installed as a plugin."), true);
        return;
    }
    PluginManager&   mgr = PluginManager::instance();
    PluginDescriptor descriptor;
    bool             existing = false;
    std::string      error;
    if (!mgr.inspect_local_plugin_package(path, descriptor, existing, error)) {
        notice(error.empty() ? _u8L("The plugin could not be installed.") : error, true);
        return;
    }

    std::string       hash_error;
    const std::string hash = plugin_trust::sha256_plugin_tree(path, hash_error);
    if (hash.empty()) {
        notice(hash_error.empty() ? _u8L("The plugin file could not be hashed.") : hash_error, true);
        return;
    }

    if (plugin_trust::local_install_needs_prompt(descriptor.plugin_key, hash)) {
        const wxString name = wxString::FromUTF8(
            (descriptor.name.empty() ? descriptor.plugin_key : descriptor.name).c_str());
        const bool replacing = plugin_trust::plugin_key_has_different_pin(plugin_trust::load_trust_store(),
                                                                          descriptor.plugin_key, hash);
        const wxString body  = replacing ?
            wxString::Format(_L("\"%s\" is already installed with a different file. Approve the new package?\n\n"
                                "Yes pins this file's SHA-256 so it can run. No is the default."),
                             name) :
            wxString::Format(_L("Install \"%s\"?\n\n"
                                "This plugin is a local file, not a catalog package. Yes pins this file's SHA-256 "
                                "so it can run on this computer. Only approve a package you trust. Plugins "
                                "installed before signing must be re-approved."),
                             name);
        MessageDialog trust_dlg(m_parent, body, _L("Trust this plugin?"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
        trust_dlg.SetButtonLabel(wxID_NO, _L("No"), /*set_focus=*/true);
        if (trust_dlg.ShowModal() != wxID_YES)
            return;
    }

    if (!mgr.install_plugin(path, descriptor, error)) {
        notice(error.empty() ? _u8L("The plugin could not be installed.") : error, true);
        return;
    }
    std::string pin_error;
    const fs::path    pin_root       = descriptor.plugin_root.empty() ? path : fs::path(descriptor.plugin_root);
    const std::string installed_hash = plugin_trust::sha256_plugin_tree(pin_root, hash_error);
    if (installed_hash.empty() || !plugin_trust::pin_hash(descriptor.plugin_key, installed_hash, pin_error)) {
        notice(pin_error.empty() ? (hash_error.empty() ? _u8L("The plugin was copied but could not be pinned as trusted.") : hash_error) :
                                   pin_error,
               true);
        mgr.rescan_plugins();
        send_state();
        return;
    }
    mgr.rescan_plugins();
    send_state();
}

void HomePlugins::toggle(const std::string& key, bool enable)
{
    PluginManager& mgr = PluginManager::instance();
    if (enable) {
        mgr.load_plugin(key);
        send_state();
        return;
    }
    if (mgr.is_plugin_loaded(key) || mgr.is_plugin_load_in_progress(key)) {
        mgr.unload_plugin(key);
        send_state();
        return;
    }
    mgr.mark_plugin_install_state_disabled(key);
    mgr.rescan_plugins();
    send_state();
}

void HomePlugins::capability_toggle(const std::string& key, const std::string& name, const std::string& type, bool enable)
{
    if (name.empty())
        return;
    PluginManager& mgr = PluginManager::instance();
    if (!mgr.is_plugin_loaded(key)) {
        notice(_u8L("Enable the plugin before changing a capability."), true);
        send_state();
        return;
    }
    mgr.set_capability_enabled({ plugin_capability_type_from_string(type), name, key }, enable);
    send_state();
}

void HomePlugins::reload(const std::string& key, bool clear)
{
    PluginManager& mgr = PluginManager::instance();
    if (mgr.is_plugin_loaded(key) || mgr.is_plugin_load_in_progress(key))
        mgr.unload_plugin(key);
    mgr.load_plugin(key, /*skip_deps=*/!clear);
    send_state();
}

void HomePlugins::remove(const std::string& key)
{
    MessageDialog dlg(m_parent, _L("Delete this plugin? The package is removed from disk."), _L("Plugins"),
                      wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    if (dlg.ShowModal() != wxID_YES)
        return;
    std::string error;
    if (!PluginManager::instance().delete_plugin(key, error)) {
        notice(error.empty() ? _u8L("The plugin could not be deleted.") : error, true);
        return;
    }
    PluginManager::instance().rescan_plugins();
    send_state();
}

void HomePlugins::reveal(const std::string& key)
{
    PluginDescriptor d;
    if (!PluginManager::instance().try_get_plugin_descriptor(key, d))
        return;
    fs::path    resolved;
    std::string error;
    if (!resolve_allowed_plugin_root(d, get_plugin_directories({}),
                                     _u8L("The plugin folder is outside the allowed plugin directories."),
                                     resolved, error)) {
        notice(error.empty() ? _u8L("The plugin folder could not be found.") : error, true);
        return;
    }
    resolved.make_preferred();
    desktop_open_any_folderEx(resolved.string());
}

void HomePlugins::run(const std::string& key)
{
    PluginManager& mgr = PluginManager::instance();
    auto           running = m_run.find(key);
    if (running != m_run.end() && running->second.state == "running")
        return;

    if (!mgr.is_plugin_loaded(key) && !mgr.is_plugin_load_in_progress(key)) {
        m_run[key] = { "failure", _u8L("Enable the plugin first") };
        notice(m_run[key].message, true);
        send_state();
        return;
    }

    auto caps = mgr.get_plugin_capabilities(key, PluginCapabilityType::Script, true);
    if (caps.empty() || !caps.front()) {
        const auto disabled = mgr.get_plugin_capabilities(key, PluginCapabilityType::Script, false);
        if (!disabled.empty() && disabled.front())
            m_run[key] = { "failure", _u8L("Enable the script capability first.") };
        else
            m_run[key] = { "failure", _u8L("This plugin has no script capability to run.") };
        notice(m_run[key].message, true);
        send_state();
        return;
    }

    m_run[key] = { "running", {} };
    send_state();

    const std::string cap_name = caps.front()->name();
    auto launch = mgr.launch_script_capability(key, cap_name,
        [this, alive = m_alive, key](const ScriptCompletion& completion) {
            wxGetApp().CallAfter([this, alive, key, completion]() {
                if (!*alive)
                    return;
                if (wxTheApp == nullptr || wxGetApp().is_closing())
                    return;
                if (completion.cancelled) {
                    m_run.erase(key);
                } else if (!completion.error.empty() ||
                           (completion.result.status != PluginResult::Success &&
                            completion.result.status != PluginResult::Skipped)) {
                    const std::string msg = HomeTab::sanitize_run_message(
                        !completion.error.empty() ? completion.error : completion.result.message);
                    m_run[key] = { "failure", msg };
                    if (!msg.empty())
                        notice(msg, true);
                } else {
                    m_run[key] = { "success", HomeTab::sanitize_run_message(completion.result.message) };
                }
                send_state();
            });
        });

    if (!launch.launched_async) {
        const std::string msg = launch.outcome.error.empty() ? _u8L("The script could not be started.") :
                                                               launch.outcome.error;
        m_run[key] = { "failure", msg };
        notice(msg, true);
        send_state();
    }
}

} // namespace GUI
} // namespace Slic3r
