#ifndef slic3r_GUI_UntrustedSettingsGuard_hpp_
#define slic3r_GUI_UntrustedSettingsGuard_hpp_

// The settings check for files we did not write: project 3MFs (their project settings and the
// presets embedded in them), preset files and bundles, and the config block of a G-code file.
//
// A post-processing script ("post_process") is a program the slicer starts on this computer every
// time G-code is exported - for Bambu Lab printers already every time a plate is sliced. Opening a
// shared project must not be enough to run one. When a file brings scripts (or an output name that
// leaves the output folder) the user has not got in any preset of their own, we ask; the default
// answer, and the only answer when nobody is at the PC (hidden / phone-driven instance), is to
// remove them. Print-host addresses and credentials, and bed textures on network shares, are
// removed without asking.
//
// The rules themselves live in libslic3r/UntrustedInput.hpp (unit tested); this file only knows
// where the user's own presets are and how to ask.

#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

class DynamicPrintConfig;
class Preset;

namespace GUI {

// file_label: the file's name as shown to the user.
// project_config: the project settings read from the file (may be null).
// embedded_presets: the presets embedded in the file (may be null or empty).
// strip_network: also drop print-host addresses / credentials and network bed files. On for
// projects (a project has no business pointing the printer tab or uploads elsewhere); off for a
// preset file the user imports on purpose, which may well be their own printer with its host.
// Changes the configs in place. Returns true when something was removed.
bool guard_untrusted_settings(const std::string &file_label, DynamicPrintConfig *project_config, std::vector<Preset *> *embedded_presets,
                              bool strip_network = true);

// Opens one of the current project's attachments (the 3MF's Auxiliaries: manuals, pictures, models)
// with its default application - when the file really is one of them and of a document / picture /
// model type. A program, script, shortcut or unknown type is shown selected in its folder instead;
// a path outside the attachments is refused. The Project page asks for these by path, and that page
// renders the project's description, which is text from the 3MF.
void open_project_attachment(const boost::filesystem::path &path);

// Installs PresetBundle::untrusted_config_filter so preset imports and G-code config imports get
// the same check.
void install_untrusted_config_filter();

} // namespace GUI
} // namespace Slic3r

#endif
