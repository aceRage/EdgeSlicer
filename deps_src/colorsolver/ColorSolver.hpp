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

// ---------------------------------------------------------------------------
// Vendored into EdgeSlicer from sentientstardust-dev/OrcaSlicer-ImageMap
// @ 92548381056dbf72836b0a1bdc455f238218dbfb, trimmed to the candidate /
// kd-tree / Oklab core.  See NOTICE for what was kept and what was dropped.
//
// The one behavioural change against upstream: there is no mix-model enum any
// more.  Upstream could mix through pigment-painter (GPL-3.0 + a 36 MiB LUT) or
// prusa-fdm-mixer; this copy always mixes with the fork's own header-only
// src/libslic3r/filament_mixer_model.h (MIT), chained pairwise in exactly the
// order MixedFilamentManager::blend_color_multi uses.  One mix model means the
// solver, the preview swatch and the sidebar chip all predict the same colour.
// ---------------------------------------------------------------------------

#ifndef slic3r_ColorSolver_hpp_
#define slic3r_ColorSolver_hpp_

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {

// How the nearest candidate is turned into a weight vector.
enum class ColorSolverLookupMode : int
{
    // Return the winning candidate's own weights.  Every weight is then an exact
    // multiple of 1/total_units, which is what the batch dialog wants because it
    // has to write integer percentages.
    ClosestMix = 0,
    // Inverse-error blend of the best two candidates.  Produces weights that are
    // not on the unit lattice; kept for callers that want a continuous answer.
    BlendClosestTwo = 1
};

// Which space the nearest search measures in.
enum class ColorSolverMode : int
{
    RGB = 0,
    Oklab = 1,
    // Oklab with the chroma-dependent axis weights capped, plus a penalty on
    // candidates that come out lighter than the target.  This is the mode the
    // batch matcher uses: on a saturated target it stops the search trading a
    // large lightness error for a small hue one.
    OklabSoftCap4Dark4 = 2
};

// A fully enumerated set of reachable mixes of one particular component set,
// with two kd-trees over it (linear sRGB and Oklab).
struct ColorSolverCandidateSet {
    struct KdNode {
        uint32_t candidate_idx { 0 };
        int      left { -1 };
        int      right { -1 };
        uint8_t  axis { 0 };
    };

    size_t component_count { 0 };
    // The unit budget the set was enumerated at; every weight is a multiple of
    // 1/total_units.  Callers that must emit integer units read this back.
    int    total_units { 0 };
    std::vector<float> rgbs;               // 3 per candidate, sRGB 0..1
    std::vector<float> perceptual_coords;  // 3 per candidate, Oklab
    std::vector<float> weights;            // component_count per candidate, sums to 1
    std::vector<KdNode> kd_nodes;
    std::vector<KdNode> perceptual_kd_nodes;
    int kd_root { -1 };
    int perceptual_kd_root { -1 };

    bool empty() const
    {
        return component_count == 0 || rgbs.empty() || rgbs.size() % 3 != 0 ||
               weights.size() != (rgbs.size() / 3) * component_count;
    }

    size_t candidate_count() const { return rgbs.size() / 3; }
};

using ColorSolverCandidateCache = std::map<std::string, ColorSolverCandidateSet>;

// Constraints on which mixes are reachable at all.  A component is either unused
// (weight exactly 0) or carries a share inside [min_component_percent,
// max_component_percent]; min_components / max_components bound how many may be
// non-zero at once; allowed_pairs rules out combinations that cannot physically
// be mixed.  The defaults reproduce "any mix of everything".
struct ColorSolverConstraints {
    int    min_component_percent { 0 };
    int    max_component_percent { 100 };
    size_t min_components { 1 };
    size_t max_components { 0 };   // 0 = no cap
    // Row-major component_count x component_count, or empty for "all allowed".
    // A mix may use components i and j together only when allowed_pairs[i * n + j]
    // is non-zero.  The batch dialog feeds its filament-category compatibility
    // matrix in here, so an incompatible pair is never enumerated at all rather
    // than being filtered out of an answer after the fact.
    std::vector<uint8_t> allowed_pairs;

    bool operator==(const ColorSolverConstraints &rhs) const
    {
        return min_component_percent == rhs.min_component_percent &&
               max_component_percent == rhs.max_component_percent &&
               min_components == rhs.min_components &&
               max_components == rhs.max_components &&
               allowed_pairs == rhs.allowed_pairs;
    }
};

ColorSolverLookupMode color_solver_lookup_mode_from_index(int mode);
ColorSolverMode       color_solver_mode_from_index(int mode);

// Unit budget per component count.  Fewer components can afford a finer lattice.
int    color_solver_total_units_for_component_count(size_t component_count);
// Number of compositions of total_units into component_count non-negative parts.
size_t color_solver_candidate_count(size_t component_count, int total_units);

// Mix component colours with the fork's mixer, chained pairwise in ascending
// component order with an accumulating weight - byte-for-byte the algorithm in
// MixedFilamentManager::blend_color_multi.  Components with a non-positive
// weight are skipped entirely, so a single non-zero weight returns that
// component's own colour unchanged.
std::array<float, 3> mix_color_solver_components(const std::vector<std::array<float, 3>> &component_colors,
                                                 const std::vector<int>                  &weights);
std::array<float, 3> mix_color_solver_components(const std::vector<std::array<float, 3>> &component_colors,
                                                 const std::vector<float>                &weights);

std::array<float, 3> color_solver_oklab_from_srgb(const std::array<float, 3> &rgb);
std::array<float, 3> color_solver_srgb_from_oklab(const std::array<float, 3> &oklab);

std::string color_solver_candidate_cache_key(const std::vector<std::array<float, 3>> &component_colors,
                                             int                                      total_units = 0,
                                             const ColorSolverConstraints            &constraints = {});
ColorSolverCandidateSet build_color_solver_candidates(const std::vector<std::array<float, 3>> &component_colors,
                                                      int                                      total_units = 0,
                                                      const ColorSolverConstraints            &constraints = {});
void build_color_solver_candidate_kd_trees(ColorSolverCandidateSet &candidates);
const ColorSolverCandidateSet &color_solver_candidates(ColorSolverCandidateCache                &cache,
                                                       const std::vector<std::array<float, 3>> &component_colors,
                                                       int                                      total_units = 0,
                                                       const ColorSolverConstraints            &constraints = {});

// The nearest reachable mix to target_rgb, as one weight per component summing
// to 1.  Empty when the candidate set is empty.
std::vector<float> solve_color_solver_weights_for_target(const ColorSolverCandidateSet &candidates,
                                                         const std::array<float, 3>     &target_rgb,
                                                         ColorSolverLookupMode           lookup_mode,
                                                         ColorSolverMode                 solver_mode);

// As above but returns the winning candidate's index, so the caller can read the
// exact mixed colour back out of candidates.rgbs without re-mixing (and so an
// exact endpoint stays exact).  Returns size_t(-1) when there is no answer.
size_t solve_color_solver_candidate_for_target(const ColorSolverCandidateSet &candidates,
                                               const std::array<float, 3>     &target_rgb,
                                               ColorSolverMode                 solver_mode);

// The best `max_sets` *component sets* for a target, best first, as 0-based
// component indices.
//
// The two queries above answer "which single mix is nearest", in log time, off
// the kd-tree - which is what a per-pixel caller needs.  This one answers a
// different question: "which combinations of filaments are worth looking at at
// all".  A caller that can afford a finer search than the unit lattice (the
// batch colour-match dialog sweeps whole percentages and scores in dE2000) wants
// several sets to refine, not one mix; picking only the lattice winner throws
// away sets whose best ratio lies between two lattice points.  It is an exact
// linear pass keeping the best error per distinct set rather than a kd-tree
// query, because the answer is per set and not per candidate.
std::vector<std::vector<size_t>> solve_color_solver_top_component_sets(const ColorSolverCandidateSet &candidates,
                                                                      const std::array<float, 3>     &target_rgb,
                                                                      ColorSolverMode                 solver_mode,
                                                                      size_t                          max_sets);

} // namespace Slic3r

#endif
