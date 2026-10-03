#ifndef slic3r_HubMedia_hpp_
#define slic3r_HubMedia_hpp_

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// The hub's camera policy, as pure rules (no processes, sockets or clocks) so
// tests/slic3rutils/hub_media_tests.cpp can drive them with made-up hosts and a mocked probe:
//
//   * PASSTHROUGH IS THE DEFAULT. A camera stream is relayed as the printer sends it. The phone's
//     Medium/Low steps are re-encodes (go2rtc running ffmpeg), opt-in and started lazily by go2rtc
//     when a viewer opens them; they are only OFFERED when a real test encode succeeded.
//   * WHICH ENCODER: never trust `ffmpeg -encoders` alone. Candidates are tried in a fixed order and
//     each one is probed by actually encoding a second of test video: x86 QSV, then VAAPI (both need
//     /dev/dri), ARM `h264_v4l2m2m` (Pi 4, /dev/video11), then software libx264/libopenh264 - software
//     only on an x86 host with enough cores; an ARM host without a hardware encoder gets ONE software
//     transcode at the Low step.
//   * HOW MANY AT ONCE: a cap on concurrent transcodes (per distinct stream; viewers of the same
//     variant share one ffmpeg). Past the cap a viewer is served the passthrough stream, never an
//     error (TranscodeGate). Streams stop when the last viewer leaves (go2rtc ends the exec source).
//   * PORTS for the go2rtc listeners are configurable, with conflict handling (resolve_port).
namespace Slic3r {
namespace HubMedia {

// ---- the host ---------------------------------------------------------------------------------

enum class Os { Windows, Linux, Mac, Other };
enum class Arch { X86, Arm, Other };

struct Host
{
    Os   os { Os::Linux };
    Arch arch { Arch::X86 };
    int  cores { 1 };
    bool dri_render { false }; // /dev/dri/renderD128 exists (QSV / VAAPI possible)
    bool video11 { false };    // /dev/video11 exists (Pi 4 hardware H.264 encoder)
};

// Below this many cores a software transcode is not offered on an x86 host: the hub must stay
// responsive for the slicer instance that shares the machine.
constexpr int MIN_SOFTWARE_CORES_X86 = 4;

// ---- encoders ---------------------------------------------------------------------------------

enum class Encoder { None, Qsv, Vaapi, V4l2m2m, X264, OpenH264 };

inline const char* encoder_label(Encoder e)
{
    switch (e) {
    case Encoder::Qsv:      return "qsv";
    case Encoder::Vaapi:    return "vaapi";
    case Encoder::V4l2m2m:  return "v4l2m2m";
    case Encoder::X264:     return "libx264";
    case Encoder::OpenH264: return "libopenh264";
    default:                return "none";
    }
}

inline bool is_hardware(Encoder e) { return e == Encoder::Qsv || e == Encoder::Vaapi || e == Encoder::V4l2m2m; }

// The names of the video encoders in `ffmpeg -hide_banner -encoders` output: a line is six flag
// characters starting with V, then the name.
inline std::set<std::string> encoder_names(const std::string& encoders_text)
{
    std::set<std::string> out;
    size_t pos = 0;
    while (pos < encoders_text.size()) {
        size_t nl = encoders_text.find('\n', pos);
        if (nl == std::string::npos) nl = encoders_text.size();
        std::string line = encoders_text.substr(pos, nl - pos);
        pos              = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t i = 0;
        while (i < line.size() && line[i] == ' ') ++i;
        const size_t flags_at = i;
        while (i < line.size() && line[i] != ' ') ++i;
        if (i - flags_at != 6 || line[flags_at] != 'V') continue;
        while (i < line.size() && line[i] == ' ') ++i;
        const size_t name_at = i;
        while (i < line.size() && line[i] != ' ') ++i;
        if (i > name_at) out.insert(line.substr(name_at, i - name_at));
    }
    return out;
}

// One thing to try. `global_args` go on go2rtc's ffmpeg command before the input (device set-up),
// `template_args` are the encoder options (go2rtc's `h264` template), `filter_tail` is appended to
// the scale filter for encoders that take hardware frames (empty for the others).
struct Candidate
{
    Encoder                  encoder { Encoder::None };
    std::vector<std::string> global_args;
    std::string              template_args;
    std::string              filter_tail;
    bool                     software { false };
};

inline Candidate make_candidate(Encoder e)
{
    Candidate c;
    c.encoder = e;
    switch (e) {
    case Encoder::Qsv:
        c.global_args   = { "-init_hw_device", "qsv=hw", "-filter_hw_device", "hw" };
        c.template_args = "-codec:v h264_qsv -profile:v main -bf 0 -look_ahead 0";
        c.filter_tail   = "format=nv12,hwupload=extra_hw_frames=16";
        break;
    case Encoder::Vaapi:
        c.global_args   = { "-vaapi_device", "/dev/dri/renderD128" };
        c.template_args = "-codec:v h264_vaapi -profile:v main -bf 0";
        c.filter_tail   = "format=nv12,hwupload";
        break;
    case Encoder::V4l2m2m:
        c.template_args = "-codec:v h264_v4l2m2m -pix_fmt yuv420p -bf 0";
        break;
    case Encoder::X264:
        c.template_args = "-codec:v libx264 -g:v 30 -preset:v superfast -tune:v zerolatency -profile:v main -level:v 4.1";
        c.software      = true;
        break;
    case Encoder::OpenH264:
        c.template_args = "-codec:v libopenh264 -profile:v constrained_baseline -rc_mode bitrate -bf 0";
        c.software      = true;
        break;
    default: break;
    }
    return c;
}

// The candidates for this host, in the order they are probed. `listed` is what the ffmpeg says it has
// (necessary, not sufficient: every candidate is still test-encoded).
inline std::vector<Candidate> candidates(const Host& h, const std::set<std::string>& listed)
{
    auto has = [&](const char* n) { return listed.count(n) != 0; };
    std::vector<Candidate> out;
    const bool linux_os = h.os == Os::Linux;
    if (linux_os && h.arch == Arch::X86) {
        if (h.dri_render && has("h264_qsv")) out.push_back(make_candidate(Encoder::Qsv));
        if (h.dri_render && has("h264_vaapi")) out.push_back(make_candidate(Encoder::Vaapi));
    }
    if (linux_os && h.arch == Arch::Arm && h.video11 && has("h264_v4l2m2m")) out.push_back(make_candidate(Encoder::V4l2m2m));
    // Software. On x86 Linux only with enough cores; on ARM Linux always (the cap and the Low-only
    // step keep it to one cheap transcode); elsewhere (Windows, the existing behaviour) as before.
    const bool software_ok = !(linux_os && h.arch == Arch::X86) || h.cores >= MIN_SOFTWARE_CORES_X86;
    if (software_ok) {
        if (has("libx264")) out.push_back(make_candidate(Encoder::X264));
        if (has("libopenh264")) out.push_back(make_candidate(Encoder::OpenH264));
    }
    return out;
}

// The arguments of one test encode: a second of synthetic video through the candidate's real chain.
// Exit status 0 means the encoder works on this host with these devices.
inline std::vector<std::string> test_encode_args(const std::string& ffmpeg, const Candidate& c)
{
    std::vector<std::string> a = { ffmpeg, "-hide_banner", "-v", "error" };
    a.insert(a.end(), c.global_args.begin(), c.global_args.end());
    a.insert(a.end(), { "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=15", "-frames:v", "15" });
    std::string vf = "scale=320:-2";
    if (!c.filter_tail.empty()) vf += "," + c.filter_tail;
    a.insert(a.end(), { "-vf", vf });
    // the template, split on spaces (go2rtc splits it the same way)
    size_t pos = 0;
    while (pos < c.template_args.size()) {
        size_t sp = c.template_args.find(' ', pos);
        if (sp == std::string::npos) sp = c.template_args.size();
        if (sp > pos) a.push_back(c.template_args.substr(pos, sp - pos));
        pos = sp + 1;
    }
    a.insert(a.end(), { "-f", "null", "-" });
    return a;
}

// ---- the decision -------------------------------------------------------------------------------

struct ProbeLine
{
    std::string encoder;
    bool        ok { false };
};

struct Choice
{
    Candidate                encoder;               // encoder == None: passthrough only
    std::vector<std::string> quality;               // the steps offered: any of "med", "low"; empty = passthrough only
    int                      default_cap { 0 };     // concurrent transcodes; 0 = unlimited
    std::vector<ProbeLine>   probes;                // what was tried, in order
    std::string              reason;                // one sentence for the log and the hub page
};

// The steps an encoder on this host is offered at. An ARM host running software gets Low only.
inline std::vector<std::string> quality_steps(const Host& h, const Candidate& c)
{
    if (c.encoder == Encoder::None) return {};
    if (c.software && h.os == Os::Linux && h.arch != Arch::X86) return { "low" };
    return { "med", "low" };
}

// Concurrent transcodes when the operator has not said. Windows and macOS keep what they always
// had: no cap.
inline int default_cap(const Host& h, const Candidate& c)
{
    if (c.encoder == Encoder::None) return 0;
    if (h.os != Os::Linux) return 0;
    if (h.arch == Arch::X86) {
        if (is_hardware(c.encoder)) return 6;
        return std::max(1, std::min(4, h.cores / 2));
    }
    if (is_hardware(c.encoder)) return 2;
    return 1; // ARM (Pi class) in software
}

using ProbeFn = std::function<bool(const Candidate&)>;

inline Choice choose(const Host& h, const std::set<std::string>& listed, const ProbeFn& probe)
{
    Choice ch;
    const std::vector<Candidate> list = candidates(h, listed);
    for (const Candidate& c : list) {
        const bool ok = probe(c);
        ch.probes.push_back({ encoder_label(c.encoder), ok });
        if (ok) {
            ch.encoder     = c;
            ch.quality     = quality_steps(h, c);
            ch.default_cap = default_cap(h, c);
            ch.reason      = std::string(encoder_label(c.encoder)) + (is_hardware(c.encoder) ? " (hardware)" : " (software)") + " passed a test encode";
            return ch;
        }
    }
    ch.reason = list.empty() ? "no usable H.264 encoder for this host: cameras are relayed as they come (passthrough only)"
                             : "no encoder passed a test encode: cameras are relayed as they come (passthrough only)";
    return ch;
}

// EDGESLICER_MAX_TRANSCODES / --hub-max-transcodes: 0 = unlimited, 1..32, anything else = the default.
inline int parse_cap(const std::string& text, int fallback)
{
    if (text.empty()) return fallback;
    for (char c : text)
        if (!std::isdigit((unsigned char) c)) return fallback;
    if (text.size() > 3) return fallback;
    const int n = std::atoi(text.c_str());
    return n > 32 ? fallback : n;
}

// ---- the cap, enforced where a viewer asks for a stream ------------------------------------------

// A viewer asks go2rtc for `<base>_med` or `<base>_low`. If that variant is already running (another
// viewer) it is shared; if a slot is free it takes it; otherwise the viewer is handed `<base>`, the
// passthrough stream - the picture is full quality, not a failure. A slot is held until the last
// viewer of that variant is gone.
class TranscodeGate
{
public:
    explicit TranscodeGate(int cap = 0) : m_cap(cap) {}

    struct Ticket
    {
        std::string served;           // the stream name to hand to go2rtc
        bool        transcode { false }; // holds (a share of) a slot
        bool        downgraded { false }; // asked for a variant, got the source
    };

    void set_cap(int cap) { std::lock_guard<std::mutex> l(m_mutex); m_cap = cap; }
    int  cap() const { std::lock_guard<std::mutex> l(m_mutex); return m_cap; }

    Ticket admit(const std::string& requested, bool is_variant, const std::string& base)
    {
        std::lock_guard<std::mutex> l(m_mutex);
        Ticket t;
        t.served = requested;
        if (!is_variant) return t;
        auto it = m_active.find(requested);
        if (it != m_active.end()) { ++it->second; t.transcode = true; return t; }
        if (m_cap > 0 && (int) m_active.size() >= m_cap) {
            t.served     = base;
            t.downgraded = true;
            ++m_downgraded;
            return t;
        }
        m_active[requested] = 1;
        t.transcode         = true;
        return t;
    }

    void release(const Ticket& t)
    {
        if (!t.transcode) return;
        std::lock_guard<std::mutex> l(m_mutex);
        auto it = m_active.find(t.served);
        if (it == m_active.end()) return;
        if (--it->second <= 0) m_active.erase(it);
    }

    int active() const { std::lock_guard<std::mutex> l(m_mutex); return (int) m_active.size(); }
    int downgraded_total() const { std::lock_guard<std::mutex> l(m_mutex); return m_downgraded; }

private:
    mutable std::mutex         m_mutex;
    int                        m_cap { 0 };
    std::map<std::string, int> m_active;
    int                        m_downgraded { 0 };
};

// "name_med" -> {"name", "med"}; anything that does not end in a step we offer is not a variant.
struct VariantName
{
    std::string base;
    std::string step;
    bool        is_variant { false };
};

inline VariantName split_variant(const std::string& stream, const std::vector<std::string>& steps)
{
    VariantName v;
    v.base = stream;
    for (const std::string& s : steps) {
        const std::string suffix = "_" + s;
        if (stream.size() > suffix.size() && stream.compare(stream.size() - suffix.size(), suffix.size(), suffix) == 0) {
            v.base       = stream.substr(0, stream.size() - suffix.size());
            v.step       = s;
            v.is_variant = true;
            return v;
        }
    }
    return v;
}

// A request head's first line is `GET /api/ws?src=name_low&x=1 HTTP/1.1`; give one query parameter a new
// value (percent-encoded by the caller), leaving the rest of the head alone. Returns the head unchanged
// when the parameter is not there.
inline std::string replace_query_param(const std::string& head, const std::string& key, const std::string& value)
{
    const size_t eol = head.find("\r\n");
    const std::string line = head.substr(0, eol == std::string::npos ? head.size() : eol);
    const size_t q = line.find('?');
    const size_t sp2 = line.rfind(' ');
    if (q == std::string::npos || sp2 == std::string::npos || sp2 < q) return head;
    const std::string query = line.substr(q + 1, sp2 - q - 1);
    std::string       out_q;
    bool              changed = false;
    size_t            pos     = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string part = query.substr(pos, amp - pos);
        pos                    = amp + 1;
        if (part.empty()) continue;
        if (part.compare(0, key.size() + 1, key + "=") == 0) { out_q += (out_q.empty() ? "" : "&") + key + "=" + value; changed = true; }
        else out_q += (out_q.empty() ? "" : "&") + part;
    }
    if (!changed) return head;
    return line.substr(0, q + 1) + out_q + line.substr(sp2) + (eol == std::string::npos ? "" : head.substr(eol));
}

// ---- ports ----------------------------------------------------------------------------------------

enum class PortStatus { Ok, Moved, Failed };

struct PortResult
{
    int        port { 0 };
    PortStatus status { PortStatus::Failed };
    int        asked { 0 };
};

// A port setting: digits, 1..65535; anything else (including empty) is "not set" (0).
inline int parse_port(const std::string& text)
{
    if (text.empty() || text.size() > 5) return 0;
    for (char c : text)
        if (!std::isdigit((unsigned char) c)) return 0;
    const int n = std::atoi(text.c_str());
    return (n >= 1 && n <= 65535) ? n : 0;
}

// The environment (which --hub-... flags also set) beats the hub's settings file beats nothing.
inline int configured_port(const std::string& env_text, int settings_value)
{
    const int e = parse_port(env_text);
    if (e > 0) return e;
    return settings_value >= 1 && settings_value <= 65535 ? settings_value : 0;
}

// `requested` > 0 is the operator's choice: if it is taken, a desktop hub steps to the next free port
// (Moved) and says so; a service-mode hub does not guess - it fails (Failed) so the operator hears
// about it. `requested` == 0 scans first..last for a free one (the automatic WebRTC range).
inline PortResult resolve_port(int requested, int first, int last, bool service, const std::function<bool(int)>& is_free)
{
    PortResult r;
    r.asked = requested;
    if (requested > 0) {
        if (is_free(requested)) { r.port = requested; r.status = PortStatus::Ok; return r; }
        if (service) return r;
        for (int p = requested + 1; p <= std::min(requested + 20, 65535); ++p)
            if (is_free(p)) { r.port = p; r.status = PortStatus::Moved; return r; }
        return r;
    }
    for (int p = first; p <= last; ++p)
        if (is_free(p)) { r.port = p; r.status = PortStatus::Ok; return r; }
    return r;
}

// ---- the RTSP restream ------------------------------------------------------------------------------

// go2rtc's RTSP listener is loopback-only by default. Another program on the same machine (Home
// Assistant or Frigate with host networking) can read the relay there. A listener on other interfaces
// is only allowed with credentials - an open camera stream on the LAN is not something to switch on by
// a typo.
struct RtspPlan
{
    std::string listen_host { "127.0.0.1" };
    std::string user, pass;
    bool        exposed { false };
    std::string warning;
};

inline bool is_loopback_host(const std::string& h) { return h.empty() || h == "127.0.0.1" || h == "localhost" || h == "::1"; }

inline RtspPlan plan_rtsp(const std::string& listen_text, const std::string& user, const std::string& pass)
{
    RtspPlan p;
    if (is_loopback_host(listen_text)) return p;
    bool host_ok = listen_text == "0.0.0.0" || listen_text == "::";
    if (!host_ok) {
        int dots = 0;
        host_ok  = !listen_text.empty();
        for (char c : listen_text) {
            if (c == '.') ++dots;
            else if (!std::isdigit((unsigned char) c)) host_ok = false;
        }
        host_ok = host_ok && dots == 3;
    }
    if (!host_ok) { p.warning = "EDGESLICER_GO2RTC_RTSP_LISTEN is not an address; the RTSP restream stays on loopback"; return p; }
    if (user.empty() || pass.empty()) {
        p.warning = "the RTSP restream was asked to listen on " + listen_text + " but no EDGESLICER_GO2RTC_RTSP_USER / _PASS is set; it stays on loopback";
        return p;
    }
    p.listen_host = listen_text;
    p.user        = user;
    p.pass        = pass;
    p.exposed     = true;
    return p;
}

} // namespace HubMedia
} // namespace Slic3r

#endif // slic3r_HubMedia_hpp_
