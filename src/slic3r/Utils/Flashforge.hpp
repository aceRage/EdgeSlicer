#ifndef slic3r_FlashForge_hpp_
#define slic3r_FlashForge_hpp_

#include <vector>
#include <string>
#include <wx/string.h>
#include "PrintHost.hpp"
#include "SerialMessage.hpp"
#include "SerialMessageType.hpp"
#include "../../libslic3r/PrintConfig.hpp"

namespace Slic3r {
class DynamicPrintConfig;
class Http;

struct FlashforgeMaterialSlot
{
    int         slot_id {0}; // API is 1-based.
    bool        has_filament {false};
    std::string material_name;
    std::string material_color;
};

struct FlashforgeDiscoveredPrinter
{
    std::string name;
    std::string serial_number;
    std::string ip_address;
};

// The pure parts of the port-8898 local API: the pieces that only look at bytes the printer sent
// (or at a name the user typed) and decide what they mean. They live here rather than in an
// anonymous namespace inside Flashforge.cpp so tests can reach them without a printer, a socket or
// wx - see tests/slic3rutils/flashforge_tests.cpp.
namespace FlashforgeLocalApi {

// The AD5-series decision. The HTTP local API can only be spoken with a serial number AND a check
// code in hand, so a printer missing either stays on the legacy TCP-8899 M-code console. This is
// the single place that answer is made; test(), upload() and the send dialog all defer to it.
bool uses_local_api(const std::string& serial_number, const std::string& check_code);

// "http://1.2.3.4:8898/detail" out of whatever the user put in the Hostname field.
std::string host_name_of(const std::string& host);
std::string url_for(const std::string& host, const std::string& path);

// The printer rejects '=' and friends in an upload name; everything outside [A-Za-z0-9._-] becomes
// '_', and an empty name becomes "print" plus the fallback extension.
std::string sanitize_filename(const std::string& filename, const std::string& fallback_extension = {});

// `code`/`err` != 0 in a /detail or /uploadGcode reply is a printer-side refusal and the message it
// carries is what the user must see. Returns false and fills error_text then, and also on a body
// that is not a JSON object at all.
bool validate_response(const std::string& response_body, std::string& error_text);

// The IFS (material station) part of a /detail reply: how many slots, what is in them, and whether
// this printer reports a station at all. Returns false only on a body that will not parse.
bool parse_detail_material_slots(const std::string&                   response_body,
                                 std::vector<FlashforgeMaterialSlot>& slots,
                                 bool&                                supports_material_station);

} // namespace FlashforgeLocalApi

class Flashforge : public PrintHost
{
public:
    explicit Flashforge(DynamicPrintConfig *config);
    ~Flashforge() override = default;

    const char *get_name() const override;

    bool                       test(wxString &curl_msg) const override;
    wxString                   get_test_ok_msg() const override;
    wxString                   get_test_failed_msg(wxString &msg) const override;
    bool                       upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool                       has_auto_discovery() const override { return true; }
    bool                       can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string                get_host() const override { return m_host; }
    bool                       fetch_material_slots(std::vector<FlashforgeMaterialSlot>& slots, bool* supports_material_station, wxString& msg) const;
    static bool                discover_printers(std::vector<FlashforgeDiscoveredPrinter>& printers, wxString& msg, int timeout_ms = 10000, int idle_timeout_ms = 1500, int max_retries = 3);

private:
    std::string m_host;
    std::string m_serial_number;
    std::string m_check_code;
    std::string m_console_port;
    const int m_bufferSize;
    GCodeFlavor m_gcFlavor;
    Slic3r::Utils::SerialMessage controlCommand          = {"~M601 S1\r\n",Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage connectKlipperCommand   = {"~M640\r\n",Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage connectLegacyCommand    = {"~M650\r\n",Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage nozzlePosCommand        = {"~M114\r\n", Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage deviceInfoCommand       = {"~M115\r\n", Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage statusCommand           = {"~M119\r\n",Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage tempStatusCommand       = {"~M105\r\n", Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage printStatusCommand      = {"~M27\r\n", Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage saveFileCommand         = {"~M29\r\n",Slic3r::Utils::Command};
    bool upload_local_api(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn) const;
    bool test_local_api(wxString& msg) const;
    bool request_local_api_json(const std::string& path, const std::string& body, std::string& response_body, wxString& error_msg) const;
    std::string make_http_url(const std::string& path) const;
    std::string extract_host_name() const;
    int  get_err_code_from_body(const std::string &body) const;
    bool connect(wxString& msg) const;
    bool start_print(wxString& msg, const std::string& filename) const;
};

} // namespace Slic3r

#endif
