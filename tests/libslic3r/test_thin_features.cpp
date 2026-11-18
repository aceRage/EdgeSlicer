// EdgeSlicer, thin feature notice (tests/design_arachne_thin_features.md, P3: B1 / B2).
//
// The detector and the notice text on plain outlines: a plate of fins of different widths, stacked over layers, for
// the limits of the Arachne generator at Fine detail Off / Balanced / Fine text (the numbers of the P2 width table:
// 0.4 nozzle, 0.2 mm layers, outer 0.42 / inner 0.45 lines, precise outer wall, which takes 0.0858 mm off every thin
// feature) and of the Classic generator. The end-to-end counts through the real generators are in
// tests/fff_print/test_thin_feature_notice.cpp.
//
// The plate: fins 0.08, 0.16, 0.22, 0.30 and 0.50 mm wide, 16 mm long, 4 mm apart. The model widths at which the
// limits switch are
//   Off          skip under 0.186 (min_feature_size 0.10 + 0.0858), widened under 0.345 (90 % of the 0.383 line)
//   Balanced     skip under 0.186,                                  widened under 0.255 (90 % of 0.283)
//   Fine text    skip under 0.146 (0.06 + 0.0858),                  widened under 0.201 (90 % of 0.223)
// so no fin sits closer than 0.014 mm to a switch point.

#include <catch2/catch.hpp>

#include "libslic3r/Arachne/WallToolPaths.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/FineDetail.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ThinFeatures.hpp"

#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kNozzle = 0.4;
constexpr double kLayer  = 0.2;
constexpr double kOuter  = 0.42;
constexpr double kInner  = 0.45;
constexpr double kFinLen = 16.;
constexpr int    kLayers = 12;

const std::vector<double> kFinWidths{ 0.08, 0.16, 0.22, 0.30, 0.50 };

double spacing(double width) { return width - kLayer * (1. - 0.25 * PI); }

ExPolygons fin_plate(const std::vector<double> &widths = kFinWidths, double length = kFinLen)
{
    ExPolygons out;
    double     x = 0.;
    for (const double w : widths) {
        Polygon fin;
        fin.points = { Point::new_scale(x, 0.), Point::new_scale(x + w, 0.), Point::new_scale(x + w, length), Point::new_scale(x, length) };
        out.emplace_back(fin);
        x += 4.;
    }
    return out;
}

// The limits process_arachne() hands the detector: precise outer wall, layer 5, a 0.4 nozzle.
ThinFeatureLimits arachne_limits(FineDetailLevel level)
{
    PrintObjectConfig object;
    PrintRegionConfig region;
    PrintConfig       print;
    print.nozzle_diameter.values = { kNozzle };
    region.fine_detail.value     = level;

    const coord_t ext_width   = scaled<coord_t>(kOuter);
    const coord_t ext_spacing = scaled<coord_t>(spacing(kOuter));
    const coord_t spacing_x   = scaled<coord_t>(spacing(kInner));
    const Arachne::WideningLimits widening = Arachne::widening_limits(Arachne::make_paths_params(5, object, print, &region), ext_spacing,
                                                                      spacing_x, kLayer);
    ThinFeatureLimits limits;
    limits.skip_width = widening.input_width + 2 * (ext_width - ext_spacing);
    limits.line_width = scaled<coord_t>(Flow::rounded_rectangle_extrusion_width_from_spacing(unscale<float>(widening.output_width), float(kLayer)));
    limits.min_length = 2 * ext_width;
    return limits;
}

// Classic: a narrow island prints from the smaller external line up; with thin-wall detection from nozzle / 3.
ThinFeatureLimits classic_limits(bool thin_walls)
{
    ThinFeatureLimits limits;
    const double ext_spacing  = spacing(kOuter);
    limits.skip_width = scaled<coord_t>(thin_walls ? kNozzle / 3. : kOuter - 0.5 * 0.22 * ext_spacing);
    limits.line_width = 0;
    limits.min_length = scaled<coord_t>(2. * kOuter);
    return limits;
}

// The plate on layers 1..kLayers (z = 0.2 * id), summarised.
ThinFeatureSummary stack_layers(const ExPolygons &outline, const ThinFeatureLimits &limits, std::vector<ThinFeatureLayer> &store, int first = 1,
                                int count = kLayers)
{
    for (int i = 0; i < count; ++i) {
        ThinFeatureLayer layer;
        layer.layer_id = first + i;
        layer.print_z  = kLayer * (first + i);
        find_thin_features(outline, limits, layer);
        if (!layer.empty())
            store.push_back(std::move(layer));
    }
    std::vector<const ThinFeatureLayer *> pointers;
    for (const ThinFeatureLayer &layer : store)
        pointers.push_back(&layer);
    return summarize_thin_features(pointers);
}

} // namespace

TEST_CASE("Thin features: a plate with no thin part has no spots", "[ThinFeatures]")
{
    // A 24 x 20 plate with a 1.2 mm fin: corners and edges leave only slivers, which are not features.
    ExPolygons outline = fin_plate({ 1.2 });
    Polygon    plate;
    plate.points = { Point::new_scale(10., 0.), Point::new_scale(34., 0.), Point::new_scale(34., 20.), Point::new_scale(10., 20.) };
    outline.emplace_back(plate);
    for (const FineDetailLevel level : { FineDetailLevel::Off, FineDetailLevel::Balanced, FineDetailLevel::FineText }) {
        DYNAMIC_SECTION("level " << int(level)) {
            std::vector<ThinFeatureLayer> store;
            const ThinFeatureSummary      summary = stack_layers(outline, arachne_limits(level), store);
            CHECK(store.empty());
            CHECK(summary.skipped.count == 0);
            CHECK(summary.widened.count == 0);
            CHECK(thin_feature_notice(summary, "plate").empty());
        }
    }
    std::vector<ThinFeatureLayer> store;
    CHECK(stack_layers(outline, classic_limits(false), store).skipped.count == 0);
    CHECK(store.empty());
}

TEST_CASE("Thin features: how many fins are skipped and widened at each Fine detail level (Arachne)", "[ThinFeatures]")
{
    struct Expect
    {
        FineDetailLevel level;
        size_t          skipped, widened;
    };
    // Fins 0.08 0.16 0.22 0.30 0.50, against the switch points in the header.
    const std::vector<Expect> expected{
        { FineDetailLevel::Off, 2, 2 },
        { FineDetailLevel::Balanced, 2, 1 },
        { FineDetailLevel::FineText, 1, 1 },
    };
    for (const Expect &e : expected) {
        DYNAMIC_SECTION("level " << int(e.level)) {
            std::vector<ThinFeatureLayer> store;
            const ThinFeatureSummary      summary = stack_layers(fin_plate(), arachne_limits(e.level), store);
            // A fin is one feature however many layers it stands on.
            CHECK(summary.skipped.count == e.skipped);
            CHECK(summary.widened.count == e.widened);
            // Length is summed over the layers: about 16 mm per fin per layer.
            CHECK_THAT(summary.skipped.total_length, WithinRel(double(e.skipped) * kLayers * kFinLen, 0.05));
            CHECK_THAT(summary.widened.total_length, WithinRel(double(e.widened) * kLayers * kFinLen, 0.05));
            // The narrowest fin of each kind, to the 0.01 mm resolution of the search.
            CHECK_THAT(summary.skipped.min_width, WithinAbs(0.09, 0.015));
            CHECK(summary.skipped.limit_width > 0.14);
            CHECK(summary.skipped.limit_width < 0.19);
            if (e.level == FineDetailLevel::FineText)
                CHECK_THAT(summary.widened.min_width, WithinAbs(0.17, 0.015));
            else
                CHECK_THAT(summary.widened.min_width, WithinAbs(0.23, 0.015));
            // The printed line the widened fins get: 0.383 mm Off, 0.283 Balanced, 0.223 Fine text.
            const double line = e.level == FineDetailLevel::Off ? 0.383 : e.level == FineDetailLevel::Balanced ? 0.283 : 0.223;
            CHECK_THAT(summary.widened.limit_width, WithinAbs(line, 0.005));
            // A location is given for the jump-to text.
            CHECK(summary.skipped.rep_layer_id >= 1);
            CHECK(summary.widened.rep_layer_id >= 1);
        }
    }
}

TEST_CASE("Thin features: a thin wall detection run prints fins narrower than a third of the nozzle, a plain Classic run skips more", "[ThinFeatures]")
{
    {
        // Classic as shipped (BBL / U1 / CC): everything up to the smaller external line (0.378) is skipped. Nothing is
        // "widened": Classic never prints a thin line wider than its feature.
        std::vector<ThinFeatureLayer> store;
        const ThinFeatureSummary      summary = stack_layers(fin_plate(), classic_limits(false), store);
        CHECK(summary.skipped.count == 4); // 0.08 .. 0.30; the 0.50 fin prints
        CHECK(summary.widened.count == 0);
    }
    {
        std::vector<ThinFeatureLayer> store;
        const ThinFeatureSummary      summary = stack_layers(fin_plate(), classic_limits(true), store);
        CHECK(summary.skipped.count == 1); // only 0.08 < 0.133
        CHECK(summary.widened.count == 0);
    }
}

TEST_CASE("Thin features: spots on consecutive layers are one feature, a gap makes two", "[ThinFeatures]")
{
    const ExPolygons outline = fin_plate({ 0.08 });
    std::vector<ThinFeatureLayer> store;
    stack_layers(outline, arachne_limits(FineDetailLevel::Off), store, 1, 4);
    ThinFeatureSummary one = summarize_thin_features([&] {
        std::vector<const ThinFeatureLayer *> p;
        for (const ThinFeatureLayer &l : store)
            p.push_back(&l);
        return p;
    }());
    CHECK(one.skipped.count == 1);
    // Layers 1-4, none on 5, then 6-9: two features.
    stack_layers(outline, arachne_limits(FineDetailLevel::Off), store, 6, 4);
    std::vector<const ThinFeatureLayer *> pointers;
    for (const ThinFeatureLayer &l : store)
        pointers.push_back(&l);
    CHECK(summarize_thin_features(pointers).skipped.count == 2);
    // Two fins at different places on the same layers stay two features.
    std::vector<ThinFeatureLayer> store2;
    CHECK(stack_layers(fin_plate({ 0.08, 0.08 }), arachne_limits(FineDetailLevel::Off), store2).skipped.count == 2);
}

TEST_CASE("Thin features: slivers and short tips are not features", "[ThinFeatures]")
{
    // A short fin (1 mm): shorter than two external line widths once the limit is taken off it.
    std::vector<ThinFeatureLayer> store;
    stack_layers(fin_plate({ 0.08 }, 0.5), arachne_limits(FineDetailLevel::Off), store);
    CHECK(store.empty());
    // A 30 degree wedge, 10 mm long: its tip is narrower than 0.345 mm for about 0.7 mm, which the estimate (half the
    // perimeter less the limit) puts well under two line widths. The tips are its only thin parts.
    Polygon wedge;
    wedge.points = { Point::new_scale(0., 0.), Point::new_scale(10., 0.), Point::new_scale(10., 5.77) };
    std::vector<ThinFeatureLayer> store2;
    const ThinFeatureSummary summary = stack_layers({ ExPolygon(wedge) }, arachne_limits(FineDetailLevel::Off), store2);
    CHECK(summary.skipped.count == 0);
    CHECK(summary.widened.count == 0);
}

TEST_CASE("Thin features: ordinary totals stay quiet", "[ThinFeatures]")
{
    // One layer of one short strip: well under the 5 mm total the notice needs.
    std::vector<ThinFeatureLayer> store;
    const ThinFeatureSummary      summary = stack_layers(fin_plate({ 0.08 }, 3.), arachne_limits(FineDetailLevel::Off), store, 1, 1);
    CHECK(summary.skipped.count == 1);
    CHECK(summary.skipped.total_length < thin_feature_min_total_length);
    CHECK(thin_feature_notice(summary, "tiny").empty());
}

TEST_CASE("Thin features: the notice text and its suggestion", "[ThinFeatures]")
{
    ThinFeatureSummary s;
    s.widened.count        = 14;
    s.widened.total_length = 120.;
    s.widened.min_width    = 0.21;
    s.widened.limit_width  = 0.383;
    s.widened.rep_layer_id = 11;
    s.widened.rep_print_z  = 2.4;
    s.skipped.count        = 3;
    s.skipped.total_length = 20.;
    s.skipped.min_width    = 0.1;
    s.skipped.limit_width  = 0.186;
    s.skipped.rep_layer_id = 3;
    s.skipped.rep_print_z  = 0.8;

    SECTION("both kinds, Fine text would print them thinner")
    {
        s.can_print_thinner = true;
        CHECK(thin_feature_notice(s, "Logo") ==
              "Logo: 14 thin features (down to 0.21 mm) were printed wider than the model, at 0.38 mm; 3 thinner than 0.19 mm were "
              "skipped. Set Fine detail to Fine text to print them thinner. Largest at Z 0.80 mm.");
    }
    SECTION("already as thin as it goes: no suggestion")
    {
        s.can_print_thinner = false;
        const std::string text = thin_feature_notice(s, "Logo");
        CHECK(text.find("Fine detail") == std::string::npos);
        CHECK(text.find("14 thin features") != std::string::npos);
    }
    SECTION("only the widened kind is long enough")
    {
        s.skipped.total_length = 1.;
        s.can_print_thinner    = true;
        const std::string text = thin_feature_notice(s, "Logo");
        CHECK(text.find("skipped") == std::string::npos);
        CHECK(text.find("Largest at Z 2.40 mm") != std::string::npos);
    }
    SECTION("only skipped features (Classic without thin walls) point at both switches")
    {
        s.widened               = {};
        s.can_print_thinner     = true;
        s.classic_without_thin_walls = true;
        CHECK(thin_feature_notice(s, "Logo") ==
              "Logo: 3 thin features thinner than 0.19 mm were not printed. Set Fine detail to Fine text, or turn on Detect thin wall, "
              "to print them thinner. Largest at Z 0.80 mm.");
    }
    SECTION("one feature")
    {
        s.widened.count = 1;
        s.skipped       = {};
        const std::string text = thin_feature_notice(s, "Logo");
        CHECK(text.find("1 thin feature (0.21 mm) was printed wider than the model, at 0.38 mm.") != std::string::npos);
    }
    SECTION("nothing long enough: no notice")
    {
        s.widened.total_length = 4.9;
        s.skipped.total_length = 4.9;
        CHECK(thin_feature_notice(s, "Logo").empty());
    }
}
