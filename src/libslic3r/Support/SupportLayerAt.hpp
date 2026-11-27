#pragma once

// Support layers at an object layer's height, shared by the support-role additions that print on the
// object's own layers (side stabilizers, support fins): find the support layer the support generators
// made at an object layer's print_z, or insert one there, and relink the layers after insertions.

#include "../ExPolygon.hpp"

namespace Slic3r {

class Layer;
class PrintObject;
class SupportLayer;

namespace support_layer_util {

// The support layer at `layer`'s print_z, or null when the support generators made none.
SupportLayer *support_layer_find(PrintObject &object, const Layer &layer);

// The support layer at `layer`'s print_z, inserting one (with the object layer's height and slice_z)
// when the support generators made none; `inserted` is set then.
SupportLayer *support_layer_at(PrintObject &object, const Layer &layer, bool &inserted);

// Keep ids equal to positions and the neighbour links intact after insertions.
void relink_support_layers(PrintObject &object);

// The Z range a support layer's extrusions fill: from its print_z down by its height, or by a merged
// slab's height when that is taller (AdaptiveLayerMerge).
double support_layer_bottom(const SupportLayer &layer);

// Everything the support layers whose Z range overlaps [bottom_z, top_z] (open interval, so layers that
// only touch it do not count) print: their islands and classic tree areas. Independent support layer
// heights and trees put support layers between and across the object's layers; this is what an
// addition on the object layer must keep out of to never print twice in the same place.
ExPolygons support_coverage(const PrintObject &object, double bottom_z, double top_z);

} // namespace support_layer_util
} // namespace Slic3r
