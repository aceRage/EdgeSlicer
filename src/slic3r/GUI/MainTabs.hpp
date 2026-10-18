#ifndef slic3r_GUI_MainTabs_hpp_
#define slic3r_GUI_MainTabs_hpp_

// The main window's tab order, kept free of wxWidgets so it can be unit tested
// (tests/slic3rutils/main_tabs_tests.cpp). MainFrame::TabPosition takes its values from here.
//
// Showcase (2026-10-18, D1) sits right after Preview; every tab after it moved up by one. Nothing
// stores a tab index (the "default_page" preference only ever holds 0 or 1, which did not move),
// web pages switch tabs by NAME (tab_from_name, used by SSWCP's sw_SwitchTab), and the page order
// is built so the real notebook index of each page is the value below (MainFrame::update_layout
// inserts the three plater pages, the Device swap inserts at Monitor).

#include <cstring>
#include <string>

namespace Slic3r {
namespace GUI {
namespace MainTabs {

enum Position : int {
    Home        = 0,
    Editor3D    = 1,   // Prepare
    Preview     = 2,
    Showcase    = 3,
    Monitor     = 4,   // Device
    Stream      = 5,
    Printers    = 6,   // was "Multi-device"
    Project     = 7,
    Calibration = 8,
    Auxiliary   = 9,   // never has a page
    DebugTool   = 10,  // never has a page
};

// The three pages that host the plater (one wxPanel added three times).
constexpr bool hosts_plater(int tab) { return tab == Editor3D || tab == Preview || tab == Showcase; }

// The names web pages use with sw_SwitchTab. -1 for an unknown name.
inline int tab_from_name(const std::string &name)
{
    struct Entry { const char *name; int tab; };
    static const Entry entries[] = {
        { "Home", Home },         { "3DEditor", Editor3D }, { "Preview", Preview },   { "Showcase", Showcase },
        { "Monitor", Monitor },   { "Stream", Stream },     { "MultiDevice", Printers }, { "Printers", Printers },
        { "Project", Project },   { "Calibration", Calibration }, { "Auxiliary", Auxiliary }, { "DebugTool", DebugTool },
    };
    for (const Entry &e : entries)
        if (name == e.name)
            return e.tab;
    return -1;
}

// Ctrl+1..4 on a settings field (Field.cpp): Home, Prepare, Preview, Device - the same four tabs
// those keys opened before Showcase moved Device up by one. -1 for any other key.
constexpr int tab_for_field_shortcut(char key)
{
    return key == '1' ? Home : key == '2' ? Editor3D : key == '3' ? Preview : key == '4' ? Monitor : -1;
}

} // namespace MainTabs
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_MainTabs_hpp_
