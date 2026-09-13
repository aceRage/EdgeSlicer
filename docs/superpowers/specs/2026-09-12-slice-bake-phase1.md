# Slice baking, phase 1: what was built

Implements phase 1 of `docs/superpowers/specs/2026-09-12-slice-bake-research.md` — the
"Exact layers" bake: turn a SLICED object's outer wall, as it will actually be extruded, into a
watertight mesh that can be re-sliced.

Branch `feat/slice-bake`, from `feat/ultra-preferences` (944457b8bb).

## The driving scenario

Slice a part at 0.3 mm **with** fuzzy skin, bake the sliced appearance to a mesh, then re-slice
that baked mesh at 0.12 mm **without** fuzzy skin. The coarse 0.3 mm fuzzy pattern is now carried
by the mesh itself, so it prints at 0.12 mm quality — the same texture, 2.5× finer layers, and no
wall jitter of the slicer's own.

This is what the bake is for, and it is the acceptance test (scenario (b) below).

## What was built

### 1. `libslic3r/SliceBake.{hpp,cpp}`

From a sliced `PrintObject`, per layer:

1. Walk `LayerRegion::perimeters` of **every** region of the layer (so painted / MMU objects,
   which are split into several `PrintRegion`s at slice time, contribute all of their outer wall).
2. Keep only the outer-wall roles: `erExternalPerimeter`, plus `erOverhangPerimeter` and this
   fork's `erOverSupportPerimeter` — the same physical loop split by role where it leaves the
   layer below. Dropping those two punches holes in exactly the overhanging bands.
3. Per **path**, not per loop, offset the stored centreline polyline outward by that path's own
   `width`/2 (via `ExtrusionPath::polygons_covered_by_width`, which is the same relationship the
   rest of the tree uses for "what does this extrusion cover"). Per-path matters because a loop
   crossing an overhang has paths of differing width, and one width would be wrong on whichever
   half did not supply it. A `ClipperSafetyOffset`-sized overlap is added so neighbouring paths of
   one loop union without leaving a zero-width crack at the joint.
4. Union the layer's footprints with `pftNonZero`, which **fills** each closed loop's interior.
   This is what makes the bake a solid rather than a hollow replica — and what makes top and
   bottom surfaces implicit: a filled top layer already is the top skin, so `erTopSolidInfill` /
   `erBottomSurface` never have to be read.
5. Turn each layer's `ExPolygons` into a closed prism over its own `[bottom_z, print_z]` band
   and stack them — see section 2, which is where the watertightness comes from.

Exclusions are **by construction**, not by filtering:

| Excluded | Why it cannot appear |
|---|---|
| Supports, support interface | live in `SupportLayer`; the bake only walks `PrintObject::layers()` |
| Brim, skirt | separate `ExtrusionEntityCollection`s on `Print` / `PrintObject` |
| Wipe tower | a separate structure on `Print` |
| Infill, inner perimeters | never read — everything radially inward of the outer loop is filled anyway |

Options (`SliceBakeOptions`): layer subset as a half-open `[layer_begin, layer_end)` (default all),
`close_gaps_radius` in mm (default 0 = the morphological close is skipped entirely, so the layer
polygons come out of the union exactly as Clipper produced them), and `in_object_frame` (default
on), which applies `trafo_centered().inverse()` so the bake lands where the `ModelVolume`'s own
mesh sits and can replace the object in place. A negative-determinant instance transform has its
winding flipped back, so a mirrored object does not bake inside-out.

Determinism: the layer loop is serial and ordered, the per-layer union is Clipper's over a
fixed-order input, and nothing is hashed. Two bakes of one slice are byte-identical — asserted.

### 2. The loft: one closed prism per layer

The bake's first draft lofted the stack with the existing `Slic3r::slices_to_mesh`. That loft is
not watertight on anything but a prism, and the fix was to stop using it.

**The defect.** `slices_to_mesh` builds the horizontal caps between consecutive layers from
Clipper diffs - `diff_ex(lower, upper)` for the free top, `diff_ex(upper, lower)` for the
overhang - while the vertical walls come from the layer polygons themselves. A diff introduces
**intersection vertices** at every crossing of the two contours, and those vertices sit in the
middle of wall edges that have no vertex there. The two meshes therefore meet along edges that
are split on the cap side and whole on the wall side: T-junctions, which `its_merge_vertices`
cannot weld, because the vertices are not coincident - they are genuinely different points. The
function's own comment says so: *"FIXME: these repairs do not fix the mesh entirely. There will
be cracks in the output."* A prismatic cube never differs layer to layer, so it produces no diff
and no T-junction, which is exactly why it alone came out clean, and why the defect is invisible
in the SLA path this loft was written for.

Measured open edges with that loft: **0** on the plain cube, **20,264** on the fuzzy cube,
**15,245** on the cylinder.

**The fix - approach (b) of the three considered, chosen by measurement.** Each layer becomes its
own **closed prism**: a bottom cap at `bottom_z`, vertical walls, and a top cap at `print_z`, all
three generated from the *same* contour point set. The prism is watertight on its own, by
construction - there is no diff anywhere in the construction and no tolerance anywhere in it.
Stacking the prisms and welding only **exactly coincident** vertices then leaves a mesh whose only
internal structure is pairs of coplanar, oppositely-wound cap faces between neighbouring layers.
Each such face is somebody's top and somebody's bottom, so every edge still carries an even number
of half-edges in each direction and the whole stack is closed.

Why not (a), splitting the wall loop edges at every cap vertex that lies on them: it needs a
point-on-segment predicate on scaled integers where the cap vertex is a *double* produced by
Clipper's intersection arithmetic, so "lies on" becomes a tolerance question - and the one thing
the crack-free construction must not have is a tolerance. (b) removes the question instead of
answering it.

The price of (b) is the coplanar internal faces where two layers overlap. Nothing downstream
minds: `its_num_open_edges()` is 0, `TriangleMesh`'s statistics report no open edges and no
repair needed, and the slicer cuts the same contours either way - the internal faces are
horizontal, so no Z plane but a band boundary ever touches one, and a band boundary is a plane
the printed layer already had.

The cap triangulation is still the existing GLU tesselator. It emits its vertices verbatim from
the coordinates handed in - the same `unscale<double>()` the wall vertices go through - so each
cap vertex is matched back to its own contour point by **exact double equality**, and the cap and
the wall end up sharing the mesh vertex. The one vertex GLU can emit that is not an input point is
a `tessCombine` vertex, produced only for a self-intersecting contour; `union_ex` output is not
self-intersecting, and if one ever appeared the triangle is dropped and counted into
`report.note` rather than welded to the wrong place.

Per-layer prisms also make the per-layer `bottom_z` / `print_z` exact for free: a variable or
adaptive layer height, and a layer subset that does not start at the bed, both come out right
without a uniform grid having to be derived for them.

`slices_to_mesh(slices, zmin, grid)` - the grid overload the first draft used - remains declared
in `SlicesToTriangleMesh.hpp` (it was already the implementation the two public overloads sit on,
just not in the header). No behaviour change; the bake simply no longer calls it.

### 3. GUI

- **Menu**: object right-click → "Bake slice to mesh…", in `create_extra_object_menu` beside
  Repair/Remesh. Enabled only when exactly one object is selected, the background slicing process
  is idle, that object's `PrintObject` has `posPerimeters` done, and `slice_bake_available()` finds
  an outer-wall loop. `posPerimeters` rather than a full G-code export, because perimeters are
  precisely what the bake reads.
- **Dialog** (`SliceBakeDialog`, built the way `RemeshDialog` is — it collects options and nothing
  else): **Result** = Replace object / Add as new object / Export STL…, a **Close small gaps**
  checkbox with a radius, and the **size line**, which states the layer count and an estimated
  triangle count **before** the run. That line is the reason the action has a dialog at all: a
  lofted 100 mm part at 0.2 mm is 500 stair-stepped bands.
- **Job** (`Jobs/SliceBakeJob`), through the existing Jobs system like `FillBedJob` /
  `ColorSplitJob`: progress via `ctl.update_status`, cancel via `ctl.was_canceled()` threaded into
  the library's progress callback (which raises `SliceBakeCancelled`). Everything that touches the
  `Model` happens in `finalize()`, on the main thread.
- **Replace** is one `Plater::TakeSnapshot`, so one Undo puts the original back. It calls
  `clear_before_change_mesh()` first — the bake's vertex indices bear no relation to the source's,
  so painted supports, seams, fuzzy skin and MMU colour are dropped (and the existing
  "custom supports removed" notification fires). The object keeps its position (the bake is
  returned in the object's frame and the volume's transform is reset to identity) and its print
  settings (the `ModelObject` and its config are untouched). Modifiers, negative volumes and
  support blockers are left alone; only model **parts** are replaced, and only by one volume,
  since the bake is a single solid covering all of them.
- After replacing, `changed_mesh()` schedules the background process and the plate's
  `update_slice_result_valid_state(false)` makes the stale slice visible in the UI, so re-slicing
  happens against the new mesh.
- The STL path asks for the filename on the main thread, before the job starts.

## Building here — the silent exit-255 trap

**Never run the MSVC link in the foreground of a Bash/PowerShell tool call.** Linking
`fff_print_tests.exe` or `libslic3r_gui` takes several minutes and overruns the tool's own
timeout. When that fires it kills the process tree, and `cmake --build` reports it as **exit 255
with no `error LNK`, no `Build FAILED`, and no executable** — which reads exactly like a link
failure in your own code and is not one. A 32 MB `/v:diagnostic` MSBuild log of one such run
contained zero compiler or linker diagnostics; it simply truncated mid-compile.

Launch detached instead, and poll for a sentinel:

Use **two** CRLF batch files, and let the inner one do its own redirect — `Start-Process` with
*both* `-RedirectStandardOutput` and `-RedirectStandardError` stalls the detached build at the
vcvars64 banner and never writes a byte:

```powershell
# bake_build.bat (CRLF):
#   set PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;<cmake bin>;%PATH%
#     (vswhere.exe lives in that Installer dir; without it vcvars64 cannot find the toolchain,
#      and cmake is not on PATH for a detached process at all)
#   call "...\VC\Auxiliary\Build\vcvars64.bat"
#   cmake --build C:/Dev/wt_bake/build --config Release --target %1 -- /nodeReuse:false /m:2
#   echo BUILD_EXIT=%ERRORLEVEL%
# bake_wrap.bat (CRLF):
#   call bake_build.bat %1 > bake_%1.log 2>&1
Start-Process -FilePath "$S\bake_wrap.bat" -ArgumentList 'fff_print_tests' `
              -WindowStyle Hidden -PassThru
```

then `until grep -q "BUILD_EXIT=" bake_fff_print_tests.log; do sleep 30; done`. The log stops
growing well before the process exits, so "no new output" is **not** a finished signal — only the
sentinel is. A cold build of `fff_print_tests` (configure + every dep + libslic3r) took about
25 minutes here with `/m:2`; the incremental rebuild after a one-file test change, under a minute.

Serialise against the other agents building on this PC with
`sh <snorca_hubtest>/with_build_lock.sh <command>` — but be careful how the command is handed
over. Wrapping `cmd.exe /c "<bat>" <arg>` inside a PowerShell `-ArgumentList` string mangles the
quoting: the lock is taken, the child never starts, **no log ever appears**, and every other
agent blocks behind a lock whose owner is doing nothing. Pass the batch file to `Start-Process`
directly, or give the lock script its command as separate arguments.

Three further traps hit on this branch:

- **Concurrent MSBuilds corrupt the PDBs.** Three runs (`/m:4`, `/m:2`, `/m:1`) were alive against
  one build tree because the wrapper commands returned while MSBuild kept running. The result was
  `error C1090: PDB API call failed, error code '23'` scattered across unrelated files. Kill every
  stray `cl`/`MSBuild`/`link` before starting a build, and confirm the count is zero.
- **`/m:1` does not serialize compilation.** The generated command line carries `/MP`, so MSVC
  still forks parallel compilers inside the single node.
- Running the test exe needs its own directory on `PATH` (31 DLLs sit beside it); invoke it by
  full path with `cd /d` to that directory, or it exits 9009.

## Tests — `tests/fff_print/test_slice_bake.cpp`, tag `[slice_bake]`

Measured 2026-09-12 on this PC (Release, `fff_print_tests.exe "[slice_bake]"`):
**8 cases, 8 passed; 6915 assertions, all passed.**

| Case | Result |
|---|---|
| (a) 20 mm cube @ 0.2 mm | **0 open edges**, 101 distinct Z levels / 100 layers, volume **8000.02 mm³ vs 8000 nominal (+0.0002%)**, bbox 20×20×20. 0.019 s |
| (b) 0.3 mm fuzzy → bake → 0.12 mm re-slice, fuzzy OFF | **0 open edges**; 67 layers, 25,220 triangles; re-slice max deviation **0.481 mm** against a derived bound of 0.51; in-band **0.063** vs cross-band **0.205** mm. 0.156 s |
| (c) cylinder r=8 h=12 @ 0.2 mm | **0 open edges**, 60 layers / 61 Z levels, radius fitted **8.0009 mm vs 8 source (0.0009 mm out)**. 0.038 s |
| (d) supports/brim/skirt excluded | 49 support layers plus a 3 mm brim and a skirt in the source; bake volume **960.003 mm³ vs the T's exact 960**, bbox 20×20×12. 0.017 s |
| layer subset | `[10,30)` → 20 layers, bbox z 2.0 .. 6.0 exactly. 0.005 s |
| determinism | two bakes of one fuzzy slice byte-identical (vertices and indices). 0.101 s |
| close_gaps_radius = 0.1 | **0 open edges**; volume 500.248 open, 500.248 closed. 0.012 s |
| 100 mm @ 0.2 mm benchmark | see below. 0.063 s |

### Watertightness: the whole point of the loft rewrite

Open edges, before the prism loft and after:

| Model | `slices_to_mesh` (before) | prisms (after) |
|---|---|---|
| 20 mm prismatic cube @ 0.2 mm | 0 | **0** |
| 20 mm cube, 0.3 mm fuzzy skin | 20,264 | **0** |
| cylinder r=8 h=12 @ 0.2 mm | 15,245 | **0** |
| 10 mm fuzzy cube, gaps closed | (not reached) | **0** |
| 100 mm cube @ 0.2 mm, 500 layers | (not measured) | **0** |

Every case is watertight. The construction guarantees it rather than repairing towards it: no
diff, no tolerance, and the caps and walls of a layer share their vertices by exact equality.

### The owner's scenario (b): the texture transfer, and the metric that can now see it

Source at 0.3 mm with fuzzy skin: wall deviation mean 0.138, **max 0.350 mm** — i.e. the fuzz
amplitude T = 0.3, as it should be. Bake: 67 layers, 25,220 triangles, watertight. Re-sliced at
**0.12 mm with fuzzy skin OFF** (168 layers): the re-sliced outer walls still deviate by **max
0.481 mm, mean per-layer max 0.379 mm**. The coarse fuzzy texture is carried by the mesh and
reproduced by the fine layers, which is what the feature is for.

Two test metrics were rewritten to make that measurable, because the first draft's were not:

**1. `wall_deviation` now measures against the NOMINAL wall plane, not the bounding box.** The
first draft took the wall's own bounding box and measured each point's distance to the nearer
side. That puts the reference plane at the *outermost* fuzz excursion, +T, so an inward excursion
at -T reads as 2T away from it, and the measured max came out at 0.630 for a 0.3 mm fuzz. The
plane is now the **median** of the points assigned to that side, which sits on the un-fuzzed wall
line where the geometry actually is. Points within 1 mm of a corner are dropped as ambiguous.

With that, the bound is **derived rather than fitted**:

- the classic fuzzy noise is uniform on [-1, +1] times `fuzzy_skin_thickness`, so the wall
  centreline moves at most **T = 0.3 mm** either side of the nominal plane;
- the bake offsets that centreline outward by the path's own `width/2` — a **constant**, which
  shifts the wall without changing its amplitude;
- the re-slice lays its own external perimeter half a line width inside the baked surface —
  again a constant;
- both constants are absorbed by measuring against the median plane, so what is left is T plus
  at most **w/2 = 0.21 mm** of slack for where the re-slice's centreline lands on a corner or on
  a step between two baked bands.

      max deviation <= T + w/2 = 0.3 + 0.21 = 0.51 mm      measured 0.481  ✓

**2. `radial_profile` → `band_profile`: deviation from the local wall plane, parameterised along
the perimeter.** The first draft compared adjacent layers by centroid-to-wall radius. On a square
cross-section that radius swings by about ±2 mm between the middle of a side and a corner —
seven times the 0.3 mm signal — so the band structure was invisible: in-band 0.546 vs cross-band
0.554, indistinguishable. Profiling the **deviation from the local wall plane** against
**position along the perimeter** removes the geometric term entirely (an unfuzzed square profiles
as all zeros), and the bands appear immediately:

| | first draft (radial) | now (band_profile) |
|---|---|---|
| within a 0.3 mm band | 0.546 mm | **0.063 mm** (77 pairs) |
| across a band boundary | 0.554 mm | **0.205 mm** (50 pairs) |
| ratio | 1.01 | **3.25** |

That ratio is the assertion that matters: it is what proves the texture came from the source's
0.3 mm layers rather than from noise the re-slice invented. The absolute in-band bound is a
resampling term, not a geometric one — 240 bins over an ~80 mm perimeter is a 0.33 mm bin, and
the fuzz is sampled every 0.8 mm with amplitude T, so two independent samplings of one identical
curve differ by a fraction of T within a bin. It is bounded at 0.5 T for that reason.

### Timing: 100 mm at 0.2 mm

`slice bake: a 100 mm part at 0.2 mm bakes in reasonable time`, a 100 mm cube, 500 layers:

    500 layers, 6,000 triangles, 2,004 vertices, bake 0.0023 s, open edges 0

The test reports these rather than asserting them (both are machine-dependent); the only bound is
a generous 60 s ceiling that would catch an accidental quadratic, plus `open edges == 0` and the
layer count. 2.3 ms for 500 layers is well inside it — the prism loft is a linear pass, and it is
in fact **an order of magnitude faster than the old loft** (0.019 s for the 20 mm cube versus
0.300 s), because it does no Clipper diff per layer at all.

A 100 mm cube is a prism, so its per-layer contour is 4 points and the triangle count is small;
the cost that matters on a real part is the per-layer Clipper union in
`slice_bake_layer_regions`, which the fuzzy cases exercise (67 layers, 25,220 triangles, 0.156 s
for a bake *and two full slices*).

## Click-tests

Still to be performed by the owner — the library is now watertight on every case, so the GUI can
be exercised against a bake that is worth looking at. `libslic3r_gui` builds clean with the
dialog, job, menu item and `ObjectList` changes in it. The five-step check:

1. Load a 20 mm cube, set **fuzzy skin = external**, thickness **0.3 mm**, layer height
   **0.3 mm**, and slice the plate.
2. Right-click the object → **Bake slice to mesh…**. The dialog's size line should read about
   67 layers; leave "Close small gaps" off and pick **Replace object**.
3. When the job finishes the object is the baked mesh — visibly stepped, carrying the fuzz — and
   the plate shows its slice as stale.
4. Set **fuzzy skin = none** and layer height **0.12 mm**, and re-slice. The preview's walls
   should still carry the coarse 0.3 mm texture, now in 0.12 mm layers with no jitter of the
   slicer's own.
5. **Ctrl+Z once** puts the original cube back, with its own fuzzy settings intact.

## Phase 1a: the owner's three click-test defects (2026-09-13)

Three defects came back from the owner's first click-test. All three are fixed on
`fix/slice-bake-transform`; the library gained an options enum, two controls and a new cap
construction, and the test set went from 8 cases to 14.

### 1. "Add as new object" came out the wrong size and in the wrong place

**Cause.** The bake is built from `PrintObject` layers, which live in PRINT space: the instance's
rotation and scale are already applied to the vertices. The old code then undid the instance
transform (`in_object_frame`) and handed the result to `ObjectList::load_mesh_object`, which
creates a ModelObject with an IDENTITY instance and drops it in the nearest empty cell. So the
rotation and scale came out of the vertices and nothing put them back: a 2x-scaled part baked to a
1x object, standing somewhere else.

**Fix.** `SliceBakeOptions::in_object_frame` (a bool) became `SliceBakeFrame` (an enum) with three
frames, and the job picks one from the result mode:

| Route | Frame | Vertices carry | Instance carries |
|---|---|---|---|
| Replace object | `Object` | neither rotation nor scale | the object's existing instance re-applies both |
| Add as new object | `World` | rotation and scale | offset only (identity rotation, identity scale) |
| Export STL | `World` | rotation and scale | nothing — a file has no instance |

The `World` maths, with `P` a point in print space, `S = shift_without_plate_offset()` and `C =
center_offset()`:

```
world                    W          = P + S
instance offset          offset_xy  = unscaled(S - C)          (set_instances adds C into shift)
mesh, so it sits at 0    P'         = P + (S - offset_xy) = P + unscaled(C)
```

`SliceBakeReport::instance_offset` returns `offset_xy`, and the job builds the ModelObject by hand
rather than through `load_mesh_object` — which would recentre and relocate it. No `ensure_on_bed()`:
the mesh's Z is already the printed Z.

**Gate.** *a scaled, rotated, offset instance bakes to the same world box* — a 10 mm cube at 2x, 30
degrees, offset (30, -20). The baked object's world bbox matches the sliced object's within one
layer height on all six faces, in both frames.

### 2. The bake looked more angular than the slice preview

**Cause.** By the time an outer wall reaches `LayerRegion::perimeters` it has been simplified at the
print's `resolution`. The preview draws the un-simplified path.

**Fix.** Three things:

* **A contour source.** `SliceBakeContourSource::SliceContours` (the new default) takes the boundary
  from `LayerRegion::slices` — the un-simplified contour the slicer cut from the mesh, which IS the
  printed outer boundary (the wall centreline sits half an external width inside it, and the bake
  offsets it back out by the same half width, so the round trip is a no-op and is skipped rather
  than performed through Clipper, which would round every convex corner). `Extrusion` keeps the old
  source, and is the only one that carries **fuzzy skin** — displacement applied to the path after
  the slice, which the contour has no trace of.
* **A Resolution control** (mm, default = the print's own `resolution`, min 0.01, max 1.0) that
  densifies a segment when the arc it stands in for departs from it by more than the tolerance.
  Sagitta `~ L * theta / 8` over the segment's own turn, so a STRAIGHT run is never subdivided
  however long — that is the difference between adding points that carry shape and adding tens of
  thousands that carry none.
* **A live triangle count** in the dialog: the counter is passed in as a callable and re-run on
  every control change, because the source, the resolution and the smoothing toggle all move it.

**Measured** on a 10 mm cylinder sliced at `resolution = 0.1`: maximum radial error **0.259 mm from
the extrusion, 0.0396 mm from the slice contour** — 6.5x rounder. On that contour the densifier
correctly adds nothing (its chords are already ten times finer than the finest tolerance offered);
on the extrusion source it adds points and the radial error falls further.

**And a "Smooth vertical steps" toggle** (off by default): consecutive layers are joined by a sloped
quad ribbon instead of stacked vertical prisms, wherever their rings correspond (same count, matched
by winding and centroid, resampled to a common point count). A pair that does not correspond falls
back to that interval's own prism. Each interval is a solid closed ON ITS OWN, which is what keeps
the stack watertight — the same argument the prism stack rests on. Measured on an 8 mm sphere: the
stepped bake's walls are **>99% exactly vertical**, the lofted bake's **<25%**, with 79 of 80 bands
lofted and 0 open edges.

*Not done, and why:* letting two consecutive ribbons share their boundary and skip the cap pair
there would remove the internal horizontal faces. It cannot be done as the ribbons are resampled —
interval `i`'s upper ring and `i+1`'s lower ring get different point counts whenever the layers
differ — and forcing a match opened a sphere by 26,175 edges and a bored block by 4,740. The cap
pairs are interior, coplanar and opposite; nothing downstream minds them.

### 3. Stray straight slivers across the hull of a baked Benchy

**Cause — none of the three that were suspected.** Not the extrusion centreline self-intersecting,
not the gap closer bridging two loops, and not a misread orientation. It was the cap construction
itself, in two ways, both traced with a throwaway probe rather than guessed at:

* The caps were triangulated by the **GLU tesselator**, which emits bare XY coordinates, and matched
  back to the wall vertices through a `std::map` keyed on the unscaled XY. That map is only
  injective if the layer visits every XY at most once. A self-touching contour — two sharp concave
  features whose offsets meet, which a Benchy's deck cutout and bow are full of — visits one twice,
  and `emplace` kept the FIRST entry, so the second visit aliased onto it and the cap triangle was
  stitched across the pinch.
* Worse, and independent of any aliasing: on a plate of five 0.55 mm slots, GLU emitted **four
  triangles per cap that span a slot** — surface covering a void — on input with a CCW contour, five
  CW holes, no repeated coordinate at all and no vertex off the contour. That is GLU getting a valid
  polygon-with-holes wrong, and no amount of cleaning the input fixes it.

**Fix.** The cap construction is now `Slic3r::Triangulation::triangulate(const ExPolygons &)`, a
constrained Delaunay triangulation that returns **indices into the polygon's own point list** rather
than coordinates. Two consequences: a triangle can never cross a constraint edge, so a void cannot be
covered; and there is nothing to match back, because `to_points()`' order (contour, then holes, per
ExPolygon) is exactly the order the vertex runs are laid down in — so an index maps to a vertex by
adding the run's base. No table, nothing to alias, and GLU's combine vertices cannot arise.

Alongside it: the per-layer union runs with Clipper's **StrictlySimple** flag
(`simplify_polygons_ex`), which splits a self-touching path into simple ones, and `unpinch_slice()`
moves any XY that still collides by ~60 nm — `Triangulation`'s own header says it does not handle
duplicate coordinates, and its fallback path merges them, which would weld the two sides of a pinch
in the cap while the walls keep them apart.

**Measured**, cap by cap over all 1002 layers of the owner's `bake_replace.3mf`:

| Cap triangulation | long-thin cap triangles | spanning a void |
|---|---|---|
| GLU (before) | 158,448 | 0 on this file; **4 per cap** on the slotted plate |
| CDT (after)  | **27,369** | **0** on both |

— a factor of **5.8** on the Benchy, and the void-spanning class gone entirely. On the synthetic
slotted plate the final mesh went from **1,880 blades (160 with visible area, worst 17 mm long and
1.19 mm²)** to **zero**, watertight throughout.

**What was tried and rejected:** rejecting needle triangles in the cap. A zero-area cap triangle
still carries three edges of the triangulation's edge graph, each shared with a neighbour that keeps
it, so dropping it leaves those edges with one incident face. A plain 20 mm cube went from 0 open
edges to 40, and a 100 mm one to 84. The sliver has to be prevented, not deleted.

### Test set

14 cases, all green (11,886 assertions). The 8 original cases are unchanged except where the
geometry source legitimately moved: the three FUZZ cases (`fuzzy skin survives a re-slice`, `the same
slice bakes to the same mesh twice`, `closing the gaps`) now pass `SliceBakeContourSource::Extrusion`
explicitly, because fuzz lives on the extrusion path and the new default is the slice contour. Six
new cases cover the transform (both frames), the contour source and the resolution, the smoothing
toggle and its fallback, the synthetic pinch, and the owner's Benchy (tagged `[.benchy]`, reading the
file from its own path — 5 MB is too much to add to a 2.2 MB test corpus).

## Phase 1b: the extrusion-source leak (2026-09-13)

The owner made the extrusion paths the only source the GUI uses (e995c5cd06 - the slice contour is
the original model without the fuzzy skin, so it is of little use for baking). With that default the
hidden Benchy case failed: **5,698 open edges** and 104.9 slivers per layer, while the same file on
the slice-contour source was clean. Fixed on `fix/slice-bake-extrusion`; `[.benchy]` now runs both
sources and both come back at **0 open edges**, stable over repeated runs.

### Why only the extrusion source

Every cause below is a near-degeneracy that a fuzzed wall's outward offset produces and a slice
contour does not. The slice contour's rings are far apart, its points are not densified, and it
never needed a nudge - which is exactly why it never leaked and why the asymmetry was the clue each
time.

### Three causes, in the order they were found

**1. `unpinch_slice` gave up.** It tried eight nudges along one direction and then left the
duplicate XY in place. That is not cosmetic: `Triangulation::triangulate` silently switches path when
its point list has a duplicate - it COLLAPSES the coincident points and returns indices into the
collapsed list, with `uint32_t::max()` in every slot its reverse map never reaches - while the wall
vertex runs were built over the uncollapsed list. Out-of-range indices were dropped (open edges) and
in-range ones stitched to the wrong vertex (slivers). Only the extrusion source ever needed more
than eight steps: its loops run within a fraction of a millimetre of each other and the resolution
densifier then inserts interpolated points on both that round to the same lattice square. Now a
spiral search that always finds a free point, with `pinch_points_unresolved` reporting any that
cannot be placed (zero on the Benchy). **4,601 → 130 open edges.**

**2. The cap was triangulated a whole slice at a time.** `Triangulation`'s inside/outside filter
cannot tell the void BETWEEN two disjoint islands from the material inside one - CGAL triangulates
the convex hull of everything it is handed and the filter keeps the gaps. Measured on layer 570 (five
islands): the cap came back with 3,117 mm² of triangles over a polygon of 387 mm², a ratio of
**eight**. Now one island at a time, with the index base a running sum of each island's own point
count, and the two-argument overload so the duplicate-collapsing path can never be taken.

**3. The nudge broke island disjointness.** The open edges were never holes - dumping one showed
edges used **three** times, a non-manifold junction where two islands' caps covered the same ground.
`unpinch_slice` moves a colliding point ~60 nm, and where two islands already run within that of each
other the nudge pushes one across the other. Measured: one layer in a thousand had two islands
intersecting by **9.3e-6 mm²** - a few square microns - and that was enough for ~600 unbalanced
edges. The layer now iterates `union_ex` (disjointness, exact on the lattice) and `unpinch_slice`
(uniqueness) to a fixed point, union first so uniqueness is the invariant that survives.

The densifier keeps working on the extrusion source throughout - that is where it has work to do,
the contours being the simplified ones.

### A dead end worth recording

Two attempts to *recover* in the cap rather than fix the geometry made things worse and were backed
out. Rejecting needle triangles opened a plain 20 mm cube by 40 edges: a zero-area cap triangle still
owns three edges of the edge graph, each shared with a neighbour that keeps it. Filling an island's
holes when its triangulation failed gave **1,894 open edges out of 16 dropped triangles**, because
the walls had already been built from the unreduced island - a cap and its walls must come from the
same geometry. And a "sanitiser" that probed each island in advance certified nothing, because
`CGAL::spatial_sort` shuffles with a random seed: `triangulate()` is not a function of its input, and
the open-edge count wandered between runs on identical geometry.

### What the Benchy test asserts now

Watertightness on both sources, no dropped cap triangles, and every invariant counter zero - all
exact, all direct, all failing before this change. The sliver COUNT is reported but no longer
asserted: the metric does not discriminate. On this file the slice-contour source, which is the clean
yardstick, scores 43 "blades" per layer against the extrusion source's 25. A Benchy hull is a long
thin outline at every height, every triangulation of one is full of long thin triangles, and no
shape-and-size rule separates those from an artefact without also knowing whether the triangle lies
inside the material - a measurement this test could not make reliably (a nearest-Z match attributed
triangles to the wrong layer and produced 72 mm triangles on a 60 mm model). The visual artefact is
addressed by construction instead: a constrained Delaunay whose constraints are the island's own
edges cannot emit a triangle that crosses one.

## Not in phase 1

Per the spec: the G-code route (Z contouring, seams), Smooth mode (OpenVDB level set), and
colour/paint carry-over. Also deferred: the layer-range picker in the dialog (the library takes
the subset; the dialog always passes "all").
