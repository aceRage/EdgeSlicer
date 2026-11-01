// Combined temperature × retraction stringing grid: G-code hooks are gated on calib_mode().
#include <catch2/catch.hpp>

#include <cmath>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/calib.hpp"
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

std::vector<std::string> gcode_lines(const std::string &gcode)
{
    std::vector<std::string> out;
    std::istringstream       in(gcode);
    std::string              line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        out.push_back(line);
    }
    return out;
}

std::vector<std::string> lines_starting_with(const std::string &gcode, const std::string &prefix)
{
    std::vector<std::string> out;
    for (const std::string &line : gcode_lines(gcode))
        if (line.rfind(prefix, 0) == 0)
            out.push_back(line);
    return out;
}

DynamicPrintConfig grid_print_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"gcode_flavor", "marlin"},
                                   {"printable_area", "-150x-150,150x-150,150x150,-150x150"},
                                   {"layer_height", "0.2"},
                                   {"initial_layer_print_height", "0.2"},
                                   {"wall_loops", "2"},
                                   {"top_shell_layers", "0"},
                                   {"bottom_shell_layers", "3"},
                                   {"sparse_infill_density", "0"},
                                   {"skirt_loops", "0"},
                                   {"brim_type", "no_brim"},
                                   {"print_sequence", "by_layer"},
                                   {"enable_prime_tower", "0"},
                                   {"use_firmware_retraction", "0"},
                                   {"use_relative_e_distances", "1"},
                                   {"wipe", "0"},
                                   {"wipe_inward", "0"},
                                   {"retract_when_changing_layer", "1"},
                                   {"retraction_minimum_travel", "1"},
                                   {"reduce_crossing_wall", "0"},
                                   {"gcode_comments", "1"},
                                   {"gcode_label_objects", "1"},
                                   {"retraction_length", "0.8"}});
    return config;
}

Calib_Params small_grid_params()
{
    Calib_Params p;
    p.mode                 = CalibMode::Calib_Temp_Retract_Grid;
    p.start                = 230.;
    p.end                  = 225.;
    p.step                 = 5.;
    p.band_height          = 8.;
    p.band_count           = 2;
    p.retract_lengths      = {0.2, 0.4, 0.6};
    p.wait_for_temperature = true;
    p.print_labels         = false;
    return p;
}

std::string slice_model(Model &model, const DynamicPrintConfig &config, const Calib_Params *calib)
{
    Print print;
    for (ModelObject *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    if (calib != nullptr)
        print.set_calib_params(*calib);
    return gcode(print);
}

std::string slice_grid(const Calib_Params &params, const DynamicPrintConfig &config)
{
    Model model;
    make_stringing_grid(model, params);
    return slice_model(model, config, &params);
}

struct BandEvent
{
    int    band = -1;
    int    temp = 0;
    double z    = 0.;
    std::string cmd;
};

std::vector<BandEvent> parse_band_events(const std::string &gcode)
{
    std::vector<BandEvent> events;
    double                 z = 0.;
    const std::regex       band_re(R"(; Calib_TempRetract: band=(\d+) temp=(-?\d+))");
    const std::vector<std::string> lines = gcode_lines(gcode);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        if (line.rfind(";Z:", 0) == 0 || line.rfind("; Z_HEIGHT:", 0) == 0) {
            const auto pos = line.find_first_of("0123456789+-.");
            if (pos != std::string::npos)
                z = std::stod(line.substr(pos));
        }
        std::smatch m;
        if (std::regex_search(line, m, band_re)) {
            BandEvent ev;
            ev.band = std::stoi(m[1]);
            ev.temp = std::stoi(m[2]);
            ev.z    = z;
            if (i + 1 < lines.size())
                ev.cmd = lines[i + 1];
            events.push_back(ev);
        }
    }
    return events;
}

double parse_e(const std::string &line)
{
    const auto pos = line.find('E');
    if (pos == std::string::npos)
        return 0.;
    return std::stod(line.substr(pos + 1));
}

bool is_retract_move(const std::string &line)
{
    if (line.rfind("G1 ", 0) != 0)
        return false;
    if (line.find('E') == std::string::npos)
        return false;
    if (line.find('X') != std::string::npos || line.find('Y') != std::string::npos || line.find('Z') != std::string::npos)
        return false;
    return parse_e(line) < -1e-6;
}

std::string strip_volatile_gcode(const std::string &gcode)
{
    std::ostringstream out;
    for (const std::string &line : gcode_lines(gcode)) {
        if (line.rfind("; generated by", 0) == 0)
            continue;
        out << line << '\n';
    }
    return out.str();
}

} // namespace

TEST_CASE("A normal slice is byte-identical when the stringing-grid params are unused", "[Calib][TempRetract][Slice]")
{
    const DynamicPrintConfig config = grid_print_config();
    Print                    print_a;
    Print                    print_b;
    Model                    model_a;
    Model                    model_b;
    init_print({TestMesh::cube_20x20x20}, print_a, model_a, config);
    init_print({TestMesh::cube_20x20x20}, print_b, model_b, config);

    Calib_Params unused = small_grid_params();
    unused.mode         = CalibMode::Calib_None;
    print_b.set_calib_params(unused);

    const std::string ga = gcode(print_a);
    const std::string gb = gcode(print_b);
    CHECK(strip_volatile_gcode(ga) == strip_volatile_gcode(gb));
    CHECK(lines_starting_with(ga, "; Calib_TempRetract:").empty());
    CHECK(lines_starting_with(gb, "; Calib_TempRetract:").empty());
}

TEST_CASE("Stringing-grid retraction overrides do not apply when calib_mode is none", "[Calib][TempRetract][Slice]")
{
    Calib_Params params = small_grid_params();
    Model        model;
    make_stringing_grid(model, params);
    params.mode = CalibMode::Calib_None;
    const std::string g = slice_model(model, grid_print_config(), &params);
    CHECK(lines_starting_with(g, "; Calib_TempRetract:").empty());
    CHECK(g.find("G10 ; retract") == std::string::npos);
    CHECK(g.find("M207") == std::string::npos);

    bool saw_preset = false;
    for (const std::string &line : gcode_lines(g)) {
        if (!is_retract_move(line))
            continue;
        CHECK(parse_e(line) == Approx(-0.8).margin(1e-4));
        saw_preset = true;
    }
    CHECK(saw_preset);
}

TEST_CASE("The stringing grid emits one temperature command per band and retraction per column", "[Calib][TempRetract][Slice]")
{
    const Calib_Params       params = small_grid_params();
    const DynamicPrintConfig config = grid_print_config();
    const std::string        g      = slice_grid(params, config);

    const auto bands = parse_band_events(g);
    REQUIRE(bands.size() == 2);
    CHECK(bands[0].band == 0);
    CHECK(bands[0].temp == 230);
    CHECK(bands[1].band == 1);
    CHECK(bands[1].temp == 225);
    CHECK(bands[1].temp < bands[0].temp);
    CHECK(bands[1].z == Approx(stringing_grid_base_height + params.band_height).margin(0.21));
    CHECK(bands[0].cmd.rfind("M109", 0) == 0);
    CHECK(bands[1].cmd.rfind("M109", 0) == 0);
    CHECK(bands[0].cmd.find("S230") != std::string::npos);
    CHECK(bands[1].cmd.find("S225") != std::string::npos);

    const auto col_lines = lines_starting_with(g, "; Calib_TempRetract: col=");
    REQUIRE_FALSE(col_lines.empty());
    std::set<int> cols;
    for (const std::string &line : col_lines) {
        const auto eq = line.find("col=");
        REQUIRE(eq != std::string::npos);
        cols.insert(std::stoi(line.substr(eq + 4)));
    }
    CHECK(cols == std::set<int>{0, 1, 2});

    // Every layer that prints objects names all three columns (by-layer sequence).
    std::set<int> layer_cols;
    int           layers_with_all = 0;
    for (const std::string &line : gcode_lines(g)) {
        if (line.rfind(";Z:", 0) == 0 || line.rfind("; Z_HEIGHT:", 0) == 0) {
            if (layer_cols.size() == 3)
                ++layers_with_all;
            layer_cols.clear();
        }
        if (line.rfind("; Calib_TempRetract: col=", 0) == 0) {
            const auto eq = line.find("col=");
            layer_cols.insert(std::stoi(line.substr(eq + 4)));
        }
    }
    if (layer_cols.size() == 3)
        ++layers_with_all;
    CHECK(layers_with_all >= 2);

    const std::regex col_re(R"(; Calib_TempRetract: col=(\d+) retract=([0-9.]+))");
    int              current_col = -1;
    double           current_L   = 0.;
    int              matched     = 0;
    for (const std::string &line : gcode_lines(g)) {
        std::smatch m;
        if (std::regex_search(line, m, col_re)) {
            current_col = std::stoi(m[1]);
            current_L   = std::stod(m[2]);
            CHECK(current_L == Approx(retract_for_column(params, current_col)));
            continue;
        }
        if (line.rfind("; Calib_TempRetract: restore", 0) == 0)
            current_col = -1;
        if (current_col < 0 || !is_retract_move(line))
            continue;
        CHECK(parse_e(line) == Approx(-current_L).margin(1e-3));
        ++matched;
    }
    CHECK(matched >= 3);

    CHECK(g.find("G10 ; retract") == std::string::npos);
    CHECK(g.find("M207") == std::string::npos);
    CHECK(g.find("CP TOOLCHANGE") == std::string::npos);
    CHECK(g.find("wipe tower") == std::string::npos);

    const auto restore = lines_starting_with(g, "; Calib_TempRetract: restore retract=");
    REQUIRE(restore.size() == 1);
    CHECK(restore.front().find("0.8") != std::string::npos);
}

TEST_CASE("Wait-for-temperature is on by default and can be turned off", "[Calib][TempRetract][Slice]")
{
    Calib_Params params = small_grid_params();
    CHECK(params.wait_for_temperature);

    const std::string wait_g = slice_grid(params, grid_print_config());
    for (const BandEvent &ev : parse_band_events(wait_g)) {
        CHECK(ev.cmd.rfind("M109", 0) == 0);
        CHECK(ev.cmd.rfind("M104", 0) != 0);
    }

    params.wait_for_temperature = false;
    const std::string no_wait_g = slice_grid(params, grid_print_config());
    const auto        no_wait   = parse_band_events(no_wait_g);
    REQUIRE(no_wait.size() == 2);
    for (const BandEvent &ev : no_wait) {
        CHECK(ev.cmd.rfind("M104", 0) == 0);
        CHECK(ev.cmd.find("M109") == std::string::npos);
    }
}

TEST_CASE("Raised labels add a label-tower object only when the toggle is on", "[Calib][TempRetract][Slice]")
{
    Calib_Params params = small_grid_params();
    CHECK_FALSE(params.print_labels);
    const std::string off_g = slice_grid(params, grid_print_config());
    CHECK(off_g.find("SG_label") == std::string::npos);

    params.print_labels = true;
    const std::string on_g = slice_grid(params, grid_print_config());
    CHECK(on_g.find("SG_label") != std::string::npos);
    CHECK(on_g.find("SG_col_00") != std::string::npos);
}
