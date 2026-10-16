#include "U1WsRoute.hpp"

#include "SnapmakerLan.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace Slic3r {
namespace GUI {

using nlohmann::json;

namespace u1ws {
static long long steady_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Never let the last reference to the page's client go on a Paho or WebSocket thread.
static void release_off_thread(std::shared_ptr<MqttClient>&& c)
{
    if (!c)
        return;
    if (c.use_count() == 1) {
        std::thread([doomed = std::move(c)]() mutable { doomed.reset(); }).detach();
        return;
    }
    c.reset();
}

// ---- per printer, for the whole process: back-off and the methods the LAN does not serve ----
// (the page builds a new cloud session - and so a new route - on every reconnect or printer switch)
struct WsPace
{
    int                   failures { 0 };
    long long             next_try { 0 };
    std::string           last_reason;
    std::set<std::string> cloud_methods;
};
static std::mutex& pace_mutex()
{
    static std::mutex m;
    return m;
}
static std::map<std::string, WsPace>& paces()
{
    static std::map<std::string, WsPace> p;
    return p;
}

} // namespace u1ws

struct U1WsRoute::State
{
    std::weak_ptr<MqttClient> owner;
    U1WsDeps                  deps;
    int                       number { 0 }; // "route #n" in the log: never the serial or address

    mutable std::mutex      mtx;
    std::condition_variable cv;
    bool                    stop { false };
    bool                    kicked { false };
    bool                    finished { false };

    std::string              sn;
    std::vector<std::string> sniffed;

    bool                         active { false };
    std::shared_ptr<MoonrakerWs> ws;
    int                          generation { 0 };
    bool                         lost { false };
    std::string                  lost_why;

    struct Pending
    {
        std::string payload, method;
        long long   sent_at { 0 };
        bool        own { false }; // the route's own request (probe, subscribe replay)
        // A write (anything not read-only): method + params, so the same write sent again while
        // this one still runs is not run twice (the page re-sends after its own 3 s timeout, and a
        // G-code like a tool change answers only when it has finished).
        std::string              signature;
        std::vector<std::string> extra_ids; // ids of the same write sent again (JSON text)
        bool                     late_logged { false };
    };
    std::map<std::string, Pending> pending;   // sent over the LAN, waiting for the answer
    std::set<std::string>          cloud_ids; // sent through the cloud while the LAN serves
    std::string                    last_subscribe;
    int                            timeouts_in_row { 0 };
    long long                      own_seq { 0 };
    std::string                    probe_key;
    bool                           probe_done { false };
    bool                           probe_ok { false };

    int         failures { 0 };
    long long   next_try { 0 };
    std::string last_reason;

    long long now() const { return deps.now_ms ? deps.now_ms() : u1ws::steady_ms(); }
    bool      sleep(long long ms)
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return stop; });
        return !stop;
    }
};

using WsStatePtr = std::shared_ptr<U1WsRoute::State>;
namespace u1ws {

// Lock order: State::mtx, then pace_mutex().
static void load_pace(U1WsRoute::State& s)
{
    std::lock_guard<std::mutex> lk(pace_mutex());
    auto                        it = paces().find(s.sn);
    if (it == paces().end())
        return;
    s.failures    = it->second.failures;
    s.next_try    = it->second.next_try;
    s.last_reason = it->second.last_reason;
}
static void save_pace(const U1WsRoute::State& s)
{
    if (s.sn.empty())
        return;
    std::lock_guard<std::mutex> lk(pace_mutex());
    WsPace& p      = paces()[s.sn];
    p.failures     = s.failures;
    p.next_try     = s.next_try;
    p.last_reason  = s.last_reason;
}
static bool is_cloud_method(const std::string& sn, const std::string& method)
{
    std::lock_guard<std::mutex> lk(pace_mutex());
    auto                        it = paces().find(sn);
    return it != paces().end() && it->second.cloud_methods.count(method) > 0;
}
static bool add_cloud_method(const std::string& sn, const std::string& method)
{
    std::lock_guard<std::mutex> lk(pace_mutex());
    return paces()[sn].cloud_methods.insert(method).second;
}

static void note_failure(const WsStatePtr& s, const std::string& why)
{
    std::lock_guard<std::mutex> lk(s->mtx);
    ++s->failures;
    const long long wait = U1LocalFirst::retry_after_ms(s->deps.timing, s->failures);
    s->next_try          = s->now() + wait;
    if (why != s->last_reason)
        BOOST_LOG_TRIVIAL(info) << "[U1WsRoute] route #" << s->number << ": the printer stays on the cloud: " << why
                                << " (next LAN try in " << wait / 1000 << " s)";
    s->last_reason = why;
    save_pace(*s);
}

static void deliver(const WsStatePtr& s, const std::string& topic, const std::string& payload)
{
    std::shared_ptr<MqttClient> o = s->owner.lock();
    if (o)
        o->Deliver(topic, payload);
    release_off_thread(std::move(o));
}

static void publish_cloud(const WsStatePtr& s, const std::string& topic, const std::string& payload)
{
    std::shared_ptr<MqttClient> o = s->owner.lock();
    if (o) {
        std::string msg;
        try {
            o->PublishDirect(topic, payload, 1, msg);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "[U1WsRoute] cloud publish failed: " << e.what();
        }
    }
    release_off_thread(std::move(o));
}

// ---- the WebSocket's messages (on its I/O thread) ----
static void on_ws_message(const WsStatePtr& s, int generation, const std::string& text)
{
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return;
    std::string sn;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        if (s->stop || generation != s->generation)
            return;
        sn = s->sn;
    }
    // A notification: Moonraker's own stream, in the shape the cloud carries it.
    if (j.contains("method") && !j.contains("id")) {
        bool active;
        {
            std::lock_guard<std::mutex> lk(s->mtx);
            active = s->active;
        }
        if (active)
            deliver(s, U1LocalFirst::topic_for_notification(sn, j["method"].is_string() ? j["method"].get<std::string>() : ""), text);
        return;
    }
    const std::string key = U1LocalFirst::rpc_id_key(j);
    U1WsRoute::State::Pending p;
    bool                      found = false;
    bool                      active;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        active   = s->active;
        auto it  = s->pending.find(key);
        if (it != s->pending.end()) {
            p     = it->second;
            found = true;
            s->pending.erase(it);
            s->timeouts_in_row = 0;
        }
        if (found && p.own) {
            if (key == s->probe_key) {
                s->probe_done = true;
                s->probe_ok   = j.contains("result");
                s->cv.notify_all();
            }
            return; // the route's own request: nothing for the page
        }
        if (found && U1LocalFirst::is_method_not_found(j))
            s->cloud_ids.insert(key);
    }
    if (found && U1LocalFirst::is_method_not_found(j)) {
        // Not on the LAN: through the cloud, now and from now on, for this printer.
        if (add_cloud_method(sn, p.method))
            BOOST_LOG_TRIVIAL(info) << "[U1WsRoute] route #" << s->number << ": " << p.method
                                    << " is not served on the LAN; it goes through the cloud";
        publish_cloud(s, sn + "/request", p.payload);
        return;
    }
    if (!active)
        return;
    deliver(s, sn + "/response", text);
    // The same write sent again while it ran: every id the page used gets the answer.
    for (const std::string& extra : p.extra_ids) {
        json copy  = j;
        copy["id"] = json::parse(extra, nullptr, false);
        deliver(s, sn + "/response", copy.dump());
    }
}

static void on_ws_closed(const WsStatePtr& s, int generation, const std::string& why)
{
    std::lock_guard<std::mutex> lk(s->mtx);
    if (generation != s->generation || !s->active)
        return;
    s->lost     = true;
    s->lost_why = "the LAN connection dropped (" + why + ")";
    s->kicked   = true;
    s->cv.notify_all();
}

// ---- falling back, and coming back ----
static void fall_back(const WsStatePtr& s, const std::string& why)
{
    std::shared_ptr<MoonrakerWs>                     ws;
    std::map<std::string, U1WsRoute::State::Pending> pending;
    std::string                                      sn, replay;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        if (!s->active)
            return;
        s->active = false;
        s->lost   = false;
        ++s->generation;
        ws      = std::move(s->ws);
        pending = std::move(s->pending);
        s->pending.clear();
        s->cloud_ids.clear();
        s->timeouts_in_row = 0;
        s->failures        = 1;
        s->next_try        = s->now() + U1LocalFirst::retry_after_ms(s->deps.timing, 1);
        s->last_reason     = why;
        save_pace(*s);
        sn     = s->sn;
        replay = s->last_subscribe;
    }
    BOOST_LOG_TRIVIAL(warning) << "[U1WsRoute] route #" << s->number << ": the printer falls back to the cloud: " << why;
    if (ws)
        ws->close();
    // The page's status stream: ask the cloud for it again (its answer has an id nobody waits for).
    if (!replay.empty()) {
        json r = json::parse(replay, nullptr, false);
        if (r.is_object()) {
            r["id"] = "edge-cloud-resub-" + std::to_string(steady_ms());
            publish_cloud(s, sn + "/request", r.dump());
        }
    }
    // What was waiting on the LAN: a request that only reads is asked again through the cloud.
    for (const auto& kv : pending)
        if (!kv.second.own && U1LocalFirst::is_idempotent_method(kv.second.method))
            publish_cloud(s, sn + "/request", kv.second.payload);
    if (s->deps.route_changed)
        s->deps.route_changed(false, why);
}

static std::string next_own_key(U1WsRoute::State& s) { return "\"edge-ws-" + std::to_string(++s.own_seq) + "\""; }

static void attempt(const WsStatePtr& s, const std::string& sn)
{
    const U1WsDeps& d = s->deps;
    std::vector<std::string> addresses = d.known_addresses ? d.known_addresses(sn) : std::vector<std::string>();
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        for (const std::string& a : s->sniffed)
            if (std::find(addresses.begin(), addresses.end(), a) == addresses.end())
                addresses.push_back(a);
    }
    if (addresses.empty())
        return note_failure(s, "no LAN address known yet");
    std::string ip;
    for (const std::string& a : addresses) {
        if (!SnapmakerLan::is_local_address(a)) // a local / private address only, never the cloud
            continue;
        if (!d.is_that_printer || d.is_that_printer(a, sn)) {
            ip = a;
            break;
        }
    }
    if (ip.empty())
        return note_failure(s, "the printer does not answer on this network");

    std::shared_ptr<MoonrakerWs> ws = MoonrakerWs::create();
    int                          gen;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        gen = ++s->generation;
    }
    std::weak_ptr<U1WsRoute::State> weak = s;
    ws->set_handlers(
        [weak, gen](const std::string& text) {
            if (WsStatePtr st = weak.lock())
                on_ws_message(st, gen, text);
        },
        [weak, gen](const std::string& why) {
            if (WsStatePtr st = weak.lock())
                on_ws_closed(st, gen, why);
        });
    std::string error;
    if (!ws->connect(ip, d.ws_port, d.connect_timeout_ms, d.idle_timeout_ms, error))
        return note_failure(s, "the printer's WebSocket did not open (" + error + ")");

    // Does it answer JSON-RPC? server.info, read-only.
    std::string probe;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        const std::string key = next_own_key(*s);
        s->probe_key          = key;
        s->probe_done = s->probe_ok = false;
        s->pending[key]       = { "", "server.info", s->now(), true };
        probe = json({ { "jsonrpc", "2.0" }, { "method", "server.info" }, { "id", json::parse(key) } }).dump();
    }
    bool answered = false;
    if (ws->send(probe)) {
        std::unique_lock<std::mutex> lk(s->mtx);
        s->cv.wait_for(lk, std::chrono::milliseconds(d.connect_timeout_ms), [&s] { return s->stop || s->probe_done; });
        answered = s->probe_done && s->probe_ok;
    }
    if (!answered) {
        ws->close();
        return note_failure(s, "the printer's WebSocket did not answer server.info");
    }

    std::string replay;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        if (s->stop) {
            ws->close();
            return;
        }
        s->ws              = ws;
        s->active          = true;
        s->lost            = false;
        s->failures        = 0;
        s->timeouts_in_row = 0;
        s->cloud_ids.clear();
        s->last_reason.clear();
        save_pace(*s);
        if (!s->last_subscribe.empty()) {
            json r = json::parse(s->last_subscribe, nullptr, false);
            if (r.is_object()) {
                const std::string key = next_own_key(*s);
                r["id"]               = json::parse(key);
                s->pending[key]       = { "", "printer.objects.subscribe", s->now(), true };
                replay                = r.dump();
            }
        }
    }
    // The page's status stream comes from the LAN from now on.
    if (!replay.empty())
        ws->send(replay);
    BOOST_LOG_TRIVIAL(info) << "[U1WsRoute] route #" << s->number << ": the printer is served over the LAN (Moonraker WebSocket)";
    if (d.route_changed)
        d.route_changed(true, "");
}

static void check_timeouts(const WsStatePtr& s)
{
    std::vector<U1WsRoute::State::Pending> late_reads;
    std::vector<std::string>               slow_writes;
    std::string                            sn;
    bool                                   give_up = false;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        if (!s->active)
            return;
        sn                = s->sn;
        const long long t = s->now();
        for (auto it = s->pending.begin(); it != s->pending.end();) {
            U1WsRoute::State::Pending& p   = it->second;
            const long long            age = t - p.sent_at;
            if (age < s->deps.request_timeout_ms) {
                ++it;
                continue;
            }
            if (p.own) {
                it = s->pending.erase(it);
                continue;
            }
            if (U1LocalFirst::is_idempotent_method(p.method)) {
                // A read that never came back: ask the cloud (safe twice), and count it.
                late_reads.push_back(p);
                ++s->timeouts_in_row;
                const json j = json::parse(p.payload, nullptr, false);
                s->cloud_ids.insert(U1LocalFirst::rpc_id_key(j));
                it = s->pending.erase(it);
                continue;
            }
            // A write is still running on the printer (a tool change, homing, a long macro): its
            // answer comes when it is done and is delivered then. It never counts towards the
            // fallback and is never sent again. Forgotten only after write_max_ms.
            if (age >= s->deps.write_max_ms) {
                it = s->pending.erase(it);
                continue;
            }
            if (!p.late_logged) {
                p.late_logged = true;
                slow_writes.push_back(p.method);
            }
            ++it;
        }
        give_up = s->timeouts_in_row >= 2;
    }
    for (const auto& p : late_reads)
        publish_cloud(s, sn + "/request", p.payload); // only reads: safe to ask twice
    for (const auto& m : slow_writes)
        BOOST_LOG_TRIVIAL(info) << "[U1WsRoute] route #" << s->number << ": " << m
                                << " is still running on the printer; its answer is delivered when it comes";
    if (give_up)
        fall_back(s, "LAN answers to reads timed out");
}

static void run(WsStatePtr s)
{
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(s->mtx);
            if (s->stop)
                break;
            long long wait = 500;
            if (!s->active && !s->sn.empty())
                wait = (std::max)(20LL, (std::min)(500LL, s->next_try - s->now()));
            s->cv.wait_for(lk, std::chrono::milliseconds(wait), [&s] { return s->stop || s->kicked; });
            if (s->stop)
                break;
            s->kicked = false;
        }
        if (s->owner.expired())
            break;
        try {
            std::string sn, lost_why;
            bool        active, lost;
            long long   next_try;
            {
                std::lock_guard<std::mutex> lk(s->mtx);
                sn       = s->sn;
                active   = s->active;
                lost     = s->lost;
                lost_why = s->lost_why;
                next_try = s->next_try;
            }
            if (sn.empty())
                continue;
            if (active) {
                if (lost)
                    fall_back(s, lost_why);
                else
                    check_timeouts(s);
            } else if (s->now() >= next_try) {
                attempt(s, sn);
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "[U1WsRoute] " << e.what();
            note_failure(s, e.what());
        }
    }
    std::shared_ptr<MoonrakerWs> ws;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        ws        = std::move(s->ws);
        s->active = false;
        ++s->generation;
    }
    if (ws)
        ws->close();
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        s->finished = true;
    }
    s->cv.notify_all();
}

} // namespace u1ws

// ------------------------------------------------------------------ route ----

void U1WsRoute::reset_pacing()
{
    std::lock_guard<std::mutex> lk(u1ws::pace_mutex());
    u1ws::paces().clear();
}

std::shared_ptr<U1WsRoute> U1WsRoute::attach(const std::shared_ptr<MqttClient>& owner, U1WsDeps deps)
{
    static std::atomic<int>    count { 0 };
    std::shared_ptr<U1WsRoute> r(new U1WsRoute());
    r->m         = std::make_shared<State>();
    r->m->owner  = owner;
    r->m->deps   = std::move(deps);
    r->m->number = ++count;
    owner->SetRoute(r);
    WsStatePtr s = r->m;
    std::thread([s]() { u1ws::run(s); }).detach();
    return r;
}

U1WsRoute::~U1WsRoute() { owner_closing(); }

void U1WsRoute::owner_closing()
{
    if (!m)
        return;
    {
        std::lock_guard<std::mutex> lk(m->mtx);
        m->stop = true;
    }
    m->cv.notify_all();
}

bool U1WsRoute::stop_and_wait(int timeout_ms)
{
    owner_closing();
    std::unique_lock<std::mutex> lk(m->mtx);
    return m->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] { return m->finished; });
}

namespace u1ws {
static void learn(const WsStatePtr& s, const std::string& sn)
{
    if (sn.empty())
        return;
    bool learned = false;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        if (s->sn.empty()) {
            s->sn = sn;
            load_pace(*s);
            s->kicked = learned = true;
        }
    }
    if (learned)
        s->cv.notify_all();
}
} // namespace u1ws

// Subscriptions stay with the cloud session: it is the fallback, and its messages are filtered.
MqttRoute::Result U1WsRoute::subscribe(const std::string& topic, int, std::string&)
{
    u1ws::learn(m, U1LocalFirst::serial_of_topic(topic));
    return Result::NotMine;
}
MqttRoute::Result U1WsRoute::unsubscribe(const std::string&, std::string&) { return Result::NotMine; }
MqttRoute::Result U1WsRoute::is_subscribed(const std::string&) const { return Result::NotMine; }

MqttRoute::Result U1WsRoute::publish(const std::string& topic, const std::string& payload, int, std::string&)
{
    if (U1LocalFirst::device_topic_kind(topic) != U1LocalFirst::DeviceTopic::Request)
        return Result::NotMine;
    const std::string sn = U1LocalFirst::serial_of_topic(topic);
    u1ws::learn(m, sn);
    const json j = json::parse(payload, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return Result::NotMine;
    const std::string method = j.contains("method") && j["method"].is_string() ? j["method"].get<std::string>() : "";
    const std::string key    = U1LocalFirst::rpc_id_key(j);
    std::shared_ptr<MoonrakerWs> ws;
    {
        std::lock_guard<std::mutex> lk(m->mtx);
        if (method == "printer.objects.subscribe")
            m->last_subscribe = payload;
        if (m->stop || !m->active || sn != m->sn)
            return Result::NotMine;
        if (key.empty() || method.empty() || u1ws::is_cloud_method(sn, method)) {
            if (!key.empty())
                m->cloud_ids.insert(key);
            return Result::NotMine; // the cloud session's own publish
        }
        const bool  write     = !U1LocalFirst::is_idempotent_method(method);
        std::string signature;
        if (write) {
            signature = method + "|" + (j.contains("params") ? j["params"].dump() : std::string());
            for (auto& kv : m->pending)
                if (!kv.second.own && kv.second.signature == signature) {
                    // Still running on the printer: do not run it twice; answer this id too.
                    kv.second.extra_ids.push_back(j["id"].dump());
                    BOOST_LOG_TRIVIAL(info) << "[U1WsRoute] route #" << m->number << ": " << method
                                            << " sent again while it still runs; not repeated";
                    return Result::Ok;
                }
        }
        ws                      = m->ws;
        m->pending[key]         = { payload, method, m->now(), false };
        m->pending[key].signature = signature;
    }
    // EARLY-ACK HOOK (owner, 2026-10-15: not yet): a write the page times out on quickly (its
    // printer.gcode.script waits 3 s, a tool change takes longer) could be answered here with an
    // early "ok" for the page while the real answer is swallowed. Decide after checking whether
    // the cloud path times out on the same button.
    if (ws && ws->send(payload))
        return Result::Ok;
    {
        std::lock_guard<std::mutex> lk(m->mtx);
        m->pending.erase(key);
        m->cloud_ids.insert(key);
        m->lost     = true;
        m->lost_why = "the LAN connection would not take a request";
    }
    kick();
    return Result::NotMine;
}

bool U1WsRoute::accept_own_message(const std::string& topic, const std::string& payload)
{
    const U1LocalFirst::DeviceTopic kind = U1LocalFirst::device_topic_kind(topic);
    if (kind == U1LocalFirst::DeviceTopic::None)
        return true;
    json j;
    if (kind == U1LocalFirst::DeviceTopic::Response)
        j = json::parse(payload, nullptr, false);
    // The printer's own LAN addresses, from its machine.system_info answer over the cloud.
    if (kind == U1LocalFirst::DeviceTopic::Response && payload.find("ip_addresses") != std::string::npos && !j.is_discarded()) {
        const std::vector<std::string> ips = U1LocalFirst::lan_ipv4_from_system_info(j);
        bool                           fresh = false;
        {
            std::lock_guard<std::mutex> lk(m->mtx);
            for (const std::string& ip : ips)
                if (std::find(m->sniffed.begin(), m->sniffed.end(), ip) == m->sniffed.end()) {
                    m->sniffed.push_back(ip);
                    fresh = true;
                }
            if (fresh && !m->active && m->failures > 0) {
                m->next_try = 0;
                m->kicked   = true;
            }
        }
        if (fresh)
            m->cv.notify_all();
    }
    std::lock_guard<std::mutex> lk(m->mtx);
    if (m->stop || !m->active || U1LocalFirst::serial_of_topic(topic) != m->sn)
        return true;
    switch (kind) {
    case U1LocalFirst::DeviceTopic::Response: {
        // Only the answers to what this route sent through the cloud.
        const std::string key = j.is_discarded() ? std::string() : U1LocalFirst::rpc_id_key(j);
        return !key.empty() && m->cloud_ids.erase(key) > 0;
    }
    case U1LocalFirst::DeviceTopic::Status:
    case U1LocalFirst::DeviceTopic::Notification:
        return false; // the LAN carries them now
    default:
        return true;
    }
}

void U1WsRoute::set_device(const std::string& sn) { u1ws::learn(m, sn); }

bool U1WsRoute::on_lan() const
{
    std::lock_guard<std::mutex> lk(m->mtx);
    return m->active;
}

std::string U1WsRoute::serial() const
{
    std::lock_guard<std::mutex> lk(m->mtx);
    return m->sn;
}

void U1WsRoute::kick()
{
    {
        std::lock_guard<std::mutex> lk(m->mtx);
        m->kicked = true;
    }
    m->cv.notify_all();
}

std::vector<std::string> U1WsRoute::cloud_methods() const
{
    std::string sn;
    {
        std::lock_guard<std::mutex> lk(m->mtx);
        sn = m->sn;
    }
    std::lock_guard<std::mutex> lk(u1ws::pace_mutex());
    auto                        it = u1ws::paces().find(sn);
    if (it == u1ws::paces().end())
        return {};
    return std::vector<std::string>(it->second.cloud_methods.begin(), it->second.cloud_methods.end());
}

// ---------------------------------------------------------------- the app ----

U1WsDeps make_app_u1_ws_deps()
{
    U1WsDeps d;
    d.known_addresses = [](const std::string& sn) {
        SnapmakerLan::start_discovery(); // at most one mDNS pass a minute
        std::vector<std::string> out;
        for (const SnapmakerLan::Device& dev : SnapmakerLan::devices())
            if (dev.id == sn && SnapmakerLan::is_local_address(dev.ip))
                out.push_back(SnapmakerLan::host_of(dev.ip));
        return out;
    };
    d.is_that_printer = [](const std::string& ip, const std::string& sn) {
        SnapmakerLan::Device found;
        std::string          error;
        return SnapmakerLan::identify_at(ip, 80, found, error) && found.id == sn;
    };
    return d;
}

} // namespace GUI
} // namespace Slic3r
