# Z Contouring (Z-layer anti-aliasing)

Z contouring (also known as Z-layer anti-aliasing, option key `zaa_enabled`) varies the actual Z height of extrusion moves so that sloped top surfaces are printed with beads whose height matches the local model edge height, instead of always printing a full fixed layer height. This reduces the stepped "staircase" look on shallow slopes at the cost of thinner beads and slower Z motion on contoured segments.

All Z contouring options live in the **Quality** category and are advanced options. The feature is disabled by default.

## Minimize wall height angle

Reduce the height of top-surface perimeters to match the model edge height. Affects perimeters with a slope less than this angle (degrees). A reasonable value is 35. Set to 0 to disable.

## Minimum Z height

Minimum Z-layer height. Also controls the slicing plane. A smaller minimum height resolves shallower slopes but thins the bead (and slows the move, see *Constant flow speed scaling*). A common value at 0.2 mm layers is 0.05–0.08 mm.

## Don't alternate fill direction

Disable alternating fill direction when using Z contouring.

## Constant flow speed scaling

Keep volumetric flow constant on contoured moves by slowing thin segments. A contoured bead varies between the minimum Z height and the layer height, so at the role's unchanged speed the thin end is starved of material and the Z axis has least time exactly where it moves most. This scales the feed rate by (local height / layer height), so every contoured segment extrudes at the same rate the role would at the nominal layer height. Contoured segments are only ever slowed, never sped up beyond the role's speed, and never below 10 mm/s.
