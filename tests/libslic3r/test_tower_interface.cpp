#include <catch2/catch.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTowerEstimate.hpp"
#include "libslic3r/GCode/WipeTowerInterface.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>

using namespace Slic3r;
namespace fs = boost::filesystem;

// Tower interface options (GCode/WipeTowerInterface.hpp): three behaviours of Bambu Studio's
// enable_tower_interface_features as options of their own, on both prime tower generators.
// A = wipe_tower_interface_temp, B = wipe_tower_interface_run_in, C = wipe_tower_interface_extra_prime.
// These cases slice a real two-filament plate (PLA + PETG, one cube each, so every layer changes
// tool) on this tree's own profiles and look at the tower the generator built.

namespace {

const std::string &profiles_dir()
{
    static const std::string dir = (fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    return dir;
}

// One vendor bundle per vendor, loaded once (they hold thousands of presets), on top of the Orca
// filament library, the way the application loads them.
PresetBundle &vendor_bundle(const std::string &vendor)
{
    static std::map<std::string, PresetBundle *> bundles;
    static PresetBundle                         *library = nullptr;
    if (auto it = bundles.find(vendor); it != bundles.end())
        return *it->second;
    const std::string saved_data_dir = data_dir();
    const fs::path    scratch        = fs::temp_directory_path() / fs::unique_path("tower_interface_%%%%-%%%%");
    fs::create_directories(scratch);
    set_data_dir(scratch.string());
    if (library == nullptr) {
        library = new PresetBundle();
        library->load_vendor_configs_from_json(profiles_dir(), PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                               ForwardCompatibilitySubstitutionRule::EnableSilent);
    }
    auto *b = new PresetBundle();
    b->load_vendor_configs_from_json(profiles_dir(), vendor, PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, library);
    set_data_dir(saved_data_dir);
    bundles[vendor] = b;
    return *b;
}

struct Machine
{
    const char *name;
    const char *vendor;
    const char *printer;
    const char *process;
    const char *filament_a; // PLA
    const char *filament_b; // PETG
    Vec2d       bed_center;
    Vec2d       tower;
    bool        bbl;        // Bambu Lab generator (WipeTower) rather than WipeTower2
    bool        generic;    // a Bambu profile sliced as a non-Bambu printer (WipeTower2)
};

const Machine X1C{ "X1C", "BBL", "Bambu Lab X1 Carbon 0.4 nozzle", "0.20mm Standard @BBL X1C", "Bambu PLA Basic @BBL X1C",
                   "Bambu PETG Basic @BBL X1C", { 128., 128. }, { 30., 180. }, true, false };
const Machine H2D{ "H2D", "BBL", "Bambu Lab H2D 0.4 nozzle", "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D",
                   "Generic PETG @BBL H2D", { 175., 160. }, { 40., 230. }, true, false };
// The X1C profiles sliced as a generic printer: WipeTower2 in single-extruder multi-material mode.
const Machine WT2{ "WT2", "BBL", "Bambu Lab X1 Carbon 0.4 nozzle", "0.20mm Standard @BBL X1C", "Bambu PLA Basic @BBL X1C",
                   "Bambu PETG Basic @BBL X1C", { 128., 128. }, { 30., 180. }, false, true };
// A tool changer: WipeTower2 without ramming.
const Machine U1{ "U1", "Snapmaker", "Snapmaker U1 (0.4 nozzle)", "0.20mm Standard @Snapmaker U1 (0.4 nozzle)",
                  "Generic PLA @U1 0.4 nozzle", "Generic PETG @U1 0.4 nozzle", { 135., 135. }, { 30., 180. }, false, false };

// A 20 x 20 x 10 mm cube on each filament; the tower gets a tool change on every layer.
TriangleMesh cube()
{
    TriangleMesh mesh = Test::mesh(Test::TestMesh::cube_20x20x20);
    mesh.scale(Vec3f(1.f, 1.f, 0.5f));
    return mesh;
}

DynamicPrintConfig machine_config(const Machine &m, std::initializer_list<ConfigBase::SetDeserializeItem> overrides,
                                  bool same_filament = false)
{
    PresetBundle &b = vendor_bundle(m.vendor);
    REQUIRE(b.printers.select_preset_by_name(m.printer, true));
    REQUIRE(b.prints.select_preset_by_name(m.process, true));
    REQUIRE(b.filaments.select_preset_by_name(m.filament_a, true));
    b.filament_presets = { m.filament_a };
    b.set_num_filaments(2, std::vector<std::string>{ "#E01919", "#1943E0" });
    b.filament_presets = { m.filament_a, same_filament ? m.filament_a : m.filament_b };
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "enable_prime_tower", "1" },
        { "wipe_tower_x", m.tower.x() },
        { "wipe_tower_y", m.tower.y() },
        { "wipe_tower_rotation_angle", 0 },
        { "prime_tower_brim_width", 3 },
        { "wipe_tower_wall_type", "rectangle" },
        { "gcode_comments", 0 },
    });
    if (m.generic)
        cfg.set_deserialize_strict({ { "printer_model", "Generic" } });
    // Both filaments on their own extruder where there are several.
    if (cfg.option<ConfigOptionFloats>("nozzle_diameter")->values.size() > 1)
        cfg.set_deserialize_strict({ { "filament_map", "1,2" } });
    cfg.set_deserialize_strict(overrides);
    return cfg;
}

void add_plate(Model &model, const Vec2d &bed_center)
{
    for (int i = 0; i < 2; ++i) {
        ModelObject *object = model.add_object();
        object->name        = i == 0 ? "cubeA" : "cubeB";
        object->add_volume(cube());
        object->volumes.front()->name = object->name;
        object->config.set("extruder", i + 1);
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(bed_center.x() + (i == 0 ? -15. : 15.), bed_center.y(), 0.));
        object->ensure_on_bed();
    }
}

std::string slice(Print &print, Model &model, const DynamicPrintConfig &cfg, const Machine &m)
{
    add_plate(model, m.bed_center);
    print.is_BBL_printer() = m.bbl;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

std::string without_timestamp(std::string gcode)
{
    const size_t at = gcode.find("; generated by ");
    if (at != std::string::npos)
        gcode.erase(at, gcode.find('\n', at) - at);
    return gcode;
}

// The G-code without its timestamp, its object label ids and the settings it lists ("; key = value"), for
// comparing two slices whose settings differ only in values that must not change the moves.
std::string moves_of(const std::string &gcode)
{
    std::istringstream in(without_timestamp(gcode));
    std::string        out, line;
    while (std::getline(in, line)) {
        const size_t eq      = line.find(" = ");
        bool         setting = line.rfind("; ", 0) == 0 && eq != std::string::npos;
        for (size_t i = 2; setting && i < eq; ++i)
            setting = std::isalnum((unsigned char) line[i]) || line[i] == '_';
        // Object label ids count up with every Model the test process makes.
        if (! setting && line.find("label id") == std::string::npos)
            out += line + "\n";
    }
    return out;
}

// The tower's tool changes that are not on its first printed layer, and those that are.
struct TowerChanges
{
    std::vector<const WipeTower::ToolChangeResult *> first_layer, later;
};

TowerChanges tower_changes(const Print &print)
{
    TowerChanges out;
    bool         first = true;
    for (const auto &layer : print.wipe_tower_data().tool_changes) {
        if (layer.empty() || wipe_tower_layer_is_sparse(layer))
            continue;
        for (const WipeTower::ToolChangeResult &tcr : layer)
            if (tcr.initial_tool != tcr.new_tool && tcr.gcode.find("CP TOOLCHANGE START") != std::string::npos)
                (first ? out.first_layer : out.later).push_back(&tcr);
        first = false;
    }
    return out;
}

struct Segment
{
    Vec2f a, b;
};

bool segments_cross(const Segment &s, const Segment &t)
{
    auto orient = [](const Vec2f &p, const Vec2f &q, const Vec2f &r) {
        const float v = (q.x() - p.x()) * (r.y() - p.y()) - (q.y() - p.y()) * (r.x() - p.x());
        return v > 1e-6f ? 1 : (v < -1e-6f ? -1 : 0);
    };
    const int o1 = orient(s.a, s.b, t.a), o2 = orient(s.a, s.b, t.b), o3 = orient(t.a, t.b, s.a), o4 = orient(t.a, t.b, s.b);
    return o1 * o2 < 0 && o3 * o4 < 0;
}

// The extruded segments of every tower layer, in the tower frame.
std::vector<std::vector<Segment>> tower_segments(const Print &print)
{
    std::vector<std::vector<Segment>> out;
    for (const auto &layer : print.wipe_tower_data().tool_changes) {
        std::vector<Segment> segs;
        for (const WipeTower::ToolChangeResult &tcr : layer)
            for (size_t i = 1; i < tcr.extrusions.size(); ++i)
                if (tcr.extrusions[i].width > 0.f)
                    segs.push_back({ tcr.extrusions[i - 1].pos, tcr.extrusions[i].pos });
        out.push_back(std::move(segs));
    }
    return out;
}

BoundingBoxf tower_extent(const Print &print)
{
    BoundingBoxf bbox;
    for (const auto &layer : tower_segments(print))
        for (const Segment &s : layer) {
            bbox.merge(s.a.cast<double>());
            bbox.merge(s.b.cast<double>());
        }
    return bbox;
}

// The pre-slice estimate for the two-filament plate.
WipeTowerFootprint estimate(const DynamicPrintConfig &cfg)
{
    return estimate_wipe_tower_footprint(cfg, resolve_wipe_tower_type(cfg), { 0, 1 }, cfg.opt_float("layer_height"), 10.);
}

std::string temp_path(const std::string &name)
{
    const fs::path dir = fs::temp_directory_path() / ("snorca_tests_tower_interface_" + std::to_string(get_current_pid()));
    fs::create_directories(dir);
    return (dir / name).string();
}

bool load_json(const std::string &path, DynamicPrintConfig &cfg)
{
    ConfigSubstitutionContext          ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    std::map<std::string, std::string> key_values;
    std::string                        reason;
    return cfg.load_from_json(path, ctxt, false, key_values, reason) == 0;
}

DynamicPrintConfig load_json_text(const std::string &name, const std::string &text)
{
    const std::string path = temp_path(name);
    {
        boost::nowide::ofstream f(path, std::ios::binary);
        f << text;
    }
    DynamicPrintConfig cfg;
    REQUIRE(load_json(path, cfg));
    fs::remove(path);
    return cfg;
}

const std::vector<const Machine *> all_machines{ &X1C, &H2D, &WT2, &U1 };

} // namespace

// ---------------------------------------------------------------------------------------------------
// The trigger and the per-filament values
// ---------------------------------------------------------------------------------------------------

TEST_CASE("Tower interface trigger modes", "[TowerInterface]")
{
    using namespace TowerInterface;
    const FilamentKind pla{ "PLA", false }, pla_cf{ "PLA-CF", false }, petg{ "PETG", false }, pctg{ "PCTG", false },
        support_pla{ "PLA", true }, abs{ "ABS", false }, asa{ "ASA", false }, pva{ "PVA", true }, pa{ "PA6-CF", false }, paht{ "PAHT-CF", false };

    CHECK(triggers(titEveryToolChange, pla, pla));
    CHECK(triggers(titEveryToolChange, pla, petg));

    CHECK_FALSE(triggers(titMaterialChange, pla, pla));
    CHECK(triggers(titMaterialChange, pla, petg));
    CHECK(triggers(titMaterialChange, pla, pla_cf));      // filament_type differs
    CHECK(triggers(titMaterialChange, pla, support_pla)); // same type, but a support filament
    CHECK_FALSE(triggers(titMaterialChange, FilamentKind{ "pla ", false }, pla));

    CHECK_FALSE(triggers(titMaterialFamilyChange, pla, pla_cf));
    CHECK_FALSE(triggers(titMaterialFamilyChange, petg, pctg));
    CHECK_FALSE(triggers(titMaterialFamilyChange, abs, asa));
    CHECK_FALSE(triggers(titMaterialFamilyChange, pa, paht));
    CHECK(triggers(titMaterialFamilyChange, pla, petg));
    CHECK(triggers(titMaterialFamilyChange, pla, support_pla));
    CHECK(triggers(titMaterialFamilyChange, pla, pva));

    CHECK(interface_temperature(-1, 250, 220) == 250);
    CHECK(interface_temperature(235, 250, 220) == 235);
    CHECK(interface_temperature(-1, 0, 220) == 220); // no recommended range: the normal temperature
}

TEST_CASE("A run-in start is kept on the printable bed", "[TowerInterface]")
{
    const BoundingBoxf bed(Vec2d(0., 0.), Vec2d(256., 256.));
    auto               identity = [](const Vec2f &p) { return p; };
    CHECK(TowerInterface::clamp_to_bed(Vec2f(50.f, 50.f), Vec2f(-1.f, 0.f), 10.f, identity, bed) == Approx(10.f));
    CHECK(TowerInterface::clamp_to_bed(Vec2f(4.f, 50.f), Vec2f(-1.f, 0.f), 10.f, identity, bed) == Approx(4.f));
    CHECK(TowerInterface::clamp_to_bed(Vec2f(250.f, 50.f), Vec2f(1.f, 0.f), 10.f, identity, bed) == Approx(6.f));
    // Rotated by 90 degrees: -x in the tower frame is -y on the bed.
    auto rotated = [](const Vec2f &p) { return Vec2f(-p.y() + 100.f, p.x() + 3.f); };
    CHECK(TowerInterface::clamp_to_bed(Vec2f(0.f, 0.f), Vec2f(-1.f, 0.f), 10.f, rotated, bed) == Approx(3.f));
}

// ---------------------------------------------------------------------------------------------------
// Off: nothing changes
// ---------------------------------------------------------------------------------------------------

TEST_CASE("With the tower interface options off, the per-filament values change nothing", "[TowerInterface]")
{
    for (const Machine *m : all_machines) {
        DYNAMIC_SECTION(m->name)
        {
            std::string plain, tuned;
            {
                Print print;
                Model model;
                plain = slice(print, model, machine_config(*m, {}), *m);
                CHECK(plain.find("tower interface") == std::string::npos);
            }
            {
                // Every per-filament value set, the process options explicitly off.
                Print print;
                Model model;
                tuned = slice(print, model,
                              machine_config(*m, { { "wipe_tower_interface_temp", "0" },
                                                   { "wipe_tower_interface_run_in", "0" },
                                                   { "wipe_tower_interface_extra_prime", "0" },
                                                   { "wipe_tower_interface_trigger", "every_toolchange" },
                                                   { "filament_tower_interface_print_temp", "280,290" },
                                                   { "filament_tower_interface_pre_extrusion_dist", "25,25" },
                                                   { "filament_tower_interface_pre_extrusion_length", "7,7" } }),
                              *m);
            }
            CHECK(moves_of(plain) == moves_of(tuned));
        }
    }
}

// Hidden: writes the G-code of each machine with every option off to $TOWER_INTERFACE_DUMP_DIR, or
// compares it byte for byte with the files there when $TOWER_INTERFACE_BASELINE_DIR is set (a dump
// made by the build before this change). [.BblRibBaseline] does the same for the rib test's plates.
TEST_CASE("Towers with the options off are unchanged", "[.TowerInterfaceBaseline]")
{
    for (const Machine *m : all_machines) {
        DYNAMIC_SECTION(m->name)
        {
            Print             print;
            Model             model;
            const std::string gcode = without_timestamp(slice(print, model, machine_config(*m, {}), *m));
            if (const char *dump = std::getenv("TOWER_INTERFACE_DUMP_DIR")) {
                boost::nowide::ofstream f((fs::path(dump) / (std::string(m->name) + ".gcode")).string(), std::ios::binary);
                f << gcode;
            }
            if (const char *base = std::getenv("TOWER_INTERFACE_BASELINE_DIR")) {
                boost::nowide::ifstream f((fs::path(base) / (std::string(m->name) + ".gcode")).string(), std::ios::binary);
                REQUIRE(f.good());
                std::stringstream ss;
                ss << f.rdbuf();
                CHECK(ss.str() == gcode);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// A: interface temperature
// ---------------------------------------------------------------------------------------------------

TEST_CASE("Heat up for tower interfaces waits for the interface temperature and restores it", "[TowerInterface]")
{
    for (const Machine *m : all_machines) {
        DYNAMIC_SECTION(m->name)
        {
            Print             print;
            Model             model;
            const auto        cfg   = machine_config(*m, { { "wipe_tower_interface_temp", "1" },
                                                           { "filament_tower_interface_print_temp", "251,262" } });
            const std::string gcode = slice(print, model, cfg, *m);

            const TowerChanges changes = tower_changes(print);
            REQUIRE(! changes.later.empty());
            for (const WipeTower::ToolChangeResult *tcr : changes.first_layer)
                CHECK(tcr->gcode.find("tower interface") == std::string::npos); // never on the first layer
            for (const WipeTower::ToolChangeResult *tcr : changes.later) {
                const std::string &g       = tcr->gcode;
                const bool         to_a    = tcr->new_tool == 0;
                const std::string  wait    = std::string("M109 S") + (to_a ? "251" : "262");
                const size_t       at_wait = g.find(wait);
                const size_t       at_wipe = g.find("; CP TOOLCHANGE WIPE");
                // The normal temperature again, without waiting (it is lower).
                const size_t       at_back = g.find("tower interface done, normal temperature", at_wipe == std::string::npos ? 0 : at_wipe);
                INFO(g);
                REQUIRE(at_wait != std::string::npos);
                REQUIRE(at_wipe != std::string::npos);
                CHECK(at_wait < at_wipe);
                CHECK(at_back != std::string::npos);
                CHECK(at_back < g.find("; CP TOOLCHANGE END"));
                CHECK(g.rfind("M104 S", at_back) > at_wipe);
                // H2D names the nozzle, like Bambu Studio; single-nozzle machines do not.
                const size_t eol = g.find('\n', at_wait);
                CHECK((g.substr(at_wait, eol - at_wait).find(" T") != std::string::npos) == (m == &H2D));
            }
            CHECK(gcode.find("M109 S251") != std::string::npos);
            CHECK(gcode.find("M109 S262") != std::string::npos);
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// B: run-in from outside the tower
// ---------------------------------------------------------------------------------------------------

TEST_CASE("The run-in starts outside the tower and enters only through wall gaps", "[TowerInterface]")
{
    for (const Machine *m : all_machines) {
        DYNAMIC_SECTION(m->name)
        {
            Print      print;
            Model      model;
            const auto cfg = machine_config(*m, { { "wipe_tower_interface_run_in", "1" },
                                                  { "wipe_tower_wall_gap", "1" },
                                                  { "filament_tower_interface_pre_extrusion_dist", "10,10" } });
            const WipeTowerFootprint before = estimate(cfg);
            slice(print, model, cfg, *m);
            const WipeTowerData &wtd = print.wipe_tower_data();
            const float          pw  = float(cfg.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0)) * 1.25f;

            // The reserve covers the run-in, the estimate covers the generator.
            CHECK(wtd.brim_width == Approx(10.f + pw / 2.f).margin(1e-3));
            CHECK(before.brim_width >= wtd.brim_width - 1e-3);
            const BoundingBoxf extent = tower_extent(print);
            CHECK(extent.min.x() >= -wtd.brim_width - 1e-3);
            CHECK(extent.max.x() <= wtd.width + wtd.brim_width + 1e-3);
            CHECK(extent.min.y() >= -wtd.brim_width - 1e-3);
            CHECK(extent.max.y() <= wtd.depth + wtd.brim_width + 1e-3);
            // ... and there is one: 10 mm out on the left (WipeTower2 turns the tower every layer, so both sides).
            CHECK(extent.min.x() == Approx(-10.).margin(0.01));
            if (! m->bbl)
                CHECK(extent.max.x() == Approx(wtd.width + 10.).margin(0.01));

            // Every run-in: a horizontal extrusion that starts outside the body. It crosses the wall
            // line (x = 0 or the tower width) where that layer and the three below have a gap: no
            // other extrusion of those layers crosses its path.
            const auto layers  = tower_segments(print);
            size_t     run_ins = 0;
            for (size_t l = 0; l < layers.size(); ++l)
                for (const Segment &s : layers[l]) {
                    const bool horizontal = std::abs(s.a.y() - s.b.y()) < 1e-3f;
                    const bool left_out   = std::min(s.a.x(), s.b.x()) < -5.f;
                    const bool right_out  = std::max(s.a.x(), s.b.x()) > wtd.width + 5.f;
                    if (! horizontal || ! (left_out || right_out))
                        continue;
                    ++run_ins;
                    const float   y    = s.a.y();
                    const float   wall = left_out ? 0.f : wtd.width;
                    // From the start to half a line inside the wall line: purge lines start a line in.
                    const Segment path{ Vec2f(left_out ? -10.f : wtd.width + 10.f, y), Vec2f(wall + (left_out ? 0.5f : -0.5f) * pw, y) };
                    for (size_t below = 0; below < 4 && below <= l; ++below)
                        for (const Segment &t : layers[l - below]) {
                            const bool same_line = std::abs(t.a.y() - y) < 1e-3f && std::abs(t.b.y() - y) < 1e-3f;
                            if (same_line)
                                continue; // the run-in itself
                            INFO("layer " << l << " run-in at y " << y << " crossed on layer " << (l - below) << " by (" << t.a.x() << ", "
                                          << t.a.y() << ") - (" << t.b.x() << ", " << t.b.y() << ")");
                            CHECK_FALSE(segments_cross(path, t));
                        }
                }
            CHECK(run_ins == tower_changes(print).later.size());
        }
    }
}

TEST_CASE("The run-in needs the wall gaps", "[TowerInterface]")
{
    for (const Machine *m : { &X1C, &U1 }) {
        DYNAMIC_SECTION(m->name)
        {
            std::string with_gap_off, plain;
            {
                Print print;
                Model model;
                with_gap_off = slice(print, model, machine_config(*m, { { "wipe_tower_interface_run_in", "1" }, { "wipe_tower_wall_gap", "0" } }), *m);
                CHECK(print.wipe_tower_data().brim_width == Approx(3.f).margin(0.5)); // the brim only
                CHECK(tower_extent(print).min.x() > -4.);
            }
            {
                Print print;
                Model model;
                plain = slice(print, model, machine_config(*m, { { "wipe_tower_wall_gap", "0" } }), *m);
            }
            CHECK(with_gap_off.find("run-in") == std::string::npos);
            CHECK(moves_of(with_gap_off) == moves_of(plain));
        }
    }
}

TEST_CASE("A tower whose run-in room leaves the bed is refused", "[TowerInterface]")
{
    // The reserved area includes the run-in room (it is reported as the tower brim), and a tower
    // whose reserved area leaves the printable area is refused rather than printed half off the
    // bed. The GUI moves the tower inwards with the same estimate before it gets here.
    Print      print;
    Model      model;
    const auto cfg = machine_config(X1C, { { "wipe_tower_interface_run_in", "1" }, { "wipe_tower_x", 6 } });
    add_plate(model, X1C.bed_center);
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    print.set_status_silent();
    CHECK_THROWS(print.process());
}

// ---------------------------------------------------------------------------------------------------
// C: extra prime
// ---------------------------------------------------------------------------------------------------

TEST_CASE("Extra prime pushes the interface pre-extrusion length plus 2 mm", "[TowerInterface]")
{
    for (const Machine *m : all_machines) {
        for (const bool run_in : { false, true }) {
            DYNAMIC_SECTION(m->name << (run_in ? " with run-in" : ""))
            {
                Print      print;
                Model      model;
                const auto cfg = machine_config(*m, { { "wipe_tower_interface_extra_prime", "1" },
                                                      { "wipe_tower_interface_run_in", run_in ? "1" : "0" },
                                                      { "filament_tower_interface_pre_extrusion_length", "3,3" } });
                slice(print, model, cfg, *m);
                const TowerChanges changes = tower_changes(print);
                REQUIRE(! changes.later.empty());
                for (const WipeTower::ToolChangeResult *tcr : changes.first_layer)
                    CHECK(tcr->gcode.find("G1 E5.0000 F100") == std::string::npos);
                for (const WipeTower::ToolChangeResult *tcr : changes.later) {
                    const std::string &g     = tcr->gcode;
                    const size_t       prime = g.find("G1 E5.0000 F100\n");
                    INFO(g);
                    REQUIRE(prime != std::string::npos);
                    CHECK(prime < g.find("; CP TOOLCHANGE WIPE"));
                    if (run_in) {
                        // At the run-in start, after the travel out and before the run-in line.
                        CHECK(g.find("; tower interface run-in") < prime);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// Trigger modes on a real plate
// ---------------------------------------------------------------------------------------------------

TEST_CASE("Two filaments of one material are an interface only for every tool change", "[TowerInterface]")
{
    for (const Machine *m : { &X1C, &U1 }) {
        for (const char *trigger : { "every_toolchange", "material_change", "material_family_change" }) {
            DYNAMIC_SECTION(m->name << " " << trigger)
            {
                Print             print;
                Model             model;
                const std::string gcode = slice(print, model,
                                                machine_config(*m, { { "wipe_tower_interface_temp", "1" },
                                                                     { "filament_tower_interface_print_temp", "251,251" },
                                                                     { "wipe_tower_interface_trigger", trigger } },
                                                               true /* same filament twice */),
                                                *m);
                const bool every = std::string(trigger) == "every_toolchange";
                CHECK((gcode.find("; tower interface temperature") != std::string::npos) == every);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// Bambu Studio import and export
// ---------------------------------------------------------------------------------------------------

TEST_CASE("Bambu Studio's enable_tower_interface_features maps onto the three options", "[TowerInterface][Config]")
{
    SECTION("true turns on the three options and the wall gaps, at material changes")
    {
        const DynamicPrintConfig cfg = load_json_text("bambu_on.json", R"({ "name": "bambu", "from": "User",
            "enable_tower_interface_features": "1", "prime_tower_skip_points": "0",
            "filament_tower_interface_pre_extrusion_dist": ["12"] })");
        CHECK(cfg.opt_bool("wipe_tower_interface_temp"));
        CHECK(cfg.opt_bool("wipe_tower_interface_run_in"));
        CHECK(cfg.opt_bool("wipe_tower_interface_extra_prime"));
        CHECK(cfg.opt_enum<TowerInterfaceTrigger>("wipe_tower_interface_trigger") == titMaterialChange);
        CHECK(cfg.opt_bool("wipe_tower_wall_gap"));
        // Neutralised, so a re-save cannot re-arm the migration once the user turns them off.
        CHECK_FALSE(cfg.opt_bool("enable_tower_interface_features"));
        CHECK(cfg.option<ConfigOptionFloats>("filament_tower_interface_pre_extrusion_dist")->get_at(0) == Approx(12.));
    }
    SECTION("false leaves ours alone")
    {
        const DynamicPrintConfig cfg = load_json_text("bambu_off.json", R"({ "name": "bambu", "from": "User",
            "enable_tower_interface_features": "0", "prime_tower_skip_points": "0" })");
        CHECK_FALSE(cfg.has("wipe_tower_interface_temp"));
        CHECK_FALSE(cfg.has("wipe_tower_interface_run_in"));
        CHECK_FALSE(cfg.has("wipe_tower_interface_extra_prime"));
        // prime_tower_skip_points is our wall gap.
        CHECK_FALSE(cfg.opt_bool("wipe_tower_wall_gap"));
    }
    SECTION("a file that already has our options keeps them")
    {
        const DynamicPrintConfig cfg = load_json_text("ours.json", R"({ "name": "ours", "from": "User",
            "enable_tower_interface_features": "1", "wipe_tower_interface_temp": "0" })");
        CHECK_FALSE(cfg.opt_bool("wipe_tower_interface_temp"));
        CHECK_FALSE(cfg.has("wipe_tower_interface_run_in"));
    }
    SECTION("a migrated config saved and loaded again does not migrate again")
    {
        DynamicPrintConfig cfg = load_json_text("first.json", R"({ "name": "bambu", "from": "User", "enable_tower_interface_features": "1" })");
        // The user turns the options off; a diff-serialized save drops the defaults.
        cfg.erase("wipe_tower_interface_temp");
        cfg.erase("wipe_tower_interface_run_in");
        cfg.erase("wipe_tower_interface_extra_prime");
        cfg.erase("wipe_tower_interface_trigger");
        std::string saved = "{ \"name\": \"resaved\", \"from\": \"User\"";
        for (const std::string &key : cfg.keys())
            saved += ", \"" + key + "\": \"" + cfg.opt_serialize(key) + "\"";
        saved += " }";
        const DynamicPrintConfig again = load_json_text("again.json", saved);
        CHECK_FALSE(again.has("wipe_tower_interface_temp"));
    }
}

TEST_CASE("The bundled vendor presets keep their shipped behaviour", "[TowerInterface][Config]")
{
    // 50 BBL (H2D / H2C / X2D) and 9 Flashforge Creator 5 process presets carry
    // enable_tower_interface_features = 1; the Creator 5 ones also prime_tower_skip_points = 0.
    const std::string ff_c5 = (fs::path(profiles_dir()) / "Flashforge" / "process" / "0.20mm Standard @FF C5.json").string();
    SECTION("loaded as a file, they would map")
    {
        DynamicPrintConfig cfg;
        REQUIRE(load_json(ff_c5, cfg));
        CHECK(cfg.opt_bool("wipe_tower_interface_run_in"));
        CHECK(cfg.opt_bool("wipe_tower_wall_gap"));
    }
    SECTION("loaded as vendor presets, they do not")
    {
        SystemPresetTowerKeysScope scope;
        DynamicPrintConfig         cfg;
        REQUIRE(load_json(ff_c5, cfg));
        CHECK_FALSE(cfg.has("wipe_tower_interface_run_in"));
        CHECK_FALSE(cfg.has("wipe_tower_wall_gap"));
        CHECK(cfg.opt_bool("enable_tower_interface_features"));
    }
    SECTION("the H2D system process preset")
    {
        PresetBundle &b = vendor_bundle("BBL");
        REQUIRE(b.prints.select_preset_by_name("0.20mm Standard @BBL H2D", true));
        const DynamicPrintConfig &cfg = b.prints.get_selected_preset().config;
        CHECK_FALSE(cfg.opt_bool("wipe_tower_interface_temp"));
        CHECK_FALSE(cfg.opt_bool("wipe_tower_interface_run_in"));
        CHECK_FALSE(cfg.opt_bool("wipe_tower_interface_extra_prime"));
        CHECK(cfg.opt_bool("enable_tower_interface_features")); // Bambu's value, kept, not mapped
    }
}

TEST_CASE("Export Bambu 3MF writes the interface switch from the three options", "[TowerInterface][BambuExport]")
{
    auto export_with = [](std::initializer_list<ConfigBase::SetDeserializeItem> items, BambuExport::Report &report) {
        DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
        cfg.set_deserialize_strict({ { "enable_prime_tower", "1" },
                                     { "enable_tower_interface_features", "1" }, // a stale Bambu value is never written
                                     { "filament_tower_interface_print_temp", "245" },
                                     { "filament_tower_interface_pre_extrusion_dist", "8" },
                                     { "filament_tower_interface_pre_extrusion_length", "1.5" } });
        cfg.set_deserialize_strict(items);
        BambuExport::Context ctx = BambuExport::Context::from_project(cfg);
        return BambuExport::convert_project(cfg, ctx, report);
    };
    auto value = [](const BambuExport::Config &out, const char *key) {
        const auto it = out.find(key);
        return it == out.end() ? std::string("<missing>") : BambuExport::serialize(it->second);
    };
    auto has_note = [](const BambuExport::Report &report) {
        return std::any_of(report.notes.begin(), report.notes.end(),
                           [](const std::string &n) { return n.find("enable_tower_interface_features") != std::string::npos; });
    };
    {
        BambuExport::Report       report;
        const BambuExport::Config out = export_with({}, report);
        CHECK(value(out, "enable_tower_interface_features") == "0");
        CHECK_FALSE(has_note(report));
        CHECK(out.count("wipe_tower_interface_temp") == 0);
        // Per-filament values under Bambu's names, as they are.
        CHECK(value(out, "filament_tower_interface_print_temp") == "245");
        CHECK(value(out, "filament_tower_interface_pre_extrusion_dist") == "8");
        CHECK(value(out, "filament_tower_interface_pre_extrusion_length") == "1.5");
        // Our wall gap is Bambu's skip points.
        CHECK(value(out, "prime_tower_skip_points") == "1");
    }
    for (const char *one : { "wipe_tower_interface_temp", "wipe_tower_interface_run_in", "wipe_tower_interface_extra_prime" }) {
        BambuExport::Report       report;
        const BambuExport::Config out = export_with({ { one, "1" } }, report);
        INFO(one);
        CHECK(value(out, "enable_tower_interface_features") == "1");
        CHECK(has_note(report)); // only a subset was on
    }
    {
        BambuExport::Report       report;
        const BambuExport::Config out = export_with({ { "wipe_tower_interface_temp", "1" },
                                                      { "wipe_tower_interface_run_in", "1" },
                                                      { "wipe_tower_interface_extra_prime", "1" },
                                                      { "wipe_tower_wall_gap", "0" } },
                                                    report);
        CHECK(value(out, "enable_tower_interface_features") == "1");
        CHECK_FALSE(has_note(report));
        CHECK(value(out, "prime_tower_skip_points") == "0");
    }
}

TEST_CASE("A Bambu 3MF round trip maps the interface switch both ways", "[TowerInterface][3mf]")
{
    for (const bool on : { true, false }) {
        DYNAMIC_SECTION((on ? "on" : "off"))
        {
            DynamicPrintConfig cfg = machine_config(X1C, { { "wipe_tower_interface_temp", on ? "1" : "0" },
                                                           { "wipe_tower_interface_run_in", on ? "1" : "0" },
                                                           { "wipe_tower_interface_extra_prime", on ? "1" : "0" },
                                                           { "wipe_tower_interface_trigger", "every_toolchange" },
                                                           { "filament_tower_interface_print_temp", "233,266" },
                                                           { "filament_tower_interface_pre_extrusion_dist", "9,11" } });
            Model model;
            add_plate(model, X1C.bed_center);
            PlateData *plate   = new PlateData();
            plate->plate_index = 0;
            plate->objects_and_instances.emplace_back(0, 0);
            plate->objects_and_instances.emplace_back(1, 0);
            const std::string   path = temp_path(std::string("bambu_") + (on ? "on" : "off") + ".3mf");
            BambuExport::Report report;
            StoreParams         sp;
            sp.path            = path.c_str();
            sp.model           = &model;
            sp.config          = &cfg;
            sp.plate_data_list = { plate };
            sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
            sp.bambu_compat    = true;
            sp.bambu_report    = &report;
            REQUIRE(store_bbs_3mf(sp));
            release_PlateData_list(sp.plate_data_list);

            DynamicPrintConfig        back;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
            Model                     loaded;
            PlateDataPtrs             plates;
            std::vector<Preset *>     presets;
            bool                      is_bbl = false;
            Semver                    version;
            REQUIRE(load_bbs_3mf(path.c_str(), &back, &ctxt, &loaded, &plates, &presets, &is_bbl, &version, nullptr,
                                 LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
            release_PlateData_list(plates);
            for (Preset *p : presets)
                delete p;
            CHECK(is_bbl); // read as a Bambu Studio project
            // The trigger is ours only: Bambu's switch comes back at material changes.
            if (on) {
                CHECK(back.opt_bool("wipe_tower_interface_temp"));
                CHECK(back.opt_bool("wipe_tower_interface_run_in"));
                CHECK(back.opt_bool("wipe_tower_interface_extra_prime"));
                CHECK(back.opt_enum<TowerInterfaceTrigger>("wipe_tower_interface_trigger") == titMaterialChange);
            } else {
                CHECK_FALSE((back.has("wipe_tower_interface_temp") && back.opt_bool("wipe_tower_interface_temp")));
                CHECK_FALSE((back.has("wipe_tower_interface_run_in") && back.opt_bool("wipe_tower_interface_run_in")));
            }
            CHECK(back.opt_serialize("filament_tower_interface_print_temp") == "233,266");
            CHECK(back.opt_serialize("filament_tower_interface_pre_extrusion_dist") == "9,11");
            fs::remove(path);
        }
    }
}

// Hidden: writes each machine's G-code with all three options on (material-change trigger) to
// $TOWER_INTERFACE_DUMP_DIR/<machine>_interface.gcode, for a look at the moves.
TEST_CASE("Write tower interface G-code for inspection", "[.TowerInterfaceDump]")
{
    const char *dump = std::getenv("TOWER_INTERFACE_DUMP_DIR");
    REQUIRE(dump != nullptr);
    for (const Machine *m : all_machines) {
        Print             print;
        Model             model;
        const std::string gcode = slice(print, model,
                                        machine_config(*m, { { "wipe_tower_interface_temp", "1" },
                                                             { "wipe_tower_interface_run_in", "1" },
                                                             { "wipe_tower_interface_extra_prime", "1" },
                                                             { "filament_tower_interface_print_temp", "240,255" } }),
                                        *m);
        boost::nowide::ofstream f((fs::path(dump) / (std::string(m->name) + "_interface.gcode")).string(), std::ios::binary);
        f << gcode;
    }
}
