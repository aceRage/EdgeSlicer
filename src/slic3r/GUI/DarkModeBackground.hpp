#pragma once

#include <wx/colour.h>

class wxWindow;

namespace Slic3r { namespace GUI {

// What GUI_App::UpdateDarkUI does with a window's background in dark mode.
//
// A child window that never had a background colour of its own reports the system button face
// (COLOR_BTNFACE, #F0F0F0) from GetBackgroundColour(), yet MSW paints it with the brush of the
// nearest ancestor that does have one (wxWindowMSW::MSWGetBgBrush): wxStaticText, wxCheckBox,
// wxStaticBitmap, a plain wxPanel inside a coloured panel, ... all look like their parent.
// gDarkColors maps #F0F0F0 to #3F3F46 (an entry Snapmaker added for the mixed-filament dialog's
// dividers in ac3dafe08a; OrcaSlicer keeps it commented out for this reason), so pinning the
// mapped value on such a child painted a lighter grey band behind every label on the Device page,
// in Preferences and in most dialogs.
struct DarkBackground
{
    enum class Action {
        Map,        // an ordinary colour: SetBackgroundColour(colour) when it differs from the old one
        LeaveUnset, // a native control that follows its parent: touch nothing, it paints `colour`
        SetParent,  // a plain panel that follows its parent: SetBackgroundColour(colour), so custom
                    // children that copy their parent's colour (Label, StaticBox) get the right one
    };
    Action   action;
    wxColour colour; // the background the window shows in dark mode (used for text contrast too)
};

// The decision only; the caller applies it. Pure apart from reading the window tree.
DarkBackground dark_mode_background_for(wxWindow *window);

// The background a window actually shows when it has no colour of its own: its nearest ancestor
// that has one, or the top-level window. Not mapped to dark.
wxColour visible_parent_background(wxWindow *window);

}} // namespace Slic3r::GUI
