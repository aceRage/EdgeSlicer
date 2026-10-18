#ifndef slic3r_MoonrakerHost_hpp_
#define slic3r_MoonrakerHost_hpp_

#include <string>
#include <vector>
#include <wx/string.h>

#include "PrintHost.hpp"

class wxArrayString;

namespace Slic3r {

class DynamicPrintConfig;
class Http;

// Native Moonraker HTTP print host (Klipper's API server: Fluidd/Mainsail, Sovol SV07/SV08/Zero,
// and any other box that speaks Moonraker without `[octoprint_compat]`).
//
// Distinct from Snapmaker's `Moonraker` / `Moonraker_Mqtt` classes (MoonRaker.hpp) and from the
// `htMoonRaker` / `htMoonRaker_mqtt` enum members those use. Those have no config key and are
// the Device-tab Snapmaker path. This host is `htMoonrakerHttp`, config key `"moonraker"`.
//
// Endpoints (https://moonraker.readthedocs.io/en/latest/web_api/):
//   GET  /server/info                     -- connection test (result.klippy_state)
//   GET  /printer/info                    -- fallback test used by some shims
//   GET  /server/files/roots              -- storage picker; optional, older Moonraker 404s
//   POST /server/files/upload (multipart) -- form: file, root, print[, plateindex]
//   POST /printer/print/start (json)      -- {"filename":"..."} if upload did not start/queue
//
// Auth: X-Api-Key when printhost_apikey is set. No HTTP Basic/Digest.
class MoonrakerHost : public PrintHost
{
public:
    explicit MoonrakerHost(DynamicPrintConfig *config);
    ~MoonrakerHost() override = default;

    const char *get_name() const override;

    bool                         test(wxString &curl_msg) const override;
    wxString                     get_test_ok_msg() const override;
    wxString                     get_test_failed_msg(wxString &msg) const override;
    bool                         upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool                         has_auto_discovery() const override { return false; }
    bool                         can_test() const override { return true; }
    PrintHostPostUploadActions   get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string                  get_host() const override { return m_host; }
    bool                         get_storage(wxArrayString &storage_path, wxArrayString &storage_name) const override;
    const std::string           &get_apikey() const { return m_apikey; }
    const std::string           &get_cafile() const { return m_cafile; }

protected:
    std::string m_host;
    std::string m_apikey;
    std::string m_cafile;

    void        set_auth(Http &http) const;
    std::string make_url(const std::string &path) const;
    bool        start_print(wxString &error_msg, const std::string &filename) const;
};

// Network-free parsers so Catch2 can cover the Moonraker reply shapes without a printer.
bool        moonraker_host_is_server_info(const std::string &body);
bool        moonraker_host_is_printer_info(const std::string &body);
// Roots whose permissions include 'w' (rw / rwd). Empty when the body is not a roots list.
std::vector<std::string> moonraker_host_writable_roots(const std::string &body);

struct MoonrakerHostUploadReply
{
    std::string stored_path;                 // item.path, or empty when the body had none
    bool        started_or_queued { false }; // print_started || print_queued
};

MoonrakerHostUploadReply moonraker_host_parse_upload_reply(const std::string &body);

} // namespace Slic3r

#endif
