#include <catch2/catch.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/Model.hpp"
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

TEST_CASE("Object towers: at most two towers per plate, in model list order", "[ObjectTowers]")
{
    Print print;
    Model model;
    add_row(model, H2D, { 2, 2, 2 }, 95.);
    process(print, model, machine_config(H2D, true), H2D);
    const ObjectTowerPlan plan = print.plan_object_towers();
    REQUIRE(plan.towers.size() == 2);
    REQUIRE(plan.warnings.size() == 1);
    CHECK(plan.warnings.front().find("cube2") != std::string::npos);
    CHECK(plan.towers[0].instance->model_instance->get_object()->name == "cube0");
    CHECK(plan.towers[1].instance->model_instance->get_object()->name == "cube1");
    CHECK(print.object_wipe_towers().size() <= 2);
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
