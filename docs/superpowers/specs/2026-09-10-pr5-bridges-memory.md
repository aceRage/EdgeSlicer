# PR #5 internal bridges: the flaky "Coordinate outside allowed range"

Branch `fix/pr5-bridges-memory`, off `feat/ultra-preferences`, with
`origin/cursor/fix-internal-bridges-hilbert-octagram-4b7b` (PR #5, "Fix internal bridges over
Hilbert Curve / Octagram Spiral sparse infill", port of Orca #15206) merged in.

## Cause

`Print::m_origin` (`src/libslic3r/Print.hpp`) was declared `Vec3d m_origin;` with no initialiser,
and Eigen does not value-initialise a `Vec3d` member, so a `Print` that never gets
`set_plate_origin()` - the test harness and the CLI, as opposed to the GUI - reads uninitialised
memory for the plate origin. `layered_print_cleareance_valid()` computes the prime-tower position
as `wipe_tower_x.get_at(plate_index) + plate_origin(0)` into a `float`; with garbage in `m_origin`
that became `-inf`, `scale_()` of which casts to `INT64_MIN` on x86-64, and translating the
prime-tower convex hull by that point produced coordinates of `INT64_MIN + <the real hull corner>`
which `ClipperLib::RangeTest` rejected inside `Print::validate()`.

PR #5 did not introduce the defect and its bridge code never threw. What PR #5 changed was the
*stack and heap layout* of the test binary, so the garbage that `m_origin` happened to alias
changed from benign to `-inf` - which is why the failure looked like a memory error tied to the new
turning-pattern branch, appeared and disappeared with `--order rand` seeds, and vanished when the
test wrapped `print.process()` in a try/catch.

## Instrumented evidence

MSVC AddressSanitizer could not be used on this box: the installed toolset (14.44.35207) ships no
`clang_rt.asan*` libraries at all, and linking a 14.44-compiled binary against the newest available
runtime (14.34) gave `STATUS_DLL_INIT_FAILED` (0xC0000142) at startup. Instead the reproducer was
built RelWithDebInfo, where it fails **100% of the time** rather than intermittently, and
`ClipperLib::RangeTest` in `deps_src/clipper/clipper.cpp` was instrumented to dump the offending
point and a `CaptureStackBackTrace` symbolised stack.

```
PR5HUNT-RANGE: x=-9223372036791576250(0x8000000003c45946) y=-9223372036831576250(0x800000000161ff46)
  #01 Slic3r::ClipperLib::RangeTest            (deps_src/clipper/clipper.cpp:639)
  #03 Slic3r::ClipperLib::ClipperBase::AddPathInternal (deps_src/clipper/clipper.cpp:833)
  #09 Slic3r::intersection                     (src/libslic3r/ClipperUtils.cpp:690)
  #10 Slic3r::layered_print_cleareance_valid   (src/libslic3r/Print.cpp:1612)
  #11 Slic3r::Print::validate                  (src/libslic3r/Print.cpp:1730)
  #12 Slic3r::Test::init_print                 (tests/fff_print/test_data.cpp:219)
  #14 ____C_A_T_C_H____T_E_S_T____22           (tests/fff_print/test_printobject.cpp:139)
```

Two things fall out of that stack. The throw is in `Print::validate()`, reached from the test's
`init_print()` helper - **before `print.process()` is ever called**, so no slicing code and none of
PR #5's code is on the stack. And the bit pattern decodes exactly:

```
0x8000000003c45946 - INT64_MIN = 63199558 = 63.199558 mm
0x800000000161ff46 - INT64_MIN = 23199558 = 23.199558 mm
```

which is the prime-tower hull corner at `(width + brim, depth + brim)` = `(60 + 3.199558,
20 + 3.199558)` mm, translated by a point whose components are `INT64_MIN`. The "valid low word with
a garbage `0x80000000` high word" was therefore never a truncated 64-bit value - it is
`INT64_MIN + a perfectly valid coordinate`, the signature of `static_cast<coord_t>` of a
non-finite `double`.

Confirmed directly by tracing the inputs at `Print.cpp:1600`:

```
PR5HUNT-WT: width=60.000000 depth=20.000000 brim=3.199558 height=-107374176.000000 x=-inf y=-inf a=0.000000 filaments=1
```

`x` and `y` are `-inf` and `height` is nonsense, all three sourced from the uninitialised
`m_origin` / an unset plate. Earlier instrumentation ruled out the suspects the brief listed: the
`std::upper_bound` candidate range in `construct_anchored_polygon` is never inverted and never
dereferenced out of bounds (0 hits), no out-of-range point ever leaves `construct_anchored_polygon`
into `union_safety_offset` (0 hits), `n_vlines` and the scan bounding box are always sane, and
try/catch wrappers around `construct_anchored_polygon`, `bridge_over_infill`, `clip_fill_surfaces`
and `combine_infill` never fired.

## Fix

One line, in `src/libslic3r/Print.hpp`:

```cpp
Vec3d   m_origin { Vec3d::Zero() };
```

`(0, 0, 0)` is what `set_plate_origin()` is given for the single-plate case and what every non-GUI
caller assumed it already held. `get_plate_origin()` has ten callers - `Brim.cpp`,
`GCode/PrintExtents.cpp`, `GCode.cpp`, and five sites in `Print.cpp` - all of which were reading
the same garbage. PR #5's behaviour is untouched; no file it added or edited was modified.

A scan of the surrounding classes for the same hazard (an Eigen member with no initialiser) found
only `PrintObject::m_size`, which is assigned unconditionally in the `PrintObject` constructor
(`PrintObject.cpp:107`) and so is safe.

## Proofs

All on the merged branch with the fix applied.

- The pair `fff_print_tests "[PrintObject][InternalBridge][Regression]"`: **20/20 pass**
  (RelWithDebInfo, where it previously failed 20/20).
- Shuffle seeds `--order rand --rng-seed {7, 42, 90210, 1, 1337}`: **5/5 pass**, including the
  three seeds (7, 42, 90210) recorded as failing.
- The full `fff_print_tests` binary in **Release**: **10/10 runs green**, 90 test cases and 3741
  assertions each, with **zero** "Coordinate outside allowed range" in any run.

## Tree support

Yes - same cause, cured by the same fix.

`test_support_material.cpp:893`, "SupportMaterial: classic tree says its interface layer count is
object-wide", was tagged `[!mayfail]` for throwing ClipperLib "Coordinate outside allowed range"
from `TreeSupport3D.cpp:73` on roughly one full-suite run in four, never in isolation. That guard's
limit is `65536 * 16384` = 1073741824, the ~1073 mm the old note observed. The failure was
full-suite-only and nondeterministic for the same reason as PR #5's: the garbage aliased by
`m_origin` depends on what ran before it.

Across the 10 full-suite Release runs above the test executed every time and never produced a range
error. The `[!mayfail]` tag has been left in place - removing it is a separate, deliberate change
and this branch should stay minimal - but the comment above it can now be resolved.
