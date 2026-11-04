#include <catch2/catch.hpp>

#include "libslic3r/AdaptiveLayerMerge.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Slic3r;

namespace {

DynamicPrintConfig mixed_diameter_config(double d0, double d1, double layer_height = 0.2)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = { d0, d1 };
    config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FFFFFF", "#804020" };
    config.option<ConfigOptionInts>("filament_map")->values        = { 1, 2 };
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionFloat>("layer_height")->value               = layer_height;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = layer_height;
    config.option<ConfigOptionFloats>("min_layer_height")->values = { 0.07, 0.07 };
    config.option<ConfigOptionFloats>("max_layer_height")->values = { 0.56, 0.56 };
    config.option<ConfigOptionBool>("enable_prime_tower")->value  = false;
    config.option<ConfigOptionBool>("enable_support")->value      = false;
    config.option<ConfigOptionInt>("skirt_loops")->value          = 0;
    config.option<ConfigOptionFloat>("elefant_foot_compensation")->value = 0.;
    return config;
}

DynamicPrintConfig single_diameter_config(double d)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(1);
    config.set_num_filaments(1);
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = { d };
    config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75 };
    // 0.07 so the uses_multiple_nozzle_diameters() early return is what fires: at 0.2 on a 0.4
    // nozzle, adaptive_layer_band_size is 1 anyway and the case would not pin the gate.
    config.option<ConfigOptionFloat>("layer_height")->value               = 0.07;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.07;
    config.option<ConfigOptionBool>("enable_prime_tower")->value = false;
    config.option<ConfigOptionBool>("enable_support")->value     = false;
    config.option<ConfigOptionInt>("skirt_loops")->value         = 0;
    return config;
}

ModelObject *add_cube(Model &model, const char *name, double x, double y, double z, int extruder)
{
    ModelObject *object = model.add_object();
    object->name        = name;
    ModelVolume *volume = object->add_volume(make_cube(x, y, z));
    volume->config.set("extruder", extruder);
    object->add_instance();
    object->ensure_on_bed();
    return object;
}

// One object, two side-by-side 20 x 10 x `height` mm volumes, filaments 1 and 2.
ModelObject *add_two_filament_block(Model &model, double height)
{
    ModelObject *object = model.add_object();
    object->name        = "mixed-cube";
    TriangleMesh left   = make_cube(20., 10., height);
    TriangleMesh right  = make_cube(20., 10., height);
    right.translate(0.f, 10.f, 0.f);
    ModelVolume *v0 = object->add_volume(left);
    v0->config.set("extruder", 1);
    ModelVolume *v1 = object->add_volume(right);
    v1->config.set("extruder", 2);
    object->add_instance();
    object->ensure_on_bed();
    return object;
}

ModelObject *add_two_filament_cube(Model &model) { return add_two_filament_block(model, 20.); }

// One volume: walls/solid on `wall_filament`, sparse infill on `infill_filament` (1-based).
ModelObject *add_wall_infill_cube(Model &model, int wall_filament, int infill_filament)
{
    ModelObject *object = model.add_object();
    object->name        = "wall-infill";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
    volume->config.set("extruder", wall_filament);
    volume->config.set("sparse_infill_filament", infill_filament);
    object->add_instance();
    object->ensure_on_bed();
    return object;
}

// Two side-by-side volumes. The first prints walls with `wall_on_filament`. The second has walls
// off (`wall_loops` 0) but still names `walls_off_filament` as wall_filament, so an ungated
// collector would pick it up.
ModelObject *add_walls_off_second_volume(Model &model, int wall_on_filament, int walls_off_filament)
{
    ModelObject *object = model.add_object();
    object->name        = "walls-off";
    TriangleMesh left   = make_cube(20., 10., 20.);
    TriangleMesh right  = make_cube(20., 10., 20.);
    right.translate(0.f, 10.f, 0.f);
    ModelVolume *v0 = object->add_volume(left);
    v0->config.set("extruder", wall_on_filament);
    ModelVolume *v1 = object->add_volume(right);
    v1->config.set("extruder", walls_off_filament);
    v1->config.set("wall_loops", 0);
    v1->config.set("wall_filament", walls_off_filament);
    object->add_instance();
    object->ensure_on_bed();
    return object;
}

void apply_print(Print &print, Model &model, const DynamicPrintConfig &config)
{
    print.set_status_silent();
    print.apply(model, config);
}

double region_nozzle(const Print &print, const PrintObject &object, size_t region_id)
{
    return nozzle_diameter_for_filament(print.config(),
                                        (unsigned int) object.printing_region(region_id).config().wall_filament.value);
}

struct PatchSnap
{
    size_t region_id { 0 };
    size_t layer_id { 0 };
    size_t bottom_idx { 0 };
    size_t top_idx { 0 };
    bool   group_top { false };
    double merged_height { 0. };
    double area { 0. };

    bool operator==(const PatchSnap &rhs) const
    {
        return region_id == rhs.region_id && layer_id == rhs.layer_id && bottom_idx == rhs.bottom_idx &&
               top_idx == rhs.top_idx && group_top == rhs.group_top &&
               std::abs(merged_height - rhs.merged_height) < EPSILON && std::abs(area - rhs.area) < 1.;
    }
};

std::vector<PatchSnap> snapshot_patches(const PrintObject &object)
{
    std::vector<PatchSnap> out;
    for (size_t layer_id = 0; layer_id < object.layer_count(); ++ layer_id) {
        const Layer *layer = object.layers()[layer_id];
        for (size_t region_id = 0; region_id < layer->region_count(); ++ region_id) {
            for (const AdaptiveMergePatch &patch : layer->get_region(int(region_id))->adaptive_merge_patches) {
                PatchSnap snap;
                snap.region_id     = region_id;
                snap.layer_id      = layer_id;
                snap.bottom_idx    = patch.bottom_idx;
                snap.top_idx       = patch.top_idx;
                snap.group_top     = patch.group_top;
                snap.merged_height = patch.merged_height;
                snap.area          = ::Slic3r::area(patch.area());
                out.emplace_back(snap);
            }
        }
    }
    return out;
}

} // namespace

TEST_CASE("Adaptive region merge: mixed 0.4/0.8 cube bands the coarse region only", "[AdaptiveRegionMerge]")
{
    Model model;
    add_two_filament_cube(model);
    Print print;
    apply_print(print, model, mixed_diameter_config(0.4, 0.8));
    REQUIRE(print.objects().size() == 1);
    REQUIRE(print.uses_multiple_nozzle_diameters());

    PrintObject *object = print.objects_mutable().front();
    object->slice();
    REQUIRE(object->layer_count() >= 2);
    REQUIRE(object->num_printing_regions() >= 2);

    AdaptiveMergePlanAccess::compute(*object);

    const double layer_height = object->layers().front()->height;
    const size_t expected_band = adaptive_layer_band_size(0.8, 0.4, layer_height);
    REQUIRE(expected_band >= 2);
    const AdaptiveLayerHeightRange window08 = adaptive_layer_height_range(0.8);

    std::vector<double> heights;
    heights.reserve(object->layer_count());
    for (const Layer *layer : object->layers())
        heights.emplace_back(layer->height);
    const std::vector<AdaptiveLayerBand> expected_bands =
        build_adaptive_layer_bands(heights, expected_band, window08);

    size_t fine_region   = size_t(-1);
    size_t coarse_region = size_t(-1);
    for (size_t region_id = 0; region_id < object->num_printing_regions(); ++ region_id) {
        const double d = region_nozzle(print, *object, region_id);
        if (std::abs(d - 0.4) < 0.05)
            fine_region = region_id;
        else if (std::abs(d - 0.8) < 0.05)
            coarse_region = region_id;
    }
    REQUIRE(fine_region != size_t(-1));
    REQUIRE(coarse_region != size_t(-1));

    size_t fine_patches   = 0;
    size_t coarse_group_tops = 0;
    std::vector<size_t> coarse_tops;
    for (size_t layer_id = 0; layer_id < object->layer_count(); ++ layer_id) {
        const Layer *layer = object->layers()[layer_id];
        REQUIRE(layer->region_count() == object->num_printing_regions());

        const LayerRegion *fine   = layer->get_region(int(fine_region));
        const LayerRegion *coarse = layer->get_region(int(coarse_region));
        fine_patches += fine->adaptive_merge_patches.size();
        for (const AdaptiveMergePatch &patch : coarse->adaptive_merge_patches) {
            if (! patch.group_top)
                continue;
            ++ coarse_group_tops;
            coarse_tops.emplace_back(patch.top_idx);
            CHECK(window08.contains(patch.merged_height));
        }

        std::vector<ExPolygons> areas;
        for (const LayerRegion *layerm : layer->regions())
            for (const AdaptiveMergePatch &patch : layerm->adaptive_merge_patches)
                if (! patch.area().empty())
                    areas.emplace_back(patch.area());
        for (size_t i = 0; i < areas.size(); ++ i)
            for (size_t j = i + 1; j < areas.size(); ++ j)
                CHECK(::Slic3r::area(intersection_ex(areas[i], areas[j])) == Approx(0.).margin(1.));
    }

    CHECK(fine_patches == 0);
    REQUIRE(coarse_group_tops > 0);
    REQUIRE_FALSE(expected_bands.empty());
    for (const AdaptiveLayerBand &band : expected_bands) {
        const bool found = std::find(coarse_tops.begin(), coarse_tops.end(), band.top_idx) != coarse_tops.end();
        CHECK(found);
    }
}

TEST_CASE("Adaptive region merge: a single-diameter plate produces no patches", "[AdaptiveRegionMerge]")
{
    Model model;
    add_cube(model, "single", 20., 20., 20., 1);
    Print print;
    apply_print(print, model, single_diameter_config(0.4));
    REQUIRE_FALSE(print.uses_multiple_nozzle_diameters());

    PrintObject *object = print.objects_mutable().front();
    object->slice();
    REQUIRE(object->layer_count() >= 2);
    AdaptiveMergePlanAccess::compute(*object);
    CHECK(snapshot_patches(*object).empty());
}

TEST_CASE("Adaptive region merge: zero layers and one layer do not crash", "[AdaptiveRegionMerge]")
{
    // Unsliced objects have m_slicing_params.valid == false. The F11 guard returns before reading
    // layer_height, so this is safe in release builds too, not merely loud under assert.
    SECTION("zero layers: an unsliced object plans nothing") {
        Model model;
        add_two_filament_cube(model);
        Print print;
        apply_print(print, model, mixed_diameter_config(0.4, 0.8));
        REQUIRE(print.objects().size() == 1);
        PrintObject *object = print.objects_mutable().front();
        REQUIRE(object->layer_count() == 0);
        AdaptiveMergePlanAccess::compute(*object);
        CHECK(snapshot_patches(*object).empty());
    }

    SECTION("one layer: sliced, no bands possible") {
        Model model;
        add_two_filament_block(model, 0.2);
        Print print;
        apply_print(print, model, mixed_diameter_config(0.4, 0.8));
        PrintObject *object = print.objects_mutable().front();
        object->slice();
        REQUIRE(object->layer_count() >= 1);
        REQUIRE(object->layer_count() <= 1);
        AdaptiveMergePlanAccess::compute(*object);
        CHECK(snapshot_patches(*object).empty());
    }
}

TEST_CASE("Adaptive region merge: uses_multiple_nozzle_diameters true/false pair", "[AdaptiveRegionMerge]")
{
    {
        Model model;
        add_two_filament_cube(model);
        Print print;
        apply_print(print, model, mixed_diameter_config(0.4, 0.8));
        CHECK(print.uses_multiple_nozzle_diameters());
    }
    {
        Model model;
        add_cube(model, "single", 20., 20., 20., 1);
        Print print;
        apply_print(print, model, single_diameter_config(0.4));
        CHECK_FALSE(print.uses_multiple_nozzle_diameters());
    }
}

TEST_CASE("Adaptive region merge: planner is idempotent on the same sliced input", "[AdaptiveRegionMerge]")
{
    Model model;
    add_two_filament_cube(model);
    Print print;
    apply_print(print, model, mixed_diameter_config(0.4, 0.8));
    PrintObject *object = print.objects_mutable().front();
    object->slice();
    AdaptiveMergePlanAccess::compute(*object);
    const std::vector<PatchSnap> first = snapshot_patches(*object);
    REQUIRE_FALSE(first.empty());
    AdaptiveMergePlanAccess::compute(*object);
    CHECK(snapshot_patches(*object) == first);
}

TEST_CASE("Adaptive region merge: an object whose own nozzle is the reference never bands", "[AdaptiveRegionMerge]")
{
    CHECK(adaptive_layer_band_size(0.4, 0.4, 0.07) == 1);
    CHECK(adaptive_layer_band_size(0.8, 0.4, 0.07) >= 2);

    Model model;
    ModelObject *fine   = add_cube(model, "fine-only", 20., 20., 20., 1);
    ModelObject *coarse = add_cube(model, "coarse-mate", 20., 20., 20., 2);
    coarse->instances[0]->set_offset(Vec3d(40., 0., 0.));
    Print print;
    apply_print(print, model, mixed_diameter_config(0.4, 0.8, 0.07));
    REQUIRE(print.objects().size() == 2);
    REQUIRE(print.uses_multiple_nozzle_diameters());

    PrintObject *fine_object = nullptr;
    for (PrintObject *obj : print.objects_mutable())
        if (obj->model_object() == fine)
            fine_object = obj;
    REQUIRE(fine_object != nullptr);
    fine_object->slice();
    REQUIRE(fine_object->layer_count() >= 2);
    AdaptiveMergePlanAccess::compute(*fine_object);
    CHECK(snapshot_patches(*fine_object).empty());
}

TEST_CASE("Adaptive region merge: a finer infill-only nozzle does not size the walls", "[AdaptiveRegionMerge]")
{
    Model model;
    add_wall_infill_cube(model, 1, 2);
    Print print;
    apply_print(print, model, mixed_diameter_config(0.4, 0.2, 0.07));
    REQUIRE(print.objects().size() == 1);
    REQUIRE(print.uses_multiple_nozzle_diameters());

    PrintObject *object = print.objects_mutable().front();
    object->slice();
    REQUIRE(object->layer_count() >= 2);

    bool saw_split = false;
    for (size_t region_id = 0; region_id < object->num_printing_regions(); ++ region_id) {
        const PrintRegionConfig &config = object->printing_region(region_id).config();
        if (config.wall_filament.value == 1 && config.sparse_infill_filament.value == 2)
            saw_split = true;
    }
    REQUIRE(saw_split);

    AdaptiveMergePlanAccess::compute(*object);
    CHECK(adaptive_layer_band_size(0.4, 0.2, 0.07) >= 2);
    CHECK(adaptive_layer_band_size(0.4, 0.4, 0.07) == 1);
    CHECK(snapshot_patches(*object).empty());
}

TEST_CASE("Adaptive region merge: a walls-off region does not size the band", "[AdaptiveRegionMerge]")
{
    Model model;
    add_walls_off_second_volume(model, 1, 2);
    Print print;
    apply_print(print, model, mixed_diameter_config(0.4, 0.2, 0.07));
    REQUIRE(print.objects().size() == 1);
    REQUIRE(print.uses_multiple_nozzle_diameters());

    PrintObject *object = print.objects_mutable().front();
    object->slice();
    REQUIRE(object->layer_count() >= 2);

    bool saw_wall_04  = false;
    bool saw_gated_02 = false;
    for (size_t region_id = 0; region_id < object->num_printing_regions(); ++ region_id) {
        const PrintRegionConfig &config = object->printing_region(region_id).config();
        if (config.wall_loops.value > 0 && config.wall_filament.value == 1)
            saw_wall_04 = true;
        if (config.wall_loops.value == 0 && config.wall_filament.value == 2)
            saw_gated_02 = true;
    }
    REQUIRE(saw_wall_04);
    REQUIRE(saw_gated_02);

    AdaptiveMergePlanAccess::compute(*object);
    CHECK(adaptive_layer_band_size(0.4, 0.2, 0.07) >= 2);
    CHECK(adaptive_layer_band_size(0.4, 0.4, 0.07) == 1);
    CHECK(snapshot_patches(*object).empty());
}
