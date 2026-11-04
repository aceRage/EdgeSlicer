#include "LayerlingServer.hpp"

#include "HttpServer.hpp"
#include "GUI_App.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Layerling.hpp"
#include "libslic3r/UntrustedInput.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <random>
#include <sstream>

namespace Slic3r {
namespace GUI {

namespace fs = boost::filesystem;

struct LayerlingServer::State
{
    std::mutex mutex;
    fs::path   root;

    fs::path get_root()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return root;
    }
};

namespace {

std::string utf8_of(const fs::path &p)
{
#ifdef _WIN32
    return boost::nowide::narrow(p.wstring());
#else
    return p.string();
#endif
}

fs::path path_of(const std::string &utf8)
{
#ifdef _WIN32
    return fs::path(boost::nowide::widen(utf8));
#else
    return fs::path(utf8);
#endif
}

// Writes its own head: no "Access-Control-Allow-Origin: *" (HttpServer adds that to the login
// callback responses) - Layerling's files are for the tab, which is same-origin.
class LayerlingResponse : public HttpServer::Response
{
public:
    LayerlingResponse(int status, std::string reason, fs::path file, std::string relative)
        : m_status(status), m_reason(std::move(reason)), m_file(std::move(file)), m_relative(std::move(relative))
    {}

    void write_response(std::stringstream &out) override
    {
        std::string body;
        std::string type = "text/plain; charset=utf-8";
        std::string etag;
        if (!m_file.empty()) {
            boost::nowide::ifstream in(utf8_of(m_file).c_str(), std::ios::binary);
            if (in) {
                std::ostringstream ss;
                ss << in.rdbuf();
                body = ss.str();
                type = layerling::mime_type_for(m_file.filename().string());
                boost::system::error_code ec;
                const std::time_t         t = fs::last_write_time(m_file, ec);
                if (!ec)
                    etag = "\"" + std::to_string((long long) t) + "-" + std::to_string(body.size()) + "\"";
            } else if (m_status == 200) {
                m_status = 404;
                m_reason = "Not Found";
            }
        }
        if (body.empty() && m_status != 200)
            body = std::to_string(m_status) + " " + m_reason + "\n";

        const bool not_modified = m_status == 200 && !etag.empty() && m_if_none_match.find(etag) != std::string::npos;
        out << "HTTP/1.1 " << (not_modified ? 304 : m_status) << " " << (not_modified ? "Not Modified" : m_reason) << "\r\n";
        out << "X-Content-Type-Options: nosniff\r\n";
        out << "Cross-Origin-Resource-Policy: same-origin\r\n";
        if (m_status == 200) {
            out << "Cache-Control: " << layerling::cache_control_for(m_relative) << "\r\n";
            if (!etag.empty())
                out << "ETag: " << etag << "\r\n";
        } else {
            out << "Cache-Control: no-store\r\n";
        }
        out << "Connection: close\r\n";
        if (not_modified) {
            out << "\r\n";
            return;
        }
        out << "Content-Type: " << type << "\r\n";
        out << "Content-Length: " << body.size() << "\r\n";
        out << "\r\n";
        out << body;
    }

private:
    int         m_status;
    std::string m_reason;
    fs::path    m_file;
    std::string m_relative;
};

fs::path relative_to_path(const fs::path &root, const std::string &relative)
{
    fs::path p = root;
    size_t   pos = 0;
    while (pos <= relative.size()) {
        const size_t slash = relative.find('/', pos);
        const std::string seg = relative.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!seg.empty())
            p /= path_of(seg);
        if (slash == std::string::npos)
            break;
        pos = slash + 1;
    }
    return p;
}

std::shared_ptr<HttpServer::Response> handle(const std::shared_ptr<LayerlingServer::State> &state, const std::string &target)
{
    const fs::path root = state->get_root();
    if (root.empty())
        return std::make_shared<LayerlingResponse>(503, "Service Unavailable", fs::path(), std::string());
    bool       forbidden = false;
    const auto candidates = layerling::candidate_files(target, &forbidden);
    if (forbidden) {
        BOOST_LOG_TRIVIAL(warning) << "Layerling server: refused " << target.substr(0, std::min<size_t>(target.size(), 200));
        return std::make_shared<LayerlingResponse>(403, "Forbidden", fs::path(), std::string());
    }
    boost::system::error_code ec;
    for (const std::string &rel : candidates) {
        const fs::path file = relative_to_path(root, rel);
        if (fs::is_regular_file(fs::symlink_status(file, ec)) && untrusted::is_path_within_root(root, file))
            return std::make_shared<LayerlingResponse>(200, "OK", file, rel);
    }
    // The build's own 404 page, with a 404 status.
    const fs::path not_found = root / "404.html";
    if (fs::is_regular_file(fs::symlink_status(not_found, ec)))
        return std::make_shared<LayerlingResponse>(404, "Not Found", not_found, "404.html");
    return std::make_shared<LayerlingResponse>(404, "Not Found", fs::path(), std::string());
}

} // namespace

LayerlingServer::LayerlingServer() : m_state(std::make_shared<State>()) {}

LayerlingServer::~LayerlingServer() { stop(); }

bool LayerlingServer::running() const { return m_server != nullptr && m_server->is_started(); }

int LayerlingServer::port() const { return m_server ? int(m_server->get_port()) : 0; }

std::string LayerlingServer::origin() const { return "http://127.0.0.1:" + std::to_string(port()); }

bool LayerlingServer::start(const fs::path &app_root, std::string &error)
{
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->root = app_root;
    }
    if (running())
        return true;

    AppConfig *config = wxGetApp().app_config;
    int        wanted = 0;
    if (config != nullptr) {
        try {
            wanted = std::stoi(config->get("layerling_port"));
        } catch (...) {
            wanted = 0;
        }
    }
    const bool remembered = wanted >= 1024 && wanted <= 65535;
    if (!remembered) {
        std::random_device rd;
        wanted = 20000 + int(rd() % 12000);
    }
    m_wanted = wanted;

    m_server = std::make_unique<HttpServer>(boost::asio::ip::port_type(wanted));
    auto state = m_state;
    m_server->set_request_handler([state](const std::string &url) { return handle(state, url); });
    try {
        m_server->start();
    } catch (const std::exception &e) {
        error = e.what();
        m_server.reset();
        return false;
    }
    // A health check could restart the server on another port: the tab is ours, a reload fixes a
    // hiccup, and a moved origin would hide the user's designs.
    m_server->stop_health_check();
    if (!m_server->is_started()) {
        error = "the local server did not start";
        m_server.reset();
        return false;
    }
    const int bound = int(m_server->get_port());
    m_moved         = remembered && bound != wanted;
    if (!remembered && config != nullptr)
        config->set("layerling_port", std::to_string(bound));
    BOOST_LOG_TRIVIAL(info) << "Layerling server: serving on 127.0.0.1:" << bound << (m_moved ? " (remembered port was busy)" : "");
    return true;
}

void LayerlingServer::stop()
{
    if (m_server) {
        m_server->stop();
        m_server.reset();
    }
}

} // namespace GUI
} // namespace Slic3r
