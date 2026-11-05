#include <catch2/catch.hpp>

#include <cmath>
#include <cstddef>
#include <string>

#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/libslic3r.h"

#include "test_data.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// make_cube() facets: {2,3} = top cap, {0,1} = bottom cap.
const std::vector<int> TOP_CAP    = {2, 3};
const std::vector<int> BOTTOM_CAP = {0, 1};

DynamicPrintConfig painted_cube_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FFFFFF", "#804020", "#208040"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4, 0.4};
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->percent = false;
    config.option<ConfigOptionFloat>("layer_height")->value               = 0.2;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.2;
    config.option<ConfigOptionInt>("top_shell_layers")->value             = 5;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value          = 4;
    return config;
}

// Paints the top face of `volume` with filament 2 and its bottom face with filament 3.
void paint_top_and_bottom(ModelVolume &volume)
{
    TriangleSelector selector(volume.mesh());
    for (int i : TOP_CAP)
        selector.set_facet(i, EnforcerBlockerType::Extruder2);
    for (int i : BOTTOM_CAP)
        selector.set_facet(i, EnforcerBlockerType::Extruder3);
    REQUIRE(volume.mmu_segmentation_facets.set(selector));
}

size_t layers_with_filament(const PrintObject &object, int filament_id)
{
    size_t count = 0;
    for (const Layer *layer : object.layers())
        for (const LayerRegion *region : layer->regions())
            if (region->region().config().wall_filament.value == filament_id && !region->slices.empty()) {
                ++count;
                break;
            }
    return count;
}

PrintObject *slice_painted_cube(Print &print, Model &model, const DynamicPrintConfig &config)
{
    ModelObject *object = model.add_object();
    object->name        = "mm-segmentation.stl";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 4.));
    object->add_instance();
    object->ensure_on_bed();
    paint_top_and_bottom(*volume);
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    PrintObject *po = print.objects_mutable().front();
    po->slice();
    REQUIRE(po->layers().size() == 20);
    return po;
}

} // namespace

TEST_CASE("Small painted feature options keep the previous behavior by default", "[MultiMaterialSegmentation]")
{
    const DynamicPrintConfig defaults = DynamicPrintConfig::full_print_config();
    CHECK_THAT(defaults.opt_float("mmu_segmented_region_min_area"), WithinAbs(0.01, 1e-12));
    CHECK_THAT(defaults.opt_float("mmu_segmented_region_opening_threshold"), WithinAbs(-1., 1e-12));
}

TEST_CASE("Painted top and bottom surfaces keep their shell-depth colouring at default min-area filters", "[MultiMaterialSegmentation]")
{
    DynamicPrintConfig config = painted_cube_config();
    Print              print;
    Model              model;
    const PrintObject *object = slice_painted_cube(print, model, config);
    CHECK(layers_with_filament(*object, 2) == 5);
    CHECK(layers_with_filament(*object, 3) == 4);
}

TEST_CASE("Painted surfaces are still colored when small region filtering is disabled", "[MultiMaterialSegmentation]")
{
    // 0 disables both the area filter and the morphological opening; large painted faces must be unaffected.
    DynamicPrintConfig config = painted_cube_config();
    config.option<ConfigOptionFloat>("mmu_segmented_region_min_area")->value         = 0.;
    config.option<ConfigOptionFloat>("mmu_segmented_region_opening_threshold")->value = 0.;

    Print print;
    Model model;
    const PrintObject *object = slice_painted_cube(print, model, config);
    CHECK(layers_with_filament(*object, 2) == 5);
    CHECK(layers_with_filament(*object, 3) == 4);
}
