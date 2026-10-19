#include "AnycubicLanMonitor.hpp"

#include "MQTT.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

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
std::unordered_map<std::string, std::shared_ptr<Conn>> g_conns;
bool                                               g_stop { false };

std::string key_of(const std::string &host) { return AnycubicLan::bare_host(host); }

void close_conn(Conn &c)
{
    if (c.mqtt) {
        std::string msg;
        c.mqtt->Disconnect(msg);
        c.mqtt.reset();
    }
    c.snap.connected  = false;
    c.snap.connecting = false;
}

bool open_conn(const std::shared_ptr<Conn> &c)
{
    if (!c) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_stop) {
            c->snap.connecting = false;
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
        return false;
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
            return false;
        }
        mqtt->SetMessageCallback([wp = std::weak_ptr<Conn>(c)](const std::string &topic, const std::string &payload) {
            auto p = wp.lock();
            if (!p) return;
            AnycubicLan::parse_report(topic, payload, p->snap.status);
        });
        mqtt->Subscribe(AnycubicLan::mqtt_public_filter(model_id, cn), 0, msg);
        const auto q = AnycubicLan::command_envelope(AnycubicLan::make_msgid(), cn, model_id, "info", "query", nlohmann::json::object());
        mqtt->Publish(AnycubicLan::mqtt_slicer_topic(model_id, cn, "info"), q.dump(), 0, msg);

        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_stop) {
            mqtt->Disconnect(msg);
            c->snap.connecting = false;
            return false;
        }
        c->snap.info       = info;
        c->creds           = creds;
        c->mqtt            = mqtt;
        c->snap.connected  = true;
        c->snap.connecting = false;
        c->snap.failures   = 0;
        c->snap.exists     = true;
        return true;
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> lock(g_mutex);
        c->snap.connecting = false;
        c->snap.error      = e.what();
        c->snap.failures++;
        return false;
    }
}

std::shared_ptr<Conn> get_or_make(const std::string &host)
{
    const std::string k = key_of(host);
    auto              it = g_conns.find(k);
    if (it != g_conns.end()) return it->second;
    auto c         = std::make_shared<Conn>();
    c->host        = k;
    c->snap.exists = true;
    g_conns[k]     = c;
    return c;
}

void launch_open(const std::shared_ptr<Conn> &c)
{
    if (!c) return;
    std::thread([c] { open_conn(c); }).detach();
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
    // open run on a detached thread; describe_hosts / the event watcher only read the last snap.
    std::shared_ptr<Conn> c;
    bool                  launch = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_stop) return {};
        c             = get_or_make(host);
        c->keep_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(KEEP_MS);
        if (c->snap.connected && c->mqtt) return c->snap;
        if (!c->snap.connecting) {
            c->snap.connecting = true;
            launch             = true;
        }
    }
    if (launch) launch_open(c);
    return c->snap;
}

CommandResult command(const std::string &host, const std::string &type, const std::string &action, const nlohmann::json &inner,
                      int /*timeout_ms*/)
{
    CommandResult         r;
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_stop) {
            r.error = "shutting down";
            return r;
        }
        c = get_or_make(host);
    }
    if (!c->snap.connected || !c->mqtt) {
        if (!open_conn(c) || !c->mqtt) {
            r.error = c->snap.error.empty() ? "not connected" : c->snap.error;
            return r;
        }
    }
    const std::string cn       = c->creds.device_id.empty() ? c->snap.info.cn : c->creds.device_id;
    const std::string model_id = c->creds.model_id.empty() ? c->snap.info.model_id : c->creds.model_id;
    const auto        env      = AnycubicLan::command_envelope(AnycubicLan::make_msgid(), cn, model_id, type, action, inner);
    std::string       msg;
    const std::string topic = AnycubicLan::mqtt_slicer_topic(model_id, cn, type);
    r.sent                  = c->mqtt->Publish(topic, env.dump(), 0, msg);
    if (!r.sent) r.error = msg;
    return r;
}

void shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stop = true;
    for (auto &kv : g_conns)
        if (kv.second) close_conn(*kv.second);
    g_conns.clear();
}

void reset_for_tests()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stop = false;
    for (auto &kv : g_conns)
        if (kv.second) close_conn(*kv.second);
    g_conns.clear();
}

} // namespace AnycubicLanMonitor
} // namespace Slic3r
