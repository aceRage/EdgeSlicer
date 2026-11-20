#ifdef EDGE_PYTHON_PLUGINS

#include "PluginWarnings.hpp"

#include "slic3r/plugin/PluginResolver.hpp"

#include <wx/msgdlg.h>
#include <wx/window.h>

#include "I18N.hpp"

namespace Slic3r::GUI {

bool confirm_save_with_unresolved_plugins(wxWindow* parent, const ConfigBase& config)
{
    const auto unresolved = config.unresolved_plugin_capabilities();
    if (unresolved.empty())
        return true;
    const std::string text = unresolved_plugins_save_message(unresolved);
    wxMessageDialog dlg(parent, wxString::FromUTF8(text.c_str()), _L("Missing plugin"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    return dlg.ShowModal() == wxID_YES;
}

void warn_if_missing_plugins(wxWindow* parent, const PresetBundle& preset_bundle)
{
    refresh_missing_plugins(preset_bundle);
    const std::string text = missing_plugins_warning_message();
    if (text.empty())
        return;
    wxMessageDialog dlg(parent, wxString::FromUTF8(text.c_str()), _L("Missing plugin"), wxOK | wxICON_WARNING);
    dlg.ShowModal();
}

} // namespace Slic3r::GUI

#endif
