#include <catch2/catch.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "../fff_print/test_data.hpp"

#include <regex>
#include <string>
#include <vector>

using namespace Slic3r;

// Extruder-change long retraction (long_retractions_when_ec / retraction_distances_when_ec).
//
// On a dual-nozzle machine (H2D and friends) the change_filament_gcode macro carries
//
//     {if long_retraction_when_ec}
//     M620.11 K1 I[current_filament_id] B[current_hotend] R{retraction_distance_when_ec} F...
//     {else}
//     M620.11 K0 I[current_filament_id] B[current_hotend] R0
//     {endif}
//
// and the firmware uses K/R to park the idle filament with a controlled retraction instead of
// unloading it. Those two placeholders are PER TOOLCHANGE and are keyed by the filament being
// switched TO - Bambu Studio sets them at four toolchange sites from the target filament's entry
// of the two per-filament options.
//
// This fork used to hardcode the pair to false/0 globally ("Ultra: remaining single-mapped shims"),
// so every switch emitted K0 ... R0 no matter what the project config said. These cases pin the
// per-toolchange behaviour and, just as importantly, the ABSENT-VALUE behaviour: both options are
// nullable, and a filament that carries no value (an old project, or a preset predating the keys)
// must still produce K0 R0 rather than the raw nil sentinel - a nil bool is 0xFF, which would read
// as "true", and a nil float is NaN, which would land "Rnan" on the wire.

namespace {

// The M620.11 K line, reduced to the (K, R) pair.
struct KR
{
    std::string k;
    std::string r;
    bool operator==(const KR &rhs) const { return k == rhs.k && r == rhs.r; }
};

// An exported G-code carries a "; key = value" config dump that echoes change_filament_gcode
// verbatim - both literal branches of the macro included - so every scan must skip it. The dump sits
// at the END of the file for a non-Bambu printer and right after the header for a Bambu one
// (Print::is_BBL_printer()), so cut the delimited block out wherever it is instead of assuming
// the tail: a head-of-file dump used to make the "body" empty and every scan find nothing.
std::string body_of(const std::string &gcode)
{
    static const std::string begin_tag = "; CONFIG_BLOCK_START";
    static const std::string end_tag   = "; CONFIG_BLOCK_END";
    std::string body = gcode;
    for (size_t b; (b = body.find(begin_tag)) != std::string::npos;) {
        const size_t e = body.find(end_tag, b);
        body.erase(b, e == std::string::npos ? std::string::npos : e + end_tag.size() - b);
    }
    return body;
}

std::vector<KR> collect_kr(const std::string &gcode_in)
{
    const std::string gcode = body_of(gcode_in);
    std::vector<KR>   out;
    // "; EC K<k> R<r>" - the probe template below, chosen over a literal M620.11 so the case does
    // not depend on this fork shipping an H2D machine preset in the test tree.
    const std::regex  re(R"(; EC K([0-9]+) R([0-9.]+))");
    auto              it  = std::sregex_iterator(gcode.begin(), gcode.end(), re);
    const auto        end = std::sregex_iterator();
    for (; it != end; ++it)
        out.push_back(KR{ (*it)[1].str(), (*it)[2].str() });
    return out;
}

// A two-filament print: two cubes, auto-assigned to filament 0 and filament 1, with a
// change_filament_gcode that prints the two placeholders on every toolchange.
DynamicPrintConfig two_filament_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        // The probe mirrors the shipped H2D change_filament_gcode: {if long_retraction_when_ec}
        // selects between two literal K lines and only R is interpolated. Using the real shape
        // means the case tests what the machine template actually does with these placeholders.
        { "change_filament_gcode",
          "{if long_retraction_when_ec}\n"
          "; EC K1 R{retraction_distance_when_ec}\n"
          "{else}\n"
          "; EC K0 R0\n"
          "{endif}" },
        { "layer_height", 0.3 },
        { "initial_layer_print_height", 0.3 },
        // Keep the print tiny but force a toolchange on EVERY layer: the walls print with
        // filament 1 and the infill with filament 2, so the plate alternates the whole way up.
        // (Two separate objects would only change tool once, which cannot show per-filament values.)
        { "wall_loops", 1 },
        { "wall_filament", 1 },
        { "sparse_infill_filament", 2 },
        { "solid_infill_filament", 2 },
        { "sparse_infill_density", "25%" },
        { "top_shell_layers", 0 },
        { "bottom_shell_layers", 0 },
        { "enable_prime_tower", false },
        { "enable_support", false },
        { "skirt_loops", 0 },
    });
    return config;
}


std::string slice_two_filaments(const DynamicPrintConfig &config)
{
    // One cube whose walls and infill use different filaments: a toolchange twice per layer.
    return Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config);
}

} // namespace

SCENARIO("Extruder-change retraction placeholders follow the target filament", "[EcRetraction]")
{
    GIVEN("a two-filament print whose filaments carry different _when_ec values")
    {
        DynamicPrintConfig config = two_filament_config();
        // Filament 0: feature on, 10 mm. Filament 1: feature off.
        config.set_deserialize_strict({
            { "long_retractions_when_ec", "1,0" },
            { "retraction_distances_when_ec", "10,7" },
        });

        const std::string     gcode = slice_two_filaments(config);
        const std::vector<KR> krs   = collect_kr(gcode);

        THEN("the print really performed toolchanges")
        {
            REQUIRE(krs.size() >= 2);
        }

        THEN("every emitted pair is one of the two filaments' own values")
        {
            // Switching TO filament 0 -> K1 R10. Switching TO filament 1 -> K0 R0
            // (the distance is forced to 0 when the feature is off, so K and R never disagree).
            for (const KR &kr : krs) {
                const bool to_filament_0 = (kr.k == "1");
                if (to_filament_0)
                    REQUIRE(kr.r == "10");
                else
                    REQUIRE(kr.r == "0");
            }
        }

        THEN("both filaments' values actually appear, so the value is not a global constant")
        {
            bool saw_on  = false;
            bool saw_off = false;
            for (const KR &kr : krs) {
                if (kr.k == "1")
                    saw_on = true;
                else if (kr.k == "0")
                    saw_off = true;
            }
            REQUIRE(saw_on);
            REQUIRE(saw_off);
        }
    }

    GIVEN("a two-filament print with the feature enabled on both filaments")
    {
        DynamicPrintConfig config = two_filament_config();
        config.set_deserialize_strict({
            { "long_retractions_when_ec", "1,1" },
            { "retraction_distances_when_ec", "10,10" },
        });

        const std::string     gcode = slice_two_filaments(config);
        const std::vector<KR> krs   = collect_kr(gcode);

        THEN("every switch carries K1 R10, the way Bambu Studio emits it")
        {
            REQUIRE(krs.size() >= 2);
            // Report the whole sequence when it is not uniform, so a stray entry is identifiable.
            std::string seq;
            for (const KR &kr : krs)
                seq += "K" + kr.k + "R" + kr.r + " ";
            INFO("sequence: " << seq);
            for (const KR &kr : krs) {
                REQUIRE(kr.k == "1");
                REQUIRE(kr.r == "10");
            }
        }
    }

    GIVEN("a two-filament print whose config never mentions the two options")
    {
        // The absent-value case: an old project or a preset predating these keys. The options are
        // nullable, and the registered defaults are false / 10 - so the pair must come out K0 R0,
        // i.e. byte-identical to what this fork produced before the options existed.
        const std::vector<KR> krs = collect_kr(slice_two_filaments(two_filament_config()));

        THEN("every switch is K0 R0")
        {
            REQUIRE(krs.size() >= 2);
            for (const KR &kr : krs) {
                REQUIRE(kr.k == "0");
                REQUIRE(kr.r == "0");
            }
        }
    }

    GIVEN("a config whose _when_ec values are explicitly nil")
    {
        // Nil must not leak the sentinel into the placeholders: a nil bool is 0xFF, which reads
        // as "true" through the ordinary accessor, and a nil float is NaN, which would print
        // "Rnan". This is checked on the config directly rather than through a slice: a NaN in a
        // min/max-bounded option does not survive Print::validate(), so an all-nil plate never
        // reaches G-code export at all. The reachable case - the key simply absent - is the
        // scenario above, and it is the one an old project actually produces.
        DynamicPrintConfig config = two_filament_config();
        config.set_deserialize_strict({
            { "long_retractions_when_ec", "nil,nil" },
            { "retraction_distances_when_ec", "nil,nil" },
        });

        THEN("both entries really are nil, and are recognised as nil")
        {
            const auto *lr = config.option<ConfigOptionBoolsNullable>("long_retractions_when_ec");
            const auto *rd = config.option<ConfigOptionFloatsNullable>("retraction_distances_when_ec");
            REQUIRE(lr != nullptr);
            REQUIRE(rd != nullptr);
            REQUIRE(lr->is_nil(0));
            REQUIRE(rd->is_nil(0));
            // The raw value is the sentinel, which is exactly why the emitter may not read it
            // without checking is_nil() first: as a plain bool it would be true, not false.
            REQUIRE(lr->values[0] != 0);
        }
    }
}

SCENARIO("Extruder-change retraction probe works in both G-code layouts", "[EcRetraction]")
{
    // Print::is_BBL_printer() decides where the config dump goes: after the header for a Bambu
    // printer (the H2D case this feature is for), at the end otherwise. It used to be an
    // uninitialized member, so a stack Print in a test picked a layout at random and the tail-only
    // scan above saw an empty body whenever the garbage byte was non-zero.
    GIVEN("a default-constructed Print")
    {
        Slic3r::Print print;
        THEN("it is not a Bambu printer until the GUI or CLI says so")
        {
            REQUIRE_FALSE(print.is_BBL_printer());
        }
    }

    for (const bool bbl : { false, true }) {
        GIVEN(std::string("the per-filament values, is_BBL_printer = ") + (bbl ? "true" : "false"))
        {
            DynamicPrintConfig config = two_filament_config();
            config.set_deserialize_strict({
                { "long_retractions_when_ec", "1,0" },
                { "retraction_distances_when_ec", "10,7" },
            });
            Slic3r::Print print;
            Slic3r::Model model;
            Slic3r::Test::init_print({ Slic3r::Test::TestMesh::cube_20x20x20 }, print, model, config);
            print.is_BBL_printer() = bbl;
            const std::string     gcode = Slic3r::Test::gcode(print);
            const std::vector<KR> krs   = collect_kr(gcode);

            THEN("the config dump is where the layout puts it")
            {
                const size_t config_at = gcode.find("; CONFIG_BLOCK_START");
                const size_t exec_at   = gcode.find("; EXECUTABLE_BLOCK_START");
                REQUIRE(config_at != std::string::npos);
                if (bbl && exec_at != std::string::npos)
                    REQUIRE(config_at < exec_at);
            }
            THEN("switches to filament 0 carry K1 R10 and switches to filament 1 carry K0 R0")
            {
                REQUIRE(krs.size() >= 2);
                bool saw_on = false, saw_off = false;
                for (const KR &kr : krs) {
                    if (kr.k == "1") {
                        saw_on = true;
                        REQUIRE(kr.r == "10");
                    } else {
                        saw_off = true;
                        REQUIRE(kr == KR{ "0", "0" });
                    }
                }
                REQUIRE(saw_on);
                REQUIRE(saw_off);
            }
        }
    }
}

SCENARIO("The two _when_ec options are registered as nullable per-filament options", "[EcRetraction]")
{
    const DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    GIVEN("the full print config")
    {
        THEN("long_retractions_when_ec is a nullable bool vector")
        {
            const ConfigOption *opt = config.option("long_retractions_when_ec");
            REQUIRE(opt != nullptr);
            REQUIRE(opt->type() == coBools);
            REQUIRE(opt->nullable());
        }

        THEN("retraction_distances_when_ec is a nullable float vector")
        {
            const ConfigOption *opt = config.option("retraction_distances_when_ec");
            REQUIRE(opt != nullptr);
            REQUIRE(opt->type() == coFloats);
            REQUIRE(opt->nullable());
        }

        THEN("the shipped defaults are feature-off")
        {
            DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
            c.set_num_filaments(2);
            INFO("long=" << c.option("long_retractions_when_ec")->serialize()
                 << " dist=" << c.option("retraction_distances_when_ec")->serialize());
            REQUIRE(c.option("long_retractions_when_ec")->serialize() == "0,0");
        }

        THEN("they resize with the filament count, not with the extruder count")
        {
            DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
            c.set_num_filaments(4);
            REQUIRE(c.option<ConfigOptionBoolsNullable>("long_retractions_when_ec")->values.size() == 4);
            REQUIRE(c.option<ConfigOptionFloatsNullable>("retraction_distances_when_ec")->values.size() == 4);
        }

        THEN("they round-trip through serialization, nil included")
        {
            DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
            c.set_num_filaments(2);
            c.set_deserialize_strict({
                { "long_retractions_when_ec", "1,nil" },
                { "retraction_distances_when_ec", "10,nil" },
            });
            REQUIRE(c.option("long_retractions_when_ec")->serialize() == "1,nil");
            REQUIRE(c.option("retraction_distances_when_ec")->serialize() == "10,nil");
        }
    }
}
