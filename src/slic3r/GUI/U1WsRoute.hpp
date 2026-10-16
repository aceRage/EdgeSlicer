#pragma once

// U1 local-first, Plan B (owner decision 2026-10-14, hardware probe 2026-10-15): a Snapmaker U1
// bound to the Snapmaker account is talked to over the LAN - its own Moonraker WebSocket,
// ws://<printer>/websocket, which answers without a token in cloud mode - whenever it is there,
// and through the Snapmaker cloud only for what the LAN cannot do. Design:
// tests/design_u1_local_first.md section 6 (main checkout).
//
// The Device page keeps its cloud MQTT session (sw_create_mqtt_client with the cloud broker) and
// all its subscriptions. A U1WsRoute rides on that session's MqttClient (MqttClient::SetRoute):
//   - a JSON-RPC request the page (or our Moonraker_Mqtt host) publishes on <sn>/request goes to
//     the printer's WebSocket instead; the answer comes back as a <sn>/response message;
//   - Moonraker's notifications come back on <sn>/notification, notify_status_update (the stream
//     printer.objects.subscribe starts) on <sn>/status;
//   - a method the WebSocket does not know (-32601) is sent again through the cloud and goes there
//     from then on; a request whose LAN answer never comes is sent through the cloud when it only
//     reads (anything else may already have run);
//   - while the LAN serves, the cloud's own answers, notifications and status for this printer are
//     dropped (only answers to requests the route sent through the cloud get through), so the page
//     sees every message once;
//   - when the WebSocket drops (keep-alive pings), or two LAN answers in a row time out, everything
//     goes through the cloud again at once; the LAN is retried after 5/15/30/60 s, then every 2 min.
// Uploads are unchanged: the page already sends files to the printer's HTTP port 80.
//
// app_config "snapmaker_local_first": "0" = cloud only. Anything else (missing included) = on.

#include "slic3r/Utils/MQTT.hpp"
#include "slic3r/Utils/MoonrakerWs.hpp"
#include "slic3r/Utils/U1LocalFirst.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

struct U1WsDeps
{
    U1LocalFirst::Timing timing;
    long long request_timeout_ms { 10000 }; // a LAN answer later than this counts as lost
    int       connect_timeout_ms { 4000 };
    int       idle_timeout_ms { 8000 };      // WebSocket keep-alive: silent this long = gone
    int       ws_port { 80 };
    // LAN addresses known for this serial number (local addresses only; the route adds the ones
    // the printer reports in machine.system_info).
    std::function<std::vector<std::string>(const std::string& sn)> known_addresses;
    // Does the host at `ip` answer as this printer (serial number over port 80)?
    std::function<bool(const std::string& ip, const std::string& sn)> is_that_printer;
    // Optional: the route changed (true = LAN). `why` never names the printer or its address.
    std::function<void(bool on_lan, const std::string& why)> route_changed;
    std::function<long long()> now_ms;
};

class U1WsRoute : public MqttRoute
{
public:
    static std::shared_ptr<U1WsRoute> attach(const std::shared_ptr<MqttClient>& owner, U1WsDeps deps);
    ~U1WsRoute() override;

    Result subscribe(const std::string& topic, int qos, std::string& msg) override;
    Result unsubscribe(const std::string& topic, std::string& msg) override;
    Result publish(const std::string& topic, const std::string& payload, int qos, std::string& msg) override;
    Result is_subscribed(const std::string& topic) const override;
    bool   accept_own_message(const std::string& topic, const std::string& payload) override;
    void   owner_closing() override;

    void        set_device(const std::string& sn);
    bool        on_lan() const;
    std::string serial() const;
    void        kick();
    bool        stop_and_wait(int timeout_ms);
    // Methods this process sends through the cloud for that printer (tests, logs).
    std::vector<std::string> cloud_methods() const;
    static void reset_pacing(); // tests

    struct State;

private:
    U1WsRoute() = default;
    std::shared_ptr<State> m;
};

// The app's world: SnapmakerLan for addresses and identity.
U1WsDeps make_app_u1_ws_deps();

} // namespace GUI
} // namespace Slic3r
