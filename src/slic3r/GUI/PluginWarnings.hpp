#pragma once

#ifdef EDGE_PYTHON_PLUGINS

class wxWindow;

namespace Slic3r {
class ConfigBase;
class PresetBundle;
}

namespace Slic3r::GUI {

// #16929: warn on preset save when a referenced plugin is missing. Returns false if the user cancels.
bool confirm_save_with_unresolved_plugins(wxWindow* parent, const ConfigBase& config);

// #16929: warn after opening a 3MF / project whose presets name a missing plugin.
void warn_if_missing_plugins(wxWindow* parent, const PresetBundle& preset_bundle);

} // namespace Slic3r::GUI

#endif
