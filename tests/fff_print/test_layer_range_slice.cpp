// Height ranges (ModelObject::layer_config_ranges) as the slicer reads them.

#include <catch2/catch.hpp>

#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

void one_cube(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "cube.stl";
    object->add_volume(mesh(TestMesh::cube_20x20x20));
    object->add_instance()->set_offset(Vec3d(80., 80., 0.));
    object->ensure_on_bed();
}

} // namespace

// A 3MF written by another program, or by an older version, can hold a height range without a layer_height: the
// reader takes the range's options as they are. The range then prints at the object's layer height.
TEST_CASE("Height range without a layer height slices at the object's layer height", "[LayerRanges]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "layer_height", "0.2" }, { "initial_layer_print_height", "0.2" }, { "wall_loops", "2" } });

    Model plain;
    one_cube(plain);
    Model ranged;
    one_cube(ranged);
    DynamicPrintConfig range;
    range.set_key_value("wall_loops", new ConfigOptionInt(4));
    ranged.objects.front()->layer_config_ranges[{ 4., 8. }].assign_config(std::move(range));
    REQUIRE_FALSE(ranged.objects.front()->layer_config_ranges.begin()->second.has("layer_height"));

    Print print_plain;
    print_plain.apply(plain, config);
    REQUIRE(print_plain.objects().size() == 1);
    const SlicingParameters sp = print_plain.objects().front()->slicing_parameters();
    REQUIRE(sp.valid);

    SECTION("the height profile is the object's own") {
        const std::vector<coordf_t> without = layer_height_profile_from_ranges(sp, t_layer_config_ranges());
        const std::vector<coordf_t> with    = layer_height_profile_from_ranges(sp, ranged.objects.front()->layer_config_ranges);
        CHECK(with == without);
    }

    SECTION("slicing gives the same layers, the range's own option applied") {
        print_plain.set_status_silent();
        print_plain.process();

        Print print;
        print.apply(ranged, config);
        print.set_status_silent();
        print.process();
        REQUIRE(print.objects().size() == 1);
        const PrintObject &object = *print.objects().front();
        const PrintObject &plain_object = *print_plain.objects().front();
        REQUIRE(object.layer_count() == plain_object.layer_count());
        REQUIRE(object.layer_count() == 100);
        for (size_t i = 0; i < object.layer_count(); ++i)
            CHECK_THAT(object.get_layer(int(i))->print_z, Catch::Matchers::WithinAbs(plain_object.get_layer(int(i))->print_z, EPSILON));
        // The range's own setting still reaches its layers (it has a region of its own there).
        bool four_walls_in_range = false;
        for (size_t i = 0; i < object.layer_count(); ++i) {
            const Layer *layer = object.get_layer(int(i));
            if (layer->print_z > 4. + EPSILON && layer->print_z < 8. - EPSILON)
                for (const LayerRegion *region : layer->regions())
                    four_walls_in_range = four_walls_in_range || region->region().config().wall_loops.value == 4;
        }
        CHECK(four_walls_in_range);

        // Applying the same model again changes nothing: the range is not taken as edited each time.
        CHECK(print.apply(ranged, config) == Print::APPLY_STATUS_UNCHANGED);
    }
}
