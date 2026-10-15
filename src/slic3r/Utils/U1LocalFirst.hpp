#pragma once

// U1 local-first: the rules behind carrying a cloud-bound Snapmaker U1's device traffic over the LAN
// (GUI/U1LanRoute.cpp does the work). Everything here is plain logic - topics, addresses, pacing,
// the approval rule, the per-slot store and the phone hub's key store - so it can be tested with no
// broker, printer or wx app. Design: tests/design_u1_local_first.md (main checkout).
//
// The Device page talks JSON-RPC to the printer over MQTT. Bound to a Snapmaker account it uses the
// Snapmaker cloud's broker; added by IP/PIN it uses the printer's own LAN broker. Both carry the same
// four topics per printer - <sn>/request, <sn>/response, <sn>/status, <sn>/notification - with the
// same payloads, so a cloud session's device topics can be served by a LAN session instead while
// the page stays in its cloud mode. The LAN broker only accepts a client holding a certificate the
// printer issued for its client id; the printer hands that certificate out again
// (server.client_manager.confirm_lan_status) to any client id it has authorised once on its screen.

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {
namespace U1LocalFirst {

// ---------------------------------------------------------------- topics ----

enum class DeviceTopic { None, Request, Response, Status, Notification };

// "<sn>/request" and the like: one non-empty first level (a serial number or the '+' wildcard)
// and one of the four device suffixes, nothing more. "<sn>/config/response/<id>" is not one: the
// cloud's own topics stay on the cloud.
DeviceTopic device_topic_kind(const std::string& topic);
inline bool is_device_topic(const std::string& topic) { return device_topic_kind(topic) != DeviceTopic::None; }
// The serial number a device topic names; "" for any other topic or a wildcard.
std::string serial_of_topic(const std::string& topic);

// Where a device topic is served, and which of the two sessions' messages reach the page.
enum class Via { Cloud, Lan };

// The device topics the page (and our own host) asked for, and which session serves them now.
class RouteTable
{
public:
    // Remember a device subscription; false for any other topic (it stays with the cloud session).
    bool want(const std::string& topic, int qos);
    bool unwant(const std::string& topic);
    bool wanted(const std::string& topic) const { return m_wanted.count(topic) > 0; }
    const std::map<std::string, int>& wanted() const { return m_wanted; }
    Via  via() const { return m_via; }
    void set_via(Via v) { m_via = v; }
    // A message from `source` reaches the page only when it is not a device topic served by the
    // other session (that would deliver one answer twice while the route switches).
    bool accept(Via source, const std::string& topic) const;
    // Where an outgoing publish / subscribe on `topic` goes.
    Via  target(const std::string& topic) const { return is_device_topic(topic) ? m_via : Via::Cloud; }

private:
    std::map<std::string, int> m_wanted;
    Via                        m_via { Via::Cloud };
};

// ----------------------------------------------------------- addresses ----

bool is_private_ipv4(const std::string& ip);
// The printer's own IPv4 LAN addresses from a machine.system_info answer (Moonraker:
// result.system_info.network.<iface>.ip_addresses[{family, address}]), private ranges only, in
// interface order. Accepts the whole JSON-RPC response or just its result.
std::vector<std::string> lan_ipv4_from_system_info(const nlohmann::json& response);

// ---------------------------------------------------------------- pacing ----

struct Timing
{
    // Retries of the LAN while the cloud serves: after the 1st, 2nd, ... failure; the last repeats.
    std::vector<long long> retry_ms { 5000, 15000, 30000, 60000, 120000 };
    long long lan_down_grace_ms { 3000 };      // LAN session not connected this long -> fall back
    long long lan_probe_interval_ms { 10000 }; // port-80 check while on the LAN
    int       lan_probe_fails { 2 };           // that many failed checks in a row -> fall back
    long long approval_poll_ms { 3000 };       // confirm_lan_status while the printer asks
    long long approval_wait_ms { 90000 };      // how long the printer's prompt is waited for
    int       max_unanswered_asks { 2 };       // then the printer is not asked again
    long long device_wait_ms { 5000 };         // wait this long for the access code (set_engine)
};

// How long to wait before the next LAN attempt after `failures` failed ones (0 = at once).
long long retry_after_ms(const Timing& t, int failures);

struct LanHealth
{
    bool      mqtt_connected { true };
    long long down_for_ms { 0 };  // how long the LAN session has been not connected (0 when it is)
    int       probe_fails { 0 };  // port-80 checks in a row that did not answer
};
bool should_fall_back(const Timing& t, const LanHealth& h);

// ------------------------------------------------------------- approval ----

// confirm_lan_status / request_lan_auth answers (result.state).
enum class AuthAnswer { Success, Unauthorized, Authorizing, NotAvailable, NoAnswer };
AuthAnswer parse_auth_answer(const nlohmann::json& result);
// For the log: the answer's state and message and whether it carried the certificate - never the
// certificate, key or client id themselves.
std::string describe_auth_answer(const nlohmann::json& result);

struct AskState
{
    bool authorized { false };    // the printer accepted this slot's client id before
    int  asks { 0 };              // approval requests that went unanswered
    bool asked_this_session { false };
};

enum class AuthStep { UseLan, AskApproval, StayCloud };
// What to do after a confirm_lan_status check: use the certificate it returned, ask the person to
// approve this client id on the printer (once per session, and not after `max_unanswered_asks`
// requests nobody answered), or keep the cloud.
AuthStep next_auth_step(const Timing& t, AuthAnswer check, const AskState& s);

// ---------------------------------------------------------- slot store ----
//
// <datadir>/hub/u1_lan_auth.json. One LAN client id per slot (a running EdgeSlicer claims the
// lowest free slot), so two slicers on one PC never share a client id on the printer's broker; per
// serial what the printer said about that id. No certificate or key is kept: the printer hands the
// certificate out again on every connect.
//   {"version": 1, "slots": {"0": {"client_id": "<protected>",
//                                  "printers": {"<sn>": {"authorized": true, "asks": 0}}}}}

struct Protector
{
    // Windows: DPAPI (user scope) + base64. Identity when unset.
    std::function<std::string(const std::string&)> protect;
    std::function<std::string(const std::string&)> unprotect; // "" when it cannot be read back
};

// "orca-<uuid4>": the shape of the page's own LAN client ids.
std::string new_client_id();

class SlotStore
{
public:
    static SlotStore from_json(const nlohmann::json& j);
    nlohmann::json   to_json() const { return m_j; }

    // This slot's client id; a new one is made (and kept) when the slot has none or its stored
    // one cannot be read back (e.g. another Windows user's DPAPI blob).
    std::string client_id(int slot, const Protector& p);
    AskState    ask_state(int slot, const std::string& sn) const;
    void        set_authorized(int slot, const std::string& sn, bool authorized);
    void        count_unanswered_ask(int slot, const std::string& sn);
    // Only `slot` changes; everything else is taken from `other` (what is on disk now), so two
    // processes writing their own slots never undo each other.
    void        merge_into(nlohmann::json& other, int slot) const;

private:
    nlohmann::json& printer(int slot, const std::string& sn);
    nlohmann::json  m_j { nlohmann::json::object() };
};

// The lowest slot `try_claim` accepts, -1 when none of `max_slots` is free.
int pick_slot(const std::function<bool(int)>& try_claim, int max_slots = 8);

// --------------------------------------------- phone hub key store ----
//
// <datadir>/hub/snapmaker_keys.json: the certificate the Device page connected a printer with, kept
// (when the person allowed it) so the phone can connect that printer too. Only a LAN connect is
// kept - a cloud connect's certificate and 64-hex client id belong to that instance's cloud session,
// and a phone connect with them kicked the page off the cloud. Entries are filed under the page
// origin (the page server's port) that made them: the page's LAN client id lives in that origin's
// storage, so only the process serving that origin may reuse it.
//   {"version": 2, "<sn>": {"<origin>": {ca, cert, key, clientId, port}}}
bool           should_remember_keys(const nlohmann::json& connect_params);
nlohmann::json keys_put(nlohmann::json all, const std::string& sn, const std::string& origin, const nlohmann::json& entry);
// The entry this origin may use; null when there is none (version-1 flat entries are never used).
nlohmann::json keys_get(const nlohmann::json& all, const std::string& sn, const std::string& origin);

} // namespace U1LocalFirst
} // namespace Slic3r
