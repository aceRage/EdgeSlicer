#pragma once
#ifndef slic3r_CliNamedPresets_hpp_
#define slic3r_CliNamedPresets_hpp_

// Presets named on the command line (--printer-preset, --process-preset, --filament-presets) are written
// as flat files for --load-settings / --load-filaments (Snapmaker_Orca.cpp, NamedPresets). Kept out of
// that file so Catch2 can test it.

#include <string>

namespace Slic3r {

// The "from" and "inherits" of such a file. A system preset, and a user preset with no system parent,
// are "system" with no "inherits", as before. A user printer preset with a system parent is "User"
// inheriting that parent: written as "system" it stood for a system printer of its own name, which no
// system process lists, so every process was "not compatible" (exit 239).
struct CliNamedPresetOrigin
{
    std::string from;
    std::string inherits;
};
CliNamedPresetOrigin cli_named_preset_origin(bool is_printer, bool is_user, const std::string &system_parent);

// The system preset a machine or process file loaded with --load-settings stands for, which the
// processes' compatible_printers are matched against: itself when "from" is "system", else its
// "inherits".
std::string cli_config_system_name(const std::string &from, const std::string &name, const std::string &inherits);

} // namespace Slic3r

#endif // slic3r_CliNamedPresets_hpp_
