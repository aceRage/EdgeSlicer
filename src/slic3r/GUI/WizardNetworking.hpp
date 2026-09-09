#ifndef slic3r_GUI_WizardNetworking_hpp_
#define slic3r_GUI_WizardNetworking_hpp_

namespace Slic3r { namespace GUI {

// Decides whether the setup wizard should turn the `installed_networking` preference on for the
// user it just finished serving. A Bambu Lab printer is unusable without the network plug-in -
// Account > Login hands the sign-in ticket to the plug-in, and with no plug-in the loopback
// callback 404s - but `installed_networking` has no default, so a first run reads it as false and
// nobody ever offers the download.
//
// The one thing this must not do is undo a deliberate choice, so "never set" and "set to false"
// are kept apart: AppConfig::has() tells them apart, and a key that is present and false is a user
// who turned the plug-in off and is left alone.
//
//   key_present       - app_config->has("installed_networking")
//   key_value         - its value when present (ignored when absent)
//   any_bbl_selected  - the wizard is finishing with at least one Bambu Lab (BBL vendor) printer
bool wizard_should_enable_networking(bool key_present, bool key_value, bool any_bbl_selected);

} } // namespace Slic3r::GUI

#endif // slic3r_GUI_WizardNetworking_hpp_
