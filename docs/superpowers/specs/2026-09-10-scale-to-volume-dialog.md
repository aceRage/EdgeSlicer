# Scale to build volume: ask for mode, gaps, and centering before scaling

Research spec. Branch would be `feat/scale-to-volume-dialog`, off `feat/ultra-preferences`.
Refs are from this tree (`C:\Dev\SnapmakerOrcaPhone`, `feat/ultra-preferences`) unless marked
`[Bambu]` for the read-only reference clone at `C:\Dev\BambuStudio`.

## 1. Current behaviour, traced

Menu: `MenuFactory::append_menu_item_scale_selection_to_fit_print_volume`
(`src/slic3r/GUI/GUI_Factories.cpp:1089-1094`) calls `plater()->scale_selection_to_fit_print_volume()`
gated on `can_scale_to_print_volume()`. Wired into the common object menu (`GUI_Factories.cpp:1791`)
and the BBL/Ultra extra menu (`:1856`, built separately, comment notes the common entry never
reaches it) — no SLA/FFF split at the menu level.

`Plater::scale_selection_to_fit_print_volume()` (`Plater.cpp:21149`) forwards to
`Plater::priv::scale_selection_to_fit_print_volume()` (`Plater.cpp:13616-13623`), compiled under
`ENABLE_ENHANCED_PRINT_VOLUME_FIT` (`libslic3r/Technologies.hpp:61`, `1 && ENABLE_2_4_0_BETA2`,
and `ENABLE_2_4_0_BETA2=1` at line 54 — so this build always takes the enhanced path):
```cpp
this->view3D->get_canvas3d()->get_selection().scale_to_fit_print_volume(this->bed.build_volume());
```
The `#else` overload, `Selection::scale_to_fit_print_volume(const DynamicPrintConfig&)`
(`Selection.cpp:1477-1519`), is dead code in this build (rectangle-only, no circle case) — worth
flagging as removable, but no runtime path exercises it.

**Enabler** `can_scale_to_print_volume()` (`Plater.cpp:17486-17494`): not empty, no cut object
selected, bed type is `Rectangle` or `Circle` (Convex/Custom disables the item — no fallback math,
inherited unchanged by the dialog).

**Live overload** `Selection::scale_to_fit_print_volume(const BuildVolume&)` (`Selection.cpp:1386-1475`).
Shared `fit(s, offset)` lambda (`1388-1414`): no-ops if `s<=0.0 || s==1.0`; else
`take_snapshot("Scale To Fit")` (`1392`, the one undo label), `TransformationType` set to
`World|Relative|Joint` (`1394-1397`, i.e. `World_Relative_Joint`, `GUI_Geometry.hpp:33`),
`setup_cache()` → `scale(s*Vec3d::Ones(), type)` → `do_scale("")`, then `setup_cache()` →
`translate(offset, {relative})` → `do_move("")` (empty snapshot strings = "don't push another
snapshot", per `GLCanvas3D::do_scale`/`do_move`, `GLCanvas3D.cpp:5160-5165`, which only calls
`take_snapshot` `if (!snapshot_type.empty())`). One snapshot, two mutations — the pattern the
dialog must preserve.

**Rectangular bed** (`fit_rectangle`, `1416-1429`): `print_volume = volume.bounding_volume()`
(`libslic3r/BuildVolume.hpp:53`, `m_bboxf` — AABB of `printable_area()`+`printable_height()` only;
**never reads `bed_exclude_area`**, referenced elsewhere e.g. `Plater.cpp:10891,24131` but not by
`BuildVolume`'s derivation). `box_size = get_bounding_box().size() + 0.02*Vec3d::Ones()` (`1421`,
the 0.01mm-per-side slack). `factor = min(sx,sy,sz)`, offset recenters on `print_volume.center()`.
Wipe tower is a separate `GLVolume` (`is_wipe_tower`) structurally excluded from ordinary
selections (`Selection.cpp:167,175-176`), so it's never touched by this feature either way.

**Multi-object**: `get_bounding_box()` (`Selection.cpp:891-903`) merges
`transformed_convex_hull_bounding_box()` over every volume in `m_list`, no `is_modifier`/
`is_wipe_tower` filter. With `Joint` ("group scaled as one solid body", `GUI_Geometry.hpp:28-29`),
a multi-object selection scales/moves as one rigid group about the combined bbox center — already
today's behaviour, unchanged by the dialog.

**Circular bed** (`fit_circle`, `1431-1464`): smallest enclosing circle (Welzl) of the selection's
world-space convex hull, `+0.01` slack (`1454`), `factor = min(print_radius/circle_radius,
printable_height/max_z)`. Not an "inscribed rectangle" — there is no rectangle step; correct this
framing when implementing. (The `0.02`/`0.01` slacks on the rectangle/circle paths are unconditional
anti-float-rounding, unrelated to the new gap settings — a gap default of `0` still leaves them in
place.)

**Undo**: `take_snapshot("Scale To Fit")` (`1392`) is the only snapshot; both `do_scale("")`/
`do_move("")` skip pushing another (see above). The dialog keeps this: one snapshot before the
first mutation, everything else empty-string.

## 2. Dialog design (house style: `FillBedDialog`)

Mirror `src/slic3r/GUI/FillBedDialog.{hpp,cpp}` (just merged): `DPIDialog`, `wxFlexGridSizer(2,2,..)`,
`::Label` never bare `wxStaticText` (`FillBedDialog.cpp:113-117`: a `wxStaticText` reports the
system button-face colour as its background and `UpdateDlgDarkUI()` paints a visible grey band
behind it in dark mode; `::Label` copies the parent's background instead), `::TextInput` mm fields
with `wxFILTER_NUMERIC` + live `wxEVT_TEXT` recompute, `::CheckBox`, `DialogButtons(this,
{"OK","Cancel"})`, `wxGetApp().UpdateDlgDarkUI(this)` at the end of the ctor, `on_dpi_changed`
re-fitting.

New files `src/slic3r/GUI/ScaleToVolumeDialog.{hpp,cpp}` (~90 / ~230 lines, matching FillBedDialog's
~106/~365 split). `struct ScaleToVolumeSettings { enum class Mode { Uniform, NonUniform } mode =
Uniform; double edge_gap_mm = 0.; double top_gap_mm = 0.; bool auto_center = true; };` — edge/top
gaps are X/Y bed-edge and max-height clearances.

AppConfig section `scale_to_volume` (mirrors `FillBedDialog.cpp:18-24,30-62`'s `CFG_*` +
clamp-on-load pattern): keys `scale_to_volume/mode` (`"uniform"`/`"nonuniform"`, default uniform),
`/edge_gap`, `/top_gap` (mm strings, default `"0.00"`, clamp `[0, MARGIN_MAX]`, reuse or mirror
`FillBedDialog.cpp:26`'s `MARGIN_MAX=50.`), `/auto_center` (bool string, default `true`).

Rows in order: Scale mode (`::ComboBox`, Uniform/Non-uniform, like the Layout combo at
`FillBedDialog.cpp:150-162`), Edge gap `TextInput`, Top gap `TextInput`, Auto-center `::CheckBox`
(default checked). A red `::Label` warning row (styled like `m_warning_text`,
`FillBedDialog.cpp:221-227`, `#D9534F`) plus `dlg_btns->GetOK()->Enable(false)` when a resulting
dimension would be non-positive (§3) — new; `FillBedDialog` warns but never disables OK.

**Where Plater shows it**: `Plater::priv::scale_selection_to_fit_print_volume()` (`Plater.cpp:13616`)
opens the dialog first (`load_from_config()` defaults, `ShowModal()`, bail on Cancel); both
overloads gain a trailing `const ScaleToVolumeSettings&` parameter so the (dead, §1) `#else` path
stays source-compatible — the settings then thread into `...get_selection().scale_to_fit_print_volume(this->bed.build_volume(), s)` (or `(*config, s)` under the `#else`), same `#if
ENABLE_ENHANCED_PRINT_VOLUME_FIT` split as today.

## 3. Math

Bed AABB `(W,D,H)` from `bounding_volume()`; object bbox `(w,d,h)` from `get_bounding_box().size()`
plus the existing unconditional slack.

**Uniform**: `factor = min((W-2*edge_gap)/w, (D-2*edge_gap)/d, (H-top_gap)/h)`. Same `fit()` call;
offset recenters on the gap-shrunk volume's own center (top gap only shifts usable height, Z is
always drop-to-bed, never centered).

**Non-uniform**: `fx=(W-2g)/w, fy=(D-2g)/d, fz=(H-t)/h`, applied via `Selection::scale(Vec3d(fx,fy,fz),
type)` — a real `Vec3d` instead of `s*Vec3d::Ones()` (contrast `1401`). **Frame: keep
`World_Relative_Joint`.** Traced through `transform_instance_relative` (`Selection.cpp:3193-3208`):
with `world()` true, the new transform composes *outside* the instance matrix (`trafo *
inst_trafo.get_matrix()`), so a non-uniform world-frame scale shears once composed with any existing
rotation (diagonal scale and rotation don't commute unless uniform). **Recommendation: world-frame
scale of the bounding box** — for a 45°-rotated cube this makes its AABB fill the target footprint;
the mesh is sheared (rotated cube becomes a rotated parallelepiped), correct for "fills the bed on
all three axes," not "preserve shape." Tooltip: *"Non-uniform scaling fits the object's bounding box
to the volume; a rotated part is sheared, not rotated, to fit."* The alternative (instance/local
frame, scale before rotation) preserves shape but makes the rotated footprint miss the target on the
diagonal — worse for the stated goal.

**Circular beds**: unchanged mechanism — smallest-enclosing-circle fit, not an inscribed rectangle
(correcting the brief's framing). Edge gap subtracts from the bed radius: `effective_radius =
print_circle_radius - edge_gap`; top gap subtracts from `printable_height()` the same way.
Non-uniform has no well-defined per-axis target on a circular footprint — recommend disabling the
Non-uniform combo entry (or silently falling back to Uniform) when `volume.type() ==
BuildVolume_Type::Circle`; flagged as an open implementation question, not resolved here.

**Clamp**: if `(W-2*edge_gap)`, `(D-2*edge_gap)`, `(H-top_gap)`, or `effective_radius*2` is `<=0`,
show the warning and disable OK — `fit()`'s existing `s<=0.0` guard (`1389`) would otherwise silently
no-op, which reads as a bug rather than a rejected input.

## 4. Auto-center off

Z unchanged, always drop-to-bed via `offset.z() = -get_bounding_box().min.z()` (`1406`). X/Y: skip
the `print_volume.center() - get_bounding_box().center()` recentring (`1428`), translate only the
minimum needed to bring the gap-shrunk post-scale AABB inside bounds — per-axis clamp of box
min/max against `[vol.min+edge_gap, vol.max-edge_gap]`, shifting only the violated direction, zero
otherwise. Circular bed: clamp box-center distance from bed-circle center to `effective_radius` if
exceeded (same minimal-shift idea, along the bed-center-to-box-center vector).

## 5. Edge cases

- **SLA**: no separate `Plater` SLA branch — `can_scale_to_print_volume` and both overloads are
  printer-technology-agnostic; SLA gating elsewhere (`Plater.cpp:22195,25811,25841`) doesn't touch
  this path; applies identically to SLA and FFF.
- **Assemble view**: `scale_and_translate` (`Selection.cpp:1599`) already special-cases
  `CanvasAssembleView` (skips `requires_check_outside_state()`); math is canvas-agnostic. Follow-up:
  confirm its context menu exposes this item (not inspected here).
- **Cut objects**: already excluded via `has_selected_cut_object()` (`Plater.cpp:17490`) — menu disabled.
- **Per-instance scale**: only the `Absolute`-transform branch of `scale_and_translate`
  (`1529-1539`) cares about existing factors; this feature always uses `Relative` (`1396`), so
  existing per-instance factors are preserved multiplicatively, same as today.
- **Negative volumes/modifiers**: not filtered from `get_bounding_box()` or the per-volume loop
  (`1544-1586`) — scale/translate with the object, unchanged.
- **Undo label**: stays `"Scale To Fit"` (`1392`) though the menu reads "Scale to build volume" —
  flag as a nice-to-have rename, not required here.

## 6. Test plan (click-tests; GUI `Selection` math isn't unit-testable in isolation)

1. Cube, Uniform, gaps 0 — matches today exactly (regression baseline).
2. Cube, Non-uniform, gaps 0 — panel shows different scale % per axis; bbox X/Y/Z each equal bed
   W/D/H within the 0.01 slack.
3. Cube rotated 45° about Z, Non-uniform — resulting world-frame AABB fills bed W/D/H; mesh visibly
   sheared (confirms the documented frame choice).
4. Edge gap 5, top gap 10, Uniform — panel Size = bed dims minus 2×5 (X/Y) minus 10 (Z); object
   sits >=5mm from every bed edge, >=10mm below max height.
5. Auto-center off, asymmetric starting XY — XY center unchanged after scale (within the minimal
   clamp), Z still 0.
6. Circular bed — Uniform fits the enclosing circle inside `bed_radius - edge_gap`; Non-uniform
   disabled or falls back to Uniform.
7. Two objects selected — group scales/moves as one rigid body about combined bbox center;
   relative offset between them preserved.
8. Gaps large enough to make a dimension <=0 — OK disabled, warning shown.
9. Undo — one Ctrl+Z fully reverts scale+position in one step (Undo history shows one "Scale To
   Fit" entry).

## 7. Estimated lines per file

`ScaleToVolumeDialog.hpp` ~90 (cf. `FillBedDialog.hpp`'s 106); `.cpp` ~230 (ctor, load/save config,
warning recompute, `on_dpi_changed`); `Selection.hpp` +2 (trailing param, both overloads);
`Selection.cpp` +50/-10 (`fit` gains gap/mode params, non-uniform branch, auto-center-off
translation, circle gap subtraction); `Plater.cpp` +15 (shows dialog, threads settings through);
`GUI_Factories.cpp` 0 (menu wiring unchanged). Roughly 320 new lines, ~75 changed.

## Summary

Today `Selection::scale_to_fit_print_volume(const BuildVolume&)` (`Selection.cpp:1386-1475`)
uniformly scales by `min(sx,sy,sz)` with a fixed 0.01mm slack, drops to bed, recenters, in one undo
step (`take_snapshot` + two no-snapshot `do_scale`/`do_move` calls). It ignores `bed_exclude_area`
(never read by `BuildVolume`'s bbox); the wipe tower is structurally excluded from selections
already. Multi-object selections scale/move as one rigid group via `World_Relative_Joint`. The new
`ScaleToVolumeDialog` (house style copied from the just-merged `FillBedDialog`) adds
Uniform/Non-uniform mode, edge/top gaps, and auto-center, persisted under AppConfig section
`scale_to_volume`. Non-uniform mode reuses `World_Relative_Joint` with a per-axis `Vec3d` instead
of a scalar.

**Recommended frame: world frame (today's, unchanged)** — `transform_instance_relative`'s world
branch composes the new scale outside the existing rotation, so a non-uniform world scale on a
rotated instance shears the mesh rather than rotating a scaled box — the right trade-off for
"make the AABB fill the bed." The alternative (instance/local frame) preserves shape but makes the
rotated footprint miss the target dimensions on the diagonal, defeating the feature's purpose.
