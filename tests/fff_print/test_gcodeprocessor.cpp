#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

// Bambu firmware uses the " FEATURE: " style reserved tags, everything else the Slic3r-compatible
// "TYPE:" style, so which list applies depends on the printer kind passed in.
TEST_CASE("Reserved keyword detection follows the printer kind it is given", "[GCodeProcessor]")
{
    struct Case
    {
        const char *name;
        std::string gcode;
        bool        reserved_on_bbl;
        bool        reserved_on_non_bbl;
    };

    // Loop + DYNAMIC_SECTION rather than GENERATE(values<Case>): Catch2 v2 has no
    // StringMaker for the aggregate, and the project's test guide forbids reused SECTION names.
    const Case cases[] = {
        {"compatible feature tag", ";TYPE:Prime tower", false, true},
        {"compatible layer tag", ";LAYER_CHANGE", false, true},
        {"bbl feature tag", "; FEATURE: Outer wall", true, false},
        {"tag shared by both lists", ";_GP_FIRST_LINE_M73_PLACEHOLDER", true, true},
        {"bbl spells this one with a leading space", ";COLOR_CHANGE", false, true},
        {"ordinary comment", "; heat the bed", false, false},
        {"not a comment at all", "G1 X10 Y10 F3000", false, false},
        // A tag counts only as the whole comment's prefix, so neither a tag mentioned mid-comment
        // nor one trailing a real command is a reserved use.
        {"tag text later in the comment", "; the TYPE:Prime tower marker", false, false},
        {"tag trailing a command", "G1 X10 ;TYPE:Prime tower", false, false},
    };

    for (const auto &test_case : cases) {
        DYNAMIC_SECTION(test_case.name)
        {
            std::vector<std::string> tags;
            REQUIRE(GCodeProcessor::contains_reserved_tags(test_case.gcode, 5, tags, true) == test_case.reserved_on_bbl);

            tags.clear();
            REQUIRE(GCodeProcessor::contains_reserved_tags(test_case.gcode, 5, tags, false) == test_case.reserved_on_non_bbl);
        }
    }
}

TEST_CASE("Reserved keyword detection reports every offending line", "[GCodeProcessor]")
{
    const std::string gcode = ";TYPE:Prime tower\nG1 X10\n;LAYER_CHANGE\n";

    std::vector<std::string> tags;
    REQUIRE(GCodeProcessor::contains_reserved_tags(gcode, 5, tags, false));
    REQUIRE(tags.size() == 2);
    // Reported in the order they appear, which is what makes the max_count cut-off meaningful.
    CHECK(tags[0] == "TYPE:Prime tower");
    CHECK(tags[1] == "LAYER_CHANGE");

    SECTION("the reported count is capped at max_count")
    {
        tags.clear();
        REQUIRE(GCodeProcessor::contains_reserved_tags(gcode, 1, tags, false));
        CHECK(tags.size() == 1);
        CHECK(tags[0] == "TYPE:Prime tower");
    }

    SECTION("a max_count of zero still reports the first tag")
    {
        tags.clear();
        REQUIRE(GCodeProcessor::contains_reserved_tags(gcode, 0, tags, false));
        CHECK(tags.size() == 1);
    }

    SECTION("g-code with nothing reserved in it reports nothing")
    {
        tags.clear();
        CHECK_FALSE(GCodeProcessor::contains_reserved_tags("G28\n; home all axes\n", 5, tags, false));
        CHECK(tags.empty());
    }
}

TEST_CASE("GCodeProcessorResult move assignment transfers members the copy assignment used to miss", "[GCodeProcessor]")
{
    GCodeProcessorResult src;
    src.filename = "export.gcode";
    src.backtrace_enabled = true;
    src.support_traditional_timelapse = false;
    src.moves.emplace_back();
    GCodeProcessorResult dst;
    dst = std::move(src);
    REQUIRE(dst.filename == "export.gcode");
    REQUIRE(dst.backtrace_enabled);
    REQUIRE_FALSE(dst.support_traditional_timelapse);
    REQUIRE(dst.moves.size() == 1);
}

TEST_CASE("GCodeProcessorResult copy assignment keeps the source and carries the full member set", "[GCodeProcessor]")
{
    GCodeProcessorResult src;
    src.filename = "export.gcode";
    src.backtrace_enabled = true;
    src.nozzle_hrc = 55;
    src.required_nozzle_HRC = {0, 3};
    src.filament_vitrification_temperature = {45, 60};
    src.support_traditional_timelapse = false;
    src.moves.emplace_back();
    GCodeProcessorResult dst;
    dst = src;
    REQUIRE(dst.filename == "export.gcode");
    REQUIRE(dst.backtrace_enabled);
    REQUIRE(dst.nozzle_hrc == 55);
    REQUIRE(dst.required_nozzle_HRC == std::vector<int>({0, 3}));
    REQUIRE(dst.filament_vitrification_temperature == std::vector<int>({45, 60}));
    REQUIRE_FALSE(dst.support_traditional_timelapse);
    REQUIRE(dst.moves.size() == 1);
    // The copy leaves the source intact.
    REQUIRE(src.moves.size() == 1);
    REQUIRE(src.filename == "export.gcode");
}

TEST_CASE("post-process lines_ends match newline offsets", "[GCodeProcessor]")
{
    auto make_config = [](bool by_time) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_num_extruders(2);
        config.set_num_filaments(2);
        config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
        config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
        config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
        config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
        config.option<ConfigOptionFloat>("preheat_time")->value = by_time ? 30. : 0.;
        // Orca #11791: the processor's backtracked preheat also needs ooze prevention.
        config.option<ConfigOptionBool>("ooze_prevention")->value = true;
        config.option<ConfigOptionBool>("gcode_comments")->value = true;
        config.set_deserialize_strict({{"brim_type", "no_brim"},
                                       {"skirt_loops", "0"},
                                       {"layer_height", "0.2"},
                                       {"initial_layer_print_height", "0.2"},
                                       {"gcode_flavor", "marlin"}});
        return config;
    };

    auto run_case = [](DynamicPrintConfig config, bool by_time) {
        Print print;
        Model model;
        init_print({make_cube(40., 40., 20.), make_cube(40., 40., 20.)}, print, model, config, false);
        REQUIRE(model.objects.size() == 2);
        model.objects[0]->config.set("extruder", 1);
        model.objects[1]->config.set("extruder", 2);
        print.apply(model, config);
        print.is_BBL_printer() = false;

        GCodeProcessorResult result;
        const std::string    exported = Test::gcode(print, result);
        REQUIRE((exported.find("preheat T") != std::string::npos) == by_time);
        REQUIRE(exported.size() > GCodeProcessor::Output_Block_Size);

        std::vector<size_t> newline_ends;
        for (size_t i = exported.find('\n'); i != std::string::npos; i = exported.find('\n', i + 1))
            newline_ends.push_back(i + 1);
        REQUIRE(result.lines_ends.size() == newline_ends.size());
        const auto difference = std::mismatch(result.lines_ends.begin(), result.lines_ends.end(), newline_ends.begin());
        INFO("first difference at line " << (difference.first - result.lines_ends.begin() + 1));
        CHECK(difference.first == result.lines_ends.end());
    };

    SECTION("BySize") { run_case(make_config(false), false); }
    SECTION("ByTime") { run_case(make_config(true), true); }
}

TEST_CASE("A small-bed config marks an object near the large-bed edge outside", "[PerPlatePresets][GCodeProcessor]")
{
    DynamicPrintConfig large = DynamicPrintConfig::full_print_config();
    large.set_deserialize_strict({
        {"printable_area", "0x0,256x0,256x256,0x256"},
        {"printable_height", "250"},
        {"brim_type", "no_brim"},
        {"skirt_loops", "0"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
    });

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, large);
    REQUIRE_FALSE(model.objects.empty());
    REQUIRE_FALSE(model.objects.front()->instances.empty());
    // 20 mm cube sitting around (200, 10): inside a 256 mm bed, outside a 180 mm A1-mini bed.
    model.objects.front()->instances.front()->set_offset(Vec3d(200., 10., 0.));
    model.objects.front()->ensure_on_bed();

    DynamicPrintConfig small = large;
    small.set_deserialize_strict({{"printable_area", "0x0,180x0,180x180,0x180"}, {"printable_height", "180"}});
    const Pointfs small_area = small.option<ConfigOptionPoints>("printable_area")->values;
    model.update_print_volume_state(BuildVolume(small_area, small.opt_float("printable_height")));
    REQUIRE(model.objects.front()->instances.front()->print_volume_state != ModelInstancePVS_Inside);

    // Processor records the Print's own bed. toolpath_outside is only reset (never set true)
    // in GCodeProcessor.cpp, so P7 uses print_volume_state / PartPlate::check_outside.
    Print inside;
    Model inside_model;
    init_print({TestMesh::cube_20x20x20}, inside, inside_model, small);
    inside.is_BBL_printer() = false;
    GCodeProcessorResult result;
    Test::gcode(inside, result);
    REQUIRE_FALSE(result.printable_area.empty());
    const BoundingBoxf bed(result.printable_area);
    CHECK(bed.max.x() <= 180. + 1e-3);
    CHECK(bed.max.y() <= 180. + 1e-3);
    CHECK_FALSE(result.toolpath_outside);
}
