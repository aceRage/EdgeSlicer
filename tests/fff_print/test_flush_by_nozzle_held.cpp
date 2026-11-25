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
