#include "PluginsInspectorDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "PluginsInspectorLogic.hpp"

#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PluginConfig.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"

#include <boost/log/trivial.hpp>

#include <stdexcept>

#include <wx/sizer.h>
#include <wx/webview.h>

namespace Slic3r { namespace GUI {
namespace {

PluginsInspectorDialog* s_live = nullptr;

wxString inspector_url(PluginsInspectorDialog::Tab tab)
{
    const char* query = tab == PluginsInspectorDialog::Tab::Configure ? "?tab=config" : "?tab=permissions";
    return wxString::FromUTF8(wxGetApp().page_url(std::string("/web/dialog/PluginsInspector/index.html") + query));
}

PluginCapabilityId capability_id_from_payload(const nlohmann::json& payload, const std::string& plugin_key)
{
    PluginCapabilityId id;
    id.plugin_key = plugin_key;
    id.name       = payload.value("capability_name", payload.value("name", std::string()));
    id.type       = plugin_capability_type_from_string(payload.value("capability_type", payload.value("type", std::string())));
    return id;
}

} // namespace

PluginsInspectorDialog::Tab inspector_tab_from_command(const std::string& command)
{
    if (command == "plugin_configure" || command == "plugin_config_get" || command == "plugin_config_save" ||
        command == "plugin_config_restore")
        return PluginsInspectorDialog::Tab::Configure;
    return PluginsInspectorDialog::Tab::Permissions;
}

void PluginsInspectorDialog::show_for(wxWindow* parent, const std::string& plugin_key, Tab tab)
{
    if (plugin_key.empty())
        return;
    if (s_live != nullptr) {
        s_live->m_plugin_key = plugin_key;
        s_live->m_tab        = tab;
        s_live->reload();
        s_live->Raise();
        return;
    }
    auto* dialog = new PluginsInspectorDialog(parent, plugin_key, tab);
    dialog->Show();
}

void PluginsInspectorDialog::notify_permissions_changed(const std::string& plugin_key)
{
    if (s_live != nullptr && (plugin_key.empty() || s_live->m_plugin_key == plugin_key))
        s_live->reload();
}

PluginsInspectorDialog::PluginsInspectorDialog(wxWindow* parent, std::string plugin_key, Tab tab)
    : DPIDialog(parent, wxID_ANY, _L("Plugin"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX)
    , m_plugin_key(std::move(plugin_key))
    , m_tab(tab)
{
    s_live = this;
    SetMinSize(FromDIP(wxSize(720, 520)));
    SetSize(FromDIP(wxSize(880, 640)));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_browser   = WebView::CreateWebView(this, inspector_url(m_tab));
    if (m_browser == nullptr)
        throw std::runtime_error("Failed to create a WebView for the Plugins inspector");
    sizer->Add(m_browser, 1, wxEXPAND);
    SetSizer(sizer);
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &PluginsInspectorDialog::on_loaded, this);
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PluginsInspectorDialog::on_message, this);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& evt) {
        if (s_live == this)
            s_live = nullptr;
        evt.Skip();
    });
    wxGetApp().UpdateDlgDarkUI(this);
    CentreOnParent();
}

PluginsInspectorDialog::~PluginsInspectorDialog()
{
    if (s_live == this)
        s_live = nullptr;
}

void PluginsInspectorDialog::on_dpi_changed(const wxRect&)
{
    const int em = em_unit();
    SetMinSize(wxSize(72 * em, 52 * em));
    Fit();
}

void PluginsInspectorDialog::on_loaded(wxWebViewEvent&)
{
    m_ready = true;
    reload();
}

void PluginsInspectorDialog::send_json(const nlohmann::json& payload)
{
    if (m_browser == nullptr)
        return;
    const wxString script = "HandleStudio(" + wxString::FromUTF8(payload.dump(-1, ' ', true)) + ")";
    WebView::RunScript(m_browser, script);
}

void PluginsInspectorDialog::show_status(const wxString& message, const std::string& level)
{
    send_json({ { "command", "status_message" }, { "message", into_u8(message) }, { "level", level } });
}

void PluginsInspectorDialog::send_plugin()
{
    PluginDescriptor descriptor;
    nlohmann::json   payload;
    payload["command"] = "inspector_plugin";
    payload["tab"]     = m_tab == Tab::Configure ? "config" : "permissions";
    if (PluginManager::instance().try_get_plugin_descriptor(m_plugin_key, descriptor)) {
        SetTitle(wxString::Format(_L("Plugin: %s"), from_u8(descriptor.name.empty() ? descriptor.plugin_key : descriptor.name)));
        payload["data"] = build_inspector_plugin_payload(descriptor);
    } else {
        payload["data"] = nlohmann::json::object();
        payload["error"] = into_u8(_L("This plugin is no longer installed."));
    }
    send_json(payload);
}

void PluginsInspectorDialog::reload()
{
    if (m_ready)
        send_plugin();
}

void PluginsInspectorDialog::mutate_permission(const nlohmann::json& payload)
{
    const auto finish = [&](bool success) {
        send_plugin();
        const std::string request_id = payload.value("request_id", std::string());
        send_json({ { "command", "plugin_permissions_result" },
                    { "plugin_key", m_plugin_key },
                    { "request_id", request_id },
                    { "success", success } });
    };

    PluginInstallState stored;
    if (!PluginManager::instance().get_install_state(m_plugin_key, stored)) {
        show_status(_L("Install this plugin to edit its permissions."), "warn");
        finish(false);
        return;
    }

    const PermissionMutationParse parsed = parse_permission_mutation(payload, stored.permissions);
    if (!parsed.ok || parsed.plugin_key != m_plugin_key) {
        show_status(parsed.error.empty() ? _L("Invalid permission save request.") : parsed.error, "error");
        finish(false);
        return;
    }
    if (parsed.needs_full_access_confirm) {
        PluginDescriptor descriptor;
        const std::string name = PluginManager::instance().try_get_plugin_descriptor(m_plugin_key, descriptor) &&
                                         !descriptor.name.empty() ?
                                     descriptor.name :
                                     m_plugin_key;
        if (!PluginAuditManager::prompt_enable_full_access(name)) {
            finish(false);
            return;
        }
    }
    if (!PluginManager::instance().mutate_plugin_permissions(m_plugin_key, parsed.mutation)) {
        show_status(_L("Failed to save permissions."), "error");
        finish(false);
        return;
    }
    finish(true);
    PluginDescriptor descriptor;
    const wxString plugin_name =
        PluginManager::instance().try_get_plugin_descriptor(m_plugin_key, descriptor) && !descriptor.name.empty() ?
            from_u8(descriptor.name) :
            from_u8(m_plugin_key);
    show_status(wxString::Format(_L("Permissions saved for \"%s\"."), plugin_name), "success");
}

void PluginsInspectorDialog::handle_config(const nlohmann::json& payload)
{
    const std::string command = payload.value("command", std::string());
    const PluginCapabilityId id = capability_id_from_payload(payload, m_plugin_key);
    if (id.empty()) {
        show_status(_L("This capability is no longer available."), "warn");
        return;
    }
    if (command == "plugin_config_get" || command == "capability_config_get")
        send_json(PluginConfig::get_config_response(id));
    else if (command == "plugin_config_save" || command == "capability_config_save")
        send_json(PluginConfig::save_config_response(id, payload.contains("config") ? payload.at("config") : nlohmann::json::object()));
    else if (command == "plugin_config_restore" || command == "capability_config_restore")
        send_json(PluginConfig::restore_config_response(id));
}

void PluginsInspectorDialog::on_message(wxWebViewEvent& evt)
{
    try {
        const nlohmann::json msg = nlohmann::json::parse(into_u8(evt.GetString()));
        const std::string    command = msg.value("command", std::string());
        if (command == "inspector_ready" || command == "plugin_state")
            send_plugin();
        else if (command == "mutate_plugin_permission")
            mutate_permission(msg);
        else if (command == "plugin_config_get" || command == "plugin_config_save" || command == "plugin_config_restore" ||
                 command == "capability_config_get" || command == "capability_config_save" ||
                 command == "capability_config_restore")
            handle_config(msg);
    } catch (const nlohmann::json::exception& ex) {
        BOOST_LOG_TRIVIAL(warning) << "Plugins inspector received a malformed script message: " << ex.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "Plugins inspector received a non-JSON script message";
    }
}

}} // namespace Slic3r::GUI
