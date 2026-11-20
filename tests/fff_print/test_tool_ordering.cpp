#include <catch2/catch.hpp>

// Print.hpp first: it brings in ExtrusionEntity.hpp, which ToolOrdering.hpp expects to be declared.
#include "libslic3r/Print.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include "test_data.hpp"

#include <optional>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// Count G-code tool-select lines for 0-based `tool`. Matches a line that starts with Tn, with an
// optional comment (Edge's GCodeWriter emits "T16 ; change extruder" when full_gcode_comment is on).
size_t count_tool_selects(const std::string &gcode, unsigned int tool)
{
    const std::string prefix = "T" + std::to_string(tool);
    size_t            count  = 0;
    std::string::size_type line_start = 0;
    while (line_start < gcode.size()) {
        std::string::size_type line_end = gcode.find('\n', line_start);
        if (line_end == std::string::npos)
            line_end = gcode.size();
        std::string::size_type i = line_start;
        while (i < line_end && (gcode[i] == ' ' || gcode[i] == '\t' || gcode[i] == '\r'))
            ++i;
        if (i + prefix.size() <= line_end && gcode.compare(i, prefix.size(), prefix) == 0) {
            const std::string::size_type after = i + prefix.size();
            if (after == line_end || gcode[after] == ' ' || gcode[after] == '\t' || gcode[after] == ';' || gcode[after] == '\r')
                ++count;
        }
        line_start = line_end + 1;
    }
    return count;
}

// One object made of two parts on a `num_filaments`-filament machine: a 20 x 20 x 10 mm base on
// filament 1 and a 10 x 10 x 5 mm inlay in its upper half on `inlay_filament`. The lower half of
// the object is printed with filament 1 alone, the upper half with both filaments.
void init_inlay_print(Print &print, int inlay_filament, size_t num_filaments)
{
    REQUIRE(inlay_filament >= 1);
    REQUIRE(size_t(inlay_filament) <= num_filaments);
    REQUIRE(num_filaments <= MAXIMUM_FILAMENT_NUMBER);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(unsigned(num_filaments));
    config.set_num_filaments(unsigned(num_filaments));
    config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.assign(num_filaments * num_filaments, 140.);
    config.set_deserialize_strict({
        { "enable_prime_tower",                   false },
        { "machine_start_gcode",                  "" },
        { "machine_end_gcode",                    "" },
        // Relative-E Marlin flavor for a non-BBL printer requires G92 E0 at each layer (Print::validate).
        { "layer_change_gcode",                   "G92 E0" },
        { "gcode_flavor",                         "marlin" },
        { "use_relative_e_distances",             true },
        { "single_extruder_multi_material_priming", false },
        { "skirt_loops",                          0 }
    });

    Model model;
    ModelObject *object = model.add_object();
    object->name        = "inlay.stl";

    ModelVolume *base = object->add_volume(make_cube(20., 20., 10.));
    base->config.set("extruder", 1);

    TriangleMesh inlay_mesh = make_cube(10., 10., 5.);
    inlay_mesh.translate(5.f, 5.f, 5.f);
    ModelVolume *inlay = object->add_volume(std::move(inlay_mesh));
    inlay->config.set("extruder", inlay_filament);

    object->add_instance();
    object->ensure_on_bed();

    print.set_status_silent();
    print.is_BBL_printer() = false;
    print.apply(model, config);
    const StringObjectException validation = print.validate();
    INFO("validate() error: " << validation.string);
    REQUIRE(validation.string.empty());
}

// Slices the inlay print and counts the tool selects of the inlay filament.
size_t inlay_tool_changes(int inlay_filament, size_t num_filaments)
{
    Print print;
    init_inlay_print(print, inlay_filament, num_filaments);
    const std::string gcode = Test::gcode(print);
    return count_tool_selects(gcode, unsigned(inlay_filament - 1));
}

} // namespace

TEST_CASE("flush-order cache key does not overlap filament set and previous tool", "[ToolOrdering]")
{
    const std::optional<unsigned int> none;
    const std::optional<unsigned int> prev0{0u};

    const auto key_prev0_f1     = make_flush_order_cache_key({0}, prev0);
    const auto key_prev0_f1_f17 = make_flush_order_cache_key({0, 16}, prev0);
    const auto key_prev0_f1_f33 = make_flush_order_cache_key({0, 32}, prev0);
    const auto key_prev0_f1_f64 = make_flush_order_cache_key({0, 63}, prev0);

    // The old uint32_t key with a 16-bit offset: layer {0} after tool 0 collided with {0,16} after tool 0.
    CHECK_FALSE(key_prev0_f1 == key_prev0_f1_f17);
    CHECK((key_prev0_f1 < key_prev0_f1_f17) != (key_prev0_f1_f17 < key_prev0_f1));
    // `1 << 32` was undefined on the uint32_t key; filament 33 (index 32) must stay distinct.
    CHECK_FALSE(key_prev0_f1 == key_prev0_f1_f33);
    CHECK((key_prev0_f1 < key_prev0_f1_f33) != (key_prev0_f1_f33 < key_prev0_f1));
    // Edge's MAXIMUM_FILAMENT_NUMBER is 64 (0-based 63).
    CHECK_FALSE(key_prev0_f1 == key_prev0_f1_f64);
    CHECK((key_prev0_f1 < key_prev0_f1_f64) != (key_prev0_f1_f64 < key_prev0_f1));
    CHECK_FALSE(make_flush_order_cache_key({16}, none) == make_flush_order_cache_key({}, prev0));

    CHECK(make_flush_order_cache_key({2, 0, 1}, prev0) == make_flush_order_cache_key({0, 1, 2}, prev0));
    CHECK(make_flush_order_cache_key({0}, none).initial_extruder == ~0u);
}

// Filaments 1–16 were already in the non-overlapping half of the old 32-bit key. A 16-filament
// inlay must still be selected, matching the 2-filament control (G-code below 17 filaments is
// unchanged aside from palette size).
SCENARIO("Tool ordering keeps a filament at or below 16 that joins a layer of filament 1", "[ToolOrdering]")
{
    GIVEN("16 filaments and a base on filament 1 with an inlay in its upper half")
    {
        WHEN("the inlay is on filament 2")
        {
            const size_t tool_changes_2 = inlay_tool_changes(2, 16);

            THEN("filament 2 is selected for the inlay") { REQUIRE(tool_changes_2 > 0); }

            AND_WHEN("the inlay is on filament 16")
            {
                const size_t tool_changes_16 = inlay_tool_changes(16, 16);

                THEN("filament 16 is selected as often as filament 2 was") { REQUIRE(tool_changes_16 == tool_changes_2); }
            }
        }
    }
}

// The tool order of a layer is cached under a key built from the layer's filaments and the filament
// that was active before it. The two halves of the key used to overlap from filament 17 on, so a layer
// printing filaments 1 and 17 after filament 1 took the cached order of a layer printing filament 1
// alone, and filament 17 was never printed.
SCENARIO("Tool ordering keeps a filament above 16 that joins a layer of filament 1", "[ToolOrdering]")
{
    GIVEN("17 filaments and a base on filament 1 with an inlay in its upper half")
    {
        WHEN("the inlay is on filament 16")
        {
            const size_t tool_changes_16 = inlay_tool_changes(16, 17);

            THEN("filament 16 is selected for the inlay") { REQUIRE(tool_changes_16 > 0); }

            AND_WHEN("the inlay is on filament 17")
            {
                const size_t tool_changes_17 = inlay_tool_changes(17, 17);

                THEN("filament 17 is selected as often as filament 16 was") { REQUIRE(tool_changes_17 == tool_changes_16); }
            }
        }
    }
}

// Edge allows MAXIMUM_FILAMENT_NUMBER (64) filaments. A uint32_t bit-key is undefined at id >= 32,
// so a 33-filament palette is the first count that is both >32 and still a legal Edge project.
SCENARIO("Tool ordering keeps a filament above 32 that joins a layer of filament 1", "[ToolOrdering]")
{
    GIVEN("33 filaments and a base on filament 1 with an inlay in its upper half")
    {
        WHEN("the inlay is on filament 16")
        {
            const size_t tool_changes_16 = inlay_tool_changes(16, 33);

            THEN("filament 16 is selected for the inlay") { REQUIRE(tool_changes_16 > 0); }

            AND_WHEN("the inlay is on filament 33")
            {
                const size_t tool_changes_33 = inlay_tool_changes(33, 33);

                THEN("filament 33 is selected as often as filament 16 was") { REQUIRE(tool_changes_33 == tool_changes_16); }
            }
        }
    }
}
