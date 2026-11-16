# Startup sounds (M1006 tunes from MIDI) - phase 1

Owner go 2026-11-13. Branch `feat/startup-sounds`.

## What the profiles tell us about M1006

Grep of `resources/profiles/BBL/machine` (26 files, 30 blocks, 358 step lines):

| Printer (profile) | start G-code tune | end G-code tune |
|---|---|---|
| X1, X1 Carbon (0.2/0.4/0.6/0.8) | yes, 19 steps | 0.4 only |
| A1, A1 mini | yes | yes |
| A2L, P2S, H2C, H2D, H2D Pro, H2S, H3C, X2D (template files) | yes, 6 steps | yes, ~8 steps |
| P1P/P1S, A1 mini variants without the line, non-BBL vendors | no | no |

Every block has the same shape:

```
M17            ; motors on (every block; the X1 drops motor current right after)
M400 S1
M1006 S1       ; start of tune
M1006 A53 B9 L50 C53 D9 M99 E53 F9 N99   ; one step, repeated
M1006 W        ; wait for playback
```

Facts from the data:
- Step lines always carry all nine parameters `A B L C D M E F N`; no other letters appear.
- A/C/E (notes): 0, 37..61. `0` is used for a silent voice (A1, X1 tunes keep a volume of 100 on it).
  Values match MIDI note numbers well (H3C start = F3, G#3, C#4 arpeggio). Supported, not proven.
- B/D/F (durations): 0, 9, 10, 15, 18, 20. B = D = F on every line except a few A1 mini lines
  (`A0 B0 L100 C.. D10 ..`) where a silent voice has duration 0. So voices *may* have their own
  durations, but nothing shows how the step length is then decided.
- L/M/N (volumes): 0, 30..100. A rest is written `A0 B15 L0 C0 D15 M0 E0 F15 N0` (H3C end tune).
- `S1` and `W` only ever appear once per block, first and last.
- `M17` before every tune suggests the sound comes from the stepper motors (three voices = three
  motors?). Inference only.

## Unknowns (to settle with `h3c_m1006_timing.gcode`)

1. **Time unit of B/D/F.** Converter constant `ms_per_unit`, default **20 ms** (stock tunes have
   63..230 units and sound roughly 1.5..4 s). Calibration section 1 plays B = 1, 5, 10, 20, 50, 100.
2. **Largest B.** Stock max is 20; the converter splits steps at `max_units_per_step` = 100.
3. **Note range.** Stock 37..61. Default fold range 36..84; section 2 plays MIDI 24..108 in octaves.
4. **Voices with different durations.** Converter always writes B = D = F. Section 3 tests B10/D30/F60.
5. **Most lines per block** (and any firmware buffer limit). Default cap 100 lines / 10 s.
   Section 4 plays one 100-line block.
6. **Rest form.** Converter writes `A0 .. L0` (the H3C form). Section 5 compares it with a volume-0 note.
7. **Re-articulation.** A note held across a step boundary (another voice changed) is written again
   in the next step; whether the printer re-strikes it is unknown.

## Where the tune goes

Replace the existing block in **Machine start G-code**: from the first `M1006 S1` line to the next
`M1006 W` line (both included), plus our own `; startup sound: <name>` label line just above it
(so a second replace removes it). `M17` / `M400 S1` before the block stay. No new placeholder or
hook: the stock block already sits near the top of the start G-code (before homing; on the A1 just after the heaters are set) and
a placeholder would need a profile change on 26 files. The label is a separate comment line
because no stock profile puts a comment on an M1006 line.

## Converter (`src/libslic3r/GCode/StartupSound.{hpp,cpp}`)

- MIDI format 0/1 (format 2 refused), PPQ and SMPTE time division, tempo map from all tracks,
  running status, note-on velocity 0 = note-off, size limits for untrusted files.
- Tracks merged; channel 10 (drums) skipped by default.
- Starts/ends snap to `quantise_ms` (50 ms); every start or end is a step boundary.
- At most 3 notes per step, highest first (melody on top); extras dropped and counted.
- Octave folding into [min_note, max_note]; volume = velocity / 127 x max_volume.
- Free voices repeat the top note (`unison_fill`, as the H2/H3 tunes do) - owner decision D3.
- Units come from rounded absolute times, so rounding does not drift over a long tune.
- Caps: `max_steps` (100) and `max_seconds` (10 s); notes cut by a cap count as dropped.

## Start-delay cost

The printer waits (`M1006 W`) for the whole tune before it goes on with the start G-code, so every second of tune is
a second added to every print. The stock H3C tune is about 63 units (~1.3 s at 20 ms). The default
10 s cap allows a noticeably slower start; the dialog shows the duration before inserting.

## GUI

Printer settings > Machine G-code > under Machine start G-code: **Startup sound...** button, shown
only when that G-code already has a tune block (so only M1006-capable Bambu printers). The dialog
picks a .mid, shows the options, the line count, duration, dropped/folded notes and the M1006
lines, and **Insert** replaces the block in the edited preset (the user saves it as usual). Options,
including the calibrated ms-per-unit, are remembered in the app config section `startup_sound`.

## Owner decisions

- D1: default `ms_per_unit` after calibration (and per printer, if the A1/X1 differ from the H3C).
- D2: default caps (10 s / 100 lines) - or lower, given the start-delay cost.
- D3: unison fill on by default?
- D4: also offer the end-of-print tune (same block format in Machine end G-code) in phase 2?
- D5: ship a few built-in tunes (public domain) or only "bring your own MIDI"?

## Ending sound (2026-11-13)

Same converter and dialog (`StartupSoundDialog::Target::End`), button under Machine end G-code.
End tunes in the stock profiles: X1/X1C (all nozzles inherit the 0.4 end G-code), A1, A1 mini, A2L,
P2S, H2C, H2D, H2D Pro, H2S, H3C, X2D - the same printers as the start tune. P1P, P1S and X1E have
no M1006 in either G-code, so they get neither button. No insertion where there is no tune: whether
their firmware plays M1006 is unknown, and the stock end tunes sit between purge/air filtration and
the final `M400` / `M18`, a point that is not the same across printers.
Settings: the calibration (ms per unit) is shared; the other options are kept per tune (section
`ending_sound`, falling back to the start tune's values until saved once).
