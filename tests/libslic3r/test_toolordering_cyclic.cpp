#include <catch2/catch.hpp>

#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <functional>
#include <string>
#include <vector>

using namespace Slic3r;

TEST_CASE("toolchange_ordering config keys exist", "[ToolOrdering][Cyclic][Config]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(config.has("toolchange_ordering"));
    REQUIRE(config.opt_enum<ToolChangeOrderingType>("toolchange_ordering") == ToolChangeOrderingType::Default);
    REQUIRE(config.has("toolchange_cyclic_order"));
    REQUIRE(config.opt_string("toolchange_cyclic_order").empty());
    REQUIRE(config.has("toolchange_cyclic_first_layer"));
    REQUIRE(config.opt_bool("toolchange_cyclic_first_layer") == false);
}

TEST_CASE("parse_cyclic_order parses user cyclic toolchange sequences", "[ToolOrdering][Cyclic]")
{
    // Filament numbers are 1-based in the UI; the parser returns 0-based indices.
    SECTION("well-formed sequence") {
        REQUIRE(parse_cyclic_order("3,2,1,4", 4) == std::vector<unsigned int>({2, 1, 0, 3}));
    }

    SECTION("surrounding whitespace is tolerated") {
        REQUIRE(parse_cyclic_order(" 3 , 2 ,1, 4 ", 4) == std::vector<unsigned int>({2, 1, 0, 3}));
    }

    SECTION("out-of-range and non-positive entries are dropped") {
        // 0 is below the 1-based range, 5 is above it for a 4-filament setup, -1 is invalid.
        REQUIRE(parse_cyclic_order("0,5,-1,2", 4) == std::vector<unsigned int>({1}));
    }

    SECTION("duplicates keep only the first occurrence") {
        REQUIRE(parse_cyclic_order("2,2,1,2", 4) == std::vector<unsigned int>({1, 0}));
    }

    SECTION("garbage tokens are ignored") {
        REQUIRE(parse_cyclic_order("3,abc,,2,x1", 4) == std::vector<unsigned int>({2, 1}));
    }

    SECTION("tokens that only start with a number are ignored") {
        // "2x" must be dropped rather than parsed as filament 2.
        REQUIRE(parse_cyclic_order("3,2x,1", 4) == std::vector<unsigned int>({2, 0}));
    }

    SECTION("empty string yields an empty order") {
        REQUIRE(parse_cyclic_order("", 4).empty());
    }

    SECTION("a partial sequence only names the filaments it lists") {
        REQUIRE(parse_cyclic_order("3,1", 4) == std::vector<unsigned int>({2, 0}));
    }
}

TEST_CASE("apply_cyclic_order sorts ascending or follows a custom sequence", "[ToolOrdering][Cyclic]")
{
    SECTION("empty order sorts the layer ascending") {
        std::vector<unsigned int> filaments{3, 1, 2, 0};
        apply_cyclic_order(filaments, {});
        REQUIRE(filaments == std::vector<unsigned int>({0, 1, 2, 3}));
    }

    SECTION("custom sequence 3,2,1,4") {
        std::vector<unsigned int> filaments{0, 1, 2, 3};
        apply_cyclic_order(filaments, parse_cyclic_order("3,2,1,4", 4));
        REQUIRE(filaments == std::vector<unsigned int>({2, 1, 0, 3}));
    }

    SECTION("unlisted filaments print last in ascending order") {
        std::vector<unsigned int> filaments{3, 1, 0, 2};
        apply_cyclic_order(filaments, parse_cyclic_order("3,1", 4));
        REQUIRE(filaments == std::vector<unsigned int>({2, 0, 1, 3}));
    }

    SECTION("subset of layer filaments still follows the sequence") {
        std::vector<unsigned int> filaments{3, 0};
        apply_cyclic_order(filaments, parse_cyclic_order("3,2,1,4", 4));
        REQUIRE(filaments == std::vector<unsigned int>({0, 3}));
    }
}

// ============================================================================
// PR 50 follow-up — integration-level coverage of the two pieces the unit tests
// above do not reach: the first-layer skip and the cyclic_filament_count
// derivation from physical + mixed counts.
//
// get_custom_seq is the callback reorder_extruders_for_minimum_flush_volume
// hands to the flush-volume reorder. It is built by make_cyclic_custom_seq from
// exactly the inputs the slicer computes, so driving that factory exercises the
// real decision rather than a reimplementation of it: which layers get a forced
// sequence, and what that sequence is.
//
// The filament count matters because it is the range parse_cyclic_order
// validates against. It is physical + enabled mixed - NOT sqrt(flush matrix) -
// so on a fork setup where the flush matrix is short, or where a mixed filament
// adds a virtual index past the physical count, a user sequence naming the high
// filament is still accepted. Getting this wrong silently truncates the
// sequence, which is the interaction the review flagged against the fork
// physical_extruder_map / H2C rack work.
// ============================================================================

namespace {

// Run the real callback for one layer and return the 1-based sequence it forces.
// `applied` is the callback return: false means the layer keeps its own order.
std::vector<int> run_custom_seq(const std::function<bool(int, std::vector<int> &)> &fn, int layer_idx, bool &applied)
{
    std::vector<int> out;
    applied = fn(layer_idx, out);
    return out;
}

} // namespace

TEST_CASE("get_custom_seq skips the first layer unless toolchange_cyclic_first_layer is set",
          "[ToolOrdering][Cyclic]")
{
    // Three layers, all printing filaments 0..2 in an order the cyclic sequence
    // would change, so "skipped" and "applied" are distinguishable.
    const std::vector<std::vector<unsigned int>> layer_filaments = {
        {2, 0, 1}, // layer 0
        {2, 0, 1}, // layer 1
        {2, 0, 1}, // layer 2
    };
    const std::vector<unsigned int> cyclic_order = parse_cyclic_order("3,2,1", 3);
    REQUIRE(cyclic_order == std::vector<unsigned int>({2, 1, 0}));

    SECTION("first layer is left alone by default") {
        auto fn = make_cyclic_custom_seq({}, layer_filaments,
                                         /*use_cyclic_ordering*/ true,
                                         /*cyclic_first_layer*/ false,
                                         cyclic_order);

        bool applied = false;
        run_custom_seq(fn, 0, applied);
        // Declining is what preserves the adhesion-optimized first-layer order.
        CHECK_FALSE(applied);

        // Later layers still get the cyclic sequence, 1-based.
        const std::vector<int> seq1 = run_custom_seq(fn, 1, applied);
        CHECK(applied);
        CHECK(seq1 == std::vector<int>({3, 2, 1}));

        const std::vector<int> seq2 = run_custom_seq(fn, 2, applied);
        CHECK(applied);
        CHECK(seq2 == std::vector<int>({3, 2, 1}));
    }

    SECTION("opting in forces the sequence onto the first layer too") {
        auto fn = make_cyclic_custom_seq({}, layer_filaments,
                                         /*use_cyclic_ordering*/ true,
                                         /*cyclic_first_layer*/ true,
                                         cyclic_order);

        bool                   applied = false;
        const std::vector<int> seq0    = run_custom_seq(fn, 0, applied);
        CHECK(applied);
        CHECK(seq0 == std::vector<int>({3, 2, 1}));
    }

    SECTION("with cyclic ordering off no layer is forced") {
        auto fn = make_cyclic_custom_seq({}, layer_filaments,
                                         /*use_cyclic_ordering*/ false,
                                         /*cyclic_first_layer*/ false,
                                         {});

        for (int layer = 0; layer < 3; ++layer) {
            DYNAMIC_SECTION("layer " << layer) {
                bool applied = false;
                run_custom_seq(fn, layer, applied);
                CHECK_FALSE(applied);
            }
        }
    }
}

TEST_CASE("get_custom_seq lets an explicit other-layers sequence win over cyclic order",
          "[ToolOrdering][Cyclic]")
{
    // A user-specified per-layer sequence must not be overridden by the cyclic
    // one, including on the first layer when cyclic_first_layer is set.
    const std::vector<std::vector<unsigned int>> layer_filaments = {{0, 1, 2}, {0, 1, 2}, {0, 1, 2}};
    // Layers 1..2 (1-based) print 1,3,2 by explicit request.
    const std::vector<LayerPrintSequence> other_layers_seqs = {{{1, 2}, {1, 3, 2}}};

    auto fn = make_cyclic_custom_seq(other_layers_seqs, layer_filaments,
                                     /*use_cyclic_ordering*/ true,
                                     /*cyclic_first_layer*/ true,
                                     parse_cyclic_order("3,2,1", 3));

    bool applied = false;
    // layer_idx is 0-based here, the sequence range is 1-based: layer 0 -> layer 1.
    const std::vector<int> seq0 = run_custom_seq(fn, 0, applied);
    CHECK(applied);
    CHECK(seq0 == std::vector<int>({1, 3, 2}));

    const std::vector<int> seq1 = run_custom_seq(fn, 1, applied);
    CHECK(applied);
    CHECK(seq1 == std::vector<int>({1, 3, 2}));

    // Layer 3 (0-based 2) is outside the explicit range, so cyclic applies there.
    const std::vector<int> seq2 = run_custom_seq(fn, 2, applied);
    CHECK(applied);
    CHECK(seq2 == std::vector<int>({3, 2, 1}));
}

TEST_CASE("get_custom_seq only reorders the filaments a layer actually prints", "[ToolOrdering][Cyclic]")
{
    // A layer using a subset keeps just that subset, in cyclic order. This is the
    // property that makes the callback safe to apply to every layer.
    const std::vector<std::vector<unsigned int>> layer_filaments = {
        {0, 1, 2, 3},
        {3, 0}, // subset
        {},     // empty layer
    };

    auto fn = make_cyclic_custom_seq({}, layer_filaments, true, true, parse_cyclic_order("3,2,1,4", 4));

    bool applied = false;
    CHECK(run_custom_seq(fn, 0, applied) == std::vector<int>({3, 2, 1, 4}));
    CHECK(applied);

    // Of the subset {0,3}: the sequence "3,2,1,4" ranks filament 0 (listed third)
    // ahead of filament 3 (listed fourth), so 0 prints first -> 1-based {1,4}.
    CHECK(run_custom_seq(fn, 1, applied) == std::vector<int>({1, 4}));
    CHECK(applied);

    // An empty layer yields an empty sequence rather than being skipped.
    CHECK(run_custom_seq(fn, 2, applied).empty());
    CHECK(applied);

    // Out-of-range layer indices decline rather than reading past the snapshot.
    run_custom_seq(fn, 3, applied);
    CHECK_FALSE(applied);
    run_custom_seq(fn, -1, applied);
    CHECK_FALSE(applied);
}

TEST_CASE("cyclic_filament_count derives the sequence range from physical and mixed counts",
          "[ToolOrdering][Cyclic]")
{
    SECTION("no mixed manager: the physical count is the range") {
        CHECK(cyclic_filament_count(4, nullptr, 99) == 4);
        CHECK(cyclic_filament_count(1, nullptr, 99) == 1);
    }

    SECTION("zero physical falls back to the extruder count") {
        // Only when there is no physical count to go on at all.
        CHECK(cyclic_filament_count(0, nullptr, 3) == 3);
    }

    SECTION("mixed filaments extend the range past the physical count") {
        MixedFilamentManager           mgr;
        const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
        // Custom mixed rows over 3 physical filaments add addressable indices.
        mgr.add_custom_filament(1, 2, 50, colors);
        mgr.add_custom_filament(2, 3, 50, colors);
        REQUIRE(mgr.enabled_count() > 0);
        REQUIRE(mgr.total_filaments(3) == 3 + mgr.enabled_count());

        const unsigned int count = cyclic_filament_count(3, &mgr, 99);
        CHECK(count == (unsigned int) mgr.total_filaments(3));
        CHECK(count > 3);

        // The point of the wider range: a sequence naming a mixed filament index
        // is accepted rather than dropped as out of range. Against the physical
        // count alone, filament 4 would not survive the parse.
        const std::string seq = "4,1";
        CHECK(parse_cyclic_order(seq, count) == std::vector<unsigned int>({3, 0}));
        CHECK(parse_cyclic_order(seq, 3) == std::vector<unsigned int>({0}));
    }
}

TEST_CASE("a short flush matrix does not truncate the cyclic sequence", "[ToolOrdering][Cyclic]")
{
    // The regression the derivation guards against: number_of_extruders comes from
    // sqrt(flush_volumes_matrix), which can be smaller than the real filament count
    // on fork setups. Because it is only the zero-physical fallback, the sequence
    // range still covers every physical filament.
    const unsigned int short_flush_matrix_extruders = 2;
    CHECK(cyclic_filament_count(4, nullptr, short_flush_matrix_extruders) == 4);

    // So a 4-filament sequence survives in full.
    CHECK(parse_cyclic_order("4,3,2,1", cyclic_filament_count(4, nullptr, short_flush_matrix_extruders))
          == std::vector<unsigned int>({3, 2, 1, 0}));
    // Whereas validating against the short matrix alone would drop the top half.
    CHECK(parse_cyclic_order("4,3,2,1", short_flush_matrix_extruders) == std::vector<unsigned int>({1, 0}));
}
