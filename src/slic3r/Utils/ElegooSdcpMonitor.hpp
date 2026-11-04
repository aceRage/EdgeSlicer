#ifndef slic3r_ElegooSdcpMonitor_hpp_
#define slic3r_ElegooSdcpMonitor_hpp_

// The hub's SDCP connection to an Elegoo printer (Centauri Carbon 1): at most one per printer,
// shared by everything in this process that wants to know about it - the /api/printers rows (the
// phone app, the hub's /summary, the desktop Printers tab), the event watcher, the phone's pause /
// resume / stop / light / fan, the desktop's CANVAS reads, and the camera relay's Cmd 386.
//
// Connected only on demand (owner, 2026-10-18), because the CC1 takes only a few websocket clients
// (four, per the elegoo-homeassistant docs; about five, per the camera relay's reading) and its own
// web page, the Elegoo app, the camera and an upload need them too:
//  * A request opens it and keeps it for KEEP (60 s) after the last one. Requests are demand():
//    an /api/printers that somebody is looking at (the app, the hub page, the Printers tab - the
//    hub's own background poll is not one), a CANVAS read, an upload / start-print, a control.
//  * Nobody asking: a check-in at most every CHECKIN (5 min) - connect, one status, disconnect - so
//    the row and the event watcher are never older than that.
//  * A check-in (or a live connection) that finds the printer printing while somebody is subscribed
//    to notifications (note_subscribers) keeps the connection for the whole print, and KEEP after it
//    ends, so started / paused / finished / failed reach the phone on time. With no subscribers it is
//    never held.
//  * Every step has a deadline (resolve 4 s, connect 4 s, handshake 5 s, write 10 s, silence 45 s
//    then a Cmd 0 probe answered within 15 s); a printer that is off costs a background attempt,
//    never a caller's time: touch() and demand() never wait for the network.
//  * One connection per printer per PC: a lock file under <datadir>/hub/sdcp makes a second
//    EdgeSlicer window leave the printer to the first (its row then says so).
//  * The printer's own status pushes are the data while connected; Cmd 324 (CANVAS) only while it
//    is demanded, only while AmsConnectStatus is 1, at most every 15 s.
//  * No address, mainboard id or serial in the log.

#include "ElegooCanvas.hpp"
#include "ElegooSdcp.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r {
namespace SdcpMonitor {

// Any thread, never blocks. The printer at `host` (a bare host name or address) as last heard. A
// passive look: it keeps the printer known (so it gets its check-ins) but opens no connection that
// stays - the event watcher and the hub's background poll use this.
Sdcp::Snapshot touch(const std::string& host);

// Any thread, never blocks. Somebody needs the printer now: connect (if not connected) and stay for
// KEEP after this call. Returns the snapshot as touch() does.
Sdcp::Snapshot demand(const std::string& host);

// Somebody is subscribed to notifications (a configured destination, a browser, the app): told by
// the hub on every poll. A print found by a check-in is then held for its whole length.
void note_subscribers(bool any);

struct CommandResult
{
    bool        sent { false };     // it went out on the connection
    bool        answered { false }; // the printer answered it (ack is its Ack)
    int         ack { -1 };
    std::string frame;              // the answer as the printer sent it
    std::string error;              // why not, English
};

// Worker thread only (it waits): a request for the printer (connects if needed, waiting at most
// `timeout_ms` for that and the answer together), then one command and its answer.
CommandResult command(const std::string& host, int cmd, const nlohmann::json& data, int timeout_ms);

// Worker thread only: the same, but only over a connection that is already up - no demand, no
// connect. `sent` is false (and nothing happened) when there is none. The camera relay's Cmd 386.
CommandResult command_if_connected(const std::string& host, int cmd, const nlohmann::json& data, int timeout_ms);

// Whether this process holds a live connection to the printer right now.
bool connected(const std::string& host);

// The CANVAS through the printer's connection (a request: it connects when needed, within
// opts.step_timeout_ms). False when this process cannot use it - another window holds the printer,
// or it did not come up in time - and the caller then reads it the old way on a short connection of
// its own; true when `out` is the answer, good or bad.
bool canvas_query_shared(const std::string& host, const CanvasQueryOptions& opts, CanvasQueryResult& out);

// Closes every connection and stops the network thread. Safe to call more than once; the next
// touch() starts over.
void shutdown();

// ---- for the tests and the gates ----

// The SDCP port (3030, or the SNORCA_SDCP_PORT environment variable). Tests point it at a mock.
void set_port_for_tests(int port);
// KEEP and CHECKIN in milliseconds (0 = the defaults, 60 s and 5 min, or SNORCA_SDCP_KEEP_S /
// SNORCA_SDCP_CHECKIN_S from the environment for the gate).
void set_timing_for_tests(int keep_ms, int checkin_ms);
// Forget every connection now, so a test starts clean.
void reset_for_tests();

} // namespace SdcpMonitor
} // namespace Slic3r

#endif // slic3r_ElegooSdcpMonitor_hpp_
