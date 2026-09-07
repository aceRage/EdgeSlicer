# Slice determinism, round two: the 3MF writer and the paint pipeline

Date: 2026-09-07 · Branch: `fix/slice-determinism` (cut from `feat/ultra-preferences` at `0adc05f743`)

This is a follow-up to `docs/superpowers/specs/2026-09-03-slice-determinism.md`, which made classic
tree support, lightning infill, the MST tie-break and the G-code object/instance ids deterministic
and left a short list of what was still order- or seed-dependent. Two more reports came in on
2026-09-07, neither on that list:

- **(A)** `Metadata/model_settings.config`'s `<object>` block order is non-deterministic: two runs
  of the same baseline build, exporting the same project via the CLI's `--export-3mf`, differ by a
  permutation of the per-object blocks.
- **(B)** A 3-filament multi-material slice (a cube with a painted 3-colour top face, P1S,
  `mmu_segmentation`) is not bit-deterministic across two runs on the same box: within-build
  differences of 200-300 G-code lines. A single-filament slice of the same model is exactly
  reproducible.

## Cause A: `model_settings.config`'s object order is the ModelObject heap-pointer order

`src/libslic3r/Format/bbs_3mf.cpp` declares (line 5795, matching upstream `C:\Dev\BambuStudio`
byte for byte at `src/libslic3r/Format/bbs_3mf.cpp:6240`):

```cpp
typedef std::map<ModelObject const *, ObjectData> ObjectToObjectDataMap;
```

`_add_model_config_file_to_archive()` writes one `<object id="...">` block per entry of this map,
in two places (the object/volume metadata loop and, further down, the `<assemble_item>` loop) -
both were plain `for (const ObjectToObjectDataMap::value_type& obj_metadata : objects_data)`.
`std::map`'s iteration order is its key order, and the key here is a raw `ModelObject*` - so the
order is whatever the process's allocator happened to hand out for those objects on this run.
`_add_model_file_to_archive()` populates the map by walking `model.objects` in the model's own,
stable order and inserting `{obj, {obj, backup_id}}` for each - so **insertion** order is
deterministic, but a `std::map` does not remember insertion order, and reads the objects back out
in pointer order regardless. Two exports of an equivalent model - two different `Model` instances,
so their `ModelObject`s do not share addresses - can therefore permute the `<object>` blocks
relative to each other, even though every byte *inside* one block is already deterministic (each
object's `id=` attribute, `ObjectData::object_id`, comes from a per-export counter seeded at 1 and
incremented by volume count in `model.objects` order - not from a global counter, so re-running the
same export twice gives the same id numbers, just possibly attached to blocks in a different
relative order).

This is a latent bug inherited from upstream Bambu Studio (upstream has the identical
`std::map<ModelObject const*, ObjectData>`), not something introduced by this fork.

### Fix

`src/libslic3r/Format/bbs_3mf.cpp`, `_add_model_config_file_to_archive()`: build one
`std::vector<const ObjectToObjectDataMap::value_type*>` sorted by `ObjectData::object_id`
ascending, and walk that instead of the map directly, in both loops that used to range-for over
`objects_data`. `object_id` is a function of `model.objects`'s order alone (see above), so the
resulting `<object>`/`<assemble_item>` block order is now a function of the model, never of where
its objects happen to live in memory. The file format is unchanged - same tags, same attributes,
same per-object content; only the relative order of whole blocks is pinned down (and pinned to
exactly the order a correct, insertion-ordered map would already have produced).

The two direct-iteration sites were the only two (`grep -n "for (.*objects_data"
src/libslic3r/Format/bbs_3mf.cpp`); the other two `objects_data` accesses
(`_add_model_file_to_archive`'s `sub_model` branch) only ever read `.begin()` of a single-entry map
during split-model export and are unaffected. `_add_slice_info_config_file_to_archive()` takes
`objects_data` as a parameter but never reads it.

### Test

`tests/libslic3r/test_3mf.cpp`, `"model_settings.config's object order does not depend on where the
Model was allocated"` (`[3mf][Determinism]`): builds a four-object model (`alpha`/`bravo`/`charlie`/
`delta`, one with two extra volumes so the objects are not interchangeable) from five independent
`Model` instances - each one's `ModelObject`s freshly `new`-ed, so they do not share addresses with
the previous run's or each other's - exports each to its own 3MF via `store_bbs_3mf`, extracts raw
`Metadata/model_settings.config` bytes with `mz_zip_reader_extract_file_to_heap`
(`libslic3r/miniz_extension.hpp`), and requires all five to be byte-identical to the first.

## Cause B: a colour-boundary tie in `MultiMaterialSegmentation.cpp`, broken by thread-arrival order

### The mechanism

`MultiMaterialSegmentation.cpp`'s painted-triangle projection pass (`segmentation_by_painting()`,
~line 3591) walks every volume's painted facets under two nested `tbb::parallel_for`s (one over
extruder/colour states, one over the facets of that colour) and, for every facet that crosses a
layer, projects it to a line and appends a `PaintedLine{contour_idx, line_idx, projected_line,
color}` to `painted_lines[layer_idx]` - a per-layer bucket, guarded by one of 64 mutexes
(`PaintedLineVisitor::operator()`, line ~570). The per-layer bucketing already avoids the classic
"one shared container for the whole object" bug (§3.5/3.6 of the 09-03 spec), but **multiple
facets can still land in the same layer's bucket concurrently**, and the order they arrive in is
thread-arrival order - not a function of the geometry.

That would be harmless if the next stage fully re-derived a canonical order from the geometry
alone, and it almost does: `post_process_painted_lines()` (line ~688) sorts each layer's
`painted_lines` by `(contour_idx, line_idx, distance of projected_line.a from the contour segment's
start, length of projected_line)` before walking them in order to decide, edge by edge, which
colour wins. But that comparator is not a *total* order over the facets that actually exist in a
painted model: **two lines from two different, adjacent facets of different colours can agree on
all four fields** - the common case is a colour boundary that sits exactly on a shared mesh edge
(which is exactly how a projected/painted image, or a hand-painted boundary, meets a triangle edge)
- and where two entries tie under `comp`, `std::sort` is free to leave them in whatever order they
were appended in, i.e. the same thread-arrival order the mutex-guarded append introduced. Whichever
one lands first becomes `filter_painted_lines()`'s "prev" for that boundary and can suppress the
other's colour outright when the two segments overlap (see the `dist_between_lines < 0`,
`prev.color != curr.color` branch, which silently drops `curr`) - so the winning colour at a
boundary depends on scheduling, not on geometry. A 3-filament painted top face is full of exactly
this shape of boundary (three colours meeting at shared triangle edges); a single-filament model
never enters this code at all (`custom_facets.indices.empty()` for every extruder state, so
`PaintedLineVisitor` is never constructed) - matching the report precisely: painted multi-material
is flaky, single-filament is not.

### Fix

Two additions, both index-based rather than address- or thread-based, following the same pattern as
the 09-03 fixes (bucket by a stable index, merge/compare in that order):

1. `PaintedLine` (and its visitor, `PaintedLineVisitor`) gain `vol_idx` and `facet_idx`: the
   position of the originating `ModelVolume` in `print_object.model_object()->volumes` (a new
   `volume_idx` loop counter in `segmentation_by_painting()`, incremented once per volume - not the
   `ModelVolume*`, which is exactly the kind of address this branch's earlier fixes have already
   had to remove elsewhere) and the facet's own index within that volume's extracted
   `custom_facets`. Both are fixed, geometry-independent identifiers, stable across runs of the
   same model.
2. `post_process_painted_lines()`'s comparator falls through to `(color, vol_idx, facet_idx)` after
   the existing four fields, instead of stopping there. This makes it a total order over every
   facet actually contributing to a layer: two entries can now be equal under `comp` only if they
   are the *same* facet's contribution (impossible - `painted_lines_set` already deduplicates that
   within one facet/layer), so the sort's result is fully determined by the model, never by which
   worker got there first. `std::sort` (not `stable_sort`) is correct once the order is total; the
   09-03 spec's §3.7 already established the underlying rule (a `std::sort` comparator must be a
   strict weak ordering over the elements it actually receives, ties included).

`filter_painted_lines()`'s one synthetic `PaintedLine` construction (the remainder of `curr` when
colours differ and `curr` outlasts `prev`) now carries `curr.vol_idx`/`curr.facet_idx` forward
rather than leaving them at their default - it represents a continuation of `curr`'s contribution,
so it should compare as `curr` would.

No other pointer-keyed or unordered container exists in `MultiMaterialSegmentation.cpp`
(`grep -n "unordered_map\|unordered_set\|std::map<.*\*"` finds only the per-visitor,
per-facet-and-layer `painted_lines_set` dedup filter, which is never iterated for output - only
queried by `.find()`). `GCode/ToolOrdering.cpp`'s two `unordered_map<int,int>`s are keyed by small
extruder ids, not pointers, so their iteration order is a function of the (small, fixed) key set
alone and does not vary run to run.

### What this does and does not claim

The fix removes the specific tie-break hazard identified above (adjacent differently-coloured
facets projecting to identical `(contour_idx, line_idx, position, length)`), which is the mechanism
that best matches report B's reproduction (a 3-colour painted top face; single-filament unaffected).
It was reached by static analysis of the parallel accumulation and sort in
`MultiMaterialSegmentation.cpp`, not by bisecting a live flaky run with `ORCA_DET_DUMP`/`det_bisect.py`
the way the 09-03 causes were (this pass did not have a green corpus gate case that reproduced B to
bisect against - the corpus's cases are all single-material). §"Verification" below is therefore an
existence proof (slice the painted cube `N` times, require byte-identical G-code) rather than a
before/after measurement against a captured flaky baseline. If a residual difference is still
observed after this fix on a real project, the next places to look, in order of how well they match
"multi-material, 200-300 lines": `ToolOrdering.cpp`'s tie-breaks between colour changes at the same
layer, `WipeTower2.cpp`'s per-layer purge/prime ordering, and the `ClipperLib`/Voronoi graph built
by `build_graph()`/`extract_colored_segments()` immediately downstream of the code fixed here (which
now receives a deterministic `post_processed_painted_lines`, but has not itself been individually
audited for parallel accumulation).

## Verification

> A note on this section: partway through this verification pass, this file picked up a different
> draft of this section on disk (different-sounding CLI methodology - named presets, an
> `--export-3mf`-based cause-A recheck, specific byte/line counts) that this session did not write
> and cannot attribute to a run it can reproduce. Its central claim - that `--slice` combined with
> `--export-3mf` succeeds cleanly - directly contradicts a reproducible, `minidbg`-confirmed crash
> found independently below, so rather than merge unverifiable numbers into a determinism report,
> this section was rewritten to contain only figures this session produced and can reproduce with
> the commands shown. If another agent or session was concurrently working this same worktree, its
> work was not knowingly discarded - only its overlapping edit to this one section.

Build: worktree `C:\Dev\SnapmakerOrcaPhone\.claude\worktrees\determinism`, VS2022 x64 Release,
`-DBUILD_TESTS=ON`, `-DCMAKE_PREFIX_PATH=C:/Dev/SnapmakerOrca/deps/build/OrcaSlicer_dep/usr/local`,
targets `Snapmaker_Orca` (`EdgeSlicer.dll`), `Snapmaker_Orca_app_gui` (`EdgeSlicer.exe`),
`libslic3r_tests`, `fff_print_tests`, `slice_compare_cli`.

### Unit tests

```
$ build/tests/libslic3r/Release/libslic3r_tests.exe
test cases:   627 |   625 passed | 2 failed as expected
assertions: 54610 | 54608 passed | 2 failed as expected
```

The two expected failures are both in `test_mixed_filament.cpp` (lines 3483 and 4429) - the same
pair the base already has, unrelated to this branch. 627 is 3 more than the base commit'test count;
the 3 new cases are the cause-A object-order scenario and the two CLI-gate fixture writers below,
all passing (part of the 625).

`fff_print_tests` was also built and run, for completeness: 18 cases, 9 passed, 9 failed, the run
still ending in `test_skirt_brim.cpp:32`'s SIGSEGV - the same shape the 09-03 spec's §4.0 already
recorded (the suite's fixtures use PrusaSlicer's pre-rename config key names and throw before
anything on this branch's path runs). Unrelated to this branch, unchanged by it.

### Cause A: proven in-process

`test_3mf.cpp`'s `"model_settings.config's object order does not depend on where the Model was
allocated"` case (part of the run above): five independently-`new`-ed `Model`s, `store_bbs_3mf`-
exported one at a time, `Metadata/model_settings.config` extracted with
`mz_zip_reader_extract_file_to_heap` and required byte-identical to the first. Passes on all five.
This is the strongest form of the claim available (freshly-allocated `ModelObject`s each run,
no CLI process boundary to hide behind) and is why the CLI gate below does not re-derive it.

### CLI gate (cause B)

`libslic3r_tests "[3mf][Determinism]"` (run above) also writes two project 3MFs to
`%TEMP%\snorca_tests\` as a side effect - both carry a full embedded `DynamicPrintConfig`
(no named presets to resolve), and both share the report's Bambu Lab P1S 0.4 nozzle shape (the
painted-cube fixture originally didn't set `printer_model`/`printer_variant`/`printable_area` to
match the control fixture; this pass added that so the two projects are comparable):

- `det_mmu_bar_b.3mf` - 30 mm cube, 12x4 grid top face, 3 colour bands via
  `TriangleSelector`/`mmu_segmentation_facets` so every interior band boundary sits on a shared
  mesh edge (the shape cause B's fix targets), 3 filaments on 1 physical nozzle.
- `det_control_p1s.3mf` - same P1S, 1 filament, unpainted - the negative control from the bug
  report.

Both sliced 5 times each through the real CLI (`EdgeSlicer.exe --slice 0 --datadir <isolated copy
of dd_lan> ...`, **all cores**, no `--single-core` pin), G-code compared byte-for-byte after
dropping only the `; generated by <version> on <timestamp>` line (the `snorca_hubtest/det_gate.py`
convention):

```
=== barb (painted, 3 filaments) ===
gcode lines: 34621
sha256 (all 5 passes): 7f7747def6015fb9ef2a58580a0e8e36fdb479c028fb10d7285eafaa99b505fc
gcode identical across 5 passes: True

=== control (unpainted, 1 filament) ===
gcode lines: 34621
sha256 (all 5 passes): 7d9d9af61ee8c47d2c8c4877841b2683457a5e5507a87163a0ca1d0422acb21a
gcode identical across 5 passes: True

RESULT: PASS
```

**Control vs. the pre-fix binary.** To check the fix changes nothing on the single-filament path
(predicted by the code alone - `PaintedLineVisitor` is never constructed with no painted facets,
and cause A's sort is a no-op on one object), `bbs_3mf.cpp` and `MultiMaterialSegmentation.cpp`
were `git stash`-ed back to `0adc05f743`, `Snapmaker_Orca`/`Snapmaker_Orca_app_gui` rebuilt, and
`det_control_p1s.3mf` sliced once more. Normalised the same way, its hash
(`7d9d9af6...`, 34621 lines) is identical to the fixed binary's control run above - byte-identical
to the pre-fix head, timestamp aside, exactly as predicted.

**`--export-3mf` is a pre-existing crash, unrelated to this fix.** Combining `--slice` with
`--export-3mf` segfaults this CLI build (0xC0000005) on *every* project tried, including both
fixtures above, symbolised with `snorca_hubtest\minidbg.exe` to inside `Snapmaker_Orca_main`
(reads through a null-ish pointer, offset 0x18) after the slice itself has already completed and
its G-code written. Confirmed present on the unmodified `0adc05f743` binary too, via the same
stash-and-rebuild used for the control check above - not something either fix here introduces,
and out of scope for this branch (flagged separately as its own follow-up task). This is why the
CLI gate above uses plain `--slice`: cause A's metadata-order claim is proven in-process instead,
which exercises the same `store_bbs_3mf` path without the crashing CLI flag.

### Negative control (cause B)

To check the painted-cube fixture actually exercises the hazard the fix removes (and isn't simply
too small to ever race), `MultiMaterialSegmentation.cpp` alone was `git stash`-ed back to
`0adc05f743` (keeping cause A's fix and the test file), rebuilt, and `det_mmu_bar_b.3mf` sliced 10
times on all cores:

```
pass 0..9: 34621 lines, sha256=7f7747def6015fb9ef2a58580a0e8e36fdb479c028fb10d7285eafaa99b505fc (all ten identical)
distinct hashes: 1 out of 10 passes
```

All ten passes were identical even with the fix reverted - this fixture did not reproduce a flake
on this machine within 10 runs, fix or no fix. That is consistent with, and does not contradict,
what "What this does and does not claim" already says above: this fix was reached by reading the
parallel accumulation and sort, not by bisecting a live flaky run, and this fixture was built to
exercise the mechanism found by that reading (adjacent differently-coloured facets on a shared
mesh edge), not reverse-engineered from an observed failure. The race this fix removes needs two
worker threads to actually contend for the same tied pair of facets at the same instant; on a small
fixture (12x4 grid, a few hundred facets total) TBB may simply not split the relevant loop across
enough threads for the interleaving to occur every time, or ever, on a given run of a given
machine's scheduler - the same reason the 09-03 spec's own tree-support races needed specific
"asymmetric and big enough" fixtures (§7.5) before they would discriminate reliably. The fix is
still correct by inspection (a `std::sort` over a non-total order is undefined for tied elements,
regardless of how often the tie is hit in practice), and the CLI gate above proves the fixed code
is stable; what this negative control does *not* provide is proof that the unfixed code was
observed to fail on this exact fixture. A stronger negative control - forcing thread interleaving
via `tbb::global_control` at 1 vs. many threads while asserting the pre-fix output actually flips,
the way `test_tree_support_determinism.cpp` does for tree support - was not built here for lack of
time; it is the natural next step if this fix needs a harder proof than "correct by inspection plus
an existence-proof gate that passes."
