#ifndef slic3r_ElegooSdcpMonitor_hpp_
#define slic3r_ElegooSdcpMonitor_hpp_

// The hub's one SDCP connection per Elegoo printer (Centauri Carbon 1), shared by everything in this
// process that wants to know about it: the /api/printers rows (the phone app, the hub's /summary,
// the desktop Printers monitor), the event watcher, the phone's pause / resume / stop / light / fan,
// and the desktop's CANVAS reads (canvas_query() goes through here while the connection is up).
//
// Why one long-lived connection rather than one per question: the CC1 takes only a few websocket
// clients at once (four, per the elegoo-homeassistant docs; the fifth handshake is declined, which
// ElegooLink.cpp already maps), and the printer's own web UI, the Elegoo app, the camera relay's
// Cmd 386 and an upload all need one too. ElegooSlicer's elegoo-link keeps exactly one persistent
// connection per printer with a "ping" every 20 s; so does this.
//
// The rules it keeps:
//  * Every step has a deadline (resolve 4 s, connect 4 s, handshake 5 s, a write 10 s, silence
//    45 s then a Cmd 0 probe that must be answered within 15 s). A printer that is off costs a
//    background attempt with backoff (2 s doubling to 60 s, 15 s while somebody watches), never a
//    caller's time: touch() and the snapshot never wait for the network.
//  * The printer's own status pushes are the data. Cmd 0 is sent on connect, when somebody starts
//    watching, and as the liveness probe; Cmd 324 (CANVAS) only while somebody watches, only while
//    the printer says a CANVAS is attached (AmsConnectStatus 1), and at most every 15 s.
//  * One connection per printer per PC: a lock file under <datadir>/hub/sdcp makes a second
//    EdgeSlicer window leave the printer to the first (its row then says so).
//  * Idle for two minutes (nothing asked about the printer) and the connection is closed.
//  * No address, mainboard id or serial in the log.

#include "ElegooCanvas.hpp"
#include "ElegooSdcp.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r {
namespace SdcpMonitor {

// Any thread. The printer at `host` (a bare host name or address), starting its connection if there
// is none and keeping it from going idle. Never blocks on the network: the first call returns a
// snapshot with nothing in it yet.
Sdcp::Snapshot touch(const std::string& host);

// Somebody is looking at the printers right now (the app's Devices page or printer screen, the hub
// page, the desktop Printers tab). Lets the CANVAS be read for the next 45 s.
void note_watching();
bool watching();

struct CommandResult
{
    bool        sent { false };     // it went out on the connection
    bool        answered { false }; // the printer answered it (ack is its Ack)
    int         ack { -1 };
    std::string error;              // why not, English
};

// Worker thread only (it waits): send one command over the printer's connection and wait at most
// `timeout_ms` for the printer's answer to it. Fails at once when the printer is not connected.
CommandResult command(const std::string& host, int cmd, const nlohmann::json& data, int timeout_ms);

// The CANVAS through the printer's live connection. False when this process has no live connection
// to it (the caller then reads it the old way, on a short connection of its own); true when `out`
// is the answer, good or bad. Blocks for at most opts.step_timeout_ms.
bool canvas_query_shared(const std::string& host, const CanvasQueryOptions& opts, CanvasQueryResult& out);

// Closes every connection and stops the network thread. Safe to call more than once.
void shutdown();

// ---- for the tests and the gates ----

// The SDCP port (3030, or the SNORCA_SDCP_PORT environment variable). Tests point it at a mock.
void set_port_for_tests(int port);
// Forget every connection now (closed, not idle-timed out), so a test starts clean.
void reset_for_tests();

} // namespace SdcpMonitor
} // namespace Slic3r

#endif // slic3r_ElegooSdcpMonitor_hpp_
