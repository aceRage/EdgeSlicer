// Multi-colour filaments, phase P1: the hard rule. The visual-only keys (filament_colour_split /
// _angle / _twist, visual_only_option_keys()) never change G-code and never invalidate a slice.
// tests/design_multicolour_filament_view.md 6.3. Built on the cost-overrides slice tests' shape
// (test_cost_overrides_slice.cpp: two cubes, two filaments, a prime tower).

#include <catch2/catch.hpp>

#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Two dual / tri-colour slots, two cubes, a prime tower: every per-filament path the keys could touch.
DynamicPrintConfig two_multicolour_filaments()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values     = { 1.75, 1.75 };
    config.option<ConfigOptionFloats>("nozzle_diameter")->values       = { 0.4, 0.4 };
    config.option<ConfigOptionStrings>("filament_colour")->values      = { "#FF0000", "#00FF00" };
    config.option<ConfigOptionStrings>("filament_multi_colors")->values = { "#FF0000|#FFFF00", "#00FF00|#0000FF|#FF0000" };
    config.option<ConfigOptionInts>("filament_colour_mode")->values    = { 0, 0 };
    config.option<ConfigOptionBool>("enable_prime_tower")->value       = true;
    config.option<ConfigOptionBool>("enable_support")->value           = false;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values          = { 15. };
    config.option<ConfigOptionFloats>("wipe_tower_y")->values          = { 15. };
    config.option<ConfigOptionFloat>("prime_tower_width")->value       = 35.;
    config.set_deserialize_strict({ { "brim_type", "no_brim" }, { "skirt_loops", "0" }, { "wipe_tower_wall_type", "rectangle" },
                                    { "sparse_infill_density", "10%" } });
    return config;
}

// The same with the keys set to non-default values (6.3, test 1).
DynamicPrintConfig with_visual_keys(DynamicPrintConfig config)
{
    config.set_key_value("filament_colour_split", new ConfigOptionStrings{ "30|70", "25|25|50" });
    config.set_key_value("filament_colour_angle", new ConfigOptionFloats{ 137., -45. });
    config.set_key_value("filament_colour_twist", new ConfigOptionFloats{ 90., 0. });
    // Glow-in-the-dark (G1)
    config.set_key_value("filament_glow", new ConfigOptionInts{ 1, 0 });
    config.set_key_value("filament_glow_colour", new ConfigOptionStrings{ "#7CFF6B", "" });
    config.set_key_value("filament_glow_intensity", new ConfigOptionFloat(150.));
    config.set_key_value("filament_gradient_dwell", new ConfigOptionFloats{ 1500., -1. });
    config.set_key_value("filament_gradient_transition", new ConfigOptionFloats{ 300., 0. });
    return config;
}

// ...and with other non-default values, for the "only the visual keys changed" re-apply.
DynamicPrintConfig with_other_visual_keys(DynamicPrintConfig config)
{
    config.set_key_value("filament_colour_split", new ConfigOptionStrings{ "55|45", "10|80|10" });
    config.set_key_value("filament_colour_angle", new ConfigOptionFloats{ 0., 270. });
    config.set_key_value("filament_colour_twist", new ConfigOptionFloats{ -15., 720. });
    config.set_key_value("filament_glow", new ConfigOptionInts{ -1, 1 });
    config.set_key_value("filament_glow_colour", new ConfigOptionStrings{ "", "#FF00FF" });
    config.set_key_value("filament_glow_intensity", new ConfigOptionFloat(20.));
    config.set_key_value("filament_gradient_dwell", new ConfigOptionFloats{ 400., 12000. });
    config.set_key_value("filament_gradient_transition", new ConfigOptionFloats{ 0., 50. });
    return config;
}

void two_cubes(Model &model)
{
    ModelObject *first = model.add_object();
    first->name        = "cube-a.stl";
    first->add_volume(mesh(TestMesh::cube_20x20x20));
    first->add_instance()->set_offset(Vec3d(80., 40., 0.));
    first->ensure_on_bed();
    ModelObject *second = model.add_object();
    second->name        = "cube-b.stl";
    second->add_volume(mesh(TestMesh::cube_20x20x20));
    second->add_instance()->set_offset(Vec3d(120., 40., 0.));
    second->ensure_on_bed();
    second->volumes.front()->config.set("extruder", 2);
}

std::string read_file(const std::string &path)
{
    std::ifstream t(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
}

std::string export_to(Print &print)
{
    const boost::filesystem::path path = scratch_path();
    print.set_status_silent();
    print.process();
    print.export_gcode(path.string(), nullptr, nullptr);
    std::string gcode = read_file(path.string());
    boost::nowide::remove(path.string().c_str());
    return gcode;
}

// Only the line carrying the wall-clock time of the export may differ between two exports.
std::string without_timestamp(const std::string &gcode)
{
    std::istringstream in(gcode);
    std::string        line, out;
    while (std::getline(in, line))
        if (line.rfind("; generated by", 0) != 0)
            out += line + "\n";
    return out;
}

std::string slice(const Model &model, const DynamicPrintConfig &config)
{
    Print print;
    print.apply(model, config);
    return export_to(print);
}

} // namespace

TEST_CASE("Multi-colour keys: the G-code is identical with and without them", "[MultiColour][GCode]")
{
    Model model;
    two_cubes(model);
    const std::string plain   = slice(model, two_multicolour_filaments());
    const std::string colours = slice(model, with_visual_keys(two_multicolour_filaments()));
    REQUIRE(plain.size() > 1000);
    // The whole file, config block included, apart from the export timestamp line.
    CHECK(plain.size() == colours.size());
    CHECK(without_timestamp(plain) == without_timestamp(colours));
    // The colours themselves are still metadata in the block (D2: unchanged behaviour).
    CHECK(plain.find("; filament_multi_colors = ") != std::string::npos);
}

TEST_CASE("Multi-colour keys: changing them invalidates nothing", "[MultiColour][Invalidation]")
{
    Model model;
    two_cubes(model);
    Print print;
    print.apply(model, with_visual_keys(two_multicolour_filaments()));
    export_to(print);
    REQUIRE(print.is_step_done(psGCodeExport));

    // Only the visual keys changed: nothing to redo, not even the export.
    CHECK(print.apply(model, with_other_visual_keys(two_multicolour_filaments())) == PrintBase::APPLY_STATUS_UNCHANGED);
    CHECK(print.is_step_done(psGCodeExport));
    // Removed altogether (an older project): the same.
    CHECK(print.apply(model, two_multicolour_filaments()) == PrintBase::APPLY_STATUS_UNCHANGED);
    CHECK(print.is_step_done(psGCodeExport));
    // Back again.
    CHECK(print.apply(model, with_visual_keys(two_multicolour_filaments())) == PrintBase::APPLY_STATUS_UNCHANGED);
    CHECK(print.is_step_done(psGCodeExport));
    CHECK(print.is_step_done(posSlice));

    // Control: a real colour change (filament_multi_colors, D2) still re-runs the export, as today.
    DynamicPrintConfig recoloured = with_visual_keys(two_multicolour_filaments());
    recoloured.option<ConfigOptionStrings>("filament_multi_colors")->values = { "#FF0000|#0000FF", "#00FF00|#0000FF|#FF0000" };
    CHECK(print.apply(model, recoloured) == PrintBase::APPLY_STATUS_INVALIDATED);
    CHECK_FALSE(print.is_step_done(psGCodeExport));
    CHECK(print.is_step_done(posSlice));
}

TEST_CASE("Multi-colour keys: never in the config block or the Print's config", "[MultiColour][GCode]")
{
    Model model;
    two_cubes(model);
    Print print;
    print.apply(model, with_visual_keys(two_multicolour_filaments()));
    const std::string gcode = export_to(print);
    for (const std::string &key : visual_only_option_keys()) {
        INFO(key);
        CHECK(gcode.find(key) == std::string::npos);
        CHECK_FALSE(print.full_print_config().has(key));
    }
}

TEST_CASE("Multi-colour keys: custom G-code cannot read them", "[MultiColour][Placeholders]")
{
    // The same "Not a variable name" error as for a key that does not exist at all.
    const std::string templ = GENERATE(as<std::string>{}, "{filament_colour_angle[0]}", "{filament_colour_split[1]}",
                                       "{filament_colour_twist[0]}", "{filament_glow[0]}", "{filament_glow_colour[1]}",
                                       "{filament_glow_intensity}", "{no_such_option_xyz[0]}");
    CAPTURE(templ);
    DynamicPrintConfig config = with_visual_keys(two_multicolour_filaments());
    config.set_key_value("machine_start_gcode", new ConfigOptionString("; " + templ + "\n"));
    Model model;
    two_cubes(model);
    Print print;
    print.apply(model, config);
    CHECK_THROWS_WITH(export_to(print), Catch::Contains("Not a variable name"));
}

TEST_CASE("Multi-colour keys: Mixed / Full Spectrum virtual filaments slice the same", "[MultiColour][MixedFilament]")
{
    // A virtual mixed filament (MixedFilamentManager) on the second cube. Virtual filaments are not
    // physical slots: the visual keys follow filament_colour's two slots and change nothing here.
    DynamicPrintConfig base = two_multicolour_filaments();
    MixedFilamentManager mixed_mgr;
    mixed_mgr.add_custom_filament(1, 2, 50, { "#FF0000", "#00FF00" });
    mixed_mgr.mixed_filaments().front().manual_pattern = MixedFilamentManager::normalize_manual_pattern("12");
    const size_t       n_physical = 2;
    const unsigned int virtual_id = mixed_mgr.filament_id_from_mixed_index(0, n_physical);
    REQUIRE(virtual_id > n_physical);
    base.set("mixed_filament_definitions", mixed_mgr.serialize_custom_entries());

    Model model;
    two_cubes(model);
    ModelObject *second = model.objects.back();
    for (const char *key : { "wall_filament", "sparse_infill_filament", "solid_infill_filament" })
        second->config.set_key_value(key, new ConfigOptionInt(int(virtual_id)));

    Print plain_print, colour_print;
    plain_print.apply(model, base);
    colour_print.apply(model, with_visual_keys(base));
    const std::string plain   = export_to(plain_print);
    const std::string colours = export_to(colour_print);
    REQUIRE(plain_print.mixed_filament_manager().total_filaments(n_physical) == mixed_mgr.total_filaments(n_physical));
    CHECK(colour_print.mixed_filament_manager().total_filaments(n_physical) ==
          plain_print.mixed_filament_manager().total_filaments(n_physical));
    CHECK(without_timestamp(plain) == without_timestamp(colours));
}
