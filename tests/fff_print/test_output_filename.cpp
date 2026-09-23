#include <catch2/catch.hpp>

#include <boost/filesystem/path.hpp>
#include <regex>
#include <string>

#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

static const char *U1_FILENAME_FORMAT =
    "{input_filename_base}_{filament_type[initial_no_support_extruder]}_{int(total_weight*10) / 10.0}g_{print_time}.gcode";

// 40x40mm cap on an 8x8mm stem — needs support (same fixture as test_support_material).
static TriangleMesh support_capital()
{
    TriangleMesh model = make_cube(8, 8, 13);
    model.translate(16., 16., 0.);
    TriangleMesh cap = make_cube(40, 40, 2);
    cap.translate(0., 0., 12.);
    model.merge(cap);
    return model;
}

static DynamicPrintConfig two_filament_support_config(bool support_on_extruder_0)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    // Wipe tower off so first_extruder stays the first printing tool, matching the
    // G-code generator's non-BBL / no-priming path.
    config.set_deserialize_strict({
        {"nozzle_diameter",            "0.4,0.4"},
        {"filament_diameter",          "1.75,1.75"},
        {"filament_density",           "1.24,1.24"},
        {"enable_support",             "1"},
        {"support_type",               "normal(auto)"},
        {"enable_prime_tower",         "0"},
        {"sparse_infill_density",      "0"},
        {"layer_height",               "0.3"},
        {"initial_layer_print_height", "0.3"},
        {"filename_format",            U1_FILENAME_FORMAT},
    });
    if (support_on_extruder_0) {
        config.set_deserialize_strict({
            {"filament_type",              "PVA;PLA"},
            {"filament_is_support",        "1,0"},
            {"filament_soluble",           "1,0"},
            {"support_filament",           "1"},
            {"support_interface_filament", "1"},
            {"wall_filament",              "2"},
            {"sparse_infill_filament",     "2"},
            {"solid_infill_filament",      "2"},
        });
    } else {
        config.set_deserialize_strict({
            {"filament_type",              "PLA;PVA"},
            {"filament_is_support",        "0,1"},
            {"filament_soluble",           "0,1"},
            {"support_filament",           "2"},
            {"support_interface_filament", "2"},
            {"wall_filament",              "1"},
            {"sparse_infill_filament",     "1"},
            {"solid_infill_filament",      "1"},
        });
    }
    return config;
}

TEST_CASE("U1 filename_format uses initial non-support extruder after a multi-material slice", "[Print][output_filename]")
{
    const bool support_on_extruder_0 = GENERATE(true, false);
    DYNAMIC_SECTION("support on extruder " << (support_on_extruder_0 ? 0 : 1))
    {
        Print print;
        Model model;
        init_print({support_capital()}, print, model, two_filament_support_config(support_on_extruder_0));
        REQUIRE_NOTHROW(print.process());
        REQUIRE(!print.objects().empty());
        REQUIRE(!print.objects().front()->support_layers().empty());

        const unsigned int expected = support_on_extruder_0 ? 1u : 0u;
        REQUIRE(print.initial_no_support_extruder_id() == expected);

        // After slice, before G-code: the index must already be a real variable.
        // Weight may still be a placeholder string here; Export G-code names the
        // file after export_gcode(), which is asserted below.
        std::string name_after_slice;
        REQUIRE_NOTHROW(name_after_slice = print.output_filename("u1_job"));
        REQUIRE(name_after_slice.find("PLA") != std::string::npos);
        REQUIRE(name_after_slice.find("PVA") == std::string::npos);

        const boost::filesystem::path gcode_path = scratch_path();
        REQUIRE_NOTHROW(print.export_gcode(gcode_path.string(), nullptr, nullptr));
        REQUIRE(print.finished());

        std::string name_after_export;
        REQUIRE_NOTHROW(name_after_export = print.output_filename("u1_job"));
        REQUIRE(name_after_export.find("PLA") != std::string::npos);
        REQUIRE(name_after_export.find("PVA") == std::string::npos);
        // Weight token must resolve to a numeric …Ng… segment.
        const std::regex weight_re(R"(u1_job_PLA_[0-9]+(\.[0-9]+)?g_.+\.gcode)");
        REQUIRE(std::regex_match(name_after_export, weight_re));
    }
}
