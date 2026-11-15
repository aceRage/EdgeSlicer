#pragma once

// Startup sounds for Bambu printers (tests/design_startup_sounds.md).
//
// Bambu machine start / end G-code plays short tunes with M1006:
//     M1006 S1                                        ; start of a tune block
//     M1006 A53 B9 L50 C53 D9 M99 E53 F9 N99          ; one step, three voices
//     M1006 W                                         ; wait for playback
// Read from the stock profiles: A/C/E are MIDI note numbers (0 = that voice is silent), B/D/F are
// durations in an undocumented unit and L/M/N are volumes 0..100. Every stock step except a few
// A1 mini lines gives the three voices the same duration, so this converter does too.
//
// This file is GUI-free: a MIDI file (format 0 or 1) becomes a list of steps, the steps become
// G-code, and the tune block of a start G-code can be found and replaced.

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
namespace StartupSound {

// One note from a MIDI file, in seconds from the start of the file.
struct MidiNote
{
    double start_s  = 0.;
    double end_s    = 0.;
    int    key      = 60; // MIDI note number 0..127
    int    velocity = 100;
    int    channel  = 0;  // 0..15; 9 is General MIDI percussion
};

// Parses a Standard MIDI File (format 0 or 1; format 2 is refused). Tempo changes from every track
// apply to all of them. Returns false and sets `error` on a malformed or unsupported file.
bool parse_midi(const std::vector<uint8_t> &bytes, std::vector<MidiNote> &notes, std::string &error);

// The firmware has three voices.
constexpr int VOICES = 3;

struct Step
{
    int note[VOICES]   = {0, 0, 0}; // 0 = silent voice
    int volume[VOICES] = {0, 0, 0}; // 0..100
    int units          = 0;         // the B/D/F duration
    bool is_rest() const { return note[0] == 0 && note[1] == 0 && note[2] == 0; }
};

struct Options
{
    // Milliseconds per B/D/F unit: 10 ms, measured from a recording of an H3C playing a converted
    // tune (tests/startup_sound_calibration.md; 478 units played in 4.78 s, no per-line overhead,
    // the three voices of a line sound together). Stock tunes use 9..20 units per step.
    double ms_per_unit        = 10.;
    // Note starts and ends snap to this grid (ms). Shorter notes are stretched to one grid step.
    double quantise_ms        = 50.;
    // Notes outside this range are moved by whole octaves until they fit (stock tunes use 37..61).
    int    min_note           = 36;
    int    max_note           = 84;
    // Velocity 127 maps to this volume, smaller velocities scale down (never below 1).
    int    max_volume         = 99;
    // A longer step is split into several of at most this many units (stock maximum is 20).
    int    max_units_per_step = 100;
    // Caps: the tune stops at whichever comes first.
    int    max_steps          = 100;
    double max_seconds        = 10.;
    // Ignore channel 10 (General MIDI drums): a buzzer cannot play a drum kit.
    bool   skip_percussion    = true;
    // For a tune that never has two notes at once, repeat the note on the free voices (the stock
    // H2/H3 tunes play every note in unison on all three voices). A tune with a bass line keeps
    // its free voice silent.
    bool   unison_fill        = true;
    // Drop silence before the first note.
    bool   trim_leading_rest  = true;
};

struct Result
{
    std::vector<Step> steps;
    int    notes_in        = 0; // notes considered (after skipping percussion)
    int    notes_dropped   = 0; // notes never played: more than three at once, or past the caps
    int    notes_folded    = 0; // notes moved by octaves into [min_note, max_note]
    int    percussion_skipped = 0;
    bool   truncated       = false;
    int    total_units     = 0;
    double duration_s      = 0.; // total_units * ms_per_unit / 1000
    std::string error;           // non-empty when nothing could be converted
};

// Turns notes into steps. Never fails; an empty input gives an empty result.
Result convert(const std::vector<MidiNote> &notes, const Options &opt);

// parse_midi() + convert(). On a parse error the result has `error` set and no steps.
Result midi_to_steps(const std::vector<uint8_t> &bytes, const Options &opt);

// One "M1006 A.. B.. L.. C.. D.. M.. E.. F.. N.." line, no newline.
std::string step_to_gcode(const Step &step);

// An optional "; startup sound: <label>" comment line, "M1006 S1", the steps, "M1006 W", each line
// ending in '\n'. The label is reduced to printable ASCII without ';'.
std::string steps_to_gcode(const std::vector<Step> &steps, const std::string &label = std::string());

// True when the G-code has a tune block: a line starting "M1006 S1" followed later by "M1006 W".
bool has_tune_block(const std::string &gcode);

// Replaces the first tune block (from the "M1006 S1" line to the next "M1006 W" line, both
// included, plus a "; startup sound: " label line right before it) with `block` (as made by steps_to_gcode). Everything else, including the M17 /
// M400 S1 before the block, is kept byte for byte. Returns false and leaves `gcode` alone when
// there is no tune block.
bool replace_tune_block(std::string &gcode, const std::string &block);

} // namespace StartupSound
} // namespace Slic3r
