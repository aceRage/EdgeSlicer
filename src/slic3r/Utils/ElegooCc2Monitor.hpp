#ifndef slic3r_ElegooCc2Monitor_hpp_
#define slic3r_ElegooCc2Monitor_hpp_

// The hub's MQTT connection to an Elegoo Centauri Carbon 2 (or Centauri 2): at most one per printer,
// shared by everything in this process that wants to know about it - the /api/printers rows (the
// phone app, the hub's /summary, the desktop Printers tab, the CC2 device page), the event watcher
// and the pause / resume / stop controls. The CC2 counterpart of ElegooSdcpMonitor (the CC1's), with
// the same on-demand rules, because the CC2 limits its clients too ("too many clients" at
// registration) and Elegoo's own app and panel need them:
//  * A request (demand()) connects and keeps the connection for KEEP (60 s) after the last one:
//    an /api/printers somebody is looking at, a device page that is open, a control.
//  * Nobody asking: a check-in at most every CHECKIN (5 min) - connect, register, one whole status,
//    disconnect - so a row and the event watcher are never older than that.
//  * A check-in (or a live connection) that finds the printer printing while somebody is subscribed
//    to notifications keeps the connection for the whole print and KEEP after it.
//  * While connected the printer's own pushes (method 6000 on elegoo/<sn>/api_status) are the data;
//    a heartbeat {"type":"PING"} goes out every 10 s (elegoo-link's interval), a connection silent
//    for 45 s is asked for its status and dropped if that is not answered in 15 s, five push-id gaps
//    in a row ask for the whole status again, and the CANVAS (2005) is read every 15 s while somebody
//    looks and the printer has one.
//  * A failed attempt backs off 2, 4, 8, 16, 30, 60 s (15 s at most while demanded); a row older than
//    the stale limit keeps its readings but loses its buttons.
//  * One connection per printer per PC: a lock file under <datadir>/hub/cc2 makes a second
//    EdgeSlicer window leave the printer to the first.
//  * Every network step runs on the printer's own worker thread; touch() and demand() never wait.
//  * No address, serial or access code in the log.

#include "ElegooCc2.hpp"
#include "ElegooCc2Status.hpp"

#include <functional>
#include <memory>
#include <string>

namespace Slic3r {
namespace ElegooCc2Monitor {

// Any thread, never blocks. The printer at `host` (bare host or address) as last heard; it becomes
// known (and gets its check-ins) but no connection is held for it. `token` is the access code
// (the device's API key; empty = the factory default).
ElegooCc2::Snapshot touch(const std::string& host, const std::string& token);

// Any thread, never blocks. Somebody needs the printer now: connect and stay for KEEP.
ElegooCc2::Snapshot demand(const std::string& host, const std::string& token);

// Somebody is subscribed to notifications (told by the hub on every poll, as for SdcpMonitor).
void note_subscribers(bool any);

struct CallResult
{
    bool        sent { false };
    bool        answered { false };
    int         error_code { -1 }; // result.error_code
    std::string result;            // the result object as JSON text
    std::string error;             // why not, English
};

// Worker thread only (it waits): a request (connects if needed), then one method and its answer,
// within timeout_ms for both.
CallResult call(const std::string& host, const std::string& token, int method, const std::string& params_json, int timeout_ms);

// Whether this process holds a live connection to the printer right now.
bool connected(const std::string& host);

// Closes every connection and joins the workers. Safe to call more than once.
void shutdown();

// ---- for the tests and the gates ----

// How a link reaches its printer: the serial number (from /system/info) and a transport ready for
// MqttTransport::connect. The default is ElegooCc2Net's HTTP + paho. False with error set when the
// printer cannot be reached.
using Connector = std::function<bool(const std::string& host, const std::string& token, std::string& sn,
                                     std::unique_ptr<ElegooCc2::MqttTransport>& transport, std::string& error)>;
void set_connector_for_tests(Connector connector); // nullptr = the default
// KEEP, CHECKIN and the stale limit in milliseconds / seconds (0 = defaults: 60 s, 5 min, 120 s;
// or SNORCA_CC2_KEEP_S / SNORCA_CC2_CHECKIN_S from the environment). heartbeat_ms / silence_ms
// (0 = 10 s / 45 s) let a test see the heartbeat and the silence probe quickly.
void set_timing_for_tests(int keep_ms, int checkin_ms, int heartbeat_ms = 0, int silence_ms = 0);
int  stale_after_s();
// Forget every connection now (joins the workers), so a test starts clean.
void reset_for_tests();

// The default connector (ElegooCc2Net.cpp): GET /system/info, then a paho client.
bool net_connector(const std::string& host, const std::string& token, std::string& sn,
                   std::unique_ptr<ElegooCc2::MqttTransport>& transport, std::string& error);

} // namespace ElegooCc2Monitor
} // namespace Slic3r

#endif // slic3r_ElegooCc2Monitor_hpp_
