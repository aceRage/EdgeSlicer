#include <catch2/catch.hpp>

#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

// What a sliced file (.gcode.3mf) says about a plate beyond its G-code: the toolhead each filament
// is reported on (slice_info.config, <filament group_id>) and the wipe tower's entry in
// Metadata/plate_N.json. Both were found wrong by the EdgeSlicerRS port on mcbrim_test.3mf plate 2.

namespace {

// A Snapmaker U1: four identical toolheads, seven filaments in the project.
DynamicPrintConfig make_u1_config(size_t filaments)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("nozzle_diameter", new ConfigOptionFloats(std::vector<double>(4, 0.4)));
    cfg.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    cfg.set_key_value("filament_colour", new ConfigOptionStrings(std::vector<std::string>(filaments, "#FFFFFF")));
    return cfg;
}

} // namespace

TEST_CASE("A sliced plate reports each filament on the toolhead the filament map gives it", "[SlicedFile][FilamentMap]")
{
    const DynamicPrintConfig u1 = make_u1_config(7);
    // slice_info.config writes filament_maps "1 2 3 4 1 2 3" for this project; group_id is that
    // map less one, whichever filaments the plate uses. mcbrim_test plate 2 uses filaments 2-7:
    // six of them, and filament 7 (id 6) came out on toolhead 0 from a map six long.
    const std::vector<int> expected = {0, 1, 2, 3, 0, 1, 2};
    for (int id = 0; id < 7; ++id) {
        DYNAMIC_SECTION("filament " << id + 1) {
            CHECK(identity_filament_group(u1, id) == expected[size_t(id)]);
        }
    }
    CHECK(identity_filament_group(u1, 6) == identity_filament_map(u1, 7)[6] - 1);

    // Outside the map: nothing to say, toolhead 0 as before.
    CHECK(identity_filament_group(u1, -1) == 0);
    CHECK(identity_filament_group(u1, 7) == 0);

    // Not an identical multi-toolhead machine: 0 for every filament (the nozzle group result, where
    // there is one, then takes over).
    DynamicPrintConfig single = make_u1_config(7);
    single.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));
    CHECK(identity_filament_group(single, 6) == 0);
    CHECK(identity_filament_group(DynamicPrintConfig(), 2) == 0);
}

TEST_CASE("The wipe tower's plate_N.json entry is its own", "[SlicedFile]")
{
    // A rib tower's first-layer outline, in bed coordinates (scaled), its plate at (300, 0): the
    // points go round a plus shape, so points 0 and 2 are not opposite corners of its box.
    const Vec2d  origin(300., 0.);
    const Points outline = {
        Point::new_scale(340., 210.), Point::new_scale(350., 200.), Point::new_scale(360., 210.),
        Point::new_scale(370., 220.), Point::new_scale(360., 230.), Point::new_scale(350., 240.),
        Point::new_scale(340., 230.), Point::new_scale(330., 220.),
    };

    const BBoxData data = wipe_tower_first_layer_bbox_data(outline, origin, 1, 0.2f);
    CHECK(data.name == "wipe_tower");
    CHECK(data.id == 1001);
    // Not an object: no area of its own, and none taken from the last object (827.02 mm² on
    // mcbrim_test plate 2).
    CHECK(data.area == 0.f);
    CHECK(data.layer_height == 0.2f);
    // The box round the whole outline, plate-relative: 30..70 x 200..240. Points 0 and 2 alone
    // gave 40..60 x 210..210.
    REQUIRE(data.bbox.size() == 4);
    CHECK_THAT(data.bbox[0], Catch::Matchers::WithinAbs(30., 1e-6));
    CHECK_THAT(data.bbox[1], Catch::Matchers::WithinAbs(200., 1e-6));
    CHECK_THAT(data.bbox[2], Catch::Matchers::WithinAbs(70., 1e-6));
    CHECK_THAT(data.bbox[3], Catch::Matchers::WithinAbs(240., 1e-6));

    // A plain rectangle, as the non-rib tower's first four points are, gives the same box as before.
    const Points rect = {Point::new_scale(310., 20.), Point::new_scale(340., 20.), Point::new_scale(340., 50.),
                         Point::new_scale(310., 50.)};
    const BBoxData r = wipe_tower_first_layer_bbox_data(rect, origin, 0, 0.3f);
    CHECK(r.id == 1000);
    REQUIRE(r.bbox.size() == 4);
    CHECK_THAT(r.bbox[0], Catch::Matchers::WithinAbs(10., 1e-6));
    CHECK_THAT(r.bbox[1], Catch::Matchers::WithinAbs(20., 1e-6));
    CHECK_THAT(r.bbox[2], Catch::Matchers::WithinAbs(40., 1e-6));
    CHECK_THAT(r.bbox[3], Catch::Matchers::WithinAbs(50., 1e-6));

    // No outline: no box, still zero rather than whatever the memory held.
    const BBoxData none = wipe_tower_first_layer_bbox_data({}, origin, 0, 0.2f);
    CHECK(none.bbox.empty());
    CHECK(none.area == 0.f);
}

TEST_CASE("A BBoxData not filled in is zero", "[SlicedFile]")
{
    const BBoxData data;
    CHECK(data.id == 0);
    CHECK(data.area == 0.f);
    CHECK(data.layer_height == 0.f);
}
