#include "DarkModeBackground.hpp"

#include "Widgets/StateColor.hpp"

#include <wx/window.h>
#include <wx/control.h>
#include <wx/bookctrl.h>
#include <wx/settings.h>

namespace Slic3r { namespace GUI {

// A window painted with its parent's brush that never got a colour of its own (see the header).
static bool follows_parent_background(wxWindow *window)
{
#ifdef __WXMSW__
    if (window->UseBgCol() || window->IsTopLevel() || window->GetParent() == nullptr)
        return false;
    // Notebooks report a transparent background too, but they paint their own tab strip.
    if (dynamic_cast<wxBookCtrlBase *>(window) != nullptr)
        return false;
    // Only the system default: combo boxes and text fields default to COLOR_WINDOW and must still
    // go through the dark map.
    return window->HasTransparentBackground() &&
           window->GetBackgroundColour() == wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
#else
    // Elsewhere the defaults are not keys of the dark map, so nothing ever pinned them.
    (void) window;
    return false;
#endif
}

wxColour visible_parent_background(wxWindow *window)
{
    wxWindow *p = window->GetParent();
    while (p != nullptr && !p->UseBgCol() && !p->IsTopLevel() && p->GetParent() != nullptr)
        p = p->GetParent();
    return p != nullptr ? p->GetBackgroundColour() : window->GetBackgroundColour();
}

DarkBackground dark_mode_background_for(wxWindow *window)
{
    if (follows_parent_background(window)) {
        // Mapped in case that ancestor has not been through UpdateDarkUI yet; an ancestor that has
        // already holds a dark colour, which the map leaves alone.
        const wxColour shown = StateColor::darkModeColorFor(visible_parent_background(window));
        // A native control (label, check box, bitmap, slider) keeps no colour at all, so it goes on
        // following its parent even if the parent changes colour later.
        if (dynamic_cast<wxControl *>(window) != nullptr)
            return {DarkBackground::Action::LeaveUnset, shown};
        return {DarkBackground::Action::SetParent, shown};
    }
    return {DarkBackground::Action::Map, StateColor::darkModeColorFor(window->GetBackgroundColour())};
}

}} // namespace Slic3r::GUI
