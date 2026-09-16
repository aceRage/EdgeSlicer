#include "LayerTimeSpeedSmoothing.hpp"

#include <algorithm>
#include <utility>

namespace Slic3r {

// Times below this delta (seconds) are treated as unchanged.
static constexpr double LTSS_EPS = 1e-4;

static std::vector<double> speed_factors_from(const std::vector<double> &original, const std::vector<double> &solved)
{
    std::vector<double> factors(original.size(), 1.0);
    for (size_t i = 0; i < original.size(); ++i)
        factors[i] = (solved[i] > 0.0) ? original[i] / solved[i] : 1.0;
    return factors;
}

static double sum_from(const std::vector<double> &times, size_t first_layer)
{
    double total = 0.0;
    for (size_t i = first_layer; i < times.size(); ++i)
        total += times[i];
    return total;
}

// Neighbour band: a layer at `time` allows a neighbour in [time * ratio, time / ratio]
// where ratio = 1 - v. ratio <= 0 means "no constraint".
static double neighbour_lower(double time, double ratio) { return time * ratio; }
static double neighbour_upper(double time, double ratio) { return time / ratio; }

// Pull long layers down onto the upper edge of each neighbour's band, without
// going below `floor_time`. Forward then reverse until the curve stops moving.
static std::vector<double> shorten_long_layers(
    const std::vector<double> &times,
    const std::vector<double> &floor_time,
    double                     max_variation,
    size_t                     first_layer)
{
    std::vector<double> t = times;
    const size_t        n = t.size();
    if (n < 2 || first_layer + 1 >= n)
        return t;

    const double ratio = 1.0 - std::clamp(max_variation, 0.0, 1.0);
    if (ratio <= 0.0)
        return t;

    auto try_shorten = [&](size_t i, double upper_from_neighbour) {
        if (i < first_layer)
            return false;
        const double target = std::max(floor_time[i], upper_from_neighbour);
        if (t[i] > target + LTSS_EPS) {
            t[i] = target;
            return true;
        }
        return false;
    };

    for (int sweep = 0; sweep < 64; ++sweep) {
        bool changed = false;
        for (size_t i = first_layer + 1; i < n; ++i)
            changed |= try_shorten(i, neighbour_upper(t[i - 1], ratio));
        for (size_t i = n - 1; i > first_layer; --i)
            changed |= try_shorten(i - 1, neighbour_upper(t[i], ratio));
        if (!changed)
            break;
    }
    return t;
}

// Lift short layers up onto the lower edge of each neighbour's band, without
// going above `cap_time`. Dual of shorten_long_layers.
static std::vector<double> lengthen_short_layers(
    const std::vector<double> &times,
    const std::vector<double> &cap_time,
    double                     max_variation,
    size_t                     first_layer)
{
    std::vector<double> t = times;
    const size_t        n = t.size();
    if (n < 2 || first_layer + 1 >= n)
        return t;

    const double ratio = 1.0 - std::clamp(max_variation, 0.0, 1.0);
    if (ratio <= 0.0)
        return t;

    auto try_lengthen = [&](size_t i, double lower_from_neighbour) {
        if (i < first_layer)
            return false;
        const double target = std::min(cap_time[i], lower_from_neighbour);
        if (target > t[i] + LTSS_EPS) {
            t[i] = target;
            return true;
        }
        return false;
    };

    for (int sweep = 0; sweep < 64; ++sweep) {
        bool changed = false;
        for (size_t i = first_layer + 1; i < n; ++i)
            changed |= try_lengthen(i, neighbour_lower(t[i - 1], ratio));
        for (size_t i = n - 1; i > first_layer; --i)
            changed |= try_lengthen(i - 1, neighbour_lower(t[i], ratio));
        if (!changed)
            break;
    }
    return t;
}

LayerTimeSpeedSolveResult solve_layer_time_speed_up(
    const std::vector<double> &times,
    const LayerTimeSpeedUpParams &params,
    size_t first_layer)
{
    LayerTimeSpeedSolveResult out;
    out.times                   = times;
    out.effective_max_variation = std::clamp(params.max_variation, 0.0, 1.0);
    out.speed_factors.assign(times.size(), 1.0);
    if (times.size() < 2)
        return out;

    const double max_factor = 1.0 + std::max(0.0, params.max_speedup);
    std::vector<double> floor_time(times.size());
    for (size_t i = 0; i < times.size(); ++i) {
        if (i < first_layer || max_factor <= 1.0)
            floor_time[i] = times[i];
        else
            floor_time[i] = times[i] / max_factor;
    }

    out.times         = shorten_long_layers(times, floor_time, out.effective_max_variation, first_layer);
    out.speed_factors = speed_factors_from(times, out.times);
    return out;
}

LayerTimeSpeedSolveResult solve_layer_time_slowdown(
    const std::vector<double> &times,
    const LayerTimeSlowdownParams &params,
    size_t first_layer,
    const std::vector<bool> &frozen)
{
    LayerTimeSpeedSolveResult out;
    out.times                   = times;
    out.effective_max_variation = std::clamp(params.max_variation, 0.0, 1.0);
    out.speed_factors.assign(times.size(), 1.0);
    if (times.size() < 2)
        return out;

    const double max_stretch = 1.0 + std::max(0.0, params.max_slowdown);
    std::vector<double> cap_time(times.size());
    for (size_t i = 0; i < times.size(); ++i) {
        // A frozen layer's cap is its own time: try_lengthen() then never moves it, while
        // its (unchanged) time still bounds the neighbours through neighbour_lower().
        if (i < first_layer || (i < frozen.size() && frozen[i]))
            cap_time[i] = times[i];
        else
            cap_time[i] = times[i] * max_stretch;
    }

    std::vector<double> solved = lengthen_short_layers(times, cap_time, out.effective_max_variation, first_layer);

    const double base     = sum_from(times, first_layer);
    const double extra    = sum_from(solved, first_layer) - base;
    const double budget   = std::max(0.0, params.max_time_increase) * base + LTSS_EPS;
    if (extra > budget) {
        // Extra time shrinks as the allowed variation grows (v = 1 disables the constraint),
        // so the smallest v that fits the budget is found by bisection.
        double              lo   = out.effective_max_variation;
        double              hi   = 1.0;
        std::vector<double> best = times;
        for (int iter = 0; iter < 32 && (hi - lo) > 1e-4; ++iter) {
            const double              mid       = 0.5 * (lo + hi);
            const std::vector<double> candidate = lengthen_short_layers(times, cap_time, mid, first_layer);
            if (sum_from(candidate, first_layer) - base <= budget) {
                hi   = mid;
                best = candidate;
            } else {
                lo = mid;
            }
        }
        solved                      = std::move(best);
        out.effective_max_variation = hi;
    }

    out.times         = std::move(solved);
    out.speed_factors = speed_factors_from(times, out.times);
    return out;
}

} // namespace Slic3r
