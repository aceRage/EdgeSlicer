#include <catch2/catch.hpp>

#include "libslic3r/GCode/StartupSound.hpp"

#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r::StartupSound;

namespace {

using Bytes = std::vector<uint8_t>;

void put_vlq(Bytes &o, uint32_t v)
{
    uint8_t tmp[5];
    int     n = 0;
    tmp[n++]  = uint8_t(v & 0x7F);
    while (v >>= 7)
        tmp[n++] = uint8_t(0x80 | (v & 0x7F));
    while (n > 0)
        o.push_back(tmp[--n]);
}

void put_be(Bytes &o, uint32_t v, int bytes)
{
    for (int i = bytes - 1; i >= 0; --i)
        o.push_back(uint8_t(v >> (8 * i)));
}

// A track from (delta ticks, raw event bytes) pairs; adds the end-of-track meta event.
Bytes track(const std::vector<std::pair<uint32_t, Bytes>> &events)
{
    Bytes body;
    for (const auto &e : events) {
        put_vlq(body, e.first);
        body.insert(body.end(), e.second.begin(), e.second.end());
    }
    body.insert(body.end(), {0x00, 0xFF, 0x2F, 0x00});
    Bytes out = {'M', 'T', 'r', 'k'};
    put_be(out, uint32_t(body.size()), 4);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes smf(int format, int division, const std::vector<Bytes> &tracks)
{
    Bytes out = {'M', 'T', 'h', 'd', 0, 0, 0, 6};
    put_be(out, uint32_t(format), 2);
    put_be(out, uint32_t(tracks.size()), 2);
    put_be(out, uint32_t(division), 2);
    for (const Bytes &t : tracks)
        out.insert(out.end(), t.begin(), t.end());
    return out;
}

Bytes tempo(uint32_t us) { return {0xFF, 0x51, 0x03, uint8_t(us >> 16), uint8_t(us >> 8), uint8_t(us)}; }
Bytes on(int key, int vel, int ch = 0) { return {uint8_t(0x90 | ch), uint8_t(key), uint8_t(vel)}; }
Bytes off(int key, int ch = 0) { return {uint8_t(0x80 | ch), uint8_t(key), 0x40}; }

MidiNote note(double s, double e, int key, int vel = 100, int ch = 0) { return MidiNote{s, e, key, vel, ch}; }

Options test_options()
{
    Options o;
    o.ms_per_unit = 10.;
    o.quantise_ms = 50.;
    o.max_seconds = 60.;
    o.max_steps   = 1000;
    return o;
}

// Ode to Joy (Beethoven, public domain), melody plus a bass note per bar, as a format 1 file:
// track 0 tempo, track 1 melody, track 2 bass. 480 ticks per quarter, 200 bpm.
Bytes ode_to_joy()
{
    // (key, quarters); notes play for 90% of their length so repeated notes are re-struck.
    const std::vector<std::pair<int, double>> melody = {
        {64, 1}, {64, 1}, {65, 1}, {67, 1}, {67, 1}, {65, 1}, {64, 1}, {62, 1},
        {60, 1}, {60, 1}, {62, 1}, {64, 1}, {64, 1.5}, {62, 0.5}, {62, 2},
        {64, 1}, {64, 1}, {65, 1}, {67, 1}, {67, 1}, {65, 1}, {64, 1}, {62, 1},
        {60, 1}, {60, 1}, {62, 1}, {64, 1}, {62, 1.5}, {60, 0.5}, {60, 2}};
    const std::vector<std::pair<int, double>> bass = {
        {48, 4}, {43, 4}, {48, 4}, {43, 4}, {48, 4}, {43, 4}, {48, 4}, {43, 2}, {48, 2}};
    auto voice = [](const std::vector<std::pair<int, double>> &notes, int vel) {
        std::vector<std::pair<uint32_t, Bytes>> ev;
        uint32_t gap = 0;
        for (const auto &n : notes) {
            const uint32_t len   = uint32_t(n.second * 480);
            const uint32_t sound = len * 9 / 10;
            ev.push_back({gap, on(n.first, vel)});
            ev.push_back({sound, off(n.first)});
            gap = len - sound;
        }
        return track(ev);
    };
    return smf(1, 480, {track({{0, tempo(300000)}}), voice(melody, 110), voice(bass, 70)});
}

} // namespace

TEST_CASE("Startup sound: a single note parses with the right time", "[StartupSound]")
{
    const Bytes file = smf(0, 480, {track({{0, tempo(500000)}, {0, on(60, 100)}, {480, off(60)}})});
    std::vector<MidiNote> notes;
    std::string           err;
    REQUIRE(parse_midi(file, notes, err));
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].key == 60);
    CHECK(notes[0].velocity == 100);
    CHECK(notes[0].start_s == Approx(0.));
    CHECK(notes[0].end_s == Approx(0.5));
}

TEST_CASE("Startup sound: running status, velocity-0 note off and a tempo change", "[StartupSound]")
{
    // Note 1: 0..480 at 120 bpm (0.5 s). Tempo halves at tick 480 (after the running-status events,
    // since a meta event cancels running status). Note 2: 480..960 -> 0.5..1.5 s.
    Bytes ev2 = {0x3C, 0x00};               // running status: note 60 velocity 0 = off
    Bytes ev3 = {0x40, 0x50};               // running status: note 64 on
    const Bytes file = smf(0, 480, {track({{0, on(60, 90)}, {480, ev2}, {0, ev3}, {0, tempo(1000000)}, {480, off(64)}})});
    std::vector<MidiNote> notes;
    std::string           err;
    REQUIRE(parse_midi(file, notes, err));
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].key == 60);
    CHECK(notes[0].end_s == Approx(0.5));
    CHECK(notes[1].key == 64);
    CHECK(notes[1].velocity == 0x50);
    CHECK(notes[1].start_s == Approx(0.5));
    CHECK(notes[1].end_s == Approx(1.5));
}

TEST_CASE("Startup sound: format 1 tracks merge and share the tempo track", "[StartupSound]")
{
    const Bytes file = smf(1, 96, {track({{0, tempo(250000)}}),
                                   track({{0, on(60, 100)}, {96, off(60)}}),
                                   track({{96, on(64, 100)}, {96, off(64)}})});
    std::vector<MidiNote> notes;
    std::string           err;
    REQUIRE(parse_midi(file, notes, err));
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].key == 60);
    CHECK(notes[0].end_s == Approx(0.25));
    CHECK(notes[1].key == 64);
    CHECK(notes[1].start_s == Approx(0.25));
    CHECK(notes[1].end_s == Approx(0.5));
}

TEST_CASE("Startup sound: bad files are refused without crashing", "[StartupSound]")
{
    std::vector<MidiNote> notes;
    std::string           err;
    CHECK_FALSE(parse_midi(Bytes{}, notes, err));
    CHECK_FALSE(parse_midi(Bytes{'R', 'I', 'F', 'F', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, notes, err));
    const Bytes good = smf(0, 480, {track({{0, on(60, 100)}, {480, off(60)}})});
    Bytes fmt2 = good;
    fmt2[9] = 2;
    CHECK_FALSE(parse_midi(fmt2, notes, err));
    CHECK(err.find("format 2") != std::string::npos);
    // Every truncation of a good file either parses or fails cleanly.
    for (size_t n = 0; n < good.size(); ++n) {
        Bytes cut(good.begin(), good.begin() + n);
        parse_midi(cut, notes, err);
    }
    // A track length far past the end of the file.
    Bytes lying = good;
    lying[18] = 0x7F;
    CHECK_FALSE(parse_midi(lying, notes, err));
    // midi_to_steps reports the error and makes no steps.
    const Result r = midi_to_steps(fmt2, Options{});
    CHECK(r.steps.empty());
    CHECK_FALSE(r.error.empty());
}

TEST_CASE("Startup sound: at most three notes at once, highest first, extras reported", "[StartupSound]")
{
    const Result r = convert({note(0, 0.5, 60), note(0, 0.5, 64), note(0, 0.5, 67), note(0, 0.5, 72)}, test_options());
    REQUIRE(r.steps.size() == 1);
    CHECK(r.steps[0].note[0] == 72);
    CHECK(r.steps[0].note[1] == 67);
    CHECK(r.steps[0].note[2] == 64);
    CHECK(r.steps[0].units == 50);
    CHECK(r.notes_in == 4);
    CHECK(r.notes_dropped == 1);
    CHECK(r.duration_s == Approx(0.5));
}

TEST_CASE("Startup sound: notes outside the range fold by octaves", "[StartupSound]")
{
    Options o = test_options();
    o.min_note = 36;
    o.max_note = 84;
    const Result r = convert({note(0, 0.1, 100), note(0.1, 0.2, 20), note(0.2, 0.3, 60)}, o);
    REQUIRE(r.steps.size() == 3);
    CHECK(r.steps[0].note[0] == 76);
    CHECK(r.steps[1].note[0] == 44);
    CHECK(r.steps[2].note[0] == 60);
    CHECK(r.notes_folded == 2);
    // A range narrower than an octave is widened so folding always ends.
    o.min_note = 60;
    o.max_note = 62;
    const Result r2 = convert({note(0, 0.1, 30)}, o);
    REQUIRE(r2.steps.size() == 1);
    CHECK(r2.steps[0].note[0] >= 60);
    CHECK(r2.steps[0].note[0] <= 71);
}

TEST_CASE("Startup sound: rests become silent steps, unison fills free voices", "[StartupSound]")
{
    const Result r = convert({note(0, 0.2, 60), note(0.4, 0.6, 62, 127)}, test_options());
    REQUIRE(r.steps.size() == 3);
    CHECK(r.steps[0].note[0] == 60);
    CHECK(r.steps[0].note[1] == 60);
    CHECK(r.steps[0].note[2] == 60);
    CHECK(r.steps[0].volume[0] == 78); // 100/127 * 99
    CHECK(r.steps[1].is_rest());
    CHECK(r.steps[1].volume[0] == 0);
    CHECK(r.steps[1].units == 20);
    CHECK(r.steps[2].note[0] == 62);
    CHECK(r.steps[2].volume[2] == 99);
    CHECK(r.total_units == 60);

    Options o = test_options();
    o.unison_fill = false;
    const Result r2 = convert({note(0, 0.2, 60)}, o);
    REQUIRE(r2.steps.size() == 1);
    CHECK(r2.steps[0].note[1] == 0);
    CHECK(r2.steps[0].volume[1] == 0);
}

TEST_CASE("Startup sound: leading silence is trimmed unless asked to keep it", "[StartupSound]")
{
    const Result r = convert({note(1.0, 1.2, 60)}, test_options());
    REQUIRE(r.steps.size() == 1);
    Options o = test_options();
    o.trim_leading_rest = false;
    const Result r2 = convert({note(1.0, 1.2, 60)}, o);
    REQUIRE(r2.steps.size() == 2);
    CHECK(r2.steps[0].is_rest());
    CHECK(r2.steps[0].units == 100);
}

TEST_CASE("Startup sound: long notes split, caps truncate and count drops", "[StartupSound]")
{
    Options o = test_options();
    o.max_units_per_step = 100;
    const Result split = convert({note(0, 3.0, 60)}, o);
    REQUIRE(split.steps.size() == 3);
    CHECK(split.steps[2].units == 100);

    std::vector<MidiNote> scale;
    for (int i = 0; i < 10; ++i)
        scale.push_back(note(0.1 * i, 0.1 * i + 0.1, 60 + i));
    o.max_steps = 3;
    const Result capped = convert(scale, o);
    CHECK(capped.steps.size() == 3);
    CHECK(capped.truncated);
    CHECK(capped.notes_dropped == 7);

    o.max_steps   = 1000;
    o.max_seconds = 0.25;
    const Result timed = convert(scale, o);
    CHECK(timed.total_units == 25);
    CHECK(timed.duration_s == Approx(0.25));
    CHECK(timed.steps.size() == 3);
    CHECK(timed.steps.back().units == 5);
    CHECK(timed.truncated);
}

TEST_CASE("Startup sound: percussion is skipped, short notes last one grid step", "[StartupSound]")
{
    const Result r = convert({note(0, 0.5, 36, 100, 9), note(0, 0.001, 60)}, test_options());
    CHECK(r.percussion_skipped == 1);
    REQUIRE(r.steps.size() == 1);
    CHECK(r.steps[0].units == 5);
}

TEST_CASE("Startup sound: G-code text", "[StartupSound]")
{
    Step s;
    s.note[0] = 53; s.note[1] = 53; s.note[2] = 53;
    s.volume[0] = 50; s.volume[1] = 99; s.volume[2] = 99;
    s.units = 9;
    CHECK(step_to_gcode(s) == "M1006 A53 B9 L50 C53 D9 M99 E53 F9 N99");
    CHECK(steps_to_gcode({s}, "Ode;to\nJoy") == "; startup sound: Ode_to_Joy\nM1006 S1\nM1006 A53 B9 L50 C53 D9 M99 E53 F9 N99\nM1006 W\n");
    CHECK(steps_to_gcode({}, "") == "M1006 S1\nM1006 W\n");
}

TEST_CASE("Startup sound: the tune block of a start G-code is found and replaced", "[StartupSound]")
{
    const std::string h3c = "M400\n\n;=====printer start sound ===================\nM17\nM400 S1\nM1006 S1\n"
                            "M1006 A53 B9 L50 C53 D9 M99 E53 F9 N99 \nM1006 A61 B18 L60 C61 D18 M80 E61 F18 N80\n"
                            "M1006 W\n;=====printer start sound ===================\n\nM1006 S1\nM1006 W\n";
    CHECK(has_tune_block(h3c));
    std::string g = h3c;
    REQUIRE(replace_tune_block(g, "; startup sound: mine\nM1006 S1\nM1006 A60 B5 L99 C60 D5 M99 E60 F5 N99\nM1006 W\n"));
    CHECK(g == "M400\n\n;=====printer start sound ===================\nM17\nM400 S1\n; startup sound: mine\nM1006 S1\n"
               "M1006 A60 B5 L99 C60 D5 M99 E60 F5 N99\nM1006 W\n;=====printer start sound ===================\n\n"
               "M1006 S1\nM1006 W\n");
    // Replacing again finds our own block, label line included.
    REQUIRE(replace_tune_block(g, "M1006 S1\nM1006 W\n"));
    CHECK(g.find("mine") == std::string::npos);
    CHECK(g.find("M400 S1\nM1006 S1\nM1006 W\n;=====") != std::string::npos);
    // A label line as the very first line.
    std::string first = "; startup sound: x\nM1006 S1\nM1006 W\nG28\n";
    REQUIRE(replace_tune_block(first, "M1006 S1\nM1006 W\n"));
    CHECK(first == "M1006 S1\nM1006 W\nG28\n");

    std::string none = "G28\nM1006 S10\nM1006 W\n";
    CHECK_FALSE(has_tune_block(none));
    CHECK_FALSE(replace_tune_block(none, "M1006 S1\nM1006 W\n"));
    CHECK(none == "G28\nM1006 S10\nM1006 W\n");
    CHECK_FALSE(has_tune_block("M1006 S1\nM1006 A60 B5 L99 C60 D5 M99 E60 F5 N99\n"));

    // A block at the very end without a final newline stays without one.
    std::string tail = "G28\nM1006 S1\nM1006 W";
    REQUIRE(replace_tune_block(tail, "M1006 S1\nM1006 A60 B5 L99 C60 D5 M99 E60 F5 N99\nM1006 W\n"));
    CHECK(tail == "G28\nM1006 S1\nM1006 A60 B5 L99 C60 D5 M99 E60 F5 N99\nM1006 W");
}

TEST_CASE("Startup sound: Ode to Joy converts within the default caps", "[StartupSound]")
{
    const Bytes file = ode_to_joy();
    Options     o;
    o.max_steps   = 200;
    o.max_seconds = 30.;
    const Result r = midi_to_steps(file, o);
    REQUIRE(r.error.empty());
    CHECK(r.notes_in == 30 + 9);
    CHECK(r.notes_dropped == 0);
    CHECK_FALSE(r.truncated);
    // 32 quarters at 200 bpm = 9.6 s; the last note sounds for 90% of a half note.
    CHECK(r.duration_s == Approx(9.6 - 0.06).margin(0.06));
    // Two notes sound at the start: melody E4 on top, bass C3; the third voice stays silent.
    CHECK(r.steps.front().note[0] == 64);
    CHECK(r.steps.front().note[1] == 48);
    CHECK(r.steps.front().note[2] == 0);
    CHECK(r.steps.front().volume[2] == 0);
    // Between two melody notes only the held bass sounds, in its own voice (no bass blip on every
    // voice): the melody voice is silent in those gap steps.
    int gaps = 0;
    for (const Step &s : r.steps)
        if (s.note[0] == 0 && s.note[1] != 0) {
            ++gaps;
            CHECK(s.volume[0] == 0);
            CHECK(s.note[2] == 0);
        }
    CHECK(gaps > 0);
    for (const Step &s : r.steps)
        CHECK_FALSE((s.note[0] == s.note[1] && s.note[0] != 0));
}

TEST_CASE("Startup sound: calibrated unit is 10 ms", "[StartupSound]")
{
    // H3C recording: 478 units played in 4.78 s, so one B/D/F unit is 10 ms.
    CHECK(Options().ms_per_unit == 10.);
}

// Writes the owner's demo files. Hidden: run with
//   EDGESLICER_STARTSOUND_OUT=<dir> libslic3r_tests "[startup_sound_demo]"
TEST_CASE("Startup sound: write the Ode to Joy demo files", "[.][startup_sound_demo]")
{
    const char *dir = std::getenv("EDGESLICER_STARTSOUND_OUT");
    REQUIRE(dir != nullptr);
    const Bytes file = ode_to_joy();
    {
        std::ofstream mid(std::string(dir) + "/ode_to_joy.mid", std::ios::binary);
        mid.write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
    }
    Options o; // the defaults the GUI starts with
    const Result r = midi_to_steps(file, o);
    REQUIRE(r.error.empty());
    std::ofstream g(std::string(dir) + "/h3c_ode_to_joy.gcode", std::ios::binary);
    g << "; EdgeSlicer startup sound demo: Ode to Joy (Beethoven, public domain)\n"
      << "; NOT a print. No heating, no homing, no movement: only the buzzer/motor tune.\n"
      << "; Made by the StartupSound converter with " << o.ms_per_unit << " ms per B unit (calibrated on an H3C),\n"
      << "; quantise " << o.quantise_ms << " ms. " << r.steps.size() << " steps, " << r.total_units << " units, about "
      << r.duration_s << " s.\n"
      << "M17\nM400 S1\n"
      << steps_to_gcode(r.steps, "Ode to Joy")
      << "M400\n";
}

TEST_CASE("Ending sound: the tune block of an end G-code is replaced, the rest kept", "[StartupSound]")
{
    // The H3C end G-code's tail (stock profile, 2026-10-28), with the purge block before it.
    const std::string h3c_end =
        "M622 J2\nM1002 gcode_claim_action : 66\nM145 P0\nM400 S180\nM623\n"
        ";=====printer finish air purification=========\n\n;=====printer finish  sound=========\n"
        "M17\nM400 S1\nM1006 S1\n"
        "M1006 A53 B10 L99 C53 D10 M99 E53 F10 N99 \nM1006 A57 B10 L99 C57 D10 M99 E57 F10 N99 \n"
        "M1006 A0 B15 L0 C0 D15 M0 E0 F15 N0 \nM1006 A60 B10 L99 C60 D10 M99 E60 F10 N99\n"
        "M1006 W\n;=====printer finish  sound=========\nM400\nM18\n";
    REQUIRE(has_tune_block(h3c_end));
    Step s;
    s.note[0] = s.note[1] = s.note[2] = 72;
    s.volume[0] = s.volume[1] = s.volume[2] = 99;
    s.units = 20;
    const std::string block = steps_to_gcode({s}, "fanfare");
    std::string g = h3c_end;
    REQUIRE(replace_tune_block(g, block));
    const std::string before = h3c_end.substr(0, h3c_end.find("M1006 S1"));
    const std::string after  = h3c_end.substr(h3c_end.find("M1006 W\n") + 8);
    CHECK(g == before + block + after);
    CHECK(g.find("M400\nM18\n") == g.size() - 9); // the final motor release stays last
    // Replacing again swaps our block (label included) instead of adding a second tune.
    REQUIRE(replace_tune_block(g, steps_to_gcode({s}, "second")));
    CHECK(g.find("fanfare") == std::string::npos);
    CHECK(g.find("M1006 S1") == g.rfind("M1006 S1"));

    // A1 style: the motor-current comment, M400 and M18 X Y Z follow the block.
    std::string a1 = ";=====printer finish  sound=========\nM17\nM400 S1\nM1006 S1\n"
                     "M1006 A0 B20 L100 C37 D20 M40 E42 F20 N60\nM1006 W\n;=====printer finish  sound=========\n\n"
                     ";M17 X0.8 Y0.8 Z0.5 ; lower motor current to 45% power\nM400\nM18 X Y Z\n\n";
    REQUIRE(replace_tune_block(a1, block));
    CHECK(a1 == ";=====printer finish  sound=========\nM17\nM400 S1\n" + block +
                ";=====printer finish  sound=========\n\n;M17 X0.8 Y0.8 Z0.5 ; lower motor current to 45% power\nM400\nM18 X Y Z\n\n");

    // End G-code without a tune (P1P/P1S style): nothing is inserted.
    std::string p1 = "M400 ; wait for buffer to clear\nM104 S0 ; turn off hotend\nM17 S\nM18\n";
    CHECK_FALSE(has_tune_block(p1));
    CHECK_FALSE(replace_tune_block(p1, block));
    CHECK(p1 == "M400 ; wait for buffer to clear\nM104 S0 ; turn off hotend\nM17 S\nM18\n");
}
