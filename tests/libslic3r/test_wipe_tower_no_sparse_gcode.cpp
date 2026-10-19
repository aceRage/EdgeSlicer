#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/GCode/WipeTower.hpp"

#include "../fff_print/test_data.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <tbb/global_control.h>
#include <tbb/task_arena.h>

#include <iostream>

using namespace Slic3r;

// "No sparse layers" on a Bambu Lab (WipeTower, append_tcr) prime tower, checked on the exported G-code.
//
// Owner report (H2D / H2C): towers printed with no sparse layers keep failing to stick. Slicing the
// same job in Bambu Studio showed the compacted Z schedule is identical, but in this fork every
// compacted tower layer - the first one on the bed included - had its wall extruded at travel speed:
// the descent back down to the tower after change_filament_gcode was a bare "G1 Z.. F<travel>", and
// the tower's toolchange wall carries no F word of its own, so it inherited F60000. On top of that the
// "auto" brim (-1, shipped by the H2D/H2C process presets) was never resolved, so the tower had no
// brim at all while the preview drew one.

namespace {

struct TowerMove
{
    double z;       // commanded Z of the extrusion
    double layer_z; // object layer the tower was printed on (; Z_HEIGHT)
    double f;       // feedrate in effect (mm/min)
    double x;
    double y;
};

// Every extruding XY move between WIPE_TOWER_START and WIPE_TOWER_END, with the modal Z and F that
// the printer really uses for it (the last Z / F word seen anywhere before it).
std::vector<TowerMove> tower_moves(const std::string &gcode)
{
    std::vector<TowerMove> out;
    std::istringstream     in(gcode);
    std::string            line;
    double                 x = 0, y = 0, z = 0, f = 0, layer_z = 0;
    bool                   in_tower = false;
    bool                   relative_e = false;
    double                 e = 0;
    const std::regex       word("([XYZEF])(-?[0-9]*\\.?[0-9]+)");
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("; Z_HEIGHT:", 0) == 0) {
            layer_z = std::atof(line.c_str() + 11);
            continue;
        }
        if (line.find("; WIPE_TOWER_START") == 0) { in_tower = true; continue; }
        if (line.find("; WIPE_TOWER_END") == 0) { in_tower = false; continue; }
        const std::string code = line.substr(0, line.find(';'));
        if (code.rfind("M83", 0) == 0) { relative_e = true; continue; }
        if (code.rfind("M82", 0) == 0) { relative_e = false; continue; }
        // G2/G3: a rib wall's fillets are arcs when arc fitting is on (I/J are not needed here).
        if (!(code.rfind("G1 ", 0) == 0 || code.rfind("G0 ", 0) == 0 || code.rfind("G2 ", 0) == 0 || code.rfind("G3 ", 0) == 0))
            continue;
        double nx = x, ny = y, de = 0;
        bool   has_e = false;
        for (auto it = std::sregex_iterator(code.begin(), code.end(), word); it != std::sregex_iterator(); ++it) {
            const char   axis = (*it)[1].str()[0];
            const double v    = std::atof((*it)[2].str().c_str());
            switch (axis) {
            case 'X': nx = v; break;
            case 'Y': ny = v; break;
            case 'Z': z = v; break;
            case 'F': f = v; break;
            case 'E': has_e = true; de = relative_e ? v : v - e; if (!relative_e) e = v; break;
            }
        }
        const bool moves_xy = std::abs(nx - x) > 1e-6 || std::abs(ny - y) > 1e-6;
        if (in_tower && has_e && de > 1e-6 && moves_xy)
            out.push_back({z, layer_z, f, nx, ny});
        x = nx;
        y = ny;
    }
    return out;
}

// A 20 mm cube with walls in filament 1 and sparse infill in filament 2 (solid infill in filament 1),
// plus a 0.6 mm pad in filament 2. Layers 1-2 change filament (the pad), layers 3-6 are the cube's
// single-filament bottom shell (sparse tower layers, skipped), every layer above changes filament
// twice. So from layer 7 on the tower is printed well below the object and the nozzle has to come
// back down to it after every toolchange. (The pad is what makes the plate count as two-filament:
// Print::extruders() does not see per-feature filaments, and a one-filament plate drops the tower.)
DynamicPrintConfig no_sparse_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        // A Bambu-style toolchange: lift clear of the print, then switch. The lift is what the
        // compacted tower has to come back down from.
        { "change_filament_gcode", "G1 Z{max_layer_z + 3.0} F1200\nT[next_extruder]\n" },
        { "layer_height", 0.3 },
        { "initial_layer_print_height", 0.3 },
        { "wall_loops", 1 },
        { "wall_filament", 1 },
        { "sparse_infill_filament", 2 },
        { "solid_infill_filament", 1 },
        { "sparse_infill_density", "15%" },
        { "bottom_shell_layers", 6 },
        { "top_shell_layers", 0 },
        { "enable_support", false },
        { "skirt_loops", 0 },
        { "enable_prime_tower", true },
        { "wipe_tower_no_sparse_layers", true },
        { "prime_tower_width", 30 },
        { "prime_tower_brim_width", -1 },
        { "wipe_tower_x", "140" },
        { "wipe_tower_y", "140" },
        { "travel_speed", 500 },
        { "retraction_length", "0.8,0.8" },
    });
    return config;
}

std::string slice_bbl(const DynamicPrintConfig &config)
{
    Slic3r::Print print;
    Slic3r::Model model;
    ModelObject  *object = model.add_object();
    object->name         = "cube.stl";
    object->add_volume(Slic3r::Test::mesh(Slic3r::Test::TestMesh::cube_20x20x20));
    object->add_instance()->set_offset(Vec3d(40., 40., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    ModelObject *pad = model.add_object();
    pad->name        = "pad.stl";
    pad->add_volume(make_cube(10., 10., 0.6));
    pad->add_instance()->set_offset(Vec3d(80., 40., 0.));
    pad->ensure_on_bed();
    pad->config.set("extruder", 2);
    print.apply(model, config);
    print.is_BBL_printer() = true;
    const StringObjectException err = print.validate();
    INFO(err.string);
    REQUIRE(err.string.empty());
    return Slic3r::Test::gcode(print);
}

// The checks below for one wall type of the Bambu Lab generator (the rib wall squares the tower
// and adds ribs, with arcs for its fillets).
void check_no_sparse_tower(const std::string &wall)
{
    DynamicPrintConfig config = no_sparse_config();
    config.set_deserialize_strict({ { "wipe_tower_wall_type", wall }, { "enable_arc_fitting", 1 } });
    INFO("wall type " << wall);
    const std::string            gcode  = slice_bbl(config);
    const std::vector<TowerMove> moves  = tower_moves(gcode);
    REQUIRE(!moves.empty());

    const double layer_height = 0.3;
    const double travel_f     = 60. * 500.;

    // The case only means something if the tower really was compacted below the object.
    const bool compacted = std::any_of(moves.begin(), moves.end(),
                                       [](const TowerMove &m) { return m.z < m.layer_z - 1e-3; });
    REQUIRE(compacted);

    SECTION("no tower extrusion runs at travel feedrate")
    {
        // The tower's own speeds top out at 90 mm/s (F5400); the first layer is slower still.
        double worst = 0;
        for (const TowerMove &m : moves)
            worst = std::max(worst, m.f);
        INFO("fastest tower extrusion F" << worst << ", travel F" << travel_f);
        CHECK(worst <= 5400. + 1e-3);
        CHECK(worst < travel_f);
    }

    SECTION("every tower layer sits exactly one layer on top of the last one")
    {
        // Compaction must never leave a gap under a tower layer (printing into air) nor print below it.
        double top = 0;
        for (const TowerMove &m : moves) {
            if (m.z > top + 1e-3) {
                INFO("tower layer at z " << m.z << " (object layer " << m.layer_z << "), tower top was " << top);
                CHECK(m.z <= top + layer_height + 1e-3);
                top = m.z;
            } else {
                INFO("tower extrusion at z " << m.z << " below the tower top " << top);
                CHECK(m.z >= top - 1e-3);
            }
        }
    }

    SECTION("the first tower layer is on the bed and carries the auto brim")
    {
        const double first_z = std::min_element(moves.begin(), moves.end(),
                                                [](const TowerMove &a, const TowerMove &b) { return a.z < b.z; })->z;
        CHECK(std::abs(first_z - 0.3) < 1e-3);
        auto span_at = [&moves](double z) {
            double xmin = 1e9, xmax = -1e9;
            for (const TowerMove &m : moves)
                if (std::abs(m.z - z) < 1e-3) {
                    xmin = std::min(xmin, m.x);
                    xmax = std::max(xmax, m.x);
                }
            return xmax - xmin;
        };
        // The object is 20 mm tall, so the auto brim is 20 / 100 * 8 = 1.6 mm per side: the first
        // layer must reach well past the tower body. Without the brim a rectangle spans exactly 30 mm.
        const double first_span = span_at(first_z);
        INFO("first tower layer spans " << first_span << " mm");
        if (wall == "rectangle")
            CHECK(first_span > 30. + 2.);
        else {
            // A rib tower is squared off: compare with the sixth tower layer, where the brim chamfer
            // (one line narrower per layer) has run out and the ribs have barely tapered.
            const double upper_span = span_at(first_z + 5. * layer_height);
            INFO("sixth tower layer spans " << upper_span << " mm");
            CHECK(upper_span > 0.);
            CHECK(first_span > upper_span + 2.);
        }
    }
}

// The exported G-code minus what legitimately differs between two slices of the same plate: the
// header's generation timestamp and the object label ids, which come from a process-wide counter.
std::vector<std::string> comparable_lines(const std::string &gcode)
{
    std::vector<std::string> out;
    std::istringstream       in(gcode);
    std::string              line;
    while (std::getline(in, line))
        if (line.find("generated by") == std::string::npos && line.find("label id") == std::string::npos)
            out.push_back(line);
    return out;
}

// First line where two slices differ, or -1. Comparing line by line keeps a failure readable.
long long first_difference(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i])
            return (long long) i;
    return a.size() == b.size() ? -1 : (long long) n;
}

DynamicPrintConfig rib_config()
{
    DynamicPrintConfig config = no_sparse_config();
    config.set_deserialize_strict({ { "wipe_tower_wall_type", "rib" }, { "enable_arc_fitting", 1 } });
    return config;
}

} // namespace

TEST_CASE("No-sparse Bambu tower: compacted layers print at tower speed on a supported, brimmed base", "[WipeTower][NoSparseLayers]")
{
    for (const char *wall : { "rectangle", "rib" })
        DYNAMIC_SECTION("wall " << wall) { check_no_sparse_tower(wall); }
}

// ---------------------------------------------------------------------------------------------
// Orca #15917: final purge on the tower's own last layer, not the object's top (mid-air).
// Edge extras: lazy change_layer must not drop Z over the object; no-sparse sits on the last
// printed compacted Z (including sparse layer 0); SEMM rams on the extra layer; non-SEMM
// without multitool ramming skips the final purge (U1).
// ---------------------------------------------------------------------------------------------

namespace {

DynamicPrintConfig final_purge_config(bool semm, bool no_sparse)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        { "layer_height",                   0.3 },
        { "initial_layer_print_height",     0.3 },
        { "wall_loops",                     1 },
        { "sparse_infill_density",          "0%" },
        { "bottom_shell_layers",            2 },
        { "top_shell_layers",               0 },
        { "enable_support",                 false },
        { "skirt_loops",                    0 },
        { "enable_prime_tower",             true },
        { "prime_tower_width",              30 },
        { "wipe_tower_x",                   "140" },
        { "wipe_tower_y",                   "140" },
        { "purge_in_prime_tower",           "1" },
        { "gcode_comments",                 true },
        { "gcode_flavor",                   semm ? "marlin" : "klipper" },
        // Marlin relative-E needs a per-layer G92 E0; without it Print::validate fails.
        { "layer_change_gcode",             "G92 E0" },
        { "single_extruder_multi_material", semm ? "1" : "0" },
        { "enable_filament_ramming",        semm ? "1" : "0" },
        { "wipe_tower_no_sparse_layers",    no_sparse ? "1" : "0" },
        { "filament_multitool_ramming",     "0,0" },
    });
    return config;
}

struct FinalPurgeSlice
{
    std::string gcode;
    double      purge_z;
    bool        has_final_purge;
    bool        final_purge_drop;
    double      obj_xmin, obj_xmax, obj_ymin, obj_ymax;
};

FinalPurgeSlice slice_final_purge_model(Print &print, Model &model, const DynamicPrintConfig &config)
{
    print.apply(model, config);
    print.apply(model, config);
    const StringObjectException err = print.validate();
    INFO(err.string);
    REQUIRE(err.string.empty());
    print.set_status_silent();
    print.process();
    FinalPurgeSlice out;
    out.has_final_purge = print.wipe_tower_data().final_purge && !print.wipe_tower_data().final_purge->gcode.empty();
    out.final_purge_drop = print.wipe_tower_data().final_purge_drop;
    out.purge_z         = out.has_final_purge ? print.wipe_tower_data().final_purge->print_z : 0.;
    out.gcode           = Slic3r::Test::gcode(print);
    return out;
}

FinalPurgeSlice slice_final_purge(const DynamicPrintConfig &config, double size_xy = 10., double size_z = 10.,
                                  Vec3d offset = Vec3d(40., 40., 0.), double filament2_top = 2.0)
{
    Print print;
    Model model;
    ModelObject *object = model.add_object();
    object->name        = "cube.stl";
    object->add_volume(make_cube(size_xy, size_xy, size_z));
    object->add_instance()->set_offset(offset);
    object->ensure_on_bed();
    DynamicPrintConfig range_config;
    range_config.set_key_value("extruder", new ConfigOptionInt(2));
    range_config.set_key_value("layer_height", new ConfigOptionFloat(config.opt_float("layer_height")));
    object->layer_config_ranges[{0.0, filament2_top}].assign_config(std::move(range_config));
    print.auto_assign_extruders(object);
    FinalPurgeSlice out = slice_final_purge_model(print, model, config);
    out.obj_xmin = offset.x();
    out.obj_xmax = offset.x() + size_xy;
    out.obj_ymin = offset.y();
    out.obj_ymax = offset.y() + size_xy;
    return out;
}

// After the last per-layer ;Z: comment, a Z decrease must not start with XY still over the object.
void check_no_z_drop_over_object(const std::string &gcode, double obj_xmin, double obj_xmax, double obj_ymin, double obj_ymax)
{
    const size_t last_z_cmt = gcode.rfind(";Z:");
    REQUIRE(last_z_cmt != std::string::npos);
    std::istringstream in(gcode.substr(last_z_cmt));
    std::string        line;
    double             x = 0, y = 0, z = 0;
    bool               have_xy = false;
    const std::regex   word("([XYZ])(-?[0-9]*\\.?[0-9]+)");
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::string code = line.substr(0, line.find(';'));
        if (!(code.rfind("G1 ", 0) == 0 || code.rfind("G0 ", 0) == 0 || code.rfind("G2 ", 0) == 0 || code.rfind("G3 ", 0) == 0))
            continue;
        double nx = x, ny = y, nz = z;
        bool   has_z = false;
        for (auto it = std::sregex_iterator(code.begin(), code.end(), word); it != std::sregex_iterator(); ++it) {
            const char   axis = (*it)[1].str()[0];
            const double v    = std::atof((*it)[2].str().c_str());
            switch (axis) {
            case 'X': nx = v; break;
            case 'Y': ny = v; break;
            case 'Z': nz = v; has_z = true; break;
            }
        }
        if (has_z && have_xy && nz < z - 1e-3) {
            const bool start_in_object = x + 1e-3 >= obj_xmin && x - 1e-3 <= obj_xmax && y + 1e-3 >= obj_ymin && y - 1e-3 <= obj_ymax;
            INFO("Z drop from " << z << " to " << nz << " starting at XY " << x << "," << y);
            CHECK_FALSE(start_in_object);
        }
        if (std::abs(nx - x) > 1e-9 || std::abs(ny - y) > 1e-9)
            have_xy = true;
        x = nx;
        y = ny;
        z = nz;
    }
}

int count_g1_x_and_e_positive(const std::string &block)
{
    int                count = 0;
    std::istringstream in(block);
    std::string        line;
    const std::regex   word("([XYZE])(-?[0-9]*\\.?[0-9]+)");
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::string code = line.substr(0, line.find(';'));
        if (code.rfind("G1 ", 0) != 0)
            continue;
        bool   has_x = false;
        double e     = 0;
        for (auto it = std::sregex_iterator(code.begin(), code.end(), word); it != std::sregex_iterator(); ++it) {
            const char   axis = (*it)[1].str()[0];
            const double v    = std::atof((*it)[2].str().c_str());
            if (axis == 'X')
                has_x = true;
            else if (axis == 'E')
                e = v;
        }
        if (has_x && e > 1e-6)
            ++count;
    }
    return count;
}

double last_tower_extrusion_z(const std::string &gcode)
{
    const std::vector<TowerMove> moves = tower_moves(gcode);
    if (moves.empty())
        return 0.;
    return std::max_element(moves.begin(), moves.end(), [](const TowerMove &a, const TowerMove &b) { return a.z < b.z; })->z;
}

double z_on_comment_line(const std::string &gcode, const char *needle)
{
    const size_t at = gcode.rfind(needle);
    if (at == std::string::npos)
        return 0.;
    const size_t line_start = gcode.rfind('\n', at);
    const size_t begin      = line_start == std::string::npos ? 0 : line_start + 1;
    const size_t line_end   = gcode.find('\n', at);
    const std::string line  = gcode.substr(begin, (line_end == std::string::npos ? gcode.size() : line_end) - begin);
    const std::string code  = line.substr(0, line.find(';'));
    std::smatch       m;
    if (std::regex_search(code, m, std::regex("Z(-?[0-9]*\\.?[0-9]+)")))
        return std::atof(m[1].str().c_str());
    return 0.;
}

} // namespace

TEST_CASE("The final unload prints on the tower's last layer, never mid-air", "[WipeTower][GCode][FinalPurge]")
{
    const bool no_sparse = GENERATE(true, false);
    DYNAMIC_SECTION((no_sparse ? "SEMM no-sparse" : "SEMM sparse-on"))
    {
        const FinalPurgeSlice slice = slice_final_purge(final_purge_config(true, no_sparse));
        REQUIRE(slice.has_final_purge);
        CHECK(slice.final_purge_drop);
        // Filament 2 only on [0, 2] of a 10 mm cube: the tower's last active layer is ~2 mm,
        // not the object's 10 mm top.
        CHECK(slice.purge_z < 5.0);
        CHECK(slice.purge_z > 0.2);

        const size_t last_layer_z_comment = slice.gcode.rfind(";Z:");
        REQUIRE(last_layer_z_comment != std::string::npos);

        const size_t unload = slice.gcode.find("; CP TOOLCHANGE UNLOAD", last_layer_z_comment);
        REQUIRE(unload != std::string::npos);

        check_no_z_drop_over_object(slice.gcode, slice.obj_xmin, slice.obj_xmax, slice.obj_ymin, slice.obj_ymax);
    }
}

TEST_CASE("Final SEMM ramming extrudes X+E on the extra tower layer", "[WipeTower][GCode][FinalPurge]")
{
    const FinalPurgeSlice slice = slice_final_purge(final_purge_config(true, false));
    REQUIRE(slice.has_final_purge);
    REQUIRE(slice.final_purge_drop);
    const size_t last_unload = slice.gcode.rfind("; CP TOOLCHANGE UNLOAD");
    REQUIRE(last_unload != std::string::npos);
    const size_t ramming_start = slice.gcode.find("Ramming start", last_unload);
    REQUIRE(ramming_start != std::string::npos);
    const int xe = count_g1_x_and_e_positive(slice.gcode.substr(last_unload, ramming_start - last_unload));
    INFO("G1 X+E>0 count between last UNLOAD and Ramming start: " << xe);
    CHECK(xe > 0);
}

TEST_CASE("A tall object near the tower blocks the final-purge Z drop", "[WipeTower][GCode][FinalPurge][Stagger]")
{
    const bool stagger = GENERATE(false, true);
    DYNAMIC_SECTION((stagger ? "stagger on" : "stagger off"))
    {
        DynamicPrintConfig config = final_purge_config(true, false);
        config.set_deserialize_strict({
            { "extruder_clearance_radius",            72.5 },
            { "extruder_clearance_height_to_rod",     27.5 },
            { "nozzle_height",                        2.5 },
            { "prime_tower_width",                    30 },
            { "wipe_tower_x",                         "140" },
            { "wipe_tower_y",                         "140" },
            { "wipe_tower_stagger_toolchange_start",  stagger ? "1" : "0" },
        });
        // 20 mm cube sharing the tower's Y band, 40 mm away in X: inside the 72.5 mm toolhead radius.
        const FinalPurgeSlice slice = slice_final_purge(config, 20., 20., Vec3d(80., 140., 0.), 2.0);
        REQUIRE(slice.has_final_purge);
        CHECK_FALSE(slice.final_purge_drop);
        CHECK(slice.gcode.find("Travel down to the last wipe tower layer") == std::string::npos);
        CHECK(slice.gcode.find("Travel to final purge") == std::string::npos);
        // N1: without the on-tower flag, the extra set_layer still trips is_over_tower_height, so
        // ramming must not extrude X+E at object-top height. Stagger must not add a Y hop that
        // then rams (force_travel is gone; finalize unloads at current Z when drop is blocked).
        const size_t last_unload = slice.gcode.rfind("; CP TOOLCHANGE UNLOAD");
        REQUIRE(last_unload != std::string::npos);
        size_t block_end = slice.gcode.find("Ramming start", last_unload);
        if (block_end == std::string::npos)
            block_end = slice.gcode.size();
        CHECK(count_g1_x_and_e_positive(slice.gcode.substr(last_unload, block_end - last_unload)) == 0);
    }
}

TEST_CASE("No-sparse final purge Z sits one layer above the last printed tower layer, including sparse layer 0",
          "[WipeTower][GCode][FinalPurge]")
{
    DynamicPrintConfig config = final_purge_config(true, true);
    config.set_deserialize_strict({
        { "layer_height",               0.16 },
        { "initial_layer_print_height", 0.32 },
    });
    const FinalPurgeSlice slice = slice_final_purge(config, 10., 10., Vec3d(40., 40., 0.), 2.0);
    REQUIRE(slice.has_final_purge);
    const double last_z = last_tower_extrusion_z(slice.gcode);
    REQUIRE(last_z > 0.1);
    const double drop_z = z_on_comment_line(slice.gcode, "Travel down to the last wipe tower layer");
    INFO("last printed tower z " << last_z << ", drop z " << drop_z);
    REQUIRE(drop_z > 0.);
    // Layer 0 is sparse (first layer uses only the highest-numbered filament) but the non-BBL
    // emitter still prints it. The extra set_layer sits one regular layer on that printed top,
    // not one first-layer height below it.
    CHECK(std::abs(drop_z - (last_z + 0.16)) < 0.05);
}

TEST_CASE("A tower that reaches the object top does not emit Travel back up", "[WipeTower][GCode][FinalPurge]")
{
    DynamicPrintConfig config = final_purge_config(true, false);
    Print              print;
    Model              model;
    auto add_cube = [&](const char *name, double x, int extruder) {
        ModelObject *object = model.add_object();
        object->name        = name;
        object->add_volume(make_cube(10., 10., 6.));
        object->add_instance()->set_offset(Vec3d(x, 40., 0.));
        object->ensure_on_bed();
        object->config.set("extruder", extruder);
    };
    add_cube("cube_a.stl", 40., 1);
    add_cube("cube_b.stl", 70., 2);
    print.auto_assign_extruders(model.objects.front());
    print.auto_assign_extruders(model.objects.back());
    const FinalPurgeSlice slice = slice_final_purge_model(print, model, config);
    REQUIRE(slice.has_final_purge);
    const size_t last_unload = slice.gcode.rfind("; CP TOOLCHANGE UNLOAD");
    REQUIRE(last_unload != std::string::npos);
    CHECK(slice.gcode.find("Travel back up to the topmost object layer.", last_unload) == std::string::npos);
}

TEST_CASE("Non-SEMM without multitool ramming skips the final purge", "[WipeTower][GCode][FinalPurge]")
{
    const FinalPurgeSlice slice = slice_final_purge(final_purge_config(false, false));
    CHECK_FALSE(slice.has_final_purge);
    CHECK_FALSE(slice.final_purge_drop);
    CHECK(slice.gcode.find("Travel to final purge") == std::string::npos);
    CHECK(slice.gcode.find("Travel down to the last wipe tower layer") == std::string::npos);
}

TEST_CASE("Snapmaker U1 system profile skips the final purge", "[WipeTower][GCode][FinalPurge]")
{
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() /
                                            boost::filesystem::unique_path("u1_final_purge_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    ScopeGuard restore_data_dir([&] {
        set_data_dir(saved_data_dir);
        boost::system::error_code ec;
        boost::filesystem::remove_all(scratch, ec);
    });
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    // Inherit flattening still needs the Orca filament library plus the Snapmaker vendor;
    // there is no per-preset load API. Selecting the U1 printer/process/filament below is
    // what the test actually uses.
    static std::unique_ptr<PresetBundle> library;
    if (!library) {
        library = std::make_unique<PresetBundle>();
        library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                               ForwardCompatibilitySubstitutionRule::EnableSilent);
    }
    PresetBundle bundle;
    bundle.load_vendor_configs_from_json(profiles, "Snapmaker", PresetBundle::LoadSystem,
                                         ForwardCompatibilitySubstitutionRule::EnableSilent, library.get());
    REQUIRE(bundle.printers.select_preset_by_name("Snapmaker U1 (0.4 nozzle)", true));
    REQUIRE(bundle.prints.select_preset_by_name("0.20mm Standard @Snapmaker U1 (0.4 nozzle)", true));
    REQUIRE(bundle.filaments.select_preset_by_name("Generic PLA @U1 0.4 nozzle", true));
    bundle.filament_presets = { "Generic PLA @U1 0.4 nozzle" };
    bundle.set_num_filaments(3, std::vector<std::string>{ "#E01919", "#1943E0", "#19E043" });
    bundle.filament_presets = std::vector<std::string>(3, "Generic PLA @U1 0.4 nozzle");
    DynamicPrintConfig config = bundle.full_config_secure();
    config.set_deserialize_strict({
        { "enable_prime_tower",         "1" },
        { "wipe_tower_x",               30 },
        { "wipe_tower_y",               210 },
        { "wipe_tower_rotation_angle",  0 },
        { "gcode_comments",             true },
        { "layer_change_gcode",         "G92 E0" },
        { "skirt_loops",                0 },
        { "enable_support",             false },
    });
    REQUIRE(config.opt_bool("single_extruder_multi_material") == false);
    REQUIRE(config.opt_bool("enable_filament_ramming") == false);

    Print print;
    Model model;
    for (int i = 0; i < 3; ++i) {
        ModelObject *object = model.add_object();
        object->name        = "cube" + std::to_string(i);
        object->add_volume(make_cube(10., 10., 6.));
        object->config.set("extruder", i + 1);
        object->add_instance()->set_offset(Vec3d(40. + 25. * i, 40., 0.));
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
    }
    print.is_BBL_printer() = false;
    const FinalPurgeSlice slice = slice_final_purge_model(print, model, config);
    CHECK_FALSE(slice.has_final_purge);
    CHECK_FALSE(slice.final_purge_drop);
    CHECK(slice.gcode.find("Travel to final purge") == std::string::npos);
    CHECK(slice.gcode.find("Travel down to the last wipe tower layer") == std::string::npos);
}

TEST_CASE("Creality K3 system profile emits T0-T3 tool changes", "[WipeTower][GCode][CrealityK3]")
{
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() /
                                            boost::filesystem::unique_path("k3_toolchange_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    ScopeGuard restore_data_dir([&] {
        set_data_dir(saved_data_dir);
        boost::system::error_code ec;
        boost::filesystem::remove_all(scratch, ec);
    });
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    static std::unique_ptr<PresetBundle> library;
    if (!library) {
        library = std::make_unique<PresetBundle>();
        library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                               ForwardCompatibilitySubstitutionRule::EnableSilent);
    }
    PresetBundle bundle;
    bundle.load_vendor_configs_from_json(profiles, "Creality", PresetBundle::LoadSystem,
                                         ForwardCompatibilitySubstitutionRule::EnableSilent, library.get());
    REQUIRE(bundle.printers.select_preset_by_name("Creality K3 0.4 nozzle", true));
    REQUIRE(bundle.prints.select_preset_by_name("0.20mm Standard @Creality K3 0.4 nozzle", true));
    REQUIRE(bundle.filaments.select_preset_by_name("Generic PLA @Creality K3", true));
    bundle.set_num_filaments(4, std::vector<std::string>{"#E01919", "#1943E0", "#19E043", "#E0E019"});
    bundle.filament_presets = std::vector<std::string>(4, "Generic PLA @Creality K3");
    DynamicPrintConfig config = bundle.full_config_secure();
    config.set_deserialize_strict({
        { "enable_prime_tower",        "1" },
        { "wipe_tower_x",              30 },
        { "wipe_tower_y",              30 },
        { "wipe_tower_rotation_angle", 0 },
        { "gcode_comments",            true },
        { "layer_change_gcode",        "G92 E0" },
        { "skirt_loops",               0 },
        { "enable_support",            false },
    });
    REQUIRE(config.opt_bool("single_extruder_multi_material") == false);
    REQUIRE(config.opt_bool("enable_filament_ramming") == false);
    REQUIRE(config.opt_bool("tool_change_temprature_wait") == false);
    REQUIRE(config.option<ConfigOptionFloats>("nozzle_diameter")->values.size() == 4);

    Print print;
    Model model;
    for (int i = 0; i < 4; ++i) {
        ModelObject *object = model.add_object();
        object->name        = "cube" + std::to_string(i);
        object->add_volume(make_cube(10., 10., 6.));
        object->config.set("extruder", i + 1);
        object->add_instance()->set_offset(Vec3d(40. + 25. * i, 40., 0.));
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
    }
    print.is_BBL_printer() = false;
    const FinalPurgeSlice slice = slice_final_purge_model(print, model, config);
    REQUIRE_FALSE(slice.gcode.empty());
    // Creality Print K3 machine_start_gcode (START_PRINT + hardcoded 140/220 warmup).
    CHECK(slice.gcode.find("START_PRINT EXTRUDER_TEMP=") != std::string::npos);
    CHECK(slice.gcode.find("BED_TEMP=") != std::string::npos);
    CHECK(slice.gcode.find("M104 S140") != std::string::npos);
    CHECK(slice.gcode.find("M104 S220 T") != std::string::npos);
    CHECK(slice.gcode.find("G1 X120 Y0 E15") != std::string::npos);
    CHECK(slice.gcode.find("END_PRINT") != std::string::npos);
    CHECK(slice.gcode.find("G1 X130 Y255") != std::string::npos);
    // Independent toolchanger: filament i → physical Ti (U1-style, no filament_is_mixed).
    auto has_tool = [&](const char *tn) {
        const std::string needle = std::string("\n") + tn;
        return slice.gcode.find(needle + "\n") != std::string::npos ||
               slice.gcode.find(needle + " ") != std::string::npos ||
               slice.gcode.find(needle + "\r") != std::string::npos;
    };
    CHECK(has_tool("T0"));
    CHECK(has_tool("T1"));
    CHECK(has_tool("T2"));
    CHECK(has_tool("T3"));
    // Ported Creality Print change_filament_gcode: Y dodge at F30000 or I0.86 spiral.
    const bool k3_change = slice.gcode.find(" F30000") != std::string::npos &&
                           (slice.gcode.find("I0.86 J0.86 P1") != std::string::npos ||
                            slice.gcode.find("G1 Y") != std::string::npos);
    CHECK(k3_change);
    // Profile-gated M109 fix: tool_change_temprature_wait=0 → M104, not wait-M109.
    CHECK(slice.gcode.find("set nozzle temperature and wait for it to be reached") == std::string::npos);
    CHECK(slice.gcode.find("M109") == std::string::npos);
}

namespace {

DynamicPrintConfig n2_clearance_config(bool no_sparse, bool stagger = false)
{
    DynamicPrintConfig config = final_purge_config(true, no_sparse);
    config.set_deserialize_strict({
        { "extruder_clearance_radius",           72.5 },
        { "extruder_clearance_height_to_rod",    27.5 },
        { "nozzle_height",                       2.5 },
        { "wipe_tower_stagger_toolchange_start", stagger ? "1" : "0" },
    });
    return config;
}

void add_final_purge_cube(Model &model, Print &print, double size_xy, double size_z, const Vec3d &offset,
                          double filament2_top, double layer_height)
{
    ModelObject *object = model.add_object();
    object->name        = "cube.stl";
    object->add_volume(make_cube(size_xy, size_xy, size_z));
    object->add_instance()->set_offset(offset);
    object->ensure_on_bed();
    DynamicPrintConfig range_config;
    range_config.set_key_value("extruder", new ConfigOptionInt(2));
    range_config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    object->layer_config_ranges[{0.0, filament2_top}].assign_config(std::move(range_config));
    print.auto_assign_extruders(object);
}

int last_unload_xe(const std::string &gcode)
{
    const size_t last_unload = gcode.rfind("; CP TOOLCHANGE UNLOAD");
    if (last_unload == std::string::npos)
        return -1;
    size_t block_end = gcode.find("Ramming start", last_unload);
    if (block_end == std::string::npos)
        block_end = gcode.size();
    return count_g1_x_and_e_positive(gcode.substr(last_unload, block_end - last_unload));
}

void check_no_drop_no_ram(const FinalPurgeSlice &slice)
{
    CHECK_FALSE(slice.final_purge_drop);
    CHECK(slice.gcode.find("Travel to final purge") == std::string::npos);
    // Mid-print no-sparse returns also use "Travel down to the last wipe tower layer".
    // Only the suffix after the last object layer is the final-purge drop.
    const size_t last_z_cmt = slice.gcode.rfind(";Z:");
    if (last_z_cmt != std::string::npos)
        CHECK(slice.gcode.find("Travel down to the last wipe tower layer", last_z_cmt) == std::string::npos);
    else
        CHECK(slice.gcode.find("Travel down to the last wipe tower layer") == std::string::npos);
    CHECK(last_unload_xe(slice.gcode) == 0);
}

void check_drop_taken(const FinalPurgeSlice &slice)
{
    REQUIRE(slice.has_final_purge);
    CHECK(slice.final_purge_drop);
    CHECK(slice.gcode.find("Travel down to the last wipe tower layer") != std::string::npos);
}

} // namespace

TEST_CASE("Moving the object next to the tower after a far slice blocks the drop", "[WipeTower][GCode][FinalPurge][Stagger]")
{
    const bool stagger = GENERATE(false, true);
    DYNAMIC_SECTION((stagger ? "stagger on" : "stagger off"))
    {
        DynamicPrintConfig config = n2_clearance_config(false, stagger);
        Print              print;
        Model              model;
        add_final_purge_cube(model, print, 20., 20., Vec3d(40., 40., 0.), 2.0, 0.3);

        const FinalPurgeSlice far_slice = slice_final_purge_model(print, model, config);
        check_drop_taken(far_slice);

        model.objects.front()->instances.front()->set_offset(Vec3d(80., 140., 0.));
        model.objects.front()->ensure_on_bed();
        const FinalPurgeSlice near_slice = slice_final_purge_model(print, model, config);
        REQUIRE(near_slice.has_final_purge);
        check_no_drop_no_ram(near_slice);
    }
}

TEST_CASE("Moving the tower next to the object after a far slice blocks the drop", "[WipeTower][GCode][FinalPurge][Stagger]")
{
    const bool stagger = GENERATE(false, true);
    DYNAMIC_SECTION((stagger ? "stagger on" : "stagger off"))
    {
        DynamicPrintConfig config = n2_clearance_config(false, stagger);
        Print              print;
        Model              model;
        add_final_purge_cube(model, print, 20., 20., Vec3d(40., 40., 0.), 2.0, 0.3);

        const FinalPurgeSlice far_slice = slice_final_purge_model(print, model, config);
        check_drop_taken(far_slice);

        config.set_deserialize_strict({
            { "wipe_tower_x", "100" },
            { "wipe_tower_y", "40" },
        });
        const FinalPurgeSlice near_slice = slice_final_purge_model(print, model, config);
        REQUIRE(near_slice.has_final_purge);
        check_no_drop_no_ram(near_slice);
    }
}

TEST_CASE("Moving the object far from the tower after a near slice restores the drop", "[WipeTower][GCode][FinalPurge][Stagger]")
{
    const bool stagger = GENERATE(false, true);
    DYNAMIC_SECTION((stagger ? "stagger on" : "stagger off"))
    {
        DynamicPrintConfig config = n2_clearance_config(false, stagger);
        Print              print;
        Model              model;
        add_final_purge_cube(model, print, 20., 20., Vec3d(80., 140., 0.), 2.0, 0.3);

        const FinalPurgeSlice near_slice = slice_final_purge_model(print, model, config);
        REQUIRE(near_slice.has_final_purge);
        check_no_drop_no_ram(near_slice);

        model.objects.front()->instances.front()->set_offset(Vec3d(40., 40., 0.));
        model.objects.front()->ensure_on_bed();
        const FinalPurgeSlice far_slice = slice_final_purge_model(print, model, config);
        check_drop_taken(far_slice);
    }
}

TEST_CASE("A one-layer tower with a blocked drop suppresses ramming", "[WipeTower][GCode][FinalPurge]")
{
    // no-sparse + filament 2 only on layer 0 => m_plan.size()==1. Extra set_layer then
    // resets the layer counter (is_first_layer stays true), so the height guard never
    // trips. The no-drop variant must use set_suppress_ramming or a blocked drop still rams.
    //
    // Compacted pre-slice validate() uses full object_top and would reject a near layout;
    // process()'s post-slice check only sees layer 0 (rise ~0) and lets the slice through.
    // Slice far first so validate() passes, then move next to the tower and skip validate.
    DynamicPrintConfig config = n2_clearance_config(true);
    Print              print;
    Model              model;
    add_final_purge_cube(model, print, 20., 20., Vec3d(40., 40., 0.), 0.3, 0.3);

    const FinalPurgeSlice far_slice = slice_final_purge_model(print, model, config);
    REQUIRE(far_slice.has_final_purge);
    CHECK(far_slice.final_purge_drop);

    model.objects.front()->instances.front()->set_offset(Vec3d(80., 140., 0.));
    model.objects.front()->ensure_on_bed();
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    FinalPurgeSlice near_slice;
    near_slice.has_final_purge  = print.wipe_tower_data().final_purge && !print.wipe_tower_data().final_purge->gcode.empty();
    near_slice.final_purge_drop = print.wipe_tower_data().final_purge_drop;
    near_slice.gcode            = Slic3r::Test::gcode(print);
    REQUIRE(near_slice.has_final_purge);
    check_no_drop_no_ram(near_slice);
}

TEST_CASE("No-sparse Print purge Z matches the G-code drop Z", "[WipeTower][GCode][FinalPurge]")
{
    DynamicPrintConfig config = final_purge_config(true, true);
    config.set_deserialize_strict({
        { "layer_height",               0.16 },
        { "initial_layer_print_height", 0.32 },
    });
    Print print;
    Model model;
    add_final_purge_cube(model, print, 10., 10., Vec3d(40., 40., 0.), 2.0, 0.16);
    print.apply(model, config);
    print.apply(model, config);
    REQUIRE(print.validate().string.empty());
    print.set_status_silent();
    print.process();
    REQUIRE(print.wipe_tower_data().final_purge);
    REQUIRE(print.wipe_tower_data().final_purge_drop);
    const double layer_height = print.wipe_tower_data().final_purge_layer_height;
    REQUIRE(layer_height > EPSILON);

    const std::string gcode  = Slic3r::Test::gcode(print);
    const double      last_z = last_tower_extrusion_z(gcode);
    REQUIRE(last_z > 0.1);
    // Finalize adds Print's stored extra-layer height to the last printed tower Z.
    // Search after the last object layer so mid-print compacted descents do not win.
    const size_t last_z_cmt = gcode.rfind(";Z:");
    REQUIRE(last_z_cmt != std::string::npos);
    const double drop_z = z_on_comment_line(gcode.substr(last_z_cmt), "Travel down to the last wipe tower layer");
    INFO("last printed tower z " << last_z << ", Print layer_height " << layer_height << ", G-code drop z " << drop_z);
    REQUIRE(drop_z > 0.);
    CHECK(std::abs(drop_z - (last_z + layer_height)) < 0.05);
}

TEST_CASE("SEMM with ramming off does not drop onto the tower", "[WipeTower][GCode][FinalPurge]")
{
    DynamicPrintConfig config = final_purge_config(true, false);
    config.set_deserialize_strict({ { "enable_filament_ramming", "0" } });
    const FinalPurgeSlice slice = slice_final_purge(config);
    REQUIRE(slice.has_final_purge);
    CHECK_FALSE(slice.final_purge_drop);
    CHECK(slice.gcode.find("Travel down to the last wipe tower layer") == std::string::npos);
    CHECK(slice.gcode.find("Travel to final purge") == std::string::npos);
}

TEST_CASE("Widening the toolhead radius after a far slice blocks the drop", "[WipeTower][GCode][FinalPurge]")
{
    // Soft 3: clearance keys only invalidate G-code. The pick must use the current radius.
    DynamicPrintConfig config = final_purge_config(true, false);
    config.set_deserialize_strict({
        { "extruder_clearance_radius",        20. },
        { "extruder_clearance_height_to_rod", 27.5 },
        { "nozzle_height",                    2.5 },
    });
    Print print;
    Model model;
    add_final_purge_cube(model, print, 20., 20., Vec3d(40., 40., 0.), 2.0, 0.3);

    const FinalPurgeSlice far_slice = slice_final_purge_model(print, model, config);
    check_drop_taken(far_slice);

    config.set_deserialize_strict({ { "extruder_clearance_radius", 200. } });
    const FinalPurgeSlice blocked = slice_final_purge_model(print, model, config);
    REQUIRE(blocked.has_final_purge);
    check_no_drop_no_ram(blocked);
}

// Owner report (H2D, PR #226 test copy, but main does it too): with "no sparse layers" on, slicing
// stopped with "Conflicts of G-code paths have been found ... (WipeTower <-> Assembly)" although
// the tower stood well clear of every object. The plate's second filament only started far up the
// print, so the tower's first real layer (its first tool change) was hundreds of layers up. The
// brim chamfer counted "layers since the first layer" for the sparse layers below it as a negative
// number and added one brim loop per layer down. Those layers are never printed, but their walls
// went into the fake tower the conflict checker tests against: hundreds of loops spreading across
// the bed. Here the second filament starts at 10 mm and a 1 mm chip stands about 10 mm from the tower.
TEST_CASE("No-sparse Bambu tower: layers below the first tool change get no brim loops", "[WipeTower][NoSparseLayers]")
{
    DynamicPrintConfig config = no_sparse_config();
    config.set_deserialize_strict({ { "wipe_tower_wall_type", "rib" } });

    Slic3r::Print print;
    Slic3r::Model model;
    // Filament 1 up to 10 mm, filament 2 above it: no tool change at all below 10 mm.
    ModelObject *stack = model.add_object();
    stack->name        = "stack.stl";
    ModelVolume *lower = stack->add_volume(make_cube(20., 20., 10.));
    ModelVolume *upper = stack->add_volume(make_cube(20., 20., 10.));
    upper->set_offset(lower->get_offset() + Vec3d(0., 0., 10.));
    lower->config.set("extruder", 1);
    upper->config.set("extruder", 2);
    stack->add_instance()->set_offset(Vec3d(40., 40., 0.));
    stack->ensure_on_bed();
    // A low chip beside the tower (x 140..170), about 10 mm off its left side.
    ModelObject *chip = model.add_object();
    chip->name        = "chip.stl";
    chip->add_volume(make_cube(10., 10., 0.9));
    chip->add_instance()->set_offset(Vec3d(120., 145., 0.));
    chip->ensure_on_bed();
    chip->config.set("extruder", 1);

    print.apply(model, config);
    print.is_BBL_printer() = true;
    const StringObjectException err = print.validate();
    INFO(err.string);
    REQUIRE(err.string.empty());
    const std::string gcode = Slic3r::Test::gcode(print);

    // The checker's view of the tower: no layer reaches further than the tower wall at the bed (the
    // widest, as the ribs taper upwards) plus a brim. The wall is the first outline a layer records;
    // the brim loops follow it. The brim is at most 3 mm (the auto brim here is 1.6 mm).
    const std::map<float, Polylines> &walls = print.get_fake_wipe_tower().outer_wall;
    REQUIRE(walls.size() > 10);
    REQUIRE(!walls.begin()->second.empty());
    BoundingBox first = get_extents(walls.begin()->second.front());
    first.offset(scaled(3.5));
    for (const auto &[z, polylines] : walls) {
        BoundingBox b;
        for (const Polyline &pl : polylines)
            b.merge(get_extents(pl));
        INFO("tower wall at z " << z << ": " << polylines.size() << " polylines, " << unscaled(b.size().x()) << " x "
             << unscaled(b.size().y()) << " mm; allowed " << unscaled(first.size().x()) << " x " << unscaled(first.size().y()));
        CHECK(first.contains(b.min));
        CHECK(first.contains(b.max));
    }

    // ...so the chip does not "conflict" with it.
    const ConflictResultOpt conflict = print.get_conflict_result();
    INFO("conflict: " << (conflict ? conflict->_objName1 + " <-> " + conflict->_objName2 + " at " + std::to_string(conflict->_height) : std::string("none")));
    CHECK(!conflict.has_value());

    // And the printed tower never reaches past its first layer either.
    const std::vector<TowerMove> moves = tower_moves(gcode);
    REQUIRE(!moves.empty());
    const double first_z = std::min_element(moves.begin(), moves.end(), [](const TowerMove &a, const TowerMove &b) { return a.z < b.z; })->z;
    double xmin = 1e9, xmax = -1e9;
    for (const TowerMove &m : moves)
        if (std::abs(m.z - first_z) < 1e-3) {
            xmin = std::min(xmin, m.x);
            xmax = std::max(xmax, m.x);
        }
    for (const TowerMove &m : moves) {
        INFO("tower extrusion at z " << m.z << " x " << m.x << ", first layer spans x " << xmin << ".." << xmax);
        CHECK(m.x >= xmin - 0.01);
        CHECK(m.x <= xmax + 0.01);
    }
}

// This case once hung for three hours in its "wall rib" section during a full libslic3r run
// (2026-09-30, main 38ced76d66): the first rib slice of that process passed and the identical
// second one never returned. It could not be reproduced afterwards: about 40 000 rib slices, most of
// them hundreds to a process, with and without the rest of the suite before them and with 1 to 20
// TBB threads, all finished and gave the same G-code. The slice is deterministic, so a second slice of the same plate
// in the same process has to give the same G-code; a difference here is the kind of state carried
// between slices (or race) that could make one of them behave differently.
TEST_CASE("No-sparse Bambu tower: repeated rib slices in one process give the same G-code", "[WipeTower][NoSparseLayers]")
{
    const DynamicPrintConfig       config = rib_config();
    const std::vector<std::string> first  = comparable_lines(slice_bbl(config));
    REQUIRE(!first.empty());
    for (int repeat = 1; repeat <= 2; ++repeat) {
        const std::vector<std::string> again = comparable_lines(slice_bbl(config));
        const long long                diff  = first_difference(first, again);
        INFO("repeat " << repeat << ", first difference at line " << diff << ": \""
                       << (diff >= 0 && diff < (long long) first.size() ? first[size_t(diff)] : std::string("<end>")) << "\" vs \""
                       << (diff >= 0 && diff < (long long) again.size() ? again[size_t(diff)] : std::string("<end>")) << "\"");
        CHECK(diff == -1);
    }
}

// Stress run for the hang above, hidden from the normal suite. Slices the rib case over and over
// with a varying TBB thread cap and checks every result against the first one; one line per
// iteration goes to stdout so a run stopped by the test watchdog (catch_main.hpp) shows where it
// was. Iterations: EDGE_NSS_ITERS (default 200). To start from the state the full suite leaves:
//   libslic3r_tests "~[NotWorking]~[.],[.NoSparseStress]"
TEST_CASE("No-sparse Bambu tower: rib slice stress", "[.NoSparseStress]")
{
    const char *iters_env  = std::getenv("EDGE_NSS_ITERS");
    const int   iterations = iters_env ? std::max(1, std::atoi(iters_env)) : 200;
    const int   max_threads = std::max(1, tbb::this_task_arena::max_concurrency());

    const DynamicPrintConfig       config = rib_config();
    const std::vector<std::string> first  = comparable_lines(slice_bbl(config));
    REQUIRE(!first.empty());
    for (int i = 0; i < iterations; ++i) {
        // Full concurrency every other iteration, otherwise 1..max_threads.
        const int threads = (i % 2 == 0) ? max_threads : 1 + (i * 7) % max_threads;
        std::cout << "rib slice " << i << " of " << iterations << ", " << threads << " threads" << std::endl;
        tbb::global_control            cap(tbb::global_control::max_allowed_parallelism, size_t(threads));
        const std::vector<std::string> again = comparable_lines(slice_bbl(config));
        const long long                diff  = first_difference(first, again);
        INFO("iteration " << i << ", first difference at line " << diff);
        REQUIRE(diff == -1);
    }
}
