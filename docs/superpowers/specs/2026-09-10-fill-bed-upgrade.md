# Fill bed with copies: ask for a minimum gap, and pack tighter

Research spec. Branch would be `feat/fill-bed-gap`, off `feat/ultra-preferences`.
All file:line refs are from this tree (`C:\Dev\SnapmakerOrcaPhone`, `feat/ultra-preferences`).

Owner complaint: *"Fill bed with copies" leaves a lot of gaps, and the gap between copies is
uncontrollable.* Both halves are true, with different causes.

## 1. Current behaviour, traced

Two entry points, one job: object right-click "Fill bed with copies"
(`src/slic3r/GUI/GUI_Factories.cpp:931`) and the Clone dialog's **Fill** button
(`src/slic3r/GUI/CloneDialog.cpp:49-54`, `dlg_btns->GetNEXT()`, which ignores the dialog's copy
count). Both call `Plater::fill_bed_with_instances()` (`src/slic3r/GUI/Plater.cpp:21056`), which
replaces the UI job with a `FillBedJob`. No dialog, no parameters, nothing to configure.

### The gap value

`FillBedJob::prepare()` (`FillBedJob.cpp:26`) calls `init_arrange_params(m_plater)`
(`src/slic3r/GUI/Jobs/ArrangeJob.cpp:762`), whose line 782 is
`params.min_obj_distance = scaled(settings.distance);`. `settings` is
`GLCanvas3D::ArrangeSettings` (`GLCanvas3D.hpp:481-492`, `float distance = 0.f`), picked per
print technology / sequence by `get_arrange_settings()` (`GLCanvas3D.cpp:1118`) and
persisted in AppConfig section `arrange`, keys `min_object_distance_fff` / `_seq_print_fff` /
`_sla` (loaded `GLCanvas3D.cpp:1061-1096`, written `GLCanvas3D.cpp:5966`).

**So the value governing the gap today is the "Spacing" slider in the Arrange-options popup
(`GLCanvas3D.cpp:5955-5969`, 0-100 mm, default 0 = auto).** It is the only lever a user has, it
is not reachable from the Fill command at all (it lives behind the arrange toolbar button), and
its default of `0` means "auto", i.e. per-object brim width.

`min_obj_distance` becomes a per-item `inflation` in `update_selected_items_inflation()`
(`src/libslic3r/Arrange.cpp:122-123`):

```cpp
ap.inflation = params.min_obj_distance != 0 ? params.min_obj_distance / 2 :
    plate_has_tree_support ? scaled(brim_max / 2) : scaled(ap.brim_width);
```

Half-gap each, so neighbours end up `min_obj_distance` apart. `ap.brim_width`
(`src/libslic3r/ModelArrange.cpp:162-176`) is 1 mm base / 6 mm normal support / 24 mm tree support
— that is the "auto" gap; `Arrange.cpp:124-130` then clamps inflation so item+inflation still
fits. `_arrange()` sets `mod_params.min_obj_distance = 0` (`Arrange.cpp:978`) since items are
already inflated, else the `_Nester` inflates twice (`deps_src/.../nester.hpp:867,875`).

### The count estimate (`FillBedJob.cpp:146-163`)

```cpp
auto polys = offset_ex(m_selected.front().poly, params.min_obj_distance / 2);
double poly_area = poly.area() / sc;                       // inflated area
double fixed_area = unsel_area + m_selected.size() * poly_area;
double bed_area   = Polygon{m_bedpts}.area() / sc;         // NOT the shrunk bed
int needed_items = (bed_area - fixed_area) / poly_area;
```

`m_bedpts` here is the raw bed (`FillBedJob.cpp:127`). `needed_items` clones of the template are
appended with `bed_idx = MAX_PLATES_COUNT` and a `setter` that creates a new `ModelObject`
(`FillBedJob.cpp:171-184`).

### The pack

`FillBedJob::process()` (`FillBedJob.cpp:198`), on the worker thread:

* `update_arrange_params` (`Arrange.cpp:85`) — adds skirt distance to `bed_shrink_x/y` (start
  1 mm, `Arrange.hpp:130-131`); sequential print subtracts `clearance_radius/2`.
* `m_bedpts = get_shrink_bedpts(...)` (`Arrange.cpp:260`); inflation for selected and unselected
  (`FillBedJob.cpp:217-218`); `params.do_final_align = !is_bbl` (`FillBedJob.cpp:234`).
* `params.on_packed` (`FillBedJob.cpp:230-232`): `do_stop = ap.bed_idx > 0 && ap.priority == 0`.
* if `m_selected.size() > 100`, the NFP packer is skipped for a bounding-box grid
  (`FillBedJob.cpp:236-248`) from `Plater::get_empty_cells()` (`Plater.cpp:24710-24740`) — step =
  bbox + brim, no gap, no rotation, and cells checked only against `plate->get_exclude_areas()`,
  not against other objects. Otherwise `arrangement::arrange(...)` (`Arrange.cpp:1107`).

`finalize()` (`FillBedJob.cpp:260`) applies only items with `bed_idx == 0`; anything on a virtual
bed is skipped (`FillBedJob.cpp:288-292`).

## 2. Why the gaps appear

Five contributing causes, in roughly descending order of blame:

1. **The stop condition truncates the fill.** `firstfit.hpp:107` iterates `store_` checking
   `!cancelled()` each step; `cancelled()` is `params.stopcondition`, wired to `do_stop` at
   `FillBedJob.cpp:221-223`. The moment the packer opens plate 1 and places one clone there
   (`firstfit.hpp:192-200`, then `makeProgress` fires `on_packed` with `binId()==1`), `do_stop`
   flips and **the whole loop aborts** — every remaining clone is never attempted, even though
   the NFP placer might have found it a pocket on plate 0. With a greedy TOP_RIGHT start the
   first failure typically comes while space remains at the opposite corner. Biggest single
   source of "it stopped early and left a whole strip empty".
2. **The count is a pure area ratio and is systematically low.** `needed_items` divides the free
   bed area by the *inflated* item area (`FillBedJob.cpp:148,163`). Neighbours *share* the gap,
   so the real per-item footprint is `(w+g)·(h+g)`, not the fully-inflated hull area. With a big
   brim (6 / 24 mm) or a 10 mm spacing the over-count is large. (Using the raw rather than shrunk
   bed area pushes the other way, but not enough.) Under-estimating would be harmless if the
   packer stopped only when full — but with cause (1) in play, "out of items" and "out of space"
   are indistinguishable, and the bed is simply left half empty.
3. **Convex hulls, not true outlines.** `ModelInstance::get_arrange_polygon`
   (`src/libslic3r/Model.cpp:3729`) is `get_object()->convex_hull_2d(...)`, so an L-shape or a
   ring can never nest into its own concavity. Real cost for non-convex parts, but *not* what
   makes a bed of cubes look sparse.
4. **Rotation off by default, only 4 angles.** `params.allow_rotations =
   settings.enable_rotation` (`ArrangeJob.cpp:776`, default false); `fill_config`
   (`Arrange.cpp:296-299`) offers `{0,45,90,135}°`. For a rectangular part, 90° alone can change
   the count materially.
5. **Objective function / alignment.** `objfunc` (`Arrange.cpp:441-690`) blends
   distance-to-origin, pile density and neighbour alignment — not a density optimiser.
   `do_final_align` (`Arrange.cpp:288-292`) and a non-central `best_object_pos`
   (`Arrange.cpp:708-712`, `USER_DEFINED`) only move where the pile sits, not how many fit —
   cosmetic. `do_final_align = !is_bbl` here, so BBL beds keep `DONT_ALIGN`.

Fixing (1) and (2) recovers most of the missing copies; (3) and (4) set the ceiling after that.

## 3. Design for the upgrade

### 3a. The dialog

Add `FillBedDialog` (new `src/slic3r/GUI/FillBedDialog.{hpp,cpp}`), modelled on `CloneDialog`
(same `DPIDialog` + `SpinInput` + `CheckBox` + `DialogButtons` toolkit), shown by
`Plater::fill_bed_with_instances()` before the job is queued so both entry points get it:

* **Minimum gap between copies (mm)** — `SpinInput`/`DragFloat`, 0-50, default = the current
  arrange spacing (`get_arrange_settings().distance`) if non-zero, else the template's
  `ap.brim_width` so the default matches today's auto behaviour. Persisted as AppConfig
  `fill_bed/min_gap` — a *separate* key from `arrange/min_object_distance_fff`, so changing the
  fill gap does not silently retune the arrange toolbar.
* **Allow rotation** — checkbox, default off, persisted `fill_bed/allow_rotation`; feeds
  `params.allow_rotations` directly rather than `settings.enable_rotation`.
* **Will fill around existing objects** — static info text only (already what `m_unselected`
  does, `FillBedJob.cpp:89-107`).
* **Estimated copies: N** — live label from the corrected estimate below; arithmetic on the
  template bbox, no packing, so it is cheap enough to recompute on every keystroke.
* **Minimum distance from bed edge (mm)** — owner request (2026-09-10). `SpinInput`, 0-50,
  default = today's effective margin (`bed_shrink_x/y`, 1 mm, plus skirt distance) so an
  untouched dialog is bit-identical to today. Applied to all four bed sides. Persisted as
  `fill_bed/edge_margin`.
* **Front edge override (mm)** — checkbox + `SpinInput`, off by default, persisted as
  `fill_bed/front_margin_enabled` / `fill_bed/front_margin`. When on, the front strip of the
  bed (the min-Y edge in bed coordinates, the side facing the user on every supported printer)
  gets this larger margin instead of the general one. Reason: printers run dynamic flow /
  first-layer calibration (Bambu "flow dynamics" purge lines, extrusion-cali patches, Snapmaker
  U1 purge strip) along the front, and copies placed there collide with the calibration
  extrusions even though the bed shape says the area is printable. Tooltip says exactly that.
  Never reduces below the general margin: `front = max(front, edge)`.

The Clone dialog's **Fill** button routes through the same dialog, so there is one behaviour.

### 3b. Packing strategy

**(a) NFP with the exact gap, deliberate over-estimate, discard the rest.**
Set `params.min_obj_distance = scaled(gap)` from the dialog, compute `needed_items` with the
tiling formula below, multiply by ~1.3, cap at 150. Remove the `on_packed` early stop and drop
whatever lands on `bed_idx != 0` — `finalize()` already ignores those (`FillBedJob.cpp:288-292`)
and the cloning `setter` only fires inside the `bed_idx == 0` branch, so this is nearly free.
Cost: more packer work, ~O(n²), but n ≤ 150 and this path already tolerates 100.
*Change: ~60 lines in `FillBedJob.cpp`, ~80 in the new dialog.*

**(b) Grid/row packer for a single repeated shape.**
Every clone is the same shape, so tile the shrunk bed with `step = bbox + gap` in both
orientations, keep whichever yields more cells, reject cells whose inflated hull hits an exclusion
region, the wipe tower, or an unselected object. That is the `>100` branch
(`FillBedJob.cpp:236-248`) done properly — with a gap, both orientations, and collision against
`m_unselected`, not just `plate->get_exclude_areas()`. Deterministic, instant, and it *looks*
regular, which is what fill-bed users expect; wastes the leftover edge margin and helps L-shapes
no more than the hull does. *Change: ~120 lines (`fill_bed_grid()` in `FillBedJob.cpp`).*

**(c) Hybrid: grid, then NFP over the leftover pockets.**
Run (b), feed the grid-placed copies to `arrange()` as fixed items (`_arrange`'s `preload`,
`Arrange.cpp:985`), let NFP try another batch in the margins. Best density, but two code paths to
maintain and it leaves irregular stragglers beside a tidy grid — worst of both aesthetics.
*Change: (a) + (b) + ~40 lines of glue.*

### Recommendation: (a) for phase 1, (b) as an opt-in "Grid" layout in phase 2.

(a) is the smallest change that fixes the actual complaint: the gap becomes an explicit,
remembered number, and dropping the `on_packed` stop plus fixing the estimate recovers the copies
the current code silently discards. One packer; exclusion regions, wipe tower and other objects
keep working for free (already `m_unselected`); non-convex parts no worse than today. (b) follows
as a user-visible *choice* ("Grid" vs "Compact"), not a replacement — for cubes and boxes, the
common fill-bed case, a grid is denser *and* prettier than the NFP objective's output. Skip (c).

### Corrected count estimate

Replace `FillBedJob.cpp:146-163` with a tiling estimate against the **shrunk** bed:

```
free = shrunk_bed_area - unselected_area(current plate)
cell = (bbox.w + gap) * (bbox.h + gap)     // gap shared between neighbours
needed = ceil(free / cell) * OVERSHOOT     // OVERSHOOT ≈ 1.3, capped at 150
```

`get_shrink_bedpts` needs `update_arrange_params`, which today only runs in `process()`
(`FillBedJob.cpp:206-207`), after `prepare()` has already sized the clone list. Either move the
estimate into `process()`, or call `update_arrange_params` in `prepare()` and make it idempotent
— it `+=`s into `bed_shrink_x/y` (`Arrange.cpp:91-92`), so a second call double-counts the skirt;
guard with a `params.bed_shrink_applied` flag.

### Sequential printing and brim

* `is_seq_print` must keep winning: `update_selected_items_inflation` (`Arrange.cpp:105-112`)
  raises `min_obj_distance` to at least `clearance_radius + 0.001` (tall) or
  `max(MAX_OUTER_NOZZLE_DIAMETER/2, object_skirt_offset*2)` (all short). The dialog gap must go
  through the same `params.min_obj_distance` field so `std::max` still applies — **set the field,
  do not bypass the function** — and the dialog should say when seq print raises it. Note
  `init_arrange_params` (`ArrangeJob.cpp:792-793`) zeroes `min_obj_distance` when the plate's
  print sequence differs from global, so apply the dialog value *after* it returns.
* Brim: `ap.brim_width` (`ModelArrange.cpp:162-176`) is *not* added to inflation once an explicit
  distance is given — `Arrange.cpp:122` takes the `min_obj_distance/2` branch. A trap for the new
  dialog: gap 1 mm with tree support (brim 24 mm) means colliding brims. Phase 1 should treat the
  dialog value as a gap **between outlines**, using `inflation = max(gap, brim_width)/2`, or at
  minimum warn when `gap < brim_width`.

## 4. Where the gap must NOT apply

* **Bed edge.** The edge margin is `bed_shrink_x/y` (`Arrange.hpp:130-131`, default 1 mm) plus
  skirt distance (`Arrange.cpp:90-92`), applied by `get_shrink_bedpts` (`Arrange.cpp:260`). The
  dialog gap must not be added to it — an item may sit `bed_shrink` from the edge even at a 20 mm
  copy gap. The new estimate uses `get_shrink_bedpts` for its area, and `min_obj_distance` keeps
  feeding only `ap.inflation`, never `bed_shrink_*`. Do not touch `update_arrange_params`.
  The *user's* edge margin and front override are a separate, fill-only shrink applied to the
  bed points **after** `get_shrink_bedpts`: a per-side variant of `shrinkFun` (`Arrange.cpp:264`)
  that moves the min-Y edge by `max(front, edge) - bed_shrink_y` and the other three sides by
  `edge - bed_shrink_{x,y}` (clamped at 0, so the dialog can never place a copy *closer* to the
  edge than arrange would). Keep it in `FillBedJob` (a small helper next to `prepare()`), not in
  `Arrange.cpp`, so plain Arrange is untouched. The corrected count estimate uses the same
  shrunk polygon, so the live "Estimated copies" label already reflects the front strip. For
  a non-rectangular (circular / custom) bed the same edge move applies to every point by the
  sign of its offset from the centre, which is what `shrinkFun` does today; the front override
  then moves only points with `y < centre.y`, i.e. the front half — acceptable for a strip.
  Existing objects and exclusion regions are not shrunk or moved by either margin.
* **Exclusion areas** keep their own inflation rule (`Arrange.cpp:150-152`): virtual objects get
  `exclusion_gap` (1 mm, or clearance-derived under seq print) and extrusion-cali regions get 0 —
  the user's copy-to-copy gap must not leak into those either.
* **Plate boundaries.** `FillBedJob` fills *one* plate: `prepare()` selects the current plate
  (`FillBedJob.cpp:36`) and anything outside `plate_bb` goes to `m_locked` with
  `bed_idx = MAX_PLATES_COUNT` (`FillBedJob.cpp:83-86, 103-106`). Plates are separated by
  `bed_stride_x/y` (`ArrangeJob.cpp:751-759`, `LOGICAL_BED_GAP`), unrelated to the gap — never add
  it to the stride, and never let copies land on plate 1+ ((a)'s discard rule enforces this).

## 5. Test plan

### Unit level, no GUI

New `tests/libslic3r/test_fill_bed_pack.cpp` (add to `tests/libslic3r/CMakeLists.txt`). The packer
is pure libslic3r (`arrangement::arrange(items, excludes, bed, params)`), testable like
`test_polygon.cpp`.

1. **Square, exact gap.** 20x20 mm square hull, 250x250 `BoundingBox` bin, `gap = 5 mm` →
   `min_obj_distance = scaled(5)`, inflation 2.5 each. Feed 200 copies. Assert: (a) every
   `bed_idx == 0` item's transformed hull is inside the shrunk bed; (b) **min pairwise distance
   between placed hulls ≥ gap - 1e-3 mm** (O(n²) polygon-distance loop is fine at n ≤ 200);
   (c) placed count ≥ the tiling estimate (`floor(250/25)² = 100`; allow ≥ 90 for bed shrink).
2. **Same square, `gap = 0`** → count ≥ the `gap = 5` count. Catches the gap leaking into
   `bed_shrink`.
3. **L-shape** (12x12 with a 6x6 notch). Because `get_arrange_polygon` hulls, the packer sees a
   12x12 square; assert the count matches the square case, with a comment that beating it means
   dropping the convex hull (cause 3). Pins current behaviour so an outline change shows up.
4. **Early-stop regression.** Build `ArrangeParams` with the old `on_packed`/`stopcondition` pair
   and assert the placed count is *lower* than without it — direct unit-level proof of cause (1),
   and it fails loudly if anyone reinstates the stop.
5. **Count estimate.** Extract `fill_bed_estimate(bbox, gap, free_area)` as a free function so it
   is testable without wx. Assert `estimate(20x20, gap 5, 250x250) ≈ 100` and that it decreases
   monotonically in `gap`.

None of these need wx or `Plater` — `libslic3r` only, matching `tests/libslic3r/CMakeLists.txt`.
Run: `build\tests\libslic3r\Release\libslic3r_tests.exe "[fill_bed]"`.

### Owner click-tests

1. 20 mm cube, right-click → Fill: dialog appears, gap prefilled; gap 3 mm → visibly full bed,
   copies ~3 mm apart, no empty strip. Repeat at 15 mm: fewer copies, still no empty strip, and
   still ~1 mm from the bed edge (not 15 mm).
2. Clone dialog → Fill button: same dialog, same result.
3. One object parked to the side first: copies flow around it, still 3 mm clear of it.
4. BBL-vendor profile with exclusion areas: no copy in the excluded corner. Wipe tower enabled
   (2+ filaments): no copy overlaps it.
5. Print sequence "By object", gap 1 mm: gap silently raised to extruder clearance, and a slice
   reports no clearance conflict. Tree-support object, gap 1 mm: brims do not collide.
6. Fill twice in a row: second run adds nothing, no crash. Undo removes every copy in one step
   (`take_snapshot` at `Plater.cpp:21060`).
7. Multi-plate project with plate 2 selected: only plate 2 fills. Reopen the dialog: gap and
   rotation remembered.
8. Edge margin 10 mm: no copy nearer than 10 mm to any bed edge; existing parked object is
   left where it was. Front override on at 40 mm: the front 40 mm strip stays empty on a Bambu
   A1/P1S profile (compare against the purge-line area in the plate preview), the other three
   sides keep 10 mm. Front override at 5 mm with edge 10 mm: front still 10 mm (max rule).
   Margins 0/0: identical layout to today's arrange margin (1 mm).

## 6. Phased plan

**Phase 1 — the gap is a number the user sets, and the count is right.**
* New `FillBedDialog`; `Plater::fill_bed_with_instances()` shows it and hands gap +
  allow-rotation to `FillBedJob` (ctor args or setters).
* `FillBedJob::prepare()` sets `params.min_obj_distance = scaled(gap)` *after*
  `init_arrange_params`, plus `params.allow_rotations` from the checkbox.
* Replace the `needed_items` area ratio with the tiling estimate against the shrunk bed, with
  overshoot and a cap.
* **Remove the `on_packed`/`do_stop` early stop** (`FillBedJob.cpp:220-232`); rely on
  `finalize()` discarding `bed_idx != 0`. Keep `ctl.was_canceled()` in `stopcondition`.
* `inflation = max(gap, brim_width)/2` in the fill path, or a warning; persist
  `fill_bed/min_gap`, `fill_bed/allow_rotation`, `fill_bed/edge_margin`,
  `fill_bed/front_margin_enabled`, `fill_bed/front_margin`; unit tests 1, 2, 4, 5 above.
* Edge margin + front override: per-side bed shrink helper in `FillBedJob` applied after
  `get_shrink_bedpts` (section 4), fed to both the estimate and `arrangement::arrange`; unit
  test: square bed, 20 mm square, edge 10 / front 40 → every placed bbox has `min.y >= 40`,
  `min.x >= 10`, `max.x <= W-10`, `max.y <= H-10`.
* Estimated ~200 lines added, ~30 changed. No change to `Arrange.cpp` beyond possibly the
  idempotence guard in `update_arrange_params`.

**Phase 2 — tighter packing.**
* "Layout: Compact / Grid" choice in the dialog; Grid implements (b) with both orientations and
  exclusion/wipe-tower/other-object rejection, replacing the ad-hoc `>100` branch
  (`FillBedJob.cpp:236-248`) so there is one grid implementation, not two.
* Optionally widen the NFP rotation set beyond `{0,45,90,135}°` (`Arrange.cpp:296-299`) — measure
  first, more angles cost real time.
* Optionally use the true 2D outline instead of the convex hull, fill path only
  (`Model.cpp:3729`) — biggest density win for non-convex parts, biggest risk, own branch.
* Re-run tests 1-3 against Grid, plus a Grid-specific count assertion.
