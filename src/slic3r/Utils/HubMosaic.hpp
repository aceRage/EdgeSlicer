#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
namespace HubMosaic {

// The camera mosaic for a TV wall (EdgeSlicer TV app, phase 2): one H.264 stream that shows a page
// of the hub's cameras side by side, so a TV with only a couple of hardware decoders plays a whole
// 2x2 or 3x3 page with ONE of them. The hub decodes each camera of the page from the relay
// sources it already has (one small ffmpeg per cell), composites them itself and encodes the page
// with one more ffmpeg (a hardware encoder when the PC has one); go2rtc carries the result to the TV as
// the stream "edge_mosaic", through the same /api/ws tunnel every other tile uses.
//
// Everything here is pure: the layout math, the ffmpeg argument list, the encoder choice and the
// SPS gate are unit-tested (tests/slic3rutils/hub_mosaic_tests.cpp); RemoteHub runs the process.

// The go2rtc stream name the TV plays.
constexpr const char* STREAM_NAME = "edge_mosaic";
// Never more than this many cells, whatever is asked.
constexpr int MAX_CELLS = 9;

struct Layout
{
    int cols = 2;
    int rows = 2;
    int cells() const { return cols * rows; }
    std::string name() const { return std::to_string(cols) + "x" + std::to_string(rows); }
};

// "2x2", "3x3", "2x1" ...: 1..3 columns by 1..3 rows. False for anything else.
bool parse_layout(const std::string& text, Layout& out);

// The camera ids of "a,b,c": trimmed, empties and repeats dropped, at most `max` of them, and only
// ids made of letters, digits and . _ : - (at most 64 characters), as the Stream tab makes them.
std::vector<std::string> parse_ids(const std::string& csv, size_t max);

struct Size
{
    int w = 1920;
    int h = 1080;
};

// 1080 (the default) or 720 lines, 16:9. Anything else is 1080.
Size output_size(int height);

// Frames per second, 5..30; 0 or nonsense is the default 15.
int clamp_fps(int fps);

struct Rect
{
    int x = 0, y = 0, w = 0, h = 0;
};

// Cell `index` (row by row) of `layout` on a `size` canvas. Widths and heights are even (what
// yuv420p needs); the last column and row take the remainder so the cells fill the canvas exactly.
Rect cell_rect(const Layout& layout, const Size& size, int index);

enum class Encoder { None, Nvenc, Qsv, Amf, VideoToolbox, MediaFoundation, OpenH264, X264 };

const char* encoder_name(Encoder e);

// The H.264 encoders an `ffmpeg -encoders` listing offers, best first: NVENC, Quick Sync, AMF,
// VideoToolbox, Media Foundation, then the software ones (libopenh264 - the bundled LGPL build's -
// and libx264, which only a system ffmpeg has). The hub test-encodes them in this order and keeps
// the first that works on this PC.
std::vector<Encoder> encoder_preference(const std::string& encoders_listing);

// Low-latency settings for `e` at `kbps`: no B-frames, constant bitrate, a profile every TV
// decoder and both WebRTC and MSE accept.
std::vector<std::string> encoder_args(Encoder e, int kbps);

// The bitrate for a canvas at `fps`, scaled by quality "low" / "med" / "high" (anything else: med).
int default_kbps(const Size& size, int fps, const std::string& quality);

enum class CellKind {
    Rtsp,        // a go2rtc stream, read from go2rtc's loopback RTSP listener
    Mjpeg,       // a hub MJPEG relay (Bambu P1/A1 /bambu, Elegoo CC1 /elegoo), multipart over HTTP
    Placeholder, // a dark cell with a note: "Offline", "Viewer limit reached", ...
    Empty        // nothing on this cell of the page
};

struct Cell
{
    std::string id;
    CellKind    kind = CellKind::Empty;
    std::string url;   // Rtsp / Mjpeg
    std::string label; // the camera's name, drawn in the corner
    std::string note;  // Placeholder: what to say
};

struct Spec
{
    Layout            layout;
    Size              size;
    int               fps     = 15;
    int               kbps    = 4000;
    Encoder           encoder = Encoder::OpenH264;
    std::string       font;  // a TrueType file for the labels; empty draws no text
    std::vector<Cell> cells; // layout.cells() of them (missing ones are Empty)

    bool same_page(const Spec& o) const;
};

// What a label or note may say: printable ASCII without the characters ffmpeg's filter syntax or
// drawtext's expansion would act on (' \ : % ; , [ ] = and the like), at most 40 characters.
std::string sanitize_text(const std::string& text);

// drawtext's fontfile value for `path`: forward slashes, ':' escaped, single-quoted. Empty when the
// path holds a character that cannot be quoted safely.
std::string font_value(const std::string& path);

// The -filter_complex graph: each live input scaled and padded to its cell, placeholders drawn
// with `color`, labels with drawtext, all put together by xstack into [out].
std::string build_filter(const Spec& spec);

// ---- The pipeline (2026-10-23: startup time) --------------------------------------------------
// One ffmpeg reading every camera kept the TV on a blank wall for 7-13 s: ffmpeg opens its inputs
// one after another (each waits for its camera's first frame), xstack then waits for every input,
// and the hub first tried each camera once on its own. Now each live cell has its own small ffmpeg
// (a "feeder") that decodes its camera to raw frames at the cell's size, and the hub composites
// the latest frame of every cell into the canvas at a steady rate and pipes that into ONE encoder
// ffmpeg. The encoder never waits on a camera: the first keyframe goes out within a few hundred ms
// showing a "Connecting..." card per cell, and each camera appears as soon as its own first frame
// does. A camera that fails turns into an "Offline" card without touching the others.

// What a live cell shows until its first frame, and after its feeder failed.
constexpr const char* NOTE_CONNECTING = "Connecting...";
constexpr const char* NOTE_OFFLINE    = "Offline";

// Bytes of one yuv420p frame of w x h (both even).
inline size_t i420_size(int w, int h) { return (size_t) w * h * 3 / 2; }

// A feeder: one camera decoded and scaled into its cell (letterboxed), its label drawn, raw yuv420p
// frames of exactly r.w x r.h on stdout. Small probe and no input buffering so the first frame
// leaves as soon as the camera sends it.
std::vector<std::string> feeder_args(const std::string& ffmpeg, const Cell& cell, const Rect& r, const std::string& font);

// The whole canvas as one still yuv420p frame on stdout: every placeholder and empty cell as the
// stream shows it, every live cell as a card saying `live_note` (NOTE_CONNECTING / NOTE_OFFLINE)
// with its label. The compositor shows a cell's part of it while that camera has no picture.
std::vector<std::string> card_args(const std::string& ffmpeg, const Spec& spec, const std::string& live_note);

// The encoder: raw yuv420p canvas frames at spec.fps on stdin, raw H.264 (Annex B, no B-frames,
// SPS/PPS repeated on every keyframe, a keyframe every second) on stdout.
std::vector<std::string> encoder_args_raw(const std::string& ffmpeg, const Spec& spec);

// Copies cell `r` of a canvas-sized yuv420p frame `src` into the same place of `dst`.
void copy_cell_i420(unsigned char* dst, const unsigned char* src, const Size& canvas, const Rect& r);
// Writes a cell-sized yuv420p frame (r.w x r.h) into cell `r` of the canvas frame `dst`.
void blit_i420(unsigned char* dst, const Size& canvas, const Rect& r, const unsigned char* cell);

// Keeps the encoder's output from its latest SPS (every keyframe has one in front), so whoever
// starts reading the stream - go2rtc connecting after the hub already started the mosaic - gets a
// keyframe at once instead of waiting for the next one. Bounded: past `cap` bytes without an SPS
// it holds nothing until the next one.
class GopCache
{
public:
    explicit GopCache(size_t cap = 8u << 20) : m_cap(cap) {}
    void feed(const char* data, size_t n);
    // From an SPS to now; empty before the first SPS (or after the cap was hit).
    const std::string& data() const { return m_data; }
    bool keyframes_seen() const { return m_seen; }

private:
    size_t      m_cap;
    std::string m_data;
    std::string m_tail; // the last 4 bytes fed, to find a start code split across two feeds
    bool        m_seen = false;
    bool        m_open = false; // m_data holds a GOP that began with an SPS
};

// How long to wait before trying a failed camera again: 3 s, doubling, at most 30 s.
int retry_delay_ms(int failures);

// The cell note for an Elegoo relay's /elegoo/status "state": "" when the camera may be shown.
std::string elegoo_note(const std::string& state);

// Passes a raw H.264 byte stream on from its first SPS. A stream that starts mid-GOP begins with a
// slice that also reads as an HEVC header, and go2rtc then decodes nothing (see RemoteHub's
// relay_h264, the same rule). feed() appends what may be forwarded to `out`.
class H264StartGate
{
public:
    void feed(const char* data, size_t n, std::string& out);
    bool started() const { return m_started; }
    void reset() { m_started = false; m_pend.clear(); }

private:
    bool        m_started = false;
    std::string m_pend;
};

} // namespace HubMosaic
} // namespace Slic3r
