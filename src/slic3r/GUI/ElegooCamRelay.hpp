#pragma once

#include <boost/asio/ip/tcp.hpp>

#include <cstddef>
#include <string>

namespace Slic3r {
namespace GUI {

// ElegooLink camera (Centauri Carbon, CC1), re-served by the hub's local relay (BambuCamRelay's port):
//
//   GET /elegoo?ip=<printer>[&fps=N]    the camera as multipart MJPEG, for a plain <img>
//   GET /elegoo/status?ip=<printer>     {"state":"live|busy|no_camera|failed|connecting|idle","message":...}
//
// The printer serves MJPEG at http://<ip>:3031/video (Elegoo's SDCP V3.0.0 Cmd 386 returns that
// address as VideoUrl) and lets only 4 viewers in at once (the MaximumVideoStreamAllowed attribute
// of Cmd 1; the 5th is refused with Ack 1 "exceeded maximum simultaneous streaming limit"). The
// relay therefore holds ONE connection to a printer however many tiles, windows or phones watch
// it, opens it when the first viewer arrives and closes it a moment after the last one leaves.
//
// How the stream is started: the relay first asks :3031/video directly (what the community
// clients do). Only if that gives nothing does it send SDCP Cmd 386 {"Enable":1} over
// ws://<ip>:3030/websocket - which also tells it whether the printer is full (Ack 1) or has no
// camera (Ack 2) - and tries the video address once more.
namespace ElegooCam {

constexpr int kVideoPort   = 3031;
constexpr int kControlPort = 3030;
constexpr int kStreamLimit = 4; // what the printer reports as MaximumVideoStreamAllowed

enum class State { Idle, Connecting, Live, Busy, NoCamera, Failed };

const char* state_name(State s);

// ---- pure helpers (unit-tested) ----------------------------------------------------------------

// A bare host or address the relay is willing to connect to: letters, digits, dot, dash, underscore.
bool valid_host(const std::string& host);

// The host of whatever the physical printer's "Hostname or IP" field holds ("192.168.1.5",
// "http://192.168.1.5:80/", "user@printer.local"): scheme, user info, port and path removed.
// Empty when nothing usable is left.
std::string host_from_address(const std::string& address);

// The Centauri Carbon 2 / Centauri 2 use the same "Elegoo Link" host type but a different camera:
// MQTT control (no SDCP on :3030), MJPEG on :8080 on any path, and not a thing on :3031 (pycentauri
// docs/PROTOCOL.md "Centauri Carbon 2", elegoo-link's elegoo_fdm_cc2 adapter, the Home Assistant
// elegoo-printer CC2 notes). This relay is the CC1's, so those are left out of the auto-add.
// True when a printer or preset name is one of those.
bool is_cc2_name(const std::string& name);

struct VideoTarget
{
    std::string host;
    int         port = kVideoPort;
    std::string path = "/video";
};

// The request line and headers for the MJPEG stream. HTTP/1.0 on purpose: no chunked encoding to
// strip out of the middle of a JPEG.
std::string video_request(const VideoTarget& t);

// What the printer answered to Cmd 386 as VideoUrl (a bare "host:3031/video" on firmware V1.4.49).
// Accepted only if it names the printer itself (or "this host": empty, 0.0.0.0, loopback); a host
// the printer cannot know about is refused, and so is any path with whitespace or control bytes.
bool video_target_from_reply(const std::string& raw, const std::string& printer_host, VideoTarget& out);

// SDCP Cmd 386 request. MainboardID is left empty, as the print command of ElegooLink does.
std::string enable_request(bool enable, const std::string& request_id, long long timestamp_s);

struct EnableReply
{
    bool        valid = false; // a Cmd 386 response (anything else on the socket is a status push)
    int         ack   = -1;
    std::string video_url;
};
EnableReply parse_enable_reply(const std::string& ws_text);

// Ack of Cmd 386: 0 ok, 1 too many viewers, 2 no camera, 3 (and anything else) unknown error.
State state_from_ack(int ack);
// The printer's HTTP answer to GET /video, when it is not a 200.
State state_from_http_status(int status);
int   parse_http_status(const std::string& head);

// One sentence for the tile. Busy is the one people hit: the printer allows 4 viewers.
std::string message_for(State s);
std::string status_json(State s);

// Splits a byte stream of JPEGs (SOI .. EOI) into frames, whatever the multipart framing between.
class JpegSplitter
{
public:
    void feed(const char* data, size_t n);
    bool next(std::string& frame);

private:
    std::string m_buf;
};

// ---- relay ---------------------------------------------------------------------------------------

bool is_elegoo_target(const std::string& request_target);

// Answers one connection of the local relay. `target` is the request target ("/elegoo?ip=...").
void serve_client(boost::asio::ip::tcp::socket& client, const std::string& target);

// For the unit tests, which stand a fake printer up on loopback ports of their own.
namespace Testing {
void   set_ports(int video_port, int control_port); // 0 for either puts the printer's real port back
size_t session_count();                             // printers with a connection open right now
} // namespace Testing

} // namespace ElegooCam
} // namespace GUI
} // namespace Slic3r
