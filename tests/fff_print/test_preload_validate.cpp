#include <catch2/catch.hpp>

#include <string>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// "Preload all filaments" loads every filament the first layer does not print and purges it into the prime
// tower, so Print::validate() refuses it without a tower. normalize_fdm_2 (PrintApply) turns the tower off for a
// plate printed in one filament, and the check then refused every such plate, asking the user to enable the
// prime tower they had enabled. With one filament there is nothing to preload: such a plate is not refused.

namespace {

const std::string preload_refusal = "\"Preload all filaments\" requires the prime tower";

StringObjectException validate(bool prime_tower, bool two_filaments)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "preload_all_filaments", "1" },
        { "enable_prime_tower",    prime_tower ? "1" : "0" },
        { "enable_support",        "0" },
        { "skirt_loops",           "0" },
        { "brim_type",             "no_brim" },
        // full_print_config() has relative E, which validate() refuses without a per-layer reset.
        { "layer_change_gcode",    "G92 E0" },
    });
    if (two_filaments)
        config.set_deserialize_strict({ { "filament_diameter", "1.75,1.75" }, { "filament_colour", "#FF0000,#00FF00" } });
    Model model = Test::model("cube", Test::mesh(TestMesh::cube_20x20x20));
    if (two_filaments)
        model.add_object(*model.objects.front());
    double x = 80.;
    for (ModelObject *object : model.objects) {
        object->instances.front()->set_offset(Vec3d(x, 100., 0.));
        object->ensure_on_bed();
        x += 40.;
    }
    if (two_filaments)
        model.objects.back()->config.set("extruder", 2);
    Print print;
    for (ModelObject *object : model.objects)
        print.auto_assign_extruders(object);
    print.apply(model, config);
    return print.validate();
}

} // namespace

TEST_CASE("Preload all filaments does not refuse a plate printed in one filament", "[Print][Validate][Preload]")
{
    const bool prime_tower = GENERATE(true, false);
    CAPTURE(prime_tower);
    const StringObjectException err = validate(prime_tower, false);
    INFO(err.string << " (" << err.opt_key << ")");
    CHECK(err.string.find(preload_refusal) == std::string::npos);
}

TEST_CASE("Preload all filaments still needs the prime tower with two filaments", "[Print][Validate][Preload]")
{
    const StringObjectException err = validate(false, true);
    INFO(err.string << " (" << err.opt_key << ")");
    CHECK(err.string.find(preload_refusal) == 0);
    CHECK(err.opt_key == "enable_prime_tower");
}
