#include "ElegooCamRelay.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace Slic3r {
namespace GUI {
namespace ElegooCam {

namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace websocket = beast::websocket;
using tcp           = asio::ip::tcp;

// ---------------------------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------------------------

const char* state_name(State s)
{
    switch (s) {
    case State::Idle: return "idle";
    case State::Connecting: return "connecting";
    case State::Live: return "live";
    case State::Busy: return "busy";
    case State::NoCamera: return "no_camera";
    case State::Failed: return "failed";
    }
    return "failed";
}

bool valid_host(const std::string& host)
{
    if (host.empty() || host.size() > 253)
        return false;
    for (char c : host) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

static std::string trimmed(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char) s[b] <= ' ') ++b;
    while (e > b && (unsigned char) s[e - 1] <= ' ') --e;
    return s.substr(b, e - b);
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

std::string host_from_address(const std::string& address)
{
    std::string s = trimmed(address);
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos)
        s.erase(0, scheme + 3);
    const size_t cut = s.find_first_of("/?#");
    if (cut != std::string::npos)
        s.erase(cut);
    const size_t at = s.rfind('@');
    if (at != std::string::npos)
        s.erase(0, at + 1);
    const size_t colon = s.find(':');
    if (colon != std::string::npos)
        s.erase(colon);
    return valid_host(s) ? s : std::string();
}

bool is_cc2_name(const std::string& name)
{
    const std::string n = lower(name);
    return n.find("carbon 2") != std::string::npos || n.find("centauri 2") != std::string::npos ||
           n.find("carbon2") != std::string::npos || n.find("centauri2") != std::string::npos;
}

std::string video_request(const VideoTarget& t)
{
    return "GET " + t.path + " HTTP/1.0\r\nHost: " + t.host + ":" + std::to_string(t.port) +
           "\r\nAccept: */*\r\nUser-Agent: EdgeSlicer\r\nConnection: close\r\n\r\n";
}

bool video_target_from_reply(const std::string& raw, const std::string& printer_host, VideoTarget& out)
{
    std::string s = trimmed(raw);
    if (s.empty())
        return false;
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) {
        if (lower(s.substr(0, scheme)) != "http")
            return false;
        s.erase(0, scheme + 3);
    }
    std::string authority = s, path = "/";
    const size_t slash = s.find('/');
    if (slash != std::string::npos) {
        authority = s.substr(0, slash);
        path      = s.substr(slash);
    }
    std::string host = authority;
    int         port = 80;
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        const std::string p = authority.substr(colon + 1);
        if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos)
            return false;
        port = std::stoi(p);
        if (port < 1 || port > 65535)
            return false;
    }
    const std::string lh = lower(host);
    if (lh.empty() || lh == "0.0.0.0" || lh == "localhost" || lh == "127.0.0.1")
        host = printer_host;
    else if (lh != lower(printer_host))
        return false; // the printer cannot send this relay to a host other than itself
    if (!valid_host(host) || path.empty() || path.size() > 200 || path[0] != '/')
        return false;
    for (char c : path)
        if ((unsigned char) c <= ' ' || (unsigned char) c >= 0x7f)
            return false;
    out.host = host;
    out.port = port;
    out.path = path;
    return true;
}

std::string enable_request(bool enable, const std::string& request_id, long long timestamp_s)
{
    nlohmann::json j;
    j["Id"]                 = "";
    j["Data"]["Cmd"]        = 386;
    j["Data"]["Data"]["Enable"] = enable ? 1 : 0;
    j["Data"]["RequestID"]  = request_id;
    j["Data"]["MainboardID"] = "";
    j["Data"]["TimeStamp"]  = timestamp_s;
    j["Data"]["From"]       = 1;
    return j.dump();
}

EnableReply parse_enable_reply(const std::string& ws_text)
{
    EnableReply r;
    try {
        const nlohmann::json j = nlohmann::json::parse(ws_text);
        if (!j.is_object() || !j.contains("Data") || !j["Data"].is_object())
            return r;
        const auto& d = j["Data"];
        if (!d.contains("Cmd") || !d["Cmd"].is_number_integer() || d["Cmd"].get<int>() != 386)
            return r;
        r.valid = true;
        if (d.contains("Data") && d["Data"].is_object()) {
            const auto& dd = d["Data"];
            if (dd.contains("Ack") && dd["Ack"].is_number_integer())
                r.ack = dd["Ack"].get<int>();
            if (dd.contains("VideoUrl") && dd["VideoUrl"].is_string())
                r.video_url = dd["VideoUrl"].get<std::string>();
        }
    } catch (...) {
        r = EnableReply();
    }
    return r;
}

State state_from_ack(int ack)
{
    switch (ack) {
    case 0: return State::Live;
    case 1: return State::Busy;
    case 2: return State::NoCamera;
    default: return State::Failed;
    }
}

State state_from_http_status(int status)
{
    if (status == 200)
        return State::Live;
    if (status == 429 || status == 503)
        return State::Busy;
    return State::Failed;
}

int parse_http_status(const std::string& head)
{
    // "HTTP/1.1 200 OK"
    const size_t sp = head.find(' ');
    if (head.compare(0, 5, "HTTP/") != 0 || sp == std::string::npos || sp + 4 > head.size())
        return 0;
    const std::string code = head.substr(sp + 1, 3);
    if (code.find_first_not_of("0123456789") != std::string::npos)
        return 0;
    return std::stoi(code);
}

std::string message_for(State s)
{
    switch (s) {
    case State::Busy: return "Camera busy: too many viewers (Elegoo allows " + std::to_string(kStreamLimit) + ").";
    case State::NoCamera: return "This printer reports no camera.";
    case State::Failed:
        return "Can't read the printer's camera. Check that it is on and on this network. If other viewers are open, the printer lets only " +
               std::to_string(kStreamLimit) + " in.";
    case State::Connecting: return "Connecting to the camera...";
    default: return "";
    }
}

std::string status_json(State s)
{
    nlohmann::json j;
    j["state"]   = state_name(s);
    j["message"] = message_for(s);
    return j.dump();
}

void JpegSplitter::feed(const char* data, size_t n)
{
    m_buf.append(data, n);
}

bool JpegSplitter::next(std::string& frame)
{
    constexpr size_t kMaxFrame = 4 * 1024 * 1024;
    auto byte = [this](size_t i) { return (unsigned char) m_buf[i]; };

    size_t start = std::string::npos;
    for (size_t i = 0; i + 1 < m_buf.size(); ++i)
        if (byte(i) == 0xFF && byte(i + 1) == 0xD8) {
            start = i;
            break;
        }
    if (start == std::string::npos) {
        // Nothing starts here. A trailing 0xFF may be the first half of a marker; keep that one byte.
        if (!m_buf.empty() && byte(m_buf.size() - 1) == 0xFF)
            m_buf.erase(0, m_buf.size() - 1);
        else
            m_buf.clear();
        return false;
    }
    if (start > 0)
        m_buf.erase(0, start);
    size_t end = std::string::npos;
    for (size_t i = 2; i + 1 < m_buf.size(); ++i)
        if (byte(i) == 0xFF && byte(i + 1) == 0xD9) {
            end = i + 2;
            break;
        }
    if (end == std::string::npos) {
        if (m_buf.size() > kMaxFrame)
            m_buf.clear(); // garbage guard
        return false;
    }
    frame.assign(m_buf, 0, end);
    m_buf.erase(0, end);
    return true;
}

bool is_elegoo_target(const std::string& request_target)
{
    return request_target == "/elegoo" || request_target.compare(0, 8, "/elegoo?") == 0 || request_target.compare(0, 8, "/elegoo/") == 0;
}

// ---------------------------------------------------------------------------------------------
// One shared connection per printer
// ---------------------------------------------------------------------------------------------

namespace {

using FramePtr = std::shared_ptr<const std::string>;
using Clock    = std::chrono::steady_clock;

constexpr int kConnectMs      = 6000;  // resolve + connect + the HTTP head
constexpr int kEnableMs       = 8000;  // the whole SDCP exchange
constexpr int kStallMs        = 10000; // no byte from a live stream
constexpr int kIdleGraceMs    = 1500;  // a tile that re-parks and comes straight back keeps the stream
constexpr int kFirstFrameMs   = 25000; // what a viewer waits for the first picture
constexpr int kFrameWaitMs    = 12000; // what a viewer waits for the next one
constexpr int kRecordKeepSecs = 60;    // how long a refusal is remembered for the status endpoint
constexpr int kMaxReconnects  = 2;

std::atomic<int> g_video_port { kVideoPort };
std::atomic<int> g_control_port { kControlPort };

class Session;

class Hub
{
public:
    static Hub& get()
    {
        static Hub& h = *new Hub(); // never destroyed: session threads are detached
        return h;
    }
    std::shared_ptr<Session> acquire(const std::string& host);
    void                     retire(const std::string& host, Session* s, State final_state);
    State                    status(const std::string& host);
    size_t                   count()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_sessions.size();
    }

private:
    std::mutex                                       m_mutex;
    std::map<std::string, std::shared_ptr<Session>>  m_sessions;
    std::map<std::string, std::pair<State, Clock::time_point>> m_records;
};

class Session : public std::enable_shared_from_this<Session>
{
public:
    explicit Session(std::string host)
        : m_host(std::move(host)), m_resolver(m_ioc), m_sock(m_ioc), m_deadline(m_ioc), m_tick(m_ioc)
    {}

    void start()
    {
        auto self = shared_from_this();
        std::thread([self]() { self->run(); }).detach();
    }

    // ---- viewer side: any thread ----
    bool add_viewer()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_closing)
            return false;
        ++m_viewers;
        return true;
    }
    void remove_viewer()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_viewers > 0 && --m_viewers == 0)
            m_idle_since = Clock::now();
    }
    State state()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_state;
    }
    // Blocks until the first picture is there or the session has given up; Connecting means "still waiting".
    State wait_first(int timeout_ms)
    {
        std::unique_lock<std::mutex> lk(m_mtx);
        m_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this]() { return m_state != State::Connecting; });
        return m_state;
    }
    // The newest frame newer than `seq`. False when the session is over or nothing came in time.
    bool next_frame(uint64_t& seq, FramePtr& frame, int timeout_ms)
    {
        std::unique_lock<std::mutex> lk(m_mtx);
        m_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this, &seq]() { return m_seq != seq || m_closing; });
        if (m_seq == seq)
            return false;
        seq   = m_seq;
        frame = m_frame;
        return true;
    }

private:
    // ---- session thread ----
    void run()
    {
        begin_video(VideoTarget{ m_host, g_video_port.load(), "/video" }, true);
        arm_tick();
        m_ioc.run();
        State final_state;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_closing = true;
            if (m_state == State::Connecting || m_state == State::Live)
                m_state = m_idle_stop ? State::Idle : State::Failed;
            final_state = m_state;
        }
        m_cv.notify_all();
        Hub::get().retire(m_host, this, final_state);
    }

    void publish(std::string&& frame)
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_frame = std::make_shared<const std::string>(std::move(frame));
            ++m_seq;
            m_state = State::Live;
        }
        m_cv.notify_all();
    }

    // Ends the session with `s` as what viewers (and the status endpoint) are told.
    void finish(State s)
    {
        if (m_stopping)
            return;
        m_stopping = true;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_closing = true;
            m_state   = s;
        }
        m_cv.notify_all();
        abort_io();
        m_deadline.cancel();
        m_tick.cancel();
    }

    void abort_io()
    {
        beast::error_code ig;
        m_resolver.cancel();
        m_sock.close(ig);
        if (m_ws)
            beast::get_lowest_layer(*m_ws).close(ig);
    }

    void arm_deadline(int ms)
    {
        const uint64_t gen = ++m_deadline_gen;
        m_deadline.expires_after(std::chrono::milliseconds(ms));
        auto self = shared_from_this();
        m_deadline.async_wait([self, gen](const beast::error_code& ec) {
            if (ec || gen != self->m_deadline_gen || self->m_stopping)
                return;
            self->abort_io(); // the pending operation completes with an error and routes the failure
        });
    }

    void arm_tick()
    {
        m_tick.expires_after(std::chrono::milliseconds(400));
        auto self = shared_from_this();
        m_tick.async_wait([self](const beast::error_code& ec) {
            if (ec || self->m_stopping)
                return;
            bool leave = false;
            {
                std::lock_guard<std::mutex> lk(self->m_mtx);
                if (self->m_viewers == 0 && Clock::now() - self->m_idle_since > std::chrono::milliseconds(kIdleGraceMs)) {
                    self->m_closing = true; // no new viewer may join this session any more
                    leave           = true;
                }
            }
            if (leave) {
                // Last viewer gone: close the printer's connection, which frees its video slot.
                self->m_idle_stop = true;
                self->finish(State::Idle);
            } else {
                self->arm_tick();
            }
        });
    }

    // ---- the MJPEG stream ----
    void begin_video(const VideoTarget& t, bool allow_fallback)
    {
        if (m_stopping)
            return;
        m_allow_fallback = allow_fallback;
        m_target         = t;
        m_request        = video_request(t);
        beast::error_code ig;
        m_sock.close(ig);
        m_head_buf.consume(m_head_buf.size());
        m_splitter = JpegSplitter();
        arm_deadline(kConnectMs);
        auto self = shared_from_this();
        m_resolver.async_resolve(t.host, std::to_string(t.port), [self](const beast::error_code& ec, tcp::resolver::results_type results) {
            if (self->m_stopping)
                return;
            if (ec)
                return self->video_failed(0);
            asio::async_connect(self->m_sock, results, [self](const beast::error_code& ec2, const tcp::endpoint&) {
                if (self->m_stopping)
                    return;
                if (ec2)
                    return self->video_failed(0);
                beast::error_code ig2;
                self->m_sock.set_option(tcp::no_delay(true), ig2);
                asio::async_write(self->m_sock, asio::buffer(self->m_request), [self](const beast::error_code& ec3, size_t) {
                    if (self->m_stopping)
                        return;
                    if (ec3)
                        return self->video_failed(0);
                    asio::async_read_until(self->m_sock, self->m_head_buf, "\r\n\r\n", [self](const beast::error_code& ec4, size_t head_len) {
                        if (self->m_stopping)
                            return;
                        if (ec4)
                            return self->video_failed(0);
                        self->on_video_head(head_len);
                    });
                });
            });
        });
    }

    void on_video_head(size_t head_len)
    {
        std::string head(asio::buffers_begin(m_head_buf.data()), asio::buffers_begin(m_head_buf.data()) + head_len);
        m_head_buf.consume(head_len);
        const int status = parse_http_status(head);
        if (status != 200)
            return video_failed(status);
        // Whatever followed the head in the same read is already picture.
        std::string rest(asio::buffers_begin(m_head_buf.data()), asio::buffers_end(m_head_buf.data()));
        m_head_buf.consume(m_head_buf.size());
        m_splitter.feed(rest.data(), rest.size());
        drain_frames();
        read_video();
    }

    void drain_frames()
    {
        std::string f;
        while (m_splitter.next(f)) {
            m_got_frame_ever = true;
            publish(std::move(f));
            f.clear();
        }
    }

    void read_video()
    {
        if (m_stopping)
            return;
        arm_deadline(kStallMs);
        auto self = shared_from_this();
        m_sock.async_read_some(asio::buffer(m_rd), [self](const beast::error_code& ec, size_t n) {
            if (self->m_stopping)
                return;
            if (ec)
                return self->video_failed(0);
            self->m_splitter.feed(self->m_rd.data(), n);
            self->drain_frames();
            self->read_video();
        });
    }

    // The stream did not start (no connection, no 200) or ended.
    void video_failed(int http_status)
    {
        if (m_stopping)
            return;
        m_deadline.cancel();
        beast::error_code ig;
        m_sock.close(ig);
        if (m_got_frame_ever) {
            // It was playing and dropped. Try again a couple of times; the viewers stay attached.
            if (m_reconnects < kMaxReconnects) {
                ++m_reconnects;
                return begin_video(m_target, false);
            }
            return finish(State::Failed);
        }
        if (http_status != 0 && http_status != 200) {
            const State s = state_from_http_status(http_status);
            if (s == State::Busy)
                return finish(State::Busy);
        }
        if (m_allow_fallback)
            return begin_enable();
        finish(State::Failed);
    }

    // ---- SDCP Cmd 386, only when asking the stream address directly gave nothing ----
    static std::string new_request_id()
    {
        std::string s = boost::uuids::to_string(boost::uuids::random_generator()());
        s.erase(std::remove(s.begin(), s.end(), '-'), s.end());
        return s;
    }

    void begin_enable()
    {
        if (m_stopping)
            return;
        m_ws = std::make_unique<websocket::stream<tcp::socket>>(m_ioc);
        arm_deadline(kEnableMs);
        auto self = shared_from_this();
        m_resolver.async_resolve(m_host, std::to_string(g_control_port.load()), [self](const beast::error_code& ec, tcp::resolver::results_type results) {
            if (self->m_stopping)
                return;
            if (ec)
                return self->enable_failed();
            asio::async_connect(beast::get_lowest_layer(*self->m_ws), results, [self](const beast::error_code& ec2, const tcp::endpoint&) {
                if (self->m_stopping)
                    return;
                if (ec2)
                    return self->enable_failed();
                self->m_ws->async_handshake(self->m_host + ":" + std::to_string(g_control_port.load()), "/websocket", [self](const beast::error_code& ec3) {
                    if (self->m_stopping)
                        return;
                    if (ec3)
                        return self->enable_failed(); // e.g. the printer's own limit on control connections
                    self->m_enable_msg = enable_request(true, new_request_id(), (long long) std::time(nullptr));
                    self->m_ws->text(true);
                    self->m_ws->async_write(asio::buffer(self->m_enable_msg), [self](const beast::error_code& ec4, size_t) {
                        if (self->m_stopping)
                            return;
                        if (ec4)
                            return self->enable_failed();
                        self->read_enable_reply();
                    });
                });
            });
        });
    }

    void read_enable_reply()
    {
        m_ws_buf.consume(m_ws_buf.size());
        auto self = shared_from_this();
        m_ws->async_read(m_ws_buf, [self](const beast::error_code& ec, size_t) {
            if (self->m_stopping)
                return;
            if (ec)
                return self->enable_failed();
            const EnableReply r = parse_enable_reply(beast::buffers_to_string(self->m_ws_buf.data()));
            if (!r.valid)
                return self->read_enable_reply(); // a status push, not our answer
            self->enable_answered(r);
        });
    }

    void enable_answered(const EnableReply& r)
    {
        const State s = state_from_ack(r.ack);
        if (s != State::Live)
            return finish(s); // 1: the printer is full, 2: no camera, anything else: it could not
        VideoTarget t{ m_host, g_video_port.load(), "/video" };
        VideoTarget from_reply;
        if (video_target_from_reply(r.video_url, m_host, from_reply))
            t = from_reply;
        // Close the control connection politely and go on to the picture; it is not needed to keep
        // the stream running, and the printer has few of them to give out.
        auto self = shared_from_this();
        m_ws->async_close(websocket::close_code::normal, [self, t](const beast::error_code&) {
            if (self->m_stopping)
                return;
            beast::error_code ig;
            if (self->m_ws)
                beast::get_lowest_layer(*self->m_ws).close(ig);
            self->m_ws.reset();
            self->begin_video(t, false);
        });
    }

    void enable_failed()
    {
        if (m_stopping)
            return;
        finish(State::Failed);
    }

    // ---- state ----
    const std::string m_host;

    std::mutex              m_mtx;
    std::condition_variable m_cv;
    State                   m_state   = State::Connecting;
    int                     m_viewers = 0;
    Clock::time_point       m_idle_since = Clock::now();
    bool                    m_closing = false;
    FramePtr                m_frame;
    uint64_t                m_seq = 0;

    // Session-thread-only from here on. m_ioc is first so it is destroyed last.
    asio::io_context                                  m_ioc;
    tcp::resolver                                     m_resolver;
    tcp::socket                                       m_sock;
    std::unique_ptr<websocket::stream<tcp::socket>>   m_ws;
    asio::steady_timer                                m_deadline;
    asio::steady_timer                                m_tick;
    asio::streambuf                                   m_head_buf;
    beast::flat_buffer                                m_ws_buf;
    std::array<char, 65536>                           m_rd;
    JpegSplitter                                      m_splitter;
    VideoTarget                                       m_target;
    std::string                                       m_request;
    std::string                                       m_enable_msg;
    uint64_t                                          m_deadline_gen   = 0;
    bool                                              m_stopping       = false;
    bool                                              m_idle_stop      = false;
    bool                                              m_allow_fallback = true;
    bool                                              m_got_frame_ever = false;
    int                                               m_reconnects     = 0;
};

std::shared_ptr<Session> Hub::acquire(const std::string& host)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_sessions.find(host);
    if (it != m_sessions.end() && it->second->add_viewer())
        return it->second;
    auto s = std::make_shared<Session>(host);
    s->add_viewer();
    m_sessions[host] = s;
    s->start();
    return s;
}

void Hub::retire(const std::string& host, Session* s, State final_state)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_sessions.find(host);
    if (it != m_sessions.end() && it->second.get() == s)
        m_sessions.erase(it);
    if (final_state == State::Busy || final_state == State::NoCamera || final_state == State::Failed)
        m_records[host] = { final_state, Clock::now() };
    else
        m_records.erase(host); // it played: whatever was wrong before is not any more
}

State Hub::status(const std::string& host)
{
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_sessions.find(host);
        if (it != m_sessions.end())
            s = it->second;
        else {
            auto rec = m_records.find(host);
            if (rec != m_records.end() && Clock::now() - rec->second.second < std::chrono::seconds(kRecordKeepSecs))
                return rec->second.first;
            return State::Idle;
        }
    }
    return s->state();
}

// ---- the HTTP side of the local relay ----

void write_all(tcp::socket& s, const std::string& data)
{
    asio::write(s, asio::buffer(data));
}

std::string percent_decode(const std::string& v)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%' && i + 2 < v.size() && std::isxdigit((unsigned char) v[i + 1]) && std::isxdigit((unsigned char) v[i + 2])) {
            out += (char) std::stoi(v.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else if (v[i] == '+') {
            out += ' ';
        } else {
            out += v[i];
        }
    }
    return out;
}

std::string query_value(const std::string& target, const std::string& key)
{
    const size_t q = target.find('?');
    if (q == std::string::npos)
        return std::string();
    size_t pos = q + 1;
    while (pos < target.size()) {
        size_t amp = target.find('&', pos);
        if (amp == std::string::npos)
            amp = target.size();
        const std::string kv = target.substr(pos, amp - pos);
        const size_t eq      = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key)
            return percent_decode(kv.substr(eq + 1));
        pos = amp + 1;
    }
    return std::string();
}

std::string json_response(int code, const char* reason, const std::string& body, const std::string& extra_headers = std::string())
{
    return "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
           "Access-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: X-Elegoo-Camera\r\n" + extra_headers +
           "Connection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

// Releases the viewer slot on every way out of a stream.
struct ViewerGuard
{
    std::shared_ptr<Session> s;
    ~ViewerGuard()
    {
        if (s)
            s->remove_viewer();
    }
};

} // namespace

void serve_client(tcp::socket& client, const std::string& target)
{
    const std::string path = target.substr(0, target.find('?'));
    const std::string host = query_value(target, "ip");
    if (!valid_host(host)) {
        write_all(client, json_response(400, "Bad Request", status_json(State::Failed)));
        return;
    }
    if (path == "/elegoo/status") {
        write_all(client, json_response(200, "OK", status_json(Hub::get().status(host))));
        return;
    }
    if (path != "/elegoo") {
        write_all(client, json_response(404, "Not Found", status_json(State::Failed)));
        return;
    }

    int fps = 0;
    try { fps = std::stoi(query_value(target, "fps")); } catch (...) { fps = 0; }
    if (fps < 0 || fps > 60)
        fps = 0;

    ViewerGuard guard{ Hub::get().acquire(host) };
    const State first = guard.s->wait_first(kFirstFrameMs);
    if (first != State::Live) {
        // A refusal the tile can explain: 503 with the reason in a header and the body.
        const State shown = first == State::Connecting ? State::Failed : first;
        write_all(client, json_response(503, "Service Unavailable", status_json(shown),
                                        std::string("X-Elegoo-Camera: ") + state_name(shown) + "\r\n"));
        return;
    }

    static const char head[] = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: multipart/x-mixed-replace; boundary=elegooframe\r\n"
                               "Cache-Control: no-cache\r\n"
                               "Access-Control-Allow-Origin: *\r\n"
                               "Connection: close\r\n\r\n";
    write_all(client, std::string(head, sizeof(head) - 1));

    uint64_t seq = 0;
    FramePtr frame;
    Clock::time_point last_sent {};
    bool              sent_any = false;
    while (guard.s->next_frame(seq, frame, kFrameWaitMs)) {
        if (fps > 0 && sent_any && Clock::now() - last_sent < std::chrono::milliseconds(1000 / fps))
            continue;
        char part[128];
        const int hl = std::snprintf(part, sizeof(part), "--elegooframe\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", frame->size());
        write_all(client, std::string(part, (size_t) hl));
        write_all(client, *frame);
        write_all(client, "\r\n");
        last_sent = Clock::now();
        sent_any  = true;
    }
}

namespace Testing {
void set_ports(int video_port, int control_port)
{
    g_video_port   = video_port > 0 ? video_port : kVideoPort;
    g_control_port = control_port > 0 ? control_port : kControlPort;
}
size_t session_count()
{
    return Hub::get().count();
}
} // namespace Testing

} // namespace ElegooCam
} // namespace GUI
} // namespace Slic3r
