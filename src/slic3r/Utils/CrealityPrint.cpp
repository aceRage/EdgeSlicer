#include "CrealityPrint.hpp"
#include "CrealityPrintModel.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
#include <boost/foreach.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/convert.hpp>

#include <curl/curl.h>
#include <wx/progdlg.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "Bonjour.hpp"
#include "slic3r/GUI/BonjourDialog.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

#include <fstream>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
using std::to_string;

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {

CrealityPrint::CrealityPrint(DynamicPrintConfig* config) : 
    m_host(config->opt_string("print_host")), 
    m_web_ui(config->opt_string("print_host_webui")),
    m_cafile(config->opt_string("printhost_cafile")),
    m_port(config->opt_string("printhost_port")),
    m_apikey(config->opt_string("printhost_apikey")),
    m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{}

const char* CrealityPrint::get_name() const { return "Creality Print"; }

std::string CrealityPrint::get_host() const {
    return m_host;
}
void  CrealityPrint::set_auth(Http& http) const
{
    http.header("Authorization", "Bearer " + m_apikey);
    if (!m_cafile.empty()) {
        http.ca_file(m_cafile);
    }
}

wxString CrealityPrint::get_test_ok_msg() const { return _(L("Connected to CrealityPrint successfully!")); }

wxString CrealityPrint::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to CrealityPrint"), msg.Truncate(256));
}

bool CrealityPrint::test(wxString& msg) const
{ 
    bool res = true;
    const char* name = get_name();
    auto url = make_url("info");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;
    // Here we do not have to add custom "Host" header - the url contains host filled by user and libCurl will set the header by itself.
    auto http = Http::get(std::move(url));
    apply_tls(http); // not verified unless the printer has a CA file (printhost_cafile)
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status %
                                            body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got version: %2%") % name % body;
            const std::string model = CrealityPrintModel::parse_model_from_info(body);
            if (!model.empty()) {
                m_model = model;
                BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Detected model: %2%") % name % m_model;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .on_ip_resolve([&](std::string address) {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Remember resolved address to be reused at successive REST API call.
            msg = GUI::from_u8(address);
        })
#endif // WIN32
        .perform_sync();

    return res;
}

PrintHostPostUploadActions CrealityPrint::get_post_upload_actions() const {
    return PrintHostPostUploadAction::StartPrint; 
}

bool CrealityPrint::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{   
    const char* name = get_name();
    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();
    wxString test_msg;
    if (!test(test_msg)) {
        error_fn(std::move(test_msg));
        return false;
    }

    bool res = true;
    const auto safe_upload_filename = safe_filename(upload_filename.string());
    // Only encode the URL path segment; keep the multipart filename and start-print path as the stored filename.
    auto url = make_url("upload/" + Http::url_encode(safe_upload_filename));

    auto  http = Http::post(url); // std::move(url));
    apply_tls(http); // not verified unless the printer has a CA file (printhost_cafile)
    set_auth(http);
    // Upstream Orca #14089: CFS-capable models reject (or ignore) the multipart path field.
    if (!CrealityPrintModel::omit_upload_path(m_model))
        http.form_add("path", upload_parent_path.string());
    http.form_add_file("file", upload_data.source_path.string(), safe_upload_filename)

        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;

            if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
                wxString errormsg;
                if (!start_print(errormsg, safe_upload_filename)) {
                    error_fn(std::move(errormsg));
                    res = false;
                }
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file to %2%: %3%, HTTP %4%, body: `%5%`") % name % url % error %
                                            status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                res = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();
    return res;
}

std::string CrealityPrint::make_url(const std::string &path) const
{
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0) {
        if (m_host.back() == '/') {
            return (boost::format("%1%%2%") % m_host % path).str();
        } else {
            return (boost::format("%1%/%2%") % m_host % path).str();
        }
    } else {
        return (boost::format("http://%1%/%2%") % m_host % path).str();
    }
}

std::string CrealityPrint::safe_filename(const std::string &filename) const
{
    std::string safe_filename = filename;
    std::replace(safe_filename.begin(), safe_filename.end(), ' ', '_');

    return safe_filename;
}

static void ws_connect(net::io_context &ioc, websocket::stream<beast::tcp_stream> &ws, const std::string &host_url, const std::string &port)
{
    // Upstream Http::get_host_from_url; this fork's Http::url_host strips scheme and port.
    std::string host = CrealityPrintModel::ws_host(host_url);
    if (host.empty())
        throw std::runtime_error("CrealityPrint: empty print_host");

    tcp::resolver resolver{ioc};
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(5));
    auto const results = resolver.resolve(host, port);
    beast::get_lowest_layer(ws).connect(results);
    host += ':' + std::to_string(beast::get_lowest_layer(ws).socket().remote_endpoint().port());

    ws.set_option(websocket::stream_base::decorator([](websocket::request_type &req) {
        req.set(http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " websocket-client-coro");
    }));
    ws.handshake(host, "/");
    // K1-family firmware requires TEXT-mode WebSocket frames; K2-family accepts both.
    ws.text(true);
}

bool CrealityPrint::start_print(wxString &msg, const std::string &filename) const
{
    try {
        const std::string gcode_path = CrealityPrintModel::gcode_path(m_model, filename);

        json j2 = {{"method", "set"}, {"params", {{"opGcodeFile", "printprt:" + gcode_path}}}};

        net::io_context                      ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        ws_connect(ioc, ws, m_host, "9999");

        ws.write(net::buffer(to_string(j2)));

        // K1-family firmware closes the WebSocket right after accepting the
        // start command, so a blocking read here surfaces a benign
        // "End of file [asio.misc:2]" even though the print already started
        // (the command is delivered by write()). Read best-effort, ignore errors.
        beast::flat_buffer buffer;
        beast::error_code  read_ec;
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(3));
        ws.read(buffer, read_ec);

        // Same reason: the printer may have already closed the connection. A close
        // error here is not a failure - the start command was sent above.
        beast::error_code close_ec;
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(3));
        ws.close(websocket::close_code::normal, close_ec);
        return true;
    } catch (std::exception const &e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Error starting print: " << e.what();
        msg = wxString::FromUTF8(e.what());
        return false;
    }
}

}
