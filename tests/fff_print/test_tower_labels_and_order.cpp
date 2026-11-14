#include <catch2/catch.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Model.hpp"
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// Object labels around a Type 2 wipe tower's tool change (WipeTowerIntegration::append_tcr2), and
// the order copies are printed in on a tower layer (GCode::process_layer, chain from the tower).

namespace {

struct Placed
{
    const char* name;
    double      x, y;
    int         extruder;
};

// A two-tool printer (the U1's kind: no ramming, the Type 2 tower) with Klipper object labels.
std::string slice(const std::vector<Placed>& objects, double tower_x, double tower_y, bool tool_change_on_wipe_tower,
                  const std::string& flavor = "klipper")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values         = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values          = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values           = {0.4, 0.4};
    config.option<ConfigOptionBools>("filament_multitool_ramming")->values = {false, false};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = true;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values                = {tower_x};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values                = {tower_y};
    config.option<ConfigOptionFloat>("prime_tower_width")->value             = 35.;
    config.option<ConfigOptionBool>("tool_change_on_wipe_tower")->value      = tool_change_on_wipe_tower;
    config.set_deserialize_strict({
        {"gcode_flavor",               flavor},
        {"exclude_object",             "1"},
        {"printable_area",             "0x0,250x0,250x250,0x250"},
        {"skirt_loops",                0},
        {"brim_type",                  "no_brim"},
        {"gcode_comments",             "1"},
        {"machine_start_gcode",        ""},
        {"change_filament_gcode",      ""},
        {"layer_height",               "0.2"},
        {"initial_layer_print_height", "0.2"},
    });

    Print print;
    Model model;
    for (const Placed& p : objects) {
        ModelObject* o = model.add_object();
        o->name        = p.name;
        o->add_volume(mesh(TestMesh::cube_20x20x20));
        o->add_instance()->set_offset(Vec3d(p.x, p.y, 0.));
        o->ensure_on_bed();
        o->volumes.front()->config.set("extruder", p.extruder);
    }
    print.apply(model, config);
    print.set_status_silent();
    REQUIRE_FALSE(print.is_BBL_printer());
    REQUIRE(print.has_wipe_tower());
    return Test::gcode(print);
}

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

bool is_tool_line(const std::string& line) { return line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1])); }

// For each tool change: the object label open when it is written ("" when none).
std::vector<std::string> labels_open_at_tool_changes(const std::string& gcode)
{
    std::vector<std::string> out;
    std::istringstream       in(gcode);
    std::string              line, open;
    while (std::getline(in, line)) {
        if (starts_with(line, "EXCLUDE_OBJECT_START NAME="))
            open = line.substr(26);
        else if (starts_with(line, "EXCLUDE_OBJECT_END"))
            open.clear();
        else if (is_tool_line(line))
            out.push_back(open);
    }
    return out;
}

// For each layer, the objects in the order they are started.
std::vector<std::vector<std::string>> object_order_per_layer(const std::string& gcode)
{
    std::vector<std::vector<std::string>> layers;
    std::istringstream                    in(gcode);
    std::string                           line;
    while (std::getline(in, line)) {
        if (starts_with(line, ";LAYER_CHANGE"))
            layers.emplace_back();
        else if (starts_with(line, "EXCLUDE_OBJECT_START NAME=") && !layers.empty()) {
            std::string name = line.substr(26);
            name             = name.substr(0, name.find("_id_"));
            if (layers.back().empty() || layers.back().back() != name)
                layers.back().push_back(name);
        }
    }
    return layers;
}

// Total XY travel (G0/G1 with X or Y and no positive E), in mm.
double travel_mm(const std::string& gcode)
{
    std::istringstream in(gcode);
    std::string        line;
    double             x = 0., y = 0., total = 0.;
    while (std::getline(in, line)) {
        if (!(starts_with(line, "G0 ") || starts_with(line, "G1 ")))
            continue;
        const std::string code = line.substr(0, line.find(';'));
        double nx = x, ny = y, e = 0.;
        bool   has_xy = false;
        std::istringstream words(code.substr(3));
        std::string        w;
        while (words >> w) {
            if (w[0] == 'X') { nx = std::atof(w.c_str() + 1); has_xy = true; }
            else if (w[0] == 'Y') { ny = std::atof(w.c_str() + 1); has_xy = true; }
            else if (w[0] == 'E') e = std::atof(w.c_str() + 1);
        }
        if (has_xy && e <= 0.)
            total += std::hypot(nx - x, ny - y);
        x = nx;
        y = ny;
    }
    return total;
}

// Four red cubes in a row away from the tower, a blue one so the plate has two colours.
const std::vector<Placed> kRow = {
    {"near_origin", 30., 30., 1}, {"second", 70., 30., 1}, {"third", 110., 30., 1}, {"near_tower", 150., 30., 1}, {"blue", 30., 120., 2},
};
constexpr double kTowerX = 190., kTowerY = 20.;

} // namespace

TEST_CASE("Type 2 tower: an object's label is closed before the tool change", "[GCode][WipeTower][ObjectLabels]")
{
    for (bool on_tower : {false, true}) {
        DYNAMIC_SECTION((on_tower ? "tool change on the tower" : "tool change in place")) {
            const std::string              gcode = slice(kRow, kTowerX, kTowerY, on_tower);
            const std::vector<std::string> open  = labels_open_at_tool_changes(gcode);
            REQUIRE(open.size() > 4);
            size_t inside = 0;
            for (const std::string& label : open)
                if (!label.empty())
                    ++inside;
            INFO(inside << " of " << open.size() << " tool changes inside an object's label");
            CHECK(inside == 0);
        }
    }
}

TEST_CASE("Tower layers: the copies are chained from the tower, so the last one printed is the nearest to it",
          "[GCode][WipeTower][ObjectOrder]")
{
    const std::string gcode  = slice(kRow, kTowerX, kTowerY, false);
    const auto        layers = object_order_per_layer(gcode);
    REQUIRE(layers.size() > 50);
    size_t checked = 0;
    for (size_t l = 1; l < layers.size(); ++l) {
        // The red ones of this layer, in order.
        std::vector<std::string> red;
        for (const std::string& n : layers[l])
            if (n != "blue")
                red.push_back(n);
        if (red.size() != 4)
            continue;
        INFO("layer " << l);
        CHECK(red.front() == "near_origin");
        CHECK(red.back() == "near_tower");
        ++checked;
    }
    CHECK(checked > 50);
}

// Not run by default: the travel of four test layouts with the tower in four places, for the PR
// ("[.tower_travel]" to run it; TOWER_ORDER_DUMP=<prefix> also writes the row print's G-code).
TEST_CASE("Tower layers: travel of the test prints", "[.tower_travel]")
{
    const std::vector<std::vector<Placed>> layouts = {
        kRow,
        {{"a", 30., 30., 1}, {"b", 80., 30., 1}, {"c", 30., 80., 1}, {"d", 80., 80., 1}, {"e", 130., 130., 2}, {"f", 30., 130., 2}},
        {{"a", 20., 20., 1}, {"b", 60., 20., 2}, {"c", 100., 20., 1}, {"d", 20., 60., 2}, {"e", 60., 60., 1}, {"f", 100., 60., 2},
         {"g", 20., 100., 1}, {"h", 60., 100., 2}, {"i", 100., 100., 1}},
        {{"a", 100., 100., 1}, {"b", 140., 100., 1}, {"c", 100., 140., 1}, {"d", 60., 60., 2}},
    };
    const std::vector<std::pair<double, double>> towers = {{190., 20.}, {190., 190.}, {15., 190.}, {150., 150.}};
    for (size_t l = 0; l < layouts.size(); ++l)
        for (const auto& t : towers) {
            const double mm = travel_mm(slice(layouts[l], t.first, t.second, false));
            std::cout << "travel layout " << l << " tower " << t.first << "," << t.second << ": " << mm << " mm\n";
        }
    if (const char* dump = std::getenv("TOWER_ORDER_DUMP"))
        std::ofstream(std::string(dump) + "_row.gcode") << slice(kRow, kTowerX, kTowerY, false);
}
