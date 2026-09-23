#ifndef slic3r_GUI_HomePanel_hpp_
#define slic3r_GUI_HomePanel_hpp_

#include <wx/panel.h>

class wxStaticText;
class wxBoxSizer;
class Button;

namespace Slic3r {
namespace GUI {

class HubHomeView;
class WebViewPanel;

// The Home tab. It shows the phone hub page (HubHomeView). The old flutter start page
// (WebViewPanel: recent projects, Snapmaker's model library) is kept behind it for the code that
// still talks to it: it is built only on demand - File > Start page, EVT_LOAD_URL, the hub view's
// "Open start page" - and then shown in place of the hub with a strip to go back.
class HomePanel : public wxPanel
{
public:
    explicit HomePanel(wxWindow* parent);

    HubHomeView*  hub() const { return m_hub; }
    WebViewPanel* start_page();                              // builds it on first use
    WebViewPanel* built_start_page() const { return m_start; } // null until then

    void show_hub();
    void show_start_page();
    bool start_page_shown() const { return m_start != nullptr && m_start_shown; }

    // The Home tab was selected (true) or left (false).
    void on_tab_changed(bool selected);
    void sys_color_changed();

private:
    void notify_start_page(bool active);
    void apply_colours();

    wxBoxSizer*   m_sizer { nullptr };
    wxPanel*      m_strip { nullptr };
    wxStaticText* m_strip_label { nullptr };
    Button*       m_btn_back { nullptr };
    HubHomeView*  m_hub { nullptr };
    WebViewPanel* m_start { nullptr };
    bool          m_start_shown { false };
    bool          m_selected { false };
};

} // namespace GUI
} // namespace Slic3r

#endif
