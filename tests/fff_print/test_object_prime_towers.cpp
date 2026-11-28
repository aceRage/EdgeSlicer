#include <catch2/catch.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/ExtruderAreas.hpp"
#include "libslic3r/GCode/WipeTowerEstimate.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "test_data.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Per-object prime towers, PR 3 and PR 4 (tests/design_per_object_prime_towers.md): with prime_tower_per_object on a plate
// printed by object, every copy that changes filament gets its own tower beside it (Type 1 on Bambu Lab printers, Type 2 on
// the U1 and Voron), printed inside its own block. With the setting off nothing changes (the G-code gate proves that).

using namespace Slic3r;

namespace {

PresetBundle &vendor_bundle(const std::string &vendor)
{
    static std::map<std::string, std::unique_ptr<PresetBundle>> bundles;
    static std::unique_ptr<PresetBundle>                        library;
    auto it = bundles.find(vendor);
    if (it != bundles.end())
        return *it->second;
    const std::string             saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("objtowers_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    if (!library) {
        library = std::make_unique<PresetBundle>();
        library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                               ForwardCompatibilitySubstitutionRule::EnableSilent);
    }
    auto b = std::make_unique<PresetBundle>();
    b->load_vendor_configs_from_json(profiles, vendor, PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                     library.get());
    set_data_dir(saved_data_dir);
    return *(bundles[vendor] = std::move(b));
}

struct Machine
{
    const char *vendor;
    const char *printer;
    const char *process;
    const char *filament;
    Vec2d       bed_center;
    // The plate tower's position (the presets' own may not suit these plates); only a one-copy plate builds it.
    Vec2d       tower;
    bool        bbl;
};

const Machine X1C{ "BBL", "Bambu Lab X1 Carbon 0.4 nozzle", "0.20mm Standard @BBL X1C", "Bambu PLA Basic @BBL X1C", { 128., 128. }, { 30., 200. }, true };
const Machine H2D{ "BBL", "Bambu Lab H2D 0.4 nozzle", "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D", { 165., 160. }, { 40., 250. }, true };
const Machine H2C{ "BBL", "Bambu Lab H2C 0.4 nozzle", "0.20mm Standard @BBL H2C", "Bambu PLA Basic @BBL H2C", { 165., 160. }, { 40., 250. }, true };
const Machine U1{ "Snapmaker", "Snapmaker U1 (0.4 nozzle)", "0.20mm Standard @Snapmaker U1 (0.4 nozzle)", "Generic PLA @U1 0.4 nozzle", { 135., 135. }, { 30., 210. }, false };
// A Type 2 single-extruder multi-material printer (the gate's Voron case): its tower purges by the flush matrix and, by
// layer, ends with a final purge.
const Machine Voron{ "Voron", "Voron 2.4 300 0.4 nozzle", "0.20mm Standard @Voron", "Generic PLA @System", { 150., 150. }, { 30., 240. }, false };

DynamicPrintConfig machine_config(const Machine &m, bool per_object, bool by_object = true, bool tower = true)
{
    PresetBundle &b = vendor_bundle(m.vendor);
    REQUIRE(b.printers.select_preset_by_name(m.printer, true));
    REQUIRE(b.prints.select_preset_by_name(m.process, true));
    REQUIRE(b.filaments.select_preset_by_name(m.filament, true));
    b.filament_presets = { m.filament };
    b.set_num_filaments(2, std::vector<std::string>{ "#E01919", "#1943E0" });
    b.filament_presets = std::vector<std::string>(2, m.filament);
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "enable_prime_tower", tower ? "1" : "0" },
        { "prime_tower_per_object", per_object ? "1" : "0" },
        { "wipe_tower_x", m.tower.x() },
        { "wipe_tower_y", m.tower.y() },
        { "wipe_tower_rotation_angle", 0 },
        { "gcode_comments", 1 },
        { "print_sequence", by_object ? "by object" : "by layer" },
    });
    return cfg;
}

// A 20 x 20 x 1 mm cube: walls in filament 1 and infill in filament `infill` (2: every layer changes filament).
ModelObject *add_cube(Model &model, const std::string &name, int infill, const Vec3f &scale = Vec3f(1.f, 1.f, 0.05f))
{
    TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
    cube.scale(scale);
    ModelObject *object = model.add_object();
    object->name        = name;
    object->add_volume(cube);
    object->volumes.front()->name = name;
    object->config.set("wall_filament", 1);
    object->config.set("sparse_infill_filament", infill);
    object->config.set("solid_infill_filament", infill);
    return object;
}

void place(ModelObject *object, const Vec2d &pos)
{
    ModelInstance *instance = object->add_instance();
    instance->set_offset(Vec3d(pos.x(), pos.y(), 0.));
    object->center_around_origin();
    object->ensure_on_bed();
}

// Cubes in a row along X, `gap` mm apart (centre to centre), around the bed centre; colours[i] is the infill filament.
void add_row(Model &model, const Machine &m, const std::vector<int> &colours, double gap)
{
    for (size_t i = 0; i < colours.size(); ++i) {
        ModelObject *object = add_cube(model, "cube" + std::to_string(i), colours[i]);
        place(object, Vec2d(m.bed_center.x() + gap * (double(i) - 0.5 * double(colours.size() - 1)), m.bed_center.y()));
    }
}

StringObjectException apply_validate(Print &print, Model &model, const DynamicPrintConfig &cfg, const Machine &m,
                                     std::vector<StringObjectException> *warnings = nullptr)
{
    print.is_BBL_printer() = m.bbl;
    print.apply(model, cfg);
    return print.validate(warnings);
}

void process(Print &print, Model &model, const DynamicPrintConfig &cfg, const Machine &m)
{
    const StringObjectException error = apply_validate(print, model, cfg, m);
    REQUIRE(error.string.empty());
    print.set_status_silent();
    print.process();
}

size_t count_of(const std::string &text, const std::string &needle)
{
    size_t n = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size()))
        ++n;
    return n;
}

// The G-code of each copy's block: split where the export moves to the next copy.
std::vector<std::string> blocks_of(const std::string &gcode)
{
    const std::string        marker = "move to origin position for next object";
    std::vector<std::string> out;
    size_t                   start = 0;
    for (size_t pos = gcode.find(marker); pos != std::string::npos; pos = gcode.find(marker, pos + marker.size())) {
        out.push_back(gcode.substr(start, pos - start));
        start = pos;
    }
    out.push_back(gcode.substr(start));
    return out;
}

const std::string TOWER_START = "WIPE_TOWER_START";

// The extruding moves inside a block's tower sections (between WIPE_TOWER_START and WIPE_TOWER_END): where they lie on the
// bed, and the highest Z they print at.
struct TowerMoves
{
    BoundingBoxf box;
    double       max_z      = 0.;
    // The lowest Z any move of the block goes to (towers or not).
    double       min_any_z  = std::numeric_limits<double>::max();
    size_t       extrusions = 0;
};

TowerMoves tower_moves(const std::string &block)
{
    TowerMoves         out;
    bool               in_tower = false;
    double             z        = 0.;
    std::istringstream lines(block);
    for (std::string line; std::getline(lines, line);) {
        if (line.find("WIPE_TOWER_START") != std::string::npos) {
            in_tower = true;
            continue;
        }
        if (line.find("WIPE_TOWER_END") != std::string::npos) {
            in_tower = false;
            continue;
        }
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
            continue;
        if (const size_t comment = line.find(';'); comment != std::string::npos)
            line.resize(comment);
        std::istringstream words(line);
        bool               has_x = false, has_y = false;
        double             x = 0., y = 0., e = 0.;
        for (std::string word; words >> word;) {
            if (word.size() < 2)
                continue;
            const double value = std::atof(word.c_str() + 1);
            switch (word.front()) {
            case 'X': x = value; has_x = true; break;
            case 'Y': y = value; has_y = true; break;
            case 'Z': z = value; out.min_any_z = std::min(out.min_any_z, z); break;
            case 'E': e = value; break;
            default: break;
            }
        }
        if (in_tower && has_x && has_y && e > 0.) {
            out.box.merge(Vec2d(x, y));
            out.max_z = std::max(out.max_z, z);
            ++out.extrusions;
        }
    }
    return out;
}

// G-code per block on a Type 2 printer: each block prints its own tower, where that tower stands, no higher than its copy;
// no print-start priming, no final purge.
void check_type2_blocks(Print &print)
{
    REQUIRE(print.object_wipe_towers().size() == 2);
    std::string gcode;
    REQUIRE_NOTHROW(gcode = Test::gcode(print));
    CHECK(print.by_object_plan_misses() == 0);
    CHECK(count_of(gcode, "CP PRIMING") == 0);
    CHECK(count_of(gcode, "M1 Remove priming towers") == 0);
    const std::vector<std::string> blocks = blocks_of(gcode);
    REQUIRE(blocks.size() == 2);
    for (size_t i = 0; i < blocks.size(); ++i) {
        INFO("block " << i);
        const ObjectWipeTower &tower = *print.object_wipe_towers()[i];
        CHECK(count_of(blocks[i], TOWER_START) > 0);
        CHECK(count_of(blocks[i], TOWER_START) == count_of(blocks[i], "WIPE_TOWER_END"));
        const TowerMoves moves = tower_moves(blocks[i]);
        REQUIRE(moves.extrusions > 0);
        // Where the plan put this copy's tower (its generated outline, brim included; rotation 0), not where the other
        // copy's tower or the plate tower's wipe_tower_x / wipe_tower_y would be.
        REQUIRE(tower.data->wipe_tower_mesh_data.has_value());
        BoundingBox outline = get_extents(tower.data->wipe_tower_mesh_data->bottom);
        BoundingBoxf expected(unscale(outline.min) + tower.pos, unscale(outline.max) + tower.pos);
        expected.offset(3.);
        INFO("tower moves " << moves.box.min.transpose() << " - " << moves.box.max.transpose() << ", expected " << expected.min.transpose()
                            << " - " << expected.max.transpose());
        CHECK(expected.contains(moves.box.min));
        CHECK(expected.contains(moves.box.max));
        // Never below the bed. The first tower change of a later block once 'unlifted' the z-hop the travel to the copy
        // had taken, below the first layer (U1: G1 Z-.15).
        CHECK(moves.min_any_z >= print.config().z_offset.value - 1e-6);
        // Never taller than its own copy (1 mm cubes).
        const double top = unscale<double>(tower.instance->print_object->size().z());
        CHECK(moves.max_z <= top + print.config().z_offset.value + 0.25);
        // No final purge at the end of the block.
        CHECK(tower.data->final_purge->gcode.empty());
    }
    // The summed statistics are the towers' own.
    std::vector<float> sum(print.config().filament_diameter.size(), 0.f);
    for (const std::unique_ptr<ObjectWipeTower> &tower : print.object_wipe_towers())
        for (size_t i = 0; i < tower->data->used_filament.size() && i < sum.size(); ++i)
            sum[i] += tower->data->used_filament[i];
    REQUIRE(print.object_tower_statistics().used_filament.size() == sum.size());
    for (size_t i = 0; i < sum.size(); ++i)
        CHECK(std::abs(print.object_tower_statistics().used_filament[i] - sum[i]) < 1e-4f);
    CHECK(sum[0] + sum[1] > 0.f);
}

} // namespace

TEST_CASE("Object towers: normalize_fdm_2 keeps the tower only for an opted-in by-object plate of copies", "[ObjectTowers]")
{
    auto make = [](bool per_object, const char *sequence, const char *printer_model, bool spiral) {
        DynamicPrintConfig cfg;
        cfg.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
        cfg.set_key_value("prime_tower_per_object", new ConfigOptionBool(per_object));
        cfg.set_deserialize_strict({ { "print_sequence", sequence } });
        cfg.set_key_value("printer_model", new ConfigOptionString(printer_model));
        cfg.set_key_value("spiral_mode", new ConfigOptionBool(spiral));
        return cfg;
    };
    struct Row { bool per_object; const char *sequence; const char *model; bool spiral; int copies; int filaments; bool tower; };
    const Row rows[] = {
        { false, "by object", "Bambu Lab X1 Carbon", false, 2, 2, false }, // today: off for several copies
        { true,  "by object", "Bambu Lab X1 Carbon", false, 2, 2, true },  // opted in: kept
        { true,  "by object", "Bambu Lab X1 Carbon", false, 1, 2, true },  // one copy: kept, as today
        { false, "by object", "Bambu Lab X1 Carbon", false, 1, 2, true },
        { true,  "by object", "Bambu Lab X1 Carbon", false, 2, 1, false }, // one filament: off, as today
        { true,  "by object", "Bambu Lab X1 Carbon", true,  2, 2, false }, // spiral vase
        { true,  "by object", "Snapmaker U1",        false, 2, 2, true },  // Type 2 (PR 4): kept
        { false, "by object", "Snapmaker U1",        false, 2, 2, false }, // Type 2, off: as today
        { true,  "by object", "Voron 2.4 300",       false, 2, 2, true },  // Type 2, SEMM
        { true,  "by layer",  "Bambu Lab X1 Carbon", false, 2, 2, true },  // by layer: untouched
    };
    for (const Row &row : rows) {
        DynamicPrintConfig cfg = make(row.per_object, row.sequence, row.model, row.spiral);
        cfg.normalize_fdm_2(row.copies, row.filaments);
        INFO(row.model << " per_object=" << row.per_object << " " << row.sequence << " spiral=" << row.spiral << " copies=" << row.copies
                       << " filaments=" << row.filaments);
        CHECK(cfg.opt_bool("enable_prime_tower") == row.tower);
    }
}

TEST_CASE("Object towers: predicates, off and on (X1C, two 2-colour cubes by object)", "[ObjectTowers]")
{
    SECTION("off: no plate tower, no object towers, as today")
    {
        Print print;
        Model model;
        add_row(model, X1C, { 2, 2 }, 110.);
        process(print, model, machine_config(X1C, false), X1C);
        CHECK_FALSE(print.config().enable_prime_tower.value);
        CHECK_FALSE(print.object_towers_requested());
        CHECK_FALSE(print.has_wipe_tower());
        CHECK_FALSE(print.has_object_wipe_towers());
    }
    SECTION("on: the tower stays on in the config, the plate has no plate tower, each copy has its own")
    {
        Print print;
        Model model;
        add_row(model, X1C, { 2, 2 }, 110.);
        process(print, model, machine_config(X1C, true), X1C);
        CHECK(print.config().enable_prime_tower.value);
        CHECK(print.object_towers_requested());
        CHECK(print.object_tower_mode());
        CHECK_FALSE(print.has_wipe_tower());
        REQUIRE(print.object_wipe_towers().size() == 2);
        CHECK(print.object_wipe_towers()[0]->instance != print.object_wipe_towers()[1]->instance);
        CHECK(print.wipe_tower_data().tool_changes.empty());
    }
    SECTION("on, one copy: the by-layer plate tower of today")
    {
        Print print;
        Model model;
        add_row(model, X1C, { 2 }, 110.);
        process(print, model, machine_config(X1C, true), X1C);
        CHECK_FALSE(print.object_towers_requested());
        CHECK(print.has_wipe_tower());
        CHECK_FALSE(print.has_object_wipe_towers());
    }
}

TEST_CASE("Object towers: a Type 2 printer builds its towers too, with no notice (U1)", "[ObjectTowers]")
{
    Print print;
    Model model;
    add_row(model, U1, { 2, 2 }, 110.);
    std::vector<StringObjectException> warnings;
    const StringObjectException error = apply_validate(print, model, machine_config(U1, true), U1, &warnings);
    REQUIRE(error.string.empty());
    CHECK(print.wipe_tower_type() == WipeTowerType::Type2);
    CHECK(print.config().enable_prime_tower.value);
    CHECK(print.object_towers_requested());
    CHECK(print.object_tower_mode());
    CHECK_FALSE(print.has_wipe_tower());
    for (const StringObjectException &w : warnings) {
        INFO(w.string);
        CHECK(w.opt_key != "prime_tower_per_object");
    }
    print.set_status_silent();
    print.process();
    REQUIRE(print.object_wipe_towers().size() == 2);
    for (const std::unique_ptr<ObjectWipeTower> &tower : print.object_wipe_towers()) {
        // No print-start priming and no final purge on a copy's tower.
        REQUIRE(tower->data->priming != nullptr);
        CHECK(tower->data->priming->empty());
        REQUIRE(tower->data->final_purge != nullptr);
        CHECK(tower->data->final_purge->gcode.empty());
        CHECK_FALSE(tower->data->tool_changes.empty());
        REQUIRE(tower->data->wipe_tower_mesh_data.has_value());
        CHECK_FALSE(tower->data->wipe_tower_mesh_data->bottom.empty());
    }
    // The plate tower is not generated.
    CHECK(print.wipe_tower_data().tool_changes.empty());
}

TEST_CASE("Object towers: each tower stands beside its copy, on the bed, clear of the other copy", "[ObjectTowers]")
{
    for (const Machine *m : { &X1C, &H2D, &U1 }) {
        DYNAMIC_SECTION(m->printer)
        {
            Print print;
            Model model;
            add_row(model, *m, { 2, 2 }, 110.);
            process(print, model, machine_config(*m, true), *m);
            const ObjectTowerPlan plan = print.plan_object_towers();
            REQUIRE(plan.towers.size() == 2);
            CHECK(plan.warnings.empty());
            Polygon bed;
            bed.points = get_bed_shape(print.config());
            for (const ObjectTowerPlacement &t : plan.towers) {
                REQUIRE(t.placed);
                CHECK(diff(Polygons{ t.outline }, Polygons{ bed }).empty());
                for (const PrintObject *object : print.objects())
                    for (const PrintInstance &instance : object->instances()) {
                        Polygon hull = object->model_object()->convex_hull_2d(Geometry::assemble_transform(
                            Vec3d::Zero(), instance.model_instance->get_rotation(), instance.model_instance->get_scaling_factor(),
                            instance.model_instance->get_mirror()));
                        hull.translate(instance.shift - object->center_offset());
                        // Never on a copy, its own included.
                        CHECK(intersection(Polygons{ hull }, Polygons{ t.outline }).empty());
                    }
            }
            // The generated towers are where the plan put them.
            for (const std::unique_ptr<ObjectWipeTower> &tower : print.object_wipe_towers()) {
                const ObjectTowerPlacement *t = plan.find(tower->instance);
                REQUIRE(t != nullptr);
                CHECK(tower->pos == t->pos);
            }
        }
    }
}

TEST_CASE("Object towers: a copy that has no room keeps printing, without a tower and with a notice", "[ObjectTowers]")
{
    // Two 120 x 238 mm slabs 10 mm apart fill the X1C's bed (its cutter exclusion cleared): no side of either has room
    // for a tower.
    Print print;
    Model model;
    for (int i = 0; i < 2; ++i) {
        ModelObject *object = add_cube(model, "slab" + std::to_string(i), 2, Vec3f(6.f, 11.9f, 0.05f));
        place(object, Vec2d(63. + 130. * i, 128.));
    }
    DynamicPrintConfig cfg = machine_config(X1C, true);
    cfg.option<ConfigOptionPoints>("bed_exclude_area", true)->values.clear();
    std::vector<StringObjectException> warnings;
    const StringObjectException error = apply_validate(print, model, cfg, X1C, &warnings);
    INFO(error.string);
    REQUIRE(error.string.empty());
    const ObjectTowerPlan plan = print.plan_object_towers();
    REQUIRE(plan.towers.size() == 2);
    CHECK_FALSE(plan.towers[0].placed);
    CHECK_FALSE(plan.towers[1].placed);
    CHECK(plan.warnings.size() == 2);
    size_t noticed = 0;
    for (const StringObjectException &w : warnings)
        noticed += w.opt_key == "prime_tower_per_object" ? 1 : 0;
    CHECK(noticed == 2);
    print.set_status_silent();
    print.process();
    CHECK_FALSE(print.has_object_wipe_towers());
    CHECK_FALSE(print.has_wipe_tower());
}

// Five 2-colour cubes on the H2D: three in a row at the front, two behind (the cap is 4 since 2026-11-27, owner).
static void add_five(Model &model)
{
    const Vec2d spots[] = { { 70., 100. }, { 165., 100. }, { 260., 100. }, { 117., 220. }, { 212., 220. } };
    for (size_t i = 0; i < 5; ++i)
        place(add_cube(model, "cube" + std::to_string(i), 2), spots[i]);
}

TEST_CASE("Object towers: at most four towers per plate, in model list order", "[ObjectTowers]")
{
    STATIC_REQUIRE(ObjectTowerPlan::max_object_towers == 4);
    Print print;
    Model model;
    add_five(model);
    process(print, model, machine_config(H2D, true), H2D);
    const ObjectTowerPlan plan = print.plan_object_towers();
    REQUIRE(plan.towers.size() == 4);
    for (size_t i = 0; i < 4; ++i) {
        CHECK(plan.towers[i].instance->model_instance->get_object()->name == "cube" + std::to_string(i));
        CHECK(plan.towers[i].placed);
    }
    // The fifth prints without a tower, with the notice.
    REQUIRE(plan.warnings.size() == 1);
    CHECK(plan.warnings.front().find("cube4") != std::string::npos);
    CHECK(plan.warnings.front().find("at most 4 prime towers") != std::string::npos);
    CHECK(print.object_wipe_towers().size() == 4);
    for (const std::unique_ptr<ObjectWipeTower> &tower : print.object_wipe_towers())
        CHECK(tower->instance->model_instance->get_object()->name != "cube4");
}

TEST_CASE("Object towers: three and four towers on one plate", "[ObjectTowers]")
{
    for (const Machine *m : { &X1C, &H2D }) {
        for (size_t count : { size_t(3), size_t(4) }) {
            DYNAMIC_SECTION(m->printer << ", " << count << " copies")
            {
                Print print;
                Model model;
                // Two rows of short 2-colour cubes, 100 mm apart in both directions.
                for (size_t i = 0; i < count; ++i)
                    place(add_cube(model, "cube" + std::to_string(i), 2),
                          Vec2d(m->bed_center.x() - 50. + 100. * double(i % 2), m->bed_center.y() - 50. + 100. * double(i / 2)));
                process(print, model, machine_config(*m, true), *m);
                const ObjectTowerPlan plan = print.plan_object_towers();
                REQUIRE(plan.towers.size() == count);
                CHECK(plan.warnings.empty());
                for (const ObjectTowerPlacement &t : plan.towers)
                    CHECK(t.placed);
                CHECK(print.object_wipe_towers().size() == count);
            }
        }
    }
}

TEST_CASE("Object towers: a single-colour copy gets no tower and prints its block as without the setting", "[ObjectTowers]")
{
    Print print;
    Model model;
    add_row(model, X1C, { 2, 1 }, 110.);
    process(print, model, machine_config(X1C, true), X1C);
    const ObjectTowerPlan plan = print.plan_object_towers();
    REQUIRE(plan.towers.size() == 1);
    CHECK(plan.towers.front().instance->model_instance->get_object()->name == "cube0");
    REQUIRE(print.object_wipe_towers().size() == 1);
    const std::string              gcode  = Test::gcode(print);
    const std::vector<std::string> blocks = blocks_of(gcode);
    REQUIRE(blocks.size() == 2);
    // The towered block has its tower; the single-colour one has none.
    size_t with_tower = 0;
    for (const std::string &block : blocks)
        with_tower += count_of(block, TOWER_START) > 0 ? 1 : 0;
    CHECK(with_tower == 1);
}

TEST_CASE("Object towers: G-code per block (X1C, two 2-colour cubes)", "[ObjectTowers]")
{
    Print print;
    Model model;
    add_row(model, X1C, { 2, 2 }, 110.);
    process(print, model, machine_config(X1C, true), X1C);
    REQUIRE(print.object_wipe_towers().size() == 2);
    const std::string              gcode  = Test::gcode(print);
    const std::vector<std::string> blocks = blocks_of(gcode);
    REQUIRE(blocks.size() == 2);
    for (size_t i = 0; i < blocks.size(); ++i) {
        INFO("block " << i);
        // Each block prints its own tower: one tower section per tower layer of its own tower.
        const ObjectWipeTower *tower = print.object_wipe_towers()[i].get();
        size_t                 planned = 0;
        for (const std::vector<WipeTower::ToolChangeResult> &layer : tower->data->tool_changes)
            planned += layer.size();
        CHECK(planned > 0);
        CHECK(count_of(blocks[i], TOWER_START) > 0);
        // Every tower section that starts in the block ends in it.
        CHECK(count_of(blocks[i], TOWER_START) == count_of(blocks[i], "WIPE_TOWER_END"));
    }
    // The summed statistics are the towers' own.
    std::vector<float> sum(print.config().filament_diameter.size(), 0.f);
    for (const std::unique_ptr<ObjectWipeTower> &tower : print.object_wipe_towers())
        for (size_t i = 0; i < tower->data->used_filament.size() && i < sum.size(); ++i)
            sum[i] += tower->data->used_filament[i];
    REQUIRE(print.object_tower_statistics().used_filament.size() == sum.size());
    for (size_t i = 0; i < sum.size(); ++i)
        CHECK(std::abs(print.object_tower_statistics().used_filament[i] - sum[i]) < 1e-4f);
    CHECK(print.object_tower_statistics().used_filament[0] + print.object_tower_statistics().used_filament[1] > 0.f);
    // A copy's tower has no final purge.
    for (const std::unique_ptr<ObjectWipeTower> &tower : print.object_wipe_towers()) {
        REQUIRE(tower->data->final_purge != nullptr);
        CHECK(tower->data->final_purge->gcode.empty());
    }
}

TEST_CASE("Object towers: two copies of one object get a tower each", "[ObjectTowers]")
{
    Print print;
    Model model;
    ModelObject *object = add_cube(model, "twin", 2);
    place(object, Vec2d(X1C.bed_center.x() - 55., X1C.bed_center.y()));
    object->add_instance()->set_offset(Vec3d(X1C.bed_center.x() + 55., X1C.bed_center.y(), object->instances.front()->get_offset().z()));
    process(print, model, machine_config(X1C, true), X1C);
    REQUIRE(print.object_wipe_towers().size() == 2);
    CHECK(print.object_wipe_towers()[0]->pos != print.object_wipe_towers()[1]->pos);
    const std::string gcode = Test::gcode(print);
    for (const std::string &block : blocks_of(gcode))
        CHECK(count_of(block, TOWER_START) > 0);
}

TEST_CASE("Object towers: a tower follows its copy when the copy moves", "[ObjectTowers]")
{
    Print print;
    Model model;
    add_row(model, X1C, { 2, 2 }, 110.);
    const DynamicPrintConfig cfg = machine_config(X1C, true);
    process(print, model, cfg, X1C);
    REQUIRE(print.object_wipe_towers().size() == 2);
    const Vec2d before = print.object_wipe_towers()[0]->pos;
    // Move every copy 10 mm back: the towers move with them.
    for (ModelObject *object : model.objects)
        for (ModelInstance *instance : object->instances)
            instance->set_offset(instance->get_offset() + Vec3d(0., 10., 0.));
    process(print, model, cfg, X1C);
    REQUIRE(print.object_wipe_towers().size() == 2);
    const Vec2d after = print.object_wipe_towers()[0]->pos;
    CHECK(std::abs(after.y() - before.y() - 10.) < 1e-6);
}

TEST_CASE("Object towers: a copy's tower on a raft keeps its gap layer and re-runs cleanly", "[ObjectTowers]")
{
    Print print;
    Model model;
    add_row(model, X1C, { 2, 2 }, 110.);
    DynamicPrintConfig cfg = machine_config(X1C, true);
    cfg.set_deserialize_strict({ { "raft_layers", 3 }, { "raft_contact_distance", 0.6 } });
    process(print, model, cfg, X1C);
    REQUIRE(print.object_wipe_towers().size() == 2);
    std::vector<size_t> support_layers;
    for (const PrintObject *object : print.objects())
        support_layers.push_back(object->support_layers().size());
    const size_t virtual_layers = print.object_wipe_towers()[0]->data->virtual_support_layers.size();
    // Re-run only the tower step (a flush volume change does that): nothing is added to the objects, the copy's tower has
    // the same tower-only layers.
    DynamicPrintConfig cfg2 = cfg;
    cfg2.set_deserialize_strict({ { "flush_multiplier", 0.9 } });
    process(print, model, cfg2, X1C);
    REQUIRE(print.object_wipe_towers().size() == 2);
    for (size_t i = 0; i < print.objects().size(); ++i)
        CHECK(print.objects()[i]->support_layers().size() == support_layers[i]);
    CHECK(print.object_wipe_towers()[0]->data->virtual_support_layers.size() == virtual_layers);
}

TEST_CASE("Object towers: H2C blocks print M620.14 beside their own tower", "[ObjectTowers][slow]")
{
    Print print;
    Model model;
    add_row(model, H2C, { 2, 2 }, 120.);
    DynamicPrintConfig cfg = machine_config(H2C, true);
    cfg.set_deserialize_strict({ { "filament_map_mode", "Manual" }, { "filament_map", "1,2" } });
    process(print, model, cfg, H2C);
    REQUIRE(print.object_wipe_towers().size() == 2);
    const std::string              gcode  = Test::gcode(print);
    const std::vector<std::string> blocks = blocks_of(gcode);
    REQUIRE(blocks.size() == 2);
    for (const std::string &block : blocks) {
        CHECK(count_of(block, TOWER_START) > 0);
        CHECK(count_of(block, "M620.14") > 0);
    }
}

// Owner hand test of #1207 (objtowers3_error.3mf): each object is two touching cubes, one part in filament 2 and one in
// filament 1, so the copy's first layer starts on one filament and its last layer ends on the other. The export chains even
// the first block from ToolOrdering(object, -1)'s last filament, so that block's ordering starts with another filament than
// the start G-code loaded, and it makes no tool change before the block. GCode::process_layer starts the layer with the
// loaded filament; the copy's tower planned from its ordering's first one, so the first change went the other way:
// "append_tcr unexpected toolchange: layer_idx=0 ... requested=1 expected=0 initial=1".
TEST_CASE("Object towers: a copy that ends on another filament than it starts with (owner's two-part objects)", "[ObjectTowers]")
{
    // Type 1 (X1C) and Type 2 (U1): both towers start their first change from the filament loaded when the block starts.
    for (const Machine *m : { &X1C, &U1 }) {
        DYNAMIC_SECTION(m->printer)
        {
            Print print;
            Model model;
            for (int i = 0; i < 2; ++i) {
                ModelObject *object = model.add_object();
                object->name        = "assembly" + std::to_string(i);
                for (int part = 0; part < 2; ++part) {
                    TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
                    cube.scale(Vec3f(1.f, 1.f, 0.05f)); // 20 x 20 x 1 mm: 5 layers at 0.2 mm
                    cube.translate(part == 0 ? 0.5f : -20.5f, 0.f, 0.f); // 1 mm apart, as the owner's cubes
                    ModelVolume *volume = object->add_volume(cube);
                    volume->name        = "cube" + std::to_string(part);
                    volume->config.set("extruder", part == 0 ? 2 : 1);
                }
                object->config.set("extruder", 2);
                place(object, Vec2d(m->bed_center.x() + (i == 0 ? -65. : 65.), m->bed_center.y()));
            }
            process(print, model, machine_config(*m, true), *m);

            const ByObjectPlan &plan = print.by_object_plan();
            REQUIRE(plan.blocks.size() == 2);
            // The case this covers: the first block's ordering starts with another filament than the start G-code loads.
            const ToolOrdering fresh(*plan.blocks[0].instance->print_object, (unsigned int) -1);
            REQUIRE(fresh.first_extruder() != fresh.last_extruder());
            CHECK(plan.blocks[0].loaded_extruder == fresh.first_extruder());
            CHECK(plan.blocks[0].first_extruder != plan.blocks[0].loaded_extruder);
            // Later blocks start after their entry change.
            CHECK(plan.blocks[1].loaded_extruder == plan.blocks[1].first_extruder);
            REQUIRE(print.object_wipe_towers().size() == 2);
            // Each tower's first change starts from the filament loaded when its block starts.
            for (const std::unique_ptr<ObjectWipeTower> &tower : print.object_wipe_towers()) {
                const ByObjectBlock *block = nullptr;
                for (const ByObjectBlock &b : plan.blocks)
                    if (b.instance == tower->instance)
                        block = &b;
                REQUIRE(block != nullptr);
                const WipeTower::ToolChangeResult *first_change = nullptr;
                for (const std::vector<WipeTower::ToolChangeResult> &layer : tower->data->tool_changes) {
                    for (const WipeTower::ToolChangeResult &tcr : layer)
                        if (tcr.initial_tool != tcr.new_tool) {
                            first_change = &tcr;
                            break;
                        }
                    if (first_change != nullptr)
                        break;
                }
                REQUIRE(first_change != nullptr);
                CHECK(first_change->initial_tool == int(block->loaded_extruder));
            }

            std::string gcode;
            REQUIRE_NOTHROW(gcode = Test::gcode(print));
            CHECK(print.by_object_plan_misses() == 0);
            // The travel to the second copy may be split into several moves (each carries the comment blocks_of() splits on).
            const std::vector<std::string> blocks = blocks_of(gcode);
            REQUIRE(blocks.size() >= 2);
            CHECK(count_of(blocks.front(), TOWER_START) > 0);
            CHECK(count_of(blocks.back(), TOWER_START) > 0);
        }
    }
}

TEST_CASE("Object towers: G-code per block on the U1 (G12, two 2-colour cubes)", "[ObjectTowers]")
{
    Print print;
    Model model;
    add_row(model, U1, { 2, 2 }, 110.);
    process(print, model, machine_config(U1, true), U1);
    check_type2_blocks(print);
}

TEST_CASE("Object towers: a single-extruder multi-material Type 2 printer (Voron) has no final purge per block", "[ObjectTowers]")
{
    Print print;
    Model model;
    add_row(model, Voron, { 2, 2 }, 110.);
    const DynamicPrintConfig cfg = machine_config(Voron, true);
    REQUIRE(cfg.opt_bool("single_extruder_multi_material"));
    process(print, model, cfg, Voron);
    CHECK(print.wipe_tower_type() == WipeTowerType::Type2);
    REQUIRE(print.object_tower_mode());
    check_type2_blocks(print);
    for (const std::unique_ptr<ObjectWipeTower> &tower : print.object_wipe_towers()) {
        CHECK(tower->data->priming->empty());
        CHECK_FALSE(tower->data->final_purge_drop);
    }
}

TEST_CASE("Object towers: setting off on the U1 builds no towers, as today", "[ObjectTowers]")
{
    Print print;
    Model model;
    add_row(model, U1, { 2, 2 }, 110.);
    process(print, model, machine_config(U1, false), U1);
    CHECK_FALSE(print.config().enable_prime_tower.value);
    CHECK_FALSE(print.object_towers_requested());
    CHECK_FALSE(print.has_wipe_tower());
    CHECK_FALSE(print.has_object_wipe_towers());
    const std::string gcode = Test::gcode(print);
    CHECK(count_of(gcode, TOWER_START) == 0);
}

// ---------------------------------------------------------------------------------------------
// PR 5 (GUI): the dragged position (prime_tower_offset_x/y) and the per-object opt-out (object_prime_tower), both
// object settings.
// ---------------------------------------------------------------------------------------------

namespace {

void set_tower_offset(ModelObject &object, const Vec2d &offset)
{
    object.config.set_key_value("prime_tower_offset_x", new ConfigOptionFloat(offset.x()));
    object.config.set_key_value("prime_tower_offset_y", new ConfigOptionFloat(offset.y()));
}

} // namespace

TEST_CASE("Object towers: the dragged offset and the opt-out round-trip through 3MF as object settings", "[ObjectTowers]")
{
    Model model;
    add_row(model, X1C, { 2, 2 }, 110.);
    set_tower_offset(*model.objects[0], Vec2d(-12.5, 21.25));
    model.objects[1]->config.set_key_value("object_prime_tower", new ConfigOptionBool(false));

    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("objtowers5_%%%%-%%%%");
    boost::filesystem::create_directories(tmp_root);
    const std::string saved_temporary_dir = Slic3r::temporary_dir();
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string file = (tmp_root / "object_towers.3mf").string();

    StoreParams store;
    store.path     = file.c_str();
    store.model    = &model;
    store.config   = &store_config;
    store.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store));

    Model                     loaded;
    DynamicPrintConfig        loaded_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool ok = load_bbs_3mf(file.c_str(), &loaded_config, &ctxt, &loaded, &plate_data, &project_presets, &is_bbl_3mf, &file_version, nullptr,
                                 LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    Slic3r::set_temporary_dir(saved_temporary_dir);
    boost::filesystem::remove_all(tmp_root);
    REQUIRE(ok);
    REQUIRE(loaded.objects.size() == 2);

    const ModelConfigObject &first = loaded.objects[0]->config;
    REQUIRE(first.has("prime_tower_offset_x"));
    REQUIRE(first.has("prime_tower_offset_y"));
    CHECK(std::abs(first.opt_float("prime_tower_offset_x") - -12.5) < 1e-9);
    CHECK(std::abs(first.opt_float("prime_tower_offset_y") - 21.25) < 1e-9);
    CHECK_FALSE(first.has("object_prime_tower"));
    const ModelConfigObject &second = loaded.objects[1]->config;
    REQUIRE(second.has("object_prime_tower"));
    CHECK_FALSE(second.option("object_prime_tower")->getBool());
    CHECK_FALSE(second.has("prime_tower_offset_x"));
}

TEST_CASE("Object towers: a dragged offset re-plans the towers and the G-code, nothing sliced", "[ObjectTowers]")
{
    Print print;
    Model model;
    add_row(model, X1C, { 2, 2 }, 110.);
    const DynamicPrintConfig cfg = machine_config(X1C, true);
    process(print, model, cfg, X1C);
    REQUIRE(print.object_wipe_towers().size() == 2);
    REQUIRE_FALSE(Test::gcode(print).empty());
    REQUIRE(print.is_step_done(psGCodeExport));

    const ObjectTowerPlan before = print.plan_object_towers();
    REQUIRE(before.towers.size() == 2);
    const ObjectTowerPlacement &auto0 = before.towers[0];
    REQUIRE(auto0.placed);
    // Keep the automatic spot, 3 mm further along: still clear.
    set_tower_offset(*model.objects[0], auto0.pos - auto0.copy_center + Vec2d(0., 3.));
    print.is_BBL_printer() = X1C.bbl;
    CHECK(print.apply(model, cfg) == Print::APPLY_STATUS_INVALIDATED);

    // Nothing sliced re-runs ...
    for (const PrintObject *object : print.objects()) {
        CHECK(object->is_step_done(posSlice));
        CHECK(object->is_step_done(posPerimeters));
        CHECK(object->is_step_done(posPrepareInfill));
        CHECK(object->is_step_done(posInfill));
    }
    // ... the towers and the G-code do (and the skirt and brims, which are laid around the towers).
    CHECK_FALSE(print.is_step_done(psWipeTower));
    CHECK_FALSE(print.is_step_done(psGCodeExport));

    print.set_status_silent();
    print.process();
    const ObjectTowerPlan after = print.plan_object_towers();
    REQUIRE(after.towers.size() == 2);
    CHECK(after.towers[0].from_offset);
    CHECK_FALSE(after.towers[0].offset_rejected);
    CHECK((after.towers[0].pos - (auto0.pos + Vec2d(0., 3.))).norm() < 1e-4);
    const ObjectWipeTower *tower = print.object_wipe_tower(after.towers[0].instance);
    REQUIRE(tower != nullptr);
    CHECK((tower->pos - after.towers[0].pos).norm() < 1e-9);
    // The other copy keeps its automatic tower.
    CHECK_FALSE(after.towers[1].from_offset);
}

TEST_CASE("Object towers: a dragged tower follows its copy after a move", "[ObjectTowers]")
{
    Print print;
    Model model;
    add_row(model, X1C, { 2, 2 }, 110.);
    const DynamicPrintConfig cfg = machine_config(X1C, true);
    process(print, model, cfg, X1C);
    const ObjectTowerPlan first = print.plan_object_towers();
    REQUIRE(first.towers.size() == 2);
    REQUIRE(first.towers[0].placed);
    const Vec2d offset = first.towers[0].pos - first.towers[0].copy_center + Vec2d(0., 3.);
    set_tower_offset(*model.objects[0], offset);
    process(print, model, cfg, X1C);
    const ObjectTowerPlan dragged = print.plan_object_towers();
    REQUIRE(dragged.towers.size() == 2);
    REQUIRE(dragged.towers[0].from_offset);

    // Move the copy 10 mm back and 5 mm right: its tower keeps its place beside it.
    model.objects[0]->instances[0]->set_offset(model.objects[0]->instances[0]->get_offset() + Vec3d(5., 10., 0.));
    process(print, model, cfg, X1C);
    const ObjectTowerPlan moved = print.plan_object_towers();
    REQUIRE(moved.towers.size() == 2);
    CHECK(moved.towers[0].from_offset);
    CHECK((moved.towers[0].pos - (dragged.towers[0].pos + Vec2d(5., 10.))).norm() < 1e-4);
    CHECK((moved.towers[0].pos - moved.towers[0].copy_center - offset).norm() < 1e-4);
    REQUIRE(print.object_wipe_towers().size() == 2);
    const ObjectWipeTower *tower = print.object_wipe_tower(moved.towers[0].instance);
    REQUIRE(tower != nullptr);
    CHECK((tower->pos - moved.towers[0].pos).norm() < 1e-9);
}

TEST_CASE("Object towers: a tower dragged onto its copy is placed automatically, with a notice", "[ObjectTowers]")
{
    Print print;
    Model model;
    add_row(model, X1C, { 2, 2 }, 110.);
    // The corner 5 mm inside the copy from its centre: on the copy itself.
    set_tower_offset(*model.objects[0], Vec2d(-5., -5.));
    std::vector<StringObjectException> warnings;
    const StringObjectException error = apply_validate(print, model, machine_config(X1C, true), X1C, &warnings);
    REQUIRE(error.string.empty());
    const ObjectTowerPlan plan = print.plan_object_towers();
    REQUIRE(plan.towers.size() == 2);
    CHECK(plan.towers[0].offset_rejected);
    CHECK_FALSE(plan.towers[0].from_offset);
    CHECK(plan.towers[0].placed);
    CHECK((plan.towers[0].requested_pos - (plan.towers[0].copy_center + Vec2d(-5., -5.))).norm() < 1e-4);
    REQUIRE(plan.warnings.size() == 1);
    CHECK(plan.warnings.front().find("cube0") != std::string::npos);
    size_t noticed = 0;
    for (const StringObjectException &w : warnings)
        noticed += w.opt_key == "prime_tower_per_object" ? 1 : 0;
    CHECK(noticed == 1);
    print.set_status_silent();
    print.process();
    CHECK(print.object_wipe_towers().size() == 2);
}

TEST_CASE("Object towers: an object taken out (object_prime_tower off) gets none and leaves the cap to others", "[ObjectTowers]")
{
    Print print;
    Model model;
    add_five(model);
    model.objects[0]->config.set_key_value("object_prime_tower", new ConfigOptionBool(false));
    process(print, model, machine_config(H2D, true), H2D);
    const ObjectTowerPlan plan = print.plan_object_towers();
    REQUIRE(plan.towers.size() == 4);
    // No copy is over the cap (cube0 does not count).
    for (const std::string &w : plan.warnings)
        CHECK(w.find("at most") == std::string::npos);
    for (size_t i = 0; i < 4; ++i)
        CHECK(plan.towers[i].instance->model_instance->get_object()->name == "cube" + std::to_string(i + 1));
    for (const std::unique_ptr<ObjectWipeTower> &tower : print.object_wipe_towers())
        CHECK(tower->instance->model_instance->get_object()->name != "cube0");
}

// ---------------------------------------------------------------------------------------------
// Arrange with per-object towers (owner hand test 2026-11-27: Arrange and Plate > Arrange pushed the other copies onto
// new plates). The arrange is run as the CLI and ArrangeJob run it for a by-object plate: the items from
// get_instance_arrange_poly, the by-object clearance, the bed exclusion zones as fixed items (and on the H2D the bed
// clipped to what both nozzles reach, "Avoid nozzle reach zones"); then the plate is planned from the arranged copies.
// ---------------------------------------------------------------------------------------------

namespace {

enum class TowerRoom { Units, OldInflation };

// Arranges every copy of `model` for a by-object plate of `m`; moves the copies (bed 0 only) and returns the items.
ArrangePolygons arrange_by_object(Model &model, const DynamicPrintConfig &cfg, const Machine &m, TowerRoom room)
{
    ArrangePolygons selected, unselected;
    for (ModelObject *object : model.objects)
        for (ModelInstance *instance : object->instances) {
            ArrangePolygon ap = get_instance_arrange_poly(instance, cfg);
            ap.itemid         = int(selected.size());
            if (room == TowerRoom::Units)
                reserve_object_prime_tower(ap, *object, cfg);
            selected.push_back(std::move(ap));
        }
    // The bed exclusion zones (the X1C's cutter corner), as PartPlateList::preprocess_exclude_areas adds them.
    if (const auto *zone = cfg.option<ConfigOptionPoints>("bed_exclude_area"); zone != nullptr && zone->values.size() >= 3)
        for (int bed = 0; bed < 8; ++bed) {
            ArrangePolygon ap;
            for (const Vec2d &p : zone->values)
                ap.poly.contour.points.emplace_back(scaled(p.x()), scaled(p.y()));
            ap.is_virt_object = true;
            ap.bed_idx        = bed;
            ap.height         = 1;
            ap.name           = "ExcludedRegion";
            unselected.push_back(std::move(ap));
        }

    ArrangeParams params;
    params.parallel                = false;
    params.is_seq_print            = true;
    params.clearance_height_to_rod = cfg.opt_float("extruder_clearance_height_to_rod");
    params.clearance_height_to_lid = cfg.opt_float("extruder_clearance_height_to_lid");
    params.clearance_radius        = sequential_clearance_radius(cfg);
    params.printable_height        = cfg.opt_float("printable_height");
    params.nozzle_height           = cfg.opt_float("nozzle_height");
    params.min_obj_distance        = 0;
    if (const auto *pos = cfg.option<ConfigOptionPoint>("best_object_pos"))
        params.align_center = pos->value;
    params.bed_shrink_x = BED_SHRINK_SEQ_PRINT;
    params.bed_shrink_y = BED_SHRINK_SEQ_PRINT;
    arrangement::update_arrange_params(params, &cfg, selected);
    arrangement::update_selected_items_inflation(selected, &cfg, params);
    arrangement::update_unselected_items_inflation(unselected, &cfg, params);
    if (room == TowerRoom::OldInflation)
        // PR 3/PR 5: every multi-colour item grown on all sides by its whole tower, on top of half the clearance.
        for (ArrangePolygon &ap : selected) {
            const WipeTowerFootprint tower = estimate_wipe_tower_footprint(cfg, WipeTowerType::Type1, { 0, 1 }, 0.2, ap.height);
            ap.inflation += scaled<coord_t>(std::max(tower.width, tower.depth) + 2. * tower.brim_width + 5.);
        }
    Points bed = arrangement::get_shrink_bedpts(&cfg, params);
    if (&m == &H2D) {
        // Left nozzle X 0..325, right 25..350.
        auto rect = [](double x0, double x1) { return Pointfs{ Vec2d(x0, 0), Vec2d(x1, 0), Vec2d(x1, 320), Vec2d(x0, 320) }; };
        REQUIRE(arrangement::clip_bed_to_nozzle_reach(bed, compute_extruder_areas(rect(0, 350), { rect(0, 325), rect(25, 350) }, {}),
                                                       scaled(std::max(0.f, -std::min(params.bed_shrink_x, params.bed_shrink_y)))));
    }
    arrangement::arrange(selected, unselected, bed, params);
    for (ArrangePolygon &ap : selected)
        if (ap.bed_idx == 0)
            ap.apply();
    return selected;
}

// `count` 20 x 20 x 10 mm cubes (taller than the nozzle, so the full by-object clearance applies), 2-colour unless
// `colours` says otherwise, all stacked at the bed centre before the arrange.
void add_tall_cubes(Model &model, const Machine &m, size_t count, int colours = 2)
{
    for (size_t i = 0; i < count; ++i)
        place(add_cube(model, "cube" + std::to_string(i), colours, Vec3f(1.f, 1.f, 0.5f)), m.bed_center);
}

} // namespace

TEST_CASE("Object towers: arrange keeps the copies and their towers on one plate", "[ObjectTowers]")
{
    for (const Machine *m : { &X1C, &H2D }) {
        for (size_t count : { size_t(2), size_t(3), size_t(4) }) {
            DYNAMIC_SECTION(m->printer << ", " << count << " copies")
            {
                Model model;
                add_tall_cubes(model, *m, count);
                const DynamicPrintConfig cfg   = machine_config(*m, true);
                const ArrangePolygons    items = arrange_by_object(model, cfg, *m, TowerRoom::Units);
                for (const ArrangePolygon &ap : items) {
                    INFO(ap.name);
                    CHECK(ap.bed_idx == 0);
                }
                // The slicer finds a clear spot beside every copy for its tower, and the plate validates.
                Print                              print;
                std::vector<StringObjectException> warnings;
                const StringObjectException        error = apply_validate(print, model, cfg, *m, &warnings);
                INFO(error.string);
                CHECK(error.string.empty());
                const ObjectTowerPlan plan = print.plan_object_towers();
                REQUIRE(plan.towers.size() == count);
                std::string notices;
                for (const std::string &w : plan.warnings)
                    notices += w + "; ";
                INFO(notices);
                CHECK(plan.warnings.empty());
                for (const ObjectTowerPlacement &t : plan.towers)
                    CHECK(t.placed);
            }
        }
    }
}

TEST_CASE("Object towers: the old all-round growth sent the copies to other plates (X1C)", "[ObjectTowers]")
{
    // The root cause of the owner's finding: a 20 mm copy grown by ~34 mm of clearance and ~45 mm of tower on every side
    // is ~180 mm wide, so a 256 mm bed takes one of them.
    Model model;
    add_tall_cubes(model, X1C, 3);
    const ArrangePolygons items = arrange_by_object(model, machine_config(X1C, true), X1C, TowerRoom::OldInflation);
    int beds = 0;
    for (const ArrangePolygon &ap : items)
        beds = std::max(beds, ap.bed_idx + 1);
    CHECK(beds > 1);
}

TEST_CASE("Object towers: arrange reserves no tower room where none prints", "[ObjectTowers]")
{
    const DynamicPrintConfig on = machine_config(X1C, true);
    Model model;
    add_tall_cubes(model, X1C, 1);
    ModelObject   &object = *model.objects.front();
    ArrangePolygon plain  = get_instance_arrange_poly(object.instances.front(), on);
    const double   area   = std::abs(plain.poly.contour.area());

    SECTION("2-colour copy, setting on: the item becomes the copy and its tower")
    {
        ArrangePolygon ap = plain;
        REQUIRE(reserve_object_prime_tower(ap, object, on));
        CHECK(std::abs(ap.poly.contour.area()) > 2. * area);
        // The copy is still inside its unit.
        CHECK(diff(Polygons{ plain.poly.contour }, Polygons{ ap.poly.contour }).empty());
    }
    SECTION("setting off")
    {
        ArrangePolygon ap = plain;
        CHECK_FALSE(reserve_object_prime_tower(ap, object, machine_config(X1C, false)));
        CHECK(ap.poly.contour.points == plain.poly.contour.points);
    }
    SECTION("object opted out")
    {
        object.config.set_key_value("object_prime_tower", new ConfigOptionBool(false));
        ArrangePolygon ap = plain;
        CHECK_FALSE(reserve_object_prime_tower(ap, object, on));
    }
    SECTION("single-colour copy")
    {
        object.config.set("sparse_infill_filament", 1);
        object.config.set("solid_infill_filament", 1);
        ArrangePolygon ap = plain;
        CHECK_FALSE(reserve_object_prime_tower(ap, object, on));
    }
    SECTION("a copy in one filament by its own extruder: that replaces the global feature filaments, as slicing does")
    {
        // The process prints walls in 1 and infill in 2 (two colours for an object without filament settings of its
        // own); this object only sets its filament to 2, which then prints every feature: one filament, no tower.
        DynamicPrintConfig two = on;
        two.set_key_value("sparse_infill_filament", new ConfigOptionInt(2));
        two.set_key_value("solid_infill_filament", new ConfigOptionInt(2));
        for (const char *key : { "wall_filament", "sparse_infill_filament", "solid_infill_filament" })
            object.config.erase(key);
        object.config.set("extruder", 2);
        ArrangePolygon ap = plain;
        CHECK_FALSE(reserve_object_prime_tower(ap, object, two));
        // The same object with no filament of its own follows the process: two colours, a tower.
        object.config.erase("extruder");
        ArrangePolygon ap2 = plain;
        CHECK(reserve_object_prime_tower(ap2, object, two));
    }
    SECTION("a dragged tower is reserved where it was dragged")
    {
        set_tower_offset(object, Vec2d(-60., 0.));
        ArrangePolygon ap = plain;
        REQUIRE(reserve_object_prime_tower(ap, object, on));
        CHECK(get_extents(ap.poly.contour).min.x() < get_extents(plain.poly.contour).min.x() - scaled(30.));
    }
}

// Owner hand test 2026-11-27 (objtowers3_error.3mf, plate 1): three tall two-part copies on the U1, each a pair of 33 mm
// cubes in filaments 2 and 1, with dragged towers. The middle copy's dragged tower was too close to another copy, so it
// was placed automatically, behind the copy. Every unit was clear in X/Y, but the copies reach above the rod (26 mm)
// and the tower widened that copy's Y band onto both neighbours' clearance: each then had to print before the other,
// and validate refused the plate ("Assembly is too tall, and collisions will be caused"). The planner now keeps a tall
// copy's tower off such spots (another side, or no tower with the notice), and real conflicts stay flagged.
namespace {

ModelObject *add_assembly(Model &model, const std::string &name, const Vec2d &pos, const Vec2d &tower_offset)
{
    ModelObject *object = model.add_object();
    object->name        = name;
    for (int side : { 1, -1 }) {
        TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
        cube.scale(1.65f); // 33 mm
        cube.translate(-cube.bounding_box().center().cast<float>());
        ModelVolume *volume = object->add_volume(cube);
        volume->set_offset(Vec3d(17. * side, 0., 0.));
        volume->config.set("extruder", side > 0 ? 2 : 1);
    }
    object->config.set("extruder", 2);
    if (tower_offset != Vec2d::Zero())
        set_tower_offset(*object, tower_offset);
    place(object, pos);
    return object;
}

DynamicPrintConfig owner_u1_config()
{
    DynamicPrintConfig cfg = machine_config(U1, true);
    // The owner's purge volumes (filament 1 -> 2: 203, 2 -> 1: 654 mm3, multiplier 1): the towers' size.
    cfg.set_deserialize_strict({ { "flush_volumes_matrix", "0,203,654,0" }, { "flush_multiplier", "1" } });
    return cfg;
}

} // namespace

TEST_CASE("Object towers: a tall copy's tower does not break the rod order (owner's three U1 copies)", "[ObjectTowers]")
{
    const DynamicPrintConfig cfg = owner_u1_config();
    REQUIRE(cfg.opt_float("extruder_clearance_height_to_rod") < 33.);

    SECTION("the owner's layout slices: no rod conflict, and a copy left without a tower says so")
    {
        Model model;
        add_assembly(model, "AssemblyA", { 90.1315, 25.0968 }, { -78.9164, -10.8642 });
        add_assembly(model, "AssemblyB", { 225.4107, 133.9143 }, { -77.9489, -18.4743 });
        add_assembly(model, "AssemblyC", { 45.5402, 241.5359 }, { 50.5761, -12.7808 });
        Print                              print;
        std::vector<StringObjectException> warnings;
        const StringObjectException        error = apply_validate(print, model, cfg, U1, &warnings);
        INFO(error.string);
        CHECK(error.string.empty());
        const ObjectTowerPlan plan = print.plan_object_towers();
        REQUIRE(plan.towers.size() == 3);
        // A and B keep their towers (B on its left, slid back, clear of A). C then has no spot: its right side is in
        // B's tower's clearance, left and back are off the bed, and in front its tower would put C and B in each
        // other's rod band. It prints without one, with the notice, instead of the plate being refused.
        CHECK(plan.towers[0].placed);
        CHECK(plan.towers[1].placed);
        size_t unplaced = 0;
        for (const ObjectTowerPlacement &t : plan.towers)
            unplaced += t.placed ? 0 : 1;
        size_t no_room = 0;
        for (const std::string &w : plan.warnings)
            no_room += w.find("no room") != std::string::npos ? 1 : 0;
        CHECK(no_room == unplaced);
    }
    SECTION("copies that really collide are still refused")
    {
        Model model;
        add_assembly(model, "AssemblyA", { 90., 40. }, Vec2d::Zero());
        add_assembly(model, "AssemblyB", { 150., 80. }, Vec2d::Zero());
        Print                       print;
        const StringObjectException error = apply_validate(print, model, cfg, U1);
        CHECK_FALSE(error.string.empty());
    }
}
