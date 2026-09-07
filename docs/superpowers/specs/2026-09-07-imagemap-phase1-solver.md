# ImageMap Phase 1: the colour solver, fork-native

Date: 2026-09-07
Branch: `feat/imagemap-p1-solver`, from `origin/feat/ultra-preferences` @ `302d19f34d`
Worktree: `C:\Dev\SnapmakerOrcaPhone\.claude\worktrees\imagemap-p1`

Implements Phase 1 of `docs/superpowers/specs/2026-09-07-imagemap-edgeslicer-plan.md`
(on `feat/imagemap-plan`). Nothing from `feat/imagemap-p1-libs` is merged; that branch was
read as a reference and the solver was re-vendored from it by hand.

---

## 1. What shipped

| File | Lines | What |
| --- | --- | --- |
| `deps_src/colorsolver/ColorSolver.hpp` | 192 | trimmed public surface |
| `deps_src/colorsolver/ColorSolver.cpp` | 842 | the candidate / kd-tree / Oklab core |
| `deps_src/colorsolver/NOTICE` | 62 | upstream attribution + what this copy changed |
| `deps_src/colorsolver/CMakeLists.txt` | 14 | static lib, no dependencies |
| `deps_src/CMakeLists.txt` | +1 | `add_subdirectory(colorsolver)` |
| `src/libslic3r/CMakeLists.txt` | +4 | `colorsolver` linked PUBLIC |
| `src/slic3r/GUI/MixedColorMatchHelpers.cpp` | +387/-176 | the recipe search, rewritten |
| `tests/libslic3r/test_color_mix_solver.cpp` | 407 | new tests |
| `tests/libslic3r/CMakeLists.txt` | +1 | the new test |
| `README.md` | +1 | third-party list |

Against the plan's guidance of "~700 kept + ~200 new + ~150 GUI": the vendored core is
1034 lines (header + cpp, comments included) of which roughly 640 are upstream's kept code
and about 150 are new (`ColorSolverConstraints` and its enumeration, the DP candidate
counter, `solve_color_solver_top_component_sets`); the tests are 407 rather than 200, of
which about 90 is a transcribed dE2000 that has nowhere else to live; the GUI change is
+387/-176 on one file, ~150 lines of it new logic and the rest comment and reflow. Nothing
else was touched: **no new `PrintConfig` key, no new `ObjectBase` member, no `.gitattributes`
change, no CI change, no LFS object.**

## 2. What was kept from the reference, and what changed

The reference is `deps_src/colorsolver/ColorSolver.{cpp,hpp}` on `origin/feat/imagemap-p1-libs`
(the rebased Cursor PR 10), itself vendored from `sentientstardust-dev/OrcaSlicer-ImageMap`
@ `92548381056dbf72836b0a1bdc455f238218dbfb`, AGPL-3.0, "Copyright (C) 2026 sentientstardust".
Upstream `ColorSolver.cpp` is **2024 lines**; this copy is **842**.

### Kept, essentially verbatim

- the sRGB ↔ linear and sRGB ↔ Oklab conversions;
- the chroma-dependent Oklab axis weights, `OklabSoftCap4Dark4`'s dark penalty, and the
  `ColorSolverMode` / `ColorSolverLookupMode` enums;
- the candidate enumeration over a fixed unit budget
  (`color_solver_total_units_for_component_count` — 40 units for ≤ 4 components — and
  `color_solver_candidate_count`) and its string cache key;
- **both kd-trees** (linear sRGB and Oklab), their construction and their branch-and-bound
  queries, including the best/second-best bookkeeping `BlendClosestTwo` needs;
- `solve_color_solver_weights_for_target`.

### Changed

- **`ColorSolverMixModel` is deleted**, and with it every reference to `pigment-painter`
  (GPL-3.0 + a 36.3 MiB generated LUT) and `prusa-fdm-mixer`. Neither library is vendored.
  `mix_color_solver_components` now calls the fork's own header-only
  `src/libslic3r/filament_mixer_model.h` (MIT, Justin Hayes), chained pairwise with an
  accumulating weight — a transcription of `MixedFilamentManager::blend_color_multi` and of
  `blend_multi_filament_mixer`. One mix model, so the solver's prediction, the preview
  swatch and the sidebar chip are the same number rather than three that can disagree by
  ΔE 20 (the plan's M4).
- **`ColorSolverConstraints`** is new: minimum and maximum share per used component, a floor
  and a cap on how many components may be non-zero, and a pairwise `allowed_pairs` matrix.
  The batch dialog's existing per-component percentage limits and its filament-category
  compatibility matrix go straight in, so an unreachable or unmixable combination is never
  enumerated instead of being filtered out of an answer afterwards.
- **A candidate budget.** Enumeration cost is one polynomial mix per candidate, so the set
  size is a real time and memory bound. `count_constrained_candidates` counts the set by DP
  before building it, and the unit lattice drops by halves until the count is under 200 000.
- **`solve_color_solver_top_component_sets`** is new (§4 says why): the best N *component
  sets* for a target, best first, as an exact linear pass keeping the best error per set.

### Dropped entirely

The ordered-stack / contoning solver and `ColorSolverCalibratedStackModel` — the
Beer-Lambert, TD-effective-alpha, adaptive-spectral and nearest-measured-sample stack
models, the stack stretching and beam search, `mix_color_solver_ordered_stack`,
`ColorSolverStackComponentRole`. The plan shelves these for Phase 4; they are recoverable
from `origin/feat/imagemap-p1-libs`, which stays in place as the reference.

## 3. The GUI change

The wiring point is `build_best_color_match_recipe` in
`src/slic3r/GUI/MixedColorMatchHelpers.cpp`, which is what the batch colour-match dialog
(`MixedFilamentBatchDialog` → `batch_match_model_colors`) and the single-row match panel
both call. **Correction to the plan:** that function was *not* a CIE76 nearest-colour
matcher. It already searched mixes and already scored in ΔE2000. The CIE76 nearest-colour
matcher the plan refers to is `libslic3r/ObjColorMatch.hpp`'s `obj_color_distance`, which is
the *import* path (glTF/OBJ), and that is Phase 2's business, not Phase 1's. So what
follows is a comparison against the real incumbent, which is a much harder bar than CIE76.

### What it was

1. every **pair**, coarse at 5%, keep the best 30, refine those at 1%;
2. if the best pair was still worse than ΔE 0.5, score **triples** drawn from the
   **top 8 filaments by single-colour ΔE**, coarse at 10%, keep the best 20, refine those;
3. prefer the pair when the triple's advantage is under 0.5 ΔE.

The weak step is 2. The component set was chosen by how close each filament sits to the
target *on its own*, which is the wrong question: the three filaments that make the best
mix are often not the three that are individually nearest. A good brown wants yellow, and
yellow is nowhere near brown.

### What it is

1. the solver enumerates every reachable mix of the selected filaments — honouring the
   dialog's min/max component percentages and its compatibility matrix — and returns the
   best **8 pair sets**, best first, measured in Oklab;
2. each is swept at 1% and scored in ΔE2000; early return if the best is within ΔE 0.5;
3. the solver returns the best **12 triple sets**, each swept at 1% and scored in ΔE2000;
4. the same pair preference, unchanged.

Everything downstream is untouched: the same `MixedColorMatchRecipeResult`, the same pair /
gradient encoding, the same `preview_color` pipeline, so `populate_mixed_filaments_from_mappings`,
`add_batch_custom_filaments`, the sidebar chips and the 3MF round trip see no difference
beyond better numbers.

Two deliberate limits, both recorded in the code:

- **Two components minimum.** `batch_match_model_colors` reads `mix_b_percent == 0` as
  "this is a pure filament", and a single-component answer would be dropped on the way to
  `MixedFilamentManager`. The old search could not return one either.
- **Three components maximum.** `MixedFilamentDialog`'s MODE_MATCH editor is a three-corner
  picker and silently truncates a longer row when a user opens and saves it
  (`MixedFilamentDialog.cpp:3316-3341`). Storage, display colour, `mixed_filament_definitions`
  and `MixedFilamentManager::resolve` all handle N components — every gate there is
  `>= 3`, never `== 3` — so this is a UI limit and lifting it is a separate change.

## 4. Measured: old search vs new

`scratchpad/matcher_spike.cpp` runs both searches side by side outside wxWidgets, using the
fork's own mixer, `RGB2Lab` and `DeltaE00`, so the numbers are the numbers the dialog
reports. 18 targets against three palettes.

| Palette | old mean ΔE2000 | new mean ΔE2000 | nearest single filament (CIE76 pick) | changed | better | worse |
| --- | --- | --- | --- | --- | --- | --- |
| CMYW, 4 slots | 3.722 | **3.714** | 24.286 | 2/18 | 2 | **0** |
| 8 slots | 0.767 | **0.709** | 16.994 | 3/18 | 3 | **0** |
| 12 slots | 0.574 | **0.471** | 13.330 | 8/18 | 8 | **0** |

Never worse on any of the 54 cases; better on 13. Best single gain +0.65 ΔE2000
(`#CDDC39` on 12 slots, `F3/F4/F12 49/15/36` → `F3/F4/F5 55/31/14`, ΔE 0.86 → 0.22) and
`#607D8B` on 12 slots reaches ΔE 0.00 where the old search got 0.41. The gain grows with
slot count, exactly as the diagnosis predicts: at 4 filaments there are only 4 triples and
the old top-8 pool already covered all of them, so the two searches mostly agree; at 12
there are 220 and the pool saw at most 56.

The last column is the plan's stated bar in its own terms — a nearest-single-filament CIE76
matcher, which is what the import side does. Any mix search beats it by 13-24 ΔE2000; that
comparison is not close and was never the interesting one.

### Cost

Two dead ends are worth recording, because both look reasonable and both are wrong:

- **One component set instead of several.** Taking only the solver's single best answer and
  refining that made the search *worse than the old one* on 9 of 18 targets at 12 slots
  (mean 0.574 → 0.653) and on 3 of 18 at 4 slots. The solver measures on its unit lattice
  (2.5% steps) in Oklab; the caller then sweeps whole percentages in ΔE2000. Those two
  rankings do not agree at the top, so the lattice winner is often not the set whose best
  whole-percent ratio wins. Hence `solve_color_solver_top_component_sets`.
- **A coarse sweep over those sets.** Replacing the 1% sweep with 5%-coarse-plus-fine is 5×
  faster and regresses: 4 slots 3.722 → 3.753 (3 worse), 8 slots 4 worse. 3%-coarse is no
  better. The full 1% sweep is what makes the result never-worse.

With the shipped numbers (8 pair sets, 12 triple sets, 1% sweep) the whole spike — 54
matches plus six candidate-set builds — runs in **16.4 s**, about **0.30 s per model
colour** including the one-off builds. That is much slower than the old search (single-digit
milliseconds) and it is the real cost of this change: a 64-colour batch goes from
instantaneous to roughly 20 seconds. It runs on the existing worker thread, behind the
existing progress callback and cancel token, and the candidate sets are cached per palette
and constraint set so a batch pays for the enumeration once.

### The solver on its own

The `#0086D6` / `#E6007E` / `#FFE800` / `#FFFFFF` set enumerates **12 341** candidates at 40
units. Straight lattice answers, no 1% refinement (this is what the unit tests assert):

| target | closest reachable mix, ΔE2000 | nearest single filament, ΔE2000 |
| --- | --- | --- |
| `#7A5C3F` | 8.898 | 47.612 |
| `#4CAF50` | 0.308 | 32.618 |
| `#FF7043` | 2.043 | 31.808 |
| `#B0BEC5` | 4.217 | 16.466 |
| `#8E24AA` | 3.128 | 21.885 |
| `#00897B` | 3.562 | 29.613 |
| **mean** | **3.693** | **30.000** |

## 5. Tests

`tests/libslic3r/test_color_mix_solver.cpp`, six `TEST_CASE`s:

1. **endpoint exactness** — `mix_color_solver_components` with one non-zero weight returns
   that filament's colour bit-for-bit (`==` on floats, not `Approx`), for int and float
   weights; and asking the solver for a filament's own colour returns weight 1.0 on it,
   0.0 on every other, with the winning candidate's colour equal to the filament's and
   ΔE2000 exactly 0.
2. **determinism** — two independent builds produce identical `rgbs` and `weights` vectors;
   the same query run three times over three modes and both lookup modes gives identical
   weight vectors; the cache returns the same object, not an equal rebuild.
3. **monotonicity** — walking a 21-step ramp between two filaments, the solved share of the
   second filament never decreases, the weights always sum to 1, and each answer lands
   within ΔE2000 1.0 of the ramp point it was asked for.
4. **golden cases** — four targets with per-case ΔE2000 bounds set just above the measured
   values (8.898 / 0.308 / 2.043 / 4.217), plus the invariant that the weights are
   non-negative and sum to 1.
5. **a mix beats the nearest single filament** — six targets, each strictly closer in
   ΔE2000 than the CIE76 nearest-single answer.
6. **constraints** — a minimum share excludes every undershooting mix; a component cap is
   respected and pure filaments still survive it; a two-component floor makes every recipe a
   real mix; an incompatible pair is never enumerated.

Plus one that pins the mix model: a 50/50 two-component mix equals `Slic3r::filament_mixer_lerp`
on the same inputs, so the solver and the swatch cannot drift apart.

`tests/libslic3r/test_mixed_filament_color_golden.cpp` is untouched and still green — the
display-colour path did not move.

## 6. Acceptance

See the session report for the run-by-run results. In summary:

**Tests.** `libslic3r_tests` passes **628 cases / 72 344 assertions, 626 passed, 2 failed
as expected** — the two known failures in `test_mixed_filament.cpp`. The head is 621 cases
with the same two, so the seven new cases are the seven added here.
`test_mixed_filament_color_golden.cpp` is green.

**Bar A.** A CLI slice of `resources/handy_models/OrcaToleranceTest.stl` on
"Bambu Lab P1S 0.4 nozzle" / `0.20mm Standard @BBL X1C` / `Generic PLA`, isolated
`--datadir` copied from `snorca_hubtest/dd_lan`, two passes per side, baseline installed
from the head build at `302d19f34d`. **N = 0 new config keys**, so the bar is literal byte
identity apart from the `; generated by` line:

```
base sha256 (timestamp line dropped): 18f37a944317a99395770e6bf29f20f22c474601549329fdc53dc24dacb81ae7
cand sha256 (timestamp line dropped): 18f37a944317a99395770e6bf29f20f22c474601549329fdc53dc24dacb81ae7
RESULT: byte-identical apart from the timestamp line (24559 lines).
        model/unique label ids unchanged: yes
```

`; model label id: 15` on both sides, and all 129 label/`M624`/`M625` lines identical.
15 is the head's value; PR 10 was measured shifting it to 23, so this is the automated
proof that no `ObjectBase`-derived member was added. Script: `scratchpad/bar_a_imgp1.py`.

Incidentally: both the model and the presets are self-reproducible on this build (pass 0 and
pass 1 hash identically on both sides), which the corpus note in
`tests/data/support_corpus/corpus.json` says they were not. That note predates the
determinism work in `eec7c0538f` and is stale for this model.

**GUI.** There is no automation that can open the batch colour-match dialog and read its
suggestions — `snorca_hubtest/run_control_app.py` only starts the app hidden on an isolated
data dir, and the phone-control API covers slicing and sending, not dialogs. So the fallback
was taken: the numeric proof is §4's side-by-side of the old and new searches on the same
inputs, plus the unit tests. What the hidden instance did confirm is that the changed GUI
binary starts and stays up on a copy of `dd_lan` (45 s, no crash), so the new helper links
and initialises inside the real application. **Not verified by hand: that the dialog renders
the changed suggestions and that Apply still creates the rows and chips.** The Apply path
was not touched — the recipe struct, its pair/gradient encoding and its `preview_color` are
produced exactly as before — but that is an argument, not an observation.

## 7. Licence position

`deps_src/colorsolver/` is AGPL-3.0, "Copyright (C) 2026 sentientstardust". Vendoring AGPL
into an AGPL-3.0 product is fine. `NOTICE` carries the upstream attribution, the upstream
repository and commit, and an explicit list of what this copy keeps, changes and drops.
`README.md`'s "Third-party components added by this fork" line now names it.

Two items still need a human before a binary release, unchanged from the plan's §5.2:

1. that `sentientstardust-dev/OrcaSlicer-ImageMap` @ `9254838105` really carries that header
   on those files **upstream** — check the upstream repository, not the PR's copy;
2. `filament_mixer_model.h`'s claim that it is a polynomial approximation of Mixbox
   containing no Mixbox source, binaries or data. This change makes that file load-bearing
   for the colours the slicer suggests, not only for preview swatches, so it matters more
   than it did.

The GPL-3.0 dependency (pigment-painter) and the disputed-MIT one (prusa-fdm-mixer) are not
vendored, so neither needs verifying.
