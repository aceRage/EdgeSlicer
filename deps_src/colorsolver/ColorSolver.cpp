// ColorSolver
// Copyright (C) 2026 sentientstardust

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// See ColorSolver.hpp and NOTICE for what this vendored copy keeps and drops.

#include "ColorSolver.hpp"

// The fork's own mixer.  Header-only, MIT, no data files.  This replaces
// upstream's pigment-painter / prusa-fdm-mixer pair.
#include "libslic3r/filament_mixer_model.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace Slic3r {
namespace {

// Oklab soft-cap mode constants (upstream values, unchanged).
constexpr float OKLAB_SOFT_CAP4_DARK4_MIN_L_WEIGHT = 1.f;
constexpr float OKLAB_SOFT_CAP4_DARK4_MAX_AB_WEIGHT = 4.f;
constexpr float OKLAB_SOFT_CAP4_DARK4_DARK_PENALTY = 4.f;
constexpr float OKLAB_SOFT_CAP4_DARK4_DARK_TOLERANCE = 0.04f;

float clamp01(float value)
{
    if (!std::isfinite(value))
        return 0.f;
    return std::clamp(value, 0.f, 1.f);
}

float srgb_to_linear_component(float value)
{
    const float x = clamp01(value);
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb_component(float value)
{
    const float x = clamp01(value);
    return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.f / 2.4f) - 0.055f;
}

unsigned char to_byte(float value)
{
    return static_cast<unsigned char>(std::clamp(int(std::lround(clamp01(value) * 255.f)), 0, 255));
}

// The fork's mix, chained pairwise with an accumulating weight.  This is a
// transcription of MixedFilamentManager::blend_color_multi (MixedFilament.cpp)
// and of blend_multi_filament_mixer (MixedFilamentColorMapPanel.cpp), which are
// themselves the same loop: skip non-positive weights, seed from the first
// surviving colour, then lerp each further colour in at t = w / (accumulated+w).
// Keeping the order and the accumulation identical is what makes the solver's
// prediction and the sidebar chip's display colour the same number.
std::array<float, 3> mix_fork_components(const std::vector<std::array<float, 3>> &colors,
                                         const std::vector<float>                &weights)
{
    const size_t count = std::min(colors.size(), weights.size());
    if (count == 0)
        return { 0.f, 0.f, 0.f };

    size_t        first = count;
    unsigned char r = 0, g = 0, b = 0;
    double        accumulated = 0.0;

    for (size_t idx = 0; idx < count; ++idx) {
        const float weight = weights[idx];
        if (!std::isfinite(weight) || weight <= 0.f)
            continue;
        if (first == count) {
            // Endpoint exactness: a single non-zero weight must give back the
            // component's own colour untouched, not a round trip through 8 bits.
            first       = idx;
            r           = to_byte(colors[idx][0]);
            g           = to_byte(colors[idx][1]);
            b           = to_byte(colors[idx][2]);
            accumulated = double(weight);
            continue;
        }
        const double new_total = accumulated + double(weight);
        if (new_total <= 0.0)
            continue;
        const float t = float(double(weight) / new_total);
        filament_mixer::lerp(r, g, b,
                             to_byte(colors[idx][0]), to_byte(colors[idx][1]), to_byte(colors[idx][2]),
                             t, &r, &g, &b);
        accumulated = new_total;
    }

    if (first == count)
        return { 0.f, 0.f, 0.f };
    // Exactly one component contributed - hand back the input, unquantised.
    bool single = true;
    for (size_t idx = first + 1; idx < count; ++idx)
        if (std::isfinite(weights[idx]) && weights[idx] > 0.f) { single = false; break; }
    if (single)
        return { clamp01(colors[first][0]), clamp01(colors[first][1]), clamp01(colors[first][2]) };

    return { float(r) / 255.f, float(g) / 255.f, float(b) / 255.f };
}

std::array<float, 3> oklab_from_srgb(const std::array<float, 3> &rgb)
{
    const float r = srgb_to_linear_component(rgb[0]);
    const float g = srgb_to_linear_component(rgb[1]);
    const float b = srgb_to_linear_component(rgb[2]);

    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);

    return {
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s
    };
}

std::array<float, 3> srgb_from_oklab(const std::array<float, 3> &oklab)
{
    const float l_ = oklab[0] + 0.3963377774f * oklab[1] + 0.2158037573f * oklab[2];
    const float m_ = oklab[0] - 0.1055613458f * oklab[1] - 0.0638541728f * oklab[2];
    const float s_ = oklab[0] - 0.0894841775f * oklab[1] - 1.2914855480f * oklab[2];

    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;

    const float r = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    const float g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    const float b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;

    return {
        linear_to_srgb_component(r),
        linear_to_srgb_component(g),
        linear_to_srgb_component(b)
    };
}

float color_solver_oklab_chroma_factor(const std::array<float, 3> &target_oklab)
{
    const float chroma = std::hypot(target_oklab[1], target_oklab[2]);
    return std::clamp((chroma - 0.015f) / 0.13f, 0.f, 1.f);
}

std::array<float, 3> color_solver_oklab_axis_weights(const std::array<float, 3> &target_oklab)
{
    const float chroma_factor = color_solver_oklab_chroma_factor(target_oklab);
    return {
        1.f + (0.25f - 1.f) * chroma_factor,
        1.25f + (8.f - 1.25f) * chroma_factor,
        1.25f + (8.f - 1.25f) * chroma_factor
    };
}

std::array<float, 3> color_solver_perceptual_axis_weights(const std::array<float, 3> &target_oklab,
                                                          ColorSolverMode             solver_mode)
{
    std::array<float, 3> weights = color_solver_oklab_axis_weights(target_oklab);
    if (solver_mode == ColorSolverMode::OklabSoftCap4Dark4) {
        weights[0] = std::max(weights[0], OKLAB_SOFT_CAP4_DARK4_MIN_L_WEIGHT);
        weights[1] = std::min(weights[1], OKLAB_SOFT_CAP4_DARK4_MAX_AB_WEIGHT);
        weights[2] = std::min(weights[2], OKLAB_SOFT_CAP4_DARK4_MAX_AB_WEIGHT);
    }
    return weights;
}

bool color_solver_mode_is_perceptual(ColorSolverMode solver_mode)
{
    return solver_mode == ColorSolverMode::Oklab || solver_mode == ColorSolverMode::OklabSoftCap4Dark4;
}

// ---- kd-tree ----

int build_color_solver_kd_tree(const std::vector<float>                    &coords,
                               std::vector<ColorSolverCandidateSet::KdNode> &nodes,
                               std::vector<uint32_t>                       &indices,
                               size_t                                       begin,
                               size_t                                       end,
                               uint8_t                                      axis)
{
    if (begin >= end)
        return -1;

    const size_t mid = begin + (end - begin) / 2;
    auto axis_value = [&coords, axis](uint32_t candidate_idx) {
        return coords[size_t(candidate_idx) * 3 + size_t(axis)];
    };
    std::nth_element(indices.begin() + begin,
                     indices.begin() + mid,
                     indices.begin() + end,
                     [&axis_value](uint32_t lhs, uint32_t rhs) {
                         return axis_value(lhs) < axis_value(rhs);
                     });

    const int node_idx = int(nodes.size());
    ColorSolverCandidateSet::KdNode node;
    node.candidate_idx = indices[mid];
    node.axis = axis;
    nodes.emplace_back(node);

    const uint8_t next_axis = uint8_t((axis + 1) % 3);
    const int left = build_color_solver_kd_tree(coords, nodes, indices, begin, mid, next_axis);
    const int right = build_color_solver_kd_tree(coords, nodes, indices, mid + 1, end, next_axis);
    nodes[size_t(node_idx)].left = left;
    nodes[size_t(node_idx)].right = right;
    return node_idx;
}

int build_color_solver_kd_tree(const std::vector<float>                    &coords,
                               std::vector<ColorSolverCandidateSet::KdNode> &nodes)
{
    const size_t candidate_count = coords.size() / 3;
    nodes.clear();
    if (candidate_count == 0)
        return -1;

    std::vector<uint32_t> indices(candidate_count, 0);
    for (size_t idx = 0; idx < candidate_count; ++idx)
        indices[idx] = uint32_t(idx);

    nodes.reserve(candidate_count);
    return build_color_solver_kd_tree(coords, nodes, indices, 0, candidate_count, uint8_t(0));
}

void build_color_solver_kd_trees(ColorSolverCandidateSet &candidates)
{
    candidates.kd_root = build_color_solver_kd_tree(candidates.rgbs, candidates.kd_nodes);
    if (candidates.perceptual_coords.size() == candidates.rgbs.size()) {
        candidates.perceptual_kd_root =
            build_color_solver_kd_tree(candidates.perceptual_coords, candidates.perceptual_kd_nodes);
    } else {
        candidates.perceptual_kd_nodes.clear();
        candidates.perceptual_kd_root = -1;
    }
}

// ---- nearest search ----

struct ColorSolverNearestResult {
    size_t best_idx { size_t(-1) };
    size_t second_idx { size_t(-1) };
    float  best_error { std::numeric_limits<float>::max() };
    float  second_error { std::numeric_limits<float>::max() };
};

void update_color_solver_nearest_result(ColorSolverNearestResult &result, size_t candidate_idx, float error)
{
    if (candidate_idx == result.best_idx || candidate_idx == result.second_idx)
        return;

    if (error < result.best_error) {
        result.second_error = result.best_error;
        result.second_idx = result.best_idx;
        result.best_error = error;
        result.best_idx = candidate_idx;
    } else if (error < result.second_error) {
        result.second_error = error;
        result.second_idx = candidate_idx;
    }
}

float color_solver_candidate_error(const ColorSolverCandidateSet &candidates,
                                   size_t                         candidate_idx,
                                   const std::array<float, 3>    &target_rgb)
{
    const size_t rgb_idx = candidate_idx * 3;
    const float dr = candidates.rgbs[rgb_idx + 0] - target_rgb[0];
    const float dg = candidates.rgbs[rgb_idx + 1] - target_rgb[1];
    const float db = candidates.rgbs[rgb_idx + 2] - target_rgb[2];
    return dr * dr + dg * dg + db * db;
}

ColorSolverNearestResult nearest_color_solver_candidates_linear(const ColorSolverCandidateSet &candidates,
                                                                const std::array<float, 3>    &target_rgb)
{
    ColorSolverNearestResult result;
    const size_t candidate_count = candidates.candidate_count();
    for (size_t candidate_idx = 0; candidate_idx < candidate_count; ++candidate_idx)
        update_color_solver_nearest_result(result, candidate_idx,
                                           color_solver_candidate_error(candidates, candidate_idx, target_rgb));
    return result;
}

void query_color_solver_kd_tree(const ColorSolverCandidateSet &candidates,
                                const std::array<float, 3>    &target_rgb,
                                int                            node_idx,
                                ColorSolverNearestResult      &result)
{
    if (node_idx < 0 || size_t(node_idx) >= candidates.kd_nodes.size())
        return;

    const size_t candidate_count = candidates.candidate_count();
    const ColorSolverCandidateSet::KdNode &node = candidates.kd_nodes[size_t(node_idx)];
    if (size_t(node.candidate_idx) >= candidate_count) {
        query_color_solver_kd_tree(candidates, target_rgb, node.left, result);
        query_color_solver_kd_tree(candidates, target_rgb, node.right, result);
        return;
    }

    update_color_solver_nearest_result(result, size_t(node.candidate_idx),
                                       color_solver_candidate_error(candidates, size_t(node.candidate_idx), target_rgb));

    const size_t rgb_idx = size_t(node.candidate_idx) * 3;
    const size_t axis = std::min<size_t>(node.axis, 2);
    const float split_delta = target_rgb[axis] - candidates.rgbs[rgb_idx + axis];
    const int near_node = split_delta <= 0.f ? node.left : node.right;
    const int far_node = split_delta <= 0.f ? node.right : node.left;

    query_color_solver_kd_tree(candidates, target_rgb, near_node, result);
    // Pruning is against second_error, not best_error, because the caller may
    // want the runner-up too (BlendClosestTwo).
    if (split_delta * split_delta <= result.second_error)
        query_color_solver_kd_tree(candidates, target_rgb, far_node, result);
}

ColorSolverNearestResult nearest_color_solver_candidates(const ColorSolverCandidateSet &candidates,
                                                         const std::array<float, 3>    &target_rgb)
{
    ColorSolverNearestResult result;
    if (candidates.kd_root >= 0 && !candidates.kd_nodes.empty())
        query_color_solver_kd_tree(candidates, target_rgb, candidates.kd_root, result);
    if (result.best_idx >= candidates.candidate_count())
        result = nearest_color_solver_candidates_linear(candidates, target_rgb);
    return result;
}

float color_solver_candidate_perceptual_error(const ColorSolverCandidateSet &candidates,
                                              size_t                         candidate_idx,
                                              const std::array<float, 3>    &target_oklab,
                                              const std::array<float, 3>    &axis_weights,
                                              ColorSolverMode                solver_mode)
{
    const size_t coord_idx = candidate_idx * 3;
    const float dl = candidates.perceptual_coords[coord_idx + 0] - target_oklab[0];
    const float da = candidates.perceptual_coords[coord_idx + 1] - target_oklab[1];
    const float db = candidates.perceptual_coords[coord_idx + 2] - target_oklab[2];
    float error = axis_weights[0] * dl * dl + axis_weights[1] * da * da + axis_weights[2] * db * db;
    if (solver_mode == ColorSolverMode::OklabSoftCap4Dark4) {
        const float under_l = std::max(0.f, target_oklab[0] - candidates.perceptual_coords[coord_idx + 0] -
                                                OKLAB_SOFT_CAP4_DARK4_DARK_TOLERANCE);
        error += OKLAB_SOFT_CAP4_DARK4_DARK_PENALTY * color_solver_oklab_chroma_factor(target_oklab) * under_l * under_l;
    }
    return error;
}

ColorSolverNearestResult nearest_color_solver_candidates_perceptual_linear(const ColorSolverCandidateSet &candidates,
                                                                          const std::array<float, 3>    &target_oklab,
                                                                          const std::array<float, 3>    &axis_weights,
                                                                          ColorSolverMode                solver_mode)
{
    ColorSolverNearestResult result;
    const size_t candidate_count = candidates.perceptual_coords.size() / 3;
    for (size_t candidate_idx = 0; candidate_idx < candidate_count; ++candidate_idx)
        update_color_solver_nearest_result(
            result, candidate_idx,
            color_solver_candidate_perceptual_error(candidates, candidate_idx, target_oklab, axis_weights, solver_mode));
    return result;
}

void query_color_solver_perceptual_kd_tree(const ColorSolverCandidateSet &candidates,
                                           const std::array<float, 3>    &target_oklab,
                                           const std::array<float, 3>    &axis_weights,
                                           ColorSolverMode                solver_mode,
                                           int                            node_idx,
                                           ColorSolverNearestResult      &result)
{
    if (node_idx < 0 || size_t(node_idx) >= candidates.perceptual_kd_nodes.size())
        return;

    const size_t candidate_count = candidates.perceptual_coords.size() / 3;
    const ColorSolverCandidateSet::KdNode &node = candidates.perceptual_kd_nodes[size_t(node_idx)];
    if (size_t(node.candidate_idx) >= candidate_count) {
        query_color_solver_perceptual_kd_tree(candidates, target_oklab, axis_weights, solver_mode, node.left, result);
        query_color_solver_perceptual_kd_tree(candidates, target_oklab, axis_weights, solver_mode, node.right, result);
        return;
    }

    update_color_solver_nearest_result(
        result, size_t(node.candidate_idx),
        color_solver_candidate_perceptual_error(candidates, size_t(node.candidate_idx), target_oklab, axis_weights, solver_mode));

    const size_t coord_idx = size_t(node.candidate_idx) * 3;
    const size_t axis = std::min<size_t>(node.axis, 2);
    const float split_delta = target_oklab[axis] - candidates.perceptual_coords[coord_idx + axis];
    const int near_node = split_delta <= 0.f ? node.left : node.right;
    const int far_node = split_delta <= 0.f ? node.right : node.left;

    query_color_solver_perceptual_kd_tree(candidates, target_oklab, axis_weights, solver_mode, near_node, result);
    // The soft-cap mode adds a one-sided penalty on top of the weighted metric.
    // The penalty is non-negative, so this axis bound still never prunes a
    // candidate that could win.
    if (axis_weights[axis] * split_delta * split_delta <= result.second_error)
        query_color_solver_perceptual_kd_tree(candidates, target_oklab, axis_weights, solver_mode, far_node, result);
}

ColorSolverNearestResult nearest_color_solver_candidates_perceptual(const ColorSolverCandidateSet &candidates,
                                                                    const std::array<float, 3>    &target_rgb,
                                                                    ColorSolverMode                solver_mode)
{
    ColorSolverNearestResult result;
    const size_t candidate_count = candidates.perceptual_coords.size() / 3;
    if (candidate_count == 0 || candidates.perceptual_coords.size() != candidates.rgbs.size())
        return result;

    const std::array<float, 3> target_oklab = oklab_from_srgb(target_rgb);
    const std::array<float, 3> axis_weights = color_solver_perceptual_axis_weights(target_oklab, solver_mode);
    if (candidates.perceptual_kd_root >= 0 && !candidates.perceptual_kd_nodes.empty())
        query_color_solver_perceptual_kd_tree(candidates, target_oklab, axis_weights, solver_mode,
                                              candidates.perceptual_kd_root, result);
    if (result.best_idx >= candidate_count)
        result = nearest_color_solver_candidates_perceptual_linear(candidates, target_oklab, axis_weights, solver_mode);
    return result;
}

// Clamp the caller's constraints to a window that can never enumerate to
// nothing for a two-or-more component set: the same [0,50] / [50,100] window
// build_best_color_match_recipe already enforces on its own inputs.
ColorSolverConstraints sanitize_constraints(const ColorSolverConstraints &constraints, size_t component_count)
{
    ColorSolverConstraints out;
    out.min_component_percent = std::clamp(constraints.min_component_percent, 0, 50);
    out.max_component_percent = std::clamp(constraints.max_component_percent, 50, 100);
    out.max_components = constraints.max_components == 0 ? component_count
                                                         : std::min(constraints.max_components, component_count);
    if (out.max_components == 0)
        out.max_components = component_count;
    out.min_components = std::clamp<size_t>(constraints.min_components == 0 ? 1 : constraints.min_components,
                                            1, out.max_components);
    if (constraints.allowed_pairs.size() == component_count * component_count)
        out.allowed_pairs = constraints.allowed_pairs;
    return out;
}

// May components `idx` and every already-used component be mixed together?
bool pair_allowed(const ColorSolverConstraints &sane, size_t component_count,
                  const std::vector<size_t> &used, size_t idx)
{
    if (sane.allowed_pairs.empty())
        return true;
    for (const size_t other : used)
        if (sane.allowed_pairs[other * component_count + idx] == 0)
            return false;
    return true;
}

// Upper bound on the number of unit vectors the constrained enumeration below
// will emit, counted by dynamic programming so the caller never has to run it to
// find out.  Upstream had no constraints and could use a closed-form binomial;
// with a per-component window and a cap on how many components may be non-zero
// there is no closed form, and getting this wrong means either a wasted reserve
// or an enumeration that allocates gigabytes.  allowed_pairs is deliberately
// ignored here - it can only ever remove candidates, so the bound stays a bound.
size_t count_constrained_candidates(size_t component_count, int total_units, int min_units, int max_units, size_t max_components)
{
    if (component_count == 0 || total_units <= 0)
        return 0;
    const size_t units = size_t(total_units) + 1;
    const size_t caps  = max_components + 1;
    // dp[r][u]: ways to distribute r remaining units over the components seen so
    // far having used u of them.  Rolled forward one component at a time.
    std::vector<size_t> dp(units * caps, 0), next(units * caps, 0);
    dp[0 * caps + 0] = 1;
    constexpr size_t k_overflow = size_t(-1) / 4;
    for (size_t component = 0; component < component_count; ++component) {
        std::fill(next.begin(), next.end(), size_t(0));
        for (size_t r = 0; r < units; ++r)
            for (size_t u = 0; u < caps; ++u) {
                const size_t ways = dp[r * caps + u];
                if (ways == 0)
                    continue;
                next[r * caps + u] += ways;                      // component unused
                if (u + 1 >= caps)
                    continue;
                const int hi = std::min<int>(max_units, int(total_units - int(r)));
                for (int unit = std::max(1, min_units); unit <= hi; ++unit) {
                    size_t &slot = next[(r + size_t(unit)) * caps + u + 1];
                    slot += ways;
                    if (slot > k_overflow)
                        return k_overflow;
                }
            }
        dp.swap(next);
    }
    size_t total = 0;
    for (size_t u = 1; u < caps; ++u)
        total += dp[size_t(total_units) * caps + u];
    return total;
}

ColorSolverNearestResult solve_nearest(const ColorSolverCandidateSet &candidates,
                                       const std::array<float, 3>    &target_rgb,
                                       ColorSolverMode                solver_mode)
{
    ColorSolverNearestResult nearest =
        color_solver_mode_is_perceptual(solver_mode) ?
            nearest_color_solver_candidates_perceptual(candidates, target_rgb, solver_mode) :
            nearest_color_solver_candidates(candidates, target_rgb);
    if (nearest.best_idx >= candidates.candidate_count() && color_solver_mode_is_perceptual(solver_mode))
        nearest = nearest_color_solver_candidates(candidates, target_rgb);
    return nearest;
}

} // namespace

ColorSolverLookupMode color_solver_lookup_mode_from_index(int mode)
{
    return ColorSolverLookupMode(std::clamp(mode, int(ColorSolverLookupMode::ClosestMix), int(ColorSolverLookupMode::BlendClosestTwo)));
}

ColorSolverMode color_solver_mode_from_index(int mode)
{
    return ColorSolverMode(std::clamp(mode, int(ColorSolverMode::RGB), int(ColorSolverMode::OklabSoftCap4Dark4)));
}

int color_solver_total_units_for_component_count(size_t component_count)
{
    return component_count <= 4 ? 40 : (component_count == 5 ? 24 : (component_count == 6 ? 20 : 12));
}

size_t color_solver_candidate_count(size_t component_count, int total_units)
{
    if (component_count == 0 || total_units < 0)
        return 0;

    const size_t n = size_t(total_units) + component_count - 1;
    size_t k = component_count - 1;
    k = std::min(k, n - k);

    size_t result = 1;
    for (size_t idx = 1; idx <= k; ++idx)
        result = (result * (n - k + idx)) / idx;
    return result;
}

std::array<float, 3> mix_color_solver_components(const std::vector<std::array<float, 3>> &component_colors,
                                                 const std::vector<int>                  &weights)
{
    std::vector<float> float_weights;
    float_weights.reserve(weights.size());
    for (const int weight : weights)
        float_weights.emplace_back(float(std::max(0, weight)));
    return mix_fork_components(component_colors, float_weights);
}

std::array<float, 3> mix_color_solver_components(const std::vector<std::array<float, 3>> &component_colors,
                                                 const std::vector<float>                &weights)
{
    std::vector<float> safe_weights;
    safe_weights.reserve(weights.size());
    for (const float weight : weights)
        safe_weights.emplace_back(std::isfinite(weight) && weight > 0.f ? weight : 0.f);
    return mix_fork_components(component_colors, safe_weights);
}

std::array<float, 3> color_solver_oklab_from_srgb(const std::array<float, 3> &rgb)
{
    return oklab_from_srgb(rgb);
}

std::array<float, 3> color_solver_srgb_from_oklab(const std::array<float, 3> &oklab)
{
    return srgb_from_oklab(oklab);
}

std::string color_solver_candidate_cache_key(const std::vector<std::array<float, 3>> &component_colors,
                                             int                                      total_units,
                                             const ColorSolverConstraints            &constraints)
{
    const int default_total_units = color_solver_total_units_for_component_count(component_colors.size());
    const ColorSolverConstraints sane = sanitize_constraints(constraints, component_colors.size());
    std::ostringstream key;
    key << component_colors.size();
    if (total_units > 0 && total_units != default_total_units)
        key << "|tu" << total_units;
    key << "|c" << sane.min_component_percent << ',' << sane.max_component_percent << ','
        << sane.min_components << ',' << sane.max_components;
    if (!sane.allowed_pairs.empty()) {
        key << "|p";
        // Only the upper triangle carries information; the matrix is symmetric
        // by construction (a pair is either mixable or it is not).
        for (size_t i = 0; i < component_colors.size(); ++i)
            for (size_t j = i + 1; j < component_colors.size(); ++j)
                key << (sane.allowed_pairs[i * component_colors.size() + j] ? '1' : '0');
    }
    for (const std::array<float, 3> &color : component_colors) {
        key << '|'
            << int(std::lround(clamp01(color[0]) * 65535.f)) << ','
            << int(std::lround(clamp01(color[1]) * 65535.f)) << ','
            << int(std::lround(clamp01(color[2]) * 65535.f));
    }
    return key.str();
}

ColorSolverCandidateSet build_color_solver_candidates(const std::vector<std::array<float, 3>> &component_colors,
                                                      int                                      total_units,
                                                      const ColorSolverConstraints            &constraints)
{
    ColorSolverCandidateSet candidates;
    if (component_colors.empty())
        return candidates;

    const size_t component_count = component_colors.size();
    if (total_units <= 0)
        total_units = color_solver_total_units_for_component_count(component_count);
    if (total_units <= 0)
        return candidates;

    const ColorSolverConstraints sane = sanitize_constraints(constraints, component_count);
    // A component is either unused or carries a share inside the window.  Both
    // bounds are converted to whole units so the enumeration stays on the
    // lattice; min rounds up and max rounds down, i.e. a mix that appears in the
    // set always satisfies the percentage the user asked for.
    auto units_for = [&](int t, int percent, bool round_up) {
        return round_up ? int(std::ceil(double(percent) * double(t) / 100.0))
                        : int(std::floor(double(percent) * double(t) / 100.0));
    };
    // Every mix has to be mixed, and every mix costs a polynomial evaluation per
    // component, so the set size is a real time and memory budget rather than a
    // theoretical one.  A printer with many slots and no component cap would
    // enumerate tens of millions of mixes; drop the lattice a step at a time
    // until the set fits, which costs precision in the reported percentages and
    // nothing else.
    constexpr size_t k_candidate_budget = 200000;
    size_t expected = count_constrained_candidates(component_count, total_units,
                                                   units_for(total_units, sane.min_component_percent, true),
                                                   units_for(total_units, sane.max_component_percent, false),
                                                   sane.max_components);
    while (expected > k_candidate_budget && total_units > 4) {
        total_units /= 2;
        expected = count_constrained_candidates(component_count, total_units,
                                                units_for(total_units, sane.min_component_percent, true),
                                                units_for(total_units, sane.max_component_percent, false),
                                                sane.max_components);
    }
    const int min_units = units_for(total_units, sane.min_component_percent, true);
    const int max_units = units_for(total_units, sane.max_component_percent, false);

    std::vector<int> units(component_count, 0);
    candidates.component_count = component_count;
    candidates.total_units     = total_units;
    candidates.rgbs.reserve(expected * 3);
    candidates.perceptual_coords.reserve(expected * 3);
    candidates.weights.reserve(expected * component_count);

    auto emit = [&]() {
        const std::array<float, 3> mixed = mix_color_solver_components(component_colors, units);
        const std::array<float, 3> perceptual = oklab_from_srgb(mixed);
        candidates.rgbs.emplace_back(mixed[0]);
        candidates.rgbs.emplace_back(mixed[1]);
        candidates.rgbs.emplace_back(mixed[2]);
        candidates.perceptual_coords.emplace_back(perceptual[0]);
        candidates.perceptual_coords.emplace_back(perceptual[1]);
        candidates.perceptual_coords.emplace_back(perceptual[2]);
        for (size_t weight_idx = 0; weight_idx < component_count; ++weight_idx)
            candidates.weights.emplace_back(float(units[weight_idx]) / float(total_units));
    };

    std::vector<size_t> used;
    used.reserve(sane.max_components);
    std::function<void(size_t, int)> recurse = [&](size_t idx, int remaining_units) {
        if (idx + 1 == component_count) {
            units[idx] = remaining_units;
            if (remaining_units > 0) {
                if (used.size() + 1 > sane.max_components || used.size() + 1 < sane.min_components ||
                    remaining_units < min_units || remaining_units > max_units ||
                    !pair_allowed(sane, component_count, used, idx))
                    return;
            } else if (used.size() < std::max<size_t>(sane.min_components, 1)) {
                return;
            }
            emit();
            return;
        }
        // Leave this component out.
        units[idx] = 0;
        recurse(idx + 1, remaining_units);
        if (used.size() + 1 > sane.max_components || !pair_allowed(sane, component_count, used, idx))
            return;
        const int hi = std::min(max_units, remaining_units);
        used.push_back(idx);
        for (int unit = std::max(1, min_units); unit <= hi; ++unit) {
            units[idx] = unit;
            recurse(idx + 1, remaining_units - unit);
        }
        used.pop_back();
        units[idx] = 0;
    };
    recurse(0, total_units);

    // An empty result is a legitimate answer, not a bug: min_components can
    // exceed what allowed_pairs permits (every pair incompatible), and the caller
    // - build_best_color_match_recipe - already has an "no recipe" path for
    // exactly that case.  Relaxing the constraints here instead would hand the
    // dialog a mix the printer cannot make.
    build_color_solver_kd_trees(candidates);
    return candidates;
}

void build_color_solver_candidate_kd_trees(ColorSolverCandidateSet &candidates)
{
    build_color_solver_kd_trees(candidates);
}

const ColorSolverCandidateSet &color_solver_candidates(ColorSolverCandidateCache                &cache,
                                                       const std::vector<std::array<float, 3>> &component_colors,
                                                       int                                      total_units,
                                                       const ColorSolverConstraints            &constraints)
{
    const std::string key = color_solver_candidate_cache_key(component_colors, total_units, constraints);
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    return cache.emplace(key, build_color_solver_candidates(component_colors, total_units, constraints)).first->second;
}

size_t solve_color_solver_candidate_for_target(const ColorSolverCandidateSet &candidates,
                                               const std::array<float, 3>     &target_rgb,
                                               ColorSolverMode                 solver_mode)
{
    if (candidates.empty())
        return size_t(-1);
    const ColorSolverNearestResult nearest = solve_nearest(candidates, target_rgb, solver_mode);
    return nearest.best_idx < candidates.candidate_count() ? nearest.best_idx : size_t(-1);
}

std::vector<std::vector<size_t>> solve_color_solver_top_component_sets(const ColorSolverCandidateSet &candidates,
                                                                      const std::array<float, 3>     &target_rgb,
                                                                      ColorSolverMode                 solver_mode,
                                                                      size_t                          max_sets)
{
    std::vector<std::vector<size_t>> out;
    if (candidates.empty() || max_sets == 0 || candidates.component_count > 64)
        return out;

    const bool perceptual = color_solver_mode_is_perceptual(solver_mode) &&
                            candidates.perceptual_coords.size() == candidates.rgbs.size();
    std::array<float, 3> target_oklab { 0.f, 0.f, 0.f };
    std::array<float, 3> axis_weights { 1.f, 1.f, 1.f };
    if (perceptual) {
        target_oklab = oklab_from_srgb(target_rgb);
        axis_weights = color_solver_perceptual_axis_weights(target_oklab, solver_mode);
    }

    // mask -> the error of that set's best mix. At most C(64,max_components)
    // entries, and in practice a few thousand.
    std::unordered_map<uint64_t, float> best_per_set;
    best_per_set.reserve(candidates.candidate_count() / 8 + 16);
    const size_t count = candidates.candidate_count();
    for (size_t candidate = 0; candidate < count; ++candidate) {
        uint64_t mask = 0;
        for (size_t component = 0; component < candidates.component_count; ++component)
            if (candidates.weights[candidate * candidates.component_count + component] > 0.f)
                mask |= uint64_t(1) << component;
        if (mask == 0)
            continue;
        const float error = perceptual ?
            color_solver_candidate_perceptual_error(candidates, candidate, target_oklab, axis_weights, solver_mode) :
            color_solver_candidate_error(candidates, candidate, target_rgb);
        auto it = best_per_set.find(mask);
        if (it == best_per_set.end())
            best_per_set.emplace(mask, error);
        else if (error < it->second)
            it->second = error;
    }

    std::vector<std::pair<float, uint64_t>> ranked;
    ranked.reserve(best_per_set.size());
    for (const auto &entry : best_per_set)
        ranked.emplace_back(entry.second, entry.first);
    // The mask is the tie-breaker so the order is total and the answer does not
    // depend on the hash table's iteration order.
    const size_t keep = std::min(max_sets, ranked.size());
    std::partial_sort(ranked.begin(), ranked.begin() + keep, ranked.end(),
                      [](const std::pair<float, uint64_t> &lhs, const std::pair<float, uint64_t> &rhs) {
                          if (lhs.first != rhs.first)
                              return lhs.first < rhs.first;
                          return lhs.second < rhs.second;
                      });

    out.reserve(keep);
    for (size_t i = 0; i < keep; ++i) {
        std::vector<size_t> components;
        for (size_t component = 0; component < candidates.component_count; ++component)
            if (ranked[i].second & (uint64_t(1) << component))
                components.push_back(component);
        out.emplace_back(std::move(components));
    }
    return out;
}

std::vector<float> solve_color_solver_weights_for_target(const ColorSolverCandidateSet &candidates,
                                                         const std::array<float, 3>     &target_rgb,
                                                         ColorSolverLookupMode           lookup_mode,
                                                         ColorSolverMode                 solver_mode)
{
    if (candidates.empty())
        return {};

    const size_t candidate_count = candidates.candidate_count();
    const ColorSolverNearestResult nearest = solve_nearest(candidates, target_rgb, solver_mode);
    if (nearest.best_idx >= candidate_count)
        return {};

    std::vector<float> weights(candidates.component_count, 0.f);
    const size_t best_weight_idx = nearest.best_idx * candidates.component_count;
    if (lookup_mode == ColorSolverLookupMode::ClosestMix ||
        nearest.second_idx >= candidate_count ||
        nearest.best_error <= 1e-12f) {
        for (size_t idx = 0; idx < candidates.component_count; ++idx)
            weights[idx] = candidates.weights[best_weight_idx + idx];
        return weights;
    }

    const size_t second_weight_idx = nearest.second_idx * candidates.component_count;
    const float best_inv = 1.f / std::max(nearest.best_error, 1e-12f);
    const float second_inv = 1.f / std::max(nearest.second_error, 1e-12f);
    const float inv_sum = std::max(best_inv + second_inv, 1e-12f);
    for (size_t idx = 0; idx < candidates.component_count; ++idx)
        weights[idx] = clamp01((candidates.weights[best_weight_idx + idx] * best_inv +
                                candidates.weights[second_weight_idx + idx] * second_inv) / inv_sum);
    return weights;
}

} // namespace Slic3r
