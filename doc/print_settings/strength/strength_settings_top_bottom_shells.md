# Top and Bottom Shells

Controls how the top and bottom solid layers (shells) are generated.

![top-bottom-shells](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/top-bottom-shells/top-bottom-shells.png?raw=true)

## Shell Layers

This is the number of solid shell layers, including the surface layer.  
When the thickness calculated from this value is less than [shell thickness](#shell-thickness), the shell layers will be increased.

These layers are printed over the [sparse infill](strength_settings_infill), so increasing **shell layers** will increase overall part strength and top surface quality.
It's usually recommended to have at least 3 shell layers for most prints.

## Shell Thickness

The number of solid layers is increased during slicing if the thickness calculated from shell layers is thinner than this value. This avoids having too thin a shell when layer height is small.  
0 means this setting is disabled and shell thickness is determined entirely by [shell layers](#shell-layers).

## Surface Density

This setting controls the density of the top and bottom surfaces. A value of 100% means a solid surface, while lower values create a sparse surface.  
This can be used for aesthetic purposes, improving grip or creating interfaces.

## Infill/Wall Overlap

The top solid infill area is slightly enlarged to overlap with walls for better bonding and to minimize pinholes where the infill meets the walls.  
A value of 25-30% is a good starting point. The percentage value is relative to the line width of the sparse infill.

> [!TIP]
> Check [Monotonic Line](strength_settings_patterns#monotonic-line) to learn about its overlaying differences with [Monotonic](strength_settings_patterns#monotonic) and [Rectilinear](strength_settings_patterns#rectilinear).

## Surface Pattern

This setting controls the pattern of the surfaces.  
If [Shell Layers](#shell-layers) is greater than 1, the surface pattern will be applied to the outermost shell layer only and the rest will use [Internal Solid Infill Pattern](strength_settings_infill#internal-solid-infill).

> [!TIP]
> See [Infill Patterns Wiki List](strength_settings_patterns) with **detailed specifications**, including their strengths and weaknesses.

 The surface patterns are:

- **[Concentric](strength_settings_patterns#concentric)**
- **[Rectilinear](strength_settings_patterns#rectilinear)**
- **[Monotonic](strength_settings_patterns#monotonic)**
- **[Monotonic Line](strength_settings_patterns#monotonic-line)** Usually Recommended for Top.
- **[Aligned Rectilinear](strength_settings_patterns#aligned-rectilinear)**
- **[Hilbert Curve](strength_settings_patterns#hilbert-curve)**
- **[Archimedean Chords](strength_settings_patterns#archimedean-chords)**
- **[Octagram Spiral](strength_settings_patterns#octagram-spiral)**

## Fill order

Direction in which top surfaces are filled when using a center-based pattern (Concentric, Spiral Inset, Archimedean Chords, Octagram Spiral).

Outward starts at the center of the surface, so any excess material is pushed towards the edge where it is least visible. Inward starts at the edge and ends with the tight curves at the center.

Default uses shortest-path ordering, which may run in either direction.

## Surface expansion

Expands the top surfaces by this distance to connect distinct top surfaces and fill gaps.

Useful for cases where the top surface is interrupted by a raised feature, such as text on a plane.Expanding it removes the holes beneath these features and creates a continuous path with a better finish for printing on top.The expansion is applied to the original top surface, before any other processing such as bridging or overhang detection.

## Surface expansion margin

Using “Top surface expansion” may cause a surface that did not previously touch the model's outer walls to now do so.

This can cause contraction marks (such as the hull line) on the outer walls.

By adding a small margin, this contraction will not occur directly on the walls, thereby preventing a visible mark.

## Surface expansion direction

Direction in which the top surface expansion grows.

- Inward grows into the holes and gaps left by features rising from the middle of a top surface.

- Outward grows the outer edge of the surface, connecting surfaces separated by features that can divide a surface, such as a lattice pattern.

- Inward and Outward does both.

## Center surface pattern on

Chooses where the centering point of centered top/bottom surface patterns (Archimedean Chords, Octagram Spiral) is placed.

- Each Surface: centers the pattern on every individual surface region, so each island is symmetric on its own.

- Each Model: centers the pattern on each connected body. Parts that touch or overlap share one center; parts detached from the rest each get their own.

- Each Assembly: uses a single shared center for the whole object or assembly.
