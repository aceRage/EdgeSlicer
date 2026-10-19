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
                if (!start_print(errormsg, safe_upload_filename, upload_data.extended_info)) {
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

static void ws_connect(net::io_context &ioc, websocket::stream<beast::tcp_stream> &ws, const std::string &host_url, const std::string &port,
                       int timeout_s)
{
    // Upstream Http::get_host_from_url; this fork's Http::url_host strips scheme and port.
    std::string host = CrealityPrintModel::ws_host(host_url);
    if (host.empty())
        throw std::runtime_error("CrealityPrint: empty print_host");

    const int wait = std::max(1, timeout_s);
    tcp::resolver resolver{ioc};
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(wait));
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

static std::string ws_send_and_read(websocket::stream<beast::tcp_stream> &ws, const json &cmd, const std::string &expected_key, int max_reads = 20)
{
    ws.write(net::buffer(to_string(cmd)));

    for (int reads = 0, total = 0; reads < max_reads && total < max_reads * 3; ++total) {
        beast::flat_buffer buf;
        beast::error_code  ec;
        ws.read(buf, ec);
        if (ec == net::error::would_block || ec == beast::error::timeout)
            break;
        if (ec)
            throw beast::system_error{ec};
        std::string msg = beast::buffers_to_string(buf.data());
        // K1-family firmware sends periodic heartbeat pings; ack them so the
        // printer does not close the connection, then keep reading.
        if (msg.find("heart_beat") != std::string::npos) {
            beast::error_code wr_ec;
            ws.write(net::buffer(std::string("ok")), wr_ec);
            continue;
        }
        ++reads;
        if (msg.find(expected_key) != std::string::npos)
            return msg;
    }
    BOOST_LOG_TRIVIAL(warning) << "CrealityPrint: No '" << expected_key << "' response after " << max_reads << " messages";
    return {};
}

std::string CrealityPrint::query_boxes_info(const std::string &host, int timeout_s)
{
    try {
        net::io_context                      ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        ws_connect(ioc, ws, host, "9999", timeout_s);

        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(std::max(1, timeout_s)));
        json        boxs_query = {{"method", "get"}, {"params", {{"boxsInfo", 1}}}};
        std::string result     = ws_send_and_read(ws, boxs_query, "boxsInfo");

        beast::error_code close_ec;
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(1));
        ws.close(websocket::close_code::normal, close_ec);
        return result;
    } catch (std::exception const &e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Failed to query boxsInfo: " << e.what();
        return {};
    }
}

std::string CrealityPrint::query_boxes_info(int timeout_s) const { return query_boxes_info(m_host, timeout_s); }

static int enable_self_test_of(const std::map<std::string, std::string> &extended_info)
{
    const auto it = extended_info.find("enableSelfTest");
    if (it == extended_info.end() || it->second.empty())
        return 0;
    try {
        const int n = std::stoi(it->second);
        return n == 0 ? 0 : 1;
    } catch (...) {
        return 0;
    }
}

static bool use_cfs_of(const std::map<std::string, std::string> &extended_info)
{
    const auto it = extended_info.find("useCFS");
    return it != extended_info.end() && it->second == "1";
}

bool CrealityPrint::start_print(wxString &msg, const std::string &filename, const std::map<std::string, std::string> &extended_info) const
{
    try {
        const std::string gcode_path        = CrealityPrintModel::gcode_path(m_model, filename);
        const int         enable_self_test  = enable_self_test_of(extended_info);
        const bool        use_cfs           = use_cfs_of(extended_info);

        net::io_context                      ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        ws_connect(ioc, ws, m_host, "9999", 5);

        if (use_cfs) {
            json color_list = json::array();
            for (int i = 0;; ++i) {
                const auto it = extended_info.find("colorMatch_" + std::to_string(i));
                if (it == extended_info.end())
                    break;
                json item = CrealityPrintModel::color_match_item_from_value(it->second);
                if (!item.empty())
                    color_list.push_back(std::move(item));
            }

            const json color_match = CrealityPrintModel::color_match_cmd(gcode_path, color_list);
            // Creality Print retries colorMatch; two extra attempts cover a dropped TEXT frame.
            bool matched = false;
            for (int attempt = 0; attempt < 3 && !matched; ++attempt) {
                try {
                    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(5));
                    const std::string reply = ws_send_and_read(ws, color_match, "colorMatch", 8);
                    matched = !reply.empty() || attempt == 2;
                } catch (std::exception const &e) {
                    BOOST_LOG_TRIVIAL(warning) << "CrealityPrint: colorMatch attempt " << (attempt + 1) << " failed: " << e.what();
                    if (attempt == 2)
                        throw;
                }
            }

            const json multi = CrealityPrintModel::multi_color_print_cmd(gcode_path, enable_self_test);
            beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(5));
            ws.write(net::buffer(to_string(multi)));
        } else {
            const json cmd = CrealityPrintModel::op_gcode_file_cmd(gcode_path, enable_self_test);
            ws.write(net::buffer(to_string(cmd)));

            // K1-family firmware closes the WebSocket right after accepting the
            // start command, so a blocking read here surfaces a benign
            // "End of file [asio.misc:2]" even though the print already started
            // (the command is delivered by write()). Read best-effort, ignore errors.
            beast::flat_buffer buffer;
            beast::error_code  read_ec;
            beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(3));
            ws.read(buffer, read_ec);
        }

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
