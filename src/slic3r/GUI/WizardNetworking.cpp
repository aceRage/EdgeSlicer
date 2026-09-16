#include "WizardNetworking.hpp"

namespace Slic3r { namespace GUI {

bool wizard_should_enable_networking(bool key_present, bool key_value, bool any_bbl_selected)
{
    // Nothing Bambu was selected: the plug-in is not this run's business either way.
    if (! any_bbl_selected)
        return false;
    // Already on, or explicitly turned off by the user: leave the preference exactly as it stands.
    if (key_present)
        return false;
    // Unset, and a Bambu printer was just installed.
    return true;
}

} } // namespace Slic3r::GUI
