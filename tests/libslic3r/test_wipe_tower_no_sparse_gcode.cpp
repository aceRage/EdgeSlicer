#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

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
        INFO("purge_z " << slice.purge_z);
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
    const size_t last_unload = slice.gcode.rfind("; CP TOOLCHANGE UNLOAD");
    REQUIRE(last_unload != std::string::npos);
    const size_t ramming_start = slice.gcode.find("Ramming start", last_unload);
    REQUIRE(ramming_start != std::string::npos);
    const int xe = count_g1_x_and_e_positive(slice.gcode.substr(last_unload, ramming_start - last_unload));
    INFO("G1 X+E>0 count between last UNLOAD and Ramming start: " << xe);
    CHECK(xe > 0);
}

TEST_CASE("A tall object near the tower blocks the final-purge Z drop", "[WipeTower][GCode][FinalPurge]")
{
    DynamicPrintConfig config = final_purge_config(true, false);
    config.set_deserialize_strict({
        { "extruder_clearance_radius",          72.5 },
        { "extruder_clearance_height_to_rod",   27.5 },
        { "nozzle_height",                      2.5 },
        { "prime_tower_width",                  30 },
        { "wipe_tower_x",                       "140" },
        { "wipe_tower_y",                       "140" },
    });
    // 20 mm cube sharing the tower's Y band, 40 mm away in X: inside the 72.5 mm toolhead radius.
    const FinalPurgeSlice slice = slice_final_purge(config, 20., 20., Vec3d(80., 140., 0.), 2.0);
    REQUIRE(slice.has_final_purge);
    CHECK(slice.gcode.find("Travel down to the last wipe tower layer") == std::string::npos);
    CHECK(slice.gcode.find("Travel to final purge") == std::string::npos);
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
    CHECK(slice.gcode.find("Travel to final purge") == std::string::npos);
    CHECK(slice.gcode.find("Travel down to the last wipe tower layer") == std::string::npos);
}

TEST_CASE("Snapmaker U1 system profile skips the final purge", "[WipeTower][GCode][FinalPurge]")
{
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() /
                                            boost::filesystem::unique_path("u1_final_purge_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
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
    set_data_dir(saved_data_dir);
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
    CHECK(slice.gcode.find("Travel to final purge") == std::string::npos);
    CHECK(slice.gcode.find("Travel down to the last wipe tower layer") == std::string::npos);
}
