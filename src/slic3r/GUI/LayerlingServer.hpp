#ifndef slic3r_GUI_LayerlingServer_hpp_
#define slic3r_GUI_LayerlingServer_hpp_

#include <memory>
#include <string>

#include <boost/filesystem/path.hpp>

namespace Slic3r {
namespace GUI {

class HttpServer;

// Serves the unpacked Layerling web build to the Layerling tab, from 127.0.0.1 only.
//
// It is the app's own small HTTP server (HttpServer, the one behind the page server and the
// sign-in callbacks) in an instance of its own, on its own port, with a request handler that only
// reads files under the app folder (layerling::candidate_files: no "..", no PHP) and answers with
// the right content type and cache headers. The page server's and the phone hub's ports, tokens and
// handlers are not touched.
//
// The port is chosen once (a random free one in 20000-31999, below the systems' ephemeral ranges) and
// kept in app_config "layerling_port": the browser keeps Layerling's designs per origin, and the
// port is part of the origin, so the same port on every start is what keeps the designs. When that
// port is taken at start, the server runs on another one for this session and moved() says so
// (the panel tells the user; the remembered port is kept for next time).
class LayerlingServer
{
public:
    LayerlingServer();
    ~LayerlingServer();
    LayerlingServer(const LayerlingServer &)            = delete;
    LayerlingServer &operator=(const LayerlingServer &) = delete;

    // Serves `app_root` (the folder holding index.html), starting the server when it is not
    // running. A running server switches folders at once (an update).
    bool start(const boost::filesystem::path &app_root, std::string &error);
    void stop();
    bool running() const;
    int  port() const;
    // The remembered port was taken; this session runs on another one.
    bool moved() const { return m_moved; }
    int  wanted_port() const { return m_wanted; }
    // "http://127.0.0.1:<port>"
    std::string origin() const;

    struct State;

private:
    std::unique_ptr<HttpServer> m_server;
    std::shared_ptr<State>      m_state;
    bool                        m_moved  = false;
    int                         m_wanted = 0;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_LayerlingServer_hpp_
