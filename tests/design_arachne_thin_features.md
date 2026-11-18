# Design: thin features, Arachne settings and gap fill

Status: DESIGN ONLY, 2026-11-17. Nothing here is built. No code, branch, build or PR was made for it.

Read at: `origin/main` = `63bd59427b`. Curved-tops concentric fill read at `origin/feat/curved-tops-p4` =
`0bd335e597` (not on main). Reference clones, read only: upstream Orca `C:/Dev/ext/OrcaSlicer_ref` `3bfabc9831`,
Bambu Studio `C:/Dev/BambuStudio` `9aacb21bd`, PancakeSlicer `C:/Dev/ext/PancakeSlicer` `174a578f` (AGPL: facts
only, nothing copied).

Every statement is marked by where it comes from: **[code]** read at the file:line given, **[measured]** read
back from G-code sliced with the baseline install's CLI (main `63bd59427b`, scratch datadir, no GUI),
**[computed]** from the code's formulas, **[memory]** from what I know of Cura's settings and not re-fetched
today, **[guess]**. Nothing was printed.

Test files, scripts and the raw numbers: `C:/Dev/EdgeSlicerTest/test_files/arachne_thin/` (README there).

The owner's problem (2026-11-17): Arachne rarely drops to a 0.22 mm line where text is very thin although a
0.4 nozzle could print it; the Arachne settings do not tell a user what the print will look like. Same day:
curved-top concentric loops left an unfilled strip along a shape's centre line and diamond gaps at sharp
corners, and neither "Apply gap fill: Everywhere" nor the wall generator changed that.

---

## 1. What happens today (facts)

### 1.1 The three findings that matter most

1. **Arachne does not drop a 0.22 mm stroke: it prints it as a 0.38 mm line.** With the defaults every
   feature from 0.19 to 0.42 mm wide comes out as the same 0.383 mm line [measured]. The line is up to
   twice as wide as the stroke, so thin text looks fat and neighbouring strokes merge. One setting causes
   it: `min_bead_width` = 85 % of the nozzle. Setting it to 45 % gives a 0.22 mm line [measured].
2. **That one setting also decides when one wall becomes two.** `min_bead_width` feeds three different
   things (the thin-feature line width, the 1-to-2 split and the add-a-centre-line threshold,
   `WallToolPaths.cpp:521-522,533` [code]). Lowering it for text turns every 0.47-1.0 mm wall on the part
   into two thin lines of 0.25-0.50 mm instead of one line up to 0.69 mm [measured]. Cura keeps these as
   separate settings [memory]; the Prusa/Orca port folded them into one. This is the one place where
   settings alone cannot give "thin text, normal walls elsewhere".
3. **On the owner's main printers the shipped process is not Arachne at all.** Every BBL process (X1C, H2C,
   H3C ...), the Snapmaker U1 and the Elegoo CC/CC2 ship `wall_generator = classic` with
   `detect_thin_wall = 0` [code, profiles]. There a feature under 0.40 mm is not printed at all, and a
   0.40-0.9 mm one is printed as two full 0.42 mm lines on top of each other [measured]. The Arachne
   settings are hidden (`ConfigManipulation.cpp:1421-1422` [code]) and do nothing until the generator is
   switched.

### 1.2 Arachne, end to end

Keys (all `PrintObjectConfig`, `PrintConfig.hpp:1802-1808`, so **per object, not per modifier** [code]).
Percent keys are of the **smallest nozzle diameter on the printer**, not of the line width
(`WallToolPaths.cpp:30-52`, `min_physical_nozzle_diameter`) [code].

| Key | Default | On a 0.4 nozzle | What it does (code) |
|---|---|---|---|
| `min_feature_size` | 25 % | 0.10 mm | Below this the feature gets no line (`WideningBeadingStrategy.cpp:49-50,77-78`). |
| `min_bead_width` | 85 % | 0.34 mm | (a) Thin features are widened to at least this (`WideningBeadingStrategy.cpp:47`). (b) Split threshold `2 x min_bead / outer width - 1`, (c) add-centre threshold `min_bead / inner width` (`WallToolPaths.cpp:521-522`). |
| `initial_layer_min_bead_width` | 85 % | 0.34 mm | Replaces `min_bead_width` on layer 0 only (`WallToolPaths.cpp:40-46`). |
| `wall_transition_length` | 100 % | 0.4 mm | Length over which a wall count change is spread. |
| `wall_transition_angle` | 10 deg | - | Wedges opening faster than this get no extra centre line (`PrintConfig.cpp:10492-10503`). |
| `wall_transition_filter_deviation` | 25 % | 0.1 mm | How far a line may leave its ideal width to avoid a short there-and-back transition. |
| `wall_distribution_count` | 1 | - | How many walls, counted from the centre, share a width change. |
| `min_length_factor` | 0.5 | - | Open centre lines shorter than factor x their width are removed; on top/bottom layers always 0.5 x width (`WallToolPaths.cpp:683-700`). |

The stack (`BeadingStrategyFactory.cpp:19-58` [code]): Distributed (spreads width changes) -> Redistribute
(outer walls keep their width) -> **Widening** (the thin-feature rule; always on, `fill_outline_gaps` is a
`constexpr true`, `WallToolPaths.hpp:18`) -> OuterWallInset (only with precise outer wall) -> Limited (caps
the count at 2 x wall loops).

Before Arachne sees the island it is shrunk (`PerimeterGenerator.cpp:2984-2986` [code]): by
`(outer width - outer spacing) / 2` per side, or the full difference per side with `precise_outer_wall` and
Inner/Outer order. At 0.2 mm layers and 0.42 mm lines that is 0.043 mm off the feature width, or **0.086 mm
with precise outer wall** (on in the BBL and Snapmaker presets) [computed, matches measured]. So the
thinnest printable feature is `min_feature_size` + 0.086 = 0.186 mm, not 0.10 [measured: wedge starts at
0.186].

Arachne works in "spacing" widths; the line width in the G-code and in the preview is 0.043 mm more
(`Flow.cpp:209-212`). The volume extruded equals layer height x Arachne width, so a 0.223 mm line carries the
material of a 0.18 mm wide stroke [code].

**What a feature of width w becomes** (X1C 0.4, 0.2 mm layers, outer 0.42 / inner 0.45, 2 wall loops, precise
outer wall on) [measured; full data in `measurements.txt`]:

| w (mm) | Arachne, defaults | Arachne, `min_bead_width` 45 % | Classic as shipped | Classic + Detect thin wall |
|---|---|---|---|---|
| 0.10 | dropped | dropped | dropped | dropped (prints from 0.14) |
| 0.15 | dropped | dropped (0.22 line with feature size 10 %) | dropped | 0.19 line |
| 0.20 | **0.38 line** (widened) | **0.22 line** | dropped | 0.24 line |
| 0.30 | 0.38 line (widened) | 0.26 line | dropped | 0.34 line |
| 0.40 | 0.38 line | 0.36 line | 2 x 0.38 overlapping | 0.44 line |
| 0.50 | 0.46 line | 2 x 0.25 | 2 x 0.42 overlapping | 0.54 line |
| 0.60 | 0.56 line | 2 x 0.30 | 2 x 0.42 overlapping | 0.64 line |
| 0.70 | 0.65 line -> 2 x 0.40 (split at 0.70) | 2 x 0.37 | 2 x 0.42 | 2 x 0.42 (from 0.65) |
| 0.80 | 2 x 0.44 | 2 x 0.41 | 2 x 0.42 | 2 x 0.42 |
| 0.90 | 2 x 0.48 | 2 x 0.45 | 2 x 0.42 | 2 x 0.42 |
| 1.00 | 2 x 0.53 | 2 x 0.50 -> 0.42 + 0.26 + 0.42 (from 1.01) | 2 x 0.42 + 0.16-0.21 gap fill | 0.42 + 0.21 + 0.42 |
| 1.20 | 0.42 + 0.40 + 0.42 (3 from 1.16) | 0.42 + 0.44 + 0.42 | 2 x 0.42 + 0.40 gap fill | same |

The split points match the code: one line until `(1 + split threshold) x outer spacing` + 0.086 =
1.619 x 0.377 + 0.086 = 0.70 mm with 85 %, 1.01 x 0.377 + 0.086 = 0.47 mm with 45 %
(`RedistributeBeadingStrategy.cpp:37,47` [computed, matches]).

Other things on the way out:

- **Widths are stepped.** Variable-width lines are cut into pieces of near-constant width with a 0.05 mm
  tolerance (`Arachne/utils/ExtrusionLine.cpp:289,296`), so along a taper the width moves in steps of about
  0.045 mm [measured on the wedge].
- **No hard floor on width.** `min_bead_width` 30 % produced a 0.163 mm line at a 0.2 mm layer without an
  error [measured]. `Flow::mm3_per_mm` throws only when the area is zero or negative (`Flow.cpp:221-230`);
  `Flow::with_spacing` throws when width < height (`Flow.cpp:160-161`) but the Arachne path does not call it.
  Nothing warns that a line is narrower than the layer is tall or far below the nozzle bore.
- **Speed and flow.** Thin lines are outer walls: `outer_wall_speed` applies; `small_perimeter_speed` only if
  the threshold is set (0 in the BBL preset). A 0.22 x 0.2 mm line at 200 mm/s is 7 mm3/s, far under any
  volumetric limit, so nothing slows it [computed]. Whether a 0.4 nozzle lays a clean 0.22 mm line at that
  speed is a print question [guess: it needs to be slower].
- **Short centre lines** are removed by `min_length_factor` (above). Closed loops and outer walls never are.
- `printable_slices()` (`PerimeterGenerator.cpp:3456-3469`) already computes "this layer minus everything
  thinner than `min_feature_size`" for the overhang logic. It is the cheapest source for a dropped-feature
  report (section 5).

### 1.3 Classic

- Without `detect_thin_wall` an island the outer line does not fit in is offset away. BBS added a narrower
  outer line for "narrow but short" islands (`PerimeterGenerator.cpp:1840-1856`), which gives the two
  overlapping 0.38-0.42 lines measured for 0.40-0.60 mm fins: up to twice the material the feature holds.
- With `detect_thin_wall` (Strength > Walls, `Tab.cpp:3616`; a region key, so a modifier can set it) the
  medial axis prints anything from nozzle / 3 = 0.13 mm up (`PerimeterGenerator.cpp:1824-1839`) at the
  feature's own width. It tracks width better than Arachne does today [measured], but thin walls are
  separate open lines with their own starts and stops.

### 1.4 Gap fill

- **Between Classic walls:** medial axis of whatever the offsets left, width 0.2 x line .. 2 x spacing
  (`PerimeterGenerator.cpp:2209-2246`). Not controlled by "Apply gap fill"; only `filter_out_gap_fill`
  touches it [code; measured: gap fill appears with `gap_fill_target = nowhere`].
- **Between Arachne walls: there is none.** The walls themselves change width instead. On a 1.3-2.5 mm taper
  Arachne filled 94-99 % everywhere with no centre sliver; Classic dropped to 49 % for a short stretch at
  1.52 mm and to 82 % at 2.37-2.46 mm [measured, `wd_*` cases, approximate fill figure].
  "Apply gap fill: Everywhere" changed nothing in the Arachne wall output (identical lines) [measured].
- **Inside solid infill:** `Fill::_create_gap_fill` (`FillBase.cpp:331-381`): area the fill lines did not
  cover -> medial axis -> variable-width lines, gated by `gap_fill_target` (Nowhere / Top and bottom /
  Everywhere) and `filter_out_gap_fill`. It needs a gap at least 0.17 x spacing wide (0.07 mm) and the
  medial axis discards very short pieces.
- **Why slivers stay at sharp corners of concentric loops:** nested offset loops leave a small diamond
  between consecutive corners (round line ends against a mitred path). At a 90 degree corner of 0.42 mm
  lines it is about 0.17 mm across and 0.2-0.4 mm long [computed]; the medial axis treats it as too short
  [guess, not measured]. Arachne's `wall_transition_angle` (10 deg) deliberately puts no extra line into
  wedges opening faster than that, and its tooltip says so (`PrintConfig.cpp:10495-10498`).

### 1.5 Solid infill of narrow regions

- `detect_narrow_internal_solid_infill` (default on) sends narrow internal solid areas to
  `ipConcentricInternal` (`Fill.cpp:1774-1800`).
- Concentric fills run Arachne with **hard-coded** limits, not the user's: `min_bead_width` 0.85 x nozzle,
  `min_feature_size` 0.25 x nozzle, angle 10, count 1 (`FillConcentric.cpp:117-123`,
  `FillConcentricInternal.cpp:29-35`, `FillSpiralInset.cpp:358-364`). Upstream Orca and PancakeSlicer have
  the same constants [code]. No wall-generator setting reaches them.
- Infill strips narrower than 0.85 x solid spacing are collapsed after the walls
  (`PerimeterGenerator.cpp:3408-3414`).
- "Only one wall on top surfaces" re-runs Arachne for the inner walls and uses `min_width_top_surface`
  (`PerimeterGenerator.cpp:3018-3138`). Thin raised text is top surface on every layer of its height, so its
  strokes are single outer lines and follow table 1.2. Not sliced separately for this document.

### 1.6 The curved-tops observation

On `feat/curved-tops-p4` the concentric pattern calls `FillConcentric` through `fill_surface_arachne`
directly (`CurvedTops.cpp:476-512` on that branch [code]). So:

- it uses the hard-coded Arachne limits of 1.5, which is why the wall generator and its settings had no
  effect;
- it never reaches `_create_gap_fill` (that is called from the normal fill path only), which is why "Apply
  gap fill: Everywhere" had no effect;
- the corner diamonds are the ones of 1.4.

The centre-line strip is **not explained** by what I read: planar Arachne fills a taper fully (1.4). Candidates
[guess]: the open centre line is clipped or dropped on its way into curved paths, or the fill area is inset
twice. First step for whoever takes it: slice the same shape planar with a concentric top pattern and compare.

---

## 2. What the profiles ship [code, `resources/profiles` at main]

| | `wall_generator` | Arachne keys | `detect_thin_wall` |
|---|---|---|---|
| Code default | arachne | 85 % / 25 % / 100 % / 10 / 25 % / 1 | off |
| BBL, all printers (`fdm_process_common.json:100`, `fdm_process_common_h3c.json:201`) | **classic** | not set | off |
| Snapmaker U1 (`fdm_process_U1_common.json:72`) | **classic** | inherits 85 % / 25 % | off |
| Snapmaker others (`fdm_process_common.json:57`) | arachne (three presets back to classic) | 85 % / 25 % | off |
| Elegoo CC, CC2 | **classic** | not set | off |
| Elegoo others, Voron | not set = arachne | not set | off |
| Prusa | arachne | 85 % / 25 % | off |
| Qidi X3 / N, Creality (90 of 94), Anycubic (126), Sovol (33) | classic | - | mixed |

Across all vendors: 342 process files say classic, 49 say arachne. Almost nobody changes the Arachne numbers:
243 of 254 files that set `min_bead_width` say 85 % (others 80 %, 90 %, "100"); `min_feature_size` is 25 % in
239 of 246 (four say 95 %, three 20 %); one file each has angle 59 and 25; three have distribution count 2.
`initial_layer_min_bead_width` is 100 % in 45 files. No shipped preset is tuned for fine detail.

---

## 3. What other slicers do

- **Upstream Orca and Bambu Studio:** same keys, same defaults (arachne, 85 %, 25 %), same single
  `min_bead_width`, same hard-coded concentric constants [code]. Neither has a thin-feature or small-feature
  option beyond `detect_thin_wall`. Orca has `gap_fill_flow_ratio`, which main does not.
- **PancakeSlicer:** same libslic3r Arachne. One relevant fact: its non-planar conformal shells call Arachne
  with `min_bead_width` = 0.5 x the line width instead of 0.85 x nozzle
  (`engine/src/libslic3r/Nonplanar/ConformalShells.cpp:232-233`) [code]. Fact only; no code taken.
- **CuraEngine, where Arachne comes from** [memory, not re-fetched; check before quoting in a wiki]:

| Cura setting (key) | Cura default, 0.4 nozzle | Here |
|---|---|---|
| Print Thin Walls (`fill_outline_gaps`) | off | always on |
| Minimum Feature Size (`min_feature_size`) | line width / 4 = 0.1 | `min_feature_size` 25 % of nozzle |
| Minimum Thin Wall Line Width (`min_bead_width`) | 85 % of line width = 0.34 | `min_bead_width` (a) |
| Minimum Wall Line Width (`min_wall_line_width`) | 0.34 | folded into `min_bead_width` |
| Minimum Even Wall Line Width (`min_even_wall_line_width`) | = minimum wall line width | folded into `min_bead_width` (b) |
| Minimum Odd Wall Line Width (`min_odd_wall_line_width`) | = minimum wall line width | folded into `min_bead_width` (c) |
| Split Middle Line Threshold / Add Middle Line Threshold | derived from the two above | derived, not shown |
| Wall Transition Length, Wall Distribution Count, Wall Transitioning Threshold Angle, Wall Transitioning Filter Margin | 0.4, 1, 10, 0.1 | same four keys |
| Wall Transitioning Filter Distance | 100 mm | constant 100 mm (`WallToolPaths.cpp:541`) |
| Fill Gaps Between Walls (`fill_perimeter_gaps`) | pre-Arachne only | - |

  Cura explains them by outcome: "features thinner than this will not be printed, features thicker will be
  widened to the Minimum Wall Line Width", and for the minimum wall line width that it also sets the widest
  line, because N walls become N+1 where N are at their widest [memory]. The tooltips here are those
  sentences, minus the split between "thin wall" and "even / odd" minimums that makes them true.

---

## 4. Settings-only experiment the owner can do today

**Recipe for 0.22 mm text strokes on a 0.4 nozzle** (Process > Quality > "Wall generator" group, Advanced):

| Setting | Value | Was |
|---|---|---|
| Wall generator | **Arachne** | Classic on BBL / U1 / CC presets |
| Minimum wall width (`min_bead_width`) | **45 %** | 85 % |
| Minimum feature size (`min_feature_size`) | 25 % (leave) | 25 % |
| First layer minimum wall width | 45 % only if the text sits on the bed | 85 % |

Result [measured]: strokes of 0.19-0.27 mm print as a 0.22 mm line, 0.30 as 0.26, 0.40 as 0.36. Strokes under
0.19 mm are still skipped; `min_feature_size` 10 % brings that down to 0.13 mm (they print as 0.22 mm lines).
Print time of the test plate: +2 s.

Side effects:

- **Walls elsewhere change.** Any wall 0.47-0.70 mm thick becomes two lines of 0.25-0.37 mm instead of one
  line of 0.43-0.65 mm, and a third centre line appears from 1.01 mm instead of 1.16 mm and can be as thin as
  0.26 mm [measured]. More, thinner lines: more seams and starts, probably weaker thin walls [guess], though
  one 0.65 mm line from a 0.4 nozzle is not ideal either.
- **Thin lines are fragile.** A 0.22 mm line is narrower than the nozzle bore. Expect it to need a slower
  outer wall (try 30-60 mm/s) and good cooling [guess, unprinted].
- Seams: every isolated thin stroke is its own open line with a start and an end, as today.
- It is per object. Right-click the object > add the three settings there to keep other objects unchanged.
  A modifier cannot carry them.

Cannot be reached with settings alone:

1. Thin text **and** unchanged walls on the same object (finding 2).
2. A line that follows the stroke below 0.22 mm (the floor is one number; `min_bead_width` 30 % gives 0.16 mm
   lines but splits walls from 0.47 mm and adds 0.20 mm centre lines).
3. The 0.086 mm the precise outer wall takes off every thin feature (turning it off gives 0.043 back
   [computed, not sliced]).
4. Anything on concentric fills or curved tops (hard-coded limits).
5. Knowing what was skipped or widened without hunting through the Line Width view.

Alternative without Arachne: keep Classic and tick Strength > Walls > "Detect thin wall". Prints from
0.14 mm at the stroke's own width and leaves normal walls alone [measured]. Worth printing side by side.

Ready to print: `C:/Dev/EdgeSlicerTest/test_files/arachne_thin/thin_X1C_arachne_tuned_bead45.gcode.3mf`
(X1C, Generic PLA, 13 min). The same folder has the default and the two Classic variants.

---

## 5. Design

### A. One "Fine detail" control

Options:

- **A1. Leave the raw keys, fix the words only (B).** Cheapest; does not solve finding 2.
- **A2. A preset-style switch that rewrites `min_bead_width` / `min_feature_size`.** No engine work, but it
  drags the wall split along (finding 2) and fights with users who set the raw keys.
- **A3. A new key that only sets the thin-feature floor, plus the engine change C1.** *Recommended.*

A3 in detail. New key `fine_detail` (enum, Process > Quality > Wall generator, first line under the
generator; Simple mode):

| Value | Thinnest line | Skips features under | Classic generator |
|---|---|---|---|
| Off (default) | `min_bead_width` (0.34) | `min_feature_size` (0.10) | as today |
| Balanced | 60 % of nozzle (0.24 spacing, 0.28 line) | as set | thin walls detected |
| Fine text | 45 % of nozzle (0.18 spacing, 0.22 line) | 15 % of nozzle (0.06) | thin walls detected |

- The floor is clamped so the **line is never narrower than the layer is tall** (spacing >= 0.785 x layer
  height). At 0.28 mm layers "Fine text" therefore gives 0.28 mm lines and the tooltip says to lower the
  layer height for finer text. Percent of nozzle keeps it right for 0.2 / 0.6 / 0.8 nozzles.
- It changes **only** `WideningBeadingStrategy`'s output width (and its input limit in "Fine text"). Split and
  add thresholds keep using `min_bead_width`, so walls of 0.47 mm and up are byte-identical to "Off".
- With the Classic generator it acts as `detect_thin_wall` for that object/region, so the control means the
  same thing on BBL and U1 presets without telling the user to change generator.
- Scope: `PrintRegionConfig`, so it works globally, per object and **per modifier** (a box around the text).
  `PerimeterGenerator` already has the region config next to the object config. The raw Arachne keys stay
  object-level.
- Raw keys stay, in Advanced, with the new wording of B. "Off" plus raw keys is exactly today.
- Migration: the key is absent from every old preset and project, absent means Off, Off is today's G-code.
  Old EdgeSlicer, Orca and Bambu Studio drop the unknown key and print as today. No preset is changed by the
  update; whether any shipped preset gets Balanced is D3.

### B. Clarity

New labels and tooltips (percent values also show the resulting mm for the current nozzle as side text,
e.g. "45 % = 0.18 mm"; the conversion is `make_paths_params`):

| Key | Label now | Proposed label | Proposed tooltip |
|---|---|---|---|
| `min_feature_size` | Minimum feature size | **Skip features thinner than** | "Parts of the model thinner than this are not printed at all. Anything thicker gets at least one line. Percent of the nozzle diameter. Lower it to keep hair-thin details; they will be printed wider than they are (see Thinnest line)." |
| `min_bead_width` | Minimum wall width | **Thinnest line** | "The narrowest line the slicer will print. A detail thinner than this is printed this wide, so very fine text comes out bolder than drawn. It also decides when a thin wall is printed as one wide line or two narrow ones: at 85 % a wall up to 1.6 line widths is one line, at 45 % it becomes two from 1.0. Percent of the nozzle diameter. For thin text without changing the walls use Fine detail instead." |
| `initial_layer_min_bead_width` | First layer minimum wall width | **Thinnest line, first layer** | "Same as Thinnest line, for the first layer only. Wider lines stick to the bed better." |
| `wall_transition_length` | Wall transition length | **Wall count change length** | "Where a part gets thinner and loses a wall, the change is spread over this length. Longer is smoother but leaves the lines off their ideal width for longer." |
| `wall_transition_angle` | Wall transitioning threshold angle | **No centre line in corners sharper than** | "In a wedge or corner that opens faster than this angle no extra centre line is added. Lower: fewer short centre lines, small unfilled tips in sharp corners. Higher: sharper corners are filled, with more short lines." |
| `wall_transition_filter_deviation` | Wall transitioning filter margin | **Allowed width drift before changing wall count** | "How far a line may be squeezed or stretched to avoid adding a wall and dropping it again a moment later. Higher: fewer starts and stops, less even line width." |
| `wall_distribution_count` | Wall distribution count | **Walls that absorb width changes** | "When a wall does not fit a whole number of lines, this many lines, counted from the middle, are made wider or narrower. 1 keeps all the outer lines at their set width." |
| `min_length_factor` | Minimum wall length | **Drop centre lines shorter than** | (keep; say "x their own width" instead of the "mm" side text, which is wrong: `WallToolPaths.cpp:693` multiplies by the width) |
| `detect_thin_wall` | Detect thin wall | keep, add | "Classic generator only. Prints parts thinner than two lines as one line of their own width, down to a third of the nozzle." |
| `gap_fill_target` | Apply gap fill | keep, shorten | first sentence: "Fills narrow leftovers inside solid infill. It does not act on Arachne walls (they change width instead) or on curved tops." |

Grouping: split the "Wall generator" group (`Tab.cpp:3556-3567`) into "Thin features" (Fine detail, Skip
features thinner than, Thinnest line, Thinnest line first layer, Drop centre lines shorter than) and "Wall
count changes" (the four transition keys, resolution, deviation).

One diagram per setting for the wiki page and, later, the tooltip image: a wedge seen from above, 0.1 mm at
one end and 1.3 mm at the other, with the lines drawn on it. Four markers on it carry four settings: where
the first line starts (skip thinner than), the constant-width stretch (thinnest line), the 1-to-2 split, the
transition length. `thin_fins_wedge.stl` sliced in the preview is that picture; screenshots of it with two
values each are the cheapest honest diagrams.

Preview aid, in rising cost:

- **B1. Slicing notice** (cheap): "Object X: features thinner than 0.10 mm were skipped on 34 layers
  (first at Z 1.2). 2.1 m of thin detail was widened to 0.34 mm. Fine detail prints these thinner."
  Data: skipped = `slices` minus `opening(slices, min_feature / 2)`, the function at
  `PerimeterGenerator.cpp:3459-3469`; widened = single lines whose width equals the floor (known in
  `process_arachne` when the paths are converted). Area and layer counters only, summed per object,
  below a threshold nothing is said. For Classic: skipped = islands whose `offsets` came back empty.
- **B2. Jump to it:** the notice carries the first layer and an XY, like the existing collision notices.
- **B3. Overlay** (later): keep the skipped polygons per layer and draw them in the preview as a toggled
  "Skipped detail" layer; mark widened lines with their own colour in the Line Width view (it exists:
  `GCodeViewer.cpp:116-117`). Needs the polygons carried from `Print` to the viewer.

### C. Engine changes

Each is off by default and must leave the standard G-code gate identical apart from config lines.

| # | Change | Where | Risk | Tests | Hours |
|---|---|---|---|---|---|
| C1 | **Separate thin-feature floor.** `WallToolPathsParams` gains `thin_bead_width` and `thin_feature_size`; the factory passes them to Widening; split/add thresholds keep `min_bead_width`. Absent = `min_bead_width`, i.e. today. | `WallToolPaths.hpp:33-40`, `.cpp:28-46,521-539`, `BeadingStrategyFactory.cpp:19-46` | Low. Widening already takes its two numbers separately. One thing to check: `Widening::compute` hands over to the parent at `getTransitionThickness(1)`, so a floor above the split point must be clamped. | Unit: wedge and fin outlines through `WallToolPaths`, bead count and width per thickness for Off / Balanced / Fine; gate identical with the key off; the fin plate as an extra gate case | 6-8 |
| C2 | **`fine_detail` key** (A3): enum, region level, Classic mapping, layer-height clamp, ConfigManipulation, Tab, invalidation next to `PrintObject.cpp:1916-1924`, 3MF round trip | `PrintConfig.cpp/.hpp`, `Preset.cpp:1295`, `PerimeterGenerator.cpp:2988`, `Tab.cpp:3556` | Low-medium: moving the decision to the region config must not change how regions are grouped (`Layer.cpp:245` compares region keys) | Round-trip test; modifier-over-text test; gate | 8-10 |
| C3 | **Slicing notice B1/B2** | `PerimeterGenerator.cpp` (both generators), `Print` warnings | Low; cost of one extra `opening` per island only when the notice is enabled | Unit: fin plate gives the expected counts | 6-8 |
| C4 | **Shared Arachne limits for fills.** One function replaces the three hard-coded blocks; a caller may pass its own floor. Same numbers by default. | `FillConcentric.cpp:117-123`, `FillConcentricInternal.cpp:29-35`, `FillSpiralInset.cpp:358-364` | Very low | Gate identical | 3-4 |
| C5 | **Width follows the stroke** instead of a flat floor: Widening outputs `max(thickness + shrink, floor)` so a 0.30 stroke is a 0.30 line, not 0.26, with precise outer wall on. | `WideningBeadingStrategy.cpp:44-52`, `PerimeterGenerator.cpp:2984-2994` | Medium: the line then overhangs the shrunk outline it was computed on; overlaps neighbours in tight text | Fin plate widths equal the fins within 0.02 | 6-10, after the owner's print |
| C6 | **Corner and centre-line gap fill for Arachne fills and curved tops:** after the loops, uncovered = area minus lines at their widths -> opening -> medial axis -> the existing `variable_width` lines; shorter pieces than the medial axis accepts become a short straight dab. | new helper beside `Fill::_create_gap_fill` (`FillBase.cpp:331`), reused by curved tops in 2D before Z is applied | Medium-high: many tiny extrusions, stringing, time; needs `filter_out_gap_fill` and a flow cap | Corner test shapes (30 / 60 / 90 deg), covered-area figure before and after | spike 8-10, then 16-24 |
| C7 | **Widening through sharp corners** (raise flow on the corner segment by the diamond's area instead of adding a line) | `FillConcentric.cpp` after the loops; Arachne walls later | Medium: blobs on small radii | same shapes as C6 | 10-14, alternative to C6 |
| C8 | **A separate "detail" beading strategy** (own even / odd minimums as in Cura) | new strategy class | High, and C1 gets most of it | - | not proposed |
| C9 | **Thin-feature pass for dropped slivers under `min_feature_size`** | - | Not needed: lowering the limit already prints them [measured, feature size 10 %] | - | 0 |

Classic's double-line over-extrusion on 0.40-0.9 mm features (finding 3) is a BBS behaviour worth its own
small look, outside this design.

### D. What the curved-tops generator can share

- **C4's function** is the piece curved tops needs first: its concentric fill then takes the same limits as
  planar fills, and can be given the Fine-detail floor for its own surface.
- **C6/C7 work in 2D on the loops before they are draped**, so the same helper fills corner diamonds for
  curved tops. Curved tops calls `fill_surface_arachne` directly and skips gap fill today (1.6); it should
  call the shared helper explicitly rather than depend on `gap_fill_target`.
- **C3's covered-area figure** ("x % of this surface has no line over it") is the natural check for the
  centre-line strip and for the curved-tops tests.
- Not shared: the centre-line strip itself (unexplained, 1.6), seams, Z mapping, flow on slopes.

---

## 6. Tests and gates

- Unit (`tests/libslic3r`): `WallToolPaths` on a wedge and on fin outlines; expected bead count and width per
  thickness for defaults (table 1.2 becomes the regression test for today's behaviour), Off / Balanced / Fine,
  0.2 / 0.4 / 0.6 nozzles, 0.12 / 0.2 / 0.28 layers (clamp).
- `fff_print_tests`: fin plate end to end, Arachne and Classic, modifier over half the fins.
- G-code gate (`gcode-gate`, standard set): identical with every new key at its default, for each of C1-C4.
  Add `thin_fins_wedge.stl` on the X1C and the U1 as kept extra cases with Fine text on.
- Hand prints (owner): section 4's plate now; the same plate with Fine detail after C2; real embossed text
  (0.2-0.4 mm strokes) on an X1C or H2C and on the U1; a 0.2 mm nozzle run to see the percent rule holds.

---

## 7. Phasing

| Phase | Content | Hours | Needs |
|---|---|---|---|
| P0 | Owner prints the tuned plate (section 4), ideally next to the Classic + thin wall one | 0 | - |
| P1 | **Words** (B): labels, tooltips, mm side text, two groups, wiki page with wedge screenshots. No G-code change. | 6-8 | - |
| P2 | **Engine split + Fine detail** (C1 + C2) | 14-18 | P0 result picks the floors |
| P3 | **Slicing notice** (C3, B1/B2) | 6-8 | - (can run beside P2) |
| P4 | **Shared fill limits** (C4) | 3-4 | - |
| P5 | **Corner gap fill spike** (C6 vs C7) on planar concentric, then curved tops | 8-10 spike, 16-24 build | P4; curved-tops P4 merged |
| P6 | Width follows the stroke (C5); preview overlay (B3, 12-16) | 6-10 + 12-16 | P2, owner's prints |

P1, P3 and P4 are independent and safe to run at once. One PR per phase.

---

## 8. Decisions for the owner

**D1. One "Fine detail" option, or only clearer raw settings?**
- (a) Fine detail (Off / Balanced / Fine text) on top of the engine split, raw keys stay in Advanced. *Recommended.*
- (b) Clearer words only (P1) and the recipe of section 4.
- (c) Expose Cura's separate even / odd minimums as two more raw keys.

**D2. Floors for Balanced and Fine text.**
- (a) 60 % and 45 % of the nozzle (0.28 and 0.22 mm lines on a 0.4), never narrower than the layer height;
  confirm after the P0 print. *Recommended.*
- (b) Fine text at 50 % (0.24 mm line) if the 0.22 lines do not hold.
- (c) A free number instead of named levels.

**D3. Default for shipped presets.**
- (a) Off everywhere; nothing changes for anyone until chosen. *Recommended.*
- (b) Balanced on EdgeSlicer's own BBL / U1 / CC presets (changes G-code of existing thin features; they would
  print thinner and, on Classic, appear where they were skipped).

**D4. Fine detail on the Classic generator.**
- (a) It turns on thin-wall detection for that region, so the option works on the shipped BBL / U1 presets. *Recommended.*
- (b) Arachne only; greyed with a hint on Classic.
- (c) Change the BBL / U1 / CC presets to Arachne (a large G-code change; not proposed).

**D5. Renames.**
- (a) New labels and groups as in B; keys unchanged, old names kept as search aliases. *Recommended.*
- (b) Keep upstream labels, change tooltips only (easier upstream merges and web help).

**D6. Skipped / widened notice.**
- (a) Slicing notice with counts and a jump-to location (P3); overlay later if wanted. *Recommended.*
- (b) Overlay from the start. (c) Nothing.

**D7. Corner diamonds and curved-tops gaps.**
- (a) Spike gap fill against corner flow boost on planar concentric first (P5), then give curved tops the
  winner. *Recommended.*
- (b) Fix inside the curved-tops work only.

**D8. Line narrower than the stroke with precise outer wall (C5).**
- (a) Decide after the P0 print shows whether 0.26 for a 0.30 stroke matters. *Recommended.*
- (b) Build it with P2.
