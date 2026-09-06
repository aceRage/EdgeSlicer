# Offset layers on the classic wall generator

2026-09-06, branch `feat/offset-layers-classic` off `feat/ultra-preferences` (4734a41e67).

"Offset layers (experimental)" raises every odd-numbered wall by half a layer height so that the
walls of successive layers interlock instead of stacking. It arrived in this fork on the Arachne
wall generator only, and the GUI refused to switch it on for anything else. This note records what
it took to make the classic generator produce the same thing, and where the two still differ.

## What the feature actually is

Three effects, and all three live on `ExtrusionPath`:

* `z_offset` - a **fraction of the path's own height**, not a millimetre value. `GCode::_extrude`
  reads it as `m_nominal_z + path.z_offset * path.height`, so 0.5 on a 0.2 mm layer is +0.1 mm.
  The same field decides whether a Z travel is emitted at all when the raised path starts where the
  flat one ended (`slope_need_z_travel`), which is why a raised wall does not silently print flat.
* `extrusion_multiplier` - scales the flow: `_mm3_per_mm = path.mm3_per_mm * path.extrusion_multiplier
  * print_flow_ratio`, before the filament flow ratio and the role-based ratios.
* the ordering rule - the flat (even) walls of an island are laid down before the raised (odd) ones,
  because a raised wall has to have something under it to key into.

Per layer, for an object of `N` layers and only when `N >= 4`:

| layer | odd walls |
|---|---|
| 1 (the second layer) | `z_offset = 0.5`, `extrusion_multiplier = 1.5` - over-extrude to bond down onto the flat layer below |
| `N - 2` | no raise, `extrusion_multiplier = 0.5` - under-extrude so the flat top layers close cleanly |
| every other layer | `z_offset = 0.5`, multiplier 1 |

Even walls, thin walls and gap fill are never touched.

Because the G-code writer consumes nothing but those two `ExtrusionPath` fields, porting the feature
is entirely a question of setting them in the other generator - nothing downstream needed a line.

## What was ported

`src/libslic3r/PerimeterGenerator.cpp`

* The per-layer rule was a lambda inside the Arachne path (`check_and_offset_path`). It is now the
  free function `offset_layers_apply(perimeter_generator, path)` sitting above `traverse_loops`, and
  **both** generators call it, so the two cannot drift apart. The Arachne call site is otherwise
  unchanged.
* Classic (`traverse_loops`): after a loop's `ExtrusionPaths` are built and before they become an
  `ExtrusionLoop`, a loop of odd `PerimeterGeneratorLoop::depth` gets `path.inset_idx = depth` and
  `offset_layers_apply` on every one of its paths. `depth == 0` is the external perimeter (see
  `PerimeterGeneratorLoop::is_external()`), which is the same convention Arachne's `inset_idx` uses,
  so "odd" selects exactly the same walls in both. The whole block is behind
  `config->offset_layers`, so with the feature off not one line of it runs.
* Classic ordering (`process_classic`): inner-outer-inner is disabled while the feature is on,
  matching the Arachne path, and just before `defer_unsupported_loops` the island's entities go
  through a `std::stable_partition` that puts every flat wall ahead of every raised one.

`src/slic3r/GUI/ConfigManipulation.cpp` - the prerequisite dialog no longer demands Arachne, and no
longer switches the wall generator to Arachne when the user accepts the fixes. The other three
prerequisites stand: first layer height equal to layer height, top surface line width equal to outer
wall line width, spiral vase mode off.

`src/libslic3r/PrintConfig.cpp` - the tooltip says "works with either wall generator".

## How the ordering is done, and why not the same way

Arachne orders its walls by a nearest-neighbour walk over `ordered_extrusions` and then re-sorts
that vector. Classic builds an `ExtrusionEntityCollection` per island, whose order is decided by
`chain_extrusion_entities` and then by the `wall_sequence` handling (`entities.reverse()` for
outer-first, the sandwich-mode block for inner-outer-inner). The offset partition therefore has to
run **last**, after whichever of those ran, or the wall sequence would undo it.

Two deliberate differences from the Arachne code:

* Classic uses `std::stable_partition`, Arachne uses `std::sort` with the comparator
  `a.inset_idx % 2 <= b.inset_idx % 2`. A stable partition keeps the order the wall-sequence logic
  chose inside each of the two groups; the sort does not, and its `<=` is not a strict weak ordering
  (it reports `comp(a, a) == true`), which is undefined behaviour for `std::sort`. That is
  pre-existing Arachne behaviour and was left alone here on purpose - see "Not done" below.
* Classic has entities with no wall index. Thin walls come out of `variable_width` with
  `inset_idx == -1`, and `-1 % 2` is `-1` in C++, so an unguarded parity test would file them with
  the raised walls. They are never raised, so the predicate treats `inset_idx < 0` as flat.

## What stays flat

Gap fill never reaches `traverse_loops`: `process_classic` builds it separately into
`this->gap_fill` through `variable_width`. Thin walls do go through `traverse_loops` but are
appended from the `thin_walls` list, not from a `PerimeterGeneratorLoop`, so they never see the
offset block. Both keep `z_offset == 0` and `extrusion_multiplier == 1`, which the test asserts.

## Tests

`tests/fff_print/test_offset_layers.cpp`, four cases on a 20 mm cube (100 layers, 3 walls):

* off mode, classic and Arachne - every path in `perimeters` and `thin_fills` must have
  `z_offset == 0` and `extrusion_multiplier == 1`;
* on mode, classic and Arachne - the per-layer table above holds for every odd wall of every layer,
  the even walls stay flat, no flat wall follows a raised one inside an island, and the thin fills
  are flat.

The Arachne case is there to lock the behaviour that shipped now that both generators share one
helper.

## Not done / not verified

* The Arachne `std::sort` comparator's undefined behaviour was left in place, because touching it
  would change Arachne's on-mode output and this change is a port, not a fix. Worth a separate
  one-character change (`<=` to `<`, or `stable_partition`).
* Adaptive / variable layer height is still unsupported, as before: `z_offset` is a fraction of the
  path height, so a layer whose neighbour has a different height would not interlock cleanly.
* The GUI prerequisite dialog is unchanged apart from the Arachne clause; it was not exercised
  interactively in this work.

## Measured (2026-09-06)

**Off mode is byte-identical.** `tests/supportgroup_test.3mf`, `--wall-generator=classic
--offset-layers=0`, pinned single-core CLI slice against an isolated `--datadir`, baseline = the
live `EdgeSlicer.exe` at 47ec4f1e69 (whose `src/libslic3r/` is identical to this branch's base -
`git diff 47ec4f1e69 4734a41e67 -- src/libslic3r/` is empty). Two plates, 10 220 604 and 2 231 995
bytes: **two differing lines each, and both are the `; generated by` line.** The same exe against
itself differs on exactly that line too, so the baseline is self-reproducible and the comparison is
meaningful.

**On mode, a 20 mm cube** (100 layers at 0.2 mm, first layer 0.2 mm, three walls, 0.42 mm outer and
top line width). Wall index 1 is the only odd wall, so it is the raised one:

| layer | odd wall Z | odd/flat inner-wall E per mm |
|---|---|---|
| 0 (Z 0.2) | 0.3 | 1.0000 |
| 1 (Z 0.4) | 0.5 | **1.5000** |
| 2 (Z 0.6) | 0.7 | 1.0000 |
| 50 (Z 10.2) | 10.3 | 1.0000 |
| 97 (Z 19.6) | 19.7 | 1.0000 |
| 98 (Z 19.8) = N-2 | **19.8, no raise** | the two inner walls merge into one block at 0.024695 mm/mm, which is exactly the length-weighted mix of 0.033172 and **0.5** x 0.033171 |
| 99 (Z 20.0) | 20.1 | 1.0000 |

Every layer reports FLAT BEFORE RAISED: the last flat wall move precedes the first raised one, with
no interleaving. Over the whole file only the walls are ever off the nominal Z - 7435.24 mm of inner
wall at +0.1 and 7259.48 mm at +0.0, against 7824.48 mm of outer wall, 641.74 mm bottom surface,
4199.02 mm internal solid infill, 13 089.81 mm sparse infill, 825.13 mm top surface and 767.17 mm
internal bridge, all at +0.0. The same cube with the feature off has 14 694.72 mm of inner wall,
all at +0.0 - the sum of the two, the same walls, none raised.

**Arachne on the same cube produces the same pattern**, number for number: 7435.24 mm raised,
7259.48 mm flat, 1.5000 on layer 1, 0.024695 on layer 98, FLAT BEFORE RAISED everywhere. The only
difference is the order inside the flat group (Arachne lays the outer wall first, classic the inner
one, which is the configured wall sequence and predates this change).

**Gap fill stays flat.** The cube produces none, so this was measured on
`tests/supportgroup_test.3mf` with classic + offset on: 115.34 mm of "Gap infill", all at +0.0,
against 54 501.21 mm of raised inner wall. That slice also shows the caveat in the flesh - it has a
0.25 mm first layer against 0.2 mm layers, and its raised walls on that layer sit at +0.125, half of
*their own* height. That is precisely why the prerequisite dialog insists the two heights match.

**Tests.** `fff_print_tests "[OffsetLayers]"`: 4 cases, 2410 assertions, all passing. The rest of
the suite fails identically before and after this change - 9 cases / 23 assertions on the full run
and a SIGSEGV in `test_support_material.cpp:166`; running the suites that sit after that abort
point gives 4 failed cases / 4 failed assertions on both binaries. Every count is the same on the
branch's binary and on the pre-change one built from `feat/ultra-preferences`. Those are the known bare-filename
`create_directory("")` and stale-option-name failures noted in `test_over_support_surfaces.cpp`.
