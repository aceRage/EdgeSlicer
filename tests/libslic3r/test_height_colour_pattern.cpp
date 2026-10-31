// Height colour pattern, phase 1 (tests/design_height_colour_pattern.md): the generator, the
// pattern record on ModelObject, and the EdgeSlicer-only 3MF sidecar.

#include <catch2/catch.hpp>

#include <cereal/archives/binary.hpp>

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/HeightColourPattern.hpp"
#include "libslic3r/HeightPatternInput.hpp"
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
    p.repeat   = 1;   // the whole 3-filament sequence once: 3 bands
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

// The U1-style project the owner opens: `filaments` tools, one distinct colour and preset name each.
DynamicPrintConfig sample_project_config(size_t filaments)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    // Up to three filaments keep the two-extruder P1S-style layout the Bambu export tests use; four
    // or more is the U1 (one filament per tool).
    const bool u1 = filaments > 3;
    const size_t extruders = u1 ? filaments : 2;
    cfg.set_num_extruders(unsigned(extruders));
    cfg.set_num_filaments(unsigned(filaments));
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values = std::vector<double>(extruders, 0.4);
    const char *palette[] = { "#E53935", "#1E88E5", "#43A047", "#FDD835", "#8E24AA", "#FB8C00" };
    std::vector<std::string> colours, ids, types;
    for (size_t i = 0; i < filaments; ++i) {
        colours.push_back(palette[i % 6]);
        ids.push_back("Generic PLA");
        types.push_back("PLA");
    }
    cfg.option<ConfigOptionStrings>("filament_colour")->values       = colours;
    cfg.option<ConfigOptionStrings>("filament_settings_id", true)->values  = ids;
    cfg.option<ConfigOptionStrings>("filament_type", true)->values         = types;
    // A real saved project slices: relative extrusion needs the E reset at each layer.
    cfg.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0"));
    cfg.set_key_value("printer_model", new ConfigOptionString(u1 ? "Snapmaker U1" : "Bambu Lab P1S"));
    if (u1)
        cfg.set_key_value("printer_settings_id", new ConfigOptionString("Snapmaker U1 (0.4 nozzle)"));
    return cfg;
}

DynamicPrintConfig project_config() { return sample_project_config(3); }

PlateDataPtrs one_plate()
{
    PlateData *a = new PlateData();
    a->plate_index = 0;
    a->objects_and_instances.emplace_back(0, 0);
    return { a };
}

bool store(const std::string &path, Model &model, bool bambu, DynamicPrintConfig cfg = project_config())
{
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
        p.repeat   = 3;
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
    SECTION("repeat counts whole sequences: 5 filaments x 3 = 15 bands, in sequence order") {
        p.sequence = { 4, 2, 1, 3, 5 };
        p.start_z  = 0.;
        p.band_h   = 1.;
        p.gap_h    = 1.;
        p.repeat   = 3;
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(60., 0.2, 0.2), 0.2);
        REQUIRE(e.ok());
        REQUIRE(e.ranges.size() == 15);
        CHECK(e.bands == 15);
        CHECK(e.planned_bands == 15);
        CHECK(e.repeats == 3);
        CHECK(e.sequence_length == 5);
        CHECK_FALSE(e.cut_off);
        for (size_t i = 0; i < e.ranges.size(); ++i)
            CHECK(e.ranges[i].extruder == p.sequence[i % 5]);
        CHECK(height_pattern_planned_bands(3, 5, HeightPatternGapMode::ObjectFilament) == 15);
    }
    SECTION("the object top cuts the last repeat off after a named filament") {
        p.sequence = { 4, 2, 1, 3, 5 };
        p.start_z  = 0.;
        p.band_h   = 1.;
        p.gap_h    = 1.;
        p.repeat   = 3;
        // Bands start at 0, 2, 4, ...: a 17 mm object holds 9 of the 15 (the 9th starts at 16).
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(17., 0.2, 0.2), 0.2);
        REQUIRE(e.ok());
        CHECK(e.bands == 9);
        CHECK(e.planned_bands == 15);
        CHECK(e.cut_off);
        CHECK(e.last_filament == 3);        // 9th band = 4th entry of the 2nd repeat
        CHECK(e.last_sequence_index == 4);
        CHECK(e.ranges[8].extruder == 3);
        const std::string s = height_pattern_short_summary(e);
        CHECK(s.find("last repeat cut off at the object top after filament 3") != std::string::npos);
        CHECK(s.find("3 repeats of 5 filaments") != std::string::npos);
    }
    SECTION("a clipped last band of a complete repeat is not a cut-off repeat") {
        p.sequence = { 1, 2 };
        p.start_z  = 0.;
        p.band_h   = 5.;
        p.gap_h    = 0.;
        p.repeat   = 1;
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(8., 0.2, 0.2), 0.2);
        REQUIRE(e.bands == 2);
        CHECK(e.clipped_at_top);
        CHECK_FALSE(e.cut_off);
    }
    SECTION("a one-filament sequence repeated n times is n bands") {
        p.sequence = { 3 };
        p.repeat   = 4;
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(100., 0.2, 0.2), 0.2);
        CHECK(e.bands == 4);
        CHECK(e.planned_bands == 4);
        CHECK_FALSE(e.cut_off);
    }
    SECTION("next in sequence: a repeat is one pass over the sequence, bands are every other slot") {
        p.gap_mode = int(HeightPatternGapMode::NextInSequence);
        p.sequence = { 1, 2, 3, 4, 5 };
        p.repeat   = 1;
        const HeightPatternExpansion e = expand_height_colour_pattern(p, grid(100., 0.2, 0.2), 0.2);
        // slots F1..F5 = band F1, gap F2, band F3, gap F4, band F5
        REQUIRE(e.ranges.size() == 5);
        CHECK(e.bands == 3);
        CHECK(e.ranges[4].extruder == 5);
        CHECK(height_pattern_planned_bands(2, 5, HeightPatternGapMode::NextInSequence) == 5);
    }
    SECTION("a band thinner than half a layer collapses and is counted") {
        p.band_h = 0.05;
        p.gap_h  = 0.15;
        p.repeat = 1;
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
        p.repeat   = 3;
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
        q.sequence = { 1 };
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
        again.repeat = 1;
        again.sequence = { 1, 2 };
        const HeightPatternApplyResult r = apply_height_colour_pattern(*object, again, tops, 0.2, false);
        REQUIRE(r.ok);
        CHECK(r.pattern_id == 1);
        CHECK(object->height_colour_patterns.size() == 1);
        CHECK(object->layer_config_ranges.size() == 3); // 2 bands + the hand-made one
        CHECK(object->layer_config_ranges.count({ 2., 7. }) == 1);
        CHECK(object->layer_config_ranges.count({ 17., 22. }) == 1);
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

// Hidden: writes the owner's hand-check file. Run with HEIGHTPAT_SAMPLE=<path> libslic3r_tests "[.heightpat_sample]".
TEST_CASE("Height colour pattern: write the sample project", "[.heightpat_sample]")
{
    const char *out = std::getenv("HEIGHTPAT_SAMPLE");
    REQUIRE(out != nullptr);
    Model        model;
    ModelObject *object = cube_object(model, 80.);
    HeightColourPattern p = example_pattern();
    p.repeat   = 0; // to the top
    p.sequence = { 1, 2, 3, 4 };
    REQUIRE(apply_height_colour_pattern(*object, p, grid(80., 0.2, 0.2), 0.2, false, 0., 4).ok);
    REQUIRE(store(out, model, false, sample_project_config(4)));
}

TEST_CASE("Height colour pattern: a four-filament project loads with its filaments and the ranges reference them", "[HeightColourPattern][3mf]")
{
    Model        model;
    ModelObject *object = cube_object(model, 80.);
    HeightColourPattern p = example_pattern();
    p.repeat   = 0;
    p.sequence = { 1, 2, 3, 4 };
    REQUIRE(apply_height_colour_pattern(*object, p, grid(80., 0.2, 0.2), 0.2, false, 0., 4).ok);

    const std::string path = temp_file("heightpat_four.3mf");
    REQUIRE(store(path, model, false, sample_project_config(4)));
    Loaded back;
    load(path, back);
    REQUIRE(back.ok);
    const auto *colours = back.config.option<ConfigOptionStrings>("filament_colour");
    REQUIRE(colours != nullptr);
    REQUIRE(colours->values.size() == 4);
    CHECK(colours->values[0] != colours->values[3]);
    REQUIRE(back.model.objects.size() == 1);
    const ModelObject *loaded = back.model.objects[0];
    std::vector<int> extruders;
    for (const auto &kv : loaded->layer_config_ranges)
        extruders.push_back(kv.second.get().opt_int("extruder"));
    CHECK(extruders == std::vector<int>({ 1, 2, 3, 4 }));
    CHECK(height_colour_pattern_missing_filaments(*loaded, int(colours->values.size())).empty());
    // The same file seen by a 2-filament project: the pattern names filaments 3 and 4 that are not there.
    CHECK(height_colour_pattern_missing_filaments(*loaded, 2) == std::vector<int>({ 3, 4 }));
    boost::filesystem::remove(path);
}

TEST_CASE("Height colour pattern: a layer height other than the object's is reported", "[HeightColourPattern][Apply]")
{
    Model        model;
    ModelObject *object = cube_object(model);
    REQUIRE(apply_height_colour_pattern(*object, example_pattern(), grid(60., 0.2, 0.2), 0.2, false).ok);
    CHECK_FALSE(height_colour_pattern_layer_height_mismatch(*object, 0.2));
    // The print preset moved to 0.12 mm after the pattern was made: the bands would still print at 0.2.
    CHECK(height_colour_pattern_layer_height_mismatch(*object, 0.12));
}

TEST_CASE("Height colour pattern: a filament the project does not have is refused", "[HeightColourPattern][Apply]")
{
    Model        model;
    ModelObject *object = cube_object(model);
    const auto   tops   = grid(60., 0.2, 0.2);

    SECTION("in the sequence") {
        // example_pattern() uses filaments 1-3; the project has 2.
        const HeightPatternApplyResult r = apply_height_colour_pattern(*object, example_pattern(), tops, 0.2, false, 0., 2);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("Filament 3") != std::string::npos);
        CHECK(object->layer_config_ranges.empty());
        CHECK(object->height_colour_patterns.empty());
    }
    SECTION("as the fixed gap filament") {
        HeightColourPattern p = example_pattern();
        p.sequence     = { 1, 2 };
        p.gap_mode     = int(HeightPatternGapMode::Fixed);
        p.gap_filament = 5;
        CHECK_FALSE(apply_height_colour_pattern(*object, p, tops, 0.2, false, 0., 2).ok);
        CHECK(object->layer_config_ranges.empty());
    }
    SECTION("within the count, and with no count given, it applies") {
        HeightColourPattern p = example_pattern();
        CHECK(apply_height_colour_pattern(*object, p, tops, 0.2, false, 0., 3).ok);
        CHECK(remove_height_colour_pattern(*object, 1));
        CHECK(apply_height_colour_pattern(*object, p, tops, 0.2, false, 0., 0).ok);
    }
}

TEST_CASE("Height colour pattern: records saved with the old band count still load", "[HeightColourPattern][Expansion][Sidecar]")
{
    // Before the sequence-repeat fix `repeat` counted single bands. The record stores the bands it
    // generated, so loading changes none of them; only the stored count becomes whole sequences.
    const std::string legacy =
        "{\"version\":1,\"objects\":[{\"object\":1,\"patterns\":[{\"id\":1,\"start_z\":2,\"band_h\":10,\"gap_h\":10,"
        "\"repeat\":3,\"sequence\":[1,2,3,4,5],\"gap_mode\":0,\"gap_filament\":0,\"snap_to_layers\":true,"
        "\"generated\":[{\"lo\":2,\"hi\":12,\"extruder\":1,\"layer_height\":0.2},"
        "{\"lo\":22,\"hi\":32,\"extruder\":2,\"layer_height\":0.2},{\"lo\":42,\"hi\":52,\"extruder\":3,\"layer_height\":0.2}]}]}]}";
    const auto back = height_patterns_from_json(legacy);
    REQUIRE(back.size() == 1);
    REQUIRE(back[0].second.size() == 1);
    const HeightColourPattern &p = back[0].second[0];
    CHECK(p.generated.size() == 3);                    // the bands are untouched
    CHECK(p.generated[2].extruder == 3);
    CHECK(p.repeat == 1);                              // 3 old bands of a 5-filament sequence = 1 repeat
    CHECK(p.repeat_is_sequences);
    CHECK(height_pattern_band_count(p) == 3);

    // Old 7 bands over 3 filaments: 3 repeats cover them (9 bands when regenerated).
    CHECK(height_pattern_repeat_from_legacy_bands(7, 3, HeightPatternGapMode::ObjectFilament) == 3);
    CHECK(height_pattern_repeat_from_legacy_bands(3, 3, HeightPatternGapMode::ObjectFilament) == 1);
    CHECK(height_pattern_repeat_from_legacy_bands(1, 1, HeightPatternGapMode::ObjectFilament) == 1);
    // Interleaved: 3 old bands used 5 slots of a 4-filament sequence = 2 repeats (8 slots, 4 bands).
    CHECK(height_pattern_repeat_from_legacy_bands(3, 4, HeightPatternGapMode::NextInSequence) == 2);

    // A record written now says so, and is read back as it is (no second conversion).
    HeightColourPattern q = p;
    q.repeat = 2;
    std::vector<HeightColourPattern> one{ q };
    const std::string json = height_patterns_to_json({ &one });
    CHECK(json.find("repeat_seq") != std::string::npos);
    const auto again = height_patterns_from_json(json);
    REQUIRE(again.size() == 1);
    CHECK(again[0].second[0].repeat == 2);
}
