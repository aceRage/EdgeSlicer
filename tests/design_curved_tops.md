# Curved (non-planar) tops: design note

Status: **experimental, off by default** (`curved_tops_enabled`). Started as a spike on
`feat/curved-tops-spike` (owner OK 2026-11-12, D2/D3 in `tests/ideas_pancakeslicer.md`), PR #1021.
Works **only on printers whose head clearance file records a curved print**: the Elegoo Centauri
Carbon and the Bambu Lab X1 Carbon (see "Head clearance"). On any other printer the setting is
refused: the G-code is the same as with the setting off, and a slicing warning
(`SlicingCurvedTopsUnavailable`) says why. With the setting off, the G-code is byte-identical to a
build without the feature.

## What it does

- Process options (Quality > Z contouring group, object level): `curved_tops_enabled` (off),
  `curved_tops_layers` (3), `curved_tops_max_angle` (15 degrees, further capped by the head).
- `src/libslic3r/CurvedTops.cpp` runs inside the existing posContouring step, after Z contouring (ZAA),
  on the same raycast mesh:
  1. Surfaces: up-facing facets within the max angle with nothing above them, unioned in XY; a piece is
     kept when it rises at least one layer, is at least 4 mm2, and rises no more than the head's nozzle
     clearance (the fich-l "max printing height" rule).
  2. Clearance guard: no material outside the surfaces may reach into the curved band (else refused).
  3. Lowered planar body: inside a surface, planar extrusions survive only where `print_z <= S(x,y) - N*h`
     (clipped with a mesh slice at `print_z + N*h`). Walls, skins, ironing above that are clipped away.
  4. N conformal layers (one wall loop + rectilinear solid, 45/135 degrees) placed at `S(x,y) - (N-1-k)*h`,
     appended in order to the object's top layer so they print after every planar layer.
- Emission reuses ZAA: the paths carry `ContourZSamples` with a `curved_top` flag, so the emitter
  already writes per-point Z. For curved paths only (`GCode::curved_tops_travel_to`): retract and wipe
  where the nozzle is, lift straight up to (highest surface + 1 mm), travel level (no avoid-crossing
  detour, no z-hop of any type on top), descend vertically; flow from a per-sample multiplier (the first
  curved layer fills the 1-2 layer stepped gap the planar body left); speed capped at 40 mm/s, overhang
  speeds included (the CC1's Z axis is 20 mm/s, 100-500 mm/s2); ZAA speed scaling and
  `zaa_max_segment_rate` thinning do not apply to curved paths.
- The empty-layer check and the layer count (`; total layer number`, M73) are told about the curved
  block (`Layer::curved_tops_block`): the planar layers under it are emptied on purpose and not printed.

## Head clearance

Since P1 of `tests/design_curved_tops_any_printer.md` (branch `feat/curved-tops-p1`) the hard-coded
table is gone. One resolver, `resolve_head_clearance()` (HeadClearance.cpp), picks the head model from
`resources/head_clearance/` by `printer_model`; the planner, the Z contouring warning and the curved
tops check all use it. No head file = refused. From the head (`curved_tops_head_limits()`):

- allowed rise = lowest head box above the tip - 0.5 mm margin;
- head angle cap = nozzle flank angle (profile points more than 0.1 mm above the tip, seen from the
  tip land's edge) - 5 deg, at most 30 deg; the effective angle is min(`curved_tops_max_angle`, cap).

| Printer model | Lowest part | Allowed rise | Angle cap | Usable in P1 |
|---|---|---|---|---|
| Bambu Lab X1 Carbon | sock 2.5 mm (owner measured) | 2.0 mm | 30 deg | yes: the file records the Dome40 print (1.8 mm, 10.3 deg) |
| Elegoo Centauri Carbon | sock 2.5 mm (guess) | 2.0 mm | 30 deg | yes: the file records the Dome40 print |
| Bambu Lab X1, X1E; Snapmaker U1; Bambu Lab H2D | estimates | - | - | no: estimate nobody printed curved tops with (P2 acknowledgement) |
| anything else | - | - | - | no: no head data |

P1 also: every refusal is a slicing warning naming its reason (`SlicingCurvedTopsUnavailable`), a
built block gets a summary line (`SlicingCurvedTopsSummary`), the block ends with a retract + lift to
the safe Z (R5), wipes on curved lines are held to 40 mm/s, and `check_head_clearance()` runs on
curved prints and stops the export on a contact (D2). New refusals: by-object, more than one filament,
smooth timelapse tower, local-Z / sub-layer plans, mixed nozzle diameters, draft shield or skirt taller
than one layer, supports or stabilizers on the object, safe travel Z above the printable height.

## Source choice and credit

fich-l/OrcaSlicer-Nonplanar v0.1 (AGPL-3.0) is the idea source (Ahlers-style: facet selection,
grouping, max printing height, deferred top layers). PancakeSlicer's `Nonplanar/CurvedTops*.cpp`
(AGPL-3.0) was read as a reference. Both sit on upstream's `Polyline3` geometry, which this fork does
not have, so no code was copied: this is a re-implementation on top of our ZAA plumbing. Credited in
`CurvedTops.hpp`.

## Results

- Tests: `tests/fff_print/test_curved_tops.cpp` (off = no conformal path and extrusion Z never goes
  back down; unknown printer = same G-code as off plus the warning; CC1 and X1C curve; on = conformal paths only on the top layer, extrusion Z dips back
  by the cap rise; travels in the curved block level and clear for every z-hop type with wipe on/off;
  the stock X1C process keeps its layer bookkeeping; helper checks).
- Hand test: the owner printed `C:/Dev/EdgeSlicerTest/test_files/CurvedTops_Dome40_X1C.gcode.3mf`
  (40 mm puck, 5 mm cylinder + 1.8 mm cap, X1C 0.20mm Standard, Bambu PLA Basic, 3 curved layers) on
  the X1 Carbon: it came out good (2026-11-16). The same dome for the CC1
  (`CurvedTops_Dome40_CC1.gcode.3mf`) printed well earlier, per the hand-test card.

## Limits

- One object, one instance, one nozzle; no raft; no spiral vase; no variable layer height; refused
  otherwise (log line only; only the unknown-printer refusal has a slicing warning).
- Surface rise capped by the nozzle clearance: a cap may rise at most 2.0 mm. A hemisphere dome is
  mostly too steep and too tall; only its gentle top would qualify.
- The conformal layers are printed as one block at the end of the top layer: cooling sees one long
  layer and the preview shows them as part of the last layer.
- Rim: the conformal wall is a single loop; the planar side wall below it is clipped where it entered
  the band. A visible line at the rim is likely.
- The first curved layer lies on a stepped planar top (sparse infill where skins were clipped), bridging
  0-1 layer gaps with extra flow (up to 2x).
- No collision verifier over the curved block. The safety argument is only: surface rise <= nozzle
  clearance, slope <= cone, travel always at highest surface + 1 mm.

## Still to do (estimate)

| Item | Effort |
|---|---|
| Clearances from #1004's head-clearance model; more printers once measured | S |
| #1004's head collision check over the curved block | S-M |
| Emit curved layers as their own layers (preview, cooling, timelapse) | M |
| Planar body: regenerate skins on the lowered surface instead of clipping | M-L |
| Multi-object / by-object sequence, multi-tool printers (wipe tower, H2C rack) | L |
| GUI: region preview, refusal reasons as slicing warnings, paint enforce/block | M |
