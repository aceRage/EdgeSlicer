#include "HubMosaic.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace Slic3r {
namespace HubMosaic {

bool parse_layout(const std::string& text, Layout& out)
{
    if (text.size() != 3 || (text[1] != 'x' && text[1] != 'X')) return false;
    const int c = text[0] - '0', r = text[2] - '0';
    if (c < 1 || c > 3 || r < 1 || r > 3) return false;
    out.cols = c;
    out.rows = r;
    return true;
}

static bool id_char(unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == ':' || c == '-'; }

std::vector<std::string> parse_ids(const std::string& csv, size_t max)
{
    std::vector<std::string> out;
    std::set<std::string>    seen;
    std::string              cur;
    auto flush = [&]() {
        size_t a = 0, b = cur.size();
        while (a < b && cur[a] == ' ') ++a;
        while (b > a && cur[b - 1] == ' ') --b;
        std::string id = cur.substr(a, b - a);
        cur.clear();
        if (id.empty() || id.size() > 64 || out.size() >= max) return;
        for (unsigned char c : id)
            if (!id_char(c)) return;
        if (seen.insert(id).second) out.push_back(id);
    };
    for (char c : csv) {
        if (c == ',') flush();
        else cur += c;
    }
    flush();
    return out;
}

Size output_size(int height)
{
    if (height == 720) return Size{ 1280, 720 };
    return Size{ 1920, 1080 };
}

int clamp_fps(int fps)
{
    if (fps <= 0) return 15;
    return std::min(30, std::max(5, fps));
}

Rect cell_rect(const Layout& layout, const Size& size, int index)
{
    const int cols = std::max(1, layout.cols), rows = std::max(1, layout.rows);
    const int cw = (size.w / cols) & ~1, ch = (size.h / rows) & ~1;
    const int col = index % cols, row = index / cols;
    Rect r;
    r.x = col * cw;
    r.y = row * ch;
    r.w = col == cols - 1 ? size.w - r.x : cw;
    r.h = row == rows - 1 ? size.h - r.y : ch;
    r.w &= ~1;
    r.h &= ~1;
    return r;
}

const char* encoder_name(Encoder e)
{
    switch (e) {
    case Encoder::Nvenc: return "h264_nvenc";
    case Encoder::Qsv: return "h264_qsv";
    case Encoder::Amf: return "h264_amf";
    case Encoder::VideoToolbox: return "h264_videotoolbox";
    case Encoder::MediaFoundation: return "h264_mf";
    case Encoder::OpenH264: return "libopenh264";
    case Encoder::X264: return "libx264";
    default: return "";
    }
}

std::vector<Encoder> encoder_preference(const std::string& listing)
{
    // A listing line is " V....D h264_nvenc   NVIDIA NVENC ...": the name is the second field.
    std::set<std::string> names;
    std::istringstream    is(listing);
    std::string           line;
    while (std::getline(is, line)) {
        std::istringstream ls(line);
        std::string flags, name;
        if (ls >> flags >> name) names.insert(name);
    }
    std::vector<Encoder> out;
    for (Encoder e : { Encoder::Nvenc, Encoder::Qsv, Encoder::Amf, Encoder::VideoToolbox, Encoder::MediaFoundation,
                       Encoder::OpenH264, Encoder::X264 })
        if (names.count(encoder_name(e))) out.push_back(e);
    return out;
}

std::vector<std::string> encoder_args(Encoder e, int kbps)
{
    const std::string b = std::to_string(std::max(300, kbps)) + "k";
    switch (e) {
    case Encoder::Nvenc:
        return { "-c:v", "h264_nvenc", "-preset", "p3", "-tune", "ll", "-rc", "cbr", "-b:v", b, "-maxrate", b, "-bufsize", b,
                 "-profile:v", "baseline", "-zerolatency", "1", "-delay", "0" };
    case Encoder::Qsv:
        return { "-c:v", "h264_qsv", "-preset", "veryfast", "-b:v", b, "-maxrate", b, "-profile:v", "baseline",
                 "-look_ahead", "0", "-async_depth", "1" };
    case Encoder::Amf:
        return { "-c:v", "h264_amf", "-usage", "lowlatency", "-quality", "speed", "-rc", "cbr", "-b:v", b, "-maxrate", b,
                 "-profile:v", "constrained_baseline" };
    case Encoder::VideoToolbox:
        return { "-c:v", "h264_videotoolbox", "-realtime", "1", "-b:v", b, "-profile:v", "baseline" };
    case Encoder::MediaFoundation:
        return { "-c:v", "h264_mf", "-hw_encoding", "1", "-rate_control", "cbr", "-scenario", "display_remoting", "-b:v", b };
    case Encoder::X264:
        return { "-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency", "-profile:v", "baseline", "-b:v", b,
                 "-maxrate", b, "-bufsize", b };
    case Encoder::OpenH264:
    default:
        return { "-c:v", "libopenh264", "-profile:v", "constrained_baseline", "-rc_mode", "bitrate", "-b:v", b, "-maxrate", b };
    }
}

int default_kbps(const Size& size, int fps, const std::string& quality)
{
    // ~0.13 bits per pixel per frame: 1080p15 is 4 Mbps, 1080p30 8 Mbps, 720p15 1.8 Mbps. A wall of
    // small cells is mostly static printers, so this is generous rather than tight.
    const double px    = (double) size.w * size.h * clamp_fps(fps);
    double       kbps  = px * 0.13 / 1000.0;
    if (quality == "low") kbps *= 0.5;
    else if (quality == "high") kbps *= 1.5;
    return std::max(500, (int) (kbps / 100.0 + 0.5) * 100);
}

bool Spec::same_page(const Spec& o) const
{
    if (layout.cols != o.layout.cols || layout.rows != o.layout.rows || size.w != o.size.w || size.h != o.size.h ||
        fps != o.fps || kbps != o.kbps || encoder != o.encoder || cells.size() != o.cells.size())
        return false;
    for (size_t i = 0; i < cells.size(); ++i) {
        const Cell &a = cells[i], &b = o.cells[i];
        if (a.id != b.id || a.kind != b.kind || a.url != b.url || a.label != b.label || a.note != b.note) return false;
    }
    return true;
}

std::string sanitize_text(const std::string& text)
{
    std::string out;
    for (unsigned char c : text) {
        if (out.size() >= 40) break;
        if (std::isalnum(c) || c == ' ' || c == '.' || c == '-' || c == '_' || c == '(' || c == ')' || c == '#' || c == '+' ||
            c == '/' || c == '!' || c == '?' || c == '&' || c == '@')
            out += (char) c;
        else if (c >= 0x80 || c == '\t')
            continue;
        else
            out += ' ';
    }
    size_t a = 0, b = out.size();
    while (a < b && out[a] == ' ') ++a;
    while (b > a && out[b - 1] == ' ') --b;
    return out.substr(a, b - a);
}

std::string font_value(const std::string& path)
{
    if (path.empty()) return "";
    std::string p;
    for (char c : path) {
        if (c == '\'' || c == ',' || c == ';' || c == '[' || c == ']' || c == '%' || c == '\n' || c == '\r') return "";
        if (c == '\\') p += '/';
        else if (c == ':') p += "\\:";
        else p += c;
    }
    return "'" + p + "'";
}

static std::string drawtext(const std::string& font, const std::string& text, int size, bool center)
{
    const std::string t = sanitize_text(text);
    if (font.empty() || t.empty()) return "";
    std::string s = ",drawtext=fontfile=" + font + ":text='" + t + "':fontcolor=white:fontsize=" + std::to_string(size);
    if (center) s += ":x=(w-tw)/2:y=(h-th)/2";
    else s += ":box=1:boxcolor=black@0.55:boxborderw=6:x=10:y=h-th-10";
    return s;
}

std::string build_filter(const Spec& spec)
{
    const std::string font  = font_value(spec.font);
    const int         n     = spec.layout.cells();
    const std::string fps   = std::to_string(clamp_fps(spec.fps));
    std::ostringstream g;
    int                input = 0;
    bool               live  = false;
    for (int i = 0; i < n; ++i) {
        const Cell  cell = i < (int) spec.cells.size() ? spec.cells[i] : Cell{};
        const Rect  r    = cell_rect(spec.layout, spec.size, i);
        const std::string wh = std::to_string(r.w) + ":" + std::to_string(r.h);
        // Label size: readable on a TV at 3x3 without covering a 1x1.
        const int label_px = std::max(14, std::min(32, r.h / 14));
        if (i) g << ";";
        if (cell.kind == CellKind::Rtsp || cell.kind == CellKind::Mjpeg) {
            live = true;
            g << "[" << input++ << ":v]setpts=PTS-STARTPTS,fps=" << fps << ",scale=" << wh
              << ":force_original_aspect_ratio=decrease,pad=" << wh << ":(ow-iw)/2:(oh-ih)/2:color=black,setsar=1,format=yuv420p"
              << drawtext(font, cell.label, label_px, false) << "[c" << i << "]";
        } else if (cell.kind == CellKind::Placeholder) {
            g << "color=c=0x22222a:s=" << r.w << "x" << r.h << ":r=" << fps << ",format=yuv420p"
              << drawtext(font, cell.note, std::max(16, std::min(40, r.h / 10)), true)
              << drawtext(font, cell.label, label_px, false) << "[c" << i << "]";
        } else {
            g << "color=c=black:s=" << r.w << "x" << r.h << ":r=" << fps << ",format=yuv420p[c" << i << "]";
        }
    }
    g << ";";
    if (n == 1) {
        g << "[c0]" << (live ? "null" : "realtime") << "[out]";
        return g.str();
    } else {
        for (int i = 0; i < n; ++i) g << "[c" << i << "]";
        g << "xstack=inputs=" << n << ":layout=";
        for (int i = 0; i < n; ++i) {
            const Rect r = cell_rect(spec.layout, spec.size, i);
            if (i) g << "|";
            g << r.x << "_" << r.y;
        }
        g << ":fill=black";
    }
    // Without a live camera nothing paces the graph: `color` alone would run as fast as the CPU
    // allows. realtime holds it to the clock; behind live inputs it never has to wait.
    g << (live ? ",null" : ",realtime") << "[out]";
    return g.str();
}

std::vector<std::string> feeder_args(const std::string& ffmpeg, const Cell& cell, const Rect& r, const std::string& font)
{
    std::vector<std::string> a = { ffmpeg, "-hide_banner", "-nostdin", "-nostats", "-loglevel", "error" };
    if (cell.kind == CellKind::Rtsp) a.insert(a.end(), { "-rtsp_transport", "tcp", "-timeout", "5000000" });
    else a.insert(a.end(), { "-f", "mpjpeg", "-rw_timeout", "5000000" });
    // No input buffering, no frame-threaded decoding (low_delay: frame threads hold back a frame
    // per thread), and a probe that ends at the first frame with its parameters, capped at 1 s.
    a.insert(a.end(), { "-fflags", "nobuffer", "-flags", "low_delay", "-probesize", "500000", "-analyzeduration", "1000000",
                        "-i", cell.url, "-an" });
    const std::string wh       = std::to_string(r.w) + ":" + std::to_string(r.h);
    const int         label_px = std::max(14, std::min(32, r.h / 14));
    a.insert(a.end(), { "-vf",
                        "scale=" + wh + ":force_original_aspect_ratio=decrease,pad=" + wh +
                            ":(ow-iw)/2:(oh-ih)/2:color=black,setsar=1,format=yuv420p" +
                            drawtext(font_value(font), cell.label, label_px, false),
                        "-f", "rawvideo", "-pix_fmt", "yuv420p", "pipe:1" });
    return a;
}

std::vector<std::string> card_args(const std::string& ffmpeg, const Spec& spec, const std::string& live_note)
{
    Spec cards = spec;
    for (Cell& c : cards.cells)
        if (c.kind == CellKind::Rtsp || c.kind == CellKind::Mjpeg) {
            c.kind = CellKind::Placeholder;
            c.note = live_note;
            c.url.clear();
        }
    return { ffmpeg, "-hide_banner", "-nostdin", "-nostats", "-loglevel", "error", "-filter_complex", build_filter(cards),
             "-map", "[out]", "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "yuv420p", "pipe:1" };
}

std::vector<std::string> encoder_args_raw(const std::string& ffmpeg, const Spec& spec)
{
    const int fps = clamp_fps(spec.fps);
    // No -nostdin: stdin is the input (ffmpeg then never reads keys from it anyway).
    std::vector<std::string> a = { ffmpeg, "-hide_banner", "-nostats", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "yuv420p",
                                   "-s", std::to_string(spec.size.w) + "x" + std::to_string(spec.size.h), "-r", std::to_string(fps),
                                   "-i", "pipe:0", "-an" };
    const std::vector<std::string> enc = encoder_args(spec.encoder, spec.kbps);
    a.insert(a.end(), enc.begin(), enc.end());
    // A keyframe every second, SPS/PPS in front of each (a viewer joining mid-stream starts at the
    // next one, or at once from the hub's GopCache), no B-frames, raw Annex B on stdout, flushed
    // per packet.
    a.insert(a.end(), { "-g", std::to_string(fps), "-bf", "0", "-bsf:v", "dump_extra=freq=keyframe", "-flush_packets", "1",
                        "-f", "h264", "pipe:1" });
    return a;
}

void copy_cell_i420(unsigned char* dst, const unsigned char* src, const Size& canvas, const Rect& r)
{
    const size_t W = (size_t) canvas.w, H = (size_t) canvas.h;
    for (int y = 0; y < r.h; ++y) {
        const size_t o = (size_t) (r.y + y) * W + r.x;
        std::copy(src + o, src + o + r.w, dst + o);
    }
    const size_t cw = (size_t) r.w / 2, uo = W * H, vo = W * H + W * H / 4;
    for (int y = 0; y < r.h / 2; ++y) {
        const size_t o = (size_t) (r.y / 2 + y) * (W / 2) + r.x / 2;
        std::copy(src + uo + o, src + uo + o + cw, dst + uo + o);
        std::copy(src + vo + o, src + vo + o + cw, dst + vo + o);
    }
}

void blit_i420(unsigned char* dst, const Size& canvas, const Rect& r, const unsigned char* cell)
{
    const size_t W = (size_t) canvas.w, H = (size_t) canvas.h, w = (size_t) r.w, h = (size_t) r.h;
    for (size_t y = 0; y < h; ++y) std::copy(cell + y * w, cell + y * w + w, dst + (r.y + y) * W + r.x);
    const size_t cw = w / 2, uo = W * H, vo = W * H + W * H / 4, su = w * h, sv = w * h + w * h / 4;
    for (size_t y = 0; y < h / 2; ++y) {
        const size_t o = (r.y / 2 + y) * (W / 2) + r.x / 2;
        std::copy(cell + su + y * cw, cell + su + y * cw + cw, dst + uo + o);
        std::copy(cell + sv + y * cw, cell + sv + y * cw + cw, dst + vo + o);
    }
}

void GopCache::feed(const char* data, size_t n)
{
    if (n == 0) return;
    // Look for the last SPS start code in what came before (its last 4 bytes) plus this chunk.
    const std::string scan = m_tail + std::string(data, n);
    long long sps = -1;
    for (size_t k = 0; k + 3 < scan.size(); ++k)
        if (scan[k] == 0 && scan[k + 1] == 0 && scan[k + 2] == 1 && ((unsigned char) scan[k + 3] & 0x1f) == 7)
            sps = (long long) ((k > 0 && scan[k - 1] == 0) ? k - 1 : k);
    if (sps >= 0) {
        m_data.assign(scan, (size_t) sps, std::string::npos);
        m_open = true;
        m_seen = true;
    } else if (m_open) {
        m_data.append(data, n);
        if (m_data.size() > m_cap) {
            m_data.clear();
            m_open = false;
        }
    }
    m_tail = scan.size() > 4 ? scan.substr(scan.size() - 4) : scan;
}

int retry_delay_ms(int failures)
{
    if (failures <= 1) return 3000;
    if (failures >= 5) return 30000;
    return std::min(30000, 3000 << (failures - 1));
}

std::string elegoo_note(const std::string& state)
{
    if (state == "busy") return "Viewer limit reached";
    if (state == "no_camera") return "No camera";
    if (state == "failed") return "Offline";
    return "";
}

void H264StartGate::feed(const char* data, size_t n, std::string& out)
{
    if (m_started) {
        out.append(data, n);
        return;
    }
    m_pend.append(data, n);
    size_t at = std::string::npos;
    for (size_t k = 0; k + 3 < m_pend.size(); ++k)
        if (m_pend[k] == 0 && m_pend[k + 1] == 0 && m_pend[k + 2] == 1 && ((unsigned char) m_pend[k + 3] & 0x1f) == 7) {
            at = (k > 0 && m_pend[k - 1] == 0) ? k - 1 : k;
            break;
        }
    if (at == std::string::npos) {
        if (m_pend.size() > 4) m_pend.erase(0, m_pend.size() - 4);
        return;
    }
    m_started = true;
    out.append(m_pend, at, std::string::npos);
    m_pend.clear();
}

} // namespace HubMosaic
} // namespace Slic3r
