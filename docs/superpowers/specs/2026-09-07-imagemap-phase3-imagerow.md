# Image Row: Phase 3 (sub-triangle resolution)

Date: 2026-09-07
Branch: `feat/imagemap-p3-imagerow`, from `feat/imagemap-p2-imagefill`
Worktree: `C:\Dev\SnapmakerOrcaPhone\.claude\worktrees\imagemap-p3`

Plan: `docs/superpowers/specs/2026-09-07-imagemap-edgeslicer-plan.md`, Phase 3.
Companion: `docs/superpowers/specs/2026-09-07-imagemap-phase2-imagefill.md` (Phase 2, painting).

Phase 3's premise: stop being limited by the model's own triangles. An `ImageWeighted`
`MixedFilament` row samples the image directly along a fill segment, at extrusion-width
resolution, instead of the facet-resolution `mmu_segmentation_facets` painting Phase 2 writes.

## Steps 1-3 (already on this branch before this session)

- The data model: `MixedFilament::DistributionMode::ImageWeighted`, `MixedFilament::image_fill_ref`
  (a hex-encoded `ImageFillParams::to_string()`), and the hex codec
  (`MixedFilamentManager::encode_image_fill_ref` / `decode_image_fill_ref`).
- The sampler: `image_fill_sample_pixel`, `ImageRowSample`, `image_fill_sample_segment` - samples
  the image (or the row's gradient) along a straight segment in mesh space, reusing
  `image_fill_project` so a point on a facet's own plane reads exactly what Phase 2's facet
  painting would have painted there.
- The dither: `ImageRowRun`, `image_fill_dither_segment` - 1-D Floyd-Steinberg-style error
  diffusion against a small palette of reachable colours, coalesced into runs, with a minimum run
  length so nothing shorter than the nozzle can print survives.

All of the above was pure and unit-tested (652 cases, 2 expected failures) and, before this
session, unreferenced by the slicing pipeline - `SameLayerPointillisme`'s own "XY split" turned
out to be dead code (`#if 0` in `PrintObjectSlice.cpp`, `GCode.cpp`, `PrintApply.cpp`; it is
actually a per-LAYER sequence generator, not a spatial split), so step 4 had nothing to hook into
and had to build the real split.

## Step 4 (this session): the pipeline wiring

### The design, in one paragraph

On a `PrintObject` whose top-solid-infill filament (`PrintRegionConfig::solid_infill_filament`)
resolves through `MixedFilamentManager` to an enabled `ImageWeighted` row: after
`Fill::fill_surface_extrusion()` generates the ordinary top-solid-infill `ExtrusionPath`s for a
surface, each straight path is re-sampled along its own polyline through the row's image, dithered
against the row's allowed filaments (reusing `gradient_component_ids`, not a new field), and cut
into per-run pieces. Each run becomes its own small `ExtrusionEntityCollection`, tagged with the
one physical filament that run prints with. Everything downstream (`ToolOrdering`, G-code
emission) reads that tag directly instead of re-deriving an extruder from the region's config.

### Wiring points (file:line, as of this commit)

- **`src/libslic3r/Fill/Fill.cpp:309`** - `SurfaceFillParams::image_row_filament_id` (new field,
  int, default 0). See "the four-list change" below.
- **`src/libslic3r/Fill/Fill.cpp:886`** - `image_row_configured_virtual_id()`: the cheap check
  (region's configured virtual id, if it names an enabled `ImageWeighted` row with a non-empty
  `image_fill_ref`; 0 otherwise). Called from `group_fills()` (`Fill.cpp:1221`) to key
  `SurfaceFillParams`, and again from...
- **`src/libslic3r/Fill/Fill.cpp:919`** - `image_row_context_for_region()`: the full context
  (decoded `ImageFillParams`, the row's candidate ids/colours from `filament_colour`, the mesh
  bounding box, the mesh<-print transform, a synthesised mesh-space "up" normal). Built once per
  `SurfaceFill`, not per polyline, in `Layer::make_fills()`.
- **`src/libslic3r/Fill/Fill.cpp:991`** - `image_row_runs_for_path()`: samples one `ExtrusionPath`
  end to end (concatenating per-segment `image_fill_sample_segment()` calls, dropping the
  duplicate shared endpoint between segments) and calls `image_fill_dither_segment()` once for the
  whole path.
- **`src/libslic3r/Fill/Fill.cpp:1034`** - `split_extrusion_path_by_runs()`: cuts the original
  `Polyline` at each run boundary via `Polyline::split_at_length()`, producing one new
  `ExtrusionPath` per run with every other attribute (role, flow, width, height) copied from the
  original.
- **`src/libslic3r/Fill/Fill.cpp:1077`** - `split_top_infill_by_image_row()`: the per-surface
  driver, called from `Layer::make_fills()` right after `f->fill_surface_extrusion()` (same
  function, ~30 lines later). Replaces the collection `fill_surface_extrusion()` just pushed with
  one small collection per run (or leaves a non-`erTopSolidInfill` child, e.g. gap fill, or a
  degenerate/one-run path, wrapped but untouched).
- **`src/libslic3r/ExtrusionEntityCollection.hpp:49`** - the new field,
  `ExtrusionEntityCollection::image_row_extruder_1based` (0 default; a 1-based PHYSICAL filament
  id when set - already resolved, not a virtual id needing `resolve_mixed()`). Propagated through
  the copy/move constructors, `operator=` and `swap()`
  (`ExtrusionEntityCollection.hpp`/`.cpp`) so `clone()` (used pervasively, e.g. by
  `chained_path_from`) preserves it.
- **`src/libslic3r/GCode/ToolOrdering.cpp:354`** - `LayerTools::extruder()` checks the new field
  first, so `WipingExtrusions::is_overriddable()`'s soluble check asks about the run's own
  filament, not the region's nominal one.
- **`src/libslic3r/GCode/ToolOrdering.cpp:873`** - `ToolOrdering::collect_extruders()`'s fills
  loop: a collection carrying the new field registers its own filament into
  `layer_tools.extruders` directly (via `resolve_mixed()`, a no-op for an already-physical id) and
  is excluded from the `has_solid_infill`/`has_sparse_infill` aggregate bucket below it - which
  otherwise would have registered only the row's un-split `resolve()` fallback id, not the ids the
  runs actually use. **This is the fix that matters**: without it, `GCode.cpp`'s
  `layer_tools.has_extruder(correct_extruder_id)` check (`GCode.cpp` around the `printing_extruders`
  build-up) would not find the run's extruder in the layer's tool list and would silently
  reassign it to `layer_tools.extruders.back()`.
- **`src/libslic3r/GCode.cpp:5256` and `:5277`** - `configured_filament_id_1based` and
  `configured_extruder_id` (the two local lambdas in the big per-layer extrusion loop that decide
  which physical filament an `ObjectByExtruder::Island::Region::INFILL` collection actually prints
  with) check the new field first, after the existing per-layer `extruder_override` check (so a
  forced-extruder layer override still wins, exactly as it already did for every other role).
- **`src/libslic3r/MixedFilament.hpp:419` / `.cpp:289`** - `MixedFilamentManager::hex_to_srgb01()`,
  a small new public helper (wraps the existing file-local `parse_hex_color`) so Fill.cpp can turn
  a physical filament's configured `#RRGGBB` into the `std::array<float,3>` sRGB the dither
  expects, without duplicating a hex parser.
- **`src/libslic3r/Format/bbs_3mf.cpp`** - `_add_image_fill_to_archive()` (writer) now also scans
  `mixed_filament_definitions` for `ImageWeighted` rows and keeps their referenced asset alive.
  This was not part of the original plan and was found empirically - see "one bug this session
  found and fixed" below.

### The four-list change

Per the plan's own note (Section 2's table), a new fill-affecting field has to go into all four
places the fork already tracks for `over_support_flow`/`over_support_speed`, or it risks silently
changing fill grouping:

1. `SurfaceFillParams`'s fields (`Fill.cpp:309`)
2. `operator<`'s `RETURN_COMPARE_NON_EQUAL` chain (`Fill.cpp`, next to the other role/id fields)
3. `operator==` (same file)
4. `Layer::is_perimeter_compatible` (`Layer.cpp` ~197-201)

Steps 1-3 are done: `image_row_filament_id` is compared in all three, defaulting to 0 so a print
that never uses the feature never differentiates an existing SurfaceFillParams group on this
field (Bar A's byte-identity is the proof).

**Step 4 (`is_perimeter_compatible`) was deliberately NOT touched**, and here is the reasoning,
which the codebase should keep next to the field itself (it now does, in `Fill.cpp`'s own
comment): `is_perimeter_compatible` governs merging for WALL/PERIMETER generation
(`PerimeterGenerator`), a completely different code path from `Layer::make_fills`'s fill grouping.
`over_support_flow`/`over_support_speed` needed both lists because over-support affects both walls
(via `outer_wall_speed` interactions) and fills. The image row touches ONLY top solid infill fill
generation - it never changes what a wall prints with, so there is no wall-merge hazard for it to
guard against. Adding an unrelated field to `is_perimeter_compatible` would not fix a real bug and
would risk its own subtle behaviour change for a comparator that has nothing to do with fills.

### The order rule (tool changes bounded by filaments, not runs)

The design brief asked for "all of extruder A's runs, then B's, then C's" within a surface so tool
changes are bounded by filament count, not run count. That grouping does not need to happen inside
`Fill.cpp`'s own ordering of `layerm->fills.entities`: **G-code emission already groups by
extruder for the WHOLE LAYER**, not per surface - the outer loop in `GCode.cpp`'s per-layer
extrusion pass iterates `layer_tools.extruders` (in an order already reordered to minimize
switches, `ToolOrdering::reorder_extruders`) and, for each extruder, emits every entity across
every region that resolves to it before moving to the next. Since each run is now its own
`ExtrusionEntityCollection` tagged with one filament, this pre-existing mechanism buckets them
correctly with no change needed to the order they are pushed into `fills.entities` in. The bound
this produces is strictly better than "per surface": the whole LAYER visits each of its needed
filaments once (in `layer_tools.extruders`'s order), so per-layer tool changes are bounded by
`layer_tools.extruders.size()` (which itself is deduplicated and reordered for minimum switches),
not by the number of runs or even the number of surfaces. Measured on Bar B: 3 filaments, tool
changes per layer never exceeded 3 (bound: `2*(filaments-1) = 4`).

### Coordinate spaces and the identity-matrix assumption

Fill polylines live in the `PrintObject`'s own working space (what `trafo_centered()` maps
INTO). `image_fill_project()`'s `box`/`p`/`facet_normal` are in the volume's own MESH space (what
`ModelVolume::mesh()` is expressed in - confirmed by reading `image_fill_apply()`, which passes
`volume.mesh().its` straight through with no extra transform). `image_row_context_for_region()`
computes `mesh_from_print = object.trafo_centered().inverse()` and applies it per sampled point;
the mesh-space "up" normal (needed by a Box projection, and by nothing else) is the object-space
+Z axis run through the same inverse's linear part.

**Stated, unenforced assumption**: the object is a single model-part volume with an identity local
matrix (`ModelVolume::get_matrix()`), i.e. the part was not itself moved/rotated independently of
the object placement `trafo_centered()` already undoes. `image_row_context_for_region()` picks the
first `is_model_part()` volume and never reads its matrix. A multi-volume object, or a volume with
its own non-identity transform, still slices and still prints correctly for every OTHER feature -
it simply is not sub-triangle-resolved (falls back to the row's `resolve()` per-layer cycle, the
same degradation an image-row print gets from any other early-exit in
`image_row_context_for_region()`). Bar A and Bar B both use single, untransformed volumes, so this
assumption is UNVERIFIED beyond the reasoning above; it is the most concrete follow-up for anyone
touching this code next.

A second, related simplification: sample spacing and run-length units are computed by treating
mesh-space arc length as equal to print-space arc length, which is only exactly true for a
uniform-scale object. A non-uniformly scaled object would sample at the wrong density and cut runs
at slightly wrong lengths; it would not crash or misplace colours by more than that. Not covered by
Bar A/B.

### What is NOT wired (explicitly out of scope for step 4)

- **Ironing.** The plan text says "top solid layers (and optionally ironing)". Ironing entities
  are generated by a separate code path in `Layer::make_fills` (`ironing_params.layerm->fills...`)
  with different geometry semantics (thin cover lines); step 4 does not touch it. An ImageWeighted
  row's ironing pass (if ironing is enabled at all) prints with the region's un-split `resolve()`
  fallback, same as every other un-split case.
- **Arachne / non-`ExtrusionPath` top surfaces.** `split_top_infill_by_image_row()` only
  recognises a plain `ExtrusionPath` role-tagged `erTopSolidInfill`. A top surface pattern that
  produces `ExtrusionMultiPath`/`ExtrusionLoop` children (Concentric-family patterns using
  variable width, or `FillLockedZag`'s own multi-width override) is left ungrouped, wrapped in its
  own solo collection with no override - it falls back to the row's `resolve()` cycle. Bar A/B use
  Rectilinear/Monotonic top surfaces, the common case, so this path is untested here.
- **Preview / GUI.** No preview or per-layer filament-view change was made. A slice using this
  feature previews correctly in the sense that the G-code is correct, but the 3D preview's colour
  swatch for a run may still show the region's nominal (un-split) colour. Not measured.
- **Wipe-tower `flush_into_infill` interaction.** Per-run collections are role `erTopSolidInfill`,
  which `WipingExtrusions::is_overriddable()` already excludes from `flush_into_infill` (that
  option only applies to `erInternalInfill`) unless `flush_into_objects` is also on. That
  combination (`flush_into_objects` + an image row on the same object) is unverified.

### One bug this session found and fixed (not in the original step-4 brief)

Building Bar B's project exposed a real gap: `_BBS_3MF_Exporter::_add_image_fill_to_archive()`
(the 3MF writer that decides which `Metadata/image_fill/<sha>.png` assets to keep) only scanned
`ModelVolume::config`'s `image_fill_params` key - the Phase 2 reference. It had no idea a
`MixedFilament` row's `image_fill_ref` (Phase 3's reference, living in the project-wide
`mixed_filament_definitions` string, not on any volume) could ALSO name an asset. A project whose
only image reference was an `ImageWeighted` row therefore silently saved with the row intact but
the picture dropped - the config string round-trips, `image_row_context_for_region()` finds no
image at `assets.pixels(sha)`, and the fill quietly falls back to the un-split `resolve()` cycle.
Fixed by having the writer also decode `mixed_filament_definitions` (via a scratch
`MixedFilamentManager::load_custom_entries()`) and add each `ImageWeighted` row's referenced asset
to the keep-list. This was diagnosed empirically (temporary instrumentation, since removed) when
Bar B initially showed a uniform colour per layer instead of a per-position split - see "anything
unverified" in the session report for how this was found.

## Acceptance

- **Unit tests**: 653 cases, 651 passed, 2 failed as expected (the same 2 pre-existing expected
  failures as the step-3 baseline's 652/2 - one new test case, `[barb3]`, added and passing).
- **Bar A**: `OrcaToleranceTest.stl` on "Bambu Lab P1S 0.4 nozzle" / "0.20mm Standard @BBL X1C" /
  "Generic PLA", isolated `--datadir` copies of `dd_lan`: byte-identical (SHA-256 match, timestamp
  line excepted) between the step-3 baseline and the step-4 candidate; 0 new `; key = value`
  CONFIG_BLOCK lines; `; model label id` and every `M624`/`M625` label line unchanged (129 of
  them). Also run, both PASS with the same byte-for-byte result: `tests/data/support_corpus/
  onepart_ledge.3mf` with `--over-support-surfaces=1` (plus support enabled), and
  `OrcaToleranceTest.stl` with `--offset-layers=1`.
- **Bar B**: a 50 x 50 x 3 mm plaque (`tests/libslic3r/test_image_fill.cpp`'s new `[barb3]` test
  case, built via the model-level API exactly as Phase 2's `[barb]` case was, no CLI dialog
  involved), `ramp_kw.png` (an existing Phase 2 fixture: a 1 x 16 vertical white-to-black ramp)
  applied through a Planar/Z `ImageFillParams`, three "Generic PLA" filaments recoloured
  `#000000`/`#808080`/`#FFFFFF` via the project's own embedded config, `solid_infill_filament`
  forced to the row's virtual id and `solid_infill_direction`/`top_surface_pattern` forced to
  horizontal/Monotonic via CLI overrides (see "the config-layer detour" below) - sliced on P1S /
  `0.20mm Standard @BBL X1C`:
  - The top layer's fill splits into all three tools (T0/T1/T2 all present).
  - Per-Y-band breakdown of the top layer (8 bands across the plaque's ~49 mm extrusion-move Y
    span): T0 (black) dominates bands 0-1, T1 (grey) bands 2-5, T2 (white) bands 6-7 - a clean,
    one-directional black-to-white transition with no reversal, except a single ~1 mm residual in
    T0/T1's last band that is most likely a fixed-length wipe/purge travel-with-extrusion move
    landing at that Y (both T0 and T1 show exactly the same ~1.00 mm figure there), not image
    content - not independently confirmed.
  - Tool changes: max 3 in any one layer, bound is `2*(filaments-1) = 4` - satisfied.
  - Determinism: two runs of the SAME candidate binary on the SAME project produced
    byte-identical G-code (SHA-256 match, timestamp/`M73` lines excepted) - unlike Phase 2's
    facet-painted Bar B, which the phase 2 spec (section 6.3) documents as non-deterministic on
    this box. The image-row path does not share that non-determinism, at least for this case.

### The config-layer detour (why Bar B's project needed CLI overrides, not just embedded config)

The first several Bar B attempts embedded `solid_infill_filament` on the plaque's `cfg` (the
project-wide `DynamicPrintConfig` `StoreParams::config` also carries `mixed_filament_definitions`
on) and separately on the volume's own `ModelConfig`. Neither survived `--process-preset`
selection at CLI slice time: `solid_infill_filament` is part of the process preset's own domain,
so selecting a named process preset overwrites it regardless of what the project embedded, at
both the global and the per-volume-override layer (`region_config_from_model_volume`,
`PrintObject.cpp`, applies the override and then unconditionally clamps
`solid_infill_filament`/`wall_filament`/`sparse_infill_filament` back to the physical filament
count if they exceed it - `num_extruders` there is `filament_diameter.size()`, i.e. PHYSICAL count
only, so a virtual id is subject to the SAME clamp regardless of where it came from). What DOES
survive preset selection, empirically, is a `--solid-infill-filament=<id>` CLI argument passed
AFTER `--filament-presets` (consistent with how every other CLI-argument override in this fork's
scripts is applied, e.g. `--support-top-z-distance` overriding a process preset's own default).
`mixed_filament_definitions` itself and `filament_colour` are NOT part of the process preset's
domain and DO survive as embedded project config, which is why only `solid_infill_filament` (and,
for a clean band pattern, `solid_infill_direction`/`top_surface_pattern`) needed the CLI-argument
route. `snorca_hubtest/slice_barb3.py` has the final recipe.

## What step 5 still needs

- **The GUI checkbox**: "Nozzle-resolution dither (top surfaces)" - no GUI work was done this
  session; `ImageWeighted` is reachable today only by hand-constructing a `MixedFilament` row
  (as the `[barb3]` test does) or by editing a project's `mixed_filament_definitions` string
  directly. The dialog needs a way to set `distribution_mode = ImageWeighted` and populate
  `image_fill_ref` from the existing Image Fill flow's own asset/projection picker.
- **Doc completion**: this file. Also worth a follow-up note in the plan's own Phase 3 acceptance
  section once step 5 lands, since the plan's "GUI check - preview shows the dither" line is still
  open (see "What is NOT wired" above).
- **The identity-matrix assumption**: worth either enforcing (skip the split with a clear reason
  when the picked volume's matrix is non-identity, or when there is more than one model-part
  volume) or properly generalising (look up the actual owning `ModelVolume` for a given
  `LayerRegion`/`PrintRegion`, the way `PrintObjectRegions` does for painted regions) before this
  ships to real multi-volume parts.
- **Ironing and Arachne-pattern top surfaces**: both currently fall back silently to the
  un-split `resolve()` cycle; worth deciding whether that is acceptable shipped behaviour or
  whether ironing specifically should be wired the same way top solid infill was.
- **Preview**: the 3D view does not yet reflect per-run colours for an image-row surface.
