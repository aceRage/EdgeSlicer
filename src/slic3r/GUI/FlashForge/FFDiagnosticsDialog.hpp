#ifndef slic3r_GUI_FFDiagnosticsDialog_hpp_
#define slic3r_GUI_FFDiagnosticsDialog_hpp_

#include <string>
#include <vector>

#include <wx/dialog.h>
#include <wx/string.h>

#include "FFDiagnostics.hpp"

class wxButton;
class wxStaticText;
class wxTextCtrl;
// Not in a namespace, unlike most of our widgets.
class TextInput;

namespace Slic3r { namespace GUI {

// "Test connection" and "Diagnostics" for a FlashForge LAN printer.
//
// Both buttons exist because the FlashForge stack fails in ways the device list cannot show. A
// printer that does not appear, or appears and will not accept a job, produces no visible reason -
// the tab just stays empty or the send fails. The test runs the two read-only LAN calls the tab
// already uses to identify a printer (fnet_getLanDevProduct, fnet_getLanDevDetail) and reports
// what came back, including the FNET error code and what it means. No job is ever started.
//
// The diagnostics button turns on FlashNetwork's debug logging for the session, runs the same
// test, and writes one zip the user can send on. The check code is never in it.
class FFDiagnosticsDialog : public wxDialog
{
public:
    // serial_number / ip / check_code prefill the fields; any may be empty, and the user can edit
    // them. The check code is used to make the calls and is never stored or written out.
    FFDiagnosticsDialog(wxWindow          *parent,
                        const std::string &serial_number = std::string(),
                        const std::string &ip            = std::string(),
                        const std::string &check_code    = std::string(),
                        unsigned short     port          = 8898);

private:
    void on_test(wxCommandEvent &event);
    void on_diagnostics(wxCommandEvent &event);
    void on_open_folder(wxCommandEvent &event);

    // Runs the two LAN calls. Returns a filled result; never throws, and reports a missing
    // library through unavailable_reason rather than failing.
    FFConnectionTestResult run_test();

    // Collects the log files and writes the archive. Returns the path, or an empty string on
    // failure with the reason in `error`.
    wxString write_diagnostics_zip(const FFConnectionTestResult &test, wxString &error);

    void show_result(const std::string &summary, const std::string &detail, bool ok);

    // ::TextInput deliberately: other GUI headers (Preferences.hpp, GUI_ObjectTable.hpp) declare a
    // Slic3r::GUI::TextInput of their own, and an unqualified name here binds to that phantom.
    ::TextInput  *m_serial  { nullptr };
    ::TextInput  *m_ip      { nullptr };
    ::TextInput  *m_code    { nullptr };
    wxStaticText *m_status  { nullptr };
    wxTextCtrl   *m_detail  { nullptr };
    wxButton     *m_test_btn{ nullptr };
    wxButton     *m_diag_btn{ nullptr };
    wxButton     *m_open_btn{ nullptr };

    unsigned short m_port { 8898 };
    wxString       m_last_zip_path;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FFDiagnosticsDialog_hpp_
