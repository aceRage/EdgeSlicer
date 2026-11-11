// precise_z_height (the last five layers share out what the object's top is off by) and make_overhang_printable
// (a slope under every overhang) on layers that are not as thick as the layer_height setting.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

namespace Slic3r {
// Defined in Slicing.cpp.
bool adjust_layer_series_to_align_object_height(const SlicingParameters &slicing_params, std::vector<coordf_t> &layer_series);
} // namespace Slic3r

namespace {

// Layers of these heights from z = 0, as pairs of bottom and top the way generate_object_layers() gives them.
std::vector<coordf_t> series_of(const std::vector<double> &heights)
{
    std::vector<coordf_t> out;
    double                z = 0.;
    for (double h : heights) {
        out.push_back(z);
        z += h;
        out.push_back(z);
    }
    return out;
}

std::vector<double> heights_of(const std::vector<coordf_t> &series)
{
    std::vector<double> out;
    for (size_t i = 0; i + 1 < series.size(); i += 2)
        out.push_back(series[i + 1] - series[i]);
    return out;
}

SlicingParameters limits(double object_height, double min_layer_height, double max_layer_height)
{
    SlicingParameters params;
    params.object_print_z_min = 0.;
    params.object_print_z_max = object_height;
    params.min_layer_height   = min_layer_height;
    params.max_layer_height   = max_layer_height;
    return params;
}

double sum(const std::vector<double> &values) { return std::accumulate(values.begin(), values.end(), 0.); }

bool has_precise_z_warning(const Print &print)
{
    const PrintStateBase::StateWithWarnings state = print.objects().front()->step_state_with_warnings(posSlice);
    return std::any_of(state.warnings.begin(), state.warnings.end(),
                       [](const PrintStateBase::Warning &w) { return w.message_id == int(PrintStateBase::SlicingPreciseZNotApplied); });
}

} // namespace

TEST_CASE("Precise Z height: a layer at the limit takes no part in a later round", "[PreciseZ]")
{
    SECTION("upwards: one of the last five layers near the greatest height allowed")
    {
        // Twelve layers of 0.2 mm, the first of the last five 0.27 mm, 0.28 mm allowed, and an object 0.1 mm
        // taller than the layers. The first round gives each of the five 0.02 mm: the 0.27 mm layer stops at 0.28
        // and leaves 0.01 mm, which the second round shares among the other four.
        std::vector<double> heights(12, 0.2);
        heights[7]          = 0.27;
        const double wanted = sum(heights) + 0.1;
        std::vector<coordf_t> series = series_of(heights);

        REQUIRE(adjust_layer_series_to_align_object_height(limits(wanted, 0.08, 0.28), series));
        const std::vector<double> adjusted = heights_of(series);
        REQUIRE(adjusted.size() == 12);
        CHECK_THAT(series.back(), WithinAbs(wanted, 1e-9));
        CHECK_THAT(adjusted[7], WithinAbs(0.28, 1e-9));
        for (size_t i = 8; i < 12; ++i)
            CHECK_THAT(adjusted[i], WithinAbs(0.2225, 1e-9));
        for (double h : adjusted)
            CHECK(h < 0.28 + 1e-9);
        // The layers before the last five are as they were, and each layer begins where the one beneath ends.
        for (size_t i = 0; i < 7; ++i)
            CHECK_THAT(adjusted[i], WithinAbs(0.2, 1e-12));
        for (size_t i = 2; i < series.size(); i += 2)
            CHECK(series[i] == series[i - 1]);
    }

    SECTION("downwards: the last layer near the least height allowed")
    {
        // The last layer 0.09 mm, 0.08 mm allowed, and an object 0.09 mm shorter than the layers.
        std::vector<double> heights(12, 0.2);
        heights[11]         = 0.09;
        const double wanted = sum(heights) - 0.09;
        std::vector<coordf_t> series = series_of(heights);

        REQUIRE(adjust_layer_series_to_align_object_height(limits(wanted, 0.08, 0.28), series));
        const std::vector<double> adjusted = heights_of(series);
        CHECK_THAT(series.back(), WithinAbs(wanted, 1e-9));
        CHECK_THAT(adjusted[11], WithinAbs(0.08, 1e-9));
        for (size_t i = 7; i < 11; ++i)
            CHECK_THAT(adjusted[i], WithinAbs(0.18, 1e-9));
        for (double h : adjusted)
            CHECK(h > 0.08 - 1e-9);
    }

    SECTION("what was right stays right")
    {
        // Nothing near a limit: one round, 0.02 mm each.
        std::vector<coordf_t> series = series_of(std::vector<double>(12, 0.2));
        REQUIRE(adjust_layer_series_to_align_object_height(limits(2.5, 0.08, 0.28), series));
        for (size_t i = 7; i < 12; ++i)
            CHECK_THAT(heights_of(series)[i], WithinAbs(0.22, 1e-9));
        CHECK_THAT(series.back(), WithinAbs(2.5, 1e-9));

        // Already at the object's height: nothing to do.
        series = series_of(std::vector<double>(12, 0.2));
        const std::vector<coordf_t> before = series;
        REQUIRE(adjust_layer_series_to_align_object_height(limits(2.4, 0.08, 0.28), series));
        CHECK(series == before);

        // More than the five layers can take (0.05 mm of room, 0.1 mm asked): no, and nothing is changed.
        series = series_of(std::vector<double>(12, 0.27));
        const std::vector<coordf_t> full = series;
        REQUIRE_FALSE(adjust_layer_series_to_align_object_height(limits(12 * 0.27 + 0.1, 0.08, 0.28), series));
        CHECK(series == full);

        // Fewer than six layers: no.
        series = series_of(std::vector<double>(5, 0.2));
        REQUIRE_FALSE(adjust_layer_series_to_align_object_height(limits(1.05, 0.08, 0.28), series));
    }
}

TEST_CASE("Precise Z height says so when it cannot be applied", "[PreciseZ]")
{
    auto sliced = [](Print &print, double height, const char *precise) {
        init_and_process_print({make_cube(10., 10., height)}, print, {
            {"precise_z_height", precise},
            {"layer_height", "0.2"},
            {"initial_layer_print_height", "0.2"},
            {"nozzle_diameter", "0.4"},
        });
    };

    SECTION("an object of fewer than six layers")
    {
        // 1.05 mm at 0.2 mm: five layers, ending at 1 mm.
        Print print;
        sliced(print, 1.05, "1");
        REQUIRE(print.objects().front()->layers().size() == 5);
        CHECK_THAT(print.objects().front()->layers().back()->print_z, WithinAbs(1.0, 1e-6));
        CHECK(has_precise_z_warning(print));
    }
    SECTION("an object it can be applied to")
    {
        // 3.05 mm: fifteen layers, the last five 0.21 mm.
        Print print;
        sliced(print, 3.05, "1");
        CHECK_THAT(print.objects().front()->layers().back()->print_z, WithinAbs(3.05, 1e-6));
        CHECK_FALSE(has_precise_z_warning(print));
    }
    SECTION("with the option off there is nothing to say")
    {
        Print print;
        sliced(print, 1.05, "0");
        CHECK_FALSE(has_precise_z_warning(print));
    }
}

TEST_CASE("Make overhangs printable follows each layer's own height", "[PreciseZ][MakeOverhangPrintable]")
{
    // A post 4 mm square and 6 mm tall with a cap 20 mm square and 2 mm thick on it. layer_height is 0.2 mm, and a
    // height range from 1 mm up gives the layers there 0.1 mm. With the slope at 45 degrees every layer under the
    // cap is to be narrower than the one above it by that layer's height on each side.
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "cap_on_post.stl";
    TriangleMesh post   = make_cube(4., 4., 6.);
    post.translate(-2.f, -2.f, 0.f);
    TriangleMesh cap = make_cube(20., 20., 2.);
    cap.translate(-10.f, -10.f, 6.f);
    object->add_volume(post);
    object->add_volume(cap);
    {
        DynamicPrintConfig range;
        range.set_key_value("layer_height", new ConfigOptionFloat(0.1));
        object->layer_config_ranges[{1., 8.}].assign_config(range);
    }
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"make_overhang_printable", "1"},
        {"make_overhang_printable_angle", "45"},
        {"make_overhang_printable_hole_size", "0"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"enable_support", "0"},
        // full_print_config() has relative E, which validate() refuses without a per-layer reset.
        {"layer_change_gcode", "G92 E0"},
    });

    Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);
    const StringObjectException err = print.validate();
    INFO(err.string);
    REQUIRE(err.string.empty());
    print.process();

    const auto layers = print.objects().front()->layers();
    REQUIRE(layers.size() > 60);

    size_t thin_pairs = 0, thick_pairs = 0;
    for (size_t i = 0; i + 1 < layers.size(); ++i) {
        const Layer *lower = layers[i];
        const Layer *upper = layers[i + 1];
        // Under the cap: both layers below 6 mm.
        if (upper->print_z > 6. + EPSILON)
            break;
        const double lower_width = unscale<double>(get_extents(lower->lslices).size().x());
        const double upper_width = unscale<double>(get_extents(upper->lslices).size().x());
        CAPTURE(i, lower->print_z, upper->print_z, upper->height, lower_width, upper_width);
        // tan(45 degrees) is 1: the upper layer's height, on either side.
        CHECK_THAT(upper_width - lower_width, WithinAbs(2. * upper->height, 0.02));
        if (std::abs(upper->height - 0.1) < EPSILON)
            ++thin_pairs;
        else if (std::abs(upper->height - 0.2) < EPSILON)
            ++thick_pairs;
    }
    // The case only means something if there are layers of both heights under the cap.
    CHECK(thin_pairs >= 40);
    CHECK(thick_pairs >= 3);
}
