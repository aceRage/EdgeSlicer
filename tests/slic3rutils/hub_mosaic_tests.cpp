#include <catch2/catch.hpp>

#include "slic3r/Utils/HubMosaic.hpp"

#include <algorithm>

using namespace Slic3r;
using namespace Slic3r::HubMosaic;

// The TV wall's camera mosaic: one H.264 stream per page of cameras (RemoteHub /r/<token>/mosaic,
// /relay/mosaic). The layout math, the ffmpeg command lines, the encoder choice, the canvas copies,
// the keyframe cache and the SPS gate.

static bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

static size_t index_of(const std::vector<std::string>& v, const std::string& s)
{
    return (size_t) (std::find(v.begin(), v.end(), s) - v.begin());
}

TEST_CASE("mosaic: layouts are 1..3 by 1..3", "[HubMosaic]")
{
    Layout l;
    CHECK(parse_layout("2x2", l));
    CHECK(l.cols == 2);
    CHECK(l.rows == 2);
    CHECK(l.cells() == 4);
    CHECK(parse_layout("3x3", l));
    CHECK(l.cells() == 9);
    CHECK(l.name() == "3x3");
    CHECK(parse_layout("2x1", l));
    CHECK(l.cells() == 2);
    CHECK_FALSE(parse_layout("4x4", l));
    CHECK_FALSE(parse_layout("0x2", l));
    CHECK_FALSE(parse_layout("22", l));
    CHECK_FALSE(parse_layout("", l));
    CHECK_FALSE(parse_layout("2x2x2", l));
}

TEST_CASE("mosaic: camera ids are cleaned, deduplicated and capped", "[HubMosaic]")
{
    const auto ids = parse_ids(" c1, 192.0.2.108 ,c1,,m3k9x2tile,bad id,x;y,u1:dev-1", 9);
    REQUIRE(ids.size() == 4);
    CHECK(ids[0] == "c1");
    CHECK(ids[1] == "192.0.2.108");
    CHECK(ids[2] == "m3k9x2tile");
    CHECK(ids[3] == "u1:dev-1");
    CHECK(parse_ids("a,b,c,d,e", 4).size() == 4);
    CHECK(parse_ids(std::string(65, 'a'), 4).empty());
}

TEST_CASE("mosaic: cells tile the canvas exactly, with even sizes", "[HubMosaic]")
{
    Layout l2;
    parse_layout("2x2", l2);
    const Size fhd = output_size(1080);
    CHECK(fhd.w == 1920);
    CHECK(fhd.h == 1080);
    const Rect a = cell_rect(l2, fhd, 0), d = cell_rect(l2, fhd, 3);
    CHECK(a.x == 0);
    CHECK(a.y == 0);
    CHECK(a.w == 960);
    CHECK(a.h == 540);
    CHECK(d.x == 960);
    CHECK(d.y == 540);
    CHECK(d.x + d.w == 1920);
    CHECK(d.y + d.h == 1080);

    Layout l3;
    parse_layout("3x3", l3);
    const Rect m = cell_rect(l3, fhd, 4), last = cell_rect(l3, fhd, 8);
    CHECK(m.x == 640);
    CHECK(m.y == 360);
    CHECK(last.x + last.w == 1920);
    CHECK(last.y + last.h == 1080);
    for (int i = 0; i < 9; ++i) {
        const Rect r = cell_rect(l3, output_size(720), i);
        CHECK(r.w % 2 == 0);
        CHECK(r.h % 2 == 0);
    }
    CHECK(output_size(720).w == 1280);
    CHECK(output_size(480).h == 1080); // anything else is the default
}

TEST_CASE("mosaic: frame rate and bitrate", "[HubMosaic]")
{
    CHECK(clamp_fps(0) == 15);
    CHECK(clamp_fps(2) == 5);
    CHECK(clamp_fps(60) == 30);
    CHECK(clamp_fps(24) == 24);
    const int med = default_kbps(output_size(1080), 15, "med");
    CHECK(med == 4000);
    CHECK(default_kbps(output_size(1080), 30, "") == 8100);
    CHECK(default_kbps(output_size(1080), 15, "low") == 2000);
    CHECK(default_kbps(output_size(1080), 15, "high") == 6100);
    CHECK(default_kbps(output_size(720), 5, "low") >= 500);
}

TEST_CASE("mosaic: hardware encoders lead, software ones follow", "[HubMosaic]")
{
    // What the bundled LGPL build lists (abridged): no libx264.
    const std::string listing = " V....D libopenh264          OpenH264 H.264\n"
                                " V....D h264_amf             AMD AMF H.264 Encoder\n"
                                " V....D h264_mf              H264 via MediaFoundation\n"
                                " V....D h264_nvenc           NVIDIA NVENC H.264 encoder\n"
                                " V..... h264_qsv             H.264 (Intel Quick Sync Video)\n"
                                " VFS..D mjpeg                MJPEG (Motion JPEG)\n";
    const auto pref = encoder_preference(listing);
    REQUIRE(pref.size() == 5);
    CHECK(pref[0] == Encoder::Nvenc);
    CHECK(pref[1] == Encoder::Qsv);
    CHECK(pref[2] == Encoder::Amf);
    CHECK(pref[3] == Encoder::MediaFoundation);
    CHECK(pref[4] == Encoder::OpenH264);
    CHECK(encoder_preference(" VFS..D mjpeg  MJPEG\n").empty());
    CHECK(encoder_preference(" V....D libx264  x264\n")[0] == Encoder::X264);

    for (Encoder e : { Encoder::Nvenc, Encoder::Qsv, Encoder::Amf, Encoder::MediaFoundation, Encoder::OpenH264, Encoder::X264,
                       Encoder::VideoToolbox }) {
        const auto args = encoder_args(e, 4000);
        REQUIRE(args.size() >= 2);
        CHECK(args[0] == "-c:v");
        CHECK(args[1] == encoder_name(e));
        CHECK(has(args, "4000k"));
    }
    // Low latency on NVENC: no lookahead, no delay, a baseline stream.
    const auto nv = encoder_args(Encoder::Nvenc, 4000);
    CHECK(has(nv, "ll"));
    CHECK(has(nv, "-zerolatency"));
    CHECK(has(nv, "baseline"));
    // The LGPL build's software encoder takes none of libx264's flags.
    const auto oh = encoder_args(Encoder::OpenH264, 4000);
    CHECK_FALSE(has(oh, "-preset"));
    CHECK_FALSE(has(oh, "-tune"));
}

static Spec page_2x2()
{
    Spec s;
    parse_layout("2x2", s.layout);
    s.size    = output_size(1080);
    s.fps     = 15;
    s.kbps    = 4000;
    s.encoder = Encoder::Nvenc;
    s.font    = "C:\\Windows\\Fonts\\arial.ttf";
    s.cells   = {
        Cell{ "u1", CellKind::Rtsp, "rtsp://127.0.0.1:41001/u1_192_0_2_108", "Left U1", "" },
        Cell{ "c1", CellKind::Mjpeg, "http://127.0.0.1:6000/bambu?ip=192.0.2.150&code=x&fps=15", "P1S", "" },
        Cell{ "cc1", CellKind::Placeholder, "", "Workshop CC1", "Viewer limit reached" },
        Cell{},
    };
    return s;
}

TEST_CASE("mosaic: the filter graph scales each live camera into its cell and stacks them", "[HubMosaic]")
{
    const std::string g = build_filter(page_2x2());
    // Two inputs, in cell order.
    CHECK(g.find("[0:v]setpts=PTS-STARTPTS,fps=15,scale=960:540:force_original_aspect_ratio=decrease,pad=960:540:") != std::string::npos);
    CHECK(g.find("[1:v]setpts=PTS-STARTPTS") != std::string::npos);
    CHECK(g.find("[2:v]") == std::string::npos);
    // The placeholder is drawn, not read: a busy CC1 is never connected.
    CHECK(g.find("color=c=0x22222a:s=960x540:r=15") != std::string::npos);
    CHECK(g.find("text='Viewer limit reached'") != std::string::npos);
    CHECK(g.find("color=c=black:s=960x540:r=15,format=yuv420p[c3]") != std::string::npos);
    CHECK(g.find("[c0][c1][c2][c3]xstack=inputs=4:layout=0_0|960_0|0_540|960_540:fill=black,null[out]") != std::string::npos);
    // Labels with the font, its drive colon escaped for the filter syntax.
    CHECK(g.find("fontfile='C\\:/Windows/Fonts/arial.ttf'") != std::string::npos);
    CHECK(g.find("text='Left U1'") != std::string::npos);
}

TEST_CASE("mosaic: without a live camera the graph is held to the clock", "[HubMosaic]")
{
    Spec s = page_2x2();
    for (auto& c : s.cells) c.kind = CellKind::Placeholder;
    CHECK(build_filter(s).find(",realtime[out]") != std::string::npos);
    Spec one;
    parse_layout("1x1", one.layout);
    one.cells = { Cell{ "u1", CellKind::Rtsp, "rtsp://127.0.0.1:1/u1", "U1", "" } };
    const std::string g = build_filter(one);
    CHECK(g.find("xstack") == std::string::npos);
    CHECK(g.find("[c0]null[out]") != std::string::npos);
}

TEST_CASE("mosaic: no font, no text; text never escapes its quotes", "[HubMosaic]")
{
    Spec s = page_2x2();
    s.font.clear();
    CHECK(build_filter(s).find("drawtext") == std::string::npos);
    CHECK(sanitize_text("Ace's [printer]: 100%; a=b,c\\d") == "Ace s  printer   100   a b c d");
    CHECK(sanitize_text(std::string(80, 'x')).size() == 40);
    CHECK(font_value("C:\\Windows\\Fonts\\arial.ttf") == "'C\\:/Windows/Fonts/arial.ttf'");
    CHECK(font_value("/usr/share/fonts/it's.ttf").empty());
    CHECK(font_value("").empty());
}

TEST_CASE("mosaic: a feeder decodes one camera into its cell as raw frames", "[HubMosaic]")
{
    const Spec s  = page_2x2();
    const Rect r1 = cell_rect(s.layout, s.size, 1);
    const auto a  = feeder_args("ffmpeg.exe", s.cells[1], r1, s.font);
    CHECK(a.front() == "ffmpeg.exe");
    CHECK(has(a, "-nostdin"));
    // The Bambu relay as multipart JPEG, one input, no buffering, no frame-threaded decoding.
    CHECK(has(a, "mpjpeg"));
    CHECK(std::count(a.begin(), a.end(), std::string("-i")) == 1);
    const size_t mj = index_of(a, "http://127.0.0.1:6000/bambu?ip=192.0.2.150&code=x&fps=15");
    REQUIRE(mj < a.size());
    CHECK(a[mj - 1] == "-i");
    CHECK(has(a, "nobuffer"));
    CHECK(has(a, "low_delay"));
    const size_t vf = index_of(a, "-vf");
    REQUIRE(vf + 1 < a.size());
    CHECK(a[vf + 1].find("scale=960:540:force_original_aspect_ratio=decrease,pad=960:540:") == 0);
    CHECK(a[vf + 1].find("text='P1S'") != std::string::npos);
    CHECK(a[vf + 1].find("fontfile='C\\:/Windows/Fonts/arial.ttf'") != std::string::npos);
    CHECK(a[a.size() - 4] == "rawvideo");
    CHECK(a[a.size() - 2] == "yuv420p");
    CHECK(a.back() == "pipe:1");
    // A go2rtc stream over loopback RTSP (TCP).
    const auto r = feeder_args("ffmpeg.exe", s.cells[0], cell_rect(s.layout, s.size, 0), "");
    CHECK(has(r, "rtsp://127.0.0.1:41001/u1_192_0_2_108"));
    CHECK(has(r, "tcp"));
    CHECK_FALSE(has(r, "mpjpeg"));
    CHECK(r[index_of(r, "-vf") + 1].find("drawtext") == std::string::npos); // no font, no label
}

TEST_CASE("mosaic: the cards draw every live cell as a note, never read a camera", "[HubMosaic]")
{
    const auto a = card_args("ffmpeg.exe", page_2x2(), NOTE_CONNECTING);
    CHECK(std::count(a.begin(), a.end(), std::string("-i")) == 0);
    const std::string g = a[index_of(a, "-filter_complex") + 1];
    CHECK(g.find("text='Connecting...'") != std::string::npos);
    CHECK(g.find("text='Left U1'") != std::string::npos);              // the live cell's label
    CHECK(g.find("text='Viewer limit reached'") != std::string::npos); // the placeholder as it is
    CHECK(g.find("rtsp://") == std::string::npos);
    CHECK(has(a, "-frames:v"));
    CHECK(a[index_of(a, "-frames:v") + 1] == "1");
    CHECK(a.back() == "pipe:1");
    const auto off = card_args("ffmpeg.exe", page_2x2(), NOTE_OFFLINE);
    CHECK(off[index_of(off, "-filter_complex") + 1].find("text='Offline'") != std::string::npos);
}

TEST_CASE("mosaic: the encoder reads raw canvas frames and writes raw H.264 to stdout", "[HubMosaic]")
{
    const auto a = encoder_args_raw("ffmpeg.exe", page_2x2());
    CHECK(a.front() == "ffmpeg.exe");
    CHECK_FALSE(has(a, "-nostdin")); // stdin is its input
    CHECK(a[index_of(a, "-i") + 1] == "pipe:0");
    CHECK(a[index_of(a, "-s") + 1] == "1920x1080");
    CHECK(a[index_of(a, "-r") + 1] == "15");
    CHECK(has(a, "rawvideo"));
    CHECK(has(a, "h264_nvenc"));
    CHECK(has(a, "dump_extra=freq=keyframe"));
    CHECK(a[a.size() - 3] == "-f");
    CHECK(a[a.size() - 2] == "h264");
    CHECK(a.back() == "pipe:1");
    CHECK(a[index_of(a, "-g") + 1] == "15"); // a keyframe every second at 15 fps
    CHECK(a[index_of(a, "-bf") + 1] == "0");
}

TEST_CASE("mosaic: cells are written into and copied out of the canvas", "[HubMosaic]")
{
    const Size canvas{ 8, 4 };
    Layout     l;
    parse_layout("2x2", l);
    const Rect r = cell_rect(l, canvas, 3); // x 4, y 2, 4 x 2
    REQUIRE(r.x == 4);
    REQUIRE(r.y == 2);
    std::vector<unsigned char> frame(i420_size(8, 4), 0);
    // A 4x2 cell: Y 1..8, U 101 102, V 201 202.
    const std::vector<unsigned char> cell = { 1, 2, 3, 4, 5, 6, 7, 8, 101, 102, 201, 202 };
    REQUIRE(cell.size() == i420_size(4, 2));
    blit_i420(frame.data(), canvas, r, cell.data());
    CHECK(frame[2 * 8 + 4] == 1);
    CHECK(frame[2 * 8 + 7] == 4);
    CHECK(frame[3 * 8 + 4] == 5);
    CHECK(frame[3 * 8 + 7] == 8);
    CHECK(frame[0] == 0);
    // U plane 4x2 at offset 32: row 1, columns 2..3; V at offset 40.
    CHECK(frame[32 + 1 * 4 + 2] == 101);
    CHECK(frame[32 + 1 * 4 + 3] == 102);
    CHECK(frame[40 + 1 * 4 + 2] == 201);
    CHECK(frame[40 + 1 * 4 + 3] == 202);
    CHECK(frame[32] == 0);

    std::vector<unsigned char> copy(i420_size(8, 4), 9);
    copy_cell_i420(copy.data(), frame.data(), canvas, r);
    CHECK(copy[2 * 8 + 4] == 1);
    CHECK(copy[3 * 8 + 7] == 8);
    CHECK(copy[32 + 1 * 4 + 3] == 102);
    CHECK(copy[40 + 1 * 4 + 2] == 201);
    CHECK(copy[0] == 9); // outside the cell: untouched
    CHECK(copy[32] == 9);
}

TEST_CASE("mosaic: a late reader starts at the latest keyframe", "[HubMosaic]")
{
    GopCache          cache;
    const std::string p("\x00\x00\x01\x41\xAA", 5);                         // a P-slice
    const std::string key1("\x00\x00\x00\x01\x67\x42\x00\x00\x01\x65\x01", 11); // SPS + IDR
    const std::string key2("\x00\x00\x00\x01\x67\x43\x00\x00\x01\x65\x02", 11);
    cache.feed(p.data(), p.size());
    CHECK(cache.data().empty());
    CHECK_FALSE(cache.keyframes_seen());
    cache.feed(key1.data(), key1.size());
    CHECK(cache.keyframes_seen());
    CHECK(cache.data() == key1);
    cache.feed(p.data(), p.size());
    CHECK(cache.data() == key1 + p);
    // The next keyframe, its start code split across two reads: the cache starts over at it.
    cache.feed(key2.data(), 3);
    cache.feed(key2.data() + 3, key2.size() - 3);
    CHECK(cache.data() == key2);
    // Past the cap without a keyframe it holds nothing rather than an unbounded GOP.
    GopCache small(16);
    small.feed(key1.data(), key1.size());
    small.feed(p.data(), p.size());
    CHECK(small.data().size() == 16);
    small.feed(p.data(), p.size());
    CHECK(small.data().empty());
    small.feed(key2.data(), key2.size());
    CHECK(small.data() == key2);
}

TEST_CASE("mosaic: a failed camera is tried again with backoff", "[HubMosaic]")
{
    CHECK(retry_delay_ms(1) == 3000);
    CHECK(retry_delay_ms(2) == 6000);
    CHECK(retry_delay_ms(3) == 12000);
    CHECK(retry_delay_ms(4) == 24000);
    CHECK(retry_delay_ms(5) == 30000);
    CHECK(retry_delay_ms(50) == 30000);
}

TEST_CASE("mosaic: a new page is a different spec, the same page is not", "[HubMosaic]")
{
    const Spec a = page_2x2();
    Spec       b = page_2x2();
    CHECK(a.same_page(b));
    b.cells[1].id = "c2";
    CHECK_FALSE(a.same_page(b));
    Spec c = page_2x2();
    c.cells[2].note = "Offline";
    CHECK_FALSE(a.same_page(c));
}

TEST_CASE("mosaic: an Elegoo relay at its viewer limit is a placeholder", "[HubMosaic]")
{
    CHECK(elegoo_note("busy") == "Viewer limit reached");
    CHECK(elegoo_note("no_camera") == "No camera");
    CHECK(elegoo_note("failed") == "Offline");
    CHECK(elegoo_note("live").empty());
    CHECK(elegoo_note("idle").empty());
    CHECK(elegoo_note("").empty());
}

TEST_CASE("mosaic: the stream is passed on from its first SPS", "[HubMosaic]")
{
    H264StartGate gate;
    std::string   out;
    const std::string slice("\x00\x00\x01\x41\xAA\xBB", 6);    // a P-slice: dropped
    const std::string sps("\x00\x00\x00\x01\x67\x42\x00\x1f", 8); // SPS (4-byte start code)
    gate.feed(slice.data(), slice.size(), out);
    CHECK(out.empty());
    CHECK_FALSE(gate.started());
    // An SPS split across two reads is still found.
    gate.feed(sps.data(), 3, out);
    CHECK(out.empty());
    gate.feed(sps.data() + 3, sps.size() - 3, out);
    CHECK(gate.started());
    CHECK(out == sps);
    out.clear();
    gate.feed(slice.data(), slice.size(), out);
    CHECK(out == slice); // everything after it passes untouched
    gate.reset();
    CHECK_FALSE(gate.started());
}
