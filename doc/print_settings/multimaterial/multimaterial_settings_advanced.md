# Multimaterial Advanced

- [Interlocking Beam](#interlocking-beam)
- [Interface Shells](#interface-shells)
- [Maximum Width of Segmented Region](#maximum-width-of-segmented-region)
- [Interlocking depth of Segmented Region](#interlocking-depth-of-segmented-region)
- [Interlocking Beam Width](#interlocking-beam-width)
- [Interlocking Direction](#interlocking-direction)
- [Interlocking Beam Layers](#interlocking-beam-layers)
- [Interlocking Depth](#interlocking-depth)
- [Interlocking Boundary Avoidance](#interlocking-boundary-avoidance)

## Interlocking Beam

Generate interlocking beam structure at the locations where different filaments touch. This improves the adhesion between filaments, especially models printed in different materials.

## Interface Shells

Force the generation of solid shells between adjacent materials/volumes. Useful for multi-extruder prints with translucent materials or manual soluble support material.

## Maximum Width of Segmented Region

Maximum width of a segmented region. Zero disables this feature.

## Interlocking depth of Segmented Region

Interlocking depth of a segmented region. It will be ignored if \"mmu_segmented_region_max_width\" is zero or if \"mmu_segmented_region_interlocking_depth\" is bigger than \"mmu_segmented_region_max_width\". Zero disables this feature.

## Interlocking Beam Width

The width of the interlocking structure beams.

## Interlocking Direction

Orientation of interlock beams.

## Interlocking Beam Layers

The height of the beams of the interlocking structure, measured in number of layers. Less layers is stronger, but more prone to defects.

## Interlocking Depth

The distance from the boundary between filaments to generate interlocking structure, measured in cells. Too few cells will result in poor adhesion.

## Interlocking Boundary Avoidance

The distance from the outside of a model where interlocking structures will not be generated, measured in cells.

## Toolchange ordering

Determines the order of tool changes on each layer.

- **Default**: Starts with the last used extruder to minimize tool changes.
- **Cyclic**: Uses a fixed tool sequence each layer. This sacrifices speed for better surface quality, as the extra toolchanges allow layers more time to cool.

## Toolchange order

Custom filament sequence used by the cyclic toolchange ordering, as filament numbers separated by commas (e.g. "3,2,1,4"). Each layer prints its filaments following this sequence; filaments not listed are printed last, in ascending order. Leave empty to cycle through the filaments in ascending order.

Whether to apply the cyclic order to the first layer as well is controlled separately: by default the first layer is instead ordered for the best bed adhesion (filaments that print small, fragile first-layer features are printed last), so the cyclic order's cooling benefit does not apply there. Enable *Apply cyclic order to first layer* only if you need the exact same tool sequence on every layer.

## Order-independent overlap carving

When two normal parts of the same object overlap, the smaller part carves the larger one, no matter which order the parts appear in the object list. With this off the part listed later always carves the one listed earlier, so a small part sitting inside a bigger one is erased outright when it happens to be listed first. Useful for multi-body STEP imports, where the exporting CAD program decides the body order.

## Paint depth mode

Controls how thick a painted (multi-material/multi-color) claim is before the object reverts to its base filament for walls, solid infill and sparse infill. The depth is measured perpendicular to the painted surface, so it is the same on vertical walls, curves and flat tops.

- **Unlimited**: the painted color extends as deep as the painted region reaches.
- **Limited by walls**: the claim is bounded by a number of wall widths (see *Paint depth walls*). This is the default.
- **Limited by distance**: the claim is bounded by an absolute distance in millimetres (see *Paint depth distance*).

## Paint depth walls

Thickness of a painted claim, expressed as a number of wall widths, when *Paint depth mode* is set to "Limited by walls".

## Paint depth distance

How thick a painted claim is, measured perpendicular to the painted surface, when *Paint depth mode* is set to "Limited by distance". This is the number to read as millimetres of colour depth.

## Paint sparse infill

If enabled (default), a painted claim's sparse infill is printed in the painted filament, so colour shows through sparse regions; if disabled, the sparse infill reverts to the base filament even where walls are painted.

## Paint depth solid interfaces

Print solid shells at colour boundaries inside the object so the base colour does not bleed through thin walls near a painted surface.
