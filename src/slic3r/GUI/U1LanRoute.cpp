#include "U1LanRoute.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "RemoteAccess.hpp" // post_to_main
#include "RemoteHub.hpp"    // hub_dir
#include "SnapmakerLan.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/InstanceLock.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/beast/core/detail/base64.hpp>
#include <boost/format.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/filesystem.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#endif

namespace Slic3r {
namespace GUI {

using nlohmann::json;
using U1LocalFirst::Via;
namespace fs = boost::filesystem;

static long long steady_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Drop a strong reference without ever letting the last one go on a Paho callback thread or the
// worker (a client destroyed there would wait for its own threads).
static void release_elsewhere(std::shared_ptr<MqttClient>&& c)
{
    if (!c)
        return;
    if (c.use_count() == 1) {
        std::thread([doomed = std::move(c)]() mutable { doomed.reset(); }).detach();
        return;
    }
    c.reset();
}

// ------------------------------------------------------------------ state ----

struct U1LanRoute::State
{
    std::weak_ptr<MqttClient> owner;
    U1LanDeps                 deps;

    mutable std::mutex      mtx;
    std::condition_variable cv;
    bool                    stop { false };
    bool                    kicked { false };
    bool                    finished { false };

    std::string              sn;
    std::string              code { "12345678" }; // the U1's default pairing access code
    std::vector<std::string> sniffed;            // addresses the printer reported over the cloud

    U1LocalFirst::RouteTable    table;
    std::set<std::string>       acked; // device subscriptions the serving broker acknowledged
    std::shared_ptr<MqttClient> lan;
    std::string                 lan_ip;

    int       failures { 0 };
    long long next_try { 0 };
    bool      asked_this_session { false };
    bool      no_slot { false };
    std::string last_reason;
    long long down_since { -1 };
    int       probe_fails { 0 };
    long long next_probe { 0 };

    long long now() const { return deps.now_ms ? deps.now_ms() : steady_now_ms(); }

    // Interruptible sleep; false when the route is stopping.
    bool sleep(long long ms)
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return stop; });
        return !stop;
    }
    bool stopping() const
    {
        std::lock_guard<std::mutex> lk(mtx);
        return stop;
    }
};

using StatePtr = std::shared_ptr<U1LanRoute::State>;

static void note_failure(const StatePtr& s, const std::string& why)
{
    std::lock_guard<std::mutex> lk(s->mtx);
    ++s->failures;
    const long long wait = U1LocalFirst::retry_after_ms(s->deps.timing, s->failures);
    s->next_try          = s->now() + wait;
    if (why != s->last_reason) // one line per new reason, not one per retry
        BOOST_LOG_TRIVIAL(info) << "[U1LanRoute] " << s->sn << " stays on the cloud: " << why << " (next LAN try in " << wait / 1000 << " s)";
    s->last_reason = why;
}

// The LAN session's messages reach the page as the cloud client's own.
static void wire_lan_messages(const StatePtr& s, const std::shared_ptr<MqttClient>& lan)
{
    std::weak_ptr<U1LanRoute::State> weak = s;
    lan->SetMessageCallback([weak](const std::string& topic, const std::string& payload) {
        StatePtr st = weak.lock();
        if (!st)
            return;
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            if (st->stop || !st->table.accept(Via::Lan, topic))
                return;
        }
        std::shared_ptr<MqttClient> o = st->owner.lock();
        if (o)
            o->Deliver(topic, payload);
        release_elsewhere(std::move(o));
    });
}

static void close_client(std::shared_ptr<MqttClient> c)
{
    if (!c)
        return;
    c->SetMessageCallback(nullptr);
    std::string ignore;
    try {
        c->Disconnect(ignore);
    } catch (const std::exception&) {}
}

// Paho throws (MQTT error -1, TCP/TLS connect failure) for a host that refuses the connection.
static bool safe_connect(const std::shared_ptr<MqttClient>& c, std::string& msg)
{
    try {
        return c->Connect(msg);
    } catch (const std::exception& e) {
        msg = e.what();
        return false;
    }
}

// Move the device topics onto `lan`. False when the LAN broker refused a subscription.
static bool switch_to_lan(const StatePtr& s, const std::shared_ptr<MqttClient>& lan, const std::string& ip)
{
    std::set<std::string> done;
    bool                  switched = false;
    for (int round = 0; round < 5 && !switched; ++round) {
        std::map<std::string, int> wanted;
        {
            std::lock_guard<std::mutex> lk(s->mtx);
            wanted = s->table.wanted();
        }
        for (const auto& w : wanted) {
            if (done.count(w.first))
                continue;
            std::string msg;
            if (!lan->SubscribeDirect(w.first, w.second, msg)) {
                BOOST_LOG_TRIVIAL(warning) << "[U1LanRoute] the printer's broker refused " << w.first << ": " << msg;
                return false;
            }
            done.insert(w.first);
        }
        std::lock_guard<std::mutex> lk(s->mtx);
        bool all = true;
        for (const auto& w : s->table.wanted())
            all = all && done.count(w.first) > 0;
        if (all) {
            s->table.set_via(Via::Lan);
            s->lan         = lan;
            s->lan_ip      = ip;
            s->down_since  = -1;
            s->probe_fails = 0;
            s->next_probe  = s->now() + s->deps.timing.lan_probe_interval_ms;
            s->failures    = 0;
            s->last_reason.clear();
            for (const auto& t : done)
                s->acked.insert(t);
            switched = true;
        }
    }
    if (!switched)
        return false;
    // The cloud session no longer needs them (its messages on them are dropped meanwhile).
    std::shared_ptr<MqttClient> o = s->owner.lock();
    if (o)
        for (const auto& t : done) {
            std::string msg;
            o->UnsubscribeDirect(t, msg);
        }
    release_elsewhere(std::move(o));
    BOOST_LOG_TRIVIAL(info) << "[U1LanRoute] " << s->sn << " on LAN (" << ip << "), " << done.size() << " topics moved off the cloud";
    if (s->deps.route_changed)
        s->deps.route_changed(s->sn, true);
    return true;
}

static void fall_back(const StatePtr& s, const std::string& why)
{
    std::map<std::string, int>  wanted;
    std::shared_ptr<MqttClient> lan;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        wanted = s->table.wanted();
        lan    = s->lan;
    }
    // Back onto the cloud session first; until the route flips its device messages are dropped,
    // so nothing arrives twice.
    std::shared_ptr<MqttClient> o = s->owner.lock();
    std::set<std::string>       acked;
    if (o)
        for (const auto& w : wanted) {
            std::string msg;
            if (o->SubscribeDirect(w.first, w.second, msg))
                acked.insert(w.first);
            else
                o->KeepSubscription(w.first, w.second); // subscribed again when that session is back
        }
    release_elsewhere(std::move(o));
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        s->table.set_via(Via::Cloud);
        s->acked = acked;
        s->lan.reset();
        s->lan_ip.clear();
        s->down_since  = -1;
        s->probe_fails = 0;
        s->failures    = 1;
        s->next_try    = s->now() + U1LocalFirst::retry_after_ms(s->deps.timing, 1);
        s->last_reason = why;
    }
    BOOST_LOG_TRIVIAL(warning) << "[U1LanRoute] " << s->sn << " falling back to the cloud: " << why;
    close_client(lan);
    if (s->deps.route_changed)
        s->deps.route_changed(s->sn, false);
}

static json pairing(const StatePtr& s, const std::string& ip, const std::string& method, const json& params)
{
    std::string code;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        code = s->code;
    }
    try {
        return s->deps.pairing ? s->deps.pairing(ip, code, method, params) : json(nullptr);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "[U1LanRoute] pairing exchange failed: " << e.what();
        return nullptr;
    }
}

static void attempt(const StatePtr& s, const std::string& sn)
{
    const U1LanDeps& d = s->deps;

    // ---- where is it ----
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
        if (s->stopping())
            return;
        if (!d.is_that_printer || d.is_that_printer(a, sn)) {
            ip = a;
            break;
        }
    }
    if (ip.empty())
        return note_failure(s, "the printer does not answer on this network");

    // ---- may this PC use it ----
    const std::string client_id = d.client_id ? d.client_id() : std::string();
    if (client_id.empty()) {
        std::lock_guard<std::mutex> lk(s->mtx);
        if (!s->no_slot)
            BOOST_LOG_TRIVIAL(warning) << "[U1LanRoute] no free LAN client slot on this PC; staying on the cloud";
        s->no_slot  = true;
        s->next_try = s->now() + 3600 * 1000LL;
        return;
    }
    json                       params = { { "clientid", client_id } };
    json                       answer = pairing(s, ip, "server.client_manager.confirm_lan_status", params);
    U1LocalFirst::AuthAnswer   check  = U1LocalFirst::parse_auth_answer(answer);
    U1LocalFirst::AskState     ask    = d.ask_state ? d.ask_state(sn) : U1LocalFirst::AskState();
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        ask.asked_this_session = s->asked_this_session;
    }
    const U1LocalFirst::AuthStep step = U1LocalFirst::next_auth_step(d.timing, check, ask);
    if (step == U1LocalFirst::AuthStep::StayCloud) {
        if (check == U1LocalFirst::AuthAnswer::NoAnswer)
            return note_failure(s, "the printer's pairing port did not answer");
        if (check == U1LocalFirst::AuthAnswer::NotAvailable)
            return note_failure(s, "the printer does not offer a LAN connection");
        if (ask.authorized && d.set_authorized)
            d.set_authorized(sn, false); // the printer forgot this PC
        return note_failure(s, "this PC is not approved on the printer");
    }
    if (step == U1LocalFirst::AuthStep::AskApproval) {
        {
            std::lock_guard<std::mutex> lk(s->mtx);
            s->asked_this_session = true;
        }
        static std::atomic<unsigned> seq { 0 };
        const json req = { { "clientid", client_id },
                           { "app_id", "orca-" + std::to_string(steady_now_ms()) + std::to_string(++seq) } };
        json       r   = pairing(s, ip, "server.client_manager.request_lan_auth", req);
        const auto a   = U1LocalFirst::parse_auth_answer(r);
        if (a == U1LocalFirst::AuthAnswer::Success) {
            answer = r;
        } else if (a == U1LocalFirst::AuthAnswer::Authorizing || a == U1LocalFirst::AuthAnswer::Unauthorized) {
            BOOST_LOG_TRIVIAL(info) << "[U1LanRoute] asked " << sn << " to approve this PC on its screen";
            if (d.approval_requested)
                d.approval_requested(sn);
            const long long until = s->now() + d.timing.approval_wait_ms;
            answer                = nullptr;
            while (s->now() < until) {
                if (!s->sleep(d.timing.approval_poll_ms))
                    return;
                json c = pairing(s, ip, "server.client_manager.confirm_lan_status", params);
                if (U1LocalFirst::parse_auth_answer(c) == U1LocalFirst::AuthAnswer::Success) {
                    answer = c;
                    break;
                }
            }
            if (answer.is_null()) {
                if (d.count_unanswered_ask)
                    d.count_unanswered_ask(sn);
                return note_failure(s, "the printer's approval prompt was not answered");
            }
        } else {
            return note_failure(s, "the printer did not take the approval request");
        }
    }
    if (d.set_authorized)
        d.set_authorized(sn, true);

    // ---- connect ----
    U1LanCreds creds;
    creds.ca        = answer.value("ca", "");
    creds.cert      = answer.value("cert", "");
    creds.key       = answer.value("key", "");
    creds.port      = answer.contains("port") && answer["port"].is_number_integer() ? answer["port"].get<int>() : 8883;
    creds.client_id = answer.contains("clientid") && answer["clientid"].is_string() && !answer["clientid"].get<std::string>().empty() ?
                          answer["clientid"].get<std::string>() :
                          client_id;
    if (s->stopping())
        return;
    std::shared_ptr<MqttClient> lan;
    try {
        lan = d.make_lan_client ? d.make_lan_client(ip, creds) : nullptr;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "[U1LanRoute] could not build the LAN session: " << e.what();
    }
    if (!lan)
        return note_failure(s, "could not build the LAN session");
    wire_lan_messages(s, lan);
    std::string msg;
    if (!safe_connect(lan, msg)) {
        close_client(lan);
        return note_failure(s, "the printer's broker refused the connection (" + msg + ")");
    }
    if (s->stopping() || !switch_to_lan(s, lan, ip)) {
        close_client(lan);
        if (!s->stopping())
            note_failure(s, "the printer's broker refused the subscriptions");
        return;
    }
}

static void check_lan_health(const StatePtr& s)
{
    std::shared_ptr<MqttClient> lan;
    std::string                 ip;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        lan = s->lan;
        ip  = s->lan_ip;
    }
    const long long         now = s->now();
    U1LocalFirst::LanHealth h;
    h.mqtt_connected = lan && lan->CheckConnected();
    bool probe_now   = false;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        if (!h.mqtt_connected) {
            if (s->down_since < 0)
                s->down_since = now;
            h.down_for_ms = now - s->down_since;
        } else {
            s->down_since = -1;
        }
        probe_now = now >= s->next_probe;
    }
    if (probe_now) {
        const bool ok = !s->deps.probe || s->deps.probe(ip);
        std::lock_guard<std::mutex> lk(s->mtx);
        s->probe_fails = ok ? 0 : s->probe_fails + 1;
        s->next_probe  = s->now() + s->deps.timing.lan_probe_interval_ms;
    }
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        h.probe_fails = s->probe_fails;
    }
    if (U1LocalFirst::should_fall_back(s->deps.timing, h))
        fall_back(s, h.mqtt_connected ? "the printer stopped answering on the LAN" : "the LAN session dropped");
}

static void run(StatePtr s)
{
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(s->mtx);
            if (s->stop)
                break;
            long long wait = 1000;
            if (s->table.via() == Via::Cloud && !s->sn.empty())
                wait = (std::max)(20LL, (std::min)(1000LL, s->next_try - s->now()));
            s->cv.wait_for(lk, std::chrono::milliseconds(wait), [&s] { return s->stop || s->kicked; });
            if (s->stop)
                break;
            s->kicked = false;
        }
        if (s->owner.expired())
            break;
        try {
            std::string sn;
            Via         via;
            long long   next_try;
            {
                std::lock_guard<std::mutex> lk(s->mtx);
                sn       = s->sn;
                via      = s->table.via();
                next_try = s->next_try;
            }
            if (sn.empty())
                continue;
            if (via == Via::Lan)
                check_lan_health(s);
            else if (s->now() >= next_try)
                attempt(s, sn);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "[U1LanRoute] " << e.what();
            note_failure(s, e.what());
        }
    }
    std::shared_ptr<MqttClient> lan;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        lan = std::move(s->lan);
        s->table.set_via(Via::Cloud);
    }
    close_client(lan);
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        s->finished = true;
    }
    s->cv.notify_all();
}

// ------------------------------------------------------------------ route ----

std::shared_ptr<U1LanRoute> U1LanRoute::attach(const std::shared_ptr<MqttClient>& owner, U1LanDeps deps)
{
    std::shared_ptr<U1LanRoute> r(new U1LanRoute());
    r->m        = std::make_shared<State>();
    r->m->owner = owner;
    r->m->deps  = std::move(deps);
    owner->SetRoute(r);
    StatePtr s = r->m;
    std::thread([s]() { run(s); }).detach();
    return r;
}

U1LanRoute::~U1LanRoute() { owner_closing(); }

void U1LanRoute::owner_closing()
{
    if (!m)
        return;
    {
        std::lock_guard<std::mutex> lk(m->mtx);
        m->stop = true;
    }
    m->cv.notify_all();
}

bool U1LanRoute::stop_and_wait(int timeout_ms)
{
    owner_closing();
    std::unique_lock<std::mutex> lk(m->mtx);
    return m->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] { return m->finished; });
}

static void learn_serial(const StatePtr& s, const std::string& topic)
{
    const std::string sn = U1LocalFirst::serial_of_topic(topic);
    if (sn.empty())
        return;
    bool learned = false;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        if (s->sn.empty()) {
            s->sn    = sn;
            s->kicked = learned = true;
        }
    }
    if (learned)
        s->cv.notify_all();
}

MqttRoute::Result U1LanRoute::subscribe(const std::string& topic, int qos, std::string& msg)
{
    if (!U1LocalFirst::is_device_topic(topic))
        return Result::NotMine;
    learn_serial(m, topic);
    std::shared_ptr<MqttClient> lan;
    {
        std::lock_guard<std::mutex> lk(m->mtx);
        if (m->stop)
            return Result::NotMine;
        m->table.want(topic, qos);
        if (m->table.via() == Via::Lan)
            lan = m->lan;
    }
    bool ok = false;
    if (lan) {
        ok = lan->SubscribeDirect(topic, qos, msg);
    } else {
        std::shared_ptr<MqttClient> o = m->owner.lock();
        ok                            = o && o->SubscribeDirect(topic, qos, msg);
        release_elsewhere(std::move(o));
    }
    if (ok) {
        std::lock_guard<std::mutex> lk(m->mtx);
        m->acked.insert(topic);
    }
    return ok ? Result::Ok : Result::Failed;
}

MqttRoute::Result U1LanRoute::unsubscribe(const std::string& topic, std::string& msg)
{
    if (!U1LocalFirst::is_device_topic(topic))
        return Result::NotMine;
    std::shared_ptr<MqttClient> lan;
    {
        std::lock_guard<std::mutex> lk(m->mtx);
        if (m->stop)
            return Result::NotMine;
        m->table.unwant(topic);
        m->acked.erase(topic);
        if (m->table.via() == Via::Lan)
            lan = m->lan;
    }
    bool ok = false;
    if (lan) {
        ok = lan->UnsubscribeDirect(topic, msg);
    } else {
        std::shared_ptr<MqttClient> o = m->owner.lock();
        ok                            = o && o->UnsubscribeDirect(topic, msg);
        release_elsewhere(std::move(o));
    }
    return ok ? Result::Ok : Result::Failed;
}

MqttRoute::Result U1LanRoute::publish(const std::string& topic, const std::string& payload, int qos, std::string& msg)
{
    if (!U1LocalFirst::is_device_topic(topic))
        return Result::NotMine;
    learn_serial(m, topic);
    std::shared_ptr<MqttClient> lan;
    {
        std::lock_guard<std::mutex> lk(m->mtx);
        if (m->stop)
            return Result::NotMine;
        if (m->table.via() == Via::Lan)
            lan = m->lan;
    }
    if (lan) {
        if (lan->PublishDirect(topic, payload, qos, msg))
            return Result::Ok;
        kick(); // the health check decides whether to fall back
        return Result::Failed;
    }
    return Result::NotMine; // the cloud session's own publish
}

MqttRoute::Result U1LanRoute::is_subscribed(const std::string& topic) const
{
    if (!U1LocalFirst::is_device_topic(topic))
        return Result::NotMine;
    std::lock_guard<std::mutex> lk(m->mtx);
    if (m->stop)
        return Result::NotMine;
    return m->acked.count(topic) ? Result::Ok : Result::Failed;
}

bool U1LanRoute::accept_own_message(const std::string& topic, const std::string& payload)
{
    // The cloud carries the printer's machine.system_info answer (our host asks for it on every
    // connect): its network section says where the printer is on the LAN.
    if (U1LocalFirst::device_topic_kind(topic) == U1LocalFirst::DeviceTopic::Response &&
        payload.find("ip_addresses") != std::string::npos) {
        const json j = json::parse(payload, nullptr, false);
        if (!j.is_discarded()) {
            const std::vector<std::string> ips = U1LocalFirst::lan_ipv4_from_system_info(j);
            if (!ips.empty()) {
                bool fresh = false;
                {
                    std::lock_guard<std::mutex> lk(m->mtx);
                    for (const std::string& ip : ips)
                        if (std::find(m->sniffed.begin(), m->sniffed.end(), ip) == m->sniffed.end()) {
                            m->sniffed.push_back(ip);
                            fresh = true;
                        }
                    if (fresh && m->table.via() == Via::Cloud && m->failures > 0) {
                        m->next_try = 0; // a new address: try it now
                        m->kicked   = true;
                    }
                }
                if (fresh)
                    m->cv.notify_all();
            }
        }
    }
    std::lock_guard<std::mutex> lk(m->mtx);
    if (m->stop)
        return true;
    return m->table.accept(Via::Cloud, topic);
}

void U1LanRoute::set_device(const std::string& sn, const std::string& access_code)
{
    {
        std::lock_guard<std::mutex> lk(m->mtx);
        if (!sn.empty() && m->sn.empty()) {
            m->sn     = sn;
            m->kicked = true;
        }
        if (!access_code.empty())
            m->code = access_code;
    }
    m->cv.notify_all();
}

bool U1LanRoute::on_lan() const
{
    std::lock_guard<std::mutex> lk(m->mtx);
    return m->table.via() == Via::Lan;
}

std::string U1LanRoute::serial() const
{
    std::lock_guard<std::mutex> lk(m->mtx);
    return m->sn;
}

std::string U1LanRoute::lan_address() const
{
    std::lock_guard<std::mutex> lk(m->mtx);
    return m->lan_ip;
}

void U1LanRoute::kick()
{
    {
        std::lock_guard<std::mutex> lk(m->mtx);
        m->kicked = true;
        if (m->table.via() == Via::Cloud)
            m->next_try = 0;
    }
    m->cv.notify_all();
}

// --------------------------------------------------------- pairing port ----

json u1_pairing_exchange(const std::string& host, int port, const std::string& code, const std::string& method, const json& params,
                         int timeout_ms)
{
    static std::atomic<unsigned> seq { 0 };
    std::mt19937_64              rng((unsigned long long) steady_now_ms() ^ ((unsigned long long) ++seq << 32));
    const long long              id = (long long) (rng() & 0x3FFFFFFFFFFFLL); // fits a JSON double exactly

    std::shared_ptr<MqttClient> client;
    try {
        client = MqttClient::create("mqtt://" + host + ":" + std::to_string(port), "orca-try-" + std::to_string(rng() % 1000000000ULL));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "[U1LanRoute] pairing client: " << e.what();
        return nullptr;
    }
    struct Box
    {
        std::mutex              m;
        std::condition_variable cv;
        bool                    done { false };
        json                    result;
    };
    auto box = std::make_shared<Box>();
    const std::string clientid = params.is_object() ? params.value("clientid", "") : "";
    client->SetMessageCallback([box, id, clientid](const std::string&, const std::string& payload) {
        const json j = json::parse(payload, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("result"))
            return;
        const bool same_id     = j.contains("id") && j["id"].is_number() && j["id"].get<long long>() == id;
        const bool same_client = !j.contains("id") && j["result"].is_object() && !clientid.empty() &&
                                 j["result"].value("clientid", "") == clientid;
        if (!same_id && !same_client)
            return;
        std::lock_guard<std::mutex> lk(box->m);
        if (box->done)
            return;
        box->result = j["result"];
        box->done   = true;
        box->cv.notify_all();
    });
    std::string msg;
    json        out = nullptr;
    if (safe_connect(client, msg) && client->SubscribeDirect(code + "/config/response", 1, msg)) {
        const json body = { { "jsonrpc", "2.0" }, { "method", method }, { "params", params }, { "id", id } };
        if (client->PublishDirect(code + "/config/request", body.dump(), 1, msg)) {
            std::unique_lock<std::mutex> lk(box->m);
            if (box->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&box] { return box->done; }))
                out = box->result;
        }
    }
    close_client(client);
    return out;
}

// ------------------------------------------------------------ slot store ----

#ifdef _WIN32
static std::string b64_encode(const std::string& in)
{
    std::string out(boost::beast::detail::base64::encoded_size(in.size()), '\0');
    out.resize(boost::beast::detail::base64::encode(&out[0], in.data(), in.size()));
    return out;
}
static std::string b64_decode(const std::string& in)
{
    std::string out(boost::beast::detail::base64::decoded_size(in.size()), '\0');
    const auto  r = boost::beast::detail::base64::decode(&out[0], in.data(), in.size());
    out.resize(r.first);
    return out;
}
#endif

U1LocalFirst::Protector U1LanSlotStore::default_protector()
{
    U1LocalFirst::Protector p;
#ifdef _WIN32
    p.protect = [](const std::string& plain) -> std::string {
        DATA_BLOB in { (DWORD) plain.size(), (BYTE*) plain.data() }, out {};
        if (!CryptProtectData(&in, L"EdgeSlicer U1 LAN client id", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
            return plain; // kept readable rather than lost
        std::string blob((const char*) out.pbData, out.cbData);
        LocalFree(out.pbData);
        return "dpapi:" + b64_encode(blob);
    };
    p.unprotect = [](const std::string& stored) -> std::string {
        if (stored.rfind("dpapi:", 0) != 0)
            return stored;
        const std::string blob = b64_decode(stored.substr(6));
        DATA_BLOB         in { (DWORD) blob.size(), (BYTE*) blob.data() }, out {};
        if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
            return {};
        std::string plain((const char*) out.pbData, out.cbData);
        LocalFree(out.pbData);
        return plain;
    };
#endif
    return p;
}

// One process's claim on a slot. The in-process set stands in for POSIX fcntl locks, which a
// process never conflicts with itself on.
struct U1LanSlotStore::Lock
{
    std::string                                       path;
    std::unique_ptr<boost::interprocess::file_lock>   lock;
    static std::mutex&                                registry_mutex()
    {
        static std::mutex m;
        return m;
    }
    static std::set<std::string>& registry()
    {
        static std::set<std::string> r;
        return r;
    }
    ~Lock()
    {
        if (lock) {
            try {
                lock->unlock();
            } catch (...) {}
        }
        std::lock_guard<std::mutex> lk(registry_mutex());
        registry().erase(path);
    }
};

U1LanSlotStore::U1LanSlotStore(std::string dir, U1LocalFirst::Protector protector) : m_dir(std::move(dir)), m_protector(std::move(protector)) {}
U1LanSlotStore::~U1LanSlotStore() = default;

int U1LanSlotStore::slot()
{
    if (m_slot != -2)
        return m_slot;
    boost::system::error_code ec;
    const fs::path            lock_dir = fs::path(m_dir) / "u1_lan";
    fs::create_directories(lock_dir, ec);
    m_slot = U1LocalFirst::pick_slot([this, &lock_dir](int n) {
        const std::string path = (lock_dir / ("slot" + std::to_string(n) + ".lock")).string();
        {
            std::lock_guard<std::mutex> lk(Lock::registry_mutex());
            if (Lock::registry().count(path))
                return false;
        }
        try {
            if (!fs::exists(path)) {
                boost::nowide::ofstream touch(path, std::ios::binary | std::ios::app);
            }
#ifdef _WIN32
            auto fl = std::make_unique<boost::interprocess::file_lock>(boost::nowide::widen(path).c_str());
#else
            auto fl = std::make_unique<boost::interprocess::file_lock>(path.c_str());
#endif
            if (!fl->try_lock())
                return false;
            auto l  = std::make_unique<Lock>();
            l->path = path;
            l->lock = std::move(fl);
            {
                std::lock_guard<std::mutex> lk(Lock::registry_mutex());
                Lock::registry().insert(path);
            }
            m_lock = std::move(l);
            return true;
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "[U1LanRoute] slot lock " << n << ": " << e.what();
            return false;
        }
    });
    if (m_slot >= 0)
        BOOST_LOG_TRIVIAL(info) << "[U1LanRoute] this process uses LAN client slot " << m_slot;
    return m_slot;
}

U1LocalFirst::SlotStore U1LanSlotStore::load()
{
    try {
        boost::nowide::ifstream in((fs::path(m_dir) / "u1_lan_auth.json").string());
        if (in.good()) {
            json j;
            in >> j;
            return U1LocalFirst::SlotStore::from_json(j);
        }
    } catch (...) {}
    return U1LocalFirst::SlotStore::from_json(json::object());
}

template<class Fn> void U1LanSlotStore::update(Fn&& fn)
{
    const int n = slot();
    if (n < 0)
        return;
    InstanceLock            guard((fs::path(m_dir) / "u1_lan_auth.lock").string());
    U1LocalFirst::SlotStore st   = load();
    json                    disk = st.to_json();
    fn(st, n);
    st.merge_into(disk, n);
    const std::string path = (fs::path(m_dir) / "u1_lan_auth.json").string();
    try {
        boost::system::error_code ec;
        fs::create_directories(m_dir, ec);
        boost::nowide::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << disk.dump(1);
        out.close();
#ifndef _WIN32
        fs::permissions(path, fs::owner_read | fs::owner_write, ec);
#endif
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "[U1LanRoute] could not write the slot store: " << e.what();
    }
}

std::string U1LanSlotStore::client_id()
{
    std::string id;
    update([&](U1LocalFirst::SlotStore& st, int n) { id = st.client_id(n, m_protector); });
    return id;
}

U1LocalFirst::AskState U1LanSlotStore::ask_state(const std::string& sn)
{
    const int n = slot();
    if (n < 0)
        return {};
    InstanceLock guard((fs::path(m_dir) / "u1_lan_auth.lock").string());
    return load().ask_state(n, sn);
}

void U1LanSlotStore::set_authorized(const std::string& sn, bool authorized)
{
    update([&](U1LocalFirst::SlotStore& st, int n) { st.set_authorized(n, sn, authorized); });
}

void U1LanSlotStore::count_unanswered_ask(const std::string& sn)
{
    update([&](U1LocalFirst::SlotStore& st, int n) { st.count_unanswered_ask(n, sn); });
}

U1LanSlotStore& U1LanSlotStore::app()
{
    static U1LanSlotStore* store = new U1LanSlotStore(RemoteHub::hub_dir()); // leaked: the lock lives as long as the process
    return *store;
}

// ---------------------------------------------------------------- the app ----

bool u1_local_first_enabled()
{
    return !(wxGetApp().app_config && wxGetApp().app_config->get("snapmaker_local_first") == "0");
}

std::shared_ptr<U1LanRoute> u1_lan_route_of(const std::shared_ptr<MqttClient>& engine)
{
    return engine ? std::dynamic_pointer_cast<U1LanRoute>(engine->GetRoute()) : nullptr;
}

void attach_u1_lan_route_if_cloud(const std::shared_ptr<MqttClient>& engine, const std::string& server_address)
{
    if (!engine || !SnapmakerLan::is_cloud_host(server_address) || !u1_local_first_enabled())
        return;
    U1LanRoute::attach(engine, make_app_u1_lan_deps());
    BOOST_LOG_TRIVIAL(info) << "[U1LanRoute] local-first route attached to a cloud session";
}

static bool http_answers(const std::string& ip)
{
    bool ok = false;
    Http::get("http://" + ip + "/server/info")
        .timeout_connect(3)
        .timeout_max(4)
        .on_complete([&ok](std::string, unsigned status) { ok = status >= 200 && status < 500; })
        .on_error([&ok](std::string, std::string, unsigned status) { ok = status >= 200 && status < 500; })
        .perform_sync();
    return ok;
}

U1LanDeps make_app_u1_lan_deps()
{
    U1LanDeps d;
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
    d.probe   = [](const std::string& ip) { return http_answers(ip); };
    d.pairing = [](const std::string& ip, const std::string& code, const std::string& method, const json& params) {
        return u1_pairing_exchange(ip, 1884, code, method, params, 8000);
    };
    d.make_lan_client = [](const std::string& ip, const U1LanCreds& c) {
        return MqttClient::create("mqtts://" + ip + ":" + std::to_string(c.port), c.client_id, c.ca, c.cert, c.key);
    };
    d.client_id            = []() { return U1LanSlotStore::app().client_id(); };
    d.ask_state            = [](const std::string& sn) { return U1LanSlotStore::app().ask_state(sn); };
    d.set_authorized       = [](const std::string& sn, bool a) { U1LanSlotStore::app().set_authorized(sn, a); };
    d.count_unanswered_ask = [](const std::string& sn) { U1LanSlotStore::app().count_unanswered_ask(sn); };
    d.approval_requested   = [](const std::string& sn) {
        std::string name;
        SnapmakerLan::Device dev;
        if (SnapmakerLan::find(sn, dev))
            name = dev.name;
        RemoteAccess::post_to_main([name]() {
            if (!wxGetApp().plater())
                return;
            const std::string text = name.empty() ?
                                         _u8L("Your Snapmaker printer is asking to allow EdgeSlicer on its screen. Allow it once and this PC "
                                              "connects to it over your network instead of the Snapmaker cloud.") :
                                         (boost::format(_u8L("%1% is asking to allow EdgeSlicer on its screen. Allow it once and this PC "
                                                             "connects to it over your network instead of the Snapmaker cloud.")) %
                                          name)
                                             .str();
            wxGetApp().plater()->get_notification_manager()->push_notification(text);
        });
    };
    return d;
}

} // namespace GUI
} // namespace Slic3r
