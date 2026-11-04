#include <catch2/catch.hpp>

#include "libslic3r/AdaptiveLayerMerge.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <vector>

using namespace Slic3r;

namespace {
std::vector<double> stack(size_t n, double h) { return std::vector<double>(n, h); }

// Every band top of the finer plan must also be a band boundary-compatible point of the coarser one: a coarse band
// [b, t] never has a fine band straddling b-1|b or t|t+1.
bool nests(const std::vector<AdaptiveLayerBand> &fine, const std::vector<AdaptiveLayerBand> &coarse)
{
    for (const AdaptiveLayerBand &c : coarse)
        for (const AdaptiveLayerBand &f : fine)
            if ((f.bottom_idx < c.bottom_idx && f.top_idx >= c.bottom_idx) || (f.bottom_idx <= c.top_idx && f.top_idx > c.top_idx))
                return false;
    return true;
}

PrintConfig two_nozzle_config(double d0, double d1)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_extruders(2);
    cfg.set_num_filaments(2);
    cfg.set_deserialize_strict({ { "nozzle_diameter", std::to_string(d0) + "," + std::to_string(d1) },
                                 { "min_layer_height", "0.07,0.07" },
                                 { "max_layer_height", "0.3,0.3" } });
    PrintConfig pc;
    pc.apply(cfg, true);
    return pc;
}
} // namespace

TEST_CASE("Adaptive layer merge: height window and support gap", "[AdaptiveLayerMerge]")
{
    const AdaptiveLayerHeightRange r = adaptive_layer_height_range(0.4);
    CHECK(r.min_height == Approx(0.08));
    CHECK(r.max_height == Approx(0.28));
    CHECK(r.contains(0.28));
    CHECK(!r.contains(0.3));
    CHECK(support_top_z_for_nozzle(0.2) == Approx(0.1f));
    CHECK(support_top_z_for_nozzle(0.5) == Approx(0.2f));
    CHECK(support_top_z_for_nozzle(1.0) == Approx(0.4f));
}

TEST_CASE("Adaptive layer merge: band size", "[AdaptiveLayerMerge]")
{
    CHECK(adaptive_layer_band_size(0.8, 0.2, 0.1) == 4);  // ratio 4, 0.4 mm fits [0.16, 0.56]
    CHECK(adaptive_layer_band_size(0.4, 0.4, 0.2) == 1);  // same nozzle: nothing merges
    CHECK(adaptive_layer_band_size(0.8, 0.4, 0.02) == 8); // grows until 0.16 mm is reached
    CHECK(adaptive_layer_band_size(0.6, 0.2, 0.2) == 2);  // ratio 3 = 0.6 mm > 0.42, shrinks to 2
    CHECK(adaptive_layer_band_size(0.4, 0.2, 0.1) == 2);
    CHECK(adaptive_layer_band_size(0., 0.2, 0.1) == 1);
}

TEST_CASE("Adaptive layer merge: bands from the object base", "[AdaptiveLayerMerge]")
{
    const AdaptiveLayerHeightRange r8 = adaptive_layer_height_range(0.8);
    const AdaptiveLayerHeightRange r4 = adaptive_layer_height_range(0.4);

    SECTION("regular bands, anchored at layer 0") {
        const auto bands = build_adaptive_layer_bands(stack(12, 0.1), 4, r8);
        REQUIRE(bands.size() == 3);
        CHECK(bands[0].bottom_idx == 0);
        CHECK(bands[0].top_idx == 3);
        CHECK(bands[2].top_idx == 11);
        CHECK(bands[1].merged_height == Approx(0.4));
    }
    SECTION("remainder goes to the top, no layer is left out") {
        const auto bands = build_adaptive_layer_bands(stack(10, 0.1), 4, r8);
        REQUIRE(bands.size() == 3);
        CHECK(bands[0].top_idx == 3);
        CHECK(bands[1].top_idx == 7);
        CHECK(bands[2].bottom_idx == 8);
        CHECK(bands[2].top_idx == 9);
        const auto b5 = build_adaptive_layer_bands(stack(8, 0.1), 5, r8);
        REQUIRE(b5.size() == 2);
        CHECK(b5[0].top_idx == 4);
        CHECK(b5[1].top_idx == 7);
        CHECK(b5[1].merged_height == Approx(0.3));
    }
    SECTION("commensurate nozzles nest, so the shared grid stays in sync") {
        const auto fine   = build_adaptive_layer_bands(stack(12, 0.1), 2, r4);
        const auto coarse = build_adaptive_layer_bands(stack(12, 0.1), 4, r8);
        CHECK(fine.size() == 6);
        CHECK(nests(fine, coarse));
    }
    SECTION("structural marks are honored") {
        std::vector<uint8_t> tops(12, 0);
        tops[5] = 1;
        const auto bands = build_adaptive_layer_bands(stack(12, 0.1), 4, r8, tops);
        for (const AdaptiveLayerBand &b : bands)
            CHECK_FALSE((b.bottom_idx <= 5 && b.top_idx > 5));
    }
    SECTION("too thin or nothing to merge") {
        CHECK(build_adaptive_layer_bands(stack(12, 0.1), 1, r8).empty());
        CHECK(build_adaptive_layer_bands(stack(1, 0.1), 4, r8).empty());
        const auto b = build_adaptive_layer_bands(stack(3, 0.1), 4, r8);
        REQUIRE(b.size() == 1);
        CHECK(b[0].top_idx == 2);
    }
}

TEST_CASE("Adaptive layer merge: pull band top to material and split span", "[AdaptiveLayerMerge]")
{
    const AdaptiveLayerHeightRange r8 = adaptive_layer_height_range(0.8);
    AdaptiveLayerBand band { 4, 7, 0.4 };
    std::vector<unsigned char> material(8, 1);
    material[6] = material[7] = 0;
    REQUIRE(pull_adaptive_band_top_to_material(band, stack(8, 0.1), r8, material));
    CHECK(band.top_idx == 5);
    CHECK(band.merged_height == Approx(0.2));

    AdaptiveLayerBand lone { 4, 7, 0.4 };
    std::vector<unsigned char> only_bottom(8, 0);
    only_bottom[4] = 1;
    CHECK_FALSE(pull_adaptive_band_top_to_material(lone, stack(8, 0.1), r8, only_bottom));

    std::vector<std::pair<size_t, size_t>> groups;
    REQUIRE(split_adaptive_layer_span(stack(10, 0.1), 2, 9, 4, r8, groups));
    REQUIRE(groups.size() == 2);
    CHECK(groups[0] == std::make_pair(size_t(2), size_t(5)));
    CHECK(groups[1] == std::make_pair(size_t(6), size_t(9)));
    CHECK_FALSE(split_adaptive_layer_span(stack(10, 0.1), 2, 9, 0, r8, groups));
}

TEST_CASE("Adaptive layer merge: Edge nozzle mapping", "[AdaptiveLayerMerge]")
{
    PrintConfig pc = two_nozzle_config(0.4, 0.8);
    CHECK(has_multiple_used_nozzle_diameters(pc, { 0, 1 }));
    CHECK_FALSE(has_multiple_used_nozzle_diameters(pc, { 0 }));
    CHECK(adaptive_band_reference_nozzle(pc, { 0, 1 }) == Approx(0.4));
    CHECK(adaptive_band_reference_nozzle(pc, { 1 }) == Approx(0.8));

    // Bambu window for 0.8 is [0.16, 0.56]; the printer's max_layer_height 0.3 narrows it.
    const AdaptiveLayerHeightRange r = adaptive_layer_height_range_for_filament(pc, 2);
    CHECK(r.min_height == Approx(0.16));
    CHECK(r.max_height == Approx(0.3));
    // 0.4 nozzle: [0.08, 0.28] narrowed by min 0.07 (no-op) and max 0.3 (no-op).
    const AdaptiveLayerHeightRange r0 = adaptive_layer_height_range_for_filament(pc, 1);
    CHECK(r0.min_height == Approx(0.08));
    CHECK(r0.max_height == Approx(0.28));
}
