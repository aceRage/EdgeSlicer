#ifndef slic3r_AnycubicLanHost_hpp_
#define slic3r_AnycubicLanHost_hpp_

#include "PrintHost.hpp"

#include <string>
#include <wx/string.h>

namespace Slic3r {

class DynamicPrintConfig;

// PrintHost for Anycubic stock-firmware LAN mode (developer spike). Off by default: the host
// type is not in any profile and is offered in the devices editor only when Develop mode is on.
class AnycubicLanHost : public PrintHost
{
public:
    explicit AnycubicLanHost(DynamicPrintConfig *config);
    ~AnycubicLanHost() override = default;

    const char *get_name() const override;
    bool        test(wxString &msg) const override;
    wxString    get_test_ok_msg() const override;
    wxString    get_test_failed_msg(wxString &msg) const override;
    bool        upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool        has_auto_discovery() const override { return false; }
    bool        can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }

private:
    std::string m_host;
};

} // namespace Slic3r

#endif
