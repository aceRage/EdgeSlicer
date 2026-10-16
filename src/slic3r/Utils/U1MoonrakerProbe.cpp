#include "U1MoonrakerProbe.hpp"

#include "Http.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>

namespace Slic3r {
namespace U1MoonrakerProbe {

using nlohmann::json;
namespace beast     = boost::beast;
namespace websocket = boost::beast::websocket;
namespace net       = boost::asio;
using tcp           = boost::asio::ip::tcp;

static long long ms_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

const std::vector<std::string>& read_only_methods()
{
    static const std::vector<std::string> m { "server.info", "printer.info", "printer.objects.query" };
    return m;
}

bool is_read_only_method(const std::string& method)
{
    const auto& m = read_only_methods();
    return std::find(m.begin(), m.end(), method) != m.end();
}

const std::vector<std::string>& http_paths()
{
    static const std::vector<std::string> p { "/server/info", "/printer/info", "/printer/objects/query?print_stats&extruder" };
    return p;
}

std::string request_for(const std::string& method, long long id)
{
    if (!is_read_only_method(method))
        return {};
    json req = { { "jsonrpc", "2.0" }, { "method", method }, { "id", id } };
    if (method == "printer.objects.query")
        req["params"] = { { "objects", { { "print_stats", nullptr }, { "extruder", nullptr } } } };
    return req.dump();
}

static std::string clip(std::string s, size_t n = 120)
{
    for (char& c : s)
        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
    return s.size() > n ? s.substr(0, n) + "..." : s;
}

std::string describe_answer(const std::string& body)
{
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded())
        return body.empty() ? "empty answer" : "not JSON (" + std::to_string(body.size()) + " bytes)";
    if (j.is_object() && j.contains("error")) {
        const json& e = j["error"];
        if (e.is_object())
            return "error " + (e.contains("code") ? e["code"].dump() : std::string("?")) + ": " +
                   clip(e.contains("message") && e["message"].is_string() ? e["message"].get<std::string>() : e.dump());
        return "error: " + clip(e.dump());
    }
    const json* r = j.is_object() && j.contains("result") ? &j["result"] : &j;
    if (!r->is_object())
        return "result: " + clip(r->dump(), 40);
    std::string keys;
    int         n = 0;
    for (const auto& it : r->items()) {
        if (n++ == 8) {
            keys += ", ...";
            break;
        }
        keys += (keys.empty() ? "" : ", ") + it.key();
    }
    std::string out = "result keys: " + keys;
    if (r->contains("klippy_state") && (*r)["klippy_state"].is_string())
        out += "; klippy_state=" + (*r)["klippy_state"].get<std::string>();
    if (r->contains("state") && (*r)["state"].is_string())
        out += "; state=" + (*r)["state"].get<std::string>();
    if (r->contains("status") && (*r)["status"].is_object() && (*r)["status"].contains("print_stats") &&
        (*r)["status"]["print_stats"].is_object() && (*r)["status"]["print_stats"].contains("state"))
        out += "; print_stats.state=" + (*r)["status"]["print_stats"]["state"].dump();
    return out;
}

// ---------------------------------------------------------------- HTTP ----

static Step http_step(const std::string& host, int port, const std::string& path, int timeout_s)
{
    Step s;
    s.name        = "GET :" + std::to_string(port) + path;
    const auto t0 = std::chrono::steady_clock::now();
    Http::get("http://" + host + ":" + std::to_string(port) + path)
        .timeout_connect(timeout_s)
        .timeout_max(timeout_s + 1)
        .on_complete([&s](std::string body, unsigned status) {
            s.ok     = status >= 200 && status < 300;
            s.status = (int) status;
            s.detail = describe_answer(body);
        })
        .on_error([&s](std::string body, std::string error, unsigned status) {
            s.status = (int) status;
            s.detail = status ? ("HTTP " + std::to_string(status) + ": " + describe_answer(body)) : clip(error);
        })
        .perform_sync();
    s.ms = ms_since(t0);
    return s;
}

// ----------------------------------------------------------- WebSocket ----

// Runs one async operation on `ioc` with the stream's deadline; the result's error code.
template<class Start> static beast::error_code run_op(net::io_context& ioc, Start&& start)
{
    beast::error_code ec = net::error::would_block;
    start([&ec](beast::error_code e, auto&&...) { ec = e; });
    ioc.restart();
    ioc.run();
    return ec;
}

static void ws_steps(const std::string& host, int port, int timeout_s, std::vector<Step>& out)
{
    const std::string base = "WS :" + std::to_string(port) + "/websocket";
    Step              conn;
    conn.name     = base + " upgrade";
    const auto t0 = std::chrono::steady_clock::now();
    try {
        net::io_context                      ioc;
        tcp::resolver                        resolver(ioc);
        websocket::stream<beast::tcp_stream> ws(ioc);
        beast::error_code                    ec;
        const auto                           results = resolver.resolve(host, std::to_string(port), ec);
        if (ec) {
            conn.detail = "resolve: " + clip(ec.message());
            conn.ms     = ms_since(t0);
            out.push_back(conn);
            return;
        }
        auto& tcp_layer = beast::get_lowest_layer(ws);
        tcp_layer.expires_after(std::chrono::seconds(timeout_s));
        ec = run_op(ioc, [&](auto&& h) { tcp_layer.async_connect(results, h); });
        if (ec) {
            conn.detail = "connect: " + clip(ec.message());
            conn.ms     = ms_since(t0);
            out.push_back(conn);
            return;
        }
        websocket::response_type res;
        tcp_layer.expires_after(std::chrono::seconds(timeout_s));
        const std::string host_header = port == 80 ? host : host + ":" + std::to_string(port);
        ec = run_op(ioc, [&](auto&& h) { ws.async_handshake(res, host_header, "/websocket", h); });
        conn.status = (int) res.result_int();
        conn.ms     = ms_since(t0);
        if (ec) {
            conn.detail = "handshake: " + clip(ec.message()) + (conn.status ? " (HTTP " + std::to_string(conn.status) + ")" : "");
            out.push_back(conn);
            return;
        }
        conn.ok     = true;
        conn.detail = "upgraded";
        out.push_back(conn);

        long long id = 7000;
        for (const std::string& method : read_only_methods()) {
            Step st;
            st.name                  = base + " " + method;
            const std::string request = request_for(method, ++id);
            if (request.empty() || !is_read_only_method(method)) { // never anything else
                st.detail = "refused: not a read-only method";
                out.push_back(st);
                continue;
            }
            const auto s0 = std::chrono::steady_clock::now();
            ws.text(true);
            tcp_layer.expires_after(std::chrono::seconds(timeout_s));
            ec = run_op(ioc, [&](auto&& h) { ws.async_write(net::buffer(request), h); });
            if (ec) {
                st.detail = "send: " + clip(ec.message());
                st.ms     = ms_since(s0);
                out.push_back(st);
                break;
            }
            // Moonraker also pushes notifications (notify_proc_stat_update, ...): read until our id.
            for (;;) {
                const long long left = timeout_s * 1000LL - ms_since(s0);
                if (left <= 0) {
                    st.detail = "no answer within " + std::to_string(timeout_s) + " s";
                    break;
                }
                beast::flat_buffer buf;
                tcp_layer.expires_after(std::chrono::milliseconds(left));
                ec = run_op(ioc, [&](auto&& h) { ws.async_read(buf, h); });
                if (ec) {
                    st.detail = "read: " + clip(ec.message());
                    break;
                }
                const std::string body = beast::buffers_to_string(buf.data());
                const json        j    = json::parse(body, nullptr, false);
                if (!j.is_discarded() && j.is_object() && j.contains("id") && j["id"] == id) {
                    st.ok     = !j.contains("error");
                    st.detail = describe_answer(body);
                    break;
                }
            }
            st.ms = ms_since(s0);
            out.push_back(st);
            if (ec)
                break;
        }
        tcp_layer.expires_after(std::chrono::seconds(2));
        run_op(ioc, [&](auto&& h) { ws.async_close(websocket::close_code::normal, h); });
    } catch (const std::exception& e) {
        conn.detail = clip(e.what());
        conn.ms     = ms_since(t0);
        if (out.empty() || out.back().name != conn.name)
            out.push_back(conn);
    }
}

std::vector<Step> run(const std::string& host, const Options& opt)
{
    std::vector<Step> out;
    if (opt.http)
        for (const std::string& p : http_paths())
            out.push_back(http_step(host, opt.http_port, p, opt.timeout_s));
    for (int port : opt.ws_ports)
        ws_steps(host, port, opt.timeout_s, out);
    for (const Step& s : out)
        BOOST_LOG_TRIVIAL(info) << "[u1-moonraker-probe] " << s.name << ": " << (s.ok ? "OK" : "FAILED") << ", status " << s.status
                                << ", " << s.ms << " ms, " << s.detail;
    return out;
}

std::string report(const std::vector<Step>& steps)
{
    std::ostringstream ss;
    bool               http_ok = false;
    std::string        ws_ok;
    for (const Step& s : steps) {
        ss << (s.ok ? "OK      " : "FAILED  ") << s.name << "  (" << s.ms << " ms";
        if (s.status)
            ss << ", status " << s.status;
        ss << ")\n        " << s.detail << "\n";
        if (s.ok && s.name.rfind("GET ", 0) == 0)
            http_ok = true;
        if (s.ok && s.name.rfind("WS ", 0) == 0 && s.name.find("printer.objects.query") != std::string::npos && ws_ok.empty())
            ws_ok = s.name.substr(3, s.name.find('/') - 3);
    }
    ss << "\nLocal HTTP JSON answers: " << (http_ok ? "yes" : "no") << "\n";
    ss << "Local WebSocket JSON-RPC answers: " << (ws_ok.empty() ? "no" : "yes, on " + ws_ok) << "\n";
    return ss.str();
}

} // namespace U1MoonrakerProbe
} // namespace Slic3r
