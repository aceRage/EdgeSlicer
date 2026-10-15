#pragma once

// U1 local-first (owner decision 2026-10-14): a Snapmaker U1 bound to the signed-in Snapmaker
// account is reached over the LAN whenever it answers there, and through Snapmaker's cloud only
// when it does not. Design: tests/design_u1_local_first.md (main checkout).
//
// STATUS (owner hand test 2026-10-15): OFF by default. A U1 in cloud mode answers the pairing
// check with "link mode is not LAN", and its LAN mode turns the cloud off, so on current firmware
// this path cannot work for a cloud-bound U1. Plan B in the design note (the printer's Moonraker
// WebSocket on port 80) waits on U1MoonrakerProbe's hardware result.
//
// The Device page keeps its cloud session (sw_create_mqtt_client with the cloud broker). A
// U1LanRoute rides on that session's MqttClient (MqttClient::SetRoute) and carries the printer's
// four device topics (<sn>/request, response, status, notification) over a second session to the
// printer's own broker while that one is up; everything else stays on the cloud. The page sees one
// client, the same callbacks and the same ids, in either case.
//
//   cloud only --(printer answers on the LAN, this PC's client id is authorised)--> on LAN
//   on LAN --(LAN session down 3 s, or two port-80 checks fail)--> cloud only, retry in 5/15/30/60/120 s
//
// The printer's broker takes a client only with the certificate the printer issued for its client
// id, and it hands that certificate out again (confirm_lan_status, on its plain pairing port 1884)
// to every client id it has authorised once on its screen. Each running EdgeSlicer has a client id
// of its own (a slot in <datadir>/hub/u1_lan_auth.json), so the one-time approval is per printer per
// PC (per slot), and two slicers never share an id on one printer.

#include "slic3r/Utils/MQTT.hpp"
#include "slic3r/Utils/U1LocalFirst.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class wxWindow;

namespace Slic3r {
namespace GUI {

struct U1LanCreds
{
    std::string ca, cert, key, client_id;
    int         port { 8883 };
};

// What a route needs from the world: the app's (make_app_u1_lan_deps) or a test's.
struct U1LanDeps
{
    U1LocalFirst::Timing timing;
    // LAN addresses known for this serial number (the route adds the ones the printer reports).
    std::function<std::vector<std::string>(const std::string& sn)> known_addresses;
    // Does the host at `ip` answer as this printer (its serial number, over port 80)?
    std::function<bool(const std::string& ip, const std::string& sn)> is_that_printer;
    // Port-80 liveness check while the route is on the LAN.
    std::function<bool(const std::string& ip)> probe;
    // One JSON-RPC exchange on the printer's pairing broker; the answer's "result" (null: none).
    std::function<nlohmann::json(const std::string& ip, const std::string& code, const std::string& method, const nlohmann::json& params)> pairing;
    // The LAN session, built but not connected.
    std::function<std::shared_ptr<MqttClient>(const std::string& ip, const U1LanCreds& creds)> make_lan_client;
    // This process's LAN client id ("" = no free slot: the route stays on the cloud).
    std::function<std::string()> client_id;
    std::function<U1LocalFirst::AskState(const std::string& sn)> ask_state;
    std::function<void(const std::string& sn, bool authorized)> set_authorized;
    std::function<void(const std::string& sn)> count_unanswered_ask;
    // The printer is showing its approval prompt for this PC now.
    std::function<void(const std::string& sn)> approval_requested;
    // Optional: the route moved (true = now on the LAN).
    std::function<void(const std::string& sn, bool on_lan)> route_changed;
    // Monotonic milliseconds.
    std::function<long long()> now_ms;
};

class U1LanRoute : public MqttRoute
{
public:
    // Set a new route on `owner` (the cloud session) and start its worker.
    static std::shared_ptr<U1LanRoute> attach(const std::shared_ptr<MqttClient>& owner, U1LanDeps deps);
    ~U1LanRoute() override;

    Result subscribe(const std::string& topic, int qos, std::string& msg) override;
    Result unsubscribe(const std::string& topic, std::string& msg) override;
    Result publish(const std::string& topic, const std::string& payload, int qos, std::string& msg) override;
    Result is_subscribed(const std::string& topic) const override;
    bool   accept_own_message(const std::string& topic, const std::string& payload) override;
    void   owner_closing() override;

    // From sw_mqtt_set_engine: the printer this session serves and its pairing access code.
    void        set_device(const std::string& sn, const std::string& access_code);
    bool        on_lan() const;
    std::string serial() const;
    std::string lan_address() const;
    // Wake the worker now (tests; a retry the person asked for).
    void        kick();
    // Stop the worker and wait until it has closed its LAN session (tests).
    bool        stop_and_wait(int timeout_ms);
    // Forget every printer's back-off and "asked this session" (tests).
    static void reset_pacing();

    struct State;

private:
    U1LanRoute() = default;
    std::shared_ptr<State> m;
};

// app_config "snapmaker_local_first" == "1" turns the route on; it is OFF by default. Current U1
// firmware refuses LAN pairing while the printer is in cloud mode (and its LAN mode switches the
// cloud off), so on a cloud-bound U1 the route could only ever probe port 1884 and give up.
bool u1_local_first_enabled();
// Attach a route to a page engine when its broker is the Snapmaker cloud and local-first is on.
void attach_u1_lan_route_if_cloud(const std::shared_ptr<MqttClient>& engine, const std::string& server_address);
// The route already on an engine, if any.
std::shared_ptr<U1LanRoute> u1_lan_route_of(const std::shared_ptr<MqttClient>& engine);

// One JSON-RPC exchange on a printer's plain pairing broker (<code>/config/request ->
// <code>/config/response), with a throw-away client id. The answer's "result"; null when the broker
// could not be reached or nothing answered within `timeout_ms`.
nlohmann::json u1_pairing_exchange(const std::string& host, int port, const std::string& code, const std::string& method,
                                   const nlohmann::json& params, int timeout_ms);

// The slot store (<dir>/u1_lan_auth.json) of one process: the slot is claimed on first use with a
// lock file (<dir>/u1_lan/slot<N>.lock) held until the process exits.
class U1LanSlotStore
{
public:
    explicit U1LanSlotStore(std::string dir, U1LocalFirst::Protector protector = default_protector());
    ~U1LanSlotStore();
    int                    slot();      // -1 when every slot is taken
    std::string            client_id(); // "" without a slot
    U1LocalFirst::AskState ask_state(const std::string& sn);
    void                   set_authorized(const std::string& sn, bool authorized);
    void                   count_unanswered_ask(const std::string& sn);

    // Windows: DPAPI (current user) + base64; elsewhere the identity (the file is made 0600).
    static U1LocalFirst::Protector default_protector();
    // The app's store, under RemoteHub::hub_dir().
    static U1LanSlotStore& app();

private:
    template<class Fn> void update(Fn&& fn);
    U1LocalFirst::SlotStore load();
    std::string             m_dir;
    U1LocalFirst::Protector m_protector;
    int                     m_slot { -2 }; // -2 = not claimed yet
    struct Lock;
    std::unique_ptr<Lock> m_lock;
};

// The read-only U1 Moonraker probe (Help menu), shown only with EDGESLICER_U1_PROBE=1.
bool u1_moonraker_probe_enabled();
void run_u1_moonraker_probe_dialog(wxWindow* parent);

// The app's world: SnapmakerLan for addresses and identity, the pairing exchange on port 1884,
// MQTTS to the printer, the slot store, a notification when the printer asks for approval.
U1LanDeps make_app_u1_lan_deps();

} // namespace GUI
} // namespace Slic3r
