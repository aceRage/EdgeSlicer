#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "test_data.hpp"

#include <boost/filesystem.hpp>

#include <cmath>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// A filament change on a Bambu printer with several nozzles flushes for what the nozzle being taken LAST HELD: nothing
// when it already holds the new filament or nothing yet, the flush-matrix volume from that filament to the new one
// otherwise (Bambu Studio's NozzleStatusRecorder, LayeredNozzleGroupResult::estimate_seq_flush_weight). A change on one
// nozzle keeps the flush from the outgoing filament.
// - H2C: two nozzles of the right extruder's rack used to flush the full matrix (M620.10 L) on every change between
//   them, also when the nozzle taken already held the new filament.
// - H2D: a change onto the other extruder used to write L0 also when that nozzle last held another filament.
// The prime tower's purge per change (ToolChangeResult::purge_volume) and the G-code's M620.10 L follow the same rule.

namespace {

PresetBundle &held_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    if (bundle)
        return *bundle;
    const std::string             saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("flushheld_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    auto              library  = std::make_unique<PresetBundle>();
    library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                           ForwardCompatibilitySubstitutionRule::EnableSilent);
    bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                          library.get());
    set_data_dir(saved_data_dir);
    return *bundle;
}

struct Machine
{
    const char *printer;
    const char *process;
    const char *filament;
};

const Machine H2C{ "Bambu Lab H2C 0.4 nozzle", "0.20mm Standard @BBL H2C", "Bambu PLA Basic @BBL H2C" };
const Machine H2D{ "Bambu Lab H2D 0.4 nozzle", "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D" };
const Machine X1C{ "Bambu Lab X1 Carbon 0.4 nozzle", "0.20mm Standard @BBL X1C", "Bambu PLA Basic @BBL X1C" };

// A flush matrix with a distinct volume for every pair, so the G-code tells which pair a flush was for.
std::string flush_matrix(size_t n)
{
    std::ostringstream ss;
    for (size_t from = 0; from < n; ++from)
        for (size_t to = 0; to < n; ++to)
            ss << (from == 0 && to == 0 ? "" : ",") << (from == to ? 0 : 300 + 100 * from + 10 * to);
    return ss.str();
}

double matrix_volume(size_t from, size_t to) { return from == to ? 0. : 300. + 100. * double(from) + 10. * double(to); }

DynamicPrintConfig machine_config(const Machine &m, size_t filaments, bool tower, const std::string &filament_map)
{
    PresetBundle &b = held_bundle();
    REQUIRE(b.printers.select_preset_by_name(m.printer, true));
    REQUIRE(b.prints.select_preset_by_name(m.process, true));
    REQUIRE(b.filaments.select_preset_by_name(m.filament, true));
    b.filament_presets = { m.filament };
    const std::vector<std::string> colours{ "#E01919", "#1943E0", "#19E043", "#E0D019" };
    b.set_num_filaments(unsigned(filaments), std::vector<std::string>(colours.begin(), colours.begin() + filaments));
    b.filament_presets = std::vector<std::string>(filaments, m.filament);
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "enable_prime_tower", tower ? "1" : "0" },
        { "wipe_tower_x", 40. },
        { "wipe_tower_y", 220. },
        { "wipe_tower_rotation_angle", 0 },
        { "flush_volumes_matrix", flush_matrix(filaments) },
        { "flush_multiplier", "1" },
        { "flush_into_infill", "0" },
        { "flush_into_objects", "0" },
        { "flush_into_support", "0" },
        { "gcode_comments", 0 },
    });
    if (!filament_map.empty())
        cfg.set_deserialize_strict({ { "filament_map_mode", "Manual" }, { "filament_map", filament_map } });
    return cfg;
}

// One 20 x 20 x 2 mm cube per filament: every layer prints every filament.
std::string slice(Print &print, Model &model, const DynamicPrintConfig &cfg, size_t filaments)
{
    for (size_t i = 0; i < filaments; ++i) {
        TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.1f));
        ModelObject *object = model.add_object();
        object->name        = "cube" + std::to_string(i);
        object->add_volume(cube);
        object->volumes.front()->name = object->name;
        object->config.set("extruder", int(i + 1));
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(130. + 30. * double(i), 140., 0.));
        object->ensure_on_bed();
    }
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

struct Change
{
    int    from;
    int    to;
    double flush_length; // M620.10 A1 L
};

// The Bambu H2C / H2D change G-code: "M620 S<next>A" starts a change (the start G-code loads the first filament the same
// way), "M620.10 A1 ... L<flush_length> ..." asks the firmware for the flush into the incoming nozzle.
std::vector<Change> changes_of(const std::string &gcode, int &first_filament)
{
    static const std::regex load(R"(^M620 S(\d+)A\b.*)");
    static const std::regex flush(R"(^M620\.10 A1 .*\bL([0-9.eE+-]+)\b.*)");
    std::vector<Change> out;
    std::istringstream  ss(gcode);
    int                 current = -1, next = -1;
    first_filament              = -1;
    for (std::string line; std::getline(ss, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::smatch m;
        if (std::regex_match(line, m, load)) {
            const int f = std::stoi(m[1].str());
            if (first_filament < 0)
                first_filament = f;
            if (next >= 0)
                current = next;
            else
                current = f;
            next = f;
        } else if (std::regex_match(line, m, flush)) {
            out.push_back({ current, next, std::stod(m[1].str()) });
        }
    }
    return out;
}

// The header's default nozzle per filament (Bambu Studio's filament_nozzle_map).
std::vector<int> nozzle_map_of(const std::string &gcode)
{
    static const std::regex header(R"(^; filament_nozzle_map = ([0-9,]+)\s*$)");
    std::istringstream      ss(gcode);
    for (std::string line; std::getline(ss, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::smatch m;
        if (std::regex_match(line, m, header)) {
            std::vector<int> out;
            std::stringstream values(m[1].str());
            for (std::string v; std::getline(values, v, ',');)
                out.push_back(std::stoi(v));
            return out;
        }
    }
    return {};
}

double filament_area(const Print &print, int filament) { return M_PI / 4. * std::pow(print.config().filament_diameter.get_at(size_t(filament)), 2); }

// The flush the held rule asks for, in mm of filament, as the G-code writes it: without a tower the matrix volume; with
// one the tower's purge (less what the filament change's grab extrudes) and at least 100 mm3 when there is any.
double expected_length(const Print &print, bool tower, int from, int to, double volume)
{
    if (tower && volume > 0.) {
        const int extruder = print.config().filament_map.get_at(size_t(to)) - 1;
        if (extruder >= 0 && size_t(extruder) < print.config().grab_length.values.size())
            volume = std::max(0., volume - print.config().grab_length.get_at(size_t(extruder)) * 2.4);
        if (volume > EPSILON)
            volume = std::max(volume, 100.);
    }
    return volume / filament_area(print, to);
}

struct Replay
{
    size_t checked              = 0;
    size_t flushes              = 0;
    size_t flush_from_held      = 0; // flushes from a held filament other than the outgoing one
    size_t no_flush_held_same   = 0; // a nozzle change onto a nozzle already holding the new filament
    size_t cross_nozzle_changes = 0;
};

// Replays the changes with a per-nozzle record of the last filament each nozzle held and checks every M620.10 L.
Replay check_held_rule(const Print &print, const std::string &gcode, bool tower)
{
    int                       first = -1;
    const std::vector<Change> changes = changes_of(gcode, first);
    const std::vector<int>    nozzle  = nozzle_map_of(gcode);
    REQUIRE(first >= 0);
    REQUIRE_FALSE(changes.empty());
    REQUIRE(nozzle.size() >= print.config().filament_colour.values.size());
    std::map<int, int> held;
    held[nozzle[size_t(first)]] = first;
    Replay r;
    for (const Change &c : changes) {
        INFO("change " << c.from << " -> " << c.to << " L" << c.flush_length);
        REQUIRE(c.from >= 0);
        REQUIRE(c.to >= 0);
        double volume = 0.;
        if (nozzle[size_t(c.from)] == nozzle[size_t(c.to)]) {
            volume = matrix_volume(size_t(c.from), size_t(c.to));
        } else {
            ++r.cross_nozzle_changes;
            auto it = held.find(nozzle[size_t(c.to)]);
            if (it != held.end() && it->second != c.to) {
                volume = matrix_volume(size_t(it->second), size_t(c.to));
                if (it->second != c.from)
                    ++r.flush_from_held;
            } else if (it != held.end())
                ++r.no_flush_held_same;
        }
        CHECK_THAT(c.flush_length, WithinAbs(expected_length(print, tower, c.from, c.to, volume), 0.01));
        if (c.flush_length > 0.)
            ++r.flushes;
        held[nozzle[size_t(c.to)]] = c.to;
        ++r.checked;
    }
    return r;
}

} // namespace

TEST_CASE("H2C: two filaments on their own rack nozzles never flush", "[FlushByNozzleHeld][GCode]")
{
    const bool tower = GENERATE(true, false);
    DYNAMIC_SECTION((tower ? "prime tower" : "no prime tower"))
    {
        DynamicPrintConfig cfg = machine_config(H2C, 2, tower, "2,2");
        // Filament 1 in rack slot 1, filament 2 in rack slot 2.
        cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values = { 1, 2 };
        Print             print;
        Model             model;
        const std::string gcode  = slice(print, model, cfg, 2);
        const auto        nozzle = nozzle_map_of(gcode);
        REQUIRE(nozzle.size() == 2);
        REQUIRE(nozzle[0] != nozzle[1]);
        const Replay r = check_held_rule(print, gcode, tower);
        CHECK(r.checked >= 4);
        CHECK(r.flushes == 0);
        CHECK(r.no_flush_held_same > 0);
    }
}

TEST_CASE("H2C: a rack nozzle taking another filament than it held flushes from that filament", "[FlushByNozzleHeld][GCode]")
{
    const bool tower = GENERATE(true, false);
    DYNAMIC_SECTION((tower ? "prime tower" : "no prime tower"))
    {
        DynamicPrintConfig cfg = machine_config(H2C, 3, tower, "2,2,2");
        // Filaments 1 and 3 share rack slot 1, filament 2 has slot 2.
        cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values = { 1, 2, 1 };
        Print             print;
        Model             model;
        const std::string gcode  = slice(print, model, cfg, 3);
        const auto        nozzle = nozzle_map_of(gcode);
        REQUIRE(nozzle.size() == 3);
        REQUIRE(nozzle[0] == nozzle[2]);
        REQUIRE(nozzle[0] != nozzle[1]);
        const Replay r = check_held_rule(print, gcode, tower);
        CHECK(r.flush_from_held > 0);
        CHECK(r.no_flush_held_same > 0);
    }
}

TEST_CASE("H2D: a change onto the other extruder flushes what that nozzle last held", "[FlushByNozzleHeld][GCode]")
{
    const bool tower = GENERATE(true, false);
    DYNAMIC_SECTION((tower ? "prime tower" : "no prime tower"))
    {
        // Filaments 1 and 3 on the left extruder, filament 2 on the right one.
        DynamicPrintConfig cfg = machine_config(H2D, 3, tower, "1,2,1");
        Print              print;
        Model              model;
        const std::string  gcode  = slice(print, model, cfg, 3);
        const auto         nozzle = nozzle_map_of(gcode);
        REQUIRE(nozzle.size() == 3);
        REQUIRE(nozzle[0] == nozzle[2]);
        REQUIRE(nozzle[0] != nozzle[1]);
        const Replay r = check_held_rule(print, gcode, tower);
        CHECK(r.cross_nozzle_changes > 0);
        CHECK(r.flush_from_held > 0);
    }
}

TEST_CASE("H2C: two filaments on one rack nozzle flush from the outgoing filament", "[FlushByNozzleHeld][GCode]")
{
    const bool tower = GENERATE(true, false);
    DYNAMIC_SECTION((tower ? "prime tower" : "no prime tower"))
    {
        DynamicPrintConfig cfg = machine_config(H2C, 2, tower, "2,2");
        cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values = { 1, 1 };
        Print             print;
        Model             model;
        const std::string gcode  = slice(print, model, cfg, 2);
        const auto        nozzle = nozzle_map_of(gcode);
        REQUIRE(nozzle.size() == 2);
        REQUIRE(nozzle[0] == nozzle[1]);
        const Replay r = check_held_rule(print, gcode, tower);
        CHECK(r.cross_nozzle_changes == 0);
        CHECK(r.flushes == r.checked);
    }
}

TEST_CASE("H2C: four filaments, one on the left nozzle and three in the rack", "[FlushByNozzleHeld][GCode]")
{
    // filament_map 1,2,2,2; the rack slots of filaments 2..4 (0: filament 1 is on the left extruder, which has no rack).
    struct Layout
    {
        const char      *name;
        std::vector<int> slots;
    };
    const Layout layout = GENERATE(Layout{ "slots 1,2,3", { 0, 1, 2, 3 } }, Layout{ "slots 1,1,1", { 0, 1, 1, 1 } },
                                   Layout{ "slots 1,1,2", { 0, 1, 1, 2 } });
    const bool   tower  = GENERATE(true, false);
    DYNAMIC_SECTION(layout.name << (tower ? ", prime tower" : ", no prime tower"))
    {
        DynamicPrintConfig cfg = machine_config(H2C, 4, tower, "1,2,2,2");
        cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values = layout.slots;
        // Every layer from the second prints 2, 1, 3, 4: filament 3 goes into the rack nozzle filament 2 left a moment
        // before, coming from filament 1 on the other extruder. The flush-minimising order would print each nozzle's
        // filaments together and never re-enter a shared nozzle on another filament (PR 1169), so the plate fixes the
        // order to make the re-entry happen.
        cfg.set_deserialize_strict({ { "other_layers_print_sequence", "2,100,2,1,3,4" }, { "other_layers_print_sequence_nums", 1 } });
        Print             print;
        Model             model;
        const std::string gcode  = slice(print, model, cfg, 4);
        const auto        nozzle = nozzle_map_of(gcode);
        REQUIRE(nozzle.size() == 4);
        for (size_t a = 1; a < 4; ++a)
            for (size_t b = 1; b < 4; ++b)
                CHECK((nozzle[a] == nozzle[b]) == (layout.slots[a] == layout.slots[b]));
        const Replay r = check_held_rule(print, gcode, tower);
        CHECK(r.cross_nozzle_changes > 0);
        if (layout.slots == std::vector<int>{ 0, 1, 2, 3 }) {
            // Every filament has a nozzle of its own: nothing ever flushes.
            CHECK(r.flushes == 0);
        } else {
            // A change onto the shared rack nozzle flushes from the filament it holds, not from the outgoing one.
            CHECK(r.flush_from_held > 0);
        }
    }
}

TEST_CASE("Single nozzle with AMS: every change flushes from the outgoing filament", "[FlushByNozzleHeld][GCode]")
{
    // X1 Carbon: one nozzle, the AMS feeds two filaments. Its change G-code writes the flush as flush_length_1..4
    // extrusions between "M620 S<next>A" and "M621 S<next>A"; each change has to flush.
    DynamicPrintConfig cfg = machine_config(X1C, 2, true, "");
    Print              print;
    Model              model;
    const std::string  gcode = slice(print, model, cfg, 2);
    std::istringstream ss(gcode);
    static const std::regex load(R"(^M620 S(\d+)A\b.*)");
    static const std::regex unload(R"(^M621 S(\d+)A\b.*)");
    static const std::regex extrude(R"(^G1 E([0-9.]+) F50\b.*)");
    int    changes = 0, flushed = 0, loaded = -1;
    bool   in_change = false;
    double flush     = 0.;
    for (std::string line; std::getline(ss, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::smatch m;
        if (std::regex_match(line, m, load)) {
            // A change to another filament; not the start G-code's first load, a re-select of the loaded filament or
            // the end G-code's unload (S255).
            const int filament = std::stoi(m[1].str());
            in_change          = loaded >= 0 && filament != loaded && filament < 255;
            if (filament < 255)
                loaded = filament;
            flush = 0.;
        } else if (in_change && std::regex_match(line, m, extrude)) {
            flush += std::stod(m[1].str());
        } else if (in_change && std::regex_match(line, m, unload)) {
            in_change = false;
            ++changes;
            if (flush > 0.)
                ++flushed;
        }
    }
    CHECK(changes >= 4);
    CHECK(flushed == changes);
}

// ---- H3C, print by object ------------------------------------------------------------------------------------------
// The H3C has one extruder whose nine rack hotends count as extruders (handle_hotend_as_extruder). A multi-colour plate
// printed by object got no hotend grouping (the by-object grouping shim needs two extruders): a one-entry
// filament_nozzle_map, no pre-heat / pre-cool of the rack hotends, while the printer still put the colours in separate
// hotends (owner's H3C stopped at 97 %, tests: h3c_stuck, 2026-11-23). Such a plate is now grouped as by layer.

namespace {

const Machine H3C{ "Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Bambu PLA Basic @BBL H3C" };

DynamicPrintConfig h3c_config(size_t filaments, bool tower, const char *sequence)
{
    PresetBundle &b = held_bundle();
    REQUIRE(b.printers.select_preset_by_name(H3C.printer, true));
    REQUIRE(b.prints.select_preset_by_name(H3C.process, true));
    REQUIRE(b.filaments.select_preset_by_name(H3C.filament, true));
    b.filament_presets = { H3C.filament };
    const std::vector<std::string> colours{ "#E01919", "#1943E0", "#19E043", "#E0D019" };
    b.set_num_filaments(unsigned(filaments), std::vector<std::string>(colours.begin(), colours.begin() + filaments));
    b.filament_presets = std::vector<std::string>(filaments, H3C.filament);
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "enable_prime_tower", tower ? "1" : "0" },
        { "wipe_tower_x", 40. },
        { "wipe_tower_y", 220. },
        { "wipe_tower_rotation_angle", 0 },
        { "flush_volumes_matrix", flush_matrix(filaments) },
        { "flush_multiplier", "1" },
        { "flush_into_infill", "0" },
        { "flush_into_objects", "0" },
        { "flush_into_support", "0" },
        { "gcode_comments", 0 },
        { "print_sequence", sequence },
    });
    return cfg;
}

// Two-colour 20 x 20 x 2 mm cubes, 60 mm apart: object i has walls in filament walls[i] and infill in infills[i] (1-based),
// so every layer of every object changes colour.
std::string slice_two_colour_objects(Print &print, Model &model, const DynamicPrintConfig &cfg, const std::vector<int> &walls,
                                     const std::vector<int> &infills)
{
    for (size_t i = 0; i < walls.size(); ++i) {
        TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.1f));
        ModelObject *object = model.add_object();
        object->name        = "cube" + std::to_string(i);
        object->add_volume(cube);
        object->volumes.front()->name = object->name;
        object->config.set("wall_filament", walls[i]);
        object->config.set("sparse_infill_filament", infills[i]);
        object->config.set("solid_infill_filament", infills[i]);
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(130. + 60. * double(i), 160., 0.));
        object->ensure_on_bed();
    }
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

std::vector<std::string> gcode_lines(const std::string &gcode)
{
    std::vector<std::string> lines;
    std::istringstream       ss(gcode);
    for (std::string line; std::getline(ss, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string header_value(const std::string &gcode, const std::string &key)
{
    for (const std::string &line : gcode_lines(gcode))
        if (line.rfind("; " + key + " = ", 0) == 0)
            return line.substr(key.size() + 5);
    return "<missing>";
}

// Bambu Studio 2.9.1's rack hotend pre-heat ("M632 S<f> N R W" / "M104 I<f> S<t> N0 W ;Multi extruder pre heating" / "M633")
// and pre-cool ("M104 I<f> S<t> N0 C ;Multi extruder pre cooling"), never inside a change_filament_gcode span.
struct HotendLines
{
    size_t pre_heats = 0, pre_cools = 0, m632 = 0, m633 = 0, bad_pre_heats = 0, inside_change = 0, nozzle_change_tags = 0;
};

HotendLines hotend_lines(const std::string &gcode)
{
    static const std::regex  m632(R"(^M632 S(\d+) N R W$)");
    static const std::regex  heat(R"(^M104 I(\d+) S\d+ N0 W ;Multi extruder pre heating$)");
    const std::vector<std::string> lines = gcode_lines(gcode);
    HotendLines h;
    bool        in_change = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &l = lines[i];
        std::smatch        m, m2;
        if (l == "; FilamentChangeGCodeStart")
            in_change = true;
        else if (l == "; FilamentChangeGCodeEnd")
            in_change = false;
        if (l.rfind("; NOZZLE_CHANGE_START", 0) == 0)
            ++h.nozzle_change_tags;
        if (l.rfind("M632 ", 0) == 0)
            ++h.m632;
        if (l == "M633")
            ++h.m633;
        if (std::regex_match(l, m, heat)) {
            ++h.pre_heats;
            if (in_change)
                ++h.inside_change;
            // The pre-heat names the filament the M632 before it prepares, and an M633 closes it.
            if (i == 0 || !std::regex_match(lines[i - 1], m2, m632) || m2[1].str() != m[1].str() || i + 1 >= lines.size() ||
                lines[i + 1] != "M633")
                ++h.bad_pre_heats;
        } else if (l.rfind("M104 I", 0) == 0 && l.find(" N0 C ;Multi extruder pre cooling") != std::string::npos) {
            ++h.pre_cools;
            if (in_change)
                ++h.inside_change;
        }
    }
    return h;
}

// The number of changes that swap the rack hotend (the header's filament -> hotend map).
size_t hotend_swaps(const std::string &gcode)
{
    int                       first   = -1;
    const std::vector<Change> changes = changes_of(gcode, first);
    const std::vector<int>    nozzle  = nozzle_map_of(gcode);
    size_t                    swaps   = 0;
    for (const Change &c : changes)
        if (c.from >= 0 && c.to >= 0 && size_t(c.from) < nozzle.size() && size_t(c.to) < nozzle.size() && nozzle[size_t(c.from)] != nozzle[size_t(c.to)])
            ++swaps;
    return swaps;
}

} // namespace

TEST_CASE("H3C print by object: each colour gets its own rack hotend, pre-heated and pre-cooled", "[FlushByNozzleHeld][GCode][H3CByObject]")
{
    // The owner's plate: two objects, both printing both colours on every layer.
    DynamicPrintConfig cfg = h3c_config(2, false, "by object");
    REQUIRE(cfg.opt_bool("handle_hotend_as_extruder"));
    Print             print;
    Model             model;
    const std::string gcode = slice_two_colour_objects(print, model, cfg, { 1, 1 }, { 2, 2 });

    // The header describes the grouping the body prints with: one extruder, a hotend per filament.
    CHECK(header_value(gcode, "print_sequence") == "by object");
    CHECK(header_value(gcode, "filament_map") == "1,1");
    const std::vector<int> nozzle = nozzle_map_of(gcode);
    REQUIRE(nozzle.size() == 2);
    CHECK(nozzle[0] != nozzle[1]);
    REQUIRE(print.get_layered_nozzle_group_result() != nullptr);
    for (int f = 0; f < 2; ++f)
        CHECK(print.get_layered_nozzle_group_result()->get_nozzle_id(f, -1) == nozzle[size_t(f)]);

    // Every change goes into a hotend that already holds its filament (or none yet): nothing flushes.
    const Replay r = check_held_rule(print, gcode, false);
    CHECK(r.checked >= 10); // both objects, every layer
    CHECK(r.flushes == 0);
    // Every change to another filament swaps the hotend. (The first layer's change_filament_gcode for the filament the start
    // G-code loaded is a change from a filament to itself, as on every H3C plate, by layer too.)
    size_t colour_changes = 0;
    {
        int first = -1;
        for (const Change &c : changes_of(gcode, first))
            if (c.from != c.to)
                ++colour_changes;
    }
    CHECK(colour_changes + 1 == r.checked);
    CHECK(r.cross_nozzle_changes == colour_changes);
    CHECK(r.no_flush_held_same > 0);

    // Each hotend swap after the first gets its pre-heat (as the same plate by layer: 98 pre-heats for 99 swaps), each
    // wrapped in M632 / M633; the idle hotend is pre-cooled.
    const HotendLines h     = hotend_lines(gcode);
    const size_t      swaps = hotend_swaps(gcode);
    INFO("swaps " << swaps << ", pre-heats " << h.pre_heats << ", pre-cools " << h.pre_cools);
    CHECK(swaps == colour_changes);
    CHECK(h.pre_heats > 0);
    CHECK(h.pre_heats <= swaps);
    CHECK(h.pre_heats + 1 >= swaps);
    CHECK(h.pre_cools > 0);
    CHECK(h.m632 == h.pre_heats);
    CHECK(h.m633 == h.pre_heats);
    CHECK(h.bad_pre_heats == 0);
    CHECK(h.inside_change == 0);
    // As Bambu Studio's set_extruder (and this tree's by-layer plate without a tower), the tower-less change writes no
    // NOZZLE_CHANGE markers; those come from the prime tower's nozzle change.
    CHECK(h.nozzle_change_tags == 0);
}

TEST_CASE("H3C print by object: a hotend shared by two filaments flushes from the filament it holds", "[FlushByNozzleHeld][GCode][H3CByObject]")
{
    // Two rack hotends for three filaments: two of them share one, and each change into it flushes from what it holds.
    DynamicPrintConfig cfg = h3c_config(3, false, "by object");
    cfg.set_deserialize_strict({ { "extruder_max_nozzle_count", "2" } });
    Print             print;
    Model             model;
    const std::string gcode  = slice_two_colour_objects(print, model, cfg, { 1, 3 }, { 2, 2 });
    const auto        nozzle = nozzle_map_of(gcode);
    REQUIRE(nozzle.size() == 3);
    INFO("filament_nozzle_map " << header_value(gcode, "filament_nozzle_map"));
    REQUIRE(std::set<int>(nozzle.begin(), nozzle.end()).size() == 2);
    const Replay r = check_held_rule(print, gcode, false);
    CHECK(r.flushes > 0);
    CHECK(r.no_flush_held_same > 0);
    const HotendLines h = hotend_lines(gcode);
    CHECK(h.pre_heats > 0);
    CHECK(h.bad_pre_heats == 0);
    CHECK(h.inside_change == 0);
}

TEST_CASE("H3C print by layer keeps its hotend grouping and tower nozzle changes", "[FlushByNozzleHeld][GCode][H3CByObject]")
{
    // Control: the same plate by layer with a prime tower (the owner's working print) is grouped by the by-layer pass.
    DynamicPrintConfig cfg = h3c_config(2, true, "by layer");
    Print             print;
    Model             model;
    const std::string gcode  = slice_two_colour_objects(print, model, cfg, { 1, 1 }, { 2, 2 });
    const auto        nozzle = nozzle_map_of(gcode);
    REQUIRE(nozzle.size() == 2);
    CHECK(nozzle[0] != nozzle[1]);
    const Replay r = check_held_rule(print, gcode, true);
    CHECK(r.flushes == 0);
    const HotendLines h = hotend_lines(gcode);
    CHECK(h.pre_heats > 0);
    CHECK(h.bad_pre_heats == 0);
    CHECK(h.nozzle_change_tags > 0);
}

TEST_CASE("Print by object on other printers: no hotend grouping is added", "[FlushByNozzleHeld][GCode][H3CByObject]")
{
    // X1 Carbon (one nozzle, AMS): a by-object plate still has no grouping result and no pre-heat lines.
    {
        DynamicPrintConfig cfg = machine_config(X1C, 2, false, "");
        cfg.set_deserialize_strict({ { "print_sequence", "by object" } });
        Print             print;
        Model             model;
        const std::string gcode = slice_two_colour_objects(print, model, cfg, { 1, 1 }, { 2, 2 });
        CHECK(print.get_layered_nozzle_group_result() == nullptr);
        CHECK(gcode.find(";Multi extruder pre heating") == std::string::npos);
    }
    // H2D (two extruders): a by-object plate keeps the plate's manual map (the existing two-extruder shim).
    {
        DynamicPrintConfig cfg = machine_config(H2D, 2, false, "2,1");
        cfg.set_deserialize_strict({ { "print_sequence", "by object" } });
        Print             print;
        Model             model;
        const std::string gcode = slice_two_colour_objects(print, model, cfg, { 1, 1 }, { 2, 2 });
        CHECK(header_value(gcode, "filament_map") == "2,1");
    }
}

// ---- Print by object: grouping and per-copy plan made by Print::process (per-object prime towers, PR 1) ----------------
// The by-object nozzle grouping (two-extruder Bambu printers, with the rack pins' map move, and the H3C rack) used to run in
// GCode::_do_export. It now runs in Print::process before the prime tower step, which also stores every copy's chained
// tool ordering (ByObjectPlan); the export prints with the stored orderings. The G-code is unchanged.

namespace {

const Machine A1{ "Bambu Lab A1 0.4 nozzle", "0.20mm Standard @BBL A1", "Bambu PLA Basic @BBL A1" };

// slice_two_colour_objects without the export: Print::process only.
void process_two_colour_objects(Print &print, Model &model, const DynamicPrintConfig &cfg, const std::vector<int> &walls,
                                const std::vector<int> &infills)
{
    for (size_t i = 0; i < walls.size(); ++i) {
        TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.1f));
        ModelObject *object = model.add_object();
        object->name        = "cube" + std::to_string(i);
        object->add_volume(cube);
        object->volumes.front()->name = object->name;
        object->config.set("wall_filament", walls[i]);
        object->config.set("sparse_infill_filament", infills[i]);
        object->config.set("solid_infill_filament", infills[i]);
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(130. + 60. * double(i), 160., 0.));
        object->ensure_on_bed();
    }
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    const StringObjectException error = print.validate();
    REQUIRE(error.string.empty());
    print.set_status_silent();
    print.process();
}

DynamicPrintConfig by_object(DynamicPrintConfig cfg)
{
    cfg.set_deserialize_strict({ { "print_sequence", "by object" } });
    return cfg;
}

// H2C, three filaments, rack 0.4,0.6,0.4,0.6 on the right, filament 1 pinned to the 0.6, automatic map 1,2,1 (as
// test_h2c_rack_pins_by_object.cpp): the pin rules move filament 1 to the right extruder.
DynamicPrintConfig h2c_pinned_by_object()
{
    DynamicPrintConfig cfg = by_object(machine_config(H2C, 3, false, ""));
    cfg.set_deserialize_strict({ { "filament_map_mode", "Auto For Flush" }, { "filament_map", "1,2,1" } });
    cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_override", true)->values = { "", "0.4,0.6,0.4,0.6" };
    cfg.option<ConfigOptionFloats>("filament_rack_nozzle_diameter", true)->values  = { 0.6, 0., 0. };
    return cfg;
}

std::vector<std::vector<unsigned int>> layer_extruders(const ToolOrdering &ordering)
{
    std::vector<std::vector<unsigned int>> out;
    for (const LayerTools &lt : ordering.layer_tools())
        out.push_back(lt.extruders);
    return out;
}

// The plan: one block per copy in print order, each chained from the block before it, and every stored ordering equal to
// the one the export would build (ToolOrdering(object, first filament asked for)).
void check_plan(const Print &print, size_t copies)
{
    const ByObjectPlan &plan = print.by_object_plan();
    REQUIRE_FALSE(plan.empty());
    REQUIRE(plan.blocks.size() == copies);
    unsigned int last = (unsigned int) -1;
    for (size_t i = 0; i < plan.blocks.size(); ++i) {
        const ByObjectBlock &block = plan.blocks[i];
        INFO("block " << i);
        REQUIRE(block.instance != nullptr);
        REQUIRE(block.ordering >= 0);
        const ByObjectPlan::Ordering &stored = plan.orderings[size_t(block.ordering)];
        CHECK(stored.object == block.instance->print_object);
        CHECK(block.first_extruder == stored.tool_ordering.first_extruder());
        CHECK(block.last_extruder == stored.tool_ordering.last_extruder());
        CHECK(block.first_extruder != (unsigned int) -1);
        if (i == 0) {
            CHECK(stored.first_extruder == (unsigned int) -1);
        } else {
            // Another object: always a new ordering, chained from the previous block's last filament.
            CHECK(block.rebuilt);
            CHECK(block.chained_from == last);
            CHECK(stored.first_extruder == last);
        }
        const ToolOrdering fresh(*stored.object, stored.first_extruder);
        CHECK(layer_extruders(fresh) == layer_extruders(stored.tool_ordering));
        CHECK(fresh.first_extruder() == stored.tool_ordering.first_extruder());
        CHECK(fresh.last_extruder() == stored.tool_ordering.last_extruder());
        last = block.last_extruder;
    }
}

} // namespace

TEST_CASE("Print by object: process() groups a two-extruder plate before the export", "[ByObjectPlan]")
{
    // H2D, manual map 2,1: the grouping exists after process() alone and puts each filament on its map's extruder.
    DynamicPrintConfig cfg = by_object(machine_config(H2D, 2, false, "2,1"));
    Print              print;
    Model              model;
    process_two_colour_objects(print, model, cfg, { 1, 1 }, { 2, 2 });
    const auto group = print.get_layered_nozzle_group_result();
    REQUIRE(group != nullptr);
    CHECK(group->get_extruder_id(0, -1) == 1);
    CHECK(group->get_extruder_id(1, -1) == 0);
    check_plan(print, 2);

    // The two nozzles carry what they last held from block to block: both filaments print in the first block.
    const ByObjectPlan &plan = print.by_object_plan();
    CHECK(plan.blocks[0].nozzle_status_before.get_nozzle_filament_map().empty());
    const int left = group->get_nozzle_id(1, -1), right = group->get_nozzle_id(0, -1);
    CHECK(left != right);
    CHECK(plan.blocks[1].nozzle_status_before.get_filament_in_nozzle(left) == 1);
    CHECK(plan.blocks[1].nozzle_status_before.get_filament_in_nozzle(right) == 0);
    CHECK(plan.nozzle_status_after.get_filament_in_nozzle(left) == 1);
    CHECK(plan.nozzle_status_after.get_filament_in_nozzle(right) == 0);

    // The export prints with the stored orderings and the header names the grouping's map.
    const std::string gcode = Test::gcode(print);
    CHECK(print.by_object_plan_misses() == 0);
    CHECK(header_value(gcode, "filament_map") == "2,1");
}

TEST_CASE("Print by object: process() writes the map the H2C rack pins move", "[ByObjectPlan][RackPins]")
{
    DynamicPrintConfig cfg = h2c_pinned_by_object();
    Print              print;
    Model              model;
    // Object 1 prints filament 1 (pinned to the rack's 0.6), object 2 filament 2.
    process_two_colour_objects(print, model, cfg, { 1, 2 }, { 1, 2 });
    // Moved by process() itself, before the export.
    CHECK(print.config().filament_map.values == std::vector<int>{ 2, 1, 1 });
    CHECK(print.full_print_config().option<ConfigOptionInts>("filament_map")->values == std::vector<int>{ 2, 1, 1 });
    const auto group = print.get_layered_nozzle_group_result();
    REQUIRE(group != nullptr);
    CHECK(group->get_extruder_id(0, -1) == 1);
    CHECK(group->get_extruder_id(1, -1) == 0);
    check_plan(print, 2);
    // One filament per object: each block prints only its object's filament, so the blocks start with filaments 1 and 2.
    const ByObjectPlan &plan = print.by_object_plan();
    CHECK(plan.blocks[0].first_extruder == plan.blocks[0].last_extruder);
    CHECK(plan.blocks[1].first_extruder == plan.blocks[1].last_extruder);
    CHECK(std::set<unsigned int>{ plan.blocks[0].first_extruder, plan.blocks[1].first_extruder } == std::set<unsigned int>{ 0, 1 });

    const std::string gcode = Test::gcode(print);
    CHECK(print.by_object_plan_misses() == 0);
    CHECK(header_value(gcode, "filament_map") == "2,1,1");
    // A second export of the same slice (process() with nothing to redo) moves nothing twice and gives the same G-code.
    const std::string again = Test::gcode(print);
    CHECK(print.config().filament_map.values == std::vector<int>{ 2, 1, 1 });
    CHECK(header_value(again, "filament_map") == "2,1,1");
    CHECK(print.by_object_plan_misses() == 0);
}

TEST_CASE("Print by object: process() groups an H3C plate onto its rack hotends", "[ByObjectPlan]")
{
    DynamicPrintConfig cfg = h3c_config(2, false, "by object");
    Print              print;
    Model              model;
    process_two_colour_objects(print, model, cfg, { 1, 1 }, { 2, 2 });
    const auto group = print.get_layered_nozzle_group_result();
    REQUIRE(group != nullptr);
    const int hotend0 = group->get_nozzle_id(0, -1), hotend1 = group->get_nozzle_id(1, -1);
    CHECK(hotend0 != hotend1);
    check_plan(print, 2);
    // Every hotend keeps its filament across the blocks.
    const ByObjectPlan &plan = print.by_object_plan();
    CHECK(plan.blocks[1].nozzle_status_before.get_filament_in_nozzle(hotend0) == 0);
    CHECK(plan.blocks[1].nozzle_status_before.get_filament_in_nozzle(hotend1) == 1);

    const std::string gcode  = Test::gcode(print);
    CHECK(print.by_object_plan_misses() == 0);
    const std::vector<int> nozzle = nozzle_map_of(gcode);
    REQUIRE(nozzle.size() == 2);
    CHECK(nozzle[0] == hotend0);
    CHECK(nozzle[1] == hotend1);
}

TEST_CASE("Print by object on one-nozzle printers: a plan, no grouping", "[ByObjectPlan]")
{
    for (const Machine *m : { &X1C, &A1 }) {
        DYNAMIC_SECTION(m->printer)
        {
            DynamicPrintConfig cfg = by_object(machine_config(*m, 2, false, ""));
            Print              print;
            Model              model;
            process_two_colour_objects(print, model, cfg, { 1, 1 }, { 2, 2 });
            CHECK(print.get_layered_nozzle_group_result() == nullptr);
            check_plan(print, 2);
            // The one nozzle holds the last filament of the block before.
            const ByObjectPlan &plan = print.by_object_plan();
            CHECK(plan.blocks[1].nozzle_status_before.get_filament_in_nozzle(0) == int(plan.blocks[0].last_extruder));
            CHECK(plan.nozzle_status_after.get_filament_in_nozzle(0) == int(plan.blocks[1].last_extruder));

            const std::string gcode = Test::gcode(print);
            CHECK(print.by_object_plan_misses() == 0);
            CHECK(print.get_layered_nozzle_group_result() == nullptr);
            CHECK(gcode.find(";Multi extruder pre heating") == std::string::npos);
        }
    }
}

TEST_CASE("Print by object: two copies of one object share the plan's ordering when they can", "[ByObjectPlan]")
{
    // One two-colour object, two copies (X1C): the second copy is the same object; it reuses the ordering only when that
    // starts with the filament the first copy ended with, else it gets one chained from that filament.
    DynamicPrintConfig cfg = by_object(machine_config(X1C, 2, false, ""));
    Print              print;
    Model              model;
    {
        TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.1f));
        ModelObject *object = model.add_object();
        object->name        = "cube";
        object->add_volume(cube);
        object->volumes.front()->name = object->name;
        object->config.set("wall_filament", 1);
        object->config.set("sparse_infill_filament", 2);
        object->config.set("solid_infill_filament", 2);
        object->add_instance();
        object->add_instance();
        object->center_around_origin();
        object->instances[0]->set_offset(Vec3d(110., 160., 0.));
        object->instances[1]->set_offset(Vec3d(190., 160., 0.));
        object->ensure_on_bed();
    }
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    REQUIRE(print.validate().string.empty());
    print.set_status_silent();
    print.process();
    const ByObjectPlan &plan = print.by_object_plan();
    REQUIRE(plan.blocks.size() == 2);
    const ByObjectBlock &b0 = plan.blocks[0], &b1 = plan.blocks[1];
    CHECK(b0.instance->print_object == b1.instance->print_object);
    if (plan.orderings[size_t(b0.ordering)].tool_ordering.first_extruder() == b0.last_extruder) {
        CHECK_FALSE(b1.rebuilt);
        CHECK(b1.ordering == b0.ordering);
    } else {
        CHECK(b1.rebuilt);
        CHECK(b1.chained_from == b0.last_extruder);
    }
    Test::gcode(print);
    CHECK(print.by_object_plan_misses() == 0);
}

TEST_CASE("Print by layer: no by-object plan", "[ByObjectPlan]")
{
    DynamicPrintConfig cfg = machine_config(X1C, 2, true, "");
    Print              print;
    Model              model;
    process_two_colour_objects(print, model, cfg, { 1, 1 }, { 2, 2 });
    CHECK(print.by_object_plan().empty());
    CHECK(print.by_object_plan().blocks.empty());
}
