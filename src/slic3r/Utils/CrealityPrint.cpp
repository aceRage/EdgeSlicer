#include "CrealityPrint.hpp"
#include "CrealityPrintModel.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
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
    m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke")),
    m_printer_model(config->opt_string("printer_model"))
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
    if (!CrealityPrintModel::omit_upload_path(effective_model()))
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

struct CrealityWsClient::Impl
{
    net::io_context                                            ioc;
    std::unique_ptr<websocket::stream<beast::tcp_stream>>      ws;
    int                                                        step_ms  = 5000;
    int                                                        total_ms = 0;
    std::chrono::steady_clock::time_point                      start    = std::chrono::steady_clock::now();
    bool                                                       open     = false;
    bool                                                       timed_out = false;

    int remaining() const
    {
        if (total_ms <= 0)
            return step_ms;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        const auto left    = static_cast<long long>(total_ms) - static_cast<long long>(elapsed);
        return left > 0 ? static_cast<int>(left) : 0;
    }

    int budget() const
    {
        if (total_ms <= 0)
            return std::max(1, step_ms);
        return std::max(0, std::min(step_ms, remaining()));
    }

    template<class Cancel> bool run(bool &done, int timeout_ms, Cancel on_timeout)
    {
        if (timeout_ms <= 0) {
            on_timeout();
            timed_out = true;
            open      = false;
            return false;
        }
        ioc.restart();
        ioc.run_for(std::chrono::milliseconds(timeout_ms));
        if (done)
            return true;
        on_timeout();
        ioc.restart();
        ioc.run();
        timed_out = true;
        open      = false;
        return false;
    }

    void cancel_socket()
    {
        if (!ws)
            return;
        beast::error_code ignore;
        beast::get_lowest_layer(*ws).socket().cancel(ignore);
        beast::get_lowest_layer(*ws).socket().close(ignore);
    }

    bool fail(std::string &error, const char *step, const beast::error_code &ec)
    {
        if (timed_out)
            error = std::string(step) + ": timed out";
        else
            error = std::string(step) + ": " + ec.message();
        timed_out = false;
        open      = false;
        return false;
    }
};

CrealityWsClient::CrealityWsClient(int step_timeout_ms, int total_timeout_ms) : m(std::make_unique<Impl>())
{
    m->step_ms  = std::max(1, step_timeout_ms);
    m->total_ms = std::max(0, total_timeout_ms);
    m->start    = std::chrono::steady_clock::now();
}

CrealityWsClient::~CrealityWsClient() { close(); }

bool CrealityWsClient::is_open() const { return m && m->open; }

int CrealityWsClient::remaining_ms() const { return m ? m->remaining() : 0; }

bool CrealityWsClient::open(const std::string &host_url, const std::string &port, std::string &error)
{
    close();
    std::string host = CrealityPrintModel::ws_host(host_url);
    if (host.empty()) {
        error = "empty print_host";
        return false;
    }

    m->start     = std::chrono::steady_clock::now();
    m->timed_out = false;
    m->ws        = std::make_unique<websocket::stream<beast::tcp_stream>>(m->ioc);
    m->ws->read_message_max(1024 * 1024);
    m->ws->set_option(websocket::stream_base::decorator([](websocket::request_type &req) {
        req.set(http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " creality-ws");
    }));

    tcp::resolver               resolver(m->ioc);
    tcp::resolver::results_type results;
    beast::error_code           ec;
    bool                        done = false;
    resolver.async_resolve(host, port, [&](const beast::error_code &e, tcp::resolver::results_type r) {
        ec      = e;
        results = std::move(r);
        done    = true;
    });
    if (!m->run(done, m->budget(), [&] { resolver.cancel(); }) || ec)
        return m->fail(error, "resolve", ec);

    done = false;
    beast::get_lowest_layer(*m->ws).async_connect(results, [&](const beast::error_code &e, const tcp::endpoint &) {
        ec   = e;
        done = true;
    });
    if (!m->run(done, m->budget(), [&] { m->cancel_socket(); }) || ec)
        return m->fail(error, "connect", ec);

    try {
        host += ':' + std::to_string(beast::get_lowest_layer(*m->ws).socket().remote_endpoint().port());
    } catch (...) { host += ":9999"; }

    done = false;
    m->ws->async_handshake(host, "/", [&](const beast::error_code &e) {
        ec   = e;
        done = true;
    });
    if (!m->run(done, m->budget(), [&] { m->cancel_socket(); }) || ec)
        return m->fail(error, "handshake", ec);

    // K1-family firmware requires TEXT-mode WebSocket frames; K2-family accepts both.
    m->ws->text(true);
    m->open = true;
    return true;
}

bool CrealityWsClient::write(const std::string &payload, std::string &error)
{
    if (!m || !m->open || !m->ws) {
        error = "not connected";
        return false;
    }
    beast::error_code ec;
    bool              done = false;
    m->ws->async_write(net::buffer(payload), [&](const beast::error_code &e, std::size_t) {
        ec   = e;
        done = true;
    });
    if (!m->run(done, m->budget(), [&] { m->cancel_socket(); }) || ec)
        return m->fail(error, "write", ec);
    return true;
}

bool CrealityWsClient::read_once(std::string &out, std::string &error)
{
    if (!m || !m->open || !m->ws) {
        error = "not connected";
        return false;
    }
    beast::flat_buffer buffer;
    beast::error_code  ec;
    bool               done = false;
    m->ws->async_read(buffer, [&](const beast::error_code &e, std::size_t) {
        ec   = e;
        done = true;
    });
    if (!m->run(done, m->budget(), [&] { m->cancel_socket(); }) || ec)
        return m->fail(error, "read", ec);
    out = beast::buffers_to_string(buffer.data());
    return true;
}

bool CrealityWsClient::read_until(const std::string &expected_key, std::string &out, std::string &error)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(m->budget());
    while (std::chrono::steady_clock::now() < until) {
        std::string frame;
        if (!read_once(frame, error))
            return false;
        if (frame.find("heart_beat") != std::string::npos) {
            std::string wr_err;
            write("ok", wr_err);
            continue;
        }
        if (frame.find(expected_key) != std::string::npos) {
            out = std::move(frame);
            return true;
        }
    }
    error = "read: no '" + expected_key + "' before deadline";
    return false;
}

void CrealityWsClient::close()
{
    if (!m || !m->ws)
        return;
    if (m->open) {
        beast::error_code ec;
        bool              done = false;
        m->ws->async_close(websocket::close_code::normal, [&](const beast::error_code &e) {
            ec   = e;
            done = true;
        });
        m->run(done, std::min(1000, std::max(1, m->budget())), [&] { m->cancel_socket(); });
    }
    m->cancel_socket();
    m->open = false;
    m->ws.reset();
    m->ioc.restart();
    m->ioc.poll();
}

std::string CrealityPrint::effective_model() const { return CrealityPrintModel::resolve_model(m_model, m_printer_model); }

bool CrealityPrint::fetch_info(const std::string &host, std::string &body, std::string &error, int timeout_s)
{
    if (host.empty()) {
        error = "empty print_host";
        return false;
    }
    std::string url = host;
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0)
        url = "http://" + url;
    if (!url.empty() && url.back() == '/')
        url.pop_back();
    url += "/info";

    bool        ok        = false;
    const int   wait      = std::max(1, timeout_s);
    auto        http      = Http::get(std::move(url));
    http.tls_policy(Http::TlsPolicy::PrintHost)
        .timeout_connect(wait)
        .timeout_max(wait)
        .size_limit(512 * 1024)
        .on_error([&](std::string reply, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
            if (!reply.empty())
                body = std::move(reply);
        })
        .on_complete([&](std::string reply, unsigned) {
            body = std::move(reply);
            ok   = true;
        })
        .perform_sync();
    if (!ok && error.empty())
        error = "no answer";
    return ok;
}

bool CrealityPrint::start_print(wxString &msg, const std::string &filename) const
{
    try {
        const std::string gcode_path = CrealityPrintModel::gcode_path(effective_model(), filename);
        const json        cmd        = {{"method", "set"}, {"params", {{"opGcodeFile", "printprt:" + gcode_path}}}};

        CrealityWsClient ws(5000);
        std::string      err;
        if (!ws.open(m_host, "9999", err)) {
            BOOST_LOG_TRIVIAL(error) << "CrealityPrint: start_print open failed: " << err;
            msg = wxString::FromUTF8(err);
            return false;
        }
        if (!ws.write(to_string(cmd), err)) {
            BOOST_LOG_TRIVIAL(error) << "CrealityPrint: start_print write failed: " << err;
            msg = wxString::FromUTF8(err);
            return false;
        }
        // K1-family firmware closes the WebSocket right after accepting the start command.
        // The command is delivered by write(); a failed read is not a start failure.
        std::string unused;
        ws.read_once(unused, err);
        ws.close();
        return true;
    } catch (std::exception const &e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Error starting print: " << e.what();
        msg = wxString::FromUTF8(e.what());
        return false;
    }
}

}
