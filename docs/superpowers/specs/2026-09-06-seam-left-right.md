# Seam position Left and Right

2026-09-06, branch `feat/seam-left-right` off `feat/ultra-preferences` (02785017b2).

The seam position list has offered `Back` since Slic3r: it drags the seam of every loop toward the
back of the bed, where a rear-mounted part-cooling fan can blow at it. Auxiliary part-cooling fans on
these machines are mounted on the *side*, so the seam wants to face left or right instead. This note
records the two new values and why they cost so little.

## What Back actually is

`SeamPosition` is consumed in exactly one place that cares about geometry: `SeamComparator` in
`src/libslic3r/GCode/SeamPlacer.cpp`. `spRear` shows up there three times, plus once as a gate:

* `is_first_better` - after the enforcer/blocker, overhang and "hidden point" rules, and **before**
  the visibility/angle penalty, Back short-circuits on `a.position.y() > b.position.y()`. Because it
  comes before the penalty, the largest Y wins outright; the penalty only breaks ties between points
  at the same Y (the two back corners of a cube, say).
* `is_first_not_much_worse` - the alignment-time version of the same rule, with a slack of
  `seam_align_score_tolerance * 5`.
* `align_seam_points` - the fitted spline's segment count is derived from the string's length, and
  Back multiplies that length by `0.3` so the seam is pulled into a straighter line up the object.
* `SeamPlacer::init` gates `align_seam_points` on `spAligned || spRear || spAlignedBack`.

Notably Back does **not** ask for the raycast visibility pass (`compute_global_occlusion` /
`calculate_candidates_visibility` run for `spAligned`, `spNearest`, `spAlignedBack` only), and it
does not touch `seam_gap` or `staggered_inner_seams` - those are applied in `place_seam` and in
`GCode::_extrude` for every seam position alike. So there was nothing position-specific left to
copy.

## The change

Back, Left and Right are one rule with a different axis and a different sign: score a candidate by
`sign * position[axis]` and prefer the largest score.

| value | axis | sign | picks |
|---|---|---|---|
| `spRear` | Y | `+1` | largest Y |
| `spRight` | X | `+1` | largest X |
| `spLeft` | X | `-1` | smallest X |

`src/libslic3r/GCode/SeamPlacer.cpp`

* New free function `directional_seam_axis(setup, axis, sign)` above `SeamComparator`. Its switch
  lists every `SeamPosition` and has no `default`, so a future value makes the compiler ask whether
  it belongs there.
* `SeamComparator` gained `directional`, `directional_axis`, `directional_sign` (filled in by the
  constructor) and a `directional_score(candidate)` helper. The three `spRear` sites now read
  `directional` / `directional_score`, and the `align_seam_points` gate takes `spLeft` and
  `spRight`.
* The sign is applied as a multiplication by exactly `+1.0f` or `-1.0f`. `1.0f * y` is bit-for-bit
  `y` in IEEE-754, so the Back comparison is the same comparison it was, which is what the off-mode
  identity gate below checks.

`src/libslic3r/PrintConfig.hpp` - `spLeft, spRight` **appended** after `spRandom`. Existing values
keep their numbers (0..4), so anything that stored the integer still means what it meant.

`src/libslic3r/PrintConfig.cpp` - keys `"left"` / `"right"` in `s_keys_map_SeamPosition`, labels
`Left` / `Right` in the option definition (listed after `Back`, since the drop-down maps a selection
back through `enum_values` by key string and not by index), and the tooltip now says
"Back/Left/Right place the seam toward that side of the bed."

Nothing else needed a line. There is no `switch` over `SeamPosition` anywhere else in the tree; the
GUI drop-down (`Tab.cpp` -> `Field.cpp`) is generated from `enum_values`/`enum_labels`; the seam
painting gizmo paints enforcers and blockers and never lists positions; and there is no per-position
icon in `resources/images` to add (`param_back.svg` and friends do not exist, and the icon lookup is
an `exists()` check).

`Plater.cpp`'s pressure-advance calibration still pins `spRear` deliberately - the tower is meant to
show its seam at the back.

## Coordinates

Left and Right are bed directions, exactly as Back is. The comparison happens on `SeamCandidate`
positions, which are unscaled object coordinates - the object's own frame, axis-aligned with the bed
and centred on the object. A rotated object's "left" is therefore the left of its bounding box, same
as Back has always meant the back of the bounding box.

## Gate

`tests/fff_print/test_seam_position.cpp`, two Catch2 scenarios tagged `[Seam]`.

The shape is a **cylinder**, not a cube. A cube has two corners at the extreme of every direction and
sharp corners everywhere, so a back-left corner satisfies both "leftmost" and "backmost" and the test
would not tell the three modes apart. A 20 mm cylinder has exactly one extreme point per direction
and no corner for the angle penalty to prefer, so each mode has one right answer.

Measurement goes through `SeamPlacer::init` + `SeamPlacer::place_seam` on a copy of each outer-wall
loop rather than through exported G-code, which keeps `seam_gap`, the scarf joint and travel moves
out of the numbers.

* back: every seam has Y > 8 mm and |X| < 3 mm
* left: every seam has X < -8 mm and |Y| < 3 mm
* right: every seam has X > 8 mm and |Y| < 3 mm
* each mode's spread along its own axis is under 0.5 mm - the alignment holding the seam in a line
* left and right are mirrors of each other within 0.5 mm
* the config scenario round-trips `"left"` / `"right"` and pins the numeric values 0..6

## Off-mode identity

The rule that matters for a change inside a shared comparator: a slice with a pre-existing seam
position must be byte-identical to the one the shipped binary produces. Checked by slicing
`tests/supportgroup_test.3mf` on one CPU (`cmd /c start /wait /b /affinity 1`) with an isolated
`--datadir`, against the live `EdgeSlicer.exe` at 02785017b2, and diffing everything but the
`; generated by` line.
