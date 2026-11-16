// PR-C: the tower side of AdaptiveLayerMerge on real Bambu Lab presets (Bambu Studio 2.9.1 WipeTower.cpp 5222-7600). On a
// plate whose filaments print through more than one nozzle diameter (H3C rack 0.4 + 0.8 and 0.2 + 0.8, H2D 0.2 / 0.4) the
// Type 1 tower follows each nozzle's slab ladder: the coarse nozzle's tower visit sits at the Z of every slab it prints on
// the object, with that slab's height; nothing on the tower is extruded outside a nozzle's printable window; the tower is
// as tall as the objects; a single-diameter plate's tower is untouched.

#include <catch2/catch.hpp>

#include "libslic3r/AdaptiveLayerMerge.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTowerEstimate.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "test_data.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <iterator>
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

namespace {

double r3(double v) { return std::round(v * 1000.) / 1000.; }

PresetBundle &alm_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    if (bundle)
        return *bundle;
    const std::string             saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("almtower_%%%%-%%%%");
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

DynamicPrintConfig preset_config(const char *printer, const char *process, const char *filament_1, const char *filament_2)
{
    PresetBundle &b = alm_bundle();
    REQUIRE(b.printers.select_preset_by_name(printer, true));
    REQUIRE(b.prints.select_preset_by_name(process, true));
    REQUIRE(b.filaments.select_preset_by_name(filament_1, true));
    b.filament_presets = { filament_1 };
    b.set_num_filaments(2, std::vector<std::string>{ "#E01919", "#1943E0" });
    b.filament_presets = { filament_1, filament_2 };
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "enable_prime_tower", "1" },
        { "wipe_tower_x", 160. },
        { "wipe_tower_y", 180. },
        { "wipe_tower_rotation_angle", 0 },
        { "gcode_comments", 0 },
        { "enable_support", "0" },
    });
    if (cfg.option("filament_map") != nullptr)
        cfg.set_deserialize_strict({ { "filament_map_mode", "Manual" }, { "filament_map", "1,2" } });
    return cfg;
}

// H3C: one extruder whose rack holds the mounted nozzle and a 0.8; filament 2 is pinned to the 0.8.
DynamicPrintConfig h3c_mixed(const char *fine, const char *filament_2 = nullptr)
{
    const bool          fine_02  = std::string(fine) == "0.2";
    DynamicPrintConfig  cfg      = fine_02 ? preset_config("Bambu Lab H3C 0.2 nozzle", "0.10mm Standard @BBL H3C 0.2 nozzle",
                                                           "Bambu PLA Basic @BBL H3C 0.2 nozzle", filament_2 ? filament_2 : "Bambu PLA Basic @BBL H3C 0.8 nozzle") :
                                             preset_config("Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Bambu PLA Basic @BBL H3C",
                                                           filament_2 ? filament_2 : "Bambu PLA Basic @BBL H3C 0.8 nozzle");
    cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_diameter", true)->values = { std::string(fine) + ",0.8" };
    cfg.option<ConfigOptionFloats>("filament_rack_nozzle_diameter", true)->values  = { 0., 0.8 };
    return cfg;
}

// H2D with a 0.2 and a 0.4 nozzle, at the 0.2's layer height.
DynamicPrintConfig h2d_mixed()
{
    DynamicPrintConfig cfg = preset_config("Bambu Lab H2D 0.2 nozzle", "0.10mm Standard @BBL H2D 0.2 nozzle", "Bambu PLA Basic @BBL H2D 0.2 nozzle",
                                           "Bambu PLA Basic @BBL H2D");
    cfg.set_deserialize_strict({ { "nozzle_diameter", "0.2,0.4" } });
    return cfg;
}

// Two 20 x 20 x `height` mm cubes, one per filament: every layer the fine filament prints, and the coarse one on its slabs.
void add_cubes(Model &model, double height)
{
    for (int i = 0; i < 2; ++i) {
        TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, float(height / 20.)));
        ModelObject *object = model.add_object();
        object->name        = "cube" + std::to_string(i);
        object->add_volume(cube);
        object->volumes.front()->name = object->name;
        object->config.set("extruder", i + 1);
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(150. + 30. * i, 140., 0.));
        object->ensure_on_bed();
    }
}

struct TowerExtrusion
{
    int    tool;
    double z;
    double height;
    bool   purge; // inside the purge of a tool change ("; CP TOOLCHANGE WIPE" .. "; CP TOOLCHANGE END")
};

void collect_extrusions(const std::string &gcode, int tool_before, int tool_after, double z, double height, std::vector<TowerExtrusion> &out)
{
    static const std::regex height_re(R"(HEIGHT:\s*([0-9.]+))");
    static const std::regex e_re(R"(\bE([0-9.]+))");
    std::istringstream      in(gcode);
    std::string             line;
    int                     tool  = tool_before;
    bool                    purge = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::smatch m;
        if (line.find("[change_filament_gcode]") != std::string::npos) {
            tool = tool_after;
            continue;
        }
        if (line.find("; CP TOOLCHANGE WIPE") != std::string::npos)
            purge = true;
        else if (line.find("; CP TOOLCHANGE END") != std::string::npos)
            purge = false;
        if (!line.empty() && line[0] == ';' && std::regex_search(line, m, height_re)) {
            height = std::stod(m[1].str());
            continue;
        }
        if (line.rfind("G1 ", 0) == 0 && (line.find(" X") != std::string::npos || line.find(" Y") != std::string::npos) &&
            std::regex_search(line, m, e_re) && std::stod(m[1].str()) > 0.)
            out.push_back({ tool, z, r3(height), purge });
    }
}

struct Sliced
{
    Model                       model;
    std::unique_ptr<Print>      print;
    std::vector<TowerExtrusion> extrusions;     // every tower extrusion that reaches the G-code
    std::set<double>            coarse_object_z; // print_z of every layer the coarse cube prints
    std::map<double, std::set<double>> coarse_purge;  // print_z -> heights of the coarse filament's purge there
    std::set<double>            coarse_change;  // print_z of every tool change to the coarse filament
    std::set<double>            coarse_visit;   // print_z of every tower extrusion of the coarse filament
    double                      first_z = 0.;   // the first layer's print_z
    double                      tower_top = 0.;
    size_t                      skipped   = 0;  // tower layers merged out (combined_away)
    std::vector<AdaptiveLayerHeightRange> windows;
};

std::unique_ptr<Sliced> slice(const DynamicPrintConfig &cfg, double height)
{
    auto s   = std::make_unique<Sliced>();
    s->print = std::make_unique<Print>();
    add_cubes(s->model, height);
    s->print->is_BBL_printer() = true;
    s->print->apply(s->model, cfg);
    s->print->validate();
    s->print->set_status_silent();
    REQUIRE(!Test::gcode(*s->print).empty());

    const Print &print = *s->print;
    for (const auto &layer : print.wipe_tower_data().tool_changes) {
        for (const WipeTower::ToolChangeResult &tcr : layer) {
            if (tcr.combined_away) {
                ++s->skipped;
                continue;
            }
            const size_t before = s->extrusions.size();
            collect_extrusions(tcr.nozzle_change_result.gcode, tcr.initial_tool, tcr.initial_tool, tcr.print_z, tcr.layer_height, s->extrusions);
            collect_extrusions(tcr.gcode, tcr.initial_tool, tcr.new_tool, tcr.print_z, tcr.layer_height, s->extrusions);
            if (s->extrusions.size() > before)
                s->tower_top = std::max(s->tower_top, double(tcr.print_z));
            if (tcr.new_tool == 1 && tcr.initial_tool != 1)
                s->coarse_change.insert(r3(tcr.print_z));
        }
    }
    for (const TowerExtrusion &e : s->extrusions)
        if (e.tool == 1) {
            s->coarse_visit.insert(r3(e.z));
            if (e.purge)
                s->coarse_purge[r3(e.z)].insert(e.height);
        }
    s->first_z = r3(print.objects().front()->layers().front()->print_z);
    for (const PrintObject *object : print.objects()) {
        if (object->model_object()->name != "cube1")
            continue;
        for (const Layer *layer : object->layers())
            for (const LayerRegion *region : layer->regions())
                if (!region->perimeters.entities.empty()) {
                    s->coarse_object_z.insert(r3(layer->print_z));
                    break;
                }
    }
    // The window each nozzle's object ladder is cut in (PrintObject::compute_adaptive_merge_groups()).
    std::vector<unsigned int> used;
    print.used_printing_filaments(used);
    const double reference = print.adaptive_merge_reference_nozzle();
    const double base      = print.objects().front()->slicing_parameters().layer_height;
    for (unsigned int f = 0; f < 2; ++f) {
        const double nozzle = nozzle_diameter_for_filament(print.config(), f + 1);
        s->windows.push_back(adaptive_ladder_height_range(adaptive_layer_height_range_for_nozzle(print.config(), f + 1, used), nozzle, reference, base));
    }
    return s;
}

// exact_slab: the coarse purge is also as thick as the object slab. True where no nozzle change lays ramming lines (the
// H3C presets ram nothing); with ramming, Bambu's lane planner may keep a purge on a thinner slab inside the window.
void check_plate(const Sliced &s, double object_height, bool exact_slab)
{
    REQUIRE(s.print->uses_multiple_nozzle_diameters());
    REQUIRE(!s.extrusions.empty());
    REQUIRE(s.coarse_object_z.size() > 2);

    // Nothing on the tower outside a nozzle's printable window (Bambu Studio filament_can_print_height()), except Bambu's
    // 0.2 mm bridge flow over a sparse band, which a nozzle whose window ends below 0.2 mm lays as well.
    std::map<int, std::set<double>> heights;
    for (const TowerExtrusion &e : s.extrusions) {
        CAPTURE(e.tool, e.z, e.height);
        REQUIRE(e.tool >= 0);
        REQUIRE(e.tool < 2);
        heights[e.tool].insert(e.height);
        // The first layer is laid at its sliced height by every nozzle, the objects' first layer too (Edge's
        // AdaptiveLayerMerge never merges layer 0).
        if (e.z <= s.first_z + 1e-4)
            continue;
        const AdaptiveLayerHeightRange &w = s.windows[e.tool];
        if (w.max_height < 0.2 && std::abs(e.height - 0.2) < 1e-6)
            continue;
        CHECK(e.height + 1e-3 >= w.min_height);
        CHECK(e.height <= w.max_height + 1e-3);
    }
    for (const auto &[tool, set] : heights) {
        std::ostringstream ss;
        for (double h : set)
            ss << h << ' ';
        INFO("tool " << tool << " tower heights: " << ss.str());
        CHECK(!set.empty());
    }

    // The coarse filament comes to the tower on the slabs it prints on the object, at the same Z: every change to it is on
    // one of its object slab tops (a slab it starts already loaded needs no visit), and its purge there is as thick as the
    // object slab.
    REQUIRE(s.coarse_change.size() > 2);
    size_t exact = 0;
    for (double z : s.coarse_change) {
        CAPTURE(z);
        const auto it = s.coarse_object_z.find(z);
        REQUIRE(it != s.coarse_object_z.end());
        CHECK(s.coarse_visit.count(z) == 1);
        if (it == s.coarse_object_z.begin())
            continue; // the first layer
        const double slab   = r3(z - *std::prev(it));
        const auto   purges = s.coarse_purge.find(z);
        REQUIRE(purges != s.coarse_purge.end());
        CAPTURE(slab);
        for (double h : purges->second) {
            CAPTURE(h);
            if (exact_slab)
                CHECK(std::abs(h - slab) < 2e-3);
            else
                CHECK(h <= slab + 2e-3); // a lane may keep a purge on a thinner slab, never a thicker one
        }
        ++exact;
    }
    CHECK(exact > 1);
    // The tower is as tall as the objects.
    CHECK_THAT(s.tower_top, WithinAbs(object_height, 2e-3));
}

std::vector<std::string> tower_height_warnings(const DynamicPrintConfig &cfg)
{
    Model model;
    add_cubes(model, 2.);
    Print print;
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    std::vector<StringObjectException> warnings;
    print.validate(&warnings);
    std::vector<std::string> out;
    for (const StringObjectException &w : warnings)
        if (w.string.find("The prime tower prints every filament at the layer height") != std::string::npos)
            out.push_back(w.string);
    return out;
}

} // namespace

TEST_CASE("ALM tower: H3C 0.4 + 0.8 - the 0.8's tower slabs are its object slabs", "[ALMTower][PrintGCode]")
{
    const auto s = slice(h3c_mixed("0.4"), 4.);
    check_plate(*s, 4., true);
    // 0.2 mm layers, the 0.8 on 0.4 mm slabs.
    for (const auto &[z, h] : s->coarse_purge)
        if (z > 0.21) {
            CAPTURE(z);
            CHECK(h == std::set<double>{ 0.4 });
        }
}

TEST_CASE("ALM tower: H3C 0.2 + 0.8 - the 0.8 never lays a 0.1 mm tower line", "[ALMTower][PrintGCode]")
{
    const auto s = slice(h3c_mixed("0.2"), 4.);
    check_plate(*s, 4., true);
}

TEST_CASE("ALM tower: H2D 0.2 / 0.4 - the 0.4 follows its 0.2 mm slabs", "[ALMTower][PrintGCode]")
{
    const auto s = slice(h2d_mixed(), 3.);
    check_plate(*s, 3., false);
}

TEST_CASE("ALM tower: H3C 0.4 PLA + 0.8 PETG - contact layers on a merged tower", "[ALMTower][PrintGCode]")
{
    const auto s = slice(h3c_mixed("0.4", "Bambu PETG HF @BBL H3C 0.8 nozzle"), 4.);
    check_plate(*s, 4., true);
    // Two categories, so the tower has a contact layer somewhere (Orca #10780 Contact), and every tower layer is either
    // printed or merged out.
    bool contact = false;
    for (const auto &layer : s->print->wipe_tower_data().tool_changes)
        for (const WipeTower::ToolChangeResult &tcr : layer)
            contact |= tcr.is_contact;
    CHECK(contact);
}

TEST_CASE("ALM tower: a single-diameter plate's tower is planned as before", "[ALMTower][PrintGCode]")
{
    const DynamicPrintConfig cfg = preset_config("Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Bambu PLA Basic @BBL H3C",
                                                 "Bambu PLA Basic @BBL H3C");
    const auto s = slice(cfg, 2.);
    CHECK_FALSE(s->print->uses_multiple_nozzle_diameters());
    CHECK(s->skipped == 0);
    for (const TowerExtrusion &e : s->extrusions) {
        CAPTURE(e.tool, e.z, e.height);
        CHECK(std::abs(e.height - 0.2) < 1e-6);
    }
}

TEST_CASE("ALM tower: the pre-slice footprint estimate covers the generated mixed tower", "[ALMTower][PrintGCode]")
{
    const double             height = 20.;
    const DynamicPrintConfig cfg    = h3c_mixed("0.4");
    const auto               s      = slice(cfg, height);
    const double             depth  = s->print->wipe_tower_data().depth;
    REQUIRE(depth > 0.);
    const WipeTowerFootprint fp = estimate_wipe_tower_footprint(s->print->full_print_config(), WipeTowerType::Type1, { 0, 1 }, 0.2, height);
    CAPTURE(depth, fp.depth);
    CHECK(fp.depth >= 0.6 * depth);
    CHECK(fp.depth <= 1.6 * depth + 1.);
}

TEST_CASE("ALM tower: the mixed-diameter tower warning only where the tower is not planned in slabs", "[ALMTower]")
{
    // 0.2 + 0.8 at 0.1 mm: below the 0.8's 0.16 mm minimum. The Type 1 tower plans it in slabs: no warning.
    DynamicPrintConfig cfg = h3c_mixed("0.2");
    CHECK(tower_height_warnings(cfg).empty());
    // With no sparse layers the tower is compacted after the fact and keeps the base height, as in Bambu Studio: warned.
    cfg.set_deserialize_strict({ { "wipe_tower_no_sparse_layers", "1" } });
    CHECK(tower_height_warnings(cfg).size() == 1);
}
