// Height colour pattern, phase 1 (tests/design_height_colour_pattern.md): the generator, the
// pattern record on ModelObject, and the EdgeSlicer-only 3MF sidecar.

#include <catch2/catch.hpp>

#include <cereal/archives/binary.hpp>

#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/HeightColourPattern.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

using namespace Slic3r;

namespace {

// A uniform layer grid: first layer `first`, then `step`, up to `height`.
std::vector<double> grid(double height, double first, double step)
{
    std::vector<double> tops;
    tops.push_back(first);
    for (int k = 1; tops.back() < height - 1e-9; ++k)
        tops.push_back(first + k * step);
    tops.back() = height;
    return tops;
}

HeightColourPattern example_pattern()
{
    // The owner's example: 10 mm from Z 2, skip 10 mm, three colours.
    HeightColourPattern p;
    p.start_z  = 2.;
    p.band_h   = 10.;
    p.gap_h    = 10.;
    p.repeat   = 3;
    p.sequence = { 1, 2, 3 };
    return p;
}

void check_range(const HeightPatternRange &r, double lo, double hi, int extruder)
{
    CHECK(r.lo == Approx(lo).margin(1e-6));
    CHECK(r.hi == Approx(hi).margin(1e-6));
    CHECK(r.extruder == extruder);
}

ModelObject *cube_object(Model &model, double height = 60.)
{
    ModelObject *object = model.add_object();
    object->name        = "tall";
    object->add_volume(make_cube(20., 20., height));
    object->add_instance();
    object->ensure_on_bed();
    return object;
}

std::string zip_entry(const std::string &zip_path, const std::string &entry_name)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return {};
    size_t      size = 0;
    void       *data = mz_zip_reader_extract_file_to_heap(&archive, entry_name.c_str(), &size, 0);
    std::string result;
    if (data != nullptr) {
        result.assign(static_cast<const char *>(data), size);
        mz_free(data);
    }
    close_zip_reader(&archive);
    return result;
}

std::string temp_file(const std::string &name)
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() /
        ("snorca_tests_heightpat_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    return (tmp_root / name).string();
}

DynamicPrintConfig project_config()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_num_extruders(2);
    cfg.set_num_filaments(3);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.4, 0.4 };
    cfg.option<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00", "#0000FF" };
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab P1S"));
    return cfg;
}

PlateDataPtrs one_plate()
{
    PlateData *a = new PlateData();
    a->plate_index = 0;
    a->objects_and_instances.emplace_back(0, 0);
    return { a };
}

bool store(const std::string &path, Model &model, bool bambu)
{
    DynamicPrintConfig cfg = project_config();
    BambuExport::Report report;
    StoreParams         sp;
    sp.path            = path.c_str();
    sp.model           = &model;
    sp.config          = &cfg;
    sp.plate_data_list = one_plate();
    sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    sp.bambu_compat    = bambu;
    sp.bambu_report    = bambu ? &report : nullptr;
    return store_bbs_3mf(sp);
}

struct Loaded
{
    bool               ok = false;
    Model              model;
    DynamicPrintConfig config;
    PlateDataPtrs      plates;
    std::vector<Preset *> presets;
    bool               is_bbl_3mf = false;
    Semver             version;
    ~Loaded()
    {
        release_PlateData_list(plates);
        for (Preset *p : presets) delete p;
    }
};

void load(const std::string &path, Loaded &out)
{
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence | LoadStrategy::LoadConfig;
    out.ok = load_bbs_3mf(path.c_str(), &out.config, &ctxt, &out.model, &out.plates, &out.presets, &out.is_bbl_3mf, &out.version,
                          nullptr, strategy);
}

} // namespace

TEST_CASE("Height colour pattern: the owner's example expands to three bands", "[HeightColourPattern][Expansion]")
{
    const HeightPatternExpansion e = expand_height_colour_pattern(example_pattern(), grid(60., 0.2, 0.2), 0.2);
    REQUIRE(e.ok());
    // Gap = the object's own filament: no gap ranges at all.
    REQUIRE(e.ranges.size() == 3);
    CHECK(e.bands == 3);
    CHECK_FALSE(e.clipped_at_top);
    check_range(e.ranges[0], 2., 12., 1);
    check_range(e.ranges[1], 22., 32., 2);
    check_range(e.ranges[2], 42., 52., 3);
    for (const HeightPatternRange &r : e.ranges)
        CHECK(r.layer_height == Approx(0.2)); // Slicing.cpp dereferences it for every range
}

TEST_CASE("Height colour pattern: edges snap to the object's real layer grid", "[HeightColourPattern][Expansion]")
{
    // A 0.25 first layer shifts every boundary by 0.05: 2.05, 12.05 ... are the grid points nearest 2 and 12.
    const HeightPatternExpansion e = expand_height_colour_pattern(example_pattern(), grid(60., 0.25, 0.2), 0.2);
    REQUIRE(e.ok());
    REQUIRE(e.ranges.size() == 3);
    check_range(e.ranges[0], 2.05, 12.05, 1);
    check_range(e.ranges[1], 22.05, 32.05, 2);

    SECTION("a boundary exactly between two layers goes up") {
        HeightColourPattern p = example_pattern();
        p.start_z = 2.1; // halfway between 2.0 and 2.2
        const HeightPatternExpansion up = expand_height_colour_pattern(p, grid(60., 0.2, 0.2), 0.2);
        REQUIRE(up.ranges.size() >= 1);
        CHECK(up.ranges[0].lo == Approx(2.2).margin(1e-6));
    }
    SECTION("snapping off keeps the typed values") {
        HeightColourPattern p = example_pattern();
        p.start_z = 2.07;
        p.snap_to_layers = false;
        const HeightPatternExpansion raw = expand_height_colour_pattern(p, grid(60., 0.2, 0.2), 0.2);
        REQUIRE(raw.ranges.size() >= 1);
        CHECK(raw.ranges[0].lo == Approx(2.07).margin(1e-6));
        CHECK(raw.ranges[0].hi == Approx(12.07).margin(1e-6));
    }
    SECTION("with no gap, the next band starts exactly where the last one ended") {
        HeightColourPattern p = example_pattern();
        p.gap_h = 0.;
        const HeightPatternExpansion e0 = expand_height_colour_pattern(p, grid(60., 0.25, 0.2), 0.2);
        REQUIRE(e0.ranges.size() == 3);
        CHECK(e0.ranges[0].hi == e0.ranges[1].lo);
        CHECK(e0.ranges[1].hi == e0.ranges[2].lo);
    }
}

TEST_CASE("Height colour pattern: repeat, to-the-top and clipping", "[HeightColourPattern][Expansion]")
{
    HeightColourPattern p = example_pattern();

    SECTION("repeat 0 runs to the top of the object") {
        p.repeat = 0;
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(60., 0.2, 0.2), 0.2);
        // Starts at 2, 22, 42; the next would start at 62, beyond the top.
        CHECK(e.bands == 3);
        CHECK_FALSE(e.clipped_at_top);
    }
    SECTION("the last band is clipped to the object top") {
        p.repeat = 0;
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(48., 0.2, 0.2), 0.2);
        REQUIRE(e.bands == 3);
        CHECK(e.clipped_at_top);
        CHECK(e.ranges.back().lo == Approx(42.).margin(1e-6));
        CHECK(e.ranges.back().hi == Approx(48.).margin(1e-6));
    }
    SECTION("bands that start above the top are dropped") {
        p.repeat = 10;
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(30., 0.2, 0.2), 0.2);
        CHECK(e.bands == 2);
    }
    SECTION("a one-colour sequence is allowed") {
        p.sequence = { 2 };
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(60., 0.2, 0.2), 0.2);
        REQUIRE(e.ranges.size() == 3);
        for (const HeightPatternRange &r : e.ranges)
            CHECK(r.extruder == 2);
    }
    SECTION("the sequence cycles") {
        p.sequence = { 1, 2 };
        p.repeat   = 3;
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(60., 0.2, 0.2), 0.2);
        REQUIRE(e.ranges.size() == 3);
        CHECK(e.ranges[0].extruder == 1);
        CHECK(e.ranges[1].extruder == 2);
        CHECK(e.ranges[2].extruder == 1);
    }
    SECTION("a band thinner than half a layer collapses and is counted") {
        p.band_h = 0.05;
        p.gap_h  = 0.15;
        p.repeat = 3;
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(60., 0.2, 0.2), 0.2);
        CHECK(e.ok());
        CHECK(e.ranges.empty());
        CHECK(e.dropped == 3);
    }
}

TEST_CASE("Height colour pattern: every gap mode", "[HeightColourPattern][Expansion]")
{
    HeightColourPattern p = example_pattern();

    SECTION("the object's own filament writes no gap ranges") {
        p.gap_mode = int(HeightPatternGapMode::ObjectFilament);
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(60., 0.2, 0.2), 0.2);
        CHECK(e.ranges.size() == 3);
    }
    SECTION("a fixed filament fills every gap between bands") {
        p.gap_mode     = int(HeightPatternGapMode::Fixed);
        p.gap_filament = 4;
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(60., 0.2, 0.2), 0.2);
        REQUIRE(e.ranges.size() == 5); // B G B G B: nothing before the first band or after the last
        CHECK(e.bands == 3);
        check_range(e.ranges[0], 2., 12., 1);
        check_range(e.ranges[1], 12., 22., 4);
        check_range(e.ranges[2], 22., 32., 2);
        check_range(e.ranges[3], 32., 42., 4);
        check_range(e.ranges[4], 42., 52., 3);
    }
    SECTION("next in sequence: bands and gaps share one cursor") {
        p.gap_mode = int(HeightPatternGapMode::NextInSequence);
        p.sequence = { 1, 2, 3, 4 };
        p.repeat   = 3;
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(60., 0.2, 0.2), 0.2);
        REQUIRE(e.ranges.size() == 5);
        // band 0 = slot 0 (F1), its gap = slot 1 (F2), band 1 = slot 2 (F3), its gap = slot 3 (F4), band 2 = slot 4 (F1)
        CHECK(e.ranges[0].extruder == 1);
        CHECK(e.ranges[1].extruder == 2);
        CHECK(e.ranges[2].extruder == 3);
        CHECK(e.ranges[3].extruder == 4);
        CHECK(e.ranges[4].extruder == 1);
    }
    SECTION("next in sequence with two filaments gives plain stripes") {
        p.gap_mode = int(HeightPatternGapMode::NextInSequence);
        p.sequence = { 1, 2 };
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(60., 0.2, 0.2), 0.2);
        REQUIRE(e.ranges.size() == 5);
        for (size_t i = 0; i < e.ranges.size(); ++i)
            CHECK(e.ranges[i].extruder == (i % 2 == 0 ? 1 : 2));
    }
    SECTION("no gap in the geometry means no gap range") {
        p.gap_h        = 0.;
        p.gap_mode     = int(HeightPatternGapMode::Fixed);
        p.gap_filament = 4;
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(60., 0.2, 0.2), 0.2);
        CHECK(e.ranges.size() == 3);
    }
}

TEST_CASE("Height colour pattern: bad input produces an error, not ranges", "[HeightColourPattern][Expansion]")
{
    const std::vector<double> g = grid(60., 0.2, 0.2);
    HeightColourPattern p = example_pattern();

    p.sequence.clear();
    CHECK_FALSE(expand_height_colour_pattern(p, g, 0.2).ok());
    p = example_pattern();
    p.sequence = { 1, 0 };
    CHECK_FALSE(expand_height_colour_pattern(p, g, 0.2).ok());
    p = example_pattern();
    p.band_h = 0.;
    CHECK_FALSE(expand_height_colour_pattern(p, g, 0.2).ok());
    p = example_pattern();
    p.gap_h = -1.;
    CHECK_FALSE(expand_height_colour_pattern(p, g, 0.2).ok());
    p = example_pattern();
    p.gap_mode = int(HeightPatternGapMode::Fixed);
    p.gap_filament = 0;
    CHECK_FALSE(expand_height_colour_pattern(p, g, 0.2).ok());
    p = example_pattern();
    CHECK_FALSE(expand_height_colour_pattern(p, g, 0.).ok());
    CHECK_FALSE(expand_height_colour_pattern(p, {}, 0.2).ok()); // no grid and no height
    // No grid but a height: nothing to snap to, the typed values are used.
    const HeightPatternExpansion ungridded = expand_height_colour_pattern(p, {}, 0.2, 60.);
    REQUIRE(ungridded.ok());
    CHECK(ungridded.bands == 3);
}

TEST_CASE("Height colour pattern: layer tops come from the interleaved layer list", "[HeightColourPattern][Expansion]")
{
    const std::vector<double> layers = { 0., 0.3, 0.3, 0.5, 0.5, 0.7 };
    const std::vector<double> tops   = height_pattern_layer_tops(layers);
    REQUIRE(tops.size() == 3);
    CHECK(tops[0] == Approx(0.3));
    CHECK(tops[2] == Approx(0.7));
}

TEST_CASE("Height colour pattern: applying writes ordinary ranges and a record", "[HeightColourPattern][Apply]")
{
    Model        model;
    ModelObject *object = cube_object(model);
    const auto   tops   = grid(60., 0.2, 0.2);

    const HeightPatternApplyResult r = apply_height_colour_pattern(*object, example_pattern(), tops, 0.2, false);
    REQUIRE(r.ok);
    REQUIRE(object->layer_config_ranges.size() == 3);
    for (const auto &kv : object->layer_config_ranges) {
        REQUIRE(kv.second.get().has("layer_height"));
        REQUIRE(kv.second.get().has("extruder"));
        CHECK(kv.second.get().opt_float("layer_height") == Approx(0.2));
    }
    CHECK(object->layer_config_ranges.begin()->second.get().opt_int("extruder") == 1);
    REQUIRE(object->height_colour_patterns.size() == 1);
    CHECK(object->height_colour_patterns[0].id == 1);
    CHECK(object->height_colour_patterns[0].generated.size() == 3);
    CHECK_FALSE(height_colour_pattern_state(*object, object->height_colour_patterns[0]).modified);

    SECTION("a second pattern gets the next id") {
        HeightColourPattern q = example_pattern();
        q.start_z = 54.;
        q.band_h  = 4.;
        q.repeat  = 1;
        const HeightPatternApplyResult r2 = apply_height_colour_pattern(*object, q, tops, 0.2, false);
        REQUIRE(r2.ok);
        CHECK(r2.pattern_id == 2);
        CHECK(object->layer_config_ranges.size() == 4);
    }
    SECTION("a pattern that does not fit says so and changes nothing") {
        HeightColourPattern q = example_pattern();
        q.start_z = 100.;
        const HeightPatternApplyResult r2 = apply_height_colour_pattern(*object, q, tops, 0.2, false);
        CHECK_FALSE(r2.ok);
        CHECK(object->layer_config_ranges.size() == 3);
        CHECK(object->height_colour_patterns.size() == 1);
    }
}

TEST_CASE("Height colour pattern: overlap with hand-made ranges", "[HeightColourPattern][Apply]")
{
    Model        model;
    ModelObject *object = cube_object(model);
    const auto   tops   = grid(60., 0.2, 0.2);

    DynamicPrintConfig hand;
    hand.set_key_value("layer_height", new ConfigOptionFloat(0.1));
    hand.set_key_value("extruder", new ConfigOptionInt(2));
    object->layer_config_ranges[{ 5., 8. }].assign_config(hand);     // inside band 1 (2-12)
    object->layer_config_ranges[{ 55., 58. }].assign_config(hand);   // beyond the pattern (last band ends at 52)
    object->layer_config_ranges[{ 0., 1. }].assign_config(hand);     // before it

    SECTION("without permission nothing changes and the clash is listed") {
        const HeightPatternApplyResult r = apply_height_colour_pattern(*object, example_pattern(), tops, 0.2, false);
        CHECK_FALSE(r.ok);
        REQUIRE(r.clashes.size() == 1);
        CHECK(r.clashes[0].first == Approx(5.));
        CHECK(r.clashes[0].second == Approx(8.));
        CHECK(object->layer_config_ranges.size() == 3);
        CHECK(object->height_colour_patterns.empty());
    }
    SECTION("replace removes the clashing range and keeps the ones outside the span") {
        const HeightPatternApplyResult r = apply_height_colour_pattern(*object, example_pattern(), tops, 0.2, true);
        REQUIRE(r.ok);
        CHECK(r.clashes.size() == 1);
        CHECK(object->layer_config_ranges.size() == 5); // 3 bands + {0,1} + {55,58}
        CHECK(object->layer_config_ranges.count({ 5., 8. }) == 0);
        CHECK(object->layer_config_ranges.count({ 55., 58. }) == 1);
        CHECK(object->layer_config_ranges.count({ 0., 1. }) == 1);
    }
    SECTION("touching edges are not a clash") {
        object->layer_config_ranges.erase(t_layer_height_range{ 5., 8. });
        object->layer_config_ranges[{ 12., 22. }].assign_config(hand); // exactly the gap
        const HeightPatternApplyResult r = apply_height_colour_pattern(*object, example_pattern(), tops, 0.2, false);
        CHECK(r.ok);
    }
}

TEST_CASE("Height colour pattern: regenerate and remove act on the pattern as one unit", "[HeightColourPattern][Apply]")
{
    Model        model;
    ModelObject *object = cube_object(model);
    const auto   tops   = grid(60., 0.2, 0.2);

    DynamicPrintConfig hand;
    hand.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    hand.set_key_value("extruder", new ConfigOptionInt(2));
    object->layer_config_ranges[{ 56., 58. }].assign_config(hand);

    REQUIRE(apply_height_colour_pattern(*object, example_pattern(), tops, 0.2, false).ok);
    REQUIRE(object->layer_config_ranges.size() == 4);

    SECTION("regenerating with the same id replaces the old bands and never clashes with them") {
        HeightColourPattern again = object->height_colour_patterns[0];
        again.band_h = 5.;
        again.repeat = 2;
        const HeightPatternApplyResult r = apply_height_colour_pattern(*object, again, tops, 0.2, false);
        REQUIRE(r.ok);
        CHECK(r.pattern_id == 1);
        CHECK(object->height_colour_patterns.size() == 1);
        CHECK(object->layer_config_ranges.size() == 3); // 2 bands + the hand-made one
        CHECK(object->layer_config_ranges.count({ 2., 7. }) == 1);
        CHECK(object->layer_config_ranges.count({ 22., 27. }) == 1);
        CHECK(object->layer_config_ranges.count({ 2., 12. }) == 0);
    }
    SECTION("remove erases exactly the owned ranges") {
        REQUIRE(remove_height_colour_pattern(*object, 1));
        CHECK(object->height_colour_patterns.empty());
        REQUIRE(object->layer_config_ranges.size() == 1);
        CHECK(object->layer_config_ranges.count({ 56., 58. }) == 1);
        CHECK_FALSE(remove_height_colour_pattern(*object, 1));
    }
    SECTION("detach keeps the ranges and forgets the record") {
        REQUIRE(detach_height_colour_pattern(*object, 1));
        CHECK(object->height_colour_patterns.empty());
        CHECK(object->layer_config_ranges.size() == 4);
    }
}

TEST_CASE("Height colour pattern: a hand-edited generated range makes the pattern modified", "[HeightColourPattern][Modified]")
{
    Model        model;
    ModelObject *object = cube_object(model);
    REQUIRE(apply_height_colour_pattern(*object, example_pattern(), grid(60., 0.2, 0.2), 0.2, false).ok);
    const HeightColourPattern pattern = object->height_colour_patterns[0];
    REQUIRE_FALSE(height_colour_pattern_state(*object, pattern).modified);

    SECTION("a changed filament") {
        object->layer_config_ranges[{ 22., 32. }].set_key_value("extruder", new ConfigOptionInt(3));
        const HeightPatternState s = height_colour_pattern_state(*object, pattern);
        CHECK(s.modified);
        REQUIRE(s.changed.size() == 1);
        CHECK(s.changed[0].first == Approx(22.));
    }
    SECTION("a changed layer height") {
        object->layer_config_ranges[{ 2., 12. }].set_key_value("layer_height", new ConfigOptionFloat(0.1));
        CHECK(height_colour_pattern_state(*object, pattern).modified);
    }
    SECTION("an extra setting on a band") {
        object->layer_config_ranges[{ 42., 52. }].set_key_value("wall_loops", new ConfigOptionInt(6));
        CHECK(height_colour_pattern_state(*object, pattern).modified);
    }
    SECTION("moved bounds (the old key is gone)") {
        ModelConfig moved = object->layer_config_ranges[{ 22., 32. }];
        object->layer_config_ranges.erase(t_layer_height_range{ 22., 32. });
        object->layer_config_ranges[{ 22., 33. }] = moved;
        const HeightPatternState s = height_colour_pattern_state(*object, pattern);
        CHECK(s.modified);
        CHECK(s.changed.size() == 1);
    }
    SECTION("a deleted range") {
        object->layer_config_ranges.erase(t_layer_height_range{ 42., 52. });
        CHECK(height_colour_pattern_state(*object, pattern).modified);
    }
    SECTION("an unrelated hand-made range does not count") {
        DynamicPrintConfig hand;
        hand.set_key_value("layer_height", new ConfigOptionFloat(0.2));
        hand.set_key_value("extruder", new ConfigOptionInt(2));
        object->layer_config_ranges[{ 55., 58. }].assign_config(hand);
        CHECK_FALSE(height_colour_pattern_state(*object, pattern).modified);
    }
}

TEST_CASE("Height colour pattern: the record survives copies and undo serialisation", "[HeightColourPattern][Copy]")
{
    Model        model;
    ModelObject *object = cube_object(model);
    HeightColourPattern p = example_pattern();
    p.gap_mode     = int(HeightPatternGapMode::Fixed);
    p.gap_filament = 4;
    REQUIRE(apply_height_colour_pattern(*object, p, grid(60., 0.2, 0.2), 0.2, false).ok);

    SECTION("a copied object carries the record and the ranges") {
        ModelObject *copy = model.add_object(*object);
        REQUIRE(copy->height_colour_patterns.size() == 1);
        CHECK(copy->height_colour_patterns[0].generated.size() == 5);
        CHECK(copy->layer_config_ranges.size() == 5);
        CHECK_FALSE(height_colour_pattern_state(*copy, copy->height_colour_patterns[0]).modified);
    }
    SECTION("cereal (the undo/redo archive) round-trips every field") {
        std::stringstream ss;
        {
            cereal::BinaryOutputArchive oa(ss);
            oa(object->height_colour_patterns);
        }
        std::vector<HeightColourPattern> back;
        {
            cereal::BinaryInputArchive ia(ss);
            ia(back);
        }
        REQUIRE(back.size() == 1);
        const HeightColourPattern &a = object->height_colour_patterns[0];
        const HeightColourPattern &b = back[0];
        CHECK(b.id == a.id);
        CHECK(b.start_z == a.start_z);
        CHECK(b.band_h == a.band_h);
        CHECK(b.gap_h == a.gap_h);
        CHECK(b.repeat == a.repeat);
        CHECK(b.sequence == a.sequence);
        CHECK(b.gap_mode == a.gap_mode);
        CHECK(b.gap_filament == a.gap_filament);
        CHECK(b.snap_to_layers == a.snap_to_layers);
        REQUIRE(b.generated.size() == a.generated.size());
        for (size_t i = 0; i < b.generated.size(); ++i) {
            CHECK(b.generated[i].lo == a.generated[i].lo);
            CHECK(b.generated[i].hi == a.generated[i].hi);
            CHECK(b.generated[i].extruder == a.generated[i].extruder);
            CHECK(b.generated[i].layer_height == a.generated[i].layer_height);
        }
    }
}

TEST_CASE("Height colour pattern: the sidecar JSON round-trips and rejects garbage", "[HeightColourPattern][Json]")
{
    Model        model;
    ModelObject *with    = cube_object(model);
    ModelObject *without = cube_object(model);
    ModelObject *with2   = cube_object(model);
    (void) without;
    REQUIRE(apply_height_colour_pattern(*with, example_pattern(), grid(60., 0.2, 0.2), 0.2, false).ok);
    REQUIRE(apply_height_colour_pattern(*with2, example_pattern(), grid(60., 0.2, 0.2), 0.2, false).ok);

    std::vector<const std::vector<HeightColourPattern> *> per_object;
    for (const ModelObject *o : model.objects)
        per_object.push_back(&o->height_colour_patterns);
    const std::string json = height_patterns_to_json(per_object);

    const auto back = height_patterns_from_json(json);
    REQUIRE(back.size() == 2);
    CHECK(back[0].first == 1);
    CHECK(back[1].first == 3); // the object without patterns is skipped, indices stay model-order
    REQUIRE(back[0].second.size() == 1);
    CHECK(back[0].second[0].generated.size() == 3);
    CHECK(back[0].second[0].sequence == std::vector<int>({ 1, 2, 3 }));

    CHECK(height_patterns_from_json("").empty());
    CHECK(height_patterns_from_json("not json").empty());
    CHECK(height_patterns_from_json("{\"version\":2,\"objects\":[]}").empty());
    CHECK(height_patterns_from_json("{\"version\":1,\"objects\":[{\"object\":1,\"patterns\":[{\"id\":1}]}]}").empty());
    CHECK(height_patterns_from_json("{\"version\":1,\"objects\":[{\"object\":1,\"patterns\":[{\"id\":1,\"sequence\":[1],"
                                    "\"gap_mode\":9,\"generated\":[{\"lo\":1,\"hi\":2,\"extruder\":1,\"layer_height\":0.2}]}]}]}").empty());
}

TEST_CASE("Height colour pattern: native 3MF round trip", "[HeightColourPattern][3mf]")
{
    Model        model;
    ModelObject *object = cube_object(model);
    HeightColourPattern p = example_pattern();
    p.gap_mode     = int(HeightPatternGapMode::Fixed);
    p.gap_filament = 2;
    REQUIRE(apply_height_colour_pattern(*object, p, grid(60., 0.2, 0.2), 0.2, false).ok);

    const std::string path = temp_file("heightpat_native.3mf");
    REQUIRE(store(path, model, false));
    CHECK_FALSE(zip_entry(path, "Metadata/edgeslicer_height_patterns.json").empty());
    CHECK(zip_entry(path, "Metadata/layer_config_ranges.xml").find("opt_key=\"extruder\"") != std::string::npos);

    Loaded back;
    load(path, back);
    REQUIRE(back.ok);
    REQUIRE(back.model.objects.size() == 1);
    const ModelObject *loaded = back.model.objects[0];
    REQUIRE(loaded->layer_config_ranges.size() == object->layer_config_ranges.size());
    auto a = object->layer_config_ranges.begin();
    auto b = loaded->layer_config_ranges.begin();
    for (; a != object->layer_config_ranges.end(); ++a, ++b) {
        CHECK(b->first.first == Approx(a->first.first).margin(1e-6));
        CHECK(b->first.second == Approx(a->first.second).margin(1e-6));
        CHECK(b->second.get().opt_int("extruder") == a->second.get().opt_int("extruder"));
        CHECK(b->second.get().opt_float("layer_height") == Approx(a->second.get().opt_float("layer_height")));
    }
    REQUIRE(loaded->height_colour_patterns.size() == 1);
    const HeightColourPattern &lp = loaded->height_colour_patterns[0];
    CHECK(lp.id == 1);
    CHECK(lp.sequence == p.sequence);
    CHECK(lp.gap_mode == p.gap_mode);
    CHECK(lp.gap_filament == 2);
    CHECK(lp.generated.size() == 5);
    // Still recognised as the pattern's own output after the round trip (bounds written as text).
    CHECK_FALSE(height_colour_pattern_state(*loaded, lp).modified);
    boost::filesystem::remove(path);
}

TEST_CASE("Height colour pattern: no pattern, no sidecar; old projects load unchanged", "[HeightColourPattern][3mf]")
{
    Model        model;
    ModelObject *object = cube_object(model);
    DynamicPrintConfig hand;
    hand.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    hand.set_key_value("extruder", new ConfigOptionInt(2));
    object->layer_config_ranges[{ 5., 8. }].assign_config(hand);

    const std::string path = temp_file("heightpat_none.3mf");
    REQUIRE(store(path, model, false));
    CHECK(zip_entry(path, "Metadata/edgeslicer_height_patterns.json").empty());

    Loaded back;
    load(path, back);
    REQUIRE(back.ok);
    REQUIRE(back.model.objects.size() == 1);
    CHECK(back.model.objects[0]->height_colour_patterns.empty());
    CHECK(back.model.objects[0]->layer_config_ranges.size() == 1);
    boost::filesystem::remove(path);
}

TEST_CASE("Height colour pattern: Bambu export keeps plain ranges with the extruder key", "[HeightColourPattern][3mf][BambuExport]")
{
    Model        model;
    ModelObject *object = cube_object(model);
    REQUIRE(apply_height_colour_pattern(*object, example_pattern(), grid(60., 0.2, 0.2), 0.2, false).ok);

    const std::string path = temp_file("heightpat_bambu.3mf");
    REQUIRE(store(path, model, true));

    // EdgeSlicer-only: Bambu Studio has nothing to do with the sidecar.
    CHECK(zip_entry(path, "Metadata/edgeslicer_height_patterns.json").empty());

    const std::string ranges = zip_entry(path, "Metadata/layer_config_ranges.xml");
    REQUIRE_FALSE(ranges.empty());
    // The bands survive as ranges, filament included (the design flagged BambuExport::convert).
    CHECK(ranges.find("opt_key=\"extruder\"") != std::string::npos);
    CHECK(ranges.find("opt_key=\"layer_height\"") != std::string::npos);
    size_t bands = 0;
    for (size_t pos = ranges.find("<range"); pos != std::string::npos; pos = ranges.find("<range", pos + 1))
        ++bands;
    CHECK(bands == 3);
    // The three filament ids are all still there.
    for (const char *id : { ">1</option>", ">2</option>", ">3</option>" })
        CHECK(ranges.find(id) != std::string::npos);

    // And our loader reads the Bambu file back with the three ranges and their filaments.
    Loaded back;
    load(path, back);
    REQUIRE(back.ok);
    REQUIRE(back.model.objects.size() == 1);
    const auto &loaded = back.model.objects[0]->layer_config_ranges;
    REQUIRE(loaded.size() == 3);
    std::vector<int> extruders;
    for (const auto &kv : loaded)
        extruders.push_back(kv.second.get().opt_int("extruder"));
    CHECK(extruders == std::vector<int>({ 1, 2, 3 }));
    CHECK(back.model.objects[0]->height_colour_patterns.empty());
    boost::filesystem::remove(path);
}
