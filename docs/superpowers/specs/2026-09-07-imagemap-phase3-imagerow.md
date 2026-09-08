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

## Step 5 (this session): the GUI checkbox, the transform fix, and closing the doc

### User flow

1. Select a part (or a volume within it), open **Right-click > Apply image fill...** (the Phase 2
   dialog, `ImageFillDialog`).
2. Pick an image (or a gradient), a projection, the filaments it may use, and Detail (mm) exactly
   as Phase 2 already worked - none of that changed.
3. Tick **"Nozzle-resolution dither (top surfaces)"**. Its tooltip says exactly what it does and
   does not cover (see "Limits" below).
4. Press **Apply**. `Plater::apply_image_fill()` (Plater.cpp):
   - always runs Phase 2's facet fill (`image_fill_apply()`) first, tick or no tick - a slicer that
     never reads the image row still gets a correct, if coarser, result;
   - if the box is ticked, creates (first time) or updates (re-apply) one `MixedFilament` row on
     the project: `distribution_mode = ImageWeighted`, `gradient_component_ids` = the dialog's
     allowed-filament list, `image_fill_ref` = the applied `ImageFillParams`, `enabled = true`;
     then sets the PART's own `solid_infill_filament` (a per-volume config override, the same key
     `image_row_configured_virtual_id()` reads at slice time) to that row's virtual id;
   - if the box is unticked and the part was previously bound to an ImageWeighted row, erases the
     part's `solid_infill_filament` override - the row itself is left in
     `mixed_filament_definitions` untouched (it may be used by another part, or the user may want
     to keep it for later; step 5 does not attempt "is anyone still using this row" cleanup);
   - persists the row change to `mixed_filament_definitions` (both the live print config and
     `PresetBundle::project_config`) and calls `Sidebar::update_mixed_filament_panel(false)`, the
     same call other programmatic mixed-row edits in this file use - the new/updated row appears
     in the sidebar's Mixed Filaments panel exactly like a row added via "Add Gradient"/"Add
     Pattern"/"Add Color", with its own colour chip and summary line.
5. Re-opening the dialog on a part already bound to an enabled ImageWeighted row starts with the
   checkbox already ticked (`ImageFillDialog`'s new `initial_image_row` constructor argument,
   worked out by `Plater::apply_image_fill()` from the part's current `solid_infill_filament`
   before the dialog is constructed) - the same promise `initial` already made for the projection
   controls.

New public API used by the above, added this session: `MixedFilamentManager::
filament_id_from_mixed_index()` (MixedFilament.hpp/.cpp) - the inverse of the pre-existing
`mixed_index_from_filament_id()`, needed because the GUI knows a just-created row's position in
the manager's own vector but not, without walking every row before it, which virtual id that
resolves to. Unit-tested in test_mixed_filament.cpp.

### The instance/volume transform fix (item 2)

Reading `image_row_context_for_region()`'s own step-4 code turned up that the "identity-matrix
assumption" it documented was two different claims bundled together, only one of which was
actually a gap:

- **The shared instance rotation/scale was ALREADY correct before this session.** Every
  `PrintObject` is built from instances that share one rotation/scale (`PrintObject::trafo()`,
  set from `PrintInstances::trafo` at construction - `PrintApply.cpp`), so
  `object.trafo_centered()` already reflected it. There was nothing to fix here; step 5 adds
  `tests/libslic3r/test_image_row_transform.cpp`'s first case (a plaque on a 90-degree-rotated
  instance) to actually pin the claim instead of leaving it asserted-but-untested.
- **The volume's own local matrix (`ModelVolume::get_matrix()`) really was ignored**, and so was
  which volume a multi-volume object's region should even read (the code always took the
  object's FIRST `is_model_part()` volume, regardless of which `PrintRegion` was asking). Fixed
  by two changes in `src/libslic3r/Fill/Fill.cpp`:
  - `image_row_owning_volume()` (new): resolves the ACTUAL `ModelVolume` backing a given
    `PrintRegion`, via `PrintObjectRegions::layer_ranges[].volume_regions` - the same volume<->
    region map `PrintObjectRegions` already builds for painted-region resolution. Falls back to
    the old "first model-part volume" behaviour when the map does not (yet) know the region, so
    the single-volume case that step 4's Bar A/B verified is unaffected.
  - `image_row_context_for_region()`'s `mesh_from_print` is now `(object.trafo_centered() *
    mv->get_matrix()).inverse()`, the same composition `PrintObject.cpp` already uses for
    facet-modifier slicing (its `slice_mesh_slabs()` calls), instead of `trafo_centered()` alone.
  - `image_row_configured_virtual_id()` and `image_row_context_for_region()` both now take the
    `PrintRegion&` itself (not just its `PrintRegionConfig`), so the owning-volume lookup has a
    `PrintRegion*` to match against `volume_regions[].region`.
  - `tests/libslic3r/test_image_row_transform.cpp`'s second case is the regression guard: two
    model-part volumes in one object, each with its own local offset (70 mm apart - more than
    either plaque's own 50 mm span, so a wrong transform or wrong-volume resolution cannot
    coincidentally land inside the right box, but small enough that the combined footprint still
    fits the bare `full_print_config()` bed) and its own `ImageWeighted` row, plus different
    `wall_loops` so the two stay separate `PrintRegion`s (see the unit-tests bullet under
    "Proofs" for why that last part is needed - without it `Layer::is_perimeter_compatible()`
    merges them). If either volume resolution or the matrix composition regressed, the second
    plaque's sample points would land nowhere near its own mesh bounding box and its top surface
    would silently fall back to the un-split `resolve()` cycle (zero
    `image_row_extruder_1based`-tagged entities, or a single degenerate colour) - the test
    asserts every top-solid entity that exists for BOTH regions is fully split (none partially)
    and genuinely multi-coloured.

**Still not fixed** (documented, not attempted this session - see Fill.cpp's own updated header
comment): sample spacing and run-length cuts still treat mesh-space arc length as equal to
print-space arc length, exact only for a uniform-scale transform. A non-uniformly scaled volume
or instance would sample at a slightly wrong density and cut runs at slightly wrong lengths; it
would not crash or land colours grossly wrong. Neither Bar A/B nor the new transform tests use a
non-uniform scale.

### Ironing and Arachne-pattern top surfaces (item 3): documented, not wired

Chose **document, don't wire** for both, for the reason the phase-3 doc already gave for leaving
step 4 as it was: ironing's own fill loop (`Layer::make_ironing()`) has different geometry
semantics from `Layer::make_fills()`'s (thin cover lines, one extruder per ironed region, not a
polyline to re-sample per-run), and a Concentric-family (or other Arachne-driven) top surface
pattern never produces a plain `ExtrusionPath` for `split_top_infill_by_image_row()` to cut in
the first place - wiring either properly would be a materially different, separately-scoped
change, not a step-5-sized addition to an already-large session.

What ships instead:
- The dialog's checkbox tooltip says, verbatim, "Top solid infill only: ironing and any top
  surface pattern that is not Rectilinear or Monotonic (Concentric-family patterns, in
  particular) are not affected by this option and keep using the coarser per-layer colour cycle
  a mixed filament normally uses."
- Two new one-time log lines (`BOOST_LOG_TRIVIAL(warning)`, each gated by its own
  `std::atomic<bool>` so it fires once per process, not once per layer) in Fill.cpp:
  `log_image_row_ironing_not_split_once()` (fires when an ImageWeighted row's ironing pass would
  run) and `log_image_row_pattern_not_split_once()` (fires the first time
  `split_top_infill_by_image_row()` meets a top-solid-infill child that is not a plain
  `ExtrusionPath` - i.e. an Arachne/Concentric-family pattern).

### Preview (item 4): already correct, verified rather than changed

The 3D preview's "Filament"/tool colour view is driven entirely by `GCodeProcessor::process_T()`
reading live `T<n>` commands out of the G-code (`m_extruder_id` / `m_cp_color`,
GCodeProcessor.cpp) - it has no idea an `ExtrusionEntityCollection` exists, let alone that one
carries `image_row_extruder_1based`. Step 4 already made `ToolOrdering`/`GCode.cpp` emit the
RIGHT `T<n>` for each run (that is the whole point of the feature), so this view was already
correct before step 5 touched anything; nothing needed to change. Verified on Bar B: T0/T1/T2
commands appear correctly interleaved across the top layer's runs (see "Proofs" below for the
exact counts) - a slice opened in the Preview tab's "Filament" colour mode would show the
plaque's top layer coloured in a clean black-to-grey-to-white band, matching the ramp, not a
single flat colour. The one thing this does NOT cover, unchanged from step 4's own note: a view
that colours by the REGION's nominal (un-split) filament rather than by G-code tool - if this
fork has one - would still show the region's single nominal colour for an image-row surface,
because that kind of view reads a pre-slice attribute the split does not touch. No such view was
found or changed this session.

### Limits (for the doc and for support questions)

- Top solid infill only - not ironing, not a Concentric-family top surface pattern (see above).
- A single model-part volume with an identity local matrix samples exactly as before (Bar A/B's
  own case, still byte-identical/verified). A volume with its own transform, or a second
  model-part volume, is now sampled correctly (this session's fix) but is not yet covered by any
  hardware print (see below).
- Sample spacing assumes uniform scale (see "still not fixed" above).
- Unticking the checkbox never deletes the `MixedFilament` row it was bound to, only the part's
  own binding to it - a project can accumulate unused `ImageWeighted` rows the way it can
  accumulate any other unused mixed-filament row today.
- The GUI path (this dialog) is not exercised by an automated test - see "Proofs".

### Proofs

- **Unit tests**: `libslic3r_tests`, candidate build - **656 cases, 654 passed, 2 failed as
  expected** (the SAME two pre-existing expected failures as the step-3/step-4 baseline, zero
  unexpected failures; 78126 assertions, 78124 passed). Step 5 adds three new cases:
  `tests/libslic3r/test_image_row_transform.cpp`'s rotated-instance case and multi-volume case,
  and `test_mixed_filament.cpp`'s `filament_id_from_mixed_index` round-trip test. The
  multi-volume case surfaced, and had to work around (not fix - out of scope), a pre-existing
  engine characteristic unrelated to this fix: `Layer::is_perimeter_compatible()` deliberately
  does not compare `solid_infill_filament` (see that function's own comment, Layer.cpp), so two
  volumes differing ONLY in it get their perimeters (and, per `Layer::make_perimeters()`'s merge
  path, most of their fill area) merged into one - the test gives the two volumes different
  `wall_loops` too, to keep them independent regions, and its own comment records what was
  learned diagnosing this (empirically, via a temporary counting helper, since removed down to
  the two permanent invariant checks it left behind: every top-solid entity that exists is fully
  split, and never partially).
- **Bar A**: `OrcaToleranceTest.stl` on "Bambu Lab P1S 0.4 nozzle" / "0.20mm Standard @BBL X1C" /
  "Generic PLA", isolated `--datadir` copies of `dd_lan`, baseline = the step-4 tip (079551947d,
  `inst_imgrow_p3s4_cand`), candidate = this session's build (`inst_imgrow_p3s5_cand`) -
  **byte-identical G-code apart from the timestamp line** (SHA-256
  `4e3575799be794de08e32439404a06c3badbd878e7ef10f4daceec2f25739bdb` both sides, 24562 lines both
  sides), 0 new `; key = value` CONFIG_BLOCK lines, 129 label/M624/M625 lines unchanged. Also
  re-run against the same baseline (`bar_a_p3s4_features.py`'s own recipe, unmodified): the
  over-support-surfaces corpus case (`onepart_ledge.3mf`, over-support-surfaces on) and the
  offset-layers case (`OrcaToleranceTest.stl`, offset-layers on) - both still byte-identical
  (SHA-256 matched on both sides for each case).
- **Bar B**: the `[barb3]` plaque (unchanged since step 4 - step 5 does not touch its own test),
  sliced with the step-5 candidate on the same printer/process/filaments as step 4's own Bar B -
  **three tools present in the top layer** (T0/T1/T2), the same black-to-grey-to-white band
  progression step 4 measured (T0 dominant bands 0-1, T1 bands 2-5, T2 bands 6-7, same ~1 mm
  residual step 4's own spec attributed to a fixed-length wipe/purge move, not image content),
  max 3 tool changes in any one layer (bound `2*(3-1)=4`, satisfied). Determinism: two runs of
  the SAME candidate binary on the SAME project produced byte-identical G-code (SHA-256
  `a1f10cc5b31b4909c24654e3aa0d5670e44019409f75e324916e45161e78e019` both runs). Driven entirely
  through the model-level API (`image_fill_apply()` + `MixedFilamentManager`), exactly the way
  the dialog's own Apply handler drives it - **nobody clicked the dialog**; the GUI checkbox
  path itself has no automated coverage this session. This same slice is also item 4's
  (preview) evidence: T0/T1/T2 all appear correctly interleaved within the top layer, exactly
  matching the per-band runs `split_top_infill_by_image_row()` produced - proof that
  `GCodeProcessor`'s tool-based colouring (see item 4's own write-up) will show the right colour
  per run.
- A hidden scratch `EdgeSlicer.exe` instance (the step-5 candidate, `inst_imgrow_p3s5_cand`) was
  started on a fresh copy of `dd_lan` and left running (confirmed alive after startup), as a
  smoke check that the candidate GUI binary - built with the new checkbox code - starts cleanly.
  Not a functional test of the dialog: nobody clicked it.

### Hardware test for the owner

A 50 x 50 mm plaque, a black-to-white ramp image, three filaments loaded (e.g. black / mid-grey /
white PLA), sliced twice:

1. **Dither ON**: apply the image fill as usual, tick "Nozzle-resolution dither (top surfaces)",
   pick all three filaments, Apply, slice, print. Expect the top surface to look like a smooth
   gradient at the printer's own line-width resolution - individual line-to-line colour steps
   should be hard to pick out by eye, unlike a facet-limited or per-layer-cycle transition.
2. **Dither OFF** (same image, same three filaments, untick the box - or apply Phase 2's plain
   facet fill only): expect a visibly coarser transition - either the facet size Detail (mm) was
   set to, or (if no image fill was applied to the region at all and it just cycles through the
   row's own filament sequence) a blockier, per-layer-group banding.
   Comparing the two prints side by side is the actual acceptance bar for this feature; nothing
   in CI or the test suite can substitute for it.
