#ifndef slic3r_ElegooLink_hpp_
#define slic3r_ElegooLink_hpp_

#include <map>
#include <string>
#include <wx/string.h>
#include <boost/optional.hpp>
#include <boost/asio/ip/address.hpp>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "OctoPrint.hpp"
#include "WebSocketClient.hpp"
namespace Slic3r {

class DynamicPrintConfig;
class Http;

class ElegooLink : public OctoPrint
{
public:
    ElegooLink(DynamicPrintConfig *config);
    ~ElegooLink() override = default;
    const char* get_name() const override;
    virtual bool test(wxString &curl_msg) const override;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString& msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    // A Centauri Carbon 2 / Centauri 2: HTTP /system/info + PUT /upload + an MQTT start command
    // (ElegooCc2.hpp) instead of the CC1's SDCP. Decided by the preset's printer_model.
    bool is_cc2() const { return m_cc2; }
protected:
#ifdef WIN32
    virtual bool upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const;
#endif
    virtual bool validate_version_text(const boost::optional<std::string> &version_text) const;
    virtual bool upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const;

#ifdef WIN32
    virtual bool test_with_resolved_ip(wxString& curl_msg) const override;
    bool elegoo_test_with_resolved_ip(wxString& curl_msg) const;
#endif

private:
    bool m_cc2 { false };

    bool elegoo_test(wxString& curl_msg) const;
    bool cc2_test(wxString& curl_msg) const;
    bool cc2_upload(const std::string& url, PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const;
    bool print(WebSocketClient&                            client,
               const std::map<std::string, std::string>&   extended_info,
               const std::string                           filename, ErrorFn error_fn) const;
    bool checkResult(WebSocketClient&  client,
               ErrorFn           error_fn) const;

    bool loopUpload(std::string url, PrintHostUpload upload_data,
                    ProgressFn        prorgess_fn,
                    ErrorFn           error_fn,
                    InfoFn            info_fn) const;

    bool uploadPart(Http &http,
                    std::string       md5,
                    std::string       uuid,
                    std::string       path,
                    std::string       filename,
                    size_t            filesize,
                    size_t            offset,
                    size_t            length,
                    ProgressFn        prorgess_fn,
                    ErrorFn           error_fn,
                    InfoFn            info_fn) const;
};
}

#endif
