// EdgeSlicer: support interface temperature (support_interface_nozzle_temperature).
// tests/design_support_upgrades_2026-11-14.md part 1B, owner decisions of 2026-11-14:
//  - D2: 0 = off, the interface filament prints at its own temperatures; set, it prints at the set one.
//  - D3: the set temperature applies on every layer, the first one included (1-layer raft).
//  - D4: the filament's temperature IS the set one everywhere the slicer reasons about it (tool change,
//        prime tower, start temperatures, placeholders), so no extra waits are added. A filament that also
//        prints part of a model keeps its own temperatures, with a warning.
//  - A set temperature outside the filament's recommended range is warned about, naming the filament.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// A 30 x 30 mm, 2 mm thick cap on a 10 x 10 mm stem: one flat roof at 10 mm, supported from the bed.
TriangleMesh capped_stem()
{
    TriangleMesh model = make_cube(10, 10, 11);
    model.translate(10., 10., 0.);
    TriangleMesh cap = make_cube(30, 30, 2);
    cap.translate(0., 0., 10.);
    model.merge(cap);
    return model;
}

// Model and support body in filament 1, interface in filament 2. Filament 1 prints at 200 (205 first
// layer), filament 2 at 240 (245 first layer); recommended ranges 190-230 and 230-260.
DynamicPrintConfig iface_config(int interface_temperature, bool semm = true, bool prime_tower = false)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        { "enable_support",                       1 },
        { "support_type",                         "normal(auto)" },
        { "support_on_build_plate_only",          1 },
        { "layer_height",                         0.2 },
        { "initial_layer_print_height",           0.2 },
        { "support_filament",                     1 },
        { "support_interface_filament",           2 },
        { "support_interface_top_layers",         2 },
        { "support_interface_bottom_layers",      0 },
        { "nozzle_diameter",                      "0.4,0.4" },
        { "filament_diameter",                    "1.75,1.75" },
        { "filament_type",                        "PLA;PETG" },
        { "filament_soluble",                     "0,0" },
        { "nozzle_temperature",                   "200,240" },
        { "nozzle_temperature_initial_layer",     "205,245" },
        { "nozzle_temperature_range_low",         "190,230" },
        { "nozzle_temperature_range_high",        "230,260" },
        { "single_extruder_multi_material",       semm ? 1 : 0 },
        { "enable_prime_tower",                   prime_tower ? 1 : 0 },
        { "support_interface_nozzle_temperature", interface_temperature },
    });
    if (prime_tower) {
        config.option<ConfigOptionFloats>("wipe_tower_x")->values    = { 15. };
        config.option<ConfigOptionFloats>("wipe_tower_y")->values    = { 15. };
        config.option<ConfigOptionFloat>("prime_tower_width")->value = 35.;
        config.set_deserialize_strict({ { "wipe_tower_wall_type", "rectangle" } });
    }
    return config;
}

struct TempCheck
{
    // Target temperature of the heater printing each filament-2 extrusion (layer z -> temperatures seen).
    std::map<double, std::vector<int>> filament2_temps;
    int  interface_lines   = 0; // ;TYPE:Support interface blocks
    int  waits             = 0; // M109
    int  filament2_waits   = 0; // M109 naming filament 2's heater (T1) or issued while it is loaded (SEMM)
};

// Follows the G-code: tool changes (T<n>), M104/M109 S<t> [T<n>] targets per heater (one heater when
// `semm`), and every extrusion move made with filament 2 (T1) loaded.
TempCheck follow(const std::string &gcode, bool semm)
{
    TempCheck out;
    std::map<int, int> target; // heater -> target
    int    tool     = -1;
    bool   relative = false;
    double last_e   = 0.;
    double z        = 0.;
    std::istringstream in(gcode);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(";TYPE:Support interface", 0) == 0)
            ++out.interface_lines;
        const std::string code = line.substr(0, line.find(';'));
        std::istringstream words(code);
        std::string        cmd;
        if (!(words >> cmd))
            continue;
        std::map<char, double> args;
        for (std::string w; words >> w;)
            if (w.size() > 1)
                args[w[0]] = std::atof(w.c_str() + 1);
        if (cmd.size() > 1 && cmd[0] == 'T' && std::all_of(cmd.begin() + 1, cmd.end(), ::isdigit)) {
            tool = std::atoi(cmd.c_str() + 1);
        } else if (cmd == "M83") {
            relative = true;
        } else if (cmd == "M82") {
            relative = false;
        } else if (cmd == "G92" && args.count('E')) {
            last_e = args['E'];
        } else if (cmd == "M104" || cmd == "M109") {
            if (!args.count('S'))
                continue;
            const int heater = semm ? 0 : (args.count('T') ? int(args['T']) : tool);
            target[heater]   = int(args['S']);
            if (cmd == "M109") {
                ++out.waits;
                if (semm ? tool == 1 : heater == 1)
                    ++out.filament2_waits;
            }
        } else if (cmd == "G1" || cmd == "G0" || cmd == "G2" || cmd == "G3") {
            if (args.count('Z'))
                z = args['Z'];
            if (!args.count('E'))
                continue;
            const double e        = args['E'];
            const bool   extrudes = relative ? e > 0. : e > last_e;
            if (!relative)
                last_e = e;
            if (extrudes && (args.count('X') || args.count('Y')) && tool == 1) {
                const int heater = semm ? 0 : 1;
                out.filament2_temps[z].push_back(target.count(heater) ? target[heater] : -1);
            }
        }
    }
    return out;
}

// The G-code without comments and without temperature commands: what must not change when only the
// interface filament's temperature changes.
std::string without_temperatures(const std::string &gcode)
{
    std::istringstream in(gcode);
    std::ostringstream out;
    std::string        line;
    while (std::getline(in, line)) {
        const std::string code = line.substr(0, line.find(';'));
        if (code.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        // Temperatures, and the progress lines whose remaining time an estimate could move.
        if (code.rfind("M104", 0) == 0 || code.rfind("M109", 0) == 0 || code.rfind("M73", 0) == 0)
            continue;
        out << code << "\n";
    }
    return out.str();
}

std::vector<int> all_temps(const TempCheck &c)
{
    std::vector<int> out;
    for (const auto &[z, temps] : c.filament2_temps)
        out.insert(out.end(), temps.begin(), temps.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> validate_warnings(Print &print)
{
    std::vector<StringObjectException> warnings;
    print.validate(&warnings);
    std::vector<std::string> out;
    for (const StringObjectException &w : warnings)
        out.push_back(w.string);
    return out;
}

bool any_contains(const std::vector<std::string> &strings, const std::string &needle)
{
    return std::any_of(strings.begin(), strings.end(), [&](const std::string &s) { return s.find(needle) != std::string::npos; });
}

} // namespace

TEST_CASE("Support interface temperature: off prints the filament's own temperatures", "[SupportInterfaceTemperature]")
{
    for (const bool semm : { true, false }) {
        CAPTURE(semm);
        const std::string gcode = Test::slice({ capped_stem() }, iface_config(0, semm), true);
        const TempCheck   c     = follow(gcode, semm);
        REQUIRE(c.interface_lines > 0);
        REQUIRE(!c.filament2_temps.empty());
        CHECK(all_temps(c) == std::vector<int>{ 240 });

        Print print;
        Model model;
        init_print({ capped_stem() }, print, model, iface_config(0, semm));
        CHECK(print.support_interface_temperatures().empty());
        CHECK(!any_contains(validate_warnings(print), "support interface temperature"));
    }
}

TEST_CASE("Support interface temperature: set, the interface prints at it with no extra waits", "[SupportInterfaceTemperature]")
{
    // SEMM without a tower (M104 after the change), SEMM with a prime tower, and one heater per tool.
    struct Case { bool semm; bool tower; };
    for (const Case cs : { Case{ true, false }, Case{ true, true }, Case{ false, false } }) {
        CAPTURE(cs.semm, cs.tower);
        const std::string off_gcode = Test::slice({ capped_stem() }, iface_config(0, cs.semm, cs.tower), true);
        const std::string on_gcode  = Test::slice({ capped_stem() }, iface_config(225, cs.semm, cs.tower), true);
        const TempCheck   off       = follow(off_gcode, cs.semm);
        const TempCheck   on        = follow(on_gcode, cs.semm);
        REQUIRE(on.interface_lines > 0);
        REQUIRE(!on.filament2_temps.empty());
        CHECK(all_temps(on) == std::vector<int>{ 225 });
        // No new waits anywhere, and none for the interface filament.
        CHECK(on.waits <= off.waits);
        CHECK(on.filament2_waits <= off.filament2_waits);
        // Nothing but temperature commands changes.
        CHECK(without_temperatures(on_gcode) == without_temperatures(off_gcode));
    }
}

TEST_CASE("Support interface temperature: the first layer takes it too", "[SupportInterfaceTemperature]")
{
    // D3: a 1-layer raft is printed by the interface filament on the first layer.
    for (const bool semm : { true, false }) {
        CAPTURE(semm);
        DynamicPrintConfig config = iface_config(225, semm);
        config.set_deserialize_strict({ { "raft_layers", 1 } });
        const TempCheck c = follow(Test::slice({ capped_stem() }, config, true), semm);
        REQUIRE(!c.filament2_temps.empty());
        CHECK(c.filament2_temps.begin()->first < 0.25); // filament 2 prints on the first layer
        CHECK(all_temps(c) == std::vector<int>{ 225 });
    }
    // One heater per tool: the start sequence heats tool 1 to the set temperature for the first layer.
    {
        const std::string gcode = Test::slice({ capped_stem() }, iface_config(225, false), true);
        CHECK(gcode.find("S245 T1") == std::string::npos);
        CHECK(gcode.find("T1 S245") == std::string::npos);
    }
}

TEST_CASE("Support interface temperature: placeholders see it", "[SupportInterfaceTemperature]")
{
    // The U1's change_filament_gcode waits for temperature[next_extruder] on the next tool.
    DynamicPrintConfig config = iface_config(225, false);
    config.set_deserialize_strict({ { "change_filament_gcode",
                                      "M109 S{temperature[next_extruder]} T{next_extruder}\n"
                                      "; first_layer_temp_of_next={first_layer_temperature[next_extruder]}\n" } });
    const std::string gcode = Test::slice({ capped_stem() }, config, true);
    CHECK(gcode.find("M109 S225 T1") != std::string::npos);
    CHECK(gcode.find("M109 S240 T1") == std::string::npos);
    CHECK(gcode.find("; first_layer_temp_of_next=225") != std::string::npos);
    CHECK(gcode.find("; first_layer_temp_of_next=245") == std::string::npos);
}

TEST_CASE("Support interface temperature: resolution and warnings", "[SupportInterfaceTemperature]")
{
    SECTION("inside the range: applied, no warning")
    {
        Print print;
        Model model;
        init_print({ capped_stem() }, print, model, iface_config(245));
        CHECK(print.support_interface_temperatures().temperatures == std::map<unsigned int, int>{ { 1u, 245 } });
        const std::vector<std::string> w = validate_warnings(print);
        CHECK(!any_contains(w, "support interface temperature"));
    }
    SECTION("outside the range: applied, warned with the filament named")
    {
        Print print;
        Model model;
        init_print({ capped_stem() }, print, model, iface_config(225));
        CHECK(print.support_interface_temperatures().temperatures == std::map<unsigned int, int>{ { 1u, 225 } });
        const std::vector<std::string> w = validate_warnings(print);
        CHECK(any_contains(w, "support interface temperature 225"));
        CHECK(any_contains(w, "filament 2"));
        CHECK(any_contains(w, "230-260"));
    }
    SECTION("a range of 0 is not checked")
    {
        DynamicPrintConfig config = iface_config(300);
        config.set_deserialize_strict({ { "nozzle_temperature_range_low", "0,0" }, { "nozzle_temperature_range_high", "0,0" } });
        Print print;
        Model model;
        init_print({ capped_stem() }, print, model, config);
        CHECK(print.support_interface_temperatures().temperatures.size() == 1);
        CHECK(!any_contains(validate_warnings(print), "support interface temperature"));
    }
    SECTION("current filament as interface: not used, warned")
    {
        DynamicPrintConfig config = iface_config(225);
        config.set_deserialize_strict({ { "support_interface_filament", 0 } });
        Print print;
        Model model;
        init_print({ capped_stem() }, print, model, config);
        CHECK(print.support_interface_temperatures().temperatures.empty());
        CHECK(any_contains(validate_warnings(print), "is not used"));
    }
    SECTION("support off: nothing to do, no warning")
    {
        DynamicPrintConfig config = iface_config(225);
        config.set_deserialize_strict({ { "enable_support", 0 } });
        Print print;
        Model model;
        init_print({ capped_stem() }, print, model, config);
        CHECK(print.support_interface_temperatures().empty());
    }
}

TEST_CASE("Support interface temperature: a filament that also prints the model keeps its own", "[SupportInterfaceTemperature]")
{
    for (const bool semm : { true, false }) {
        CAPTURE(semm);
        // The model's walls in filament 2 as well: the override is ignored, the G-code is the one without it.
        DynamicPrintConfig on = iface_config(225, semm);
        on.set_deserialize_strict({ { "wall_filament", 2 } });
        DynamicPrintConfig off = iface_config(0, semm);
        off.set_deserialize_strict({ { "wall_filament", 2 } });

        Print print;
        Model model;
        init_print({ capped_stem() }, print, model, on);
        CHECK(print.support_interface_temperatures().temperatures.empty());
        const std::vector<std::string> w = validate_warnings(print);
        CHECK(any_contains(w, "is not used"));
        CHECK(any_contains(w, "also prints part of a model"));

        const std::string on_gcode  = Test::slice({ capped_stem() }, on, true);
        const std::string off_gcode = Test::slice({ capped_stem() }, off, true);
        CHECK(without_temperatures(on_gcode) == without_temperatures(off_gcode));
        CHECK(all_temps(follow(on_gcode, semm)) == all_temps(follow(off_gcode, semm)));
    }
}

TEST_CASE("Support interface temperature: config writer", "[SupportInterfaceTemperature]")
{
    DynamicPrintConfig config = iface_config(0);
    apply_support_interface_temperatures(config, { { 1u, 222 } });
    CHECK(config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{ 200, 222 });
    CHECK(config.option<ConfigOptionInts>("nozzle_temperature_initial_layer")->values == std::vector<int>{ 205, 222 });
    // A filament the config does not have is left alone.
    apply_support_interface_temperatures(config, { { 5u, 180 } });
    CHECK(config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{ 200, 222 });
    // Nothing set: nothing written.
    DynamicPrintConfig plain = iface_config(0);
    apply_support_interface_temperatures(plain, {});
    CHECK(plain.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{ 200, 240 });
}
