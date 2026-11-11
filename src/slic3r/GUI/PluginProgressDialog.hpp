#ifndef slic3r_GUI_PluginProgressDialog_hpp_
#define slic3r_GUI_PluginProgressDialog_hpp_

#include <functional>
#include <memory>

#include <wx/event.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/window.h>

#include "Widgets/ProgressDialog.hpp"

class wxTimer;
class wxTimerEvent;
class wxWindow;

namespace Slic3r { namespace GUI {

// A host-owned progress dialog for Python plugins. This class is deliberately
// Python-agnostic; the plugin layer owns any pybind/GIL concerns and marshals
// all calls to the UI thread.
class PluginProgressDialog : public ProgressDialog
{
public:
    using CloseHandler = std::function<void()>;

    PluginProgressDialog(wxWindow*       parent,
                         const wxString& title,
                         const wxString& message,
                         int             maximum,
                         int             style,
                         CloseHandler    on_destroyed = nullptr);
    ~PluginProgressDialog() override;

    static PluginProgressDialog* create_dialog(wxWindow*       parent,
                                               const wxString& title,
                                               const wxString& message,
                                               int             maximum,
                                               int             style,
                                               CloseHandler    on_destroyed);
    static bool pulse(PluginProgressDialog* dialog, const wxString& message = wxEmptyString);
    static bool update(PluginProgressDialog* dialog, int value, const wxString& message = wxEmptyString);
    static void start_pulse(PluginProgressDialog* dialog, int interval_ms, const wxString& message = wxEmptyString);
    static void stop_pulse(PluginProgressDialog* dialog);
    static void request_close(PluginProgressDialog* dialog);

    bool pulse(const wxString& message = wxEmptyString);
    bool update(int value, const wxString& message = wxEmptyString);
    void start_pulse(int interval_ms, const wxString& message = wxEmptyString);
    void stop_pulse();
    void close();
    bool is_open() const { return m_open; }

private:
    void on_timer(wxTimerEvent& event);

    bool                     m_open{true};
    wxString                 m_pulse_message;
    std::unique_ptr<wxTimer> m_timer;
    CloseHandler             m_on_destroyed;
};

}} // namespace Slic3r::GUI

#endif
