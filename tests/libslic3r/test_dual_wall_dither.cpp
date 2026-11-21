// Dual wall dithering, phase 1: the colour model and the pair table (DualWallDither.hpp).
// Design: tests/design_dual_wall_dithering.md, sections 3.1-3.2.
#include <catch2/catch.hpp>

#include <cmath>
#include <set>

#include "libslic3r/DualWallDither.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// Cyan, magenta, yellow, white, all usefully translucent.
std::vector<DwdFilament> cmyw(double td = 6.)
{
    return { { { 0.f, 0.6f, 0.9f }, td }, { { 0.9f, 0.1f, 0.5f }, td }, { { 1.f, 0.9f, 0.1f }, td }, { { 1.f, 1.f, 1.f }, td } };
}

double dist(const std::array<float, 3> &a, const std::array<float, 3> &b)
{
    return std::sqrt(std::pow(a[0] - b[0], 2) + std::pow(a[1] - b[1], 2) + std::pow(a[2] - b[2], 2));
}

} // namespace

TEST_CASE("DwD: the filament preset carries a transmission distance", "[dwd]")
{
    const ConfigOptionDef *def = print_config_def.get("filament_transmission_distance");
    REQUIRE(def != nullptr);
    CHECK(def->type == coFloats);
    FullPrintConfig config = FullPrintConfig::defaults();
    REQUIRE(config.filament_transmission_distance.values.size() == 1);
    CHECK_THAT(config.filament_transmission_distance.values.front(), WithinAbs(4., 1e-9));
}

TEST_CASE("DwD: transmission follows the design's table", "[dwd]")
{
    // 0.45 mm wall, k = 3: one-way / two-way share of what is behind.
    CHECK_THAT(dwd_transmission(1., 0.45, 3.), WithinAbs(0.26, 0.005));
    CHECK_THAT(dwd_transmission(5., 0.45, 3.), WithinAbs(0.76, 0.005));
    CHECK_THAT(std::pow(dwd_transmission(5., 0.45, 3.), 2), WithinAbs(0.58, 0.005));
    CHECK_THAT(std::pow(dwd_transmission(10., 0.45, 3.), 2), WithinAbs(0.76, 0.005));
    // Unknown or nonsense TD is opaque, never a division by zero.
    CHECK(dwd_transmission(0., 0.45, 3.) == 0.);
    CHECK(dwd_transmission(-1., 0.45, 3.) == 0.);
}

TEST_CASE("DwD: sequence counts match the design", "[dwd]")
{
    const auto strict = dwd_outer_sequences(4, false);
    CHECK(strict.size() == 23);
    CHECK(dwd_outer_sequences(4, true).size() == 35);
    CHECK(dwd_inner_sequences(4).size() == 165);

    // Strict: never more than two layers of one filament in a row, the sequence taken as repeating.
    for (const auto &seq : strict) {
        REQUIRE((seq.size() == 1 || seq.size() == 2 || seq.size() == 4));
        if (seq.size() == 1)
            continue;
        for (size_t i = 0; i < seq.size(); ++i) {
            const bool three = seq[i] == seq[(i + 1) % seq.size()] && seq[i] == seq[(i + 2) % seq.size()];
            CHECK_FALSE(three);
        }
    }
    // A 2:2 mix is written as the alternation, not as AABB.
    CHECK(std::count(strict.begin(), strict.end(), std::vector<uint8_t>{ 0, 1 }) == 1);
    CHECK(std::count(strict.begin(), strict.end(), std::vector<uint8_t>{ 0, 0, 1, 1 }) == 0);

    // Every sequence is unique.
    const auto inner = dwd_inner_sequences(4);
    CHECK(std::set<std::vector<uint8_t>>(inner.begin(), inner.end()).size() == inner.size());
}

TEST_CASE("DwD: what the model predicts", "[dwd]")
{
    const DwdModelParams model;

    SECTION("an opaque outer wall hides the inner wall")
    {
        auto fils = cmyw();
        fils[0].td_mm = 0.;
        const auto a = dwd_predict(fils, { 0 }, { 2 }, 3, model);
        const auto b = dwd_predict(fils, { 0 }, { 1 }, 3, model);
        CHECK(dist(a, b) < 1e-6);
        CHECK(dist(a, fils[0].srgb) < 1e-4);
    }
    SECTION("a translucent outer wall lets the inner wall change the colour")
    {
        const auto fils = cmyw();
        const auto band1 = dwd_predict(fils, { 0, 1 }, { 0, 1 }, 3, model); // P0 band 1
        const auto band2 = dwd_predict(fils, { 0, 1 }, { 2 }, 3, model);    // P0 band 2: yellow inside
        CHECK(dist(band1, band2) > 0.05);
        CHECK(band2[2] < band1[2]); // yellow behind takes blue away
    }
    SECTION("white on white on white is white")
    {
        const auto fils = cmyw();
        CHECK(dist(dwd_predict(fils, { 3 }, { 3 }, 3, model), { 1.f, 1.f, 1.f }) < 1e-4);
    }
    SECTION("only the mix matters when the other wall is constant")
    {
        const auto fils = cmyw();
        const auto a = dwd_predict(fils, { 0, 1 }, { 2 }, 3, model);
        const auto b = dwd_predict(fils, { 0, 0, 1, 1 }, { 2 }, 3, model);
        CHECK(dist(a, b) < 1e-5);
    }
    SECTION("bad input is black, not a crash")
    {
        const auto fils = cmyw();
        CHECK(dist(dwd_predict(fils, {}, { 0 }, 3, model), { 0.f, 0.f, 0.f }) < 1e-6);
        CHECK(dist(dwd_predict(fils, { 9 }, { 0 }, 3, model), { 0.f, 0.f, 0.f }) < 1e-6);
        CHECK(dist(dwd_predict(fils, { 0 }, { 0 }, 9, model), { 0.f, 0.f, 0.f }) < 1e-6);
    }
}

TEST_CASE("DwD: the pair table", "[dwd]")
{
    const auto           fils = cmyw();
    const DwdModelParams model;
    REQUIRE(dwd_default_backing(fils) == 3);

    const auto table = dwd_build_table(fils, 3, model);
    REQUIRE(!table.empty());
    CHECK(table.size() <= 256);
    CHECK(table.size() > 50); // CMY + W reaches many distinct colours

    // Deterministic.
    const auto again = dwd_build_table(fils, 3, model);
    REQUIRE(again.size() == table.size());
    for (size_t i = 0; i < table.size(); ++i) {
        CHECK(again[i].outer == table[i].outer);
        CHECK(again[i].inner == table[i].inner);
    }

    // Every entry is its own nearest match, and entries stay apart.
    for (size_t i = 0; i < table.size(); ++i) {
        CHECK(dwd_nearest(table, table[i].srgb) == int(i));
        CHECK(table[i].backing == 3);
        CHECK(table[i].filaments_used >= 1);
        CHECK(table[i].filaments_used <= 4);
    }

    // A smaller cap is honoured.
    DwdTableParams small;
    small.max_pairs = 16;
    const auto few = dwd_build_table(fils, 3, model, small);
    CHECK(!few.empty());
    CHECK(few.size() <= 16);

    // White is reachable and is the all-white pair.
    const int white = dwd_nearest(table, { 1.f, 1.f, 1.f });
    REQUIRE(white >= 0);
    CHECK(table[white].outer == std::vector<uint8_t>{ 3 });
    CHECK(table[white].inner == std::vector<uint8_t>{ 3 });

    // Out of range: no table.
    CHECK(dwd_build_table(fils, 7, model).empty());
    CHECK(dwd_build_table({ fils[0] }, 0, model).empty());
    CHECK(dwd_nearest({}, { 0.f, 0.f, 0.f }) == -1);
}

TEST_CASE("DwD: a pair becomes a grouped manual pattern", "[dwd]")
{
    DwdPair pair;
    pair.outer   = { 0, 1 };
    pair.inner   = { 2, 2, 2, 2, 3, 3, 3, 3 };
    pair.backing = 3;
    // Filaments 1-4, row components 1 and 2: the P0 band 3 pattern.
    CHECK(dwd_pattern(pair, { 1, 2, 3, 4 }, 1, 2) == "12,33334444,4");
    // Row components 3 and 4: those become tokens 1 and 2, and physical 1 and 2 cannot be written.
    CHECK(dwd_pattern(pair, { 1, 2, 3, 4 }, 3, 4).empty());
    pair.outer = { 2, 3 };
    pair.inner = { 2 };
    CHECK(dwd_pattern(pair, { 1, 2, 3, 4 }, 3, 4) == "12,1,2");
    // A filament the mapping does not know.
    pair.outer = { 5 };
    CHECK(dwd_pattern(pair, { 1, 2, 3, 4 }, 1, 2).empty());
}
