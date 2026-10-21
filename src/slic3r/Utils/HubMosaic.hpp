#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
namespace HubMosaic {

// The camera mosaic for a TV wall (EdgeSlicer TV app, phase 2): one H.264 stream that shows a page
// of the hub's cameras side by side, so a TV with only a couple of hardware decoders plays a whole
// 2x2 or 3x3 page with ONE of them. The hub runs one ffmpeg for it (xstack over the relay sources it
// already has, a hardware encoder when the PC has one) and go2rtc carries the result to the TV as
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

// The whole ffmpeg command line (argv, no shell): inputs, the graph, the encoder, raw H.264
// (Annex B, SPS/PPS repeated on every keyframe) on stdout.
std::vector<std::string> build_args(const std::string& ffmpeg, const Spec& spec);

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
