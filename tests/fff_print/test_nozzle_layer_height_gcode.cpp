// H3C fix (owner report 2026-11-06): on a single-extruder nozzle rack (Bambu H3C, one extruder, nine rack slots) a filament
// pinned to a 0.8 rack nozzle prints with a process of its own, and the layer height of a process CHOSEN for that nozzle sets
// the 0.8's slab height on the mixed-diameter plate (the AdaptiveLayerMerge band planner). The 0.4 keeps printing the plate's
// layer height. Without a chosen height the slab height follows the nozzle size, as before.

#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/AdaptiveLayerMerge.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"
#include "test_data.hpp"

#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

double r3(double v) { return std::round(v * 1000.) / 1000.; }

// An H3C-like printer: one 0.4 extruder whose rack holds 0.4, 0.4 and 0.8 nozzles; filament 1 prints on the mounted 0.4,
// filament 2 is pinned to the rack's 0.8. The plate's (0.4) process is at `layer`.
DynamicPrintConfig h3c_rack(const char *layer)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(1);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        { "nozzle_diameter", "0.4" },
        { "filament_diameter", "1.75,1.75" },
        { "filament_map", "1,1" },
        { "single_extruder_multi_material", "1" },
        { "enable_prime_tower", "0" },
        { "enable_support", "0" },
        { "enable_arc_fitting", "0" },
        { "slow_down_for_layer_cooling", "0,0" },
        { "filament_max_volumetric_speed", "200,200" },
        { "nozzle_temperature", "210,210" },
        { "nozzle_temperature_initial_layer", "210,210" },
        { "wall_generator", "classic" },
        { "layer_height", layer },
        { "initial_layer_print_height", layer },
        { "min_layer_height", "0.04" },
        { "max_layer_height", "0.28" },
        { "brim_type", "no_brim" },
        { "skirt_loops", "0" },
        { "elefant_foot_compensation", "0" },
        { "sparse_infill_density", "15%" },
        { "outer_wall_speed", "60" },
    });
    config.set_key_value("extruder_max_nozzle_count", new ConfigOptionIntsNullable({ 9 })); // the H3C's nine-slot rack
    config.option<ConfigOptionStrings>("extruder_rack_nozzle_diameter", true)->values = { "0.4,0.4,0.8" };
    config.option<ConfigOptionFloats>("filament_rack_nozzle_diameter", true)->values  = { 0., 0.8 };
    return config;
}

// The plate config filled with a process for each nozzle: the 0.4's (filament 1) and the 0.8 rack nozzle's (filament 2), each
// with its own outer wall speed and layer height. `chosen_08` false: the 0.8's process counts as picked for its size (no
// recorded layer height).
DynamicPrintConfig filled(const DynamicPrintConfig &plate, double h04, double h08, bool chosen_08 = true)
{
    DynamicPrintConfig p04 = plate;
    p04.set_key_value("layer_height", new ConfigOptionFloat(h04));
    p04.option<ConfigOptionFloats>("outer_wall_speed")->values = { 41. };
    DynamicPrintConfig p08 = plate;
    p08.set_key_value("layer_height", new ConfigOptionFloat(h08));
    p08.option<ConfigOptionFloats>("outer_wall_speed")->values = { 23. };

    DynamicPrintConfig tool04 = nozzle_tool_process(plate, p04, nullptr);
    set_nozzle_tool_layer_height(tool04, p04);
    DynamicPrintConfig tool08 = nozzle_tool_process(plate, p08, nullptr);
    if (chosen_08)
        set_nozzle_tool_layer_height(tool08, p08);

    DynamicPrintConfig                            out = plate;
    const std::vector<const DynamicPrintConfig *> filament_sources{ &tool04, &tool08 };
    REQUIRE(fill_nozzle_process_slots(out, { &tool04 }, &filament_sources));
    return out;
}

void add_cubes(Model &model)
{
    for (int filament = 1; filament <= 2; ++filament) {
        ModelObject *object = model.add_object();
        object->name        = "cube-" + std::to_string(filament);
        object->add_volume(TriangleMesh(its_make_cube(6., 6., 6.)));
        object->add_instance()->set_offset(Vec3d(40. + 40. * filament, 60., 0.));
        object->ensure_on_bed();
        object->volumes.front()->config.set("extruder", filament);
    }
}

void add_heights(const ExtrusionEntity &entity, std::set<double> &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            add_heights(*child, out);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        if (path->role() != erBridgeInfill && path->role() != erInternalBridgeInfill)
            out.insert(r3(path->height));
    } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const ExtrusionPath &p : multi->paths)
            out.insert(r3(p.height));
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const ExtrusionPath &p : loop->paths)
            out.insert(r3(p.height));
    }
}

// The heights an object's walls and infill are laid at, over all its layers above the first.
std::set<double> object_heights(const PrintObject &object)
{
    std::set<double> out;
    for (size_t idx = 1; idx < object.layers().size(); ++idx)
        for (const LayerRegion *layerm : object.layers()[idx]->regions()) {
            for (const ExtrusionEntity *e : layerm->perimeters.entities)
                add_heights(*e, out);
            for (const ExtrusionEntity *e : layerm->fills.entities)
                add_heights(*e, out);
        }
    return out;
}

const PrintObject *object_named(const Print &print, const std::string &name)
{
    for (const PrintObject *object : print.objects())
        if (object->model_object()->name == name)
            return object;
    return nullptr;
}

struct Sliced
{
    std::set<double> fine, coarse;
    std::string      gcode;
};

Sliced slice(const DynamicPrintConfig &config)
{
    Model model;
    add_cubes(model);
    Print print;
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    REQUIRE(print.uses_multiple_nozzle_diameters());
    Sliced out;
    out.gcode                 = Test::gcode(print);
    const PrintObject *fine   = object_named(print, "cube-1");
    const PrintObject *coarse = object_named(print, "cube-2");
    REQUIRE(fine != nullptr);
    REQUIRE(coarse != nullptr);
    out.fine   = object_heights(*fine);
    out.coarse = object_heights(*coarse);
    return out;
}

} // namespace

TEST_CASE("H3C-like single-extruder rack: the 0.8 rack nozzle is a tool with a process of its own", "[NozzleProcess][NozzleRackSlots][H3C]")
{
    const DynamicPrintConfig plate = h3c_rack("0.1");
    CHECK(supports_nozzle_processes(plate));
    CHECK(nozzle_filament_group_id(plate, 1).empty());
    CHECK(nozzle_filament_group_id(plate, 2) == "T1:0.8");
    // The H3C's filaments are put on rack sizes with the rack pins, not with a filament -> tool assignment.
    CHECK_FALSE(supports_filament_nozzle_assignment(plate));

    const DynamicPrintConfig config = filled(plate, 0.1, 0.4);
    const auto              *heights = config.option<ConfigOptionFloatsOrPercents>("nozzle_layer_height");
    REQUIRE(heights != nullptr);
    REQUIRE(heights->values.size() == 2);
    CHECK(heights->values[0].value == 0.);
    CHECK(heights->values[1].value == Approx(0.4));
    CHECK(nozzle_layer_height_for_filament(config, 1) == 0.);
    CHECK(nozzle_layer_height_for_filament(config, 2) == Approx(0.4));
    // Each filament reads its own nozzle's process: the 0.4's speed on filament 1, the 0.8's on filament 2.
    CHECK(nozzle_process_value(config, "outer_wall_speed", 0, false) == "41");
}

TEST_CASE("H3C-like rack, 0.4 + 0.8 with their own processes: each object slices at its nozzle's layer height", "[NozzleProcess][H3C][GCode]")
{
    // The plate (and the 0.4's process) at 0.1, the 0.8's process at 0.4: the 0.8 prints 0.4 mm slabs (the size ratio would
    // have given 0.2), the 0.4 every 0.1 layer.
    const Sliced chosen = slice(filled(h3c_rack("0.1"), 0.1, 0.4));
    CHECK(chosen.fine == std::set<double>{ 0.1 });
    CHECK(chosen.coarse.count(0.4) == 1);
    CHECK(chosen.coarse.count(0.2) == 0);
    for (double h : chosen.coarse)
        CHECK(h <= 0.4 + 1e-9);
    // The 0.8's outer wall prints at its process's speed.
    CHECK(chosen.gcode.find("F1380") != std::string::npos);

    SECTION("a 0.3 mm process: three 0.1 layers per slab")
    {
        const Sliced s = slice(filled(h3c_rack("0.1"), 0.1, 0.3));
        CHECK(s.fine == std::set<double>{ 0.1 });
        CHECK(s.coarse.count(0.3) == 1);
        CHECK(s.coarse.count(0.4) == 0);
    }
    SECTION("a process picked for its size (no chosen height): the slab height follows the size ratio, 0.8 / 0.4 = 2 layers")
    {
        const Sliced s = slice(filled(h3c_rack("0.1"), 0.1, 0.4, false));
        CHECK(s.fine == std::set<double>{ 0.1 });
        CHECK(s.coarse.count(0.2) == 1);
        CHECK(s.coarse.count(0.4) == 0);
    }
    SECTION("a chosen height thinner than two plate layers: the 0.8 prints every layer")
    {
        const Sliced s = slice(filled(h3c_rack("0.2"), 0.2, 0.24));
        CHECK(s.fine == std::set<double>{ 0.2 });
        CHECK(s.coarse == std::set<double>{ 0.2 });
    }
    SECTION("a chosen height above the 0.8's printable window is held to it (0.56 = 0.7 x 0.8)")
    {
        const Sliced s = slice(filled(h3c_rack("0.2"), 0.2, 0.8));
        CHECK(s.coarse.count(0.4) == 1);
        for (double h : s.coarse)
            CHECK(h <= 0.56 + 1e-9);
    }
}

TEST_CASE("Without per-nozzle processes the H3C-like rack plate slices as before and the config block has no new key", "[NozzleProcess][H3C][GCode][Identity]")
{
    const DynamicPrintConfig plate = h3c_rack("0.2");
    DynamicPrintConfig       plain = plate;
    CHECK_FALSE(fill_nozzle_process_slots(plain, { nullptr }, nullptr));
    CHECK(plain.option<ConfigOptionFloatsOrPercents>("nozzle_layer_height")->values.empty());
    const Sliced s = slice(plain);
    CHECK(s.fine == std::set<double>{ 0.2 });
    CHECK(s.coarse.count(0.4) == 1); // the size ratio: two 0.2 layers per 0.8 slab
    CHECK(s.gcode.find("; nozzle_layer_height") == std::string::npos);

    const Sliced chosen = slice(filled(plate, 0.2, 0.4));
    CHECK(chosen.gcode.find("; nozzle_layer_height") == std::string::npos);
}
