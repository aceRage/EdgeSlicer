#include "ThinFeatures.hpp"

#include "ClipperUtils.hpp"
#include "I18N.hpp"
#include "LocalesUtils.hpp"
#include "format.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace Slic3r {

namespace {

// Slivers thinner than this are not features: Clipper rounding and the arcs of the opening leave fringes a few
// micrometres wide along every edge, and corners of ordinary parts leave short ones.
constexpr double kNoiseWidth = 0.03;
// A line is "wider than the model" when the model is more than a tenth narrower than the line.
constexpr double kWidenedFraction = 0.9;
// Resolution of the "down to" width.
constexpr double kWidthStep = 0.01;
// Lowest width the "down to" search tests: below it nothing is a printable stroke anyway.
constexpr double kLowestWidth = 0.04;
// The skipped part is taken off the widened band with this margin, so the two kinds never share a sliver.
constexpr double kBandMargin = 0.02;

struct Candidate
{
    ExPolygon shape;
    double    length{0.};
};

// The spot's length: half its perimeter less the limit it was found against. Exact for a strip of that width, a
// little short for a narrower one (it errs towards not reporting).
double estimated_length(const ExPolygon &spot, double limit_mm)
{
    double perimeter = spot.contour.length();
    for (const Polygon &hole : spot.holes)
        perimeter += hole.length();
    return std::max(0., 0.5 * unscaled<double>(perimeter) - limit_mm);
}

// The part of `outline` narrower than `width_mm`, cleaned of slivers.
ExPolygons thin_part(const ExPolygons &outline, double width_mm)
{
    const ExPolygons thin = diff_ex(outline, opening_ex(outline, scaled<float>(0.5 * width_mm)));
    if (thin.empty())
        return thin;
    return opening_ex(thin, scaled<float>(0.5 * kNoiseWidth));
}

std::vector<Candidate> long_spots(const ExPolygons &thin, double limit_mm, double min_length_mm)
{
    std::vector<Candidate> out;
    for (const ExPolygon &spot : thin) {
        const double length = estimated_length(spot, limit_mm);
        if (length >= min_length_mm)
            out.push_back({ spot, length });
    }
    return out;
}

// The part of the thin band between `skip_part` and what is narrower than `width_mm`.
ExPolygons band_above(const ExPolygons &outline, const ExPolygons &skip_part, double width_mm)
{
    const ExPolygons narrow = thin_part(outline, width_mm);
    if (narrow.empty() || skip_part.empty())
        return narrow;
    const ExPolygons band = diff_ex(narrow, offset_ex(skip_part, scaled<float>(kBandMargin)));
    if (band.empty())
        return band;
    return opening_ex(band, scaled<float>(0.5 * kNoiseWidth));
}

// Smallest width in [lo_mm, hi_mm], in steps of kWidthStep, at which `exists(width)` holds; hi_mm is taken to hold.
template<class Exists>
double narrowest_width(double lo_mm, double hi_mm, Exists exists)
{
    int lo = int(std::ceil(lo_mm / kWidthStep - 1e-9));
    int hi = int(std::floor(hi_mm / kWidthStep + 1e-9));
    if (hi < lo)
        return hi_mm;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (exists(mid * kWidthStep))
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo * kWidthStep;
}

} // namespace

void find_thin_features(const ExPolygons &outline, const ThinFeatureLimits &limits, ThinFeatureLayer &layer)
{
    if (outline.empty())
        return;
    const double skip       = unscaled<double>(limits.skip_width);
    const double line       = unscaled<double>(limits.line_width);
    const double min_length = unscaled<double>(limits.min_length);
    const double wide       = line > 0. ? kWidenedFraction * line : 0.;
    const bool   has_skip   = skip > kNoiseWidth;
    const bool   has_wide   = wide > std::max(skip, kNoiseWidth) + kWidthStep;
    if (!has_skip && !has_wide)
        return;

    // One opening decides most layers: nothing long and thin in it, nothing to report.
    const double top = has_wide ? wide : skip;
    const ExPolygons thin_top = thin_part(outline, top);
    if (thin_top.empty() || long_spots(thin_top, has_skip ? skip : top, min_length).empty())
        return;

    const ExPolygons thin_skip = has_skip ? (has_wide ? thin_part(outline, skip) : thin_top) : ExPolygons();
    std::vector<Candidate> skipped = has_skip ? long_spots(thin_skip, skip, min_length) : std::vector<Candidate>();
    std::vector<Candidate> widened;
    if (has_wide) {
        const ExPolygons band = has_skip && !thin_skip.empty() ?
            opening_ex(diff_ex(thin_top, offset_ex(thin_skip, scaled<float>(kBandMargin))), scaled<float>(0.5 * kNoiseWidth)) :
            thin_top;
        widened = long_spots(band, wide, min_length);
    }
    if (skipped.empty() && widened.empty())
        return;

    if (!skipped.empty()) {
        for (Candidate &c : skipped)
            layer.skipped.push_back({ std::move(c.shape), c.length, skip });
        const double w = narrowest_width(kLowestWidth, skip, [&](double width) {
            return !long_spots(thin_part(outline, width), width, min_length).empty();
        });
        layer.skipped_min_width = layer.skipped_min_width > 0. ? std::min(layer.skipped_min_width, w) : w;
    }
    if (!widened.empty()) {
        for (Candidate &c : widened)
            layer.widened.push_back({ std::move(c.shape), c.length, line });
        const double w = narrowest_width(has_skip ? skip : kLowestWidth, wide, [&](double width) {
            return !long_spots(band_above(outline, thin_skip, width), width, min_length).empty();
        });
        layer.widened_min_width = layer.widened_min_width > 0. ? std::min(layer.widened_min_width, w) : w;
    }
}

namespace {

struct Node
{
    const ThinFeatureLayer *layer{nullptr};
    const ThinSpot         *spot{nullptr};
    BoundingBox             box;
    int                     parent{0};
};

int find_root(std::vector<Node> &nodes, int i)
{
    while (nodes[i].parent != i) {
        nodes[i].parent = nodes[nodes[i].parent].parent;
        i               = nodes[i].parent;
    }
    return i;
}

// Stacks the spots of consecutive layers that overlap, and fills `kind` from the stacks.
void summarize_kind(const std::vector<const ThinFeatureLayer *> &layers, bool skipped, ThinFeatureKind &kind)
{
    std::vector<Node> nodes;
    for (const ThinFeatureLayer *layer : layers)
        for (const ThinSpot &spot : skipped ? layer->skipped : layer->widened)
            nodes.push_back({ layer, &spot, get_extents(spot.shape), int(nodes.size()) });
    if (nodes.empty())
        return;
    // Nodes of one layer id are adjacent after this sort; within it the order is the caller's.
    std::stable_sort(nodes.begin(), nodes.end(), [](const Node &a, const Node &b) { return a.layer->layer_id < b.layer->layer_id; });
    for (size_t i = 0; i < nodes.size(); ++i)
        nodes[i].parent = int(i);

    size_t first_this = 0;
    while (first_this < nodes.size()) {
        const int layer_id = nodes[first_this].layer->layer_id;
        size_t    end_this = first_this;
        while (end_this < nodes.size() && nodes[end_this].layer->layer_id == layer_id)
            ++end_this;
        size_t end_next = end_this;
        while (end_next < nodes.size() && nodes[end_next].layer->layer_id == layer_id + 1)
            ++end_next;
        for (size_t a = first_this; a < end_this; ++a)
            for (size_t b = end_this; b < end_next; ++b) {
                if (!nodes[a].box.overlap(nodes[b].box))
                    continue;
                const int ra = find_root(nodes, int(a)), rb = find_root(nodes, int(b));
                if (ra == rb)
                    continue;
                if (!intersection_ex(ExPolygons{ nodes[a].spot->shape }, ExPolygons{ nodes[b].spot->shape }).empty())
                    nodes[rb].parent = ra;
            }
        first_this = end_this;
    }

    // Per stack: the summed length, and its longest spot.
    std::vector<double> stack_length(nodes.size(), 0.);
    std::vector<int>    stack_best(nodes.size(), -1);
    for (size_t i = 0; i < nodes.size(); ++i) {
        const int r = find_root(nodes, int(i));
        stack_length[r] += nodes[i].spot->length;
        if (stack_best[r] < 0 || nodes[i].spot->length > nodes[stack_best[r]].spot->length)
            stack_best[r] = int(i);
        kind.total_length += nodes[i].spot->length;
        kind.limit_width = std::max(kind.limit_width, nodes[i].spot->limit_width);
    }
    int    best_stack  = -1;
    double best_length = -1.;
    for (size_t i = 0; i < nodes.size(); ++i)
        if (find_root(nodes, int(i)) == int(i)) {
            ++kind.count;
            if (stack_length[i] > best_length) {
                best_length = stack_length[i];
                best_stack  = int(i);
            }
        }
    if (best_stack >= 0) {
        const Node &rep    = nodes[stack_best[best_stack]];
        kind.rep_layer_id  = rep.layer->layer_id;
        kind.rep_print_z   = rep.layer->print_z;
        const BoundingBox &bb = rep.box;
        kind.rep_xy        = Vec2d(unscaled<double>(0.5 * (double(bb.min.x()) + double(bb.max.x()))),
                                   unscaled<double>(0.5 * (double(bb.min.y()) + double(bb.max.y()))));
    }
    for (const ThinFeatureLayer *layer : layers) {
        const double w = skipped ? layer->skipped_min_width : layer->widened_min_width;
        if ((skipped ? !layer->skipped.empty() : !layer->widened.empty()) && w > 0.)
            kind.min_width = kind.min_width > 0. ? std::min(kind.min_width, w) : w;
    }
}

std::string mm(double value) { return float_to_string_decimal_point(value, 2); }

} // namespace

ThinFeatureSummary summarize_thin_features(const std::vector<const ThinFeatureLayer *> &layers_in)
{
    std::vector<const ThinFeatureLayer *> layers;
    for (const ThinFeatureLayer *layer : layers_in)
        if (layer != nullptr && !layer->empty())
            layers.push_back(layer);
    ThinFeatureSummary summary;
    summarize_kind(layers, true, summary.skipped);
    summarize_kind(layers, false, summary.widened);
    // The hint follows the layers that carry spots: Fine detail does not act on the first layer, so a spot there
    // never asks for it.
    for (const ThinFeatureLayer *layer : layers) {
        summary.can_print_thinner          = summary.can_print_thinner || layer->can_print_thinner;
        summary.classic_without_thin_walls = summary.classic_without_thin_walls || layer->classic_without_thin_walls;
    }
    return summary;
}

std::string thin_feature_notice(const ThinFeatureSummary &summary, const std::string &object_name)
{
    const bool say_widened = summary.widened.count > 0 && summary.widened.total_length >= thin_feature_min_total_length;
    const bool say_skipped = summary.skipped.count > 0 && summary.skipped.total_length >= thin_feature_min_total_length;
    if (!say_widened && !say_skipped)
        return {};

    std::string text = object_name + ": ";
    if (say_widened) {
        if (summary.widened.count == 1)
            text += Slic3r::format(_u8L("1 thin feature (%1% mm) was printed wider than the model, at %2% mm"),
                                   mm(summary.widened.min_width), mm(summary.widened.limit_width));
        else
            text += Slic3r::format(_u8L("%1% thin features (down to %2% mm) were printed wider than the model, at %3% mm"),
                                   summary.widened.count, mm(summary.widened.min_width), mm(summary.widened.limit_width));
        if (say_skipped) {
            if (summary.skipped.count == 1)
                text += Slic3r::format(_u8L("; 1 thinner than %1% mm was skipped"), mm(summary.skipped.limit_width));
            else
                text += Slic3r::format(_u8L("; %1% thinner than %2% mm were skipped"), summary.skipped.count,
                                       mm(summary.skipped.limit_width));
        }
    } else {
        if (summary.skipped.count == 1)
            text += Slic3r::format(_u8L("1 thin feature thinner than %1% mm was not printed"), mm(summary.skipped.limit_width));
        else
            text += Slic3r::format(_u8L("%1% thin features thinner than %2% mm were not printed"), summary.skipped.count,
                                   mm(summary.skipped.limit_width));
    }
    text += ".";

    if (summary.can_print_thinner)
        text += " " + (summary.classic_without_thin_walls ?
                           _u8L("Set Fine detail to Fine text, or turn on Detect thin wall, to print them thinner.") :
                           _u8L("Set Fine detail to Fine text to print them thinner."));

    const ThinFeatureKind &rep = say_skipped ? summary.skipped : summary.widened;
    if (rep.rep_layer_id >= 0)
        text += " " + Slic3r::format(_u8L("Largest at Z %1% mm."), mm(rep.rep_print_z));
    return text;
}

} // namespace Slic3r
