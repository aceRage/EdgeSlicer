#ifndef slic3r_LayerTimeSpeedSmoothing_hpp_
#define slic3r_LayerTimeSpeedSmoothing_hpp_

#include <cstddef>
#include <vector>

namespace Slic3r {

// Pure layer-time solvers for Edge layer-time speed smoothing.
//
// Adjacent layers are constrained to a relative variation band v:
//
//     t[i] >= t[i-1] * (1 - v)    and    t[i-1] >= t[i] * (1 - v)
//
// equivalently, neither layer may be more than v shorter than its neighbour.
//
// Mode A/B (speed-up) is one-sided: only long layers are shortened so they enter
// the neighbour band. Short layers are never lengthened. Each layer's time is
// floored at original / (1 + max_speedup), so speed factors stay in [1, 1+max_speedup].
//
// Mode C (slowdown) is the dual: only short layers are lengthened. Each layer is
// capped at original * (1 + max_slowdown). If the summed time growth would exceed
// max_time_increase, the variation limit is relaxed by bisection until the budget
// fits. Speed factors are <= 1.
//
// Slowdown scope (All vs ExcludeOuterWalls) and Mode A vs B (which extrusions to
// retune) are apply-side; these solvers are time-only.
//
// Layers before first_layer stay at their original times and do not constrain
// their neighbours (the first printed layer typically has its own speed).
//
// Mode C additionally takes a `frozen` mask: a frozen layer keeps its time (its cap is
// its own time) but still bounds its neighbours. The apply stage freezes every layer
// CoolingBuffer already stretched to slow_down_layer_time, so the two slowdowns never
// compound (CoolingBuffer's is a thermal floor, this one is a consistency band).
//
// Plan: 09-concept-layer-time-speed-smoothing.md
// Inspiration only: bambulab/BambuStudio#12224 (slowdown-only). Not a cherry-pick.
// G-code pipeline stage (S4 apply): GCode/LayerTimeSpeedSmoothingFilter.

struct LayerTimeSpeedSolveResult
{
    std::vector<double> times;
    // times_in[i] / times_out[i]. Mode A/B: >= 1. Mode C: <= 1.
    std::vector<double> speed_factors;
    // Variation actually enforced. Mode C may report a value larger than the
    // requested max_variation when the total-time budget forced a relax.
    double effective_max_variation = 0.;
};

struct LayerTimeSpeedUpParams
{
    // Maximum allowed relative change between adjacent layers, 0 .. 1.
    double max_variation = 0.25;
    // Maximum relative speed-up of a single layer (1.0 = 100% => factor 2, time halved).
    double max_speedup = 1.0;
};

struct LayerTimeSlowdownParams
{
    double max_variation = 0.25;
    // Maximum relative increase of a single layer's time (2.0 = 200% => up to 3x as long).
    double max_slowdown = 2.0;
    // Maximum relative increase of the summed times of the smoothed layers.
    double max_time_increase = 0.20;
};

// Reduce long-layer times toward the neighbour variation band. Never lengthens a layer.
LayerTimeSpeedSolveResult solve_layer_time_speed_up(
    const std::vector<double> &times,
    const LayerTimeSpeedUpParams &params,
    size_t first_layer = 1);

// Increase short-layer times toward the neighbour variation band. Never shortens a layer.
// frozen[i] == true keeps layer i at times[i] (shorter than times.size() means "none").
LayerTimeSpeedSolveResult solve_layer_time_slowdown(
    const std::vector<double> &times,
    const LayerTimeSlowdownParams &params,
    size_t first_layer = 1,
    const std::vector<bool> &frozen = {});

} // namespace Slic3r

#endif // slic3r_LayerTimeSpeedSmoothing_hpp_
