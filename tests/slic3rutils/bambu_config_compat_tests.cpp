#include <catch2/catch.hpp>

#include "libslic3r/BambuConfigCompat.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::BambuConfigCompat;

// Bambu Studio writes a literal "nil" into the per-extruder slots a setting does not apply to.
// This fork types many of those options as scalars, or as plain non-nullable vectors, so the
// value has to be translated before it reaches deserialize() - otherwise the option throws and
// PresetCollection::load_presets deletes the user's preset file.
//
// The rule these tests exist to protect: a nil slot means "not applicable", never zero. A speed
// or acceleration of 0 emits `G1 F0` and stalls the printer.

static const ConfigOptionDef* def_of(const std::string &key)
{
    return print_config_def.get(key);
}

static std::vector<std::string> V(std::initializer_list<const char*> items)
{
    std::vector<std::string> out;
    for (const char *s : items)
        out.emplace_back(s);
    return out;
}

TEST_CASE("has_nil spots the Bambu not-applicable marker", "[BambuCompat]")
{
    CHECK(has_nil(V({"100", "nil", "100"})));
    CHECK(has_nil(V({"nil"})));
    CHECK_FALSE(has_nil(V({"100", "200"})));
    CHECK_FALSE(has_nil(std::vector<std::string>{}));
    // "nil" is matched exactly; a value that merely contains those letters is data.
    CHECK_FALSE(has_nil(V({"nil_", "0nil"})));
}

TEST_CASE("nil slots around identical values collapse to that value", "[BambuCompat]")
{
    // top_surface_acceleration is coFloat here and coFloats in Bambu: the classic case.
    const ConfigOptionDef *def = def_of("top_surface_acceleration");
    REQUIRE(def != nullptr);
    REQUIRE(def->is_scalar());

    const NilResult r = translate_nil_array(def, V({"100", "nil", "100", "nil", "nil"}));
    CHECK(r.fix == NilFix::Collapsed);
    CHECK(r.value == "100");
    CHECK(r.changed());
    CHECK_FALSE(r.lossy());
    CHECK_FALSE(r.drop());
    CHECK(r.original == "100,nil,100,nil,nil");
}

TEST_CASE("a single surviving value collapses and is not called lossy", "[BambuCompat]")
{
    const NilResult r = translate_nil_array(def_of("sparse_infill_speed"), V({"nil", "nil", "250"}));
    CHECK(r.fix == NilFix::Collapsed);
    CHECK(r.value == "250");
    CHECK_FALSE(r.lossy());
}

TEST_CASE("differing values take the documented rule and report the loss", "[BambuCompat]")
{
    // Real shape from the owner's 0.10mm @H2D 2W Arachne.json.
    // Majority of the non-nil slots wins: 200 appears twice, 160 once.
    const NilResult r = translate_nil_array(def_of("inner_wall_speed"), V({"200", "nil", "200", "nil", "160"}));
    CHECK(r.fix == NilFix::CollapsedLossy);
    CHECK(r.value == "200");
    CHECK(r.lossy());
    CHECK(r.original == "200,nil,200,nil,160");
}

TEST_CASE("the majority wins even when it is not the first slot", "[BambuCompat]")
{
    // Real shape from 0.30mm @H2D - Custom ASA.json: 2000 twice beats 3000 once.
    const NilResult r = translate_nil_array(def_of("outer_wall_acceleration"), V({"3000", "2000", "nil", "2000", "nil"}));
    CHECK(r.fix == NilFix::CollapsedLossy);
    CHECK(r.value == "2000");
}

TEST_CASE("a tie is broken by the first non-nil slot", "[BambuCompat]")
{
    // Bambu writes the primary extruder first, so the first slot is what a single-extruder
    // machine actually prints with.
    const NilResult r = translate_nil_array(def_of("inner_wall_speed"), V({"120", "nil", "180"}));
    CHECK(r.fix == NilFix::CollapsedLossy);
    CHECK(r.value == "120");
}

TEST_CASE("percent values survive the collapse as percents", "[BambuCompat]")
{
    // small_perimeter_speed is coFloatOrPercent here, coFloatsOrPercents in Bambu.
    const NilResult r = translate_nil_array(def_of("small_perimeter_speed"), V({"80%", "nil", "80%", "nil"}));
    CHECK(r.fix == NilFix::Collapsed);
    CHECK(r.value == "80%");
}

TEST_CASE("an all-nil array keeps no value at all", "[BambuCompat]")
{
    // Seen in ELEGOO PETG Rapid Flow Rate Calibrated.json as ["nil","nil"].
    // There is nothing to keep, and fabricating a number here is exactly the forbidden move.
    const NilResult r = translate_nil_array(def_of("filament_flow_ratio"), V({"nil", "nil"}));
    CHECK(r.fix == NilFix::AllNil);
    CHECK(r.drop());
    CHECK(r.value.empty());
}

TEST_CASE("a non-nullable vector keeps its slot count and is backfilled", "[BambuCompat]")
{
    // nozzle_temperature is coInts here: collapsing it to a scalar would destroy per-extruder
    // structure, so nil slots take the nearest real neighbour instead.
    const ConfigOptionDef *def = def_of("nozzle_temperature");
    REQUIRE(def != nullptr);
    REQUIRE_FALSE(def->is_scalar());
    REQUIRE_FALSE(def->nullable);

    const NilResult r = translate_nil_array(def, V({"250", "nil", "250", "nil"}));
    CHECK(r.fix == NilFix::Backfilled);
    CHECK(r.value == "250,250,250,250");
    CHECK_FALSE(r.lossy());
}

TEST_CASE("a leading nil is filled from the value that follows", "[BambuCompat]")
{
    const NilResult r = translate_nil_array(def_of("nozzle_temperature"), V({"nil", "nil", "240"}));
    CHECK(r.fix == NilFix::Backfilled);
    CHECK(r.value == "240,240,240");
}

TEST_CASE("a backfill over differing neighbours is reported as lossy", "[BambuCompat]")
{
    // Real shape from Bambu Lab H2C 0.4 nozzle - Custom.json.
    const NilResult r = translate_nil_array(def_of("machine_max_jerk_x"), V({"20", "9", "20", "9", "nil", "nil"}));
    CHECK(r.fix == NilFix::BackfilledLossy);
    CHECK(r.value == "20,9,20,9,9,9");
    CHECK(r.lossy());
}

TEST_CASE("an already-nullable option is left completely alone", "[BambuCompat]")
{
    // These deserialize "nil" natively into their own sentinel; rewriting them here would
    // destroy the very information the nullable type exists to carry.
    const ConfigOptionDef *def = def_of("filament_retraction_length");
    if (def != nullptr && def->nullable) {
        const NilResult r = translate_nil_array(def, V({"0.8", "nil", "0.8"}));
        CHECK(r.fix == NilFix::None);
        CHECK_FALSE(r.changed());
        CHECK(r.value.empty());
    }
}

TEST_CASE("an array with no nil is not touched", "[BambuCompat]")
{
    const NilResult r = translate_nil_array(def_of("inner_wall_speed"), V({"200", "160"}));
    CHECK(r.fix == NilFix::None);
    CHECK_FALSE(r.changed());
}

TEST_CASE("an unknown option is not translated", "[BambuCompat]")
{
    const NilResult r = translate_nil_array(nullptr, V({"1", "nil"}));
    CHECK(r.fix == NilFix::None);
}

// ---------------------------------------------------------------------------------------------
// The regression that matters most: this week's `G1 F0` outage came from a speed reaching the
// G-code writer as zero. Translation must never be the thing that produces a zero.
// ---------------------------------------------------------------------------------------------
TEST_CASE("translation never invents a zero for a speed or acceleration", "[BambuCompat]")
{
    const char *speed_keys[] = {
        "inner_wall_speed", "outer_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed",
        "top_surface_speed", "support_speed", "support_interface_speed", "gap_infill_speed",
        "initial_layer_speed", "initial_layer_infill_speed", "travel_speed", "bridge_speed",
        "top_surface_acceleration", "outer_wall_acceleration", "inner_wall_acceleration",
        "default_acceleration", "travel_acceleration", "initial_layer_acceleration",
    };
    const std::vector<std::vector<std::string>> inputs = {
        V({"200", "nil"}),
        V({"nil", "200"}),
        V({"200", "nil", "200", "nil", "160"}),
        V({"nil", "nil", "120"}),
        V({"100", "nil", "100", "nil", "nil"}),
        V({"3000", "2000", "nil", "2000", "nil"}),
    };
    for (const char *key : speed_keys) {
        const ConfigOptionDef *def = def_of(key);
        if (def == nullptr)
            continue;
        for (const std::vector<std::string> &in : inputs) {
            const NilResult r = translate_nil_array(def, in);
            if (! r.changed() || r.drop())
                continue;
            INFO("key " << key << " produced \"" << r.value << "\" from \"" << r.original << "\"");
            // None of the inputs above contains a zero, so no output slot may be zero either.
            std::string token;
            std::istringstream iss(r.value);
            while (std::getline(iss, token, ',')) {
                REQUIRE_FALSE(token.empty());
                CHECK(std::stod(token) != Approx(0.0));
            }
        }
    }
}

TEST_CASE("a zero Bambu really wrote is preserved, not scrubbed", "[BambuCompat]")
{
    // Bambu genuinely stores 0 for some of these as a "not set / use default" sentinel, e.g.
    // "overhang_1_4_speed": ["0","nil","0","nil"]. Translation must carry that through
    // faithfully - the rule is "never INVENT a zero", not "never allow one".
    const NilResult r = translate_nil_array(def_of("overhang_1_4_speed"), V({"0", "nil", "0", "nil"}));
    CHECK(r.fix == NilFix::Collapsed);
    CHECK(r.value == "0");
}

// ---------------------------------------------------------------------------------------------
// End-to-end: a real Bambu preset must survive the fork's own JSON loader.
// The fixtures are copies taken from the owner's Bambu Studio data; nothing reads the live
// folder at test time.
// ---------------------------------------------------------------------------------------------
static boost::filesystem::path fixture_dir()
{
    return boost::filesystem::path(TEST_DATA_DIR) / "bambu_compat";
}

TEST_CASE("real Bambu presets load through the fork's json loader", "[BambuCompat]")
{
    const boost::filesystem::path dir = fixture_dir();
    if (! boost::filesystem::is_directory(dir)) {
        WARN("fixture directory missing: " << dir.string());
        return;
    }

    size_t seen = 0, loaded = 0, with_substitutions = 0;
    for (boost::filesystem::directory_iterator it(dir); it != boost::filesystem::directory_iterator(); ++it) {
        if (it->path().extension() != ".json")
            continue;
        ++seen;
        DynamicPrintConfig config;
        std::map<std::string, std::string> key_values;
        std::string reason;
        ConfigSubstitutions subst = config.load_from_json(
            it->path().string(), ForwardCompatibilitySubstitutionRule::Enable, key_values, reason);
        INFO("preset " << it->path().filename().string() << " reason=" << reason);
        CHECK(reason.empty());
        if (reason.empty())
            ++loaded;
        if (! subst.empty())
            ++with_substitutions;

        // Whatever landed, no speed may have become zero unless Bambu wrote a zero.
        for (const char *key : {"inner_wall_speed", "outer_wall_speed", "sparse_infill_speed"}) {
            const ConfigOption *opt = config.option(key);
            if (opt == nullptr)
                continue;
            const std::string serialized = opt->serialize();
            INFO("preset " << it->path().filename().string() << " " << key << " = " << serialized);
            CHECK(serialized != "0");
        }
    }
    INFO("fixtures " << seen << ", loaded " << loaded << ", reporting substitutions " << with_substitutions);
    CHECK(seen > 0);
    CHECK(loaded == seen);
}

TEST_CASE("the 43 presets that were deleted on 2026-09-21 load again", "[BambuCompat]")
{
    // These are the exact files named in the owner's 2026-09-21-16-52-41.log.0 as
    // "parse config ... failed", recovered from the Bambu Studio originals. Before the
    // translation every one of them threw out of deserialize() and was deleted by
    // PresetCollection::load_presets. All 43 must now parse.
    const boost::filesystem::path dir = boost::filesystem::path(TEST_DATA_DIR) / "bambu_compat_43";
    if (! boost::filesystem::is_directory(dir)) {
        WARN("fixture directory missing: " << dir.string());
        return;
    }

    size_t seen = 0, loaded = 0, failed = 0, lossy_presets = 0;
    std::vector<std::string> failures;
    for (boost::filesystem::directory_iterator it(dir); it != boost::filesystem::directory_iterator(); ++it) {
        if (it->path().extension() != ".json")
            continue;
        ++seen;
        DynamicPrintConfig config;
        std::map<std::string, std::string> key_values;
        std::string reason;
        ConfigSubstitutions subst = config.load_from_json(
            it->path().string(), ForwardCompatibilitySubstitutionRule::Enable, key_values, reason);
        if (reason.empty()) {
            ++loaded;
            if (! subst.empty())
                ++lossy_presets;
        } else {
            ++failed;
            failures.push_back(it->path().filename().string() + ": " + reason);
        }
    }
    for (const std::string &f : failures)
        WARN("still failing: " << f);
    INFO("seen " << seen << ", loaded " << loaded << ", failed " << failed
         << ", reporting a lossy substitution " << lossy_presets);
    CHECK(seen == 43);
    CHECK(failed == 0);
    CHECK(loaded == seen);
}
