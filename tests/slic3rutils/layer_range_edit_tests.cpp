// The checks on new heights typed into a height range's Min Z / Max Z in the object list (ObjectLayers): an edit
// that would start a range below the bed, leave it without height, or land it on another range's heights (which
// replaced that range and lost its settings) is refused.
//
// GUI_App.hpp first: Windows headers that the GUI headers' boost::asio path needs.
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/GUI_ObjectLayers.hpp"

#include <catch2/catch.hpp>

using namespace Slic3r;
using Slic3r::GUI::check_layer_range_edit;
using Slic3r::GUI::LayerRangeEditCheck;

namespace {

t_layer_config_ranges two_ranges()
{
    t_layer_config_ranges ranges;
    for (const t_layer_height_range r : { t_layer_height_range{ 2., 4. }, t_layer_height_range{ 6., 8. } }) {
        DynamicPrintConfig config;
        config.set_key_value("layer_height", new ConfigOptionFloat(0.1));
        ranges[r].assign_config(std::move(config));
    }
    return ranges;
}

} // namespace

TEST_CASE("Height range edit: a new start or end that keeps the ranges apart is taken", "[LayerRangeEdit]")
{
    const t_layer_config_ranges ranges = two_ranges();
    CHECK(check_layer_range_edit(ranges, { 2., 4. }, { 1., 4. }) == LayerRangeEditCheck::Ok);
    CHECK(check_layer_range_edit(ranges, { 2., 4. }, { 2., 5. }) == LayerRangeEditCheck::Ok);
    CHECK(check_layer_range_edit(ranges, { 2., 4. }, { 0., 4. }) == LayerRangeEditCheck::Ok);
    // Overlapping another range is left as it was: nothing is lost, the slicer trims the later range.
    CHECK(check_layer_range_edit(ranges, { 2., 4. }, { 2., 7. }) == LayerRangeEditCheck::Ok);
}

TEST_CASE("Height range edit: a start below the bed is refused", "[LayerRangeEdit]")
{
    CHECK(check_layer_range_edit(two_ranges(), { 2., 4. }, { -1., 4. }) == LayerRangeEditCheck::Negative);
}

TEST_CASE("Height range edit: a range without height is refused", "[LayerRangeEdit]")
{
    const t_layer_config_ranges ranges = two_ranges();
    // Max Z typed equal to Min Z.
    CHECK(check_layer_range_edit(ranges, { 2., 4. }, { 2., 2. }) == LayerRangeEditCheck::Empty);
    CHECK(check_layer_range_edit(ranges, { 2., 4. }, { 2., 2. + 0.5 * EPSILON }) == LayerRangeEditCheck::Empty);
}

TEST_CASE("Height range edit: another range's heights are refused", "[LayerRangeEdit]")
{
    // Max Z: two ranges that start together, the longer one cut back to the shorter one's end.
    t_layer_config_ranges ranges = two_ranges();
    ranges[{ 2., 6. }] = ranges.at({ 2., 4. });
    CHECK(check_layer_range_edit(ranges, { 2., 6. }, { 2., 4. }) == LayerRangeEditCheck::Taken);
    CHECK(check_layer_range_edit(ranges, { 2., 6. }, { 2., 4. + 0.5 * EPSILON }) == LayerRangeEditCheck::Taken);
    // Min Z typed at or above the range's end moves it to [min, min + 0.5]: onto a range there already.
    ranges[{ 10., 10.5 }] = ranges.at({ 2., 4. });
    CHECK(check_layer_range_edit(ranges, { 2., 4. }, { 10., 10.5 }) == LayerRangeEditCheck::Taken);
    // The range itself is not "another range".
    CHECK(check_layer_range_edit(ranges, { 2., 4. }, { 2., 4. + 2. * EPSILON }) == LayerRangeEditCheck::Ok);
}
