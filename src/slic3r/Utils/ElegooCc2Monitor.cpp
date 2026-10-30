// The on-demand CC2 connections declared in ElegooCc2Monitor.hpp: at most one MQTT session per
// printer, each driven by its own worker thread, so no caller ever waits on a printer's network.

#include "ElegooCc2Monitor.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace Slic3r {
namespace ElegooCc2Monitor {

namespace {

using namespace ElegooCc2;

const int       CHECKIN_WAIT_MS = 15000;  // a check-in that has heard no whole status by then gives up
const int       PROBE_MS        = 15000;  // a silence probe not answered in this long drops the connection
const int       CANVAS_EVERY_MS = 15000;
const int       NO_CANVAS_EVERY_MS = 60000; // a printer that said it has none: ask again now and then
const int       FORGET_MS       = 600000; // nothing asked about the printer at all: forget it
const int       LOCK_RETRY_MS   = 30000;
const int       SUBSCRIBERS_MS  = 60000;
const int       DEMANDED_BACKOFF = 15000;
const long long BACKOFF_MS[]    = { 2000, 4000, 8000, 16000, 30000, 60000 };

std::atomic<int> g_keep_ms { 60000 };
std::atomic<int> g_checkin_ms { 300000 };
std::atomic<int> g_heartbeat_ms { 10000 };  // elegoo-link's CC2 heartbeat interval
std::atomic<int> g_silence_ms { 45000 };
std::atomic<int> g_stale_s { 120 };
std::atomic<long long> g_subscribers_ms { 0 };

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool subscribers()
{
    const long long t = g_subscribers_ms.load();
    return t != 0 && now_ms() - t < SUBSCRIBERS_MS;
}

bool valid_host(const std::string& h)
{
    if (h.empty() || h.size() > 253) return false;
    for (char c : h)
        if (!(std::isalnum((unsigned char) c) || c == '.' || c == '-' || c == '_' || c == ':')) return false;
    return true;
}

struct Waiter
{
    int                     method { -1 };
    std::string             params;
    std::mutex              m;
    std::condition_variable cv;
    bool                    done { false };
    bool                    sent { false };
    bool                    answered { false };
    int                     error_code { -1 };
    std::string             result;
    std::string             error;
    std::atomic<bool>       abandoned { false }; // the caller stopped waiting: drop it when it is pending

    void finish(bool ans, int code, const std::string& res, const std::string& err)
    {
        std::lock_guard<std::mutex> lock(m);
        if (done) return;
        done       = true;
        answered   = ans;
        error_code = code;
        result     = res;
        error      = err;
        cv.notify_all();
    }
    void mark_sent()
    {
        std::lock_guard<std::mutex> lock(m);
        sent = true;
    }
};

std::mutex g_connector_mutex;
Connector  g_connector; // empty = net_connector

Connector connector()
{
    std::lock_guard<std::mutex> lock(g_connector_mutex);
    return g_connector ? g_connector : Connector(net_connector);
}

enum class Internal { Attributes, Full, Probe, Canvas };

class Link : public std::enable_shared_from_this<Link>
{
public:
    Link(std::string host, std::string token, int seq) : m_host(std::move(host)), m_token(std::move(token)), m_seq(seq)
    {
        m_last_touch = now_ms();
        m_snap.exists = true;
    }

    void start()
    {
        auto self = shared_from_this();
        m_thread  = std::thread([self]() { self->run(); });
    }

    // Any thread. Asks the worker to end; joins it for at most wait_ms, else leaves it to finish
    // on its own (it holds its own reference).
    void stop(int wait_ms)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_cv.notify_all();
        if (!m_thread.joinable()) return;
        if (std::this_thread::get_id() == m_thread.get_id()) {
            m_thread.detach();
            return;
        }
        const long long deadline = now_ms() + wait_ms;
        while (!m_finished.load() && now_ms() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (m_finished.load())
            m_thread.join();
        else
            m_thread.detach();
    }

    void touched(const std::string& token)
    {
        m_last_touch = now_ms();
        set_token(token);
    }

    void demanded(const std::string& token)
    {
        const long long now = now_ms();
        m_last_touch        = now;
        m_last_demand       = now;
        set_token(token);
        m_cv.notify_all();
    }

    bool idle() const { return m_finished.load() || m_stopping_flag.load(); }

    Snapshot snapshot()
    {
        std::lock_guard<std::mutex> lock(m_state);
        Snapshot s = m_snap;
        s.now_ms   = now_ms();
        return s;
    }

    bool connected()
    {
        std::lock_guard<std::mutex> lock(m_state);
        return m_snap.connected;
    }

    std::shared_ptr<Waiter> request(int method, const std::string& params)
    {
        auto w    = std::make_shared<Waiter>();
        w->method = method;
        w->params = params;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back(w);
        }
        m_cv.notify_all();
        return w;
    }

    void forget(const std::shared_ptr<Waiter>& w)
    {
        w->abandoned = true;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.erase(std::remove(m_queue.begin(), m_queue.end(), w), m_queue.end());
    }

private:
    void set_token(const std::string& token)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_token = token;
    }

    std::string token()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_token;
    }

    bool stopping()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stopping;
    }

    bool is_demanded(long long now) const { return now - m_last_demand.load() < g_keep_ms.load(); }

    // Waits for a demand, a queued request or a stop, at most ms.
    // Only called without a session: a queued request is a demand too (call() demands first).
    // wake_on_demand false: only a stop ends the wait early (another window holds the printer).
    void wait(int ms, bool wake_on_demand = true)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(ms), [&]() {
            return m_stopping || (wake_on_demand && is_demanded(now_ms()) && now_ms() >= m_next_attempt);
        });
    }

    // ---------------------------------------------------------- one PC, one connection ----

    void acquire_lock()
    {
        if (m_lock) return;
        try {
            const std::string dir = data_dir();
            if (dir.empty()) {
                set_other_window(false);
                return;
            }
            boost::filesystem::path   p = boost::filesystem::path(dir) / "hub" / "cc2";
            boost::system::error_code ec;
            boost::filesystem::create_directories(p, ec);
            std::string name;
            for (char c : m_host) name += std::isalnum((unsigned char) c) ? c : '_';
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
            BOOST_LOG_TRIVIAL(warning) << "CC2 printer " << m_seq << ": no connection lock (" << e.what() << "), connecting anyway";
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
            BOOST_LOG_TRIVIAL(info) << "CC2 printer " << m_seq << ": another EdgeSlicer window holds its connection";
        m_snap.other_window = other;
    }

    // ------------------------------------------------------------------ the worker ----

    void run()
    {
        try {
            acquire_lock();
            while (!stopping()) {
                const long long now = now_ms();
                if (now - std::max(m_last_touch.load(), m_last_demand.load()) > FORGET_MS) {
                    BOOST_LOG_TRIVIAL(info) << "CC2 printer " << m_seq << ": nothing asked about it for ten minutes, forgetting it";
                    break;
                }
                if (!may_connect()) {
                    if (now - m_lock_tried > LOCK_RETRY_MS) acquire_lock();
                    if (!may_connect()) {
                        fail_queue("another EdgeSlicer window is connected to this printer");
                        wait(1000, false);
                        continue;
                    }
                }
                const bool wanted = is_demanded(now) || now < m_hold_until;
                if (!m_session) {
                    if (wanted ? now >= m_next_attempt : now >= m_next_checkin) {
                        connect(!wanted);
                        continue;
                    }
                    if (!wanted) fail_queue("the printer is not connected");
                    wait(250);
                    continue;
                }
                step(now, wanted);
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "CC2 printer " << m_seq << ": worker stopped: " << e.what();
        } catch (...) {}
        close_session();
        {
            std::lock_guard<std::mutex> lock(m_state);
            m_snap.connected  = false;
            m_snap.connecting = false;
        }
        fail_pending("the connection to the printer was closed");
        fail_queue("the connection to the printer was closed");
        m_lock.reset();
        m_stopping_flag = true;
        m_finished      = true;
    }

    void connect(bool checkin)
    {
        m_checkin           = checkin;
        m_status_since_open = false;
        {
            std::lock_guard<std::mutex> lock(m_state);
            m_snap.connecting = true;
        }
        std::string                    sn, error;
        std::unique_ptr<MqttTransport> transport;
        bool                           ok = false;
        try {
            ok = connector()(m_host, token(), sn, transport, error) && transport;
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!ok) {
            fail(error.empty() ? std::string("the printer does not answer") : error);
            return;
        }
        if (stopping()) return;
        m_transport = std::move(transport);
        SessionOptions so;
        so.register_timeout_ms = 3000;
        so.reply_timeout_ms    = 5000;
        m_session              = std::make_unique<Session>(*m_transport, sn, so);
        if (!m_session->open(error)) {
            fail(error.empty() ? std::string("the printer refused the connection") : error);
            return;
        }
        if (!m_transport->subscribe(status_topic(sn), error)) {
            fail(error.empty() ? std::string("could not subscribe to the printer's status") : error);
            return;
        }
        on_open();
    }

    void on_open()
    {
        const long long now = now_ms();
        m_failures          = 0;
        m_open_at           = now;
        m_last_frame        = now;
        m_last_ping         = now;
        m_probe_sent        = 0;
        m_last_canvas       = 0;
        m_was_demanded      = false;
        {
            std::lock_guard<std::mutex> lock(m_state);
            m_snap.connected      = true;
            m_snap.connecting     = false;
            m_snap.ever_connected = true;
            m_snap.failures       = 0;
            m_snap.error.clear();
        }
        BOOST_LOG_TRIVIAL(info) << "CC2 printer " << m_seq << (m_checkin ? ": connected for a check-in" : ": connected");
        send_internal(METHOD_GET_ATTRIBUTES, Internal::Attributes);
        send_internal(METHOD_GET_STATUS, Internal::Full);
    }

    void send_internal(int method, Internal what)
    {
        if (!m_session) return;
        int         id = -1;
        std::string error;
        if (m_session->send(method, "{}", id, error)) m_internal[id] = { what, now_ms() };
    }

    bool full_pending() const
    {
        for (const auto& kv : m_internal)
            if (kv.second.first == Internal::Full || kv.second.first == Internal::Probe) return true;
        return false;
    }

    void step(long long now, bool wanted)
    {
        // Requests from other threads go out first.
        std::deque<std::shared_ptr<Waiter>> queue;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            queue.swap(m_queue);
        }
        for (auto it = m_pending.begin(); it != m_pending.end();)
            it = it->second->abandoned ? m_pending.erase(it) : std::next(it);
        // Our own requests the printer never answered are forgotten, so they are asked again.
        for (auto it = m_internal.begin(); it != m_internal.end();)
            it = now - it->second.second > PROBE_MS ? m_internal.erase(it) : std::next(it);
        for (const auto& w : queue) {
            if (w->abandoned) continue;
            int         id = -1;
            std::string error;
            if (!m_session->send(w->method, w->params, id, error)) {
                w->finish(false, -1, std::string(), error.empty() ? std::string("the request could not be sent") : error);
                continue;
            }
            w->mark_sent();
            m_pending[id] = w;
        }
        // Whatever the printer says.
        for (int i = 0; i < 20; ++i) {
            std::string topic, payload;
            if (!m_transport->receive(topic, payload, i == 0 ? 100 : 0)) break;
            on_message(topic, payload);
            if (!m_session) return;
        }
        if (!m_session) return;
        const long long t = now_ms();
        if (!m_transport->alive()) {
            fail("the connection to the printer was lost");
            return;
        }
        bool printing = false;
        bool canvas   = false, canvas_known = false;
        {
            std::lock_guard<std::mutex> lock(m_state);
            printing     = m_snap.status.valid && is_printing(m_snap.status);
            canvas       = m_snap.has_canvas;
            canvas_known = m_snap.canvas_known;
        }
        if (m_status_since_open && printing && subscribers()) m_hold_until = t + g_keep_ms.load();
        wanted = wanted || t < m_hold_until;
        if (m_checkin) {
            if (m_status_since_open)
                m_checkin = false;
            else if (t - m_open_at > CHECKIN_WAIT_MS) {
                fail("the printer did not report its status");
                return;
            }
        }
        if (!m_checkin && !wanted && m_pending.empty()) {
            close_idle();
            return;
        }
        if (m_probe_sent > 0 && m_last_frame < m_probe_sent && t - m_probe_sent > PROBE_MS) {
            fail("the printer stopped answering");
            return;
        }
        if (t - m_last_ping >= g_heartbeat_ms.load()) {
            m_last_ping = t;
            std::string error;
            m_transport->publish(request_topic(m_session->sn(), m_session->client_id()), ping_payload(), error);
        }
        if (t - m_last_frame >= g_silence_ms.load() && (m_probe_sent == 0 || m_probe_sent < m_last_frame)) {
            m_probe_sent = t;
            send_internal(METHOD_GET_STATUS, Internal::Probe);
        }
        const bool demanded = is_demanded(t);
        if (demanded && !m_was_demanded && m_status_since_open && !full_pending())
            send_internal(METHOD_GET_STATUS, Internal::Full); // somebody just started looking: a fresh one
        m_was_demanded = demanded;
        const int every = (!canvas_known || canvas) ? CANVAS_EVERY_MS : NO_CANVAS_EVERY_MS;
        if (demanded && m_status_since_open && t - m_last_canvas >= every) {
            m_last_canvas = t;
            send_internal(METHOD_GET_CANVAS, Internal::Canvas);
        }
    }

    void on_message(const std::string& topic, const std::string& payload)
    {
        const long long now = now_ms();
        m_last_frame        = now;
        const std::string sn = m_session->sn();
        if (topic == status_topic(sn)) {
            PushResult r;
            {
                std::lock_guard<std::mutex> lock(m_state);
                r = merge_push(m_snap.status, payload);
                if (r == PushResult::Merged) {
                    m_snap.status_at_ms = now;
                    take_canvas_from_status();
                }
            }
            if (r == PushResult::NeedFull && !full_pending()) send_internal(METHOD_GET_STATUS, Internal::Full);
            return;
        }
        if (topic != response_topic(sn, m_session->client_id())) return;
        if (is_pong(payload)) return;
        const Reply reply = parse_reply(payload);
        if (!reply.valid) return;
        auto pw = m_pending.find(reply.id);
        if (pw != m_pending.end()) {
            pw->second->finish(true, reply.error_code, reply.result, std::string());
            m_pending.erase(pw);
            return;
        }
        auto pi = m_internal.find(reply.id);
        if (pi == m_internal.end()) return;
        const Internal what = pi->second.first;
        m_internal.erase(pi);
        const nlohmann::json result = nlohmann::json::parse(reply.result.empty() ? std::string("{}") : reply.result, nullptr, false);
        if (result.is_discarded() || !result.is_object()) return;
        std::lock_guard<std::mutex> lock(m_state);
        switch (what) {
        case Internal::Attributes: {
            if (reply.error_code != 0 && reply.error_code != -1) break;
            if (!result.contains("software_version")) break;
            const nlohmann::json& sw = result["software_version"];
            if (sw.is_object() && sw.contains("ota_version") && sw["ota_version"].is_string())
                m_snap.firmware = sw["ota_version"].get<std::string>();
            break;
        }
        case Internal::Full:
        case Internal::Probe:
            if (reply.error_code != 0) break;
            merge_full(m_snap.status, result);
            m_snap.status_at_ms = now;
            m_status_since_open = true;
            take_canvas_from_status();
            break;
        case Internal::Canvas: {
            if (reply.error_code != 0) break;
            const auto ci = result.find("canvas_info");
            CanvasInfo info;
            std::string why;
            bool any = false;
            if (ci != result.end() && ci->is_object() && canvas_parse_payload(ci->dump(), info, why))
                for (const CanvasUnit& u : info.units) any = any || u.connected;
            m_snap.canvas_known = true;
            m_snap.has_canvas   = any;
            if (any) m_snap.canvas = std::move(info);
            else m_snap.canvas = CanvasInfo();
            break;
        }
        }
    }

    // Under m_state: a status that carries canvas_info says what the CANVAS is too.
    void take_canvas_from_status()
    {
        CanvasInfo info;
        if (!canvas_from_status(m_snap.status, info)) return;
        bool any = false;
        for (const CanvasUnit& u : info.units) any = any || u.connected;
        m_snap.canvas_known = true;
        m_snap.has_canvas   = any;
        m_snap.canvas       = any ? std::move(info) : CanvasInfo();
    }

    // -------------------------------------------------------------- closing, failing ----

    void close_session()
    {
        if (m_session) m_session->close();
        m_session.reset();
        if (m_transport) m_transport->close();
        m_transport.reset();
        m_internal.clear();
    }

    void fail_pending(const std::string& why)
    {
        for (auto& kv : m_pending) kv.second->finish(false, -1, std::string(), why);
        m_pending.clear();
    }

    void fail_queue(const std::string& why)
    {
        std::deque<std::shared_ptr<Waiter>> queue;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            queue.swap(m_queue);
        }
        for (const auto& w : queue) w->finish(false, -1, std::string(), why);
    }

    void close_idle()
    {
        close_session();
        m_checkin      = false;
        m_failures     = 0;
        m_next_checkin = now_ms() + g_checkin_ms.load();
        {
            std::lock_guard<std::mutex> lock(m_state);
            m_snap.connected  = false;
            m_snap.connecting = false;
            m_snap.failures   = 0;
            m_snap.error.clear();
        }
        BOOST_LOG_TRIVIAL(info) << "CC2 printer " << m_seq << ": closed (nothing needs it now)";
        fail_pending("the connection to the printer was closed");
    }

    void fail(const std::string& why)
    {
        const bool was_open = m_session && m_session->is_open() && m_status_since_open;
        close_session();
        m_checkin = false;
        ++m_failures;
        const long long now   = now_ms();
        long long       delay = BACKOFF_MS[std::min<size_t>(m_failures - 1, sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0]) - 1)];
        if (is_demanded(now)) delay = std::min<long long>(delay, DEMANDED_BACKOFF);
        if (was_open) delay = 2000;
        m_next_attempt = now + delay;
        m_next_checkin = now + g_checkin_ms.load();
        {
            std::lock_guard<std::mutex> lock(m_state);
            m_snap.connected  = false;
            m_snap.connecting = false;
            m_snap.failures   = m_failures;
            m_snap.error      = why;
        }
        if (was_open || m_failures == 1) BOOST_LOG_TRIVIAL(info) << "CC2 printer " << m_seq << ": " << why;
        fail_pending(why);
        fail_queue(why);
    }

    const std::string m_host;
    std::string       m_token; // under m_mutex; never logged
    const int         m_seq;
    std::thread       m_thread;
    std::atomic<bool> m_finished { false };
    std::atomic<bool> m_stopping_flag { false };

    std::mutex                          m_mutex; // m_token, m_stopping, m_queue
    std::condition_variable             m_cv;
    bool                                m_stopping { false };
    std::deque<std::shared_ptr<Waiter>> m_queue;

    // Worker thread only.
    std::unique_ptr<MqttTransport>          m_transport;
    std::unique_ptr<Session>                m_session;
    std::map<int, std::shared_ptr<Waiter>>  m_pending;
    std::map<int, std::pair<Internal, long long>> m_internal; // id -> what, when it was sent
    bool                                    m_checkin { false };
    bool                                    m_status_since_open { false };
    bool                                    m_was_demanded { false };
    int                                     m_failures { 0 };
    long long                               m_next_attempt { 0 };
    long long                               m_next_checkin { 0 };
    long long                               m_hold_until { 0 };
    long long                               m_open_at { 0 }, m_last_frame { 0 }, m_last_ping { 0 }, m_probe_sent { 0 }, m_last_canvas { 0 };
    std::unique_ptr<boost::interprocess::file_lock> m_lock;
    bool                                    m_lock_failed { false };
    long long                               m_lock_tried { 0 };
    std::atomic<long long>                  m_last_touch { 0 };
    std::atomic<long long>                  m_last_demand { 0 };

    std::mutex m_state;
    Snapshot   m_snap;
};

// Never destroyed: a worker may still be finishing at exit.
struct Manager
{
    std::mutex                                   m;
    std::map<std::string, std::shared_ptr<Link>> links;
    int                                          next_seq { 1 };
    bool                                         down { false };

    Manager()
    {
        if (const char* p = std::getenv("SNORCA_CC2_KEEP_S")) {
            const int v = std::atoi(p);
            if (v > 0 && v < 3600) g_keep_ms = v * 1000;
        }
        if (const char* p = std::getenv("SNORCA_CC2_CHECKIN_S")) {
            const int v = std::atoi(p);
            if (v > 0 && v < 86400) g_checkin_ms = v * 1000;
        }
    }
};

Manager& mgr()
{
    static Manager* m = new Manager();
    return *m;
}

std::shared_ptr<Link> find_link(const std::string& host, const std::string& token, bool create)
{
    if (!valid_host(host)) return nullptr;
    Manager&                    M = mgr();
    std::lock_guard<std::mutex> lock(M.m);
    auto                        it = M.links.find(host);
    if (it != M.links.end() && it->second->idle()) {
        it->second->stop(0);
        M.links.erase(it);
        it = M.links.end();
    }
    if (it != M.links.end()) return it->second;
    if (!create || M.down) return nullptr;
    auto l = std::make_shared<Link>(host, token, M.next_seq++);
    M.links[host] = l;
    l->start();
    return l;
}

void stop_all(int wait_ms, bool mark_down)
{
    Manager&                           M = mgr();
    std::vector<std::shared_ptr<Link>> links;
    {
        std::lock_guard<std::mutex> lock(M.m);
        if (mark_down) M.down = true;
        for (auto& kv : M.links) links.push_back(kv.second);
        M.links.clear();
    }
    for (auto& l : links) l->stop(wait_ms);
    std::lock_guard<std::mutex> lock(M.m);
    M.down = false;
}

} // namespace

Snapshot touch(const std::string& host, const std::string& token)
{
    std::shared_ptr<Link> l = find_link(host, token, true);
    if (!l) return Snapshot();
    l->touched(token);
    return l->snapshot();
}

Snapshot demand(const std::string& host, const std::string& token)
{
    std::shared_ptr<Link> l = find_link(host, token, true);
    if (!l) return Snapshot();
    l->demanded(token);
    return l->snapshot();
}

void note_subscribers(bool any) { g_subscribers_ms = any ? now_ms() : 0; }

bool connected(const std::string& host)
{
    std::shared_ptr<Link> l = find_link(host, std::string(), false);
    return l && l->connected();
}

CallResult call(const std::string& host, const std::string& token, int method, const std::string& params_json, int timeout_ms)
{
    CallResult            r;
    std::shared_ptr<Link> l = find_link(host, token, true);
    if (!l) {
        r.error = "no such printer";
        return r;
    }
    l->demanded(token);
    std::shared_ptr<Waiter> w = l->request(method, params_json.empty() ? std::string("{}") : params_json);
    {
        std::unique_lock<std::mutex> lock(w->m);
        w->cv.wait_for(lock, std::chrono::milliseconds(std::max(100, timeout_ms)), [&]() { return w->done; });
        r.sent       = w->sent;
        r.answered   = w->done && w->answered;
        r.error_code = w->error_code;
        r.result     = w->result;
        r.error      = w->done ? w->error : std::string("the printer did not answer in time");
    }
    l->forget(w);
    return r;
}

void shutdown() { stop_all(2000, true); }

void set_connector_for_tests(Connector c)
{
    std::lock_guard<std::mutex> lock(g_connector_mutex);
    g_connector = std::move(c);
}

void set_timing_for_tests(int keep_ms, int checkin_ms, int heartbeat_ms, int silence_ms)
{
    g_keep_ms      = keep_ms > 0 ? keep_ms : 60000;
    g_checkin_ms   = checkin_ms > 0 ? checkin_ms : 300000;
    g_heartbeat_ms = heartbeat_ms > 0 ? heartbeat_ms : 10000;
    g_silence_ms   = silence_ms > 0 ? silence_ms : 45000;
}

int stale_after_s() { return g_stale_s.load(); }

void reset_for_tests()
{
    stop_all(8000, false);
    g_subscribers_ms = 0;
}

} // namespace ElegooCc2Monitor
} // namespace Slic3r
