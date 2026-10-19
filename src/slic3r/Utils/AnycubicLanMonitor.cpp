#include "AnycubicLanMonitor.hpp"

#include "MQTT.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {
namespace AnycubicLanMonitor {

namespace {

constexpr int KEEP_MS = 60000;

struct Conn
{
    std::string                           host;
    AnycubicLan::Snapshot                 snap;
    std::shared_ptr<MqttClient>           mqtt;
    AnycubicLan::Credentials              creds;
    std::chrono::steady_clock::time_point keep_until { std::chrono::steady_clock::now() };
};

std::mutex                                         g_mutex;
std::condition_variable                            g_cv;
std::unordered_map<std::string, std::shared_ptr<Conn>> g_conns;
std::vector<std::thread>                           g_workers;
bool                                               g_stop { false };

std::string key_of(const std::string &host) { return AnycubicLan::bare_host(host); }

void disconnect_clients(std::vector<std::shared_ptr<MqttClient>> &clients)
{
    for (auto &m : clients) {
        if (!m) continue;
        std::string msg;
        m->Disconnect(msg);
    }
    clients.clear();
}

void join_workers(std::vector<std::thread> &workers)
{
    for (auto &t : workers)
        if (t.joinable()) t.join();
    workers.clear();
}

std::shared_ptr<Conn> get_or_make(const std::string &host)
{
    const std::string k  = key_of(host);
    auto              it = g_conns.find(k);
    if (it != g_conns.end()) return it->second;
    auto c         = std::make_shared<Conn>();
    c->host        = k;
    c->snap.exists = true;
    g_conns[k]     = c;
    return c;
}

void launch_open(const std::shared_ptr<Conn> &c);

bool open_conn(const std::shared_ptr<Conn> &c)
{
    if (!c) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_stop) {
            c->snap.connecting = false;
            g_cv.notify_all();
            return false;
        }
        c->snap.connecting = true;
        c->snap.error.clear();
    }

    AnycubicLan::Info        info;
    AnycubicLan::Credentials creds;
    std::string              error;
    if (!AnycubicLan::handshake(c->host, info, creds, error)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        c->snap.connecting = false;
        c->snap.error      = error;
        c->snap.failures++;
        g_cv.notify_all();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_stop) {
            c->snap.connecting = false;
            g_cv.notify_all();
            return false;
        }
    }

    const std::string cn       = creds.device_id.empty() ? info.cn : creds.device_id;
    const std::string model_id = creds.model_id.empty() ? info.model_id : creds.model_id;
    const std::string broker   = creds.broker_host.empty() ? AnycubicLan::bare_host(c->host) : creds.broker_host;
    const std::string uri      = AnycubicLan::mqtt_broker_uri(broker, creds.broker_port);
    try {
        auto        mqtt = MqttClient::create(uri, "edgeslicer-mon-" + AnycubicLan::make_msgid(), creds.cert, creds.cert, creds.key,
                                              creds.username, creds.password, true);
        std::string msg;
        if (!mqtt->Connect(msg)) {
            std::lock_guard<std::mutex> lock(g_mutex);
            c->snap.connecting = false;
            c->snap.error      = msg.empty() ? "MQTT connect failed" : msg;
            c->snap.failures++;
            g_cv.notify_all();
            return false;
        }
        mqtt->SetMessageCallback([wp = std::weak_ptr<Conn>(c)](const std::string &topic, const std::string &payload) {
            auto p = wp.lock();
            if (!p) return;
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_stop) return;
            AnycubicLan::parse_report(topic, payload, p->snap.status);
        });
        mqtt->Subscribe(AnycubicLan::mqtt_public_filter(model_id, cn), 0, msg);
        const auto q = AnycubicLan::command_envelope(AnycubicLan::make_msgid(), cn, model_id, "info", "query", nlohmann::json::object());
        mqtt->Publish(AnycubicLan::mqtt_slicer_topic(model_id, cn, "info"), q.dump(), 0, msg);

        std::shared_ptr<MqttClient> leftover;
        bool                        ok = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_stop) {
                leftover           = mqtt;
                c->snap.connecting = false;
            } else {
                leftover           = std::move(c->mqtt);
                c->snap.info       = info;
                c->creds           = creds;
                c->mqtt            = mqtt;
                c->snap.connected  = true;
                c->snap.connecting = false;
                c->snap.failures   = 0;
                c->snap.exists     = true;
                ok                 = true;
            }
            g_cv.notify_all();
        }
        if (leftover) leftover->Disconnect(msg);
        return ok;
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> lock(g_mutex);
        c->snap.connecting = false;
        c->snap.error      = e.what();
        c->snap.failures++;
        g_cv.notify_all();
        return false;
    }
}

void launch_open(const std::shared_ptr<Conn> &c)
{
    if (!c) return;
    std::thread t([c] { open_conn(c); });
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_stop) {
            g_workers.push_back(std::move(t));
            return;
        }
    }
    // shutdown() already ran: join here so the worker cannot outlive it.
    if (t.joinable()) t.join();
}

void stop_and_join()
{
    for (;;) {
        std::vector<std::thread>                  workers;
        std::vector<std::shared_ptr<MqttClient>>  mqtts;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_stop = true;
            g_cv.notify_all();
            for (auto &kv : g_conns) {
                if (!kv.second) continue;
                if (kv.second->mqtt) mqtts.push_back(std::move(kv.second->mqtt));
                kv.second->snap.connected  = false;
                kv.second->snap.connecting = false;
            }
            g_conns.clear();
            workers.swap(g_workers);
        }
        disconnect_clients(mqtts);
        join_workers(workers);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_workers.empty()) break;
    }
}

} // namespace

AnycubicLan::Snapshot touch(const std::string &host)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto                        c = get_or_make(host);
    return c->snap;
}

AnycubicLan::Snapshot demand(const std::string &host)
{
    // Same contract as ElegooSdcpMonitor::demand: never wait on the network. Handshake + MQTT
    // open run on a tracked worker; describe_hosts / the event watcher only read the last snap.
    std::shared_ptr<Conn>   c;
    bool                    launch = false;
    AnycubicLan::Snapshot   out;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_stop) return {};
        c             = get_or_make(host);
        c->keep_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(KEEP_MS);
        if (!(c->snap.connected && c->mqtt) && !c->snap.connecting) {
            c->snap.connecting = true;
            launch             = true;
        }
        out = c->snap;
    }
    if (launch) launch_open(c);
    return out;
}

CommandResult command(const std::string &host, const std::string &type, const std::string &action, const nlohmann::json &inner,
                      int timeout_ms)
{
    CommandResult                  r;
    std::shared_ptr<Conn>          c;
    std::shared_ptr<MqttClient>    mqtt;
    std::string                    cn, model_id;
    bool                           launch = false;
    const int                      wait_ms = std::max(0, timeout_ms);
    const auto                     deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);

    {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (g_stop) {
            r.error = "shutting down";
            return r;
        }
        c = get_or_make(host);
        if (!(c->snap.connected && c->mqtt) && !c->snap.connecting && wait_ms > 0) {
            c->snap.connecting = true;
            launch             = true;
        }
    }
    if (launch) launch_open(c);

    {
        std::unique_lock<std::mutex> lock(g_mutex);
        while (!g_stop && !(c->snap.connected && c->mqtt) && c->snap.connecting) {
            if (wait_ms == 0) break;
            if (g_cv.wait_until(lock, deadline) == std::cv_status::timeout) break;
        }
        if (g_stop) {
            r.error = "shutting down";
            return r;
        }
        if (!(c->snap.connected && c->mqtt)) {
            if (wait_ms > 0 && c->snap.connecting)
                r.error = "timeout";
            else
                r.error = c->snap.error.empty() ? "not connected" : c->snap.error;
            return r;
        }
        mqtt     = c->mqtt;
        cn       = c->creds.device_id.empty() ? c->snap.info.cn : c->creds.device_id;
        model_id = c->creds.model_id.empty() ? c->snap.info.model_id : c->creds.model_id;
    }

    const auto        env   = AnycubicLan::command_envelope(AnycubicLan::make_msgid(), cn, model_id, type, action, inner);
    std::string       msg;
    const std::string topic = AnycubicLan::mqtt_slicer_topic(model_id, cn, type);
    try {
        r.sent = mqtt && mqtt->Publish(topic, env.dump(), 0, msg);
        if (!r.sent) r.error = msg.empty() ? "publish failed" : msg;
    } catch (const std::exception &e) {
        r.sent  = false;
        r.error = e.what();
    }
    return r;
}

void apply_report_for_tests(const std::string &host, const std::string &topic, const std::string &payload)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto                        c = get_or_make(host);
    AnycubicLan::parse_report(topic, payload, c->snap.status);
    c->snap.exists = true;
}

void shutdown() { stop_and_join(); }

void reset_for_tests()
{
    stop_and_join();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stop = false;
}

} // namespace AnycubicLanMonitor
} // namespace Slic3r
