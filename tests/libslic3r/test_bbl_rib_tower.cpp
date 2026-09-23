#include <catch2/catch.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTowerEstimate.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace Slic3r;

// Rib walls on the Bambu Lab (Type1) prime tower.
//
// Bambu Lab printers use the BBS-style generator (GCode/WipeTower.cpp); every other printer uses
// WipeTower2. Bambu Studio's generator draws the tower wall as the box unioned with two diagonal
// ribs (optionally filleted), squares the tower off and extends the ribs rather than the body to
// the height-based stability minimum. These cases slice a real two-filament plate on this tree's
// own BBL system profiles and look at the tower the generator actually built.

namespace {

// The BBL vendor bundle, loaded once: it holds thousands of presets.
PresetBundle &bbl_bundle()
{
    static PresetBundle *bundle = [] {
        const std::string saved_data_dir = data_dir();
        const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("bbl_rib_%%%%-%%%%");
        boost::filesystem::create_directories(scratch);
        set_data_dir(scratch.string());
        const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
        static PresetBundle library;
        library.load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
        auto *b = new PresetBundle();
        b->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, &library);
        set_data_dir(saved_data_dir);
        return b;
    }();
    return *bundle;
}

// 20 x 20 x 40 mm, so the ribs have a height to taper over.
TriangleMesh tall_cube()
{
    TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
    cube.scale(Vec3f(1.f, 1.f, 2.f));
    return cube;
}

struct Machine
{
    const char *printer;
    const char *process;
    const char *filament;
    Vec2d       bed_center;
    Vec2d       tower;
};

const Machine X1C{ "Bambu Lab X1 Carbon 0.4 nozzle", "0.20mm Standard @BBL X1C", "Bambu PLA Basic @BBL X1C", { 128., 128. }, { 30., 200. } };
const Machine H2D{ "Bambu Lab H2D 0.4 nozzle", "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D", { 175., 160. }, { 40., 250. } };

// Two filaments of the same preset; the plate below puts one cube on each, so every layer changes
// tool and the tower is printed on every layer.
DynamicPrintConfig machine_config(const Machine &m, std::initializer_list<ConfigBase::SetDeserializeItem> overrides)
{
    PresetBundle &b = bbl_bundle();
    REQUIRE(b.printers.select_preset_by_name(m.printer, true));
    REQUIRE(b.prints.select_preset_by_name(m.process, true));
    REQUIRE(b.filaments.select_preset_by_name(m.filament, true));
    b.filament_presets = { m.filament };
    b.set_num_filaments(2, std::vector<std::string>{ "#E01919", "#1943E0" });
    b.filament_presets = { m.filament, m.filament };
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "enable_prime_tower", "1" },
        { "wipe_tower_x", m.tower.x() },
        { "wipe_tower_y", m.tower.y() },
        { "wipe_tower_rotation_angle", 0 },
        { "prime_tower_brim_width", 3 },
        { "gcode_comments", 0 },
    });
    // Both filaments on their own extruder where there are two.
    if (cfg.option<ConfigOptionFloats>("nozzle_diameter")->values.size() == 2)
        cfg.set_deserialize_strict({ { "filament_map", "1,2" } });
    cfg.set_deserialize_strict(overrides);
    return cfg;
}

// Two cubes side by side at the bed centre, the left one on filament 1, the right one on filament 2.
void add_plate(Model &model, const Vec2d &bed_center)
{
    for (int i = 0; i < 2; ++i) {
        ModelObject *object = model.add_object();
        object->name        = i == 0 ? "cubeA" : "cubeB";
        object->add_volume(tall_cube());
        object->volumes.front()->name = object->name;
        object->config.set("extruder", i + 1);
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(bed_center.x() + (i == 0 ? -15. : 15.), bed_center.y(), 0.));
        object->ensure_on_bed();
    }
}

std::string slice(Print &print, Model &model, const DynamicPrintConfig &cfg, const Vec2d &bed_center, bool bbl)
{
    add_plate(model, bed_center);
    print.is_BBL_printer() = bbl;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

struct TowerShape
{
    BoundingBoxf bbox;              // every extruded point, all layers, brim included
    BoundingBoxf bbox_above_brim;   // layers past the brim chamfer
    double       slanted_length = 0.; // extruded at neither 0 nor 90 degrees (rib sides, fillets), mm
    size_t       layers_with_ribs = 0; // layers with at least 8 mm of it
    size_t       layers = 0;
};

TowerShape tower_shape(const Print &print)
{
    TowerShape shape;
    const auto &layers = print.wipe_tower_data().tool_changes;
    for (size_t l = 0; l < layers.size(); ++l) {
        if (layers[l].empty() || wipe_tower_layer_is_sparse(layers[l]))
            continue;
        ++shape.layers;
        double slanted = 0.;
        for (const WipeTower::ToolChangeResult &tcr : layers[l])
            for (size_t i = 1; i < tcr.extrusions.size(); ++i) {
                const WipeTower::Extrusion &e = tcr.extrusions[i];
                if (e.width <= 0.f)
                    continue;
                const Vec2f a = tcr.extrusions[i - 1].pos, b = e.pos;
                for (const Vec2f &p : { a, b }) {
                    shape.bbox.merge(p.cast<double>());
                    if (l >= 12)
                        shape.bbox_above_brim.merge(p.cast<double>());
                }
                const Vec2f d = b - a;
                if (std::abs(d.x()) > 0.01f && std::abs(d.y()) > 0.01f)
                    slanted += d.norm();
            }
        shape.slanted_length += slanted;
        if (slanted >= 8.)
            ++shape.layers_with_ribs;
    }
    return shape;
}

WipeTowerFootprint estimate(const Print &print, const DynamicPrintConfig &cfg)
{
    return estimate_wipe_tower_footprint(cfg, resolve_wipe_tower_type(cfg), print.extruders(true), cfg.opt_float("layer_height"), 40.);
}

// Hidden: writes the G-code of each case to $BBL_RIB_DUMP_DIR, or compares it byte for byte with
// the files already there when $BBL_RIB_BASELINE_DIR is set (a dump made by the build before a
// change). Used to prove the port leaves rectangle towers and WipeTower2 untouched. The header's
// export time is left out; everything else must match to the byte.
std::string without_timestamp(std::string gcode)
{
    const size_t at = gcode.find("; generated by ");
    if (at != std::string::npos)
        gcode.erase(at, gcode.find('\n', at) - at);
    return gcode;
}

void dump_or_compare(const std::string &name, const std::string &gcode_in)
{
    const std::string gcode = without_timestamp(gcode_in);
    const char *dump = std::getenv("BBL_RIB_DUMP_DIR");
    const char *base = std::getenv("BBL_RIB_BASELINE_DIR");
    if (dump != nullptr) {
        boost::nowide::ofstream f((boost::filesystem::path(dump) / (name + ".gcode")).string(), std::ios::binary);
        f << gcode;
    }
    if (base != nullptr) {
        boost::nowide::ifstream f((boost::filesystem::path(base) / (name + ".gcode")).string(), std::ios::binary);
        REQUIRE(f.good());
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string before = ss.str();
        size_t            at     = 0;
        while (at < before.size() && at < gcode.size() && before[at] == gcode[at])
            ++at;
        const size_t nl         = at == 0 ? std::string::npos : before.rfind('\n', at - 1);
        const size_t line_start = nl == std::string::npos ? 0 : nl + 1;
        INFO(name << " differs from the baseline in " << base << " at byte " << at << ":\n  before: " << before.substr(line_start, 120)
                  << "\n  now:    " << gcode.substr(line_start, 120));
        CHECK(at == before.size());
        CHECK(at == gcode.size());
    }
}

} // namespace

TEST_CASE("Bambu Lab rib tower: ribs are printed, and only when asked for", "[BblRibTower]")
{
    for (const Machine *m : { &X1C, &H2D }) {
        for (const bool rib : { false, true }) {
            DYNAMIC_SECTION(m->printer << (rib ? " rib" : " rectangle"))
            {
                const DynamicPrintConfig cfg = machine_config(*m, { { "wipe_tower_wall_type", rib ? "rib" : "rectangle" },
                                                                    { "wipe_tower_rib_width", 8 },
                                                                    { "wipe_tower_extra_rib_length", 0 },
                                                                    { "wipe_tower_fillet_wall", 1 } });
                Print  print;
                Model  model;
                const std::string gcode = slice(print, model, cfg, m->bed_center, true);
                REQUIRE(print.has_wipe_tower());
                const TowerShape shape = tower_shape(print);
                REQUIRE(shape.layers > 20);
                INFO("slanted extrusion " << shape.slanted_length << " mm, ribs on " << shape.layers_with_ribs << " of " << shape.layers << " layers");
                if (rib) {
                    // Every printed tower layer carries its wall, and the four rib ends of a rib wall
                    // are slanted (rib sides, end caps, fillets).
                    CHECK(shape.layers_with_ribs == shape.layers);
                } else {
                    // Walls, purge lines, sparse infill and brim are all axis-aligned.
                    CHECK(shape.slanted_length == 0.);
                }

                // The pre-slice footprint (arrange, collision checks, bed preview) covers the tower
                // actually printed: [0, width] x [0, depth] plus the brim, from the tower position.
                const WipeTowerFootprint fp = estimate(print, cfg);
                INFO("estimate " << fp.width << " x " << fp.depth << " brim " << fp.brim_width << "; printed " << shape.bbox.min.transpose() << " - "
                                 << shape.bbox.max.transpose());
                const double tol = 0.25; // centre lines against a footprint of deposited material
                CHECK(shape.bbox.min.x() >= -fp.brim_width - tol);
                CHECK(shape.bbox.min.y() >= -fp.brim_width - tol);
                CHECK(shape.bbox.max.x() <= fp.width + fp.brim_width + tol);
                CHECK(shape.bbox.max.y() <= fp.depth + fp.brim_width + tol);
                if (rib) {
                    // The rib tower is placed by its bounding box: its first-layer wall starts at the
                    // tower position, as Bambu Studio's rib_offset does.
                    CHECK(shape.bbox_above_brim.min.x() >= -tol);
                    CHECK(shape.bbox_above_brim.min.y() >= -tol);
                    // After slicing, the tower data describes the same box the estimate did.
                    const WipeTowerData &wtd = print.wipe_tower_data();
                    CHECK(wtd.width <= fp.width + tol);
                    CHECK(wtd.depth <= fp.depth + tol);
                    CHECK(shape.bbox_above_brim.max.x() <= wtd.width + tol);
                    CHECK(shape.bbox_above_brim.max.y() <= wtd.depth + tol);
                    REQUIRE(wtd.wipe_tower_mesh_data.has_value());
                    const BoundingBox mesh_bb = get_extents(wtd.wipe_tower_mesh_data->bottom);
                    CHECK(unscaled(mesh_bb.min.x()) <= shape.bbox.min.x() + tol);
                    CHECK(unscaled(mesh_bb.max.x()) >= shape.bbox.max.x() - tol);
                    CHECK(unscaled(mesh_bb.min.y()) <= shape.bbox.min.y() + tol);
                    CHECK(unscaled(mesh_bb.max.y()) >= shape.bbox.max.y() - tol);
                }
            }
        }
    }
}

TEST_CASE("Bambu Lab tower: a cone wall prints as a rectangle", "[BblRibTower]")
{
    const DynamicPrintConfig cfg = machine_config(X1C, { { "wipe_tower_wall_type", "cone" }, { "wipe_tower_cone_angle", 20 } });
    Print print;
    Model model;
    slice(print, model, cfg, X1C.bed_center, true);
    CHECK(tower_shape(print).slanted_length == 0.);
    const WipeTowerFootprint fp = estimate(print, cfg);
    CHECK(fp.width == Approx(cfg.opt_float("prime_tower_width")));
}

TEST_CASE("Bambu Lab tower wall speed is capped by the max purge speed", "[BblRibTower]")
{
    // The generator's own G-code, before the machine's change_filament_gcode is expanded into it:
    // the fastest extruding XY move of the tower.
    auto max_tower_feedrate = [](const Print &print) {
        double max_f = 0.;
        for (const auto &layer : print.wipe_tower_data().tool_changes)
            for (const WipeTower::ToolChangeResult &tcr : layer) {
                std::istringstream in(tcr.gcode);
                for (std::string line; std::getline(in, line);) {
                    if (line.rfind("G1 ", 0) != 0 || line.find(" E") == std::string::npos ||
                        (line.find(" X") == std::string::npos && line.find(" Y") == std::string::npos))
                        continue;
                    if (const size_t f = line.find(" F"); f != std::string::npos)
                        max_f = std::max(max_f, std::atof(line.c_str() + f + 2));
                }
            }
        return max_f;
    };
    for (const char *wall : { "rectangle", "rib" }) {
        DYNAMIC_SECTION(wall)
        {
            const DynamicPrintConfig cfg = machine_config(X1C, { { "wipe_tower_wall_type", wall }, { "wipe_tower_max_purge_speed", 40 } });
            Print print;
            Model model;
            slice(print, model, cfg, X1C.bed_center, true);
            const double f = max_tower_feedrate(print);
            INFO("fastest tower extrusion F" << f);
            CHECK(f > 0.);
            CHECK(f <= 40. * 60. + 0.5);
        }
    }
}

TEST_CASE("Bambu Lab rib tower under smooth timelapse", "[BblRibTower]")
{
    // Smooth timelapse prints the tower wall on its own (only_generate_out_wall) and keeps a tower
    // even for a single filament, squared to the stability minimum.
    for (const bool one_filament : { false, true }) {
        DYNAMIC_SECTION((one_filament ? "one filament" : "two filaments"))
        {
            const DynamicPrintConfig cfg = machine_config(X1C, { { "wipe_tower_wall_type", "rib" }, { "timelapse_type", "1" } });
            Print print;
            Model model;
            add_plate(model, X1C.bed_center);
            if (one_filament)
                model.objects.back()->config.set("extruder", 1);
            print.is_BBL_printer() = true;
            print.apply(model, cfg);
            print.validate();
            print.set_status_silent();
            Test::gcode(print);
            REQUIRE(print.has_wipe_tower());
            const TowerShape shape = tower_shape(print);
            INFO("slanted extrusion " << shape.slanted_length << " mm, ribs on " << shape.layers_with_ribs << " of " << shape.layers << " layers");
            CHECK(shape.layers > 20);
            CHECK(shape.layers_with_ribs == shape.layers);
            const WipeTowerFootprint fp = estimate(print, cfg);
            INFO("estimate " << fp.width << " x " << fp.depth << " brim " << fp.brim_width << "; printed " << shape.bbox.min.transpose() << " - " << shape.bbox.max.transpose());
            CHECK(shape.bbox.min.x() >= -fp.brim_width - 0.25);
            CHECK(shape.bbox.max.x() <= fp.width + fp.brim_width + 0.25);
            CHECK(shape.bbox.max.y() <= fp.depth + fp.brim_width + 0.25);
        }
    }
}

TEST_CASE("Bambu Lab rib tower fillets are arcs unless the tower is rotated", "[BblRibTower]")
{
    // The G-code post-processor rotates X/Y of a wipe tower move but not the I/J of an arc, so a
    // rotated tower gets its fillets as short lines instead.
    auto tower_arcs = [](const Print &print) {
        size_t arcs = 0;
        for (const auto &layer : print.wipe_tower_data().tool_changes)
            for (const WipeTower::ToolChangeResult &tcr : layer) {
                std::istringstream in(tcr.gcode);
                for (std::string line; std::getline(in, line);)
                    if (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0)
                        ++arcs;
            }
        return arcs;
    };
    for (const double angle : { 0., 30. }) {
        DYNAMIC_SECTION("rotated " << angle)
        {
            const DynamicPrintConfig cfg = machine_config(X1C, { { "wipe_tower_wall_type", "rib" },
                                                                 { "wipe_tower_fillet_wall", 1 },
                                                                 { "enable_arc_fitting", 1 },
                                                                 { "wipe_tower_rotation_angle", angle } });
            Print print;
            Model model;
            slice(print, model, cfg, X1C.bed_center, true);
            CHECK(tower_shape(print).layers_with_ribs == tower_shape(print).layers);
            if (angle == 0.)
                CHECK(tower_arcs(print) > 0);
            else
                CHECK(tower_arcs(print) == 0);
        }
    }
}

TEST_CASE("Export Bambu 3MF writes the rib tower settings under Bambu's names", "[BblRibTower][BambuExport]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({
        { "enable_prime_tower", "1" },
        { "wipe_tower_wall_type", "rib" },
        { "wipe_tower_rib_width", 6.5 },
        { "wipe_tower_extra_rib_length", 2.5 },
        { "wipe_tower_fillet_wall", "0" },
        { "wipe_tower_max_purge_speed", 70 },
    });
    cfg.set_key_value("different_settings_to_system", new ConfigOptionStrings({ "wipe_tower_wall_type;wipe_tower_rib_width;wipe_tower_max_purge_speed", "", "" }));
    BambuExport::Context      ctx = BambuExport::Context::from_project(cfg);
    BambuExport::Report       report;
    const BambuExport::Config out = BambuExport::convert_project(cfg, ctx, report);
    auto value = [&out](const char *key) {
        const auto it = out.find(key);
        return it == out.end() ? std::string("<missing>") : BambuExport::serialize(it->second);
    };
    CHECK(value("prime_tower_rib_wall") == "1");
    CHECK(value("prime_tower_rib_width") == "6.5");
    CHECK(value("prime_tower_extra_rib_length") == "2.5");
    CHECK(value("prime_tower_fillet_wall") == "0");
    CHECK(value("prime_tower_max_speed") == "70");
    CHECK(out.count("wipe_tower_wall_type") == 0);
    REQUIRE(out.count("different_settings_to_system") == 1);
    CHECK(out.at("different_settings_to_system").values.front() == "prime_tower_rib_wall;prime_tower_rib_width;prime_tower_max_speed");
}

// Run with BBL_RIB_DUMP_DIR or BBL_RIB_BASELINE_DIR set; see dump_or_compare().
TEST_CASE("Rectangle Bambu Lab towers and WipeTower2 towers are unchanged by the rib port", "[.BblRibBaseline]")
{
    SECTION("X1C rectangle")
    {
        Print print;
        Model model;
        dump_or_compare("x1c_rectangle", slice(print, model, machine_config(X1C, { { "wipe_tower_wall_type", "rectangle" } }), X1C.bed_center, true));
    }
    SECTION("H2D rectangle")
    {
        Print print;
        Model model;
        dump_or_compare("h2d_rectangle", slice(print, model, machine_config(H2D, { { "wipe_tower_wall_type", "rectangle" } }), H2D.bed_center, true));
    }
    // The same Bambu profiles sliced as a non-Bambu printer run WipeTower2.
    for (const char *wall : { "rectangle", "rib", "cone" }) {
        DYNAMIC_SECTION("WipeTower2 " << wall)
        {
            Print print;
            Model model;
            dump_or_compare(std::string("wt2_") + wall,
                            slice(print, model, machine_config(X1C, { { "wipe_tower_wall_type", wall }, { "printer_model", "Generic" } }), X1C.bed_center, false));
        }
    }
}

// Hidden, for a side-by-side check against Bambu Studio's own generator: for each machine and wall
// type, slices two 20 x 20 x 40 mm cubes on filaments 1 and 2 and writes
//   $BBL_RIB_LIVE_OUT/<machine>_<wall>.gcode        this tree's G-code
//   $BBL_RIB_LIVE_OUT/<machine>_<wall>_bambu.3mf    the same plate through Export Bambu 3MF
// Slice the .3mf with bambu-studio.exe --slice 0 and compare the tower moves.
TEST_CASE("Write rib tower plates for a Bambu Studio comparison", "[.BblRibLive]")
{
    const char *out_dir = std::getenv("BBL_RIB_LIVE_OUT");
    REQUIRE(out_dir != nullptr);
    for (const Machine *m : { &X1C, &H2D }) {
        for (const char *wall : { "rectangle", "rib" }) {
            DynamicPrintConfig cfg = machine_config(*m, { { "wipe_tower_wall_type", wall }, { "wipe_tower_fillet_wall", 1 } });
            Model model;
            add_plate(model, m->bed_center);
            const std::string name = std::string(m == &X1C ? "x1c" : "h2d") + "_" + wall;
            {
                Print print;
                print.is_BBL_printer() = true;
                print.apply(model, cfg);
                print.validate();
                print.set_status_silent();
                const std::string gcode = Test::gcode(print);
                boost::nowide::ofstream f((boost::filesystem::path(out_dir) / (name + ".gcode")).string(), std::ios::binary);
                f << gcode;
            }
            PlateData *plate   = new PlateData();
            plate->plate_index = 0;
            plate->objects_and_instances.emplace_back(0, 0);
            plate->objects_and_instances.emplace_back(1, 0);
            StoreParams sp;
            const std::string path = (boost::filesystem::path(out_dir) / (name + "_bambu.3mf")).string();
            BambuExport::Report report;
            sp.path            = path.c_str();
            sp.model           = &model;
            sp.config          = &cfg;
            sp.plate_data_list = { plate };
            sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
            sp.bambu_compat    = true;
            sp.bambu_report    = &report;
            CHECK(store_bbs_3mf(sp));
            release_PlateData_list(sp.plate_data_list);
            WARN(path << ": " << report.summary());
        }
    }
}
