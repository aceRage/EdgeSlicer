# Spike: curved (non-planar) tops for the Elegoo Centauri Carbon

Status: SPIKE, branch `feat/curved-tops-spike`, owner OK 2026-11-12 (D2/D3 in `tests/ideas_pancakeslicer.md`).
Off by default; byte-identical G-code when off (gate below).

## What was built

- Process options (Quality > Z contouring group, object level): `curved_tops_enabled` (off),
  `curved_tops_layers` (3), `curved_tops_max_angle` (15 degrees, further capped by the head).
- `src/libslic3r/CurvedTops.cpp` (~420 lines) runs inside the existing posContouring step, after ZAA,
  on the same raycast mesh:
  1. Surfaces: up-facing facets within the max angle with nothing above them, unioned in XY; a piece is
     kept when it rises at least one layer, is at least 4 mm2, and rises no more than the head's nozzle
     clearance (the fich-l "max printing height" rule).
  2. Clearance guard: no material outside the surfaces may reach into the curved band (else refused).
  3. Lowered planar body: inside a surface, planar extrusions survive only where `print_z <= S(x,y) - N*h`
     (clipped with a mesh slice at `print_z + N*h`). Walls, skins, ironing above that are clipped away.
  4. N conformal layers (one wall loop + rectilinear solid, 45/135 degrees) placed at `S(x,y) - (N-1-k)*h`,
     appended in order to the object's top layer so they print after every planar layer.
- Emission reuses ZAA: the paths carry `ContourZSamples` with a new `curved_top` flag, so the emitter
  already writes per-point Z. New for curved paths only: lift to (highest surface + 1 mm) before each
  travel, travel straight (no avoid-crossing detour, it would drop to layer Z), descend vertically; flow
  from a per-sample multiplier (first curved layer fills the 1-2 layer stepped gap the planar body left);
  speed capped at 40 mm/s (the CC1's Z axis is 20 mm/s, 100-500 mm/s2; at the role speeds of 200-250 mm/s
  the firmware would throttle every segment); ZAA speed scaling skipped.
- The G-code empty-layer check is told about the curved block (`Layer::curved_tops_block`), since the
  planar layers under it are emptied on purpose.
- Head clearance: the head-clearance loader on `feat/head-clearance-zaa` had no commit yet, so the CC1 is
  hard-coded (TODO in `curved_tops_head_clearance`): nozzle stands 2.0 mm proud of the heater block /
  sock / duct, cone clears 20 degrees. NOT measured. Every other printer: 1.5 mm / 12 degrees.

## Source choice and credit

fich-l/OrcaSlicer-Nonplanar v0.1 (AGPL-3.0) is the cleaner idea source (Ahlers-style: facet selection,
grouping, max printing height, deferred top layers). PancakeSlicer's `Nonplanar/CurvedTops*.cpp`
(~4k lines, AGPL-3.0) is a heavy extension of it and sits, like fich-l, on upstream's `Polyline3`
geometry, which this fork does not have. Neither could be lifted as code: this is a re-implementation
on top of our ZAA plumbing. Credited in `CurvedTops.hpp` and the commit message. Nothing from either clone
was built or run.

## Results

- Unit/slice tests: `tests/fff_print/test_curved_tops.cpp` (off = no conformal path and extrusion Z never
  goes back down; on = conformal paths only on the top layer and the extrusion Z dips back by the cap rise;
  helper checks). Build 1515: libslic3r_tests 3255 passed (3 expected fails), fff_print_tests 998/998.
- Test print `C:/Dev/EdgeSlicerTest/test_files/CurvedTops_Dome40_CC1.{3mf,gcode.3mf}` (+ README): 40 mm puck,
  5 mm cylinder + 1.8 mm cap, CC1 0.20 mm Standard, Elegoo PLA: 1 surface (Z 5.0-6.8), 3 layers, 6 paths,
  11.2 min. Curved block = last layer from 56 %; extrusion Z 4.64-6.80; 6 lifts to Z 7.8; median segment
  0.50 mm (156 segments under 0.2 mm); rim wall runs at 10 mm/s (overhang speed: the layer below is
  small), fill at 40 mm/s.
- G-code gate (standard set, install of build 1515 vs shared baseline a71f57827c): 8 CONFIG-ONLY (the three
  new keys), 2 DIFF (u1_2col, u1_t4): only removed `SET_PRESSURE_ADVANCE ADVANCE=0.0200` lines in the
  wipe tower (50 and 255), which is main's #1002 (wipe-tower pressure advance, merged after the baseline
  and merged into this branch). No diff from this branch: off is byte-identical. An earlier run of the
  pre-merge code against baseline c2a15847be was 10/10 CONFIG-ONLY.
- Not yet printed. Nothing about surface quality or clearance is verified.

## Limits (spike)

- One object, one instance, one nozzle; no raft; no variable layer height; refused otherwise (log line).
- Surface rise capped by the nozzle clearance: on the CC1 a cap may rise at most 2.0 mm. A "real" dome
  (hemisphere) is mostly too steep and too tall; only its gentle top would qualify.
- The conformal layers are printed as one block at the end of the top layer: cooling sees one long layer,
  the preview shows them as part of the last layer, and the time estimate is right but the layer count
  is not.
- Rim: the conformal wall is a single loop; the planar side wall below it is clipped where it entered
  the band. Visible seam/line at the rim is likely.
- The first curved layer is laid on a stepped planar top (sparse infill where skins were clipped), bridging
  0-1 layer gaps with extra flow (up to 2x). Expect some sag over sparse infill.
- No collision verifier (PancakeSlicer N2). The safety argument is only: surface rise <= nozzle clearance,
  slope <= cone, travel always at highest surface + 1 mm.
- Lifts are not preceded by a retraction (travel_to retracts after the lift): small strings possible.
- Fill is one connected rectilinear polyline per curved layer (6 paths in total); the solid fill sits
  inside a single wall, so the rim is thin.
- No GUI warning when the feature refuses; only a log line.

## Effort to productionise (estimate)

| Item | Effort |
|---|---|
| Head model from feat/head-clearance-zaa (measured CC1, per-printer data) | S, once that lands |
| G-code clearance verifier over the curved block (port PancakeSlicer N2 idea) | M |
| Proper ordering: emit curved layers as their own layers (preview, cooling, layer count, timelapse) | M |
| Planar body: regenerate skins on the lowered surface instead of clipping (slice a lowered mesh) | M-L |
| Multi-object / by-object print sequence, multi-tool printers (wipe tower, H2C rack) | L |
| GUI: region preview, refusal reasons as slicing warnings, paint enforce/block | M |
| Tests: G-code level checks on more shapes, gate cases | S |

Total: roughly 3-5 weeks to something shippable as "experimental", most of it in ordering and the
verifier. Recommendation: hand-test this spike first; go further only if the top surface is visibly
better and nothing drags.
