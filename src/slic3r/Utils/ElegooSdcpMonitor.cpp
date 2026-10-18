// The persistent SDCP connections declared in ElegooSdcpMonitor.hpp: one beast websocket per printer,
// every one of them driven by a single background io_context thread, so no caller ever waits on a
// printer's network and a dead printer costs nothing but a timer.

#include "ElegooSdcpMonitor.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/filesystem.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace Slic3r {
namespace SdcpMonitor {

namespace {

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;
using json          = nlohmann::json;

const int  RESOLVE_MS        = 4000;
const int  CONNECT_MS        = 4000;
const int  HANDSHAKE_S       = 5;
const int  WRITE_MS          = 10000;
const int  PING_MS           = 20000;  // elegoo-link's heartbeat interval
const int  SILENCE_MS        = 45000;  // no frame for this long: ask for the status
const int  PROBE_MS          = 15000;  // ... and give up on the connection if that is not answered
const int  CANVAS_EVERY_MS   = 15000;
const int  WATCH_MS          = 45000;  // how long one look at the printers counts as watching
const int  IDLE_MS           = 120000; // nobody asked about the printer: close
const int  LOCK_RETRY_MS     = 30000;
const int  WATCHED_BACKOFF   = 15000;
const long long BACKOFF_MS[] = { 2000, 4000, 8000, 16000, 30000, 60000 };

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

long long wall_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string new_request_id()
{
    std::string id = boost::uuids::to_string(boost::uuids::random_generator()());
    id.erase(std::remove(id.begin(), id.end(), '-'), id.end());
    return id;
}

std::atomic<long long> g_watch_ms { 0 };

// A host the connection may be opened to: a name or an address, nothing that could smuggle a path.
bool valid_host(const std::string& h)
{
    if (h.empty() || h.size() > 253) return false;
    for (char c : h)
        if (!(std::isalnum((unsigned char) c) || c == '.' || c == '-' || c == '_' || c == ':' )) return false;
    return true;
}

struct Waiter
{
    int                     cmd { -1 };
    std::string             request_id;
    bool                    canvas { false };
    std::mutex              m;
    std::condition_variable cv;
    bool                    done { false };
    bool                    answered { false };
    int                     ack { -1 };
    std::string             error;

    void finish(bool ans, int a, const std::string& err)
    {
        std::lock_guard<std::mutex> lock(m);
        if (done) return;
        done     = true;
        answered = ans;
        ack      = a;
        error    = err;
        cv.notify_all();
    }
};

using Ws = websocket::stream<beast::tcp_stream>;

struct Conn
{
    explicit Conn(net::io_context& ioc) : ws(ioc) {}
    Ws                      ws;
    beast::flat_buffer      buf;
    std::deque<std::string> out;
    bool                    writing { false };
    long long               write_started { 0 };
    bool                    open { false };
};

class Link : public std::enable_shared_from_this<Link>
{
public:
    Link(net::io_context& ioc, std::string host, int port, int seq)
        : m_ioc(ioc), m_host(std::move(host)), m_port(port), m_seq(seq), m_resolver(ioc), m_step(ioc), m_tick(ioc)
    {
        m_last_touch = now_ms();
        std::lock_guard<std::mutex> lock(m_state);
        m_snap.exists = true;
    }

    const std::string& host() const { return m_host; }

    void start()
    {
        auto self = shared_from_this();
        net::post(m_ioc, [self]() {
            self->acquire_lock();
            self->arm_tick();
            self->tick();
        });
    }

    void stop()
    {
        auto self = shared_from_this();
        net::post(m_ioc, [self]() { self->do_stop(); });
    }

    void touched() { m_last_touch = now_ms(); }
    bool idle() const { return m_stopped.load(); }

    Sdcp::Snapshot snapshot()
    {
        std::lock_guard<std::mutex> lock(m_state);
        return m_snap;
    }

    bool connected()
    {
        std::lock_guard<std::mutex> lock(m_state);
        return m_snap.connected;
    }

    // Any thread: queue a command and register who waits for its answer.
    std::shared_ptr<Waiter> request(int cmd, const json& data, bool canvas)
    {
        auto w        = std::make_shared<Waiter>();
        w->cmd        = cmd;
        w->request_id = new_request_id();
        w->canvas     = canvas;
        {
            std::lock_guard<std::mutex> lock(m_wmutex);
            m_waiters.push_back(w);
        }
        auto self = shared_from_this();
        net::post(m_ioc, [self, w, data]() {
            if (!self->m_conn || !self->m_conn->open) {
                w->finish(false, -1, "the printer is not connected");
                return;
            }
            self->send(Sdcp::request_text(w->cmd, data, w->request_id, wall_ms(), self->m_mainboard));
            if (w->canvas) self->m_last_canvas = now_ms();
        });
        return w;
    }

    void forget(const std::shared_ptr<Waiter>& w)
    {
        std::lock_guard<std::mutex> lock(m_wmutex);
        m_waiters.erase(std::remove(m_waiters.begin(), m_waiters.end(), w), m_waiters.end());
    }

private:
    // ---------------------------------------------------------- one PC, one connection ----

    void acquire_lock()
    {
        if (m_lock) return;
        try {
            const std::string dir = data_dir();
            if (dir.empty()) {
                set_other_window(false);
                return; // no data dir (the unit tests): nothing to share with
            }
            boost::filesystem::path p = boost::filesystem::path(dir) / "hub" / "sdcp";
            boost::system::error_code ec;
            boost::filesystem::create_directories(p, ec);
            std::string name;
            for (char c : m_host) name += std::isalnum((unsigned char) c) ? c : '_';
            if (m_port != 3030) name += "_" + std::to_string(m_port);
            p /= name + ".lock";
            if (!boost::filesystem::exists(p, ec)) {
                boost::nowide::ofstream f(p.string());
            }
            auto lock = std::make_unique<boost::interprocess::file_lock>(p.string().c_str());
            if (lock->try_lock()) {
                m_lock = std::move(lock);
                set_other_window(false);
            } else {
                set_other_window(true);
            }
        } catch (const std::exception& e) {
            // A lock that cannot be made must not cost the printer its status.
            BOOST_LOG_TRIVIAL(warning) << "SDCP printer " << m_seq << ": no connection lock (" << e.what() << "), connecting anyway";
            set_other_window(false);
            m_lock_failed = true;
        }
        m_lock_tried = now_ms();
    }

    bool may_connect() const { return m_lock != nullptr || m_lock_failed || data_dir().empty(); }

    void set_other_window(bool other)
    {
        std::lock_guard<std::mutex> lock(m_state);
        if (other && !m_snap.other_window)
            BOOST_LOG_TRIVIAL(info) << "SDCP printer " << m_seq << ": another EdgeSlicer window holds its connection";
        m_snap.other_window = other;
    }

    // -------------------------------------------------------------------- the timer ----

    void arm_tick()
    {
        auto self = shared_from_this();
        m_tick.expires_after(std::chrono::seconds(1));
        m_tick.async_wait([self](const boost::system::error_code& ec) {
            if (ec || self->m_stopped) return;
            self->tick();
            self->arm_tick();
        });
    }

    void tick()
    {
        const long long now = now_ms();
        if (now - m_last_touch.load() > IDLE_MS) {
            BOOST_LOG_TRIVIAL(info) << "SDCP printer " << m_seq << ": nobody asked for two minutes, closing";
            do_stop();
            return;
        }
        if (!may_connect()) {
            if (now - m_lock_tried > LOCK_RETRY_MS) acquire_lock();
            if (!may_connect()) return;
        }
        if (!m_conn) {
            if (!m_connecting && now >= m_next_attempt) connect();
            return;
        }
        if (!m_conn->open) return; // still connecting
        Conn& c = *m_conn;
        if (c.writing && now - c.write_started > WRITE_MS) {
            fail("a write to the printer timed out");
            return;
        }
        if (m_probe_sent > 0 && m_last_frame < m_probe_sent && now - m_probe_sent > PROBE_MS) {
            fail("the printer stopped answering");
            return;
        }
        if (now - m_last_ping >= PING_MS) {
            m_last_ping = now;
            send("ping");
        }
        if (now - m_last_frame >= SILENCE_MS && (m_probe_sent == 0 || m_probe_sent < m_last_frame)) {
            m_probe_sent = now;
            send(Sdcp::request_text(Sdcp::CMD_STATUS, json::object(), new_request_id(), wall_ms(), m_mainboard));
        }
        const bool watched = watching();
        if (watched && !m_was_watched) {
            // Somebody just started looking: a fresh status rather than whatever the last push said.
            send(Sdcp::request_text(Sdcp::CMD_STATUS, json::object(), new_request_id(), wall_ms(), m_mainboard));
        }
        m_was_watched = watched;
        int ams = -1;
        {
            std::lock_guard<std::mutex> lock(m_state);
            ams = m_snap.status.ams_connect;
        }
        if (watched && ams == 1 && now - m_last_canvas >= CANVAS_EVERY_MS) {
            m_last_canvas = now;
            send(Sdcp::request_text(Sdcp::CMD_CANVAS, json::object(), new_request_id(), wall_ms(), m_mainboard));
        }
    }

    // ------------------------------------------------------------------ connecting ----

    void connect()
    {
        m_connecting = true;
        {
            std::lock_guard<std::mutex> lock(m_state);
            m_snap.connecting = true;
        }
        auto conn = std::make_shared<Conn>(m_ioc);
        m_conn    = conn;
        auto self = shared_from_this();
        auto done = std::make_shared<bool>(false);
        m_step.expires_after(std::chrono::milliseconds(RESOLVE_MS));
        m_step.async_wait([self, done](const boost::system::error_code& ec) {
            if (!ec && !*done) self->m_resolver.cancel();
        });
        m_resolver.async_resolve(m_host, std::to_string(m_port),
                                 [self, conn, done](const boost::system::error_code& ec, tcp::resolver::results_type results) {
                                     *done = true;
                                     self->m_step.cancel();
                                     if (conn != self->m_conn) return;
                                     if (ec) {
                                         self->fail(ec == net::error::operation_aborted ? "the printer's name did not resolve in time"
                                                                                        : "the printer's address does not resolve");
                                         return;
                                     }
                                     self->on_resolved(conn, results);
                                 });
    }

    void on_resolved(std::shared_ptr<Conn> conn, const tcp::resolver::results_type& results)
    {
        auto self = shared_from_this();
        beast::get_lowest_layer(conn->ws).expires_after(std::chrono::milliseconds(CONNECT_MS));
        beast::get_lowest_layer(conn->ws).async_connect(results, [self, conn](const boost::system::error_code& ec, const tcp::endpoint&) {
            if (conn != self->m_conn) return;
            if (ec) {
                self->fail(ec == beast::error::timeout ? "the printer does not answer (connect timed out)"
                                                       : "the printer refused the connection");
                return;
            }
            self->on_connected(conn);
        });
    }

    void on_connected(std::shared_ptr<Conn> conn)
    {
        auto self = shared_from_this();
        beast::get_lowest_layer(conn->ws).expires_never();
        websocket::stream_base::timeout opt;
        opt.handshake_timeout = std::chrono::seconds(HANDSHAKE_S);
        opt.idle_timeout      = websocket::stream_base::none(); // the tick keeps its own watch
        opt.keep_alive_pings  = false;
        conn->ws.set_option(opt);
        conn->ws.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) { req.set(beast::http::field::user_agent, "ElegooSlicer"); }));
        conn->ws.read_message_max(2 * 1024 * 1024);
        conn->ws.async_handshake(m_host, "/websocket", [self, conn](const boost::system::error_code& ec) {
            if (conn != self->m_conn) return;
            if (ec) {
                // A declined upgrade is how the printer says it already has as many clients as it takes.
                self->fail(ec == websocket::error::upgrade_declined ?
                               "the printer declined the connection (it may have too many clients: its web page, the Elegoo app, another slicer)" :
                               (ec == beast::error::timeout ? "the printer did not complete the websocket handshake" :
                                                              "the websocket handshake failed"));
                return;
            }
            self->on_open(conn);
        });
    }

    void on_open(std::shared_ptr<Conn> conn)
    {
        conn->open   = true;
        m_connecting = false;
        m_failures   = 0;
        const long long now = now_ms();
        m_last_frame  = now;
        m_last_ping   = now;
        m_probe_sent  = 0;
        m_last_canvas = 0;
        {
            std::lock_guard<std::mutex> lock(m_state);
            m_snap.connected      = true;
            m_snap.connecting     = false;
            m_snap.ever_connected = true;
            m_snap.failures       = 0;
            m_snap.error.clear();
        }
        BOOST_LOG_TRIVIAL(info) << "SDCP printer " << m_seq << ": connected";
        conn->ws.text(true);
        read(conn);
        // What the printer is and what it is doing; from here on it pushes every change itself.
        send(Sdcp::request_text(Sdcp::CMD_ATTRIBUTES, json::object(), new_request_id(), wall_ms(), m_mainboard));
        send(Sdcp::request_text(Sdcp::CMD_STATUS, json::object(), new_request_id(), wall_ms(), m_mainboard));
        m_was_watched = watching();
    }

    // ------------------------------------------------------------- reading, writing ----

    void read(std::shared_ptr<Conn> conn)
    {
        auto self = shared_from_this();
        conn->ws.async_read(conn->buf, [self, conn](const boost::system::error_code& ec, std::size_t) {
            if (conn != self->m_conn) return;
            if (ec) {
                self->fail(ec == websocket::error::closed ? "the printer closed the connection" : "the connection to the printer was lost");
                return;
            }
            std::string text = beast::buffers_to_string(conn->buf.data());
            conn->buf.consume(conn->buf.size());
            try {
                self->on_frame(text);
            } catch (...) {}
            if (conn == self->m_conn) self->read(conn);
        });
    }

    void send(std::string text)
    {
        if (!m_conn || !m_conn->open) return;
        m_conn->out.push_back(std::move(text));
        if (!m_conn->writing) write(m_conn);
    }

    void write(std::shared_ptr<Conn> conn)
    {
        if (conn->out.empty()) {
            conn->writing = false;
            return;
        }
        conn->writing       = true;
        conn->write_started = now_ms();
        auto self           = shared_from_this();
        conn->ws.async_write(net::buffer(conn->out.front()), [self, conn](const boost::system::error_code& ec, std::size_t) {
            if (conn != self->m_conn) return;
            if (ec) {
                self->fail("a write to the printer failed");
                return;
            }
            conn->out.pop_front();
            self->write(conn);
        });
    }

    void on_frame(const std::string& text)
    {
        const long long now = now_ms();
        m_last_frame        = now;
        Sdcp::Frame f;
        bool        canvas_ok = false;
        int         ams_before = -1, ams_after = -1;
        {
            std::lock_guard<std::mutex> lock(m_state);
            ams_before = m_snap.status.ams_connect;
            f          = Sdcp::read_frame(text, m_snap.status, m_snap.attrs);
            if (f.kind == Sdcp::FrameKind::Status) m_snap.status_at_ms = now;
            if (f.kind == Sdcp::FrameKind::Attributes && !m_snap.attrs.mainboard_id.empty()) m_mainboard = m_snap.attrs.mainboard_id;
            if (f.has_canvas) {
                CanvasInfo  info;
                std::string err;
                if (canvas_parse_payload(text, info, err)) {
                    m_snap.canvas       = std::move(info);
                    m_snap.has_canvas   = true;
                    m_snap.canvas_at_ms = now;
                    canvas_ok           = true;
                }
            }
            ams_after = m_snap.status.ams_connect;
            if (ams_after == 0) m_snap.has_canvas = false; // unplugged: its trays are gone
        }
        if (f.kind == Sdcp::FrameKind::Response) {
            std::vector<std::shared_ptr<Waiter>> waiters;
            {
                std::lock_guard<std::mutex> lock(m_wmutex);
                waiters = m_waiters;
            }
            for (const auto& w : waiters) {
                if (w->canvas) {
                    if (canvas_ok)
                        w->finish(true, 0, "");
                    else if (f.cmd == Sdcp::CMD_CANVAS && f.ack > 0)
                        w->finish(true, f.ack, "the printer refused the CANVAS request (Ack " + std::to_string(f.ack) + ")");
                    continue;
                }
                const bool mine = !f.request_id.empty() ? f.request_id == w->request_id : f.cmd == w->cmd;
                if (mine) w->finish(true, f.ack, "");
            }
        }
        // A CANVAS just plugged in while somebody watches: read it now rather than in 15 s.
        if (ams_after == 1 && ams_before != 1 && watching()) m_last_canvas = 0;
    }

    // ------------------------------------------------------------- failing, stopping ----

    void close_conn()
    {
        if (!m_conn) return;
        boost::system::error_code ignore;
        beast::get_lowest_layer(m_conn->ws).socket().shutdown(tcp::socket::shutdown_both, ignore);
        beast::get_lowest_layer(m_conn->ws).socket().close(ignore);
        m_conn.reset();
        m_resolver.cancel();
        m_step.cancel();
    }

    void fail_waiters(const std::string& why)
    {
        std::vector<std::shared_ptr<Waiter>> waiters;
        {
            std::lock_guard<std::mutex> lock(m_wmutex);
            waiters = m_waiters;
        }
        for (const auto& w : waiters) w->finish(false, -1, why);
    }

    void fail(const std::string& why)
    {
        const bool was_open = m_conn && m_conn->open;
        close_conn();
        m_connecting = false;
        ++m_failures;
        long long delay = BACKOFF_MS[std::min<size_t>(m_failures - 1, sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0]) - 1)];
        if (watching()) delay = std::min<long long>(delay, WATCHED_BACKOFF);
        if (was_open) delay = 2000; // it was up a moment ago: try again soon
        m_next_attempt = now_ms() + delay;
        {
            std::lock_guard<std::mutex> lock(m_state);
            m_snap.connected  = false;
            m_snap.connecting = false;
            m_snap.failures   = m_failures;
            m_snap.error      = why;
        }
        // The first failure and every lost connection are worth a line; a printer that stays off is not
        // worth one every minute.
        if (was_open || m_failures == 1)
            BOOST_LOG_TRIVIAL(info) << "SDCP printer " << m_seq << ": " << why << " (next try in " << delay / 1000 << " s)";
        fail_waiters(why);
    }

    void do_stop()
    {
        if (m_stopped) return;
        m_stopped = true;
        close_conn();
        m_tick.cancel();
        {
            std::lock_guard<std::mutex> lock(m_state);
            m_snap.connected  = false;
            m_snap.connecting = false;
        }
        fail_waiters("the connection to the printer was closed");
        m_lock.reset();
    }

    net::io_context&    m_ioc;
    const std::string   m_host;
    const int           m_port;
    const int           m_seq; // the printer's number in the log, instead of its address
    tcp::resolver       m_resolver;
    net::steady_timer   m_step;
    net::steady_timer   m_tick;
    std::shared_ptr<Conn> m_conn;
    bool                m_connecting { false };
    int                 m_failures { 0 };
    long long           m_next_attempt { 0 };
    long long           m_last_frame { 0 }, m_last_ping { 0 }, m_probe_sent { 0 }, m_last_canvas { 0 };
    bool                m_was_watched { false };
    std::string         m_mainboard; // io thread only; never logged
    std::unique_ptr<boost::interprocess::file_lock> m_lock;
    bool                m_lock_failed { false };
    long long           m_lock_tried { 0 };
    std::atomic<long long> m_last_touch { 0 };
    std::atomic<bool>   m_stopped { false };

    std::mutex          m_state;
    Sdcp::Snapshot      m_snap;
    std::mutex          m_wmutex;
    std::vector<std::shared_ptr<Waiter>> m_waiters;
};

// The process-wide set of connections. Allocated once and never destroyed: the network thread may
// still be running when static destructors run at exit, and it must not find its io_context gone.
struct Manager
{
    std::mutex                                                     m;
    net::io_context                                                ioc;
    std::unique_ptr<net::executor_work_guard<net::io_context::executor_type>> work;
    std::thread                                                    thread;
    std::map<std::string, std::shared_ptr<Link>>                   links;
    int                                                            port { 3030 };
    int                                                            next_seq { 1 };
    bool                                                           down { false };

    Manager()
    {
        if (const char* p = std::getenv("SNORCA_SDCP_PORT")) {
            const int v = std::atoi(p);
            if (v > 0 && v < 65536) port = v;
        }
    }

    // Under m.
    void ensure_thread()
    {
        if (thread.joinable()) return;
        ioc.restart();
        work   = std::make_unique<net::executor_work_guard<net::io_context::executor_type>>(net::make_work_guard(ioc));
        thread = std::thread([this]() {
            for (;;) {
                try {
                    ioc.run();
                    return;
                } catch (const std::exception& e) {
                    BOOST_LOG_TRIVIAL(error) << "SDCP: network thread: " << e.what();
                } catch (...) {}
            }
        });
    }

    // Under m. The live link for `host`, or a new one; stopped (idle) links are replaced.
    std::shared_ptr<Link> link_for(const std::string& host, bool create)
    {
        auto it = links.find(host);
        if (it != links.end() && it->second->idle()) {
            links.erase(it);
            it = links.end();
        }
        if (it != links.end()) return it->second;
        if (!create || down) return nullptr;
        ensure_thread();
        auto l = std::make_shared<Link>(ioc, host, port, next_seq++);
        links[host] = l;
        l->start();
        return l;
    }
};

Manager& mgr()
{
    static Manager* m = new Manager();
    return *m;
}

std::shared_ptr<Link> find_link(const std::string& host, bool create)
{
    if (!valid_host(host)) return nullptr;
    Manager&                    M = mgr();
    std::lock_guard<std::mutex> lock(M.m);
    return M.link_for(host, create);
}

} // namespace

Sdcp::Snapshot touch(const std::string& host)
{
    std::shared_ptr<Link> l = find_link(host, true);
    if (!l) return Sdcp::Snapshot();
    l->touched();
    return l->snapshot();
}

void note_watching() { g_watch_ms = now_ms(); }

bool watching()
{
    const long long w = g_watch_ms.load();
    return w != 0 && now_ms() - w < WATCH_MS;
}

CommandResult command(const std::string& host, int cmd, const nlohmann::json& data, int timeout_ms)
{
    CommandResult         r;
    std::shared_ptr<Link> l = find_link(host, false);
    if (!l || !l->connected()) {
        r.error = "the printer is not connected";
        return r;
    }
    l->touched();
    std::shared_ptr<Waiter> w = l->request(cmd, data, false);
    {
        std::unique_lock<std::mutex> lock(w->m);
        w->cv.wait_for(lock, std::chrono::milliseconds(std::max(100, timeout_ms)), [&]() { return w->done; });
        // Not sent only when the connection was gone by the time the command reached it.
        r.sent     = !(w->done && !w->answered && w->error == "the printer is not connected");
        r.answered = w->done && w->answered;
        r.ack      = w->ack;
        r.error    = w->done ? w->error : std::string("the printer did not answer in time");
    }
    l->forget(w);
    return r;
}

bool canvas_query_shared(const std::string& host, const CanvasQueryOptions& opts, CanvasQueryResult& out)
{
    std::shared_ptr<Link> l = find_link(host, false);
    if (!l || !l->connected()) return false;
    l->touched();
    const Sdcp::Snapshot before = l->snapshot();
    out                         = CanvasQueryResult();
    out.reached                 = true;
    out.ams_connect_status      = before.status.ams_connect;
    if (before.status.ams_connect == 0) {
        out.no_canvas = true;
        out.error     = "no CANVAS is attached (AmsConnectStatus 0)";
        return true;
    }
    std::shared_ptr<Waiter> w = l->request(Sdcp::CMD_CANVAS, json::object(), true);
    bool done = false;
    {
        std::unique_lock<std::mutex> lock(w->m);
        w->cv.wait_for(lock, std::chrono::milliseconds(std::max(500, opts.step_timeout_ms)), [&]() { return w->done; });
        done = w->done;
        if (!done)
            out.error = "no CANVAS payload before the timeout";
        else if (!w->error.empty())
            out.error = w->error;
    }
    l->forget(w);
    if (done && out.error.empty()) {
        const Sdcp::Snapshot after = l->snapshot();
        if (after.has_canvas) {
            out.ok   = true;
            out.info = after.canvas;
        } else {
            out.error = "no CANVAS payload in the reply";
        }
    }
    return true;
}

void shutdown()
{
    Manager&                           M = mgr();
    std::vector<std::shared_ptr<Link>> links;
    {
        std::lock_guard<std::mutex> lock(M.m);
        if (M.down) return;
        M.down = true;
        for (auto& kv : M.links) links.push_back(kv.second);
        M.links.clear();
    }
    for (auto& l : links) l->stop();
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(M.m);
        M.work.reset();
        t = std::move(M.thread);
    }
    if (t.joinable()) {
        // The stops above close every socket; give them a moment, then stop the loop whatever is left.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!M.ioc.stopped() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        M.ioc.stop();
        t.join();
    }
}

void set_port_for_tests(int port)
{
    Manager&                    M = mgr();
    std::lock_guard<std::mutex> lock(M.m);
    M.port = port > 0 ? port : 3030;
}

void reset_for_tests()
{
    Manager&                           M = mgr();
    std::vector<std::shared_ptr<Link>> links;
    {
        std::lock_guard<std::mutex> lock(M.m);
        for (auto& kv : M.links) links.push_back(kv.second);
        M.links.clear();
        M.down = false;
    }
    for (auto& l : links) l->stop();
    g_watch_ms = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

} // namespace SdcpMonitor
} // namespace Slic3r
