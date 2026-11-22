// EdgeSlicer, mixed filament wall patterns: the too few walls notice, end to end through the wall step.
//
// A 20 x 20 x 3 mm block, optionally with a fin 1.2 mm wide and 16 mm long standing on it up to z 6. The walls use a
// mixed filament whose pattern has three comma groups, one per wall. Three classic 0.4 nozzle walls need about 2 mm,
// so the block holds them and the fin does not.
#include <catch2/catch.hpp>

#include <string>
#include <utility>
#include <vector>

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedPatternWalls.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using Catch::Matchers::Contains;

namespace {

const std::vector<std::string> kColours = { "#FF0000", "#00FF00" };

struct Row
{
    std::string  definition;
    unsigned int id{0};
};

Row mixed_row(const char *pattern)
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, kColours);
    mgr.mixed_filaments().front().manual_pattern = MixedFilamentManager::normalize_manual_pattern(pattern);
    return { mgr.serialize_custom_entries(), mgr.filament_id_from_mixed_index(0, kColours.size()) };
}

DynamicPrintConfig pattern_config(const Row &row, int wall_loops)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        { "nozzle_diameter",                "0.4,0.4" },
        { "filament_diameter",              "1.75,1.75" },
        { "enable_prime_tower",             "0" },
        { "enable_support",                 "0" },
        { "layer_height",                   "0.3" },
        { "initial_layer_print_height",     "0.3" },
        { "skirt_loops",                    "0" },
        { "brim_type",                      "no_brim" },
        { "wall_generator",                 "classic" },
        { "wall_loops",                     std::to_string(wall_loops) },
        { "single_extruder_multi_material", "1" },
        { "wall_filament",                  std::to_string(row.id) },
    });
    config.option<ConfigOptionStrings>("filament_colour")->values = kColours;
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(row.definition));
    return config;
}

TriangleMesh block(bool with_fin)
{
    TriangleMesh mesh = make_cube(20., 20., 3.);
    if (with_fin) {
        TriangleMesh fin = make_cube(1.2, 16., 3.1);
        fin.translate(9.4f, 2.f, 2.9f);
        mesh.merge(fin);
    }
    return mesh;
}

struct Sliced
{
    Print print;
    Model model;
    const PrintObject &object() const { return *print.objects().front(); }
    std::string notice() const
    {
        for (const PrintStateBase::Warning &w : object().step_state_with_warnings(posPerimeters).warnings)
            if (w.message_id == int(PrintStateBase::SlicingMixedPatternTooFewWalls))
                return w.message;
        return {};
    }
};

void slice(Sliced &s, TriangleMesh mesh, const DynamicPrintConfig &config)
{
    ModelObject *object = s.model.add_object();
    object->name        = "block";
    object->add_volume(std::move(mesh));
    object->add_instance();
    s.print.apply(s.model, config);
    s.print.set_status_silent();
    s.print.process();
}

} // namespace

TEST_CASE("Mixed pattern walls notice: enough walls on a plain block is quiet", "[MixedPatternWalls]")
{
    Sliced s;
    slice(s, block(false), pattern_config(mixed_row("12,21,1"), 3));
    CAPTURE(s.notice());
    CHECK(s.object().mixed_pattern_wall_shortfalls().empty());
    CHECK(s.notice().empty());
}

TEST_CASE("Mixed pattern walls notice: Wall loops below the pattern's groups", "[MixedPatternWalls]")
{
    Sliced s;
    const Row row = mixed_row("12,21,1");
    slice(s, block(false), pattern_config(row, 2));
    const std::vector<MixedPatternWallShortfall> &found = s.object().mixed_pattern_wall_shortfalls();
    REQUIRE(found.size() == 1);
    CHECK(found.front().filament_id == row.id);
    CHECK(found.front().walls_needed == 3);
    CHECK(found.front().walls_set == 2);
    CHECK_THAT(s.notice(), Contains("block: mixed filament"));
    CHECK_THAT(s.notice(), Contains("prints 2"));
}

TEST_CASE("Mixed pattern walls notice: a fin too thin for the third wall", "[MixedPatternWalls]")
{
    Sliced s;
    slice(s, block(true), pattern_config(mixed_row("12,21,1"), 3));
    const std::vector<MixedPatternWallShortfall> &found = s.object().mixed_pattern_wall_shortfalls();
    REQUIRE(found.size() == 1);
    CHECK(found.front().walls_set == 0);
    // The fin spans z 3 to 6 at 0.3 mm layers.
    CHECK(found.front().thin_layers >= 9);
    CHECK(found.front().thin_layers <= 11);
    CHECK(found.front().first_thin_z > 3.);
    CHECK(found.front().first_thin_z < 3.7);
    CHECK_THAT(s.notice(), Contains("thin areas"));
}

TEST_CASE("Mixed pattern walls notice: a pattern without wall groups is quiet", "[MixedPatternWalls]")
{
    Sliced s;
    slice(s, block(true), pattern_config(mixed_row("12"), 1));
    CHECK(s.object().mixed_pattern_wall_shortfalls().empty());
    CHECK(s.notice().empty());
}
