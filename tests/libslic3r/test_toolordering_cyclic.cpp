#include <catch2/catch.hpp>

#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/PrintConfig.hpp"

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
