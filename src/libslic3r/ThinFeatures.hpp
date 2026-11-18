#pragma once

// EdgeSlicer, thin feature notice (tests/design_arachne_thin_features.md, P3: B1 / B2, owner decision D6 (a)).
//
// The wall generators (Arachne and Classic) skip features narrower than a limit and Arachne widens the thinnest ones to
// its thinnest line, so a part of the model comes out missing or bolder than drawn without any sign of it. This module
// only OBSERVES: while the walls of a layer are generated it finds, per layer, the spots of the model outline that
// are narrower than the generator can follow, records them, and afterwards turns the layers of an object into one
// slicing notice. It changes no extrusion and no G-code.
//
// What is a spot, a feature and a count (the thresholds are the constants in ThinFeatures.cpp):
//  - A spot is one connected piece of a layer's outline that is narrower than a limit: the part of the outline that
//    the morphological opening by half the limit removes (the same opening as printable_slices() uses), cleaned of
//    slivers thinner than 0.03 mm (Clipper rounding and the corner rounding of the opening).
//      skipped: narrower than the width below which the generator prints no line at all (Arachne: "Skip features
//               thinner than" + the shrink of the outer wall; Classic: the narrowest loop, or nozzle / 3 with thin
//               wall detection).
//      widened: Arachne only. Narrower than 90 % of the line Arachne prints for the thinnest features, i.e. the line
//               is more than a tenth wider than the model there. (Classic's thin-wall lines follow the feature.)
//  - A spot counts only if it is at least two external line widths long, so corners and sharp tips of ordinary
//    models stay out. Length is estimated as half the spot's perimeter less the limit (exact for a strip).
//  - A feature is a stack of spots on consecutive layers whose outlines overlap in XY (one thin fin of 40 layers is
//    one feature, not 40). The count is the number of such stacks; the length is the sum over the layers.
//  - The notice is shown for a kind (skipped / widened) when its summed length is at least 5 mm, so ordinary models
//    stay quiet.

#include "ExPolygon.hpp"
#include "Point.hpp"

#include <string>
#include <vector>

namespace Slic3r {

// What a generator can print, in the model's outline, for one island or group of islands.
struct ThinFeatureLimits
{
    // Features narrower than this get no line.
    coord_t skip_width{0};
    // The width of the line printed for the thinnest features that do get one; 0 when the line follows the feature
    // (nothing is widened).
    coord_t line_width{0};
    // A spot shorter than this is ignored (two external line widths).
    coord_t min_length{0};
};

struct ThinSpot
{
    ExPolygon shape;           // object coordinates, scaled
    double    length{0.};      // mm, estimate (see above)
    double    limit_width{0.}; // mm: skipped: the skip width; widened: the printed line width
};

// What one wall-generator call found on one layer. Empty (no spots) is the usual case and costs no memory.
struct ThinFeatureLayer
{
    int                   layer_id{0};
    double                print_z{0.};
    std::vector<ThinSpot> skipped;
    std::vector<ThinSpot> widened;
    double                skipped_min_width{0.}; // mm, narrowest width at which a spot still counts; 0 = none
    double                widened_min_width{0.};
    bool                  can_print_thinner{false};
    bool                  classic_without_thin_walls{false};

    bool empty() const { return skipped.empty() && widened.empty(); }
};

// Finds the spots of `outline` (model outline of the islands of one layer, scaled) narrower than `limits` and appends
// them to `layer` (layer_id, print_z and the can_print_thinner hint are the caller's). Cheap when there is nothing: one morphological opening of
// the outline, and the rest only for layers that have spots.
void find_thin_features(const ExPolygons &outline, const ThinFeatureLimits &limits, ThinFeatureLayer &layer);

struct ThinFeatureKind
{
    size_t count{0};            // features (stacks of spots on consecutive layers)
    double total_length{0.};    // mm, summed over layers
    double min_width{0.};       // mm, narrowest width seen
    double limit_width{0.};     // mm: skipped: the widest skip limit in force; widened: the widest printed line
    // Representative location: the longest spot of the feature with the largest length. Object coordinates (mm).
    int    rep_layer_id{-1};
    double rep_print_z{0.};
    Vec2d  rep_xy{Vec2d::Zero()};
};

struct ThinFeatureSummary
{
    ThinFeatureKind skipped;
    ThinFeatureKind widened;
    bool            can_print_thinner{false};
    bool            classic_without_thin_walls{false};
};

// Merges the layers of one object. The pointers may be in any order; layers without spots are ignored.
ThinFeatureSummary summarize_thin_features(const std::vector<const ThinFeatureLayer *> &layers);

// A kind is only reported at or above this summed length (mm).
constexpr double thin_feature_min_total_length = 5.;

// The notice text for one object, "" when neither kind is worth reporting.
std::string thin_feature_notice(const ThinFeatureSummary &summary, const std::string &object_name);

} // namespace Slic3r
