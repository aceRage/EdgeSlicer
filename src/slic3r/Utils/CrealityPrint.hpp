#ifndef slic3r_CrealityPrint_hpp_
#define slic3r_CrealityPrint_hpp_

#include <map>
#include <memory>
#include <string>
#include <wx/string.h>
#include <boost/optional.hpp>
#include <boost/asio/ip/address.hpp>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class Http;

// Deadline-bound WebSocket to the K-series port-9999 firmware API.
// tcp_stream::expires_after does nothing on synchronous Beast calls, so every
// step (resolve, connect, handshake, write, read, close) is async and run with
// io_context::run_for. A step that expires closes the socket and drains handlers.
// Shared by start_print, the CFS probe, and CFS filament sync.
class CrealityWsClient
{
public:
    // step_timeout_ms caps one resolve/connect/handshake/write/read.
    // total_timeout_ms is the overall budget from construction (0 = per-step only).
    explicit CrealityWsClient(int step_timeout_ms, int total_timeout_ms = 0);
    ~CrealityWsClient();
    CrealityWsClient(const CrealityWsClient &)            = delete;
    CrealityWsClient &operator=(const CrealityWsClient &) = delete;

    bool open(const std::string &host_url, const std::string &port, std::string &error);
    bool write(const std::string &payload, std::string &error);
    // Read frames until one contains expected_key. Heartbeat pings are acked with "ok".
    bool read_until(const std::string &expected_key, std::string &out, std::string &error);
    // One frame, best-effort. False on timeout or a closed peer (not always an error).
    bool read_once(std::string &out, std::string &error);
    void close();
    bool is_open() const;
    int  remaining_ms() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

class CrealityPrint : public PrintHost
{
public:
    CrealityPrint(DynamicPrintConfig* config);
    ~CrealityPrint() override = default;

    const char* get_name() const override;
    virtual bool can_test() const { return true; };
    std::string  get_host() const override;
    // Browse uses the stock Bonjour dialog (OctoPrint-style names). K-series
    // printers announce `_Creality-<id>._udp.local.` and need a DNS-SD meta-query
    // (upstream CrealityHostDiscovery + vendored deps_src/mdns). That dependency
    // is not added in this stack; see CrealityHostDiscovery.hpp.
    bool has_auto_discovery() const override { return true; }

    wxString                           get_test_ok_msg() const override;
    wxString                           get_test_failed_msg(wxString& msg) const override;
    virtual bool                       test(wxString& curl_msg) const override;
    PrintHostPostUploadActions         get_post_upload_actions() const;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;

    // GET /info with a real connect+read timeout. Empty host returns false.
    static bool fetch_info(const std::string &host, std::string &body, std::string &error, int timeout_s);
    // Port-9999 WebSocket `get boxsInfo`. timeout_s is the total deadline (probe uses ~1 s).
    // Empty on timeout or a refused handshake. Uses CrealityWsClient.
    static std::string query_boxes_info(const std::string& host, int timeout_s);
    std::string        query_boxes_info(int timeout_s = 10) const;

protected:
    virtual void set_auth(Http& http) const;
private:
    std::string m_host;
    std::string m_port;
    std::string m_apikey;
    std::string m_cafile;
    std::string m_web_ui;
    bool        m_ssl_revoke_best_effort;
    mutable std::string m_model;
    std::string         m_printer_model;

    std::string make_url(const std::string& path) const;
    bool start_print(wxString& msg, const std::string& filename, const std::map<std::string, std::string>& extended_info) const;
    std::string safe_filename(const std::string& filename) const;
    std::string effective_model() const;
};
} // namespace Slic3r

#endif
