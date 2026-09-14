#ifndef slic3r_GUI_LazyPanelHolder_hpp_
#define slic3r_GUI_LazyPanelHolder_hpp_

// A placeholder notebook page that builds its real panel the first time it is shown.
//
// Several of the main notebook's tabs (Device, Multi-device, Project, Calibration) are
// expensive to construct - together they were 58% of MainFrame::init_tabpanel - and are
// rarely the tab a user lands on at startup. The holder keeps the tab's slot in the
// Notebook so the TabPosition indices, select_tab() and the add/remove dance in
// show_device() all keep working unchanged, while the real panel is constructed on
// first Show(true) (or on demand through MainFrame's getters).
//
// The real panel becomes the holder's only child and fills it, so once created the tab
// behaves exactly as it did when it was the page itself: its own Show()/Hide() is driven
// by the holder, which is what both MonitorPanel::Show() and CalibrationPanel::Show()
// use to (re)start their refresh timers and pull the current device state. That "pull on
// show" is what lets these panels be created late without missing the device events they
// would have seen at startup - they were never event subscribers, they poll.

#include <functional>

#include <wx/panel.h>
#include <wx/sizer.h>

#include "libslic3r/StartupProfile.hpp"

namespace Slic3r { namespace GUI {

class LazyPanelHolder : public wxPanel
{
public:
    // factory: builds the real panel with the given parent (this holder). Called at most
    // once. profile_name: the StartupProfile mark emitted around the deferred build, so a
    // profiled run shows the cost where it actually happens.
    LazyPanelHolder(wxWindow *parent, const char *profile_name, std::function<wxWindow *(wxWindow *)> factory)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
        , m_profile_name(profile_name)
        , m_factory(std::move(factory))
    {
        SetSizer(new wxBoxSizer(wxVERTICAL));
    }

    bool is_realized() const { return m_panel != nullptr; }
    wxWindow *peek() const { return m_panel; }

    // Build the real panel if it does not exist yet; returns it either way.
    wxWindow *realize()
    {
        if (m_panel == nullptr && m_factory) {
            // Guard against re-entry: realizing runs arbitrary construction which can
            // pump events (webviews, popups), and a Show() during that must not recurse.
            if (m_realizing)
                return nullptr;
            m_realizing = true;
            {
                Slic3r::StartupScopedTimer t(m_profile_name);
                m_panel = m_factory(this);
            }
            m_realizing = false;
            m_factory   = nullptr;
            if (m_panel != nullptr) {
                GetSizer()->Add(m_panel, 1, wxEXPAND, 0);
                Layout();
                // The holder may already be visible (this is normally called from
                // Show(true)); make sure the freshly built panel follows its state.
                m_panel->Show(IsShown());
            }
        }
        return m_panel;
    }

    bool Show(bool show = true) override
    {
        if (show)
            realize();
        if (m_panel != nullptr) {
            // The panel's own Show() is what restarts its refresh timer and pulls the
            // current device state, so it must see every transition the holder sees.
            m_panel->Show(show);
        }
        const bool ret = wxPanel::Show(show);
        if (show && m_panel != nullptr)
            Layout();
        return ret;
    }

private:
    const char                           *m_profile_name;
    std::function<wxWindow *(wxWindow *)> m_factory;
    wxWindow                             *m_panel{nullptr};
    bool                                  m_realizing{false};
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_LazyPanelHolder_hpp_
