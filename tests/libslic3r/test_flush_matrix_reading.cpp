#include <catch2/catch.hpp>

#include "libslic3r/FilamentGroup.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentCliGates.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

using namespace Slic3r;

// flush_volumes_matrix holds, for N filaments, either ONE N x N matrix that every nozzle uses (as this fork's GUI
// and command line write two-extruder H2D / H2C projects) or one N x N matrix PER NOZZLE, nozzle after nozzle
// (2 x N x N, as Bambu Studio writes them). These cases pin how each reader takes both shapes.

namespace {

// matrix[block][from][to] = 1000 * (block + 1) + 10 * from + to + 1, so every entry is distinct and nonzero.
std::vector<double> numbered_blocks(size_t filaments, size_t blocks)
{
    std::vector<double> values;
    for (size_t b = 0; b < blocks; ++b)
        for (size_t from = 0; from < filaments; ++from)
            for (size_t to = 0; to < filaments; ++to)
                values.push_back(double(1000 * (b + 1) + 10 * from + to + 1));
    return values;
}

std::vector<double> block_of(const std::vector<double> &values, size_t filaments, size_t block)
{
    return std::vector<double>(values.begin() + block * filaments * filaments, values.begin() + (block + 1) * filaments * filaments);
}

} // namespace

TEST_CASE("flush_matrix_layout tells one shared matrix from one matrix per nozzle", "[FlushMatrix]")
{
    SECTION("one N x N matrix on two nozzles is one block")
    {
        const FlushMatrixLayout layout = flush_matrix_layout(16, 2, 4);
        CHECK(layout.filaments == 4);
        CHECK(layout.blocks == 1);
        const FlushMatrixLayout by_size = flush_matrix_layout(16, 2);
        CHECK(by_size.filaments == 4);
        CHECK(by_size.blocks == 1);
    }
    SECTION("2 x N x N on two nozzles is one block per nozzle")
    {
        for (size_t n = 1; n <= 8; ++n) {
            INFO("filaments " << n);
            const FlushMatrixLayout layout = flush_matrix_layout(2 * n * n, 2, n);
            CHECK(layout.filaments == n);
            CHECK(layout.blocks == 2);
            const FlushMatrixLayout by_size = flush_matrix_layout(2 * n * n, 2);
            CHECK(by_size.filaments == n);
            CHECK(by_size.blocks == 2);
        }
    }
    SECTION("one nozzle, or a length that is neither, is the old floor(sqrt) square")
    {
        CHECK(flush_matrix_layout(32, 1).filaments == 5);
        CHECK(flush_matrix_layout(32, 1).blocks == 1);
        CHECK(flush_matrix_layout(10, 2, 3).filaments == 3);
        CHECK(flush_matrix_layout(10, 2, 3).blocks == 1);
        CHECK(flush_matrix_layout(0, 2).filaments == 0);
    }
    SECTION("four equal blocks on a four-tool printer stay one square without a filament count")
    {
        // 4 x 2 x 2 = 16 is a 4 x 4 square; a toolchanger's list is read as it always was.
        CHECK(flush_matrix_layout(16, 4).filaments == 4);
        CHECK(flush_matrix_layout(16, 4).blocks == 1);
    }
}

TEST_CASE("get_flush_volumes_matrix gives every nozzle the whole of a single matrix", "[FlushMatrix]")
{
    SECTION("N x N on two nozzles: the whole matrix for each")
    {
        const std::vector<double> single = numbered_blocks(3, 1);
        CHECK(get_flush_volumes_matrix(single, 0, 2, 3) == single);
        CHECK(get_flush_volumes_matrix(single, 1, 2, 3) == single);
    }
    SECTION("2 x N x N: each nozzle its own block")
    {
        const std::vector<double> per_nozzle = numbered_blocks(3, 2);
        CHECK(get_flush_volumes_matrix(per_nozzle, 0, 2, 3) == block_of(per_nozzle, 3, 0));
        CHECK(get_flush_volumes_matrix(per_nozzle, 1, 2, 3) == block_of(per_nozzle, 3, 1));
    }
    SECTION("one nozzle: unchanged")
    {
        const std::vector<double> single = numbered_blocks(3, 1);
        CHECK(get_flush_volumes_matrix(single, 0, 1, 3) == single);
        CHECK(get_flush_volumes_matrix(single, size_t(-1), 2, 3) == single);
        CHECK(get_flush_volumes_matrix(single) == single);
    }
    SECTION("a too-short matrix is still cut in equal parts")
    {
        const std::vector<double> short_list = {1., 2., 3., 4., 5., 6.};
        CHECK(get_flush_volumes_matrix(short_list, 0, 2, 3) == std::vector<double>{1., 2., 3.});
        CHECK(get_flush_volumes_matrix(short_list, 1, 2, 3) == std::vector<double>{4., 5., 6.});
    }
}

TEST_CASE("The filament grouping reads a single matrix whole on both nozzles", "[FlushMatrix][FilamentGroup]")
{
    // Three filaments; 0 <-> 1 flushes 600 mm3 both ways, every pair with 2 only 50.
    const std::vector<double> single = {  0., 600., 50.,
                                        600.,   0., 50.,
                                         50.,  50.,  0. };
    PrintConfig config;
    config.nozzle_diameter.values      = {0.4, 0.4};
    config.filament_colour.values      = {"#FFFFFF", "#000000", "#FF0000"};
    config.flush_volumes_matrix.values = single;
    config.flush_multiplier.value      = 1.;

    const std::vector<FlushMatrix> matrices = GroupReorder::prepare_flush_matrices(config);
    REQUIRE(matrices.size() == 2);
    for (size_t nozzle = 0; nozzle < 2; ++nozzle) {
        INFO("nozzle " << nozzle);
        REQUIRE(matrices[nozzle].size() == 3);
        for (size_t from = 0; from < 3; ++from)
            for (size_t to = 0; to < 3; ++to)
                CHECK(matrices[nozzle][from][to] == float(single[from * 3 + to]));
    }

    // The grouping run on those matrices: four layers that each print all three filaments.
    auto group = [](const std::vector<FlushMatrix> &flush) {
        FilamentGroupContext ctx;
        ctx.model_info.flush_matrix = flush;
        ctx.model_info.layer_filaments.assign(4, std::vector<unsigned int>{0, 1, 2});
        for (const char *colour : {"#FFFFFF", "#000000", "#FF0000"}) {
            FilamentGroupUtils::FilamentInfo info;
            info.color      = FilamentGroupUtils::Color(std::string(colour));
            info.type       = "PLA";
            info.usage_type = FilamentUsageType::ModelOnly;
            ctx.model_info.filament_info.emplace_back(info);
        }
        ctx.model_info.filament_ids.resize(3);
        ctx.model_info.unprintable_filaments.assign(2, std::set<int>());
        ctx.group_info.total_filament_num  = 3;
        ctx.group_info.max_gap_threshold   = 0.01;
        ctx.group_info.mode                = FGMode::FlushMode;
        ctx.group_info.strategy            = FGStrategy::BestCost;
        ctx.group_info.filament_volume_map.assign(3, int(NozzleVolumeType::nvtHybrid));
        ctx.machine_info.max_group_size.assign(2, 3);
        ctx.machine_info.prefer_non_model_filament.assign(2, false);
        ctx.machine_info.master_extruder_id = 1;
        for (int nozzle = 0; nozzle < 2; ++nozzle) {
            MultiNozzleUtils::NozzleInfo info;
            info.diameter    = "0.4";
            info.volume_type = NozzleVolumeType::nvtStandard;
            info.extruder_id = nozzle;
            info.group_id    = nozzle;
            ctx.nozzle_info.nozzle_list.emplace_back(info);
            ctx.nozzle_info.extruder_nozzle_list[nozzle] = {nozzle};
        }
        return FilamentGroup(ctx).calc_filament_group();
    };

    const std::vector<int> map = group(matrices);
    REQUIRE(map.size() == 3);
    // Read whole, the 600 mm3 pair goes to different nozzles.
    CHECK(map[0] != map[1]);

    // The old reading cut the nine values in halves, one per nozzle, padded with zeros: it groups otherwise.
    std::vector<FlushMatrix> halved;
    for (size_t nozzle = 0; nozzle < 2; ++nozzle) {
        std::vector<double> part = get_flush_volumes_matrix(single, nozzle, 2);
        part.resize(9, 0.);
        FlushMatrix m(3, std::vector<float>(3));
        for (size_t i = 0; i < 9; ++i)
            m[i / 3][i % 3] = float(part[i]);
        halved.emplace_back(m);
    }
    CHECK(group(halved) != map);
}

TEST_CASE("The command line zeroes each nozzle's diagonal and mixed rows only", "[FlushMatrix][MixedFilamentCli]")
{
    const std::vector<std::string> colours = {"#FFFFFF", "#000000", "#FF0000", "#0000FF"};
    MixedFilamentManager           no_mixed;

    SECTION("2 x N x N, no mixed filament: only the true diagonals")
    {
        std::vector<double> values = numbered_blocks(4, 2);
        const std::vector<double> before = values;
        zero_mixed_flush_rows_and_cols_for_nozzles(values, 2, no_mixed, 4);
        for (size_t b = 0; b < 2; ++b)
            for (size_t from = 0; from < 4; ++from)
                for (size_t to = 0; to < 4; ++to) {
                    const size_t idx = b * 16 + from * 4 + to;
                    INFO("block " << b << " from " << from << " to " << to);
                    CHECK(values[idx] == (from == to ? 0. : before[idx]));
                }
    }
    SECTION("2 x N x N with a mixed slot: its row and column in both blocks")
    {
        MixedFilamentManager mgr;
        mgr.add_custom_filament(1, 2, 50, colours);
        REQUIRE(mgr.filament_id_from_mixed_index(0, 4) == 5);
        std::vector<double> values = numbered_blocks(5, 2);
        const std::vector<double> before = values;
        zero_mixed_flush_rows_and_cols_for_nozzles(values, 2, mgr, 4);
        for (size_t b = 0; b < 2; ++b)
            for (size_t from = 0; from < 5; ++from)
                for (size_t to = 0; to < 5; ++to) {
                    const size_t idx = b * 25 + from * 5 + to;
                    INFO("block " << b << " from " << from << " to " << to);
                    CHECK(values[idx] == ((from == to || from == 4 || to == 4) ? 0. : before[idx]));
                }
    }
    SECTION("one N x N matrix on two nozzles: as before")
    {
        std::vector<double> values = numbered_blocks(4, 1);
        std::vector<double> expected = values;
        zero_mixed_flush_rows_and_cols(expected, 4, no_mixed, 4);
        zero_mixed_flush_rows_and_cols_for_nozzles(values, 2, no_mixed, 4);
        CHECK(values == expected);
    }
    SECTION("one nozzle: as before")
    {
        std::vector<double> values = numbered_blocks(4, 2); // 32 values read as a 5 x 5 square, as always
        std::vector<double> expected = values;
        zero_mixed_flush_rows_and_cols(expected, 5, no_mixed, 4);
        zero_mixed_flush_rows_and_cols_for_nozzles(values, 1, no_mixed, 4);
        CHECK(values == expected);
    }
}

TEST_CASE("The layer order reads the matrix of the nozzle each change happens on", "[FlushMatrix][ToolOrdering]")
{
    // Filaments 0 and 1 on the left nozzle, 2 on the right; one 3 x 3 block per nozzle.
    const std::vector<double> per_nozzle = {  0., 290., 650.,
                                            740.,   0., 750.,
                                            650., 550.,   0.,
                                       /* right */
                                              0., 240., 340.,
                                            240.,   0., 710.,
                                            540.,  60.,   0. };
    const std::vector<int> filament_map = {1, 1, 2};

    const std::vector<std::vector<float>> table = flush_matrix_for_changes(per_nozzle, 2, filament_map);
    REQUIRE(table.size() == 3);
    for (size_t from = 0; from < 3; ++from)
        for (size_t to = 0; to < 3; ++to) {
            INFO("from " << from << " to " << to);
            const size_t block = size_t(filament_map[to] - 1);
            CHECK(table[from][to] == float(per_nozzle[block * 9 + from * 3 + to]));
            CHECK(flush_volume_for_change(per_nozzle, 2, filament_map, from, to) == table[from][to]);
        }

    // The flush the printer does for a layer printed in `order` after filament 2: each nozzle flushes from what
    // it last held, by its own matrix.
    auto true_flush = [&](const std::vector<unsigned int> &order) {
        int   held[2] = {-1, 2};
        float total   = 0.f;
        for (unsigned int b : order) {
            const int e = filament_map[b] - 1;
            if (held[e] >= 0 && held[e] != int(b))
                total += float(per_nozzle[size_t(e) * 9 + size_t(held[e]) * 3 + b]);
            held[e] = int(b);
        }
        return total;
    };

    // Each nozzle's own matrix; the right nozzle holds filament 2, the left one is empty.
    std::vector<std::vector<std::vector<float>>> nozzle_flush(2, std::vector<std::vector<float>>(3, std::vector<float>(3)));
    for (size_t block = 0; block < 2; ++block)
        for (size_t i = 0; i < 9; ++i)
            nozzle_flush[block][i / 3][i % 3] = float(per_nozzle[block * 9 + i]);
    const std::vector<unsigned int> order = get_extruders_order_per_nozzle(nozzle_flush, filament_map, {0, 1, 2}, {-1, 2}, 2u);
    CHECK(order == std::vector<unsigned int>{2, 0, 1});
    CHECK(true_flush(order) == 290.f);
    // No order of the layer flushes less.
    std::vector<unsigned int> permutation = {0, 1, 2};
    do {
        INFO(permutation[0] << permutation[1] << permutation[2]);
        CHECK(true_flush(order) <= true_flush(permutation));
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    // A filament the map leaves off the matrices' nozzles: no per-nozzle answer, the caller keeps the old solver.
    CHECK(get_extruders_order_per_nozzle(nozzle_flush, {1, 1, 3}, {0, 1, 2}, {-1, 2}, 2u).empty());
    // The left nozzle holding filament 1: it goes on from 1 (1 -> 0 is 740, so 0 after 1 costs that; the solver keeps
    // 1 first since it is what the nozzle holds).
    CHECK(get_extruders_order_per_nozzle(nozzle_flush, filament_map, {0, 1}, {1, 2}, 1u) == std::vector<unsigned int>{1, 0});

    // The 18 values read as one 4 x 4 square, as before, order the layer 2, 1, 0: 740 mm3.
    std::vector<std::vector<float>> square(4, std::vector<float>(4));
    for (size_t i = 0; i < 16; ++i)
        square[i / 4][i % 4] = float(per_nozzle[i]);
    const std::vector<unsigned int> old_order = get_extruders_order(square, {0, 1, 2}, 2u);
    CHECK(old_order == std::vector<unsigned int>{2, 1, 0});
    CHECK(true_flush(old_order) == 740.f);
}

TEST_CASE("A single matrix reads the same in every per-change reader", "[FlushMatrix]")
{
    // One N x N block (the fork's own shape) and a too-short list keep exactly the floor(sqrt) square they had.
    for (const std::vector<double> &values : {numbered_blocks(3, 1), std::vector<double>{1., 2., 3., 4., 5., 6., 7.}}) {
        const size_t n = size_t(std::sqrt(double(values.size())));
        const std::vector<std::vector<float>> table = flush_matrix_for_changes(values, 2, {2, 1, 2});
        REQUIRE(table.size() == n);
        for (size_t from = 0; from < n; ++from)
            for (size_t to = 0; to < n; ++to)
                CHECK(table[from][to] == float(values[from * n + to]));
    }
}
