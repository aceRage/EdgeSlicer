#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace Slic3r;

// Print plate on the H2C reset a finished slice instead of opening the send dialog (2026-09-23).
//
// The Print button re-applies the current settings (Plater::apply_background_progress) before it acts.
// On a dual-nozzle Bambu printer the slice overwrites the Print's filament_map with the map its
// grouping computed (ToolOrdering::reorder_extruders_for_minimum_flush_volume). A hand-grouped plate
// hands its manual map to every apply. On the H2D (no nozzle rack) the grouping returns the manual
// map unchanged; on the H2C (six-nozzle rack) it re-picks the side of every filament the plate does
// not use, e.g. manual "2 2 2 2 2 2 2 2 2 1" came back "2 2 2 2 2 2 2 2 2 2" for a plate printing
// filaments 3 and 4 only. The re-apply then saw filament_map "change", invalidated every step and
// the plate needed slicing again, on every click.
//
// Print::apply now compares the incoming map with the map the slice was computed FROM.

namespace {

PresetBundle &bbl_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    if (bundle)
        return *bundle;
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("fmap_reapply_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    static PresetBundle library;
    library.load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent);
    bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, &library);
    set_data_dir(saved_data_dir);
    return *bundle;
}

struct Machine
{
    const char *printer;
    const char *process;
    const char *filament;
    Vec2d       bed_center;
    Vec2d       tower;
};

const Machine H2C{ "Bambu Lab H2C 0.4 nozzle", "0.20mm Standard @BBL H2C", "Bambu PLA Basic @BBL H2C", { 165., 160. }, { 40., 250. } };
const Machine H2D{ "Bambu Lab H2D 0.4 nozzle", "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D", { 165., 160. }, { 40., 250. } };

constexpr int NUM_FILAMENTS = 3;

// A hand-grouped plate: filaments 1 and 2 on the right extruder, the unused filament 3 on the left.
const std::vector<int> MANUAL_MAP{ 2, 2, 1 };

DynamicPrintConfig manual_plate_config(const Machine &m, const std::vector<int> &filament_map)
{
    PresetBundle &b = bbl_bundle();
    REQUIRE(b.printers.select_preset_by_name(m.printer, true));
    REQUIRE(b.prints.select_preset_by_name(m.process, true));
    REQUIRE(b.filaments.select_preset_by_name(m.filament, true));
    b.filament_presets = { m.filament };
    b.set_num_filaments(NUM_FILAMENTS, std::vector<std::string>{ "#E01919", "#1943E0", "#19E043" });
    b.filament_presets = std::vector<std::string>(NUM_FILAMENTS, m.filament);
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "enable_prime_tower", "1" },
        { "wipe_tower_x", m.tower.x() },
        { "wipe_tower_y", m.tower.y() },
        { "wipe_tower_rotation_angle", 0 },
        { "gcode_comments", 0 },
        { "filament_map_mode", "Manual" },
    });
    cfg.option<ConfigOptionInts>("filament_map", true)->values = filament_map;
    return cfg;
}

// Two cubes on filaments 1 and 2; filament 3 is loaded but unused.
void add_plate(Model &model, const Machine &m)
{
    for (int i = 0; i < 2; ++i) {
        TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.3f));
        ModelObject *object = model.add_object();
        object->name        = "cube" + std::to_string(i);
        object->add_volume(cube);
        object->volumes.front()->name = object->name;
        object->config.set("extruder", i + 1);
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(m.bed_center.x() + 25. * (i - 0.5), m.bed_center.y(), 0.));
        object->ensure_on_bed();
    }
}

// What the grouping writes back after a slice (ToolOrdering does the same two writes).
void write_grouping_result(Print &print, const std::vector<int> &computed)
{
    const_cast<PrintConfig &>(print.config()).filament_map.values = computed;
    const_cast<DynamicPrintConfig &>(print.full_print_config()).option<ConfigOptionInts>("filament_map", true)->values = computed;
}

bool invalidated(Print::ApplyStatus status) { return status == PrintBase::APPLY_STATUS_INVALIDATED; }

} // namespace

TEST_CASE("Keep the sliced filament map while its input is unchanged", "[FilamentMapReapply]")
{
    // H2C report: the rack grouping moved the unused filament 3 to the right extruder.
    const std::vector<int> h2c_computed{ 2, 2, 2 };
    CHECK(Print::keep_sliced_filament_map(MANUAL_MAP, MANUAL_MAP, h2c_computed));
    // H2D report: the grouping returned the manual map unchanged; there is nothing to keep.
    CHECK_FALSE(Print::keep_sliced_filament_map(MANUAL_MAP, MANUAL_MAP, MANUAL_MAP));
    // A new arrangement is a real change, whatever the previous result was.
    CHECK_FALSE(Print::keep_sliced_filament_map({ 1, 2, 1 }, MANUAL_MAP, h2c_computed));
    // Nothing applied yet: no input to compare with.
    CHECK_FALSE(Print::keep_sliced_filament_map(MANUAL_MAP, {}, h2c_computed));
}

SCENARIO("Re-applying the settings of a sliced hand-grouped plate keeps the slice", "[FilamentMapReapply]")
{
    for (const Machine *m : { &H2C, &H2D }) {
        GIVEN(std::string("a sliced ") + m->printer + " plate with a manual filament map")
        {
            const DynamicPrintConfig cfg = manual_plate_config(*m, MANUAL_MAP);
            Print print;
            Model model;
            add_plate(model, *m);
            print.is_BBL_printer() = true;
            print.apply(model, cfg);
            print.validate();
            const std::string gcode = Test::gcode(print);
            REQUIRE(!gcode.empty());
            REQUIRE(print.is_step_done(psGCodeExport));
            INFO("grouping result " << ConfigOptionInts(print.config().filament_map.values).serialize());
            // The used filaments stay where the plate put them.
            CHECK(print.config().filament_map.values[0] == 2);
            CHECK(print.config().filament_map.values[1] == 2);

            WHEN("the same settings are applied again (the Print button)")
            {
                const Print::ApplyStatus status = print.apply(model, cfg);
                THEN("the slice is kept")
                {
                    CHECK_FALSE(invalidated(status));
                    CHECK(print.is_step_done(psGCodeExport));
                    CHECK(print.last_apply_changed_keys().empty());
                }
            }
            WHEN("the grouping moved the unused filament to the other side (H2C rack) and the same settings are applied again")
            {
                write_grouping_result(print, { 2, 2, 2 });
                const Print::ApplyStatus status = print.apply(model, cfg);
                THEN("the slice and its computed map are kept, and the manual map stays the grouping input")
                {
                    CHECK_FALSE(invalidated(status));
                    CHECK(print.is_step_done(psGCodeExport));
                    CHECK(print.config().filament_map.values == std::vector<int>{ 2, 2, 2 });
                    CHECK(print.filament_map_input() == MANUAL_MAP);
                }
            }
            WHEN("the plate gets a different arrangement")
            {
                write_grouping_result(print, { 2, 2, 2 });
                const Print::ApplyStatus status = print.apply(model, manual_plate_config(*m, { 1, 2, 1 }));
                THEN("the slice is invalidated and the new map is the grouping input")
                {
                    CHECK(invalidated(status));
                    CHECK_FALSE(print.is_step_done(psGCodeExport));
                    CHECK(print.filament_map_input() == std::vector<int>{ 1, 2, 1 });
                    const auto &keys = print.last_apply_changed_keys();
                    CHECK(std::find(keys.begin(), keys.end(), "filament_map") != keys.end());
                }
            }
        }
    }
}
