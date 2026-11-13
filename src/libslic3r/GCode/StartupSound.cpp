#include "StartupSound.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <utility>

namespace Slic3r {
namespace StartupSound {

namespace {

// Hard limits that keep a hostile or broken file from eating memory or time.
constexpr size_t MAX_NOTES  = 100000;
constexpr size_t MAX_TEMPOS = 10000;
// Comment line steps_to_gcode() puts before the block; replace_tune_block() replaces it with the block.
constexpr const char *LABEL_PREFIX = "; startup sound: ";

struct Reader
{
    const uint8_t *data;
    size_t         size;
    size_t         pos = 0;
    bool           ok  = true;

    bool     has(size_t n) const { return ok && n <= size - pos; }
    uint8_t  u8()
    {
        if (!has(1)) { ok = false; return 0; }
        return data[pos++];
    }
    uint32_t be(int bytes)
    {
        uint32_t v = 0;
        for (int i = 0; i < bytes; ++i)
            v = (v << 8) | u8();
        return v;
    }
    // MIDI variable-length quantity: at most 4 bytes.
    uint32_t vlq()
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const uint8_t b = u8();
            v = (v << 7) | (b & 0x7F);
            if ((b & 0x80) == 0)
                return v;
        }
        ok = false;
        return 0;
    }
    void skip(size_t n)
    {
        if (!has(n)) { ok = false; pos = size; return; }
        pos += n;
    }
};

struct RawNote
{
    uint64_t start_tick;
    uint64_t end_tick;
    int      key;
    int      velocity;
    int      channel;
};

struct Tempo
{
    uint64_t tick;
    uint32_t us_per_quarter;
};

// Parses one MTrk body. Appends finished notes and tempo changes.
bool parse_track(Reader &r, std::vector<RawNote> &notes, std::vector<Tempo> &tempos, std::string &error)
{
    uint64_t tick            = 0;
    uint8_t  running_status  = 0;
    // Open notes per (channel, key), oldest first.
    std::map<int, std::deque<std::pair<uint64_t, int>>> open;

    auto note_off = [&](int channel, int key) {
        auto it = open.find(channel * 128 + key);
        if (it == open.end() || it->second.empty())
            return; // stray note-off: ignore
        const auto on = it->second.front();
        it->second.pop_front();
        if (notes.size() < MAX_NOTES)
            notes.push_back({on.first, tick, key, on.second, channel});
    };

    while (r.ok && r.pos < r.size) {
        tick += r.vlq();
        if (!r.ok)
            break;
        uint8_t status = r.u8();
        if (!r.ok)
            break;
        if (status < 0x80) {
            // Running status: this byte is the first data byte.
            if (running_status == 0) {
                error = "MIDI: data byte without a status byte";
                return false;
            }
            --r.pos;
            status = running_status;
        }
        if (status == 0xFF) {
            const uint8_t  type = r.u8();
            const uint32_t len  = r.vlq();
            if (!r.has(len)) { r.ok = false; break; }
            if (type == 0x51 && len == 3) {
                const uint32_t us = (uint32_t(r.data[r.pos]) << 16) | (uint32_t(r.data[r.pos + 1]) << 8) | r.data[r.pos + 2];
                if (us > 0 && tempos.size() < MAX_TEMPOS)
                    tempos.push_back({tick, us});
            }
            r.skip(len);
            if (type == 0x2F)
                break; // end of track
            continue;
        }
        if (status == 0xF0 || status == 0xF7) {
            r.skip(r.vlq());
            continue;
        }
        if (status >= 0xF0) {
            // System common / real-time messages do not belong in a file.
            error = "MIDI: unexpected system message";
            return false;
        }
        running_status       = status;
        const int    kind    = status & 0xF0;
        const int    channel = status & 0x0F;
        const int    d1      = r.u8() & 0x7F;
        int          d2      = 0;
        if (kind != 0xC0 && kind != 0xD0)
            d2 = r.u8() & 0x7F;
        if (!r.ok)
            break;
        if (kind == 0x90 && d2 > 0) {
            auto &q = open[channel * 128 + d1];
            if (q.size() < 64)
                q.emplace_back(tick, d2);
        } else if (kind == 0x80 || kind == 0x90) {
            note_off(channel, d1);
        }
    }
    // Notes still held at the end of the track end there.
    for (auto &kv : open)
        for (const auto &on : kv.second)
            if (notes.size() < MAX_NOTES)
                notes.push_back({on.first, std::max(tick, on.first), kv.first % 128, on.second, kv.first / 128});
    if (!r.ok) {
        error = "MIDI: track data is cut short";
        return false;
    }
    return true;
}

int fold_into(int key, int lo, int hi)
{
    while (key < lo)
        key += 12;
    while (key > hi)
        key -= 12;
    return key;
}

std::string sanitise_label(const std::string &label)
{
    std::string out;
    for (char c : label) {
        const unsigned char u = (unsigned char) c;
        if (u >= 0x20 && u < 0x7F && c != ';')
            out += c;
        else if (!out.empty() && out.back() != '_')
            out += '_';
        if (out.size() >= 60)
            break;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '_'))
        out.pop_back();
    return out;
}

// Finds the byte ranges of the first "M1006 S1" line and the next "M1006 W" line after it.
// Returns false when there is no such pair. `end` is just past the W line's '\n' (or the end).
bool find_tune_block(const std::string &gcode, size_t &begin, size_t &end, bool &ends_with_newline)
{
    auto line_is = [](const std::string &s, size_t line_start, size_t line_end, const char *cmd) {
        size_t i = line_start;
        while (i < line_end && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        const std::string c(cmd);
        if (line_end - i < c.size() || s.compare(i, c.size(), c) != 0)
            return false;
        i += c.size();
        return i == line_end || s[i] == ' ' || s[i] == '\t' || s[i] == ';' || s[i] == '\r';
    };
    const std::string label_prefix(LABEL_PREFIX);
    bool   in_block   = false;
    size_t pos        = 0;
    size_t prev_start = std::string::npos;
    while (pos <= gcode.size()) {
        size_t nl       = gcode.find('\n', pos);
        size_t line_end = nl == std::string::npos ? gcode.size() : nl;
        if (!in_block) {
            if (line_is(gcode, pos, line_end, "M1006 S1")) {
                in_block = true;
                begin    = pos;
                // Our own label line right before the block belongs to it.
                if (prev_start != std::string::npos && gcode.compare(prev_start, label_prefix.size(), label_prefix) == 0)
                    begin = prev_start;
            }
            prev_start = pos;
        } else if (line_is(gcode, pos, line_end, "M1006 W")) {
            end               = nl == std::string::npos ? gcode.size() : nl + 1;
            ends_with_newline = nl != std::string::npos;
            return true;
        }
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return false;
}

} // namespace

bool parse_midi(const std::vector<uint8_t> &bytes, std::vector<MidiNote> &out, std::string &error)
{
    out.clear();
    error.clear();
    Reader r{bytes.data(), bytes.size()};
    if (bytes.size() < 14 || bytes[0] != 'M' || bytes[1] != 'T' || bytes[2] != 'h' || bytes[3] != 'd') {
        error = "Not a MIDI file";
        return false;
    }
    r.skip(4);
    const uint32_t header_len = r.be(4);
    if (!r.ok || header_len < 6) {
        error = "MIDI: bad header";
        return false;
    }
    const uint32_t format   = r.be(2);
    r.be(2); // track count: we read every MTrk chunk present instead
    const uint32_t division = r.be(2);
    r.skip(header_len - 6);
    if (!r.ok) {
        error = "MIDI: bad header";
        return false;
    }
    if (format > 1) {
        error = "MIDI format 2 (independent sequences) is not supported; save the file as format 0 or 1";
        return false;
    }
    if (division == 0) {
        error = "MIDI: bad time division";
        return false;
    }

    std::vector<RawNote> raw;
    std::vector<Tempo>   tempos;
    int                  tracks = 0;
    while (r.ok && r.pos + 8 <= r.size) {
        const uint32_t id  = r.be(4);
        const uint32_t len = r.be(4);
        if (!r.ok)
            break;
        const size_t available = r.size - r.pos;
        if (id == 0x4D54726B /* MTrk */) {
            Reader tr{r.data + r.pos, std::min<size_t>(len, available)};
            if (!parse_track(tr, raw, tempos, error))
                return false;
            if (len > available) {
                error = "MIDI: track data is cut short";
                return false;
            }
            ++tracks;
        }
        if (len > available)
            break;
        r.skip(len);
    }
    if (tracks == 0) {
        error = "MIDI: no tracks";
        return false;
    }

    // Tick -> seconds.
    std::function<double(uint64_t)> to_seconds;
    if (division & 0x8000) {
        const int    fps_code = -int(int8_t(division >> 8));
        const double fps      = fps_code == 29 ? 29.97 : double(fps_code);
        const int    tpf      = int(division & 0xFF);
        if (fps <= 0. || tpf <= 0) {
            error = "MIDI: bad time division";
            return false;
        }
        const double ticks_per_s = fps * tpf;
        to_seconds = [ticks_per_s](uint64_t t) { return double(t) / ticks_per_s; };
    } else {
        std::stable_sort(tempos.begin(), tempos.end(), [](const Tempo &a, const Tempo &b) { return a.tick < b.tick; });
        // Seconds at each tempo change.
        struct Seg { uint64_t tick; double sec; double s_per_tick; };
        std::vector<Seg> segs;
        const double ppq = double(division);
        segs.push_back({0, 0., 0.5 / ppq}); // 120 bpm until the first tempo event
        for (const Tempo &t : tempos) {
            const Seg   &last = segs.back();
            const double sec  = last.sec + double(t.tick - last.tick) * last.s_per_tick;
            const double spt  = double(t.us_per_quarter) / 1e6 / ppq;
            if (t.tick == last.tick)
                segs.back().s_per_tick = spt;
            else
                segs.push_back({t.tick, sec, spt});
        }
        to_seconds = [segs](uint64_t t) {
            auto it = std::upper_bound(segs.begin(), segs.end(), t, [](uint64_t v, const Seg &s) { return v < s.tick; });
            const Seg &s = *std::prev(it);
            return s.sec + double(t - s.tick) * s.s_per_tick;
        };
    }

    out.reserve(raw.size());
    for (const RawNote &n : raw)
        out.push_back({to_seconds(n.start_tick), to_seconds(n.end_tick), n.key, n.velocity, n.channel});
    std::stable_sort(out.begin(), out.end(), [](const MidiNote &a, const MidiNote &b) {
        return a.start_s < b.start_s || (a.start_s == b.start_s && a.key > b.key);
    });
    return true;
}

Result convert(const std::vector<MidiNote> &notes_in, const Options &opt_in)
{
    Options opt = opt_in;
    opt.ms_per_unit        = std::max(0.1, opt.ms_per_unit);
    opt.quantise_ms        = std::max(1., opt.quantise_ms);
    opt.min_note           = std::clamp(opt.min_note, 1, 127);
    opt.max_note           = std::clamp(opt.max_note, 1, 127);
    if (opt.max_note - opt.min_note < 11) {
        // Octave folding needs at least an octave of room.
        opt.max_note = std::min(127, opt.min_note + 11);
        opt.min_note = opt.max_note - 11;
    }
    opt.max_volume         = std::clamp(opt.max_volume, 1, 100);
    opt.max_units_per_step = std::max(1, opt.max_units_per_step);
    opt.max_steps          = std::max(1, opt.max_steps);
    opt.max_seconds        = std::max(0.01, opt.max_seconds);

    Result res;
    struct QNote { long long qs, qe; int key, folded, volume; };
    std::vector<QNote> q;
    const double grid_s = opt.quantise_ms / 1000.;
    for (const MidiNote &n : notes_in) {
        if (opt.skip_percussion && n.channel == 9) {
            ++res.percussion_skipped;
            continue;
        }
        if (n.key < 0 || n.key > 127 || n.velocity <= 0 || !std::isfinite(n.start_s) || !std::isfinite(n.end_s))
            continue;
        long long qs = std::llround(std::max(0., n.start_s) / grid_s);
        long long qe = std::llround(std::max(0., n.end_s) / grid_s);
        if (qe <= qs)
            qe = qs + 1;
        const int folded = fold_into(n.key, opt.min_note, opt.max_note);
        if (folded != n.key)
            ++res.notes_folded;
        const int vol = std::clamp(int(std::lround(double(std::min(n.velocity, 127)) / 127. * opt.max_volume)), 1, 100);
        q.push_back({qs, qe, n.key, folded, vol});
    }
    res.notes_in = int(q.size());
    if (q.empty())
        return res;

    // Segment boundaries: every quantised note start and end.
    std::vector<long long> bounds;
    bounds.reserve(q.size() * 2 + 1);
    for (const QNote &n : q) {
        bounds.push_back(n.qs);
        bounds.push_back(n.qe);
    }
    if (!opt.trim_leading_rest)
        bounds.push_back(0);
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    std::vector<size_t> order(q.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&q](size_t a, size_t b) { return q[a].qs < q[b].qs; });

    std::vector<bool>   played(q.size(), false);
    std::vector<size_t> active;
    size_t              next = 0;
    const int           max_total_units = std::max(1, int(std::floor(opt.max_seconds * 1000. / opt.ms_per_unit + 1e-9)));

    for (size_t b = 0; b + 1 < bounds.size() && !res.truncated; ++b) {
        const long long t0 = bounds[b], t1 = bounds[b + 1];
        while (next < order.size() && q[order[next]].qs <= t0)
            active.push_back(order[next++]);
        active.erase(std::remove_if(active.begin(), active.end(), [&](size_t i) { return q[i].qe <= t0; }), active.end());

        // Highest original pitch first: the melody is usually on top.
        std::vector<size_t> sounding = active;
        std::sort(sounding.begin(), sounding.end(), [&q](size_t a, size_t b) {
            return q[a].key > q[b].key || (q[a].key == q[b].key && q[a].volume > q[b].volume);
        });
        Step step;
        int  used = 0;
        std::vector<size_t> chosen;
        for (size_t i : sounding) {
            bool same = false;
            for (int v = 0; v < used; ++v)
                if (step.note[v] == q[i].folded) {
                    step.volume[v] = std::max(step.volume[v], q[i].volume);
                    same = true;
                }
            if (same) {
                chosen.push_back(i); // merged with an equal pitch: it is heard
                continue;
            }
            if (used < VOICES) {
                step.note[used]   = q[i].folded;
                step.volume[used] = q[i].volume;
                ++used;
                chosen.push_back(i);
            }
        }
        if (opt.unison_fill && used > 0)
            for (int v = used; v < VOICES; ++v) {
                step.note[v]   = step.note[0];
                step.volume[v] = step.volume[0];
            }

        // Units from rounded absolute times, so rounding errors do not add up along the tune.
        const long long origin = bounds.front();
        const long long u0     = std::llround(double(t0 - origin) * opt.quantise_ms / opt.ms_per_unit);
        const long long u1     = std::llround(double(t1 - origin) * opt.quantise_ms / opt.ms_per_unit);
        int             units  = int(std::clamp<long long>(u1 - u0, 1, 1000000));
        while (units > 0) {
            if (int(res.steps.size()) >= opt.max_steps || res.total_units >= max_total_units) {
                res.truncated = true;
                break;
            }
            Step part   = step;
            part.units  = std::min({units, opt.max_units_per_step, max_total_units - res.total_units});
            units      -= part.units;
            res.total_units += part.units;
            res.steps.push_back(part);
            for (size_t i : chosen)
                played[i] = true;
        }
    }
    // A cap can stop the tune right after a rest; a trailing rest is pointless.
    while (!res.steps.empty() && res.steps.back().is_rest()) {
        res.total_units -= res.steps.back().units;
        res.steps.pop_back();
    }
    for (bool p : played)
        if (!p)
            ++res.notes_dropped;
    res.duration_s = double(res.total_units) * opt.ms_per_unit / 1000.;
    return res;
}

Result midi_to_steps(const std::vector<uint8_t> &bytes, const Options &opt)
{
    std::vector<MidiNote> notes;
    std::string           error;
    if (!parse_midi(bytes, notes, error)) {
        Result r;
        r.error = error;
        return r;
    }
    Result r = convert(notes, opt);
    if (r.steps.empty())
        r.error = "The MIDI file has no playable notes";
    return r;
}

std::string step_to_gcode(const Step &s)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "M1006 A%d B%d L%d C%d D%d M%d E%d F%d N%d",
                  s.note[0], s.units, s.volume[0], s.note[1], s.units, s.volume[1], s.note[2], s.units, s.volume[2]);
    return buf;
}

std::string steps_to_gcode(const std::vector<Step> &steps, const std::string &label)
{
    // The label goes on its own comment line: the stock profiles never put a comment on an M1006 line.
    std::string out;
    const std::string clean = sanitise_label(label);
    if (!clean.empty())
        out += std::string(LABEL_PREFIX) + clean + '\n';
    out += "M1006 S1\n";
    for (const Step &s : steps)
        out += step_to_gcode(s) + '\n';
    out += "M1006 W\n";
    return out;
}

bool has_tune_block(const std::string &gcode)
{
    size_t b = 0, e = 0;
    bool   nl = false;
    return find_tune_block(gcode, b, e, nl);
}

bool replace_tune_block(std::string &gcode, const std::string &block)
{
    size_t b = 0, e = 0;
    bool   nl = false;
    if (!find_tune_block(gcode, b, e, nl))
        return false;
    std::string repl = block;
    if (!nl && !repl.empty() && repl.back() == '\n')
        repl.pop_back();
    else if (nl && (repl.empty() || repl.back() != '\n'))
        repl += '\n';
    gcode.replace(b, e - b, repl);
    return true;
}

} // namespace StartupSound
} // namespace Slic3r
