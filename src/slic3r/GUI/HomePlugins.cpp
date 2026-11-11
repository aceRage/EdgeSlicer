#include "HomePlugins.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/HomePluginsLogic.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"
#include "slic3r/plugin/PluginFsUtils.hpp"
#include "slic3r/plugin/PluginManager.hpp"
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
    v.trust.clear();            // Phase 5
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
    } else if (command == "plugin_run")
        stub(_u8L("Running a script from Home lands in a later phase."));
    else if (command == "plugin_configure" || command == "plugin_config_get" || command == "plugin_config_save" ||
             command == "plugin_config_restore")
        stub(_u8L("Configure opens in the Plugins inspector in a later phase."));
    else if (command == "plugin_permissions" || command == "plugin_permissions_mutate")
        stub(_u8L("Permissions open in the Plugins inspector in a later phase."));
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
    PluginManager& mgr = PluginManager::instance();
    std::string    error;
    if (!mgr.install_plugin(path, error)) {
        notice(error.empty() ? _u8L("The plugin could not be installed.") : error, true);
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

} // namespace GUI
} // namespace Slic3r
