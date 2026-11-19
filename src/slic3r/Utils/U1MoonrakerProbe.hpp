#pragma once

// A READ-ONLY look at a Snapmaker U1's own Moonraker on the LAN, for the owner to run against a
// printer in cloud (WAN) mode (Help menu, only with EDGESLICER_U1_PROBE=1). It answers one question
// for "Plan B" of tests/design_u1_local_first.md: does Moonraker's JSON-RPC answer locally while
// the printer is bound to the Snapmaker cloud - over HTTP (port 80, which the page already uploads
// to) and over its WebSocket (/websocket on port 80, and Moonraker's own port 7125)?
//
// It only ever sends HTTP GETs of the three read-only paths below and the three read-only JSON-RPC
// methods in read_only_methods(); is_read_only_method() guards every WebSocket send. No G-code, no
// print, no control, no file operation.

#include <string>
#include <vector>

namespace Slic3r {
namespace U1MoonrakerProbe {

struct Step
{
    std::string name;       // "GET :80/server/info", "WS :80/websocket server.info", ...
    bool        ok { false };
    int         status { 0 }; // HTTP status, 101 for a WebSocket upgrade, 0 when none
    long long   ms { 0 };     // how long the step took
    std::string detail;     // what came back, without serials, addresses or names
};

const std::vector<std::string>& read_only_methods(); // server.info, printer.info, printer.objects.query
bool                            is_read_only_method(const std::string& method);
const std::vector<std::string>& http_paths();        // the three GETs, relative to http://host:port

// The JSON-RPC request the probe sends for `method` (params for printer.objects.query: print_stats
// and extruder). Empty for any method that is not read-only.
std::string request_for(const std::string& method, long long id);

// A short, safe description of a JSON answer: the result's top-level keys (and klippy_state /
// print_stats.state when present), or the error's code and message. Never the whole body.
std::string describe_answer(const std::string& body);

struct Options
{
    int  timeout_s { 4 };
    bool http { true };
    // WebSocket ports to try, in order.
    std::vector<int> ws_ports { 80, 7125 };
    int  http_port { 80 };
};

// Runs every step against `host` (blocking; at most a few seconds per step).
std::vector<Step> run(const std::string& host, const Options& opt = Options());
// The steps as text for the dialog and the log, plus a one-line verdict.
std::string report(const std::vector<Step>& steps);

} // namespace U1MoonrakerProbe
} // namespace Slic3r
