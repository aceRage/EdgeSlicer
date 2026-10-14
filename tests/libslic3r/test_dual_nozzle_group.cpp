#include <catch2/catch.hpp>

#include "libslic3r/FilamentGroup.hpp"
#include "libslic3r/FilamentGroupUtils.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"

using namespace Slic3r;

// The dual-nozzle filament->nozzle grouper (H2D / H2C / X2D) is reached for any print with two or
// more filaments in use. Its inputs are assembled by ToolOrdering::build_filament_group_context,
// which sizes every per-filament structure -- the per-nozzle flush matrices above all -- from
// filament_colour.size(), and then indexes them by real filament id. When the config handed to the
// engine carried fewer filament_colour entries than filaments (which the CLI did until
// src/Snapmaker_Orca.cpp padded the list), the matrices came out too small and
// FilamentGroup::calc_one_change_budget read past the end of flush_matrix[nozzle] -- an access
// violation, not a slicing error.
//
// These cases pin the shape the grouper is entitled to assume, so a context built for F filaments
// on E nozzles keeps F x F matrices and the grouping walk over them stays in bounds.

namespace {

// One logical nozzle per extruder, the H2D shape.
MultiNozzleUtils::NozzleInfo make_nozzle(int extruder_id)
{
    MultiNozzleUtils::NozzleInfo n;
    n.diameter    = "0.4";
    n.volume_type = NozzleVolumeType::nvtStandard;
    n.extruder_id = extruder_id;
    n.group_id    = extruder_id;
    return n;
}

// The context ToolOrdering builds for `filament_num` filaments on `nozzle_num` nozzles, with the
// flush matrices correctly dimensioned filament_num x filament_num.
FilamentGroupContext make_context(int filament_num, int nozzle_num)
{
    FilamentGroupContext ctx;

    for (int nozzle = 0; nozzle < nozzle_num; ++nozzle) {
        FlushMatrix matrix(filament_num, std::vector<float>(filament_num, 0.f));
        for (int from = 0; from < filament_num; ++from)
            for (int to = 0; to < filament_num; ++to)
                if (from != to)
                    matrix[from][to] = 280.f;
        ctx.model_info.flush_matrix.emplace_back(std::move(matrix));
    }

    std::vector<unsigned int> layer;
    for (int f = 0; f < filament_num; ++f)
        layer.emplace_back((unsigned int) f);
    ctx.model_info.layer_filaments.emplace_back(layer);

    static const char *colors[] = { "#FFFFFF", "#000000", "#FF0000", "#00FF00" };
    for (int f = 0; f < filament_num; ++f) {
        FilamentGroupUtils::FilamentInfo info;
        info.color      = FilamentGroupUtils::Color(std::string(colors[f % 4]));
        info.type       = "PLA";
        info.is_support = false;
        info.usage_type = FilamentUsageType::ModelOnly;
        ctx.model_info.filament_info.emplace_back(info);
    }
    ctx.model_info.filament_ids.resize(filament_num);
    ctx.model_info.unprintable_filaments.assign(nozzle_num, std::set<int>());

    ctx.group_info.total_filament_num  = filament_num;
    ctx.group_info.max_gap_threshold   = 0.01;
    ctx.group_info.mode                = FGMode::FlushMode;
    ctx.group_info.strategy            = FGStrategy::BestCost;
    ctx.group_info.ignore_ext_filament = false;
    ctx.group_info.filament_volume_map.assign(filament_num, (int) NozzleVolumeType::nvtHybrid);

    // What calc_max_group_size returns with no AMS data, which is the CLI's case.
    ctx.machine_info.max_group_size.assign(nozzle_num, 1);
    ctx.machine_info.prefer_non_model_filament.assign(nozzle_num, false);
    ctx.machine_info.master_extruder_id = nozzle_num - 1;

    for (int nozzle = 0; nozzle < nozzle_num; ++nozzle) {
        ctx.nozzle_info.nozzle_list.emplace_back(make_nozzle(nozzle));
        ctx.nozzle_info.extruder_nozzle_list[nozzle] = { nozzle };
    }

    return ctx;
}

} // namespace

TEST_CASE("A dual-nozzle context keeps one flush matrix row per filament", "[FilamentGroup][MultiNozzle]")
{
    const FilamentGroupContext ctx = make_context(2, 2);

    REQUIRE(ctx.model_info.flush_matrix.size() == 2);
    for (const auto &matrix : ctx.model_info.flush_matrix) {
        REQUIRE((int) matrix.size() == ctx.group_info.total_filament_num);
        for (const auto &row : matrix)
            REQUIRE((int) row.size() == ctx.group_info.total_filament_num);
    }

    // Every filament the layers use has to be addressable in every matrix; a short filament_colour
    // list used to break exactly this, and calc_one_change_budget read off the end.
    const auto used_filaments = collect_sorted_used_filaments(ctx.model_info.layer_filaments);
    REQUIRE(used_filaments.size() == 2);
    for (const auto &matrix : ctx.model_info.flush_matrix)
        for (unsigned int filament : used_filaments)
            REQUIRE(filament < matrix.size());
}

TEST_CASE("Two filaments group onto a dual-nozzle machine's two nozzles", "[FilamentGroup][MultiNozzle]")
{
    FilamentGroupContext ctx = make_context(2, 2);

    FilamentGroup group(ctx);
    const std::vector<int> filament_map = group.calc_filament_group();

    REQUIRE(filament_map.size() == 2);
    for (int nozzle : filament_map) {
        REQUIRE(nozzle >= 0);
        REQUIRE(nozzle < 2);
    }
    // One slot per nozzle (max_group_size 1,1) leaves the two filaments on different nozzles.
    REQUIRE(filament_map[0] != filament_map[1]);
}

TEST_CASE("Four filaments group onto a dual-nozzle machine without leaving the matrices", "[FilamentGroup][MultiNozzle]")
{
    FilamentGroupContext ctx = make_context(4, 2);
    ctx.machine_info.max_group_size.assign(2, 4);

    FilamentGroup group(ctx);
    const std::vector<int> filament_map = group.calc_filament_group();

    REQUIRE(filament_map.size() == 4);
    for (int nozzle : filament_map) {
        REQUIRE(nozzle >= 0);
        REQUIRE(nozzle < 2);
    }
}

// Orca #14773: the k-medoids grouping is an anytime search under a wall clock, so its result can depend on how
// many restarts the machine completes. ClusteringBudget lets a caller bound it by restart count alone; with the
// clock off, the same input must give the same grouping every time. Slicing keeps the default budget.
TEST_CASE("k-medoids filament grouping is deterministic under a fixed search budget", "[FilamentGroup][MultiNozzle][Orca14773]")
{
    CHECK(ClusteringBudget{}.timeout_ms == 3000);
    CHECK(ClusteringBudget{}.max_restarts == 30);

    // Sixteen filaments on two nozzles: 2^16 / 2 candidate groupings, past the enumeration threshold.
    const int filament_num = 16;
    FilamentGroupContext ctx = make_context(filament_num, 2);
    ctx.machine_info.max_group_size.assign(2, filament_num);
    for (auto &matrix : ctx.model_info.flush_matrix)
        for (int from = 0; from < filament_num; ++from)
            for (int to = 0; to < filament_num; ++to)
                if (from != to)
                    matrix[from][to] = 100.f + float((from * 7 + to * 13) % 17) * 20.f;
    ctx.model_info.layer_filaments.clear();
    for (int layer = 0; layer < 12; ++layer) {
        std::vector<unsigned int> filaments;
        for (int f = 0; f < filament_num; ++f)
            if ((f + layer) % 3 != 0)
                filaments.emplace_back((unsigned int) f);
        ctx.model_info.layer_filaments.emplace_back(std::move(filaments));
    }

    const ClusteringBudget fixed_budget{ /*timeout_ms*/ 0, /*max_restarts*/ 4 };
    auto run = [&ctx, &fixed_budget](int &cost) {
        FilamentGroup group(ctx);
        group.set_clustering_budget(fixed_budget);
        return group.calc_filament_group(&cost);
    };
    int cost_first = 0, cost_second = 0;
    const std::vector<int> first  = run(cost_first);
    const std::vector<int> second = run(cost_second);

    REQUIRE(first.size() == size_t(filament_num));
    for (int nozzle : first) {
        REQUIRE(nozzle >= 0);
        REQUIRE(nozzle < 2);
    }
    CHECK(first == second);
    CHECK(cost_first == cost_second);
}
