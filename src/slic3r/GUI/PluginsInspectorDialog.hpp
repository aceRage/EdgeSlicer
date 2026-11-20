#ifndef slic3r_GUI_PluginsInspectorDialog_hpp_
#define slic3r_GUI_PluginsInspectorDialog_hpp_

#include <string>

#include <nlohmann/json.hpp>

#include "GUI_Utils.hpp"

class wxWebView;
class wxWebViewEvent;
class wxWindow;

namespace Slic3r { namespace GUI {

// Per-plugin inspector: permission editor (#17285 grant model) and capability config.
// Opened from Home > Plugins Permissions / Configure. Compiled only with EDGE_PYTHON_PLUGINS.
class PluginsInspectorDialog : public DPIDialog
{
public:
    enum class Tab { Permissions, Configure };

    PluginsInspectorDialog(wxWindow* parent, std::string plugin_key, Tab tab);
    ~PluginsInspectorDialog() override;

    static void show_for(wxWindow* parent, const std::string& plugin_key, Tab tab);
    static void notify_permissions_changed(const std::string& plugin_key);

    void reload();

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void on_loaded(wxWebViewEvent& evt);
    void on_message(wxWebViewEvent& evt);
    void send_json(const nlohmann::json& payload);
    void send_plugin();
    void mutate_permission(const nlohmann::json& payload);
    void handle_config(const nlohmann::json& payload);
    void show_status(const wxString& message, const std::string& level);

    std::string m_plugin_key;
    Tab         m_tab { Tab::Permissions };
    wxWebView*  m_browser { nullptr };
    bool        m_ready { false };
};

PluginsInspectorDialog::Tab inspector_tab_from_command(const std::string& command);

}} // namespace Slic3r::GUI

#endif
