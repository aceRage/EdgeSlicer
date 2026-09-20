# Fill Patterns

This page covers the pattern options for the solid surfaces of a print. For infill patterns (sparse interior fill), see [Infill Patterns](../strength/strength_settings_patterns.md).

## Infill of the top surface and bottom surface

The pattern used for the solid top, bottom and just-under-top surfaces of the object.

- **Top surface pattern** (`top_surface_pattern`): the pattern of the very top skin. Monotonic patterns travel in one direction for a cleaner look.
- **Bottom surface pattern** (`bottom_surface_pattern`): the pattern of the first solid layers sitting on the bed (or on support/raft).
- **Internal top surface pattern** (`undertop_surface_pattern`): the pattern of solid layers directly underneath the top skin.

Typical choices:

- **Rectilinear**: the default; fast and strong, slight diagonal ridges may show on the top.
- **Monotonic / Monotonic line**: all lines run in a consistent order and direction, hiding the start/stop points and giving the top surface a uniform sheen — the usual pick for visible top surfaces.
- **Concentric**: follows the shape of the outline; nice for round tops, but can leave gaps where rings don't meet.
- **Hilbert curve, Archimedean chords, Octagram spiral**: decorative fills that avoid long straight travel moves; slower but distinctive.

Pick monotonic variants when surface finish matters, and rectilinear when strength or speed matters more.
