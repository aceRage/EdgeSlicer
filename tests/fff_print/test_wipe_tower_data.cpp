#include <catch2/catch.hpp>

#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTower2.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "test_data.hpp"

#include <boost/filesystem.hpp>

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Per-object prime towers, PR 2 (tests/design_per_object_prime_towers.md 2.4): Print::_make_wipe_tower() takes the tower's
// data, the copy it belongs to and its position as parameters, WipeTower / WipeTower2 take the position instead of reading
// the plate's wipe_tower_x / y, a copy's tower-only layers stay in its WipeTowerData, and
// GCode::collect_layers_to_print(object, warn, tower) merges them for that copy. The plate tower must come out exactly as
// before: the G-code gate proves that against main; these cases prove the parameters reach the generator.

namespace Slic3r {

struct WipeTowerDataTestAccess
{
    // A WipeTowerData of its own, with its own tool ordering (the plate's m_wipe_tower_data refers to Print::m_tool_ordering).
    struct Tower
    {
        ToolOrdering                   ordering;
        std::unique_ptr<WipeTowerData> data;
        Tower() : data(new_data(ordering)) {}
    };

    static std::unique_ptr<WipeTowerData> new_data(ToolOrdering &ordering)
    {
        return std::unique_ptr<WipeTowerData>(new WipeTowerData(ordering));
    }

    static void make(Print &print, WipeTowerData &data, const PrintInstance *inst, const Vec2d &pos)
    {
        print._make_wipe_tower(data, inst, pos);
    }
    static SupportLayer *new_virtual_layer(PrintObject &object, WipeTowerData &data, coordf_t height, coordf_t print_z)
    {
        SupportLayer *layer = object.new_detached_support_layer(size_t(-1), 0, height, print_z, print_z - 0.5 * height);
        data.virtual_support_layers.push_back(layer);
        return layer;
    }
};

} // namespace Slic3r

using namespace Slic3r;
using Access = WipeTowerDataTestAccess;

namespace {

PresetBundle &vendor_bundle(const std::string &vendor)
{
    static std::map<std::string, std::unique_ptr<PresetBundle>> bundles;
    static std::unique_ptr<PresetBundle>                        library;
    auto it = bundles.find(vendor);
    if (it != bundles.end())
        return *it->second;
    const std::string             saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("wtdata_%%%%-%%%%");
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
    Vec2d       tower;
    bool        bbl;
};

const Machine H2D{ "BBL", "Bambu Lab H2D 0.4 nozzle", "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D", { 165., 160. }, { 40., 250. }, true };
const Machine H2C{ "BBL", "Bambu Lab H2C 0.4 nozzle", "0.20mm Standard @BBL H2C", "Bambu PLA Basic @BBL H2C", { 165., 160. }, { 40., 250. }, true };
const Machine H3C{ "BBL", "Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Bambu PLA Basic @BBL H3C", { 165., 160. }, { 40., 250. }, true };
const Machine X1C{ "BBL", "Bambu Lab X1 Carbon 0.4 nozzle", "0.20mm Standard @BBL X1C", "Bambu PLA Basic @BBL X1C", { 128., 128. }, { 30., 200. }, true };
const Machine A1{ "BBL", "Bambu Lab A1 0.4 nozzle", "0.20mm Standard @BBL A1", "Bambu PLA Basic @BBL A1", { 128., 128. }, { 30., 200. }, true };
const Machine U1{ "Snapmaker", "Snapmaker U1 (0.4 nozzle)", "0.20mm Standard @Snapmaker U1 (0.4 nozzle)", "Generic PLA @U1 0.4 nozzle", { 135., 135. }, { 30., 210. }, false };

const Machine *const MACHINES[] = { &H2D, &H2C, &H3C, &X1C, &U1, &A1 };

// Another position on the bed, clear of the objects: the generator's output must follow it.
const Vec2d SHIFT{ 12., -6. };

DynamicPrintConfig machine_config(const Machine &m, bool tower, bool by_object, const Vec2d &tower_pos)
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
        { "wipe_tower_x", tower_pos.x() },
        { "wipe_tower_y", tower_pos.y() },
        { "wipe_tower_rotation_angle", 0 },
        { "gcode_comments", 0 },
        { "print_sequence", by_object ? "by object" : "by layer" },
    });
    return cfg;
}

// Two-colour 20 x 20 x 1 mm cubes (walls in filament 1, infill in filament 2), so every layer changes filament: two side
// by side printed by layer, or one printed by object (a by-object plate keeps its plate tower with one copy only).
void add_objects(Model &model, const Machine &m, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.05f));
        ModelObject *object = model.add_object();
        object->name        = "cube" + std::to_string(i);
        object->add_volume(cube);
        object->volumes.front()->name = object->name;
        object->config.set("wall_filament", 1);
        object->config.set("sparse_infill_filament", 2);
        object->config.set("solid_infill_filament", 2);
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(m.bed_center.x() + 30. * (double(i) - 0.5 * double(count - 1)), m.bed_center.y(), 0.));
        object->ensure_on_bed();
    }
}

void process(Print &print, Model &model, const DynamicPrintConfig &cfg, const Machine &m, size_t objects)
{
    add_objects(model, m, objects);
    print.is_BBL_printer() = m.bbl;
    print.apply(model, cfg);
    const StringObjectException error = print.validate();
    REQUIRE(error.string.empty());
    print.set_status_silent();
    print.process();
}

void write_tcr(std::ostream &ss, const WipeTower::ToolChangeResult &t)
{
    ss << "tcr z=" << t.print_z << " h=" << t.layer_height << " start=" << t.start_pos.x() << ',' << t.start_pos.y() << " end=" << t.end_pos.x()
       << ',' << t.end_pos.y() << " priming=" << t.priming << " purge=" << t.purge_volume << " tools=" << t.initial_tool << "->" << t.new_tool
       << " extrusions=" << t.extrusions.size() << " wipe=" << t.wipe_path.size() << '\n'
       << t.gcode << '\n';
}

// Everything the tower generation writes into a WipeTowerData. selected_final_purge: also the final purge variant picked by
// the clearance check, which reads the plate's configured tower position (not a generation input).
std::string signature(const WipeTowerData &d, bool selected_final_purge = true)
{
    std::ostringstream ss;
    ss.precision(10);
    ss << "layers " << d.tool_changes.size() << '\n';
    for (const std::vector<WipeTower::ToolChangeResult> &layer : d.tool_changes) {
        ss << "layer " << layer.size() << '\n';
        for (const WipeTower::ToolChangeResult &t : layer)
            write_tcr(ss, t);
    }
    ss << "local_z " << d.local_z_tool_changes.size() << '\n';
    for (const std::vector<WipeTower::ToolChangeResult> &layer : d.local_z_tool_changes)
        for (const WipeTower::ToolChangeResult &t : layer)
            write_tcr(ss, t);
    ss << "priming " << (d.priming ? d.priming->size() : size_t(0)) << '\n';
    if (d.priming)
        for (const WipeTower::ToolChangeResult &t : *d.priming)
            write_tcr(ss, t);
    if (selected_final_purge) {
        ss << "final_purge " << (d.final_purge != nullptr) << " drop=" << d.final_purge_drop << '\n';
        if (d.final_purge)
            write_tcr(ss, *d.final_purge);
    }
    ss << "final_purge_on_tower " << (d.final_purge_on_tower != nullptr) << '\n';
    if (d.final_purge_on_tower)
        write_tcr(ss, *d.final_purge_on_tower);
    ss << "final_purge_no_drop " << (d.final_purge_no_drop != nullptr) << '\n';
    if (d.final_purge_no_drop)
        write_tcr(ss, *d.final_purge_no_drop);
    ss << "will_ram=" << d.final_purge_will_ram << " last_z=" << d.final_purge_last_z << " last_h=" << d.final_purge_layer_height << '\n';
    ss << "used";
    for (float f : d.used_filament)
        ss << ' ' << f;
    ss << "\ntoolchanges=" << d.number_of_toolchanges << " depth=" << d.depth << " width=" << d.width << " brim=" << d.brim_width
       << " height=" << d.height << '\n';
    ss << "z_and_depth";
    for (const std::pair<float, float> &zd : d.z_and_depth_pairs)
        ss << ' ' << zd.first << ':' << zd.second;
    ss << "\nreserve_boxes " << d.local_z_reserve_boxes.size() << '\n';
    ss << "mesh " << d.wipe_tower_mesh_data.has_value();
    if (d.wipe_tower_mesh_data)
        for (const Point &p : d.wipe_tower_mesh_data->bottom.points)
            ss << ' ' << p.x() << ',' << p.y();
    ss << "\nvirtual_layers " << d.virtual_support_layers.size() << '\n';
    return ss.str();
}

std::string fake_signature(const FakeWipeTower &f)
{
    std::ostringstream ss;
    ss.precision(10);
    ss << "pos=" << f.pos.x() << ',' << f.pos.y() << " w=" << f.width << " h=" << f.height << " lh=" << f.layer_height << " d=" << f.depth
       << " brim=" << f.brim_width << " walls=" << f.outer_wall.size();
    return ss.str();
}

// The plate tower generated into a WipeTowerData of its own, at the plate's position (inst == nullptr), is the one
// process() made; at another position it is the one process() makes when the plate's tower stands there.
void check_plate_tower(const Machine &m, bool by_object)
{
    INFO(m.printer << (by_object ? ", one object printed by object" : ", two objects printed by layer"));
    const size_t objects = by_object ? 1 : 2;

    Print print;
    Model model;
    process(print, model, machine_config(m, true, by_object, m.tower), m, objects);
    REQUIRE(print.has_wipe_tower());
    const std::string before      = signature(print.wipe_tower_data());
    const std::string fake_before = fake_signature(print.get_fake_wipe_tower());
    REQUIRE_FALSE(print.wipe_tower_data().tool_changes.empty());
    CHECK(print.get_fake_wipe_tower().pos == Vec2f(float(m.tower.x()), float(m.tower.y())));
    // The tower changes filament on every layer it prints.
    CHECK(print.wipe_tower_data().number_of_toolchanges > 0);
    const size_t support_layers = print.objects().front()->support_layer_count();

    // Same data, same position: the same tower, and the plate's own tower is left as it was.
    Access::Tower again;
    Access::make(print, *again.data, nullptr, m.tower);
    CHECK(signature(*again.data) == before);
    CHECK(signature(print.wipe_tower_data()) == before);
    CHECK(fake_signature(print.get_fake_wipe_tower()) == fake_before);
    CHECK(print.objects().front()->support_layer_count() == support_layers);

    // Another position passed in: the tower process() makes for a plate whose tower is configured there.
    const Vec2d   moved = m.tower + SHIFT;
    Access::Tower shifted;
    Access::make(print, *shifted.data, nullptr, moved);
    const std::string shifted_tower = signature(*shifted.data, false);
    const std::string shifted_fake  = fake_signature(print.get_fake_wipe_tower());
    CHECK(print.get_fake_wipe_tower().pos == Vec2f(float(moved.x()), float(moved.y())));

    Print print_moved;
    Model model_moved;
    process(print_moved, model_moved, machine_config(m, true, by_object, moved), m, objects);
    CHECK(signature(print_moved.wipe_tower_data(), false) == shifted_tower);
    CHECK(fake_signature(print_moved.get_fake_wipe_tower()) == shifted_fake);
    CHECK(fake_signature(print_moved.get_fake_wipe_tower()) != fake_before);

    // The generated-footprint backstop checks the position passed in, not the configured one.
    Access::Tower off_bed;
    CHECK_THROWS_AS(Access::make(print, *off_bed.data, nullptr, Vec2d(-20., m.tower.y())), Slic3r::SlicingError);
}

} // namespace

TEST_CASE("WipeTowerData: the plate tower generated into its own data matches process(), H2D", "[WipeTowerData]")
{
    check_plate_tower(H2D, false);
    check_plate_tower(H2D, true);
}

TEST_CASE("WipeTowerData: the plate tower generated into its own data matches process(), H2C", "[WipeTowerData]")
{
    check_plate_tower(H2C, false);
    check_plate_tower(H2C, true);
}

TEST_CASE("WipeTowerData: the plate tower generated into its own data matches process(), H3C", "[WipeTowerData]")
{
    check_plate_tower(H3C, false);
    check_plate_tower(H3C, true);
}

TEST_CASE("WipeTowerData: the plate tower generated into its own data matches process(), X1C", "[WipeTowerData]")
{
    check_plate_tower(X1C, false);
    check_plate_tower(X1C, true);
}

TEST_CASE("WipeTowerData: the plate tower generated into its own data matches process(), U1", "[WipeTowerData]")
{
    check_plate_tower(U1, false);
    check_plate_tower(U1, true);
}

TEST_CASE("WipeTowerData: the plate tower generated into its own data matches process(), A1", "[WipeTowerData]")
{
    check_plate_tower(A1, false);
    check_plate_tower(A1, true);
}

TEST_CASE("WipeTowerData: the position constructors place the tower where the plate's keys do", "[WipeTowerData]")
{
    for (const Machine *m : { &X1C, &U1 }) {
        INFO(m->printer);
        const DynamicPrintConfig dyn = machine_config(*m, true, false, m->tower);
        PrintConfig              config;
        config.apply(dyn, true);
        // Type 1.
        const WipeTower by_plate(config, 0, Vec3d::Zero(), 15.f, 0, 1.f);
        const WipeTower by_pos(config, Vec2d(m->tower.x(), m->tower.y()), Vec3d::Zero(), 15.f, 0, 1.f);
        CHECK(by_plate.position() == by_pos.position());
        CHECK(by_pos.position() == Vec2f(float(m->tower.x()), float(m->tower.y())));
        const WipeTower moved(config, Vec2d(m->tower + SHIFT), Vec3d::Zero(), 15.f, 0, 1.f);
        CHECK(moved.position() == Vec2f(float(m->tower.x() + SHIFT.x()), float(m->tower.y() + SHIFT.y())));
        // Type 2.
        PrintRegionConfig                     region;
        const std::vector<std::vector<float>> flush(2, std::vector<float>(2, 0.f));
        const WipeTower2 by_plate2(config, region, 0, Vec3d::Zero(), flush, 0);
        const WipeTower2 by_pos2(config, region, Vec2d(m->tower.x(), m->tower.y()), Vec3d::Zero(), flush, 0);
        CHECK(by_plate2.position() == by_pos2.position());
        const WipeTower2 moved2(config, region, Vec2d(m->tower + SHIFT), Vec3d::Zero(), flush, 0);
        CHECK(moved2.position() == Vec2f(float(m->tower.x() + SHIFT.x()), float(m->tower.y() + SHIFT.y())));
    }
}

TEST_CASE("WipeTowerData: collect_layers_to_print merges a copy's tower-only layers and nothing else", "[WipeTowerData]")
{
    Print print;
    Model model;
    process(print, model, machine_config(X1C, false, true, X1C.tower), X1C, 1);
    PrintObject &object = *const_cast<PrintObject *>(print.objects().front());
    REQUIRE(object.layers().size() >= 3);

    const std::vector<GCode::LayerToPrint> plain = GCode::collect_layers_to_print(object);
    auto same = [](const std::vector<GCode::LayerToPrint> &a, const std::vector<GCode::LayerToPrint> &b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].object_layer != b[i].object_layer || a[i].support_layer != b[i].support_layer || a[i].original_object != b[i].original_object)
                return false;
        return true;
    };
    // The defaults are today's call.
    CHECK(same(plain, GCode::collect_layers_to_print(object, true, nullptr)));
    CHECK(same(plain, GCode::collect_layers_to_print(object, false, nullptr)));

    // A tower with no tower-only layers changes nothing.
    Access::Tower tower;
    CHECK(same(plain, GCode::collect_layers_to_print(object, true, tower.data.get())));

    // One tower-only layer on an object layer's print_z pairs with it; one above the object's top is a layer of its own.
    const size_t        support_before = object.support_layer_count();
    const Layer        *third          = object.layers()[2];
    const Layer        *top            = object.layers().back();
    const SupportLayer *at_third       = Access::new_virtual_layer(object, *tower.data, third->height, third->print_z);
    const SupportLayer *above_top      = Access::new_virtual_layer(object, *tower.data, top->height, top->print_z + top->height);
    const std::vector<GCode::LayerToPrint> merged = GCode::collect_layers_to_print(object, true, tower.data.get());
    REQUIRE(merged.size() == plain.size() + 1);
    for (size_t i = 0; i + 1 < merged.size(); ++i) {
        INFO("layer " << i);
        CHECK(merged[i].object_layer == plain[i].object_layer);
        CHECK(merged[i].print_z() <= merged[i + 1].print_z());
        if (merged[i].object_layer == third)
            CHECK(merged[i].support_layer == at_third);
        else
            CHECK(merged[i].support_layer == plain[i].support_layer);
    }
    CHECK(merged.back().object_layer == nullptr);
    CHECK(merged.back().support_layer == above_top);
    // The object itself is untouched, and the plain call still sees only its own layers.
    CHECK(object.support_layer_count() == support_before);
    CHECK(same(plain, GCode::collect_layers_to_print(object)));

    tower.data->clear();
    CHECK(tower.data->virtual_support_layers.empty());
    CHECK(same(plain, GCode::collect_layers_to_print(object, true, tower.data.get())));
}

TEST_CASE("WipeTowerData: a copy's tower keeps its raft gap layer to itself", "[WipeTowerData]")
{
    // One two-colour copy on a raft, printed by object with the plate tower off: nothing inserts tower-only layers into
    // the object. The copy's tower (inst given) plans from the copy's ordering in the by-object plan, which adds a tower
    // layer in the raft gap; that layer must stay in the tower's data.
    DynamicPrintConfig cfg = machine_config(X1C, false, true, X1C.tower);
    cfg.set_deserialize_strict({ { "raft_layers", 3 }, { "raft_contact_distance", 0.6 } });
    Print print;
    Model model;
    process(print, model, cfg, X1C, 1);
    REQUIRE_FALSE(print.has_wipe_tower());
    REQUIRE(print.wipe_tower_data().tool_changes.empty());
    REQUIRE(print.by_object_plan().blocks.size() == 1);

    PrintObject &object = *const_cast<PrintObject *>(print.objects().front());
    REQUIRE(object.support_layer_count() > 0);
    const size_t      support_before = object.support_layer_count();
    const std::string fake_before    = fake_signature(print.get_fake_wipe_tower());
    const std::vector<GCode::LayerToPrint> plain = GCode::collect_layers_to_print(object);
    const PrintInstance *inst = print.by_object_plan().blocks.front().instance;
    REQUIRE(inst == &object.instances().front());

    Access::Tower tower;
    Access::make(print, *tower.data, inst, X1C.tower);
    REQUIRE_FALSE(tower.data->tool_changes.empty());
    // The gap layer: above the raft, below the object.
    REQUIRE_FALSE(tower.data->virtual_support_layers.empty());
    coordf_t raft_top = 0.;
    for (const SupportLayer *layer : object.support_layers())
        if (layer->print_z < object.layers().front()->print_z)
            raft_top = std::max(raft_top, layer->print_z);
    for (const SupportLayer *layer : tower.data->virtual_support_layers) {
        CHECK(layer->print_z > raft_top);
        CHECK(layer->print_z < object.layers().front()->print_z);
        CHECK(layer->object() == &object);
    }
    // Neither the object nor the plate's tower state changed.
    CHECK(object.support_layer_count() == support_before);
    CHECK(print.wipe_tower_data().tool_changes.empty());
    CHECK(fake_signature(print.get_fake_wipe_tower()) == fake_before);
    // Only this copy's layers carry the gap layer.
    const std::vector<GCode::LayerToPrint> with_tower = GCode::collect_layers_to_print(object, true, tower.data.get());
    CHECK(with_tower.size() == plain.size() + tower.data->virtual_support_layers.size());
    size_t found = 0;
    for (const GCode::LayerToPrint &layer : with_tower)
        for (const SupportLayer *virt : tower.data->virtual_support_layers)
            if (layer.support_layer == virt)
                ++found;
    CHECK(found == tower.data->virtual_support_layers.size());
    CHECK(GCode::collect_layers_to_print(object).size() == plain.size());
}
