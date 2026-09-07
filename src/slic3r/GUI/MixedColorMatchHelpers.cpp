#include "MixedColorMatchHelpers.hpp"
#include "MixedGradientSelector.hpp"
#include <unordered_map>
#include <unordered_set>
#include <ColorSpaceConvert.hpp>
#include "MixedFilamentColorMapPanel.hpp"
#include "GUI_App.hpp"
#include "PresetBundle.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <set>
#include <queue>
#include <sstream>
#include <mutex>
#include <boost/log/trivial.hpp>
#include "nlohmann/json.hpp"
#include "ColorSolver.hpp"
#include "libslic3r/filament_mixer.h"
#include "libslic3r/Utils.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

namespace Slic3r { namespace GUI {
wxColour parse_mixed_color(const std::string& value)
{
    wxColour color(value);
    if (!color.IsOk())
        color = wxColour("#26A69A");
    return color;
}

wxString normalize_color_match_hex(const wxString& value)
{
    wxString normalized = value;
    normalized.Trim(true);
    normalized.Trim(false);
    normalized.MakeUpper();
    if (!normalized.empty() && normalized[0] != '#')
        normalized.Prepend("#");
    // #RRGGBBAA → #RRGGBB (drop alpha; only RGB is used for matching)
    if (normalized.length() == 9)
        normalized = normalized.Mid(0, 7);
    return normalized;
}

bool try_parse_color_match_hex(const wxString& value, wxColour& color_out)
{
    const wxString normalized = normalize_color_match_hex(value);
    if (normalized.length() != 7)
        return false;

    for (size_t idx = 1; idx < normalized.length(); ++idx) {
        const unsigned char ch = static_cast<unsigned char>(normalized[idx]);
        if (!std::isxdigit(ch))
            return false;
    }

    wxColour parsed(normalized);
    if (!parsed.IsOk())
        return false;

    color_out = parsed;
    return true;
}

std::vector<int> normalize_color_match_weights(const std::vector<int>& weights, size_t count)
{
    // Guard against count == 0: the remainder-fill loop below would otherwise
    // run with an empty `out`/`remainders` and write out[0] out of bounds.
    if (count == 0)
        return {};
    std::vector<int> out = weights;
    if (out.size() != count)
        out.assign(count, int(100 / count));

    int sum = 0;
    for (int& value : out) {
        value = std::max(0, value);
        sum += value;
    }
    if (sum <= 0 && count > 0) {
        out.assign(count, 0);
        out[0] = 100;
        return out;
    }

    std::vector<double> remainders(count, 0.0);
    int                 assigned = 0;
    for (size_t idx = 0; idx < count; ++idx) {
        const double exact = 100.0 * double(out[idx]) / double(sum);
        out[idx]           = int(std::floor(exact));
        remainders[idx]    = exact - double(out[idx]);
        assigned += out[idx];
    }

    int missing = std::max(0, 100 - assigned);
    while (missing > 0) {
        size_t best_idx       = 0;
        double best_remainder = -1.0;
        for (size_t idx = 0; idx < remainders.size(); ++idx) {
            if (remainders[idx] > best_remainder) {
                best_remainder = remainders[idx];
                best_idx       = idx;
            }
        }
        ++out[best_idx];
        remainders[best_idx] = 0.0;
        --missing;
    }

    return out;
}

std::vector<int> expand_color_match_recipe_weights(const MixedColorMatchRecipeResult& recipe, size_t num_physical)
{
    std::vector<int> weights(num_physical, 0);
    if (!recipe.valid || num_physical == 0)
        return weights;

    if (!recipe.gradient_component_ids.empty()) {
        const std::vector<unsigned int> ids = MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids);
        const std::vector<int>          raw_weights =
            normalize_color_match_weights(decode_color_match_gradient_weights(recipe.gradient_component_weights, ids.size()), ids.size());
        if (ids.size() != raw_weights.size())
            return weights;
        for (size_t idx = 0; idx < ids.size(); ++idx) {
            if (ids[idx] >= 1 && ids[idx] <= num_physical)
                weights[ids[idx] - 1] = raw_weights[idx];
        }
        return weights;
    }

    if (recipe.component_a >= 1 && recipe.component_a <= num_physical)
        weights[recipe.component_a - 1] = std::max(0, 100 - std::clamp(recipe.mix_b_percent, 0, 100));
    if (recipe.component_b >= 1 && recipe.component_b <= num_physical)
        weights[recipe.component_b - 1] = std::max(0, std::clamp(recipe.mix_b_percent, 0, 100));
    return weights;
}

std::string summarize_color_match_recipe(const MixedColorMatchRecipeResult& recipe)
{
    if (!recipe.valid)
        return {};

    std::vector<unsigned int> ids;
    std::vector<int>          weights;
    if (!recipe.gradient_component_ids.empty()) {
        ids     = MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids);
        weights = normalize_color_match_weights(decode_color_match_gradient_weights(recipe.gradient_component_weights, ids.size()),
                                                ids.size());
    } else {
        ids     = {recipe.component_a, recipe.component_b};
        weights = {std::max(0, 100 - std::clamp(recipe.mix_b_percent, 0, 100)), std::max(0, std::clamp(recipe.mix_b_percent, 0, 100))};
    }
    if (ids.empty() || ids.size() != weights.size())
        return {};

    std::ostringstream out;
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        if (idx > 0)
            out << '/';
        out << 'F' << ids[idx];
    }
    out << ' ';
    for (size_t idx = 0; idx < weights.size(); ++idx) {
        if (idx > 0)
            out << '/';
        out << weights[idx] << '%';
    }
    return out.str();
}

wxBitmap make_color_match_swatch_bitmap(const wxColour& color, const wxSize& size)
{
    wxBitmap   bmp(size.GetWidth(), size.GetHeight());
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(wxColour(255, 255, 255)));
    dc.Clear();
    dc.SetPen(wxPen(wxColour(120, 120, 120), 1));
    dc.SetBrush(wxBrush(color.IsOk() ? color : wxColour("#26A69A")));
    dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

static std::vector<std::vector<bool>> build_compatibility_matrix(size_t n)
{
    std::vector<std::vector<bool>> m(n, std::vector<bool>(n, false));
    for (size_t i = 0; i < n; ++i) {
        m[i][i] = true;
        for (size_t j = i + 1; j < n; ++j) {
            bool ok = is_filament_compatible(std::vector<unsigned int>{(unsigned int)i, (unsigned int)j});
            m[i][j] = m[j][i] = ok;
        }
    }
    return m;
}

std::vector<MixedColorMatchRecipeResult> build_color_match_presets(const std::vector<std::string>& physical_colors,
                                                                   int                             min_component_percent)
{
    std::vector<MixedColorMatchRecipeResult> presets;
    if (physical_colors.size() < 2)
        return presets;

    std::vector<wxColour> palette;
    palette.reserve(physical_colors.size());
    for (const std::string& hex : physical_colors)
        palette.emplace_back(parse_mixed_color(hex));

    constexpr size_t                k_max_presets = 9999;  // effectively unlimited
    std::unordered_set<std::string> seen_colors;
    auto                            add_candidate = [&presets, &seen_colors](MixedColorMatchRecipeResult candidate) {
        if (!candidate.valid)
            return;
        const std::string color_key = normalize_color_match_hex(candidate.preview_color.GetAsString(wxC2S_HTML_SYNTAX)).ToStdString();
        if (color_key.empty() || !seen_colors.insert(color_key).second)
            return;
        presets.emplace_back(std::move(candidate));
    };

    // Only generate 50:50 ratio for two-color combinations
    auto compat = build_compatibility_matrix(palette.size());
    for (size_t left_idx = 0; left_idx < palette.size() && presets.size() < k_max_presets; ++left_idx) {
        for (size_t right_idx = left_idx + 1; right_idx < palette.size() && presets.size() < k_max_presets; ++right_idx) {
            if (!compat[left_idx][right_idx]) continue;
            add_candidate(build_pair_color_match_candidate(palette, unsigned(left_idx + 1), unsigned(right_idx + 1), 50,
                                                           min_component_percent));
        }
    }

    const size_t           triple_limit         = std::min<size_t>(palette.size(), 6);
    const std::vector<int> equal_triple_weights = normalize_color_match_weights({1, 1, 1}, 3);
    for (size_t first_idx = 0; first_idx + 2 < triple_limit && presets.size() < k_max_presets; ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 1 < triple_limit && presets.size() < k_max_presets; ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx < triple_limit && presets.size() < k_max_presets; ++third_idx) {
                if (!compat[first_idx][second_idx] || !compat[second_idx][third_idx] || !compat[first_idx][third_idx]) continue;
                const std::vector<unsigned int> ids = {unsigned(first_idx + 1), unsigned(second_idx + 1), unsigned(third_idx + 1)};
                add_candidate(build_multi_color_match_candidate(palette, ids, equal_triple_weights, min_component_percent));
                for (size_t dominant_idx = 0; dominant_idx < ids.size() && presets.size() < k_max_presets; ++dominant_idx) {
                    std::vector<int> dominant_weights(ids.size(), 25);
                    dominant_weights[dominant_idx] = 50;
                    add_candidate(build_multi_color_match_candidate(palette, ids, dominant_weights, min_component_percent));
                }
            }
        }
    }

#if 0 // four-color presets: disabled
    const size_t quad_limit = std::min<size_t>(palette.size(), 5);
    for (size_t first_idx = 0; first_idx + 3 < quad_limit && presets.size() < k_max_presets; ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 2 < quad_limit && presets.size() < k_max_presets; ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx + 1 < quad_limit && presets.size() < k_max_presets; ++third_idx) {
                for (size_t fourth_idx = third_idx + 1; fourth_idx < quad_limit && presets.size() < k_max_presets; ++fourth_idx) {
                    add_candidate(build_multi_color_match_candidate(palette,
                                                                    {unsigned(first_idx + 1), unsigned(second_idx + 1),
                                                                     unsigned(third_idx + 1), unsigned(fourth_idx + 1)},
                                                                    {25, 25, 25, 25}, min_component_percent));
                }
            }
        }
    }
#endif

    return presets;
}

// ---- BlendLUT ----

BlendLUT::BlendLUT(size_t n) : m_n(n)
{
    if (n < 2) {
        m_n = 0;
        return;
    }
    // Store only b >= a entries; m_pair[a][b - a] for b in [a, n)
    m_pair.resize(n);
    for (size_t a = 0; a < n; ++a) {
        m_pair[a].resize(n - a);
        for (size_t b = a; b < n; ++b)
            m_pair[a][b - a].resize(101);
    }
}

// ---- CIELAB conversion & helpers ----

CIELab sRGB_to_CIELab(const wxColour& c)
{
    double r = c.Red()   / 255.0;
    double g = c.Green() / 255.0;
    double b = c.Blue()  / 255.0;
    float lab[3];
    RGB2Lab(float(r), float(g), float(b), &lab[0], &lab[1], &lab[2]);
    return { double(lab[0]), double(lab[1]), double(lab[2]) };
}

double delta_e_lab(const CIELab& a, const CIELab& b)
{
    return double(DeltaE00(float(a.L), float(a.a), float(a.b),
                           float(b.L), float(b.a), float(b.b)));
}

BlendLUT build_blend_lut(const std::vector<wxColour>& palette)
{
    const size_t n = palette.size();
    BlendLUT lut(n);
    if (lut.empty()) return lut;

    for (size_t a = 0; a < n; ++a) {
        for (size_t b = a; b < n; ++b) {
            for (int pct = 0; pct <= 100; ++pct) {
                wxColour blended = blend_pair_filament_mixer(palette[a], palette[b], float(pct) / 100.f);
                lut.m_pair[a][b - a][pct] = sRGB_to_CIELab(blended);
            }
        }
    }
    return lut;
}

CIELab blend_weighted_lab_accurate(const std::vector<wxColour>& palette,
                                    const std::vector<unsigned int>& ids,
                                    const std::vector<int>& weights)
{
    if (ids.size() != weights.size() || ids.empty())
        return { 50.0, 0.0, 0.0 };

    // Sort by filament ID ascending — matches blend_display_color_from_sequence
    // which iterates IDs from 1..n. Sequential lerp order matters.
    std::vector<std::pair<unsigned int, int>> sorted;
    sorted.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
        sorted.emplace_back(ids[i], weights[i]);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<wxColour> colors;
    std::vector<double>   dweights;
    colors.reserve(sorted.size());
    dweights.reserve(sorted.size());
    for (const auto& [id, w] : sorted) {
        if (id == 0 || id > palette.size()) continue;
        colors.push_back(palette[id - 1]);
        dweights.push_back(double(std::max(0, w)));
    }

    wxColour blended = blend_multi_filament_mixer(colors, dweights);
    return sRGB_to_CIELab(blended);
}

// ---- ΔE2000 ----

double color_delta_e00(const wxColour& lhs, const wxColour& rhs)
{
    float lhs_l = 0.f, lhs_a = 0.f, lhs_b = 0.f;
    float rhs_l = 0.f, rhs_a = 0.f, rhs_b = 0.f;
    RGB2Lab(float(lhs.Red()) / 255.f, float(lhs.Green()) / 255.f, float(lhs.Blue()) / 255.f, &lhs_l, &lhs_a, &lhs_b);
    RGB2Lab(float(rhs.Red()) / 255.f, float(rhs.Green()) / 255.f, float(rhs.Blue()) / 255.f, &rhs_l, &rhs_a, &rhs_b);
    return double(DeltaE00(lhs_l, lhs_a, lhs_b, rhs_l, rhs_a, rhs_b));
}

// ---- the recipe search ----
//
// Until now this was a two-stage heuristic: scan every PAIR at 5% then refine
// the best 30 at 1%, and if that was still worse than dE 0.5, score TRIPLES
// drawn from the top-8 filaments by single-colour dE, coarsely at 10% then
// refined. The weakness was the triple stage: the component set was chosen by
// how close each filament was to the target on its own, which is the wrong
// question - the three filaments that make the best mix are often not the three
// that are individually nearest (a good brown wants yellow, and yellow is not
// near brown).
//
// It is now two stages with the same shape but a different first one. The
// vendored solver (deps_src/colorsolver) enumerates EVERY reachable mix of the
// selected filaments on a fixed unit lattice, indexes it with a kd-tree in
// Oklab, and answers "closest printable colour to this target" exactly - so the
// component set comes from an exhaustive perceptual search rather than a
// ranking heuristic. The second stage is unchanged in kind: a 1% sweep over
// that component set's weights, scored with the same dE2000 the dialog reports.
//
// Everything downstream is untouched. The result is the same struct, with the
// same pair / gradient encoding and the same preview_color pipeline, so the
// Apply path (rows in MixedFilamentManager, sidebar chips) sees no difference
// beyond better numbers.
namespace {

// The candidate sets are expensive to build (one polynomial mix per reachable
// combination) and identical for every model colour in a batch, so they are
// cached per palette + constraint set. The mutex is held across the query too:
// batch_match_model_colors runs on a worker thread, the queries are
// microseconds, and a reference into the cache must not be read while another
// thread is inserting.
std::mutex                        g_color_solver_cache_mutex;
Slic3r::ColorSolverCandidateCache g_color_solver_cache;

// Ask the solver which filaments are worth mixing. Returns up to max_sets
// component sets, best first, each as 1-based filament ids in ascending order -
// the order every downstream blend uses.
//
// Several sets rather than one: the solver measures on its unit lattice (2.5%
// steps for four filaments) and in Oklab, while the caller then sweeps whole
// percentages and scores in dE2000. Those two rankings do not always agree at
// the top, so taking only the lattice winner loses sets whose best whole-percent
// ratio sits between two lattice points. Measured: with one set the search was
// worse than the old one it replaces on 9 of 18 targets on a 12-filament
// palette; with these counts it is worse on none.
std::vector<std::vector<unsigned int>> solve_component_sets(const std::vector<wxColour>&          palette,
                                                            const wxColour&                        target,
                                                            const Slic3r::ColorSolverConstraints&  constraints,
                                                            size_t                                 max_sets)
{
    std::vector<std::array<float, 3>> colors;
    colors.reserve(palette.size());
    for (const wxColour& c : palette)
        colors.push_back({ float(c.Red()) / 255.f, float(c.Green()) / 255.f, float(c.Blue()) / 255.f });
    const std::array<float, 3> target_rgb {
        float(target.Red()) / 255.f, float(target.Green()) / 255.f, float(target.Blue()) / 255.f };

    std::vector<std::vector<size_t>> sets;
    {
        std::lock_guard<std::mutex> lock(g_color_solver_cache_mutex);
        const Slic3r::ColorSolverCandidateSet& set =
            Slic3r::color_solver_candidates(g_color_solver_cache, colors, 0, constraints);
        sets = Slic3r::solve_color_solver_top_component_sets(set, target_rgb,
                                                             Slic3r::ColorSolverMode::OklabSoftCap4Dark4, max_sets);
    }

    std::vector<std::vector<unsigned int>> out;
    out.reserve(sets.size());
    for (const std::vector<size_t>& components : sets) {
        std::vector<unsigned int> ids;
        ids.reserve(components.size());
        for (const size_t component : components)
            ids.push_back(unsigned(component + 1));
        out.emplace_back(std::move(ids));
    }
    return out;
}

} // namespace

MixedColorMatchRecipeResult build_best_color_match_recipe(const std::vector<std::string>& physical_colors,
                                                          const wxColour&                 target_color,
                                                          int                             min_component_percent,
                                                          int                             max_component_percent,
                                                          bool                            check_compatible)
{
    MixedColorMatchRecipeResult best;
    if (!target_color.IsOk() || physical_colors.size() < 2)
        return best;

    // Validate max_component_percent symmetrically with batch_match_model_colors's
    // min check. Range [50, 100]: the floor matches loop_min_weight's clamp ceiling
    // (a max < the effective min makes the search window empty and silently yields no
    // recipe); 100 is the physical cap. Out-of-range input returns an invalid result
    // rather than letting the search loops run with nonsensical bounds (e.g. a value
    // >100 would widen the upper bound past 100% and silently bypass the cap).
    if (max_component_percent < 50 || max_component_percent > 100) {
        BOOST_LOG_TRIVIAL(warning)
            << "build_best_color_match_recipe: max_component_percent=" << max_component_percent
            << " out of [50, 100]; returning invalid recipe";
        return best;
    }

    // ---- Step 1: build palette & pre-convert to Lab ----
    const size_t n = physical_colors.size();
    std::vector<wxColour> palette;
    palette.reserve(n);
    for (const std::string& hex : physical_colors)
        palette.emplace_back(parse_mixed_color(hex));
    const CIELab target_lab = sRGB_to_CIELab(target_color);

    const int  loop_min_weight      = std::max(1, std::clamp(min_component_percent, 0, 50));

    // check_compatible=false (manual batch mode) bypasses the category filter so
    // cross-type mixes (e.g. PLA+PETG) can be computed and stored. The downstream
    // slice gate (Plater::has_incompatible_mixed_filament_in_use) still blocks
    // incompatible mixes at slice time — this only widens the candidate pool during
    // recipe search. Handing an all-true matrix to the solver makes the pair
    // constraint a no-op, exactly as the old `if (!compat[i][j]) continue;` did.
    std::vector<std::vector<bool>> compat;
    if (check_compatible) {
        compat = build_compatibility_matrix(n);
    } else {
        compat.assign(n, std::vector<bool>(n, true));
    }
    std::vector<uint8_t> allowed_pairs(n * n, 1);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j)
            allowed_pairs[i * n + j] = compat[i][j] ? 1 : 0;

    auto encode_gradient_weights = [](const std::vector<int>& weights) {
        std::ostringstream ss;
        for (size_t i = 0; i < weights.size(); ++i) {
            if (i > 0) ss << '/';
            ss << weights[i];
        }
        return ss.str();
    };

    // ---- Step 2: a 1% sweep over one component set, scored in dE2000 ----
    // The set is fixed; only the ratio moves. The blend is written out here
    // rather than routed through blend_weighted_lab_accurate because that helper
    // sorts and allocates four vectors per call, and this is the inner loop:
    // the result is the same chained lerp in ascending filament order.
    auto blend_inline = [](const std::vector<wxColour>& colors, const std::vector<int>& weights) {
        unsigned char r = 0, g = 0, b = 0;
        double        accumulated = 0.0;
        bool          seeded      = false;
        for (size_t i = 0; i < colors.size(); ++i) {
            const double w = double(std::max(0, weights[i]));
            if (w <= 0.0) continue;
            if (!seeded) {
                r = (unsigned char) colors[i].Red();
                g = (unsigned char) colors[i].Green();
                b = (unsigned char) colors[i].Blue();
                accumulated = w;
                seeded = true;
                continue;
            }
            const double total = accumulated + w;
            ::Slic3r::filament_mixer_lerp(r, g, b,
                                          (unsigned char) colors[i].Red(),
                                          (unsigned char) colors[i].Green(),
                                          (unsigned char) colors[i].Blue(),
                                          float(w / total), &r, &g, &b);
            accumulated = total;
        }
        return wxColour(r, g, b);
    };

    const int weight_lo = std::max(loop_min_weight, 100 - max_component_percent);
    const int weight_hi = std::min(100 - loop_min_weight, max_component_percent);

    auto refine_pair = [&](unsigned int a, unsigned int b) {
        MixedColorMatchRecipeResult r;
        for (int pct = weight_lo; pct <= weight_hi; ++pct) {
            const wxColour blended = blend_pair_filament_mixer(palette[a - 1], palette[b - 1], float(pct) / 100.f);
            const double   de      = delta_e_lab(target_lab, sRGB_to_CIELab(blended));
            if (!r.valid || de + 1e-6 < r.delta_e) {
                r.valid         = true;
                r.component_a   = a;
                r.component_b   = b;
                r.mix_b_percent = pct;
                r.preview_color = blended;
                r.delta_e       = de;
            }
        }
        return r;
    };

    auto refine_triple = [&](const std::vector<unsigned int>& ids) {
        MixedColorMatchRecipeResult r;
        const std::vector<wxColour> colors { palette[ids[0] - 1], palette[ids[1] - 1], palette[ids[2] - 1] };
        std::vector<int> weights(3, 0);
        const int wa_hi = std::min(100 - 2 * loop_min_weight, max_component_percent);
        for (int wa = loop_min_weight; wa <= wa_hi; ++wa) {
            const int wb_hi = std::min(100 - wa - loop_min_weight, max_component_percent);
            for (int wb = loop_min_weight; wb <= wb_hi; ++wb) {
                const int wc = 100 - wa - wb;
                if (wc < loop_min_weight || wc > max_component_percent)
                    continue;
                weights[0] = wa; weights[1] = wb; weights[2] = wc;
                const wxColour blended = blend_inline(colors, weights);
                const double   de      = delta_e_lab(target_lab, sRGB_to_CIELab(blended));
                if (!r.valid || de + 1e-6 < r.delta_e) {
                    r.valid       = true;
                    r.component_a = ids[0];
                    r.component_b = ids[1];
                    // Never 0: batch_match_model_colors reads mix_b_percent == 0
                    // as "this is a pure filament, not a mix" and would drop the
                    // row on the way to MixedFilamentManager.
                    r.mix_b_percent = std::clamp(int(std::lround(100.0 * double(wb) / double(wa + wb))), 1, 99);
                    r.gradient_component_ids     = MixedFilamentManager::encode_gradient_component_ids(ids);
                    r.gradient_component_weights = encode_gradient_weights({ wa, wb, wc });
                    r.preview_color = blended;
                    r.delta_e       = de;
                }
            }
        }
        return r;
    };

    // ---- Step 3: the solver proposes the component sets, dE2000 picks ----
    // How many sets to hand to the sweep. Measured against the old search over
    // 4, 8 and 12 filament palettes x 18 targets (scratchpad matcher_spike.cpp):
    // at these numbers the new search is never worse on any of the 54 cases and
    // better on 13; dropping the triple count to 6 makes it worse on 2. Above
    // 12 nothing more changes, and each extra set costs a whole sweep.
    constexpr size_t k_pair_sets   = 8;
    constexpr size_t k_triple_sets = 12;

    Slic3r::ColorSolverConstraints constraints;
    constraints.min_component_percent = loop_min_weight;
    constraints.max_component_percent = max_component_percent;
    constraints.min_components        = 2;
    constraints.max_components        = 2;
    constraints.allowed_pairs         = allowed_pairs;

    MixedColorMatchRecipeResult best_pair;
    for (const std::vector<unsigned int>& ids : solve_component_sets(palette, target_color, constraints, k_pair_sets)) {
        if (ids.size() != 2)
            continue;
        const MixedColorMatchRecipeResult r = refine_pair(ids[0], ids[1]);
        if (r.valid && (!best_pair.valid || r.delta_e + 1e-6 < best_pair.delta_e))
            best_pair = r;
    }

    // ---- Step 4: early termination ----
    if (best_pair.valid && best_pair.delta_e <= 0.5)
        return best_pair;

    // ---- Step 5: triples ----
    // Three components maximum, because MixedFilamentDialog's MODE_MATCH editor
    // is a three-corner picker and silently truncates a longer row when the user
    // saves it. The storage, display, 3MF and slicing paths all handle more
    // (MixedFilament's gradient path is loop-over-N throughout), so this is a UI
    // limit and the only reason the cap is here.
    if (n >= 3) {
        Slic3r::ColorSolverConstraints triple_constraints = constraints;
        triple_constraints.min_components = 3;
        triple_constraints.max_components = 3;
        for (const std::vector<unsigned int>& ids :
             solve_component_sets(palette, target_color, triple_constraints, k_triple_sets)) {
            if (ids.size() != 3)
                continue;
            const MixedColorMatchRecipeResult r = refine_triple(ids);
            if (r.valid && (!best.valid || r.delta_e + 1e-6 < best.delta_e))
                best = r;
        }
    }

    // ---- final normalization: re-evaluate ΔE with consistent color_delta_e00 ----
    // Pair and triple search may use different evaluation paths; re-evaluate both
    // via the same pipeline for a fair comparison, then prefer the simpler (pair)
    // recipe when the ΔE gain is negligible.
    if (best.valid)
        best.delta_e = color_delta_e00(target_color, best.preview_color);
    if (best_pair.valid) {
        best_pair.delta_e = color_delta_e00(target_color, best_pair.preview_color);
        // Pick the true winner under unified evaluation
        if (!best.valid || best_pair.delta_e + 1e-6 < best.delta_e)
            best = std::move(best_pair);
        // Prefer simpler recipe when multi-color ΔE advantage is imperceptible
        else if (!best.gradient_component_ids.empty() &&
                 best_pair.delta_e <= best.delta_e + 0.5)
            best = std::move(best_pair);
    }

    return best;
}

// Shared constants + helpers for Full Spectrum preset-name resolution. Kept in
// an anonymous namespace so the table is not visible outside this TU (these are
// pure implementation details of the two functions below).
namespace {
constexpr const char*  kFullSpectrumBase  = "Snapmaker PLA Full Spectrum @U1 ";
constexpr double       kFallbackNozzle    = 0.4;   // the only SKU shipped historically
constexpr double       kMinNozzle         = 0.05;  // matches build_mixed_filament_display_context

// Format a nozzle diameter the same way the preset JSON names do ("0.4", "0.2"):
// two decimals, trailing zeros and a dangling dot stripped.
std::string format_nozzle_label(double mm)
{
    std::string s = float_to_string_decimal_point(mm, 2);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// Read the current printer's nozzle_diameter from preset_bundle with the same
// null-check + kMinNozzle clamp as build_mixed_filament_display_context. Falls
// back to kFallbackNozzle when preset_bundle / the option is unavailable.
double current_nozzle_diameter()
{
    double nozzle = kFallbackNozzle;
    auto*  pb     = wxGetApp().preset_bundle;
    if (pb != nullptr) {
        if (const auto* opt = pb->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
            opt != nullptr && !opt->values.empty()) {
            // (C) clamp to sane physical range -- matches build_mixed_filament_display_context
            nozzle = std::max(kMinNozzle, opt->values.front());
        }
    } else {
        BOOST_LOG_TRIVIAL(warning)
            << "full_spectrum nozzle resolution: preset_bundle null; falling back to "
            << kFullSpectrumBase << format_nozzle_label(kFallbackNozzle) << " nozzle";
    }
    return nozzle;
}
} // namespace

// Full Spectrum preset name, with nozzle diameter resolved from the current
// printer's nozzle_diameter (mirrors PresetComboBoxes.cpp's @U1 <nozzle> nozzle
// convention). Falls back to the canonical 0.4 SKU when no matching preset
// exists for the current nozzle. Follows the same preset_bundle-access pattern
// as build_mixed_filament_display_context below (null-check + warning log +
// std::max clamp) -- see the header doc for the UI-thread convention.
//
// NOTE: because this function falls back to the 0.4 name on a miss, its return
// value alone cannot tell you whether the *current* nozzle actually has a preset
// -- use full_spectrum_preset_exists_for_current_nozzle() for that.
std::string full_spectrum_preset_name()
{
    const double  nozzle    = current_nozzle_diameter();
    auto*         pb        = wxGetApp().preset_bundle;
    const std::string candidate = std::string(kFullSpectrumBase) + format_nozzle_label(nozzle) + " nozzle";

    // Candidate for the current nozzle; verify the preset actually exists.
    if (pb != nullptr && pb->filaments.find_preset(candidate) != nullptr)
        return candidate;

    // Fallback: the canonical 0.4 SKU (shipped today). If even that is missing
    // (profile not loaded), return the literal name so upstream fallback chains
    // (load_full_spectrum_colors -> canonical CMYW palette) take over.
    return std::string(kFullSpectrumBase) + format_nozzle_label(kFallbackNozzle) + " nozzle";
}

// True iff a Full Spectrum preset exists for the printer's *current* nozzle
// diameter (no fallback). Used by the batch-match guard to decide whether
// Recommended mode is available, instead of hard-coding 0.4 mm: shipping a new
// nozzle variant (e.g. 0.2/0.6/0.8) just requires adding its preset JSON under
// resources/profiles/Snapmaker/filament/, and this returns true automatically.
bool full_spectrum_preset_exists_for_current_nozzle()
{
    const double     nozzle    = current_nozzle_diameter();
    auto*            pb        = wxGetApp().preset_bundle;
    const std::string candidate = std::string(kFullSpectrumBase) + format_nozzle_label(nozzle) + " nozzle";
    return pb != nullptr && pb->filaments.find_preset(candidate) != nullptr;
}

MixedFilamentDisplayContext build_mixed_filament_display_context(const std::vector<std::string>& physical_colors)
{
    MixedFilamentDisplayContext context;
    context.num_physical    = physical_colors.size();
    context.physical_colors = physical_colors;
    context.nozzle_diameters.assign(context.num_physical, 0.4);

    auto* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return context;

    DynamicPrintConfig* print_cfg = &preset_bundle->prints.get_edited_preset().config;
    if (const ConfigOptionFloats* opt = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")) {
        const size_t opt_count = opt->values.size();
        if (opt_count > 0) {
            for (size_t i = 0; i < context.num_physical; ++i)
                context.nozzle_diameters[i] = std::max(0.05, opt->get_at(unsigned(std::min(i, opt_count - 1))));
        }
    }

    auto get_mixed_bool = [preset_bundle, print_cfg](const std::string& key, bool fallback) {
        if (const ConfigOptionBool* opt = preset_bundle->project_config.option<ConfigOptionBool>(key))
            return opt->value;
        if (const ConfigOptionInt* opt = preset_bundle->project_config.option<ConfigOptionInt>(key))
            return opt->value != 0;
        if (print_cfg != nullptr) {
            if (const ConfigOptionBool* opt = print_cfg->option<ConfigOptionBool>(key))
                return opt->value;
            if (const ConfigOptionInt* opt = print_cfg->option<ConfigOptionInt>(key))
                return opt->value != 0;
        }
        return fallback;
    };
    auto get_mixed_float = [preset_bundle, print_cfg](const std::string& key, float fallback) {
        if (preset_bundle->project_config.has(key))
            return float(preset_bundle->project_config.opt_float(key));
        if (print_cfg != nullptr && print_cfg->has(key))
            return float(print_cfg->opt_float(key));
        return fallback;
    };

    context.preview_settings.mixed_lower_bound    = std::max(0.01, double(get_mixed_float("mixed_filament_height_lower_bound", 0.04f)));
    context.preview_settings.mixed_upper_bound    = std::max(context.preview_settings.mixed_lower_bound,
                                                             double(get_mixed_float("mixed_filament_height_upper_bound", 0.16f)));
    context.preview_settings.preferred_a_height   = std::max(0.0, double(get_mixed_float("mixed_color_layer_height_a", 0.f)));
    context.preview_settings.preferred_b_height   = std::max(0.0, double(get_mixed_float("mixed_color_layer_height_b", 0.f)));
    context.preview_settings.nominal_layer_height = 0.2;
    if (print_cfg != nullptr && print_cfg->has("layer_height"))
        context.preview_settings.nominal_layer_height = std::max(0.01, print_cfg->opt_float("layer_height"));
    if (print_cfg != nullptr && print_cfg->has("wall_loops"))
        context.preview_settings.wall_loops = std::max<size_t>(1, size_t(std::max(1, print_cfg->opt_int("wall_loops"))));
    context.preview_settings.local_z_mode              = get_mixed_bool("dithering_local_z_mode", false);
    context.preview_settings.local_z_direct_multicolor = get_mixed_bool("dithering_local_z_direct_multicolor", false) &&
                                                         context.preview_settings.preferred_a_height <= EPSILON &&
                                                         context.preview_settings.preferred_b_height <= EPSILON;
    context.component_bias_enabled = get_mixed_bool("mixed_filament_component_bias_enabled", false);

    return context;
}

wxColour compute_color_match_recipe_display_color(const MixedColorMatchRecipeResult& recipe, const MixedFilamentDisplayContext& context)
{
    if (!recipe.valid)
        return recipe.preview_color.IsOk() ? recipe.preview_color : wxColour("#26A69A");

    MixedFilament entry;
    entry.component_a                = recipe.component_a;
    entry.component_b                = recipe.component_b;
    entry.mix_b_percent              = recipe.mix_b_percent;
    entry.manual_pattern             = recipe.manual_pattern;
    entry.gradient_component_ids     = recipe.gradient_component_ids;
    entry.gradient_component_weights = recipe.gradient_component_weights;
    entry.distribution_mode          = recipe.gradient_component_ids.empty() ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);

    return parse_mixed_color(compute_mixed_filament_display_color(entry, context));
}

std::vector<int> decode_color_match_gradient_weights(const std::string& value, size_t expected_components)
{
    std::vector<int> weights;
    if (value.empty() || expected_components == 0)
        return weights;

    std::string token;
    for (const char ch : value) {
        if (ch >= '0' && ch <= '9') {
            token.push_back(ch);
            continue;
        }
        if (!token.empty()) {
            weights.emplace_back(std::max(0, std::atoi(token.c_str())));
            token.clear();
        }
    }
    if (!token.empty())
        weights.emplace_back(std::max(0, std::atoi(token.c_str())));
    if (weights.size() != expected_components)
        weights.clear();
    return weights;
}

MixedColorMatchRecipeResult build_pair_color_match_candidate(
    const std::vector<wxColour>& palette, unsigned int component_a, unsigned int component_b, int mix_b_percent, int min_component_percent)
{
    MixedColorMatchRecipeResult candidate;
    if (component_a == 0 || component_b == 0 || component_a == component_b)
        return candidate;
    if (component_a > palette.size() || component_b > palette.size())
        return candidate;
    if (!color_match_weights_within_range({100 - std::clamp(mix_b_percent, 0, 100), std::clamp(mix_b_percent, 0, 100)},
                                          min_component_percent))
        return candidate;

    candidate.valid         = true;
    candidate.component_a   = component_a;
    candidate.component_b   = component_b;
    candidate.mix_b_percent = std::clamp(mix_b_percent, 0, 100);
    candidate.preview_color = blend_pair_filament_mixer(palette[component_a - 1], palette[component_b - 1],
                                                        float(candidate.mix_b_percent) / 100.f);
    return candidate;
}

MixedColorMatchRecipeResult build_multi_color_match_candidate(const std::vector<wxColour>&     palette,
                                                              const std::vector<unsigned int>& ids,
                                                              const std::vector<int>&          weights,
                                                              int                              min_component_percent)
{
    MixedColorMatchRecipeResult candidate;
    if (ids.size() < 3 || ids.size() != weights.size())
        return candidate;
    if (!color_match_weights_within_range(weights, min_component_percent))
        return candidate;

    std::vector<std::pair<int, unsigned int>> weighted_ids;
    weighted_ids.reserve(ids.size());
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        if (ids[idx] == 0 || ids[idx] > palette.size())
            return candidate;
        if (weights[idx] <= 0)
            continue;
        weighted_ids.emplace_back(weights[idx], ids[idx]);
    }
    if (weighted_ids.size() < 3)
        return candidate;

    std::sort(weighted_ids.begin(), weighted_ids.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first)
            return lhs.first > rhs.first;
        return lhs.second < rhs.second;
    });

    std::vector<unsigned int> ordered_ids;
    std::vector<int>          ordered_weights;
    ordered_ids.reserve(weighted_ids.size());
    ordered_weights.reserve(weighted_ids.size());
    for (const auto& [weight, filament_id] : weighted_ids) {
        ordered_ids.emplace_back(filament_id);
        ordered_weights.emplace_back(weight);
    }

    const std::vector<unsigned int> sequence = build_color_match_sequence(ordered_ids, ordered_weights);
    if (sequence.empty())
        return candidate;

    candidate.valid             = true;
    candidate.component_a       = ordered_ids[0];
    candidate.component_b       = ordered_ids[1];
    const int pair_weight_total = ordered_weights[0] + ordered_weights[1];
    candidate.mix_b_percent     = pair_weight_total > 0 ?
                                      std::clamp(int(std::lround(100.0 * double(ordered_weights[1]) / double(pair_weight_total))), 0, 100) :
                                      50;
    candidate.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ordered_ids);
    {
        std::ostringstream weights_ss;
        for (size_t weight_idx = 0; weight_idx < ordered_weights.size(); ++weight_idx) {
            if (weight_idx > 0)
                weights_ss << '/';
            weights_ss << ordered_weights[weight_idx];
        }
        candidate.gradient_component_weights = weights_ss.str();
    }
    candidate.preview_color = blend_sequence_filament_mixer(palette, sequence);
    return candidate;
}

bool color_match_weights_within_range(const std::vector<int>& weights, int min_component_percent)
{
    if (min_component_percent <= 0)
        return true;

    const int min_allowed       = std::clamp(min_component_percent, 0, 50);
    int       active_components = 0;
    for (const int weight : weights) {
        if (weight <= 0)
            continue;
        ++active_components;
        if (weight < min_allowed)
            return false;
    }
    return active_components >= 2;
}

std::vector<unsigned int> build_color_match_sequence(const std::vector<unsigned int>& ids, const std::vector<int>& weights)
{
    if (ids.empty() || ids.size() != weights.size())
        return {};

    constexpr int k_max_cycle = 48;

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          counts;
    filtered_ids.reserve(ids.size());
    counts.reserve(weights.size());
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        const int weight = std::max(0, weights[idx]);
        if (weight <= 0)
            continue;
        filtered_ids.emplace_back(ids[idx]);
        counts.emplace_back(std::max(1, int(std::round((double(weight) / 100.0) * k_max_cycle))));
    }

    if (filtered_ids.empty())
        return {};

    int cycle = std::accumulate(counts.begin(), counts.end(), 0);
    while (cycle > k_max_cycle) {
        auto it = std::max_element(counts.begin(), counts.end());
        if (it == counts.end() || *it <= 1)
            break;
        --(*it);
        --cycle;
    }

    if (cycle <= 0)
        return {};

    std::vector<unsigned int> sequence;
    sequence.reserve(size_t(cycle));
    std::vector<int> emitted(counts.size(), 0);
    for (int pos = 0; pos < cycle; ++pos) {
        size_t best_idx   = 0;
        double best_score = -1e9;
        for (size_t idx = 0; idx < counts.size(); ++idx) {
            const double target = double((pos + 1) * counts[idx]) / double(std::max(1, cycle));
            const double score  = target - double(emitted[idx]);
            if (score > best_score) {
                best_score = score;
                best_idx   = idx;
            }
        }
        ++emitted[best_idx];
        sequence.emplace_back(filtered_ids[best_idx]);
    }

    return sequence;
}

wxColour blend_sequence_filament_mixer(const std::vector<wxColour>& palette, const std::vector<unsigned int>& sequence)
{
    if (palette.empty() || sequence.empty())
        return wxColour("#26A69A");

    std::vector<int> counts(palette.size() + 1, 0);
    for (const unsigned int filament_id : sequence) {
        if (filament_id == 0 || filament_id > palette.size())
            continue;
        ++counts[filament_id];
    }

    std::vector<wxColour> colors;
    std::vector<double>   weights;
    colors.reserve(palette.size());
    weights.reserve(palette.size());
    for (size_t filament_id = 1; filament_id <= palette.size(); ++filament_id) {
        if (counts[filament_id] <= 0)
            continue;
        colors.emplace_back(palette[filament_id - 1]);
        weights.emplace_back(double(counts[filament_id]));
    }

    return blend_multi_filament_mixer(colors, weights);
}

// ---------------------------------------------------------------------------
// Material compatibility
// ---------------------------------------------------------------------------

enum class FilamentCategory : uint8_t {
    PLA, PETG, TPU, PET, ABS, ASA, PC, PA, SUPPORT,
    UNKNOWN
};

static constexpr const char* k_category_names[] = {
    "PLA", "PETG", "TPU", "PET", "ABS", "ASA", "PC", "PA", "SUPPORT"
};
static constexpr size_t k_category_count = sizeof(k_category_names) / sizeof(k_category_names[0]);
// Matrix dimension covers all valid categories + UNKNOWN sentinel
static constexpr size_t k_compat_dim = static_cast<size_t>(FilamentCategory::UNKNOWN) + 1;

static FilamentCategory filament_category_from_name(const std::string& name)
{
    for (size_t i = 0; i < k_category_count; ++i) {
        if (name == k_category_names[i])
            return static_cast<FilamentCategory>(i);
    }
    return FilamentCategory::UNKNOWN;
}

// 2D compatibility matrix. Dimension is tied to the enum so adding a category
// to FilamentCategory automatically grows the table.
static std::vector<std::vector<bool>> s_compat;
// atomic, not plain bool: the unlocked first read below and the locked write in
// load_filament_compatibility() would otherwise be a data race (classic non-atomic
// double-checked locking). acquire/release is sufficient — the mutex inside provides
// the heavy lifting; this flag only needs to publish "already loaded" without a race.
static std::atomic<bool>              s_compat_loaded{false};
static std::mutex                     s_compat_mutex;

static void load_filament_compatibility()
{
    if (s_compat_loaded.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(s_compat_mutex);
    if (s_compat_loaded.load(std::memory_order_acquire)) return;

    // Default: each category compatible only with itself
    s_compat.assign(k_compat_dim, std::vector<bool>(k_compat_dim, false));
    for (size_t i = 0; i < k_category_count; ++i)
        s_compat[i][i] = true;

    try {
        // Prefer user data dir (where PresetUpdater deploys updates), fall back to bundled resources
        const boost::filesystem::path user_path = (boost::filesystem::path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR
                                                    / PresetBundle::SM_BUNDLE / "filament"
                                                    / "filament_compatibility.json").make_preferred();
        const boost::filesystem::path rsrc_path = (boost::filesystem::path(Slic3r::resources_dir()) / "profiles"
                                                    / PresetBundle::SM_BUNDLE / "filament"
                                                    / "filament_compatibility.json").make_preferred();
        const std::string path = (boost::filesystem::exists(user_path) ? user_path : rsrc_path).string();

        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            BOOST_LOG_TRIVIAL(error) << "Failed to open filament compatibility config: " << path;
            return;
        }
        nlohmann::json j;
        ifs >> j;

        if (!j.contains("compatibility")) {
            BOOST_LOG_TRIVIAL(error) << "Missing 'compatibility' key in " << path;
            return;
        }

        for (auto& [cat_a_str, partner_list] : j["compatibility"].items()) {
            FilamentCategory cat_a = filament_category_from_name(cat_a_str);
            if (cat_a == FilamentCategory::UNKNOWN) {
                BOOST_LOG_TRIVIAL(warning) << "Unknown category '" << cat_a_str << "' in compatibility config";
                continue;
            }

            if (!partner_list.is_array()) {
                BOOST_LOG_TRIVIAL(warning) << "Expected array for category '" << cat_a_str << "'";
                continue;
            }

            for (auto& cat_b_val : partner_list) {
                const std::string cat_b_str = cat_b_val.get<std::string>();
                FilamentCategory   cat_b     = filament_category_from_name(cat_b_str);
                if (cat_b == FilamentCategory::UNKNOWN) {
                    BOOST_LOG_TRIVIAL(warning) << "Unknown category '" << cat_b_str << "' in compatibility config";
                    continue;
                }
                s_compat[static_cast<size_t>(cat_a)][static_cast<size_t>(cat_b)] = true;
                s_compat[static_cast<size_t>(cat_b)][static_cast<size_t>(cat_a)] = true;
            }
        }
        s_compat_loaded.store(true, std::memory_order_release);
        BOOST_LOG_TRIVIAL(info) << "Loaded filament compatibility matrix from " << path;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to parse filament compatibility config: " << e.what();
    }
}

static bool is_category_compatible(FilamentCategory a, FilamentCategory b)
{
    return s_compat[static_cast<size_t>(a)][static_cast<size_t>(b)];
}

struct ResolvedFilamentCategory {
    unsigned int     filament_id;
    FilamentCategory category;
};

static FilamentCategory get_filament_category(const std::string& filament_type);

// Resolves a set of 0-based filament IDs into their (id, category) pairs.
// Skips IDs that cannot be resolved (out of range, missing preset, missing type).
// Caller must have called load_filament_compatibility() first.
static std::vector<ResolvedFilamentCategory> resolve_filament_categories(
    const std::vector<unsigned int>& filament_ids,
    PresetBundle*                    preset_bundle)
{
    std::vector<ResolvedFilamentCategory> result;
    if (!preset_bundle) return result;

    const auto& filament_presets = preset_bundle->filament_presets;
    result.reserve(filament_ids.size());

    for (unsigned int id : filament_ids) {
        if (id >= filament_presets.size()) continue;
        const Preset* preset = preset_bundle->filaments.find_preset(filament_presets[id]);
        if (!preset) continue;
        auto* type_opt = dynamic_cast<const ConfigOptionStrings*>(preset->config.option("filament_type"));
        if (!type_opt || type_opt->values.empty()) continue;

        FilamentCategory cat = get_filament_category(type_opt->values[0]);
        result.push_back({id, cat});
    }
    return result;
}

// Hardcoded filament_type → category. New filament types should be added here.
static const std::unordered_map<std::string, FilamentCategory>& filament_type_category_map()
{
    static const std::unordered_map<std::string, FilamentCategory> m = {
        // PLA family — from classification table
        {"PLA", FilamentCategory::PLA}, {"PLA-CF", FilamentCategory::PLA},
        // ABS
        {"ABS", FilamentCategory::ABS},
        // ASA
        {"ASA", FilamentCategory::ASA},
        // PETG family — from classification table
        {"PETG", FilamentCategory::PETG}, {"PETG-CF", FilamentCategory::PETG}, {"PCTG", FilamentCategory::PETG},
        // TPU
        {"TPU", FilamentCategory::TPU},
        // PET — from compatibility matrix
        {"PET", FilamentCategory::PET},
        // PA
        {"PA", FilamentCategory::PA}, {"PA-CF", FilamentCategory::PA},
        // PC
        {"PC", FilamentCategory::PC},
        // Support materials — from classification table
        {"BVOH", FilamentCategory::SUPPORT}, {"PVA", FilamentCategory::SUPPORT},
    };
    return m;
}

static std::string normalize_filament_type(const std::string& type)
{
    std::string normalized = type;
    size_t start = normalized.find_first_not_of(" \t\r\n");
    size_t end   = normalized.find_last_not_of(" \t\r\n");
    if (start != std::string::npos && end != std::string::npos)
        normalized = normalized.substr(start, end - start + 1);
    // toupper from <cctype> requires an argument in [0, UCHAR_MAX] or EOF; passing a
    // negative char (signed byte >= 0x80, e.g. a non-ASCII filament_type) is UB. The lambda
    // forces the unsigned char conversion the C API expects.
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return normalized;
}

static FilamentCategory get_filament_category(const std::string& filament_type)
{
    const std::string normalized = normalize_filament_type(filament_type);
    const auto&       m          = filament_type_category_map();
    auto              it         = m.find(normalized);
    if (it != m.end()) return it->second;
    return FilamentCategory::UNKNOWN;
}

bool is_filament_compatible(const std::vector<unsigned int>& filament_ids)
{
    if (filament_ids.size() <= 1) return true;

    load_filament_compatibility();

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) {
        BOOST_LOG_TRIVIAL(error) << "PresetBundle is null in filament compatibility check";
        return false;
    }

    auto resolved = resolve_filament_categories(filament_ids, preset_bundle);

    for (const auto& r : resolved) {
        if (r.category == FilamentCategory::UNKNOWN) {
            BOOST_LOG_TRIVIAL(info) << "Filament type at index " << r.filament_id
                                    << " not in compatibility table, treating as incompatible";
            return false;
        }
    }

    if (resolved.size() <= 1) return true;

    if (std::all_of(resolved.begin() + 1, resolved.end(),
                    [&](const ResolvedFilamentCategory& r) { return r.category == resolved[0].category; }))
        return true;

    for (size_t i = 0; i < resolved.size(); ++i) {
        for (size_t j = i + 1; j < resolved.size(); ++j) {
            if (!is_category_compatible(resolved[i].category, resolved[j].category)) {
                BOOST_LOG_TRIVIAL(info) << "Incompatible filament categories: '"
                                        << k_category_names[static_cast<size_t>(resolved[i].category)]
                                        << "' vs '"
                                        << k_category_names[static_cast<size_t>(resolved[j].category)] << "'";
                return false;
            }
        }
    }

    return true;
}

bool is_filament_compatible(const MixedFilament& mf)
{
    std::vector<unsigned int> fids;

    if (!mf.manual_pattern.empty()) {
        // Cycle / pattern mode: the pattern tokens encode all participating
        // filaments. component_a / component_b are not added separately —
        // in cycle mode they are hardcoded to 1 and 2 and would introduce
        // phantom filaments into the compatibility check.
        const std::string norm = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
        if (norm.empty()) {
            BOOST_LOG_TRIVIAL(warning)
                << "Mixed filament compatibility: manual_pattern '"
                << mf.manual_pattern << "' normalized to empty, malformed pattern";
            return false;
        }

        PresetBundle* preset_bundle = wxGetApp().preset_bundle;
        if (!preset_bundle || preset_bundle->filament_presets.empty()) {
            BOOST_LOG_TRIVIAL(error)
                << "Mixed filament compatibility: PresetBundle is "
                << (preset_bundle ? "empty (no filament presets)" : "null")
                << ", cannot resolve pattern '" << mf.manual_pattern << "'";
            return false;
        }

        const size_t num_physical = preset_bundle->filament_presets.size();
        const auto groups = MixedFilamentManager::split_pattern_groups(norm);
        for (const auto& group : groups) {
            const auto tokens = MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
            for (const auto& token : tokens) {
                const unsigned int eid = MixedFilamentManager::physical_filament_from_token(token, mf, num_physical);
                if (eid >= 1 && eid <= num_physical)
                    fids.push_back(eid - 1);
            }
        }
    } else {
        if (mf.component_a >= 1) fids.push_back(mf.component_a - 1);
        if (mf.component_b >= 1) fids.push_back(mf.component_b - 1);
        if (!mf.gradient_component_ids.empty()) {
            for (unsigned int fid : MixedFilamentManager::decode_gradient_component_ids(mf.gradient_component_ids))
                if (fid >= 1) fids.push_back(fid - 1);
        }
    }

    if (fids.empty()) {
        BOOST_LOG_TRIVIAL(warning)
            << "Mixed filament compatibility: no valid filament IDs extracted"
            << " (component_a=" << mf.component_a
            << ", component_b=" << mf.component_b
            << ", manual_pattern='" << mf.manual_pattern
            << "', gradient_component_ids='" << mf.gradient_component_ids << "')"
            << " — treating as incompatible";
        return false;
    }

    return is_filament_compatible(fids);
}

std::optional<std::pair<unsigned int, unsigned int>> find_incompatible_filament_pair(const std::vector<unsigned int>& filament_ids)
{
    if (filament_ids.size() <= 1) return std::nullopt;

    load_filament_compatibility();

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) {
        BOOST_LOG_TRIVIAL(error) << "PresetBundle is null in filament incompatibility search";
        return std::nullopt;
    }

    auto resolved = resolve_filament_categories(filament_ids, preset_bundle);

    // Treat UNKNOWN filament types as incompatible, consistent with is_filament_compatible().
    for (const auto& r : resolved) {
        if (r.category == FilamentCategory::UNKNOWN) {
            BOOST_LOG_TRIVIAL(info) << "Filament type at index " << r.filament_id
                                    << " not in compatibility table, treating as incompatible";
            // Return this UNKNOWN paired with the first other resolved filament.
            for (const auto& other : resolved) {
                if (other.filament_id != r.filament_id)
                    return std::make_pair(r.filament_id + 1, other.filament_id + 1);
            }
        }
    }

    if (resolved.size() <= 1) return std::nullopt;

    if (std::all_of(resolved.begin() + 1, resolved.end(),
                    [&](const ResolvedFilamentCategory& r) { return r.category == resolved[0].category; }))
        return std::nullopt;

    for (size_t i = 0; i < resolved.size(); ++i) {
        for (size_t j = i + 1; j < resolved.size(); ++j) {
            if (!is_category_compatible(resolved[i].category, resolved[j].category))
                return std::make_pair(
                    resolved[i].filament_id + 1,
                    resolved[j].filament_id + 1);
        }
    }

    return std::nullopt;
}

CyclePatternParseResult parse_cycle_pattern(const std::string& normalized_pattern, int num_physical)
{
    CyclePatternParseResult result;
    if (normalized_pattern.empty() || num_physical <= 0) return result;

    const auto groups = MixedFilamentManager::split_pattern_groups(normalized_pattern);
    for (const auto& group : groups) {
        const auto tokens = MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
        for (const auto& token : tokens) {
            ++result.total_tokens;
            char* end = nullptr;
            unsigned long id = std::strtoul(token.c_str(), &end, 10);
            if (!end || *end != '\0') {
                if (result.invalid_token.empty()) result.invalid_token = token;
                continue;
            }
            if (id < 1 || id > (unsigned long)num_physical) {
                if (result.invalid_id == 0) result.invalid_id = (unsigned int)id;
                continue;
            }
            result.ids.push_back((unsigned int)id);
        }
    }
    return result;
}

std::string summarize_cycle_pattern_text(const std::string& normalized_pattern,
                                         const MixedFilament& entry,
                                         int num_physical)
{
    if (normalized_pattern.empty() || num_physical <= 0)
        return {};

    const auto groups = MixedFilamentManager::split_pattern_groups(normalized_pattern);
    if (groups.empty())
        return {};

    std::map<unsigned int, int> counts;
    int                         total = 0;
    for (const auto& group : groups) {
        const auto tokens = MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
        for (const auto& token : tokens) {
            unsigned int eid = MixedFilamentManager::physical_filament_from_token(token, entry, num_physical);
            if (eid >= 1 && eid <= static_cast<unsigned>(num_physical)) {
                counts[eid]++;
                total++;
            }
        }
    }

    if (total <= 0 || counts.empty())
        return {};

    std::vector<std::pair<unsigned int, int>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Compute floor percentages; distribute remainder via largest remainders.
    std::vector<int> pcts(sorted.size());
    int              sum_pct = 0;
    for (size_t i = 0; i < sorted.size(); ++i) {
        pcts[i]  = int((static_cast<long long>(sorted[i].second) * 100) / total);
        sum_pct += pcts[i];
    }

    if (sum_pct < 100) {
        // Remainder indexed by original position, value = count * 100 % total
        std::vector<std::pair<size_t, int>> rem;
        rem.reserve(sorted.size());
        for (size_t i = 0; i < sorted.size(); ++i)
            rem.emplace_back(i, int((static_cast<long long>(sorted[i].second) * 100) % total));
        // Sort descending by remainder, then by original index for stability
        std::sort(rem.begin(), rem.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });
        for (int extra = 100 - sum_pct; extra > 0; --extra) {
            pcts[rem.front().first]++;
            rem.erase(rem.begin());
        }
    }

    std::ostringstream out;
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0)
            out << '+';
        out << 'F' << sorted[i].first << ' ' << pcts[i] << '%';
    }
    return out.str();
}

// ---- Batch Match Mapping ----

std::vector<ModelColorEntry> extract_model_colors(const Print& print)
{
    std::vector<ModelColorEntry> colors;
    static constexpr int MAX_EXTRUDER_ID = int(MAXIMUM_EXTRUDER_NUMBER);

    auto& filament_colours = print.config().filament_colour.values;

    // Also read mixed-filament display colors for virtual ID lookup.
    // NOTE: Do NOT bind a reference via a ternary with a temporary on one branch —
    // the temporary would be destroyed at the end of the full expression, leaving
    // a dangling reference (use-after-scope UB). Use the raw pointer and null-check
    // it only at the virtual-lookup call site; the physical-colour path does not
    // depend on preset_bundle at all.
    auto* pb = wxGetApp().preset_bundle;
    // Log the null-preset_bundle condition ONCE here (not per virtual-ID iteration
    // inside the loop below) — otherwise a model with N virtual-painted faces would
    // emit N identical warnings. pb is loop-invariant, so the inner check can stay
    // a silent `continue`.
    if (pb == nullptr) {
        BOOST_LOG_TRIVIAL(warning)
            << "extract_model_colors: preset_bundle null; virtual filament colors will be skipped";
    }

    for (const PrintObject* obj : print.objects()) {
        if (!obj) continue;
        const ModelObject* model_obj = obj->model_object();
        if (!model_obj) continue;

        for (const ModelVolume* vol : model_obj->volumes) {
            if (!vol || vol->type() != ModelVolumeType::MODEL_PART) continue;

            for (int eid : vol->get_extruders()) {
                if (eid < 1 || eid > MAX_EXTRUDER_ID) continue;
                size_t idx = size_t(eid - 1);

                std::string color_hex;
                if (idx < filament_colours.size()) {
                    // Physical filament color
                    color_hex = filament_colours[idx];
                } else {
                    // Virtual mixed filament — look up its display_color.
                    // pb null is already logged once above the loop; skip silently here.
                    if (pb == nullptr) {
                        continue;
                    }
                    const MixedFilament* mf = pb->mixed_filaments.mixed_filament_from_id(
                        static_cast<unsigned int>(eid), filament_colours.size());
                    if (mf && !mf->display_color.empty())
                        color_hex = mf->display_color;
                }
                if (color_hex.empty()) continue;

                wxColour c;
                if (!try_parse_color_match_hex(color_hex, c)) {
                    BOOST_LOG_TRIVIAL(warning)
                        << "extract_model_colors: invalid color for extruder "
                        << eid << " = '" << color_hex << "', skipping";
                    continue;
                }

                // Deduplicate by NORMALIZED hex value — same RGB written differently
                // (e.g. "#ff0000" vs "#FF0000", "FF0000" vs "#FF0000", or alpha variants
                // "#RRGGBBAA" vs "#RRGGBB") must collapse to one entry. Comparing raw
                // strings would double-count such colors and waste virtual slots. Mirrors
                // the normalized dedup in build_color_match_presets (line ~213). The
                // stored hex_value keeps its original form for display.
                const std::string color_key = normalize_color_match_hex(color_hex).ToStdString();
                auto it = std::find_if(colors.begin(), colors.end(),
                    [&](const ModelColorEntry& e) {
                        return normalize_color_match_hex(e.hex_value).ToStdString() == color_key;
                    });
                if (it != colors.end()) {
                    it->extruder_ids.push_back(static_cast<unsigned int>(eid));
                    continue;
                }

                colors.push_back({
                    static_cast<unsigned int>(colors.size() + 1),
                    c,
                    color_hex,
                    {static_cast<unsigned int>(eid)}
                });
            }
        }
    }

    if (colors.size() > 64) {
        BOOST_LOG_TRIVIAL(warning)
            << "extract_model_colors: truncating " << colors.size() << " colors to 64";
        colors.resize(64);
    }

    BOOST_LOG_TRIVIAL(info)
        << "extract_model_colors: extracted " << colors.size() << " unique model colors";
    return colors;
}

// ---- Batch Match Algorithm ----

#if 0 // Dead code — no deduplication is performed (explicit policy since phase2)
std::vector<ColorMappingEntry> deduplicate_batch_mappings(
    const std::vector<ColorMappingEntry>& raw_mappings,
    double                                 merge_threshold)
{
    if (merge_threshold <= 0.0 || merge_threshold > 100.0) {
        BOOST_LOG_TRIVIAL(warning)
            << "deduplicate_batch_mappings: invalid threshold " << merge_threshold
            << ", returning unmodified";
        return raw_mappings;
    }
    std::vector<ColorMappingEntry> result;

    for (const auto& mapping : raw_mappings) {
        bool merged = false;
        for (auto& existing : result) {
            if (color_delta_e00(existing.matched_color, mapping.matched_color) < merge_threshold) {
                existing.merged_model_indices.push_back(mapping.model_color_index);
                existing.source_extruder_ids.insert(
                    existing.source_extruder_ids.end(),
                    mapping.source_extruder_ids.begin(),
                    mapping.source_extruder_ids.end());
                merged = true;
                break;
            }
        }
        if (!merged) {
            result.push_back(mapping);
        }
    }
    BOOST_LOG_TRIVIAL(debug)
        << "deduplicate_batch_mappings: " << raw_mappings.size()
        << " -> " << result.size() << " (threshold DeltaE<" << merge_threshold << ")";
    return result;
}
#endif // 0

void assign_batch_virtual_filament_ids(
    BatchMatchResult& result,
    size_t             num_physical,
    size_t             existing_mixed_count)
{
    if (num_physical < 1) {
        BOOST_LOG_TRIVIAL(error)
            << "assign_batch_virtual_filament_ids: invalid num_physical=" << num_physical;
        return;
    }
    unsigned int next_virtual_id = static_cast<unsigned int>(num_physical + existing_mixed_count + 1);

    for (auto& mapping : result.mappings) {
        if (mapping.is_pure_recipe) {
            if (mapping.recipe.component_a < 1 || mapping.recipe.component_a > num_physical) {
                BOOST_LOG_TRIVIAL(warning)
                    << "assign_batch_virtual_filament_ids: component_a="
                    << mapping.recipe.component_a << " out of range [1," << num_physical
                    << "], treating as mixed";
                mapping.target_filament_id = next_virtual_id++;
                continue;
            }
            mapping.target_filament_id = mapping.recipe.component_a;
        } else {
            mapping.target_filament_id = next_virtual_id++;
        }
    }
}

std::vector<ColorMappingEntry> merge_duplicate_recipe_mappings(
    const std::vector<ColorMappingEntry>& mappings)
{
    if (mappings.size() < 2) return mappings;

    // Fingerprint: the recipe fields that define which physical filaments are mixed and in
    // what ratio. Two recipes with the same fingerprint produce the same blend, so they must
    // share one virtual slot. Derived fields (preview_color, delta_e, display_color) are
    // excluded — they are deterministic functions of the identity fields.
    auto fingerprint = [](const MixedColorMatchRecipeResult& r) {
        std::ostringstream oss;
        // Use '|' as a field separator so (a=1,b=12) != (a=11,b=2); the ints alone would
        // collide if concatenated without a delimiter.
        oss << r.component_a << '|' << r.component_b << '|' << r.mix_b_percent << '|'
            << r.manual_pattern << '|' << r.gradient_component_ids << '|'
            << r.gradient_component_weights;
        return oss.str();
    };

    std::vector<ColorMappingEntry> result;
    result.reserve(mappings.size());
    // recipe fingerprint → index into result. Only non-pure mappings are tracked: pure
    // recipes target existing physical IDs and never allocate a new slot, so deduping them
    // is both unnecessary and semantically wrong (two pure recipes hitting the same physical
    // is the normal, expected case — they share the ID already).
    std::unordered_map<std::string, size_t> seen;

    for (const ColorMappingEntry& m : mappings) {
        if (m.is_pure_recipe) {
            result.push_back(m);
            continue;
        }
        const std::string key = fingerprint(m.recipe);
        auto it = seen.find(key);
        if (it != seen.end()) {
            // Merge into the first occurrence: union source_extruder_ids (so apply still
            // remaps every source extruder to this target) and accumulate merged_model_indices
            // for traceability. The survivor keeps its own target_filament_id — the merged-in
            // mapping's target is discarded (it was a duplicate slot allocation).
            ColorMappingEntry& survivor = result[it->second];
            survivor.source_extruder_ids.insert(survivor.source_extruder_ids.end(),
                                                m.source_extruder_ids.begin(),
                                                m.source_extruder_ids.end());
            survivor.merged_model_indices.insert(survivor.merged_model_indices.end(),
                                                 m.merged_model_indices.begin(),
                                                 m.merged_model_indices.end());
        } else {
            seen.emplace(key, result.size());
            result.push_back(m);
        }
    }

    // The merge above unions source_extruder_ids / merged_model_indices by appending.
    // The two are normally disjoint across merged mappings (extract_model_colors
    // aggregates extruders by unique color), but sort+unique defensively so
    // survivor.source_extruder_ids stays idempotent for apply_batch_match_to_model
    // (unordered_map) and merged_model_indices doesn't double-count in traceability.
    // Only runs when a merge actually happened; the common no-merge path is untouched.
    if (result.size() != mappings.size()) {
        for (ColorMappingEntry& e : result) {
            if (e.is_pure_recipe) continue;
            std::sort(e.source_extruder_ids.begin(), e.source_extruder_ids.end());
            e.source_extruder_ids.erase(
                std::unique(e.source_extruder_ids.begin(), e.source_extruder_ids.end()),
                e.source_extruder_ids.end());
            std::sort(e.merged_model_indices.begin(), e.merged_model_indices.end());
            e.merged_model_indices.erase(
                std::unique(e.merged_model_indices.begin(), e.merged_model_indices.end()),
                e.merged_model_indices.end());
        }
    }

    if (result.size() != mappings.size()) {
        BOOST_LOG_TRIVIAL(info)
            << "merge_duplicate_recipe_mappings: " << mappings.size()
            << " -> " << result.size() << " (merged " << (mappings.size() - result.size())
            << " duplicate recipe mappings)";
    }
    return result;
}

BatchMatchResult batch_match_model_colors(
    const std::vector<ModelColorEntry>&          model_colors,
    const std::vector<std::string>&             physical_colors,
    int                                          min_component_percent,
    int                                          max_component_percent,
    std::shared_ptr<std::atomic<bool>>           cancel_token,
    std::function<void(int,int)>                 progress_callback,
    bool                                         check_compatible)
{
    BatchMatchResult result;
    result.success = true;

    if (min_component_percent < 0 || min_component_percent > 50) {
        result.success = false;
        result.error_message = "min_component_percent must be in [0, 50]";
        return result;
    }
    // Symmetric with build_best_color_match_recipe's max check (see line ~375). Without this,
    // an out-of-range max would be forwarded per-recipe and silently rejected by the inner
    // function, surfacing to the user as the misleading "No valid recipes found" instead of
    // a parameter error.
    if (max_component_percent < 50 || max_component_percent > 100) {
        result.success = false;
        result.error_message = "max_component_percent must be in [50, 100]";
        return result;
    }
    if (model_colors.empty()) {
        result.success = false;
        result.error_message = "No model colors to match";
        return result;
    }
    if (physical_colors.size() < 2) {
        result.success = false;
        result.error_message = "Need at least 2 physical filaments";
        return result;
    }

    const int total_count = static_cast<int>(model_colors.size());
    for (size_t i = 0; i < model_colors.size(); ++i) {
        if (cancel_token && cancel_token->load()) {
            // User cancellation (Stop Matching) — not an error. error_message is intentionally
            // empty: handle_batch_match_result treats error_code==2 as a silent rollback and
            // never displays it. See BatchMatchResult.error_code docs.
            result.success    = false;
            result.error_code = 2;
            return result;
        }

        const auto& entry = model_colors[i];
        MixedColorMatchRecipeResult recipe =
            build_best_color_match_recipe(physical_colors, entry.color, min_component_percent, max_component_percent,
                                          check_compatible);

        if (!recipe.valid) {
            BOOST_LOG_TRIVIAL(warning)
                << "batch_match: no valid recipe for color " << entry.color_index
                << " (" << entry.hex_value << ")";
            continue;
        }

        ColorMappingEntry mapping;
        mapping.model_color_index   = entry.color_index;
        mapping.source_color         = entry.color;
        mapping.recipe               = recipe;
        mapping.delta_e              = recipe.delta_e;
        mapping.matched_color        = recipe.preview_color;
        mapping.is_pure_recipe       = (recipe.mix_b_percent == 0);
        mapping.pure_delta_e         = recipe.delta_e;
        mapping.merged_model_indices = {entry.color_index};
        mapping.source_extruder_ids  = entry.extruder_ids;
        result.mappings.push_back(mapping);

        if (progress_callback)
            progress_callback(static_cast<int>(i) + 1, total_count);
    }

    if (result.mappings.empty()) {
        result.success = false;
        result.error_message = "No valid recipes found for any model color";
        result.error_code = 1;
        return result;
    }

    // Each model color always gets its own mapping entry — no deduplication.
    // Merging by matched_color would lose distinct model-source colors whose
    // recipes happen to produce similar output shades.

    double sum_de = 0.0;
    for (const auto& m : result.mappings)
        sum_de += m.delta_e;
    result.avg_delta_e = sum_de / double(result.mappings.size());

    result.selected_physical_ids.clear();
    for (size_t i = 1; i <= physical_colors.size(); ++i)
        result.selected_physical_ids.push_back(static_cast<unsigned int>(i));

    BOOST_LOG_TRIVIAL(info)
        << "batch_match: " << total_count << " model colors -> "
        << result.mappings.size() << " unique recipes, avg DeltaE=" << result.avg_delta_e;
    return result;
}

void populate_mixed_filaments_from_mappings(
    BatchMatchResult&                           result,
    const MixedFilamentDisplayContext&          context)
{
    result.mixed_filaments.clear();

    for (const auto& mapping : result.mappings) {
        if (mapping.is_pure_recipe) continue;

        MixedFilament mf;
        mf.component_a    = mapping.recipe.component_a;
        mf.component_b    = mapping.recipe.component_b;
        mf.mix_b_percent  = mapping.recipe.mix_b_percent;
        mf.manual_pattern = mapping.recipe.manual_pattern;
        mf.stable_id      = 0; // filled by add_batch_custom_filaments
        mf.ratio_a        = 1;
        mf.ratio_b        = 1;
        mf.distribution_mode = mapping.recipe.gradient_component_ids.empty()
            ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);
        mf.gradient_component_ids     = mapping.recipe.gradient_component_ids;
        mf.gradient_component_weights = mapping.recipe.gradient_component_weights;
        mf.enabled        = true;
        mf.deleted        = false;
        mf.custom         = true;
        mf.origin_auto    = false;
        mf.ui_mode        = 2; // MATCH
        mf.display_color  = mapping.matched_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();

        result.mixed_filaments.push_back(std::move(mf));
    }
}

std::vector<std::string> recommend_best_filament_combo(
    const std::vector<ModelColorEntry>&  model_colors,
    const std::vector<std::string>&      all_preset_colors,
    int                                  min_component_percent,
    int                                  max_component_percent,
    std::shared_ptr<std::atomic<bool>>   cancel_token)
{
    if (model_colors.empty() || all_preset_colors.size() < 4)
        return {};

    // Loose bounds only: this function scores colour COMBOS, it does not impose the
    // final per-component cap (that is the Pass-2 batch_match_model_colors job, which
    // independently validates min∈[0,50]/max∈[50,100]). So the legal range here is the
    // full [0,100] for both — narrower limits (e.g. mirroring batch_match_model_colors's
    // [0,50]/[50,100]) would wrongly reject valid callers like the worker's min=15.
    // min>max is a genuine logic error: the search window is empty and every combo
    // silently scores as "no valid recipe". Reject with a log instead of returning {}.
    if (min_component_percent < 0 || min_component_percent > 100 ||
        max_component_percent < 0 || max_component_percent > 100 ||
        min_component_percent > max_component_percent) {
        BOOST_LOG_TRIVIAL(warning)
            << "recommend_best_filament_combo: invalid percent range min="
            << min_component_percent << " max=" << max_component_percent
            << " (need 0..100 and min<=max); returning empty";
        return {};
    }

    // Step 1: Top-15 pre-filter by single-color ΔE
    const size_t top_n = std::min<size_t>(15, all_preset_colors.size());
    std::vector<std::pair<double, size_t>> ranked;
    ranked.reserve(all_preset_colors.size());

    // Average ΔE of each preset color against all model colors → quick filter
    for (size_t fidx = 0; fidx < all_preset_colors.size(); ++fidx) {
        wxColour fc;
        if (!try_parse_color_match_hex(all_preset_colors[fidx], fc)) continue;
        double sum_de = 0.0;
        for (const auto& mc : model_colors)
            sum_de += color_delta_e00(fc, mc.color);
        ranked.emplace_back(sum_de / std::max(1.0, double(model_colors.size())), fidx);
    }
    std::sort(ranked.begin(), ranked.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    ranked.resize(top_n);

    // Step 2: Enumerate C(N,4) combos from top-15 candidates
    struct ComboScore { size_t i0, i1, i2, i3; double score; };
    std::vector<ComboScore> combos;
    const size_t n = ranked.size();
    for (size_t i0 = 0; i0 + 3 < n; ++i0) {
        for (size_t i1 = i0 + 1; i1 + 2 < n; ++i1) {
            for (size_t i2 = i1 + 1; i2 + 1 < n; ++i2) {
                for (size_t i3 = i2 + 1; i3 < n; ++i3) {
                    if (cancel_token && cancel_token->load()) return {};

                    // Build 4-color palette from original indices
                    std::vector<std::string> combo_colors = {
                        all_preset_colors[ranked[i0].second],
                        all_preset_colors[ranked[i1].second],
                        all_preset_colors[ranked[i2].second],
                        all_preset_colors[ranked[i3].second]
                    };

                    // Score: average batch match ΔE over model colors (single-threaded, small loop).
                    // check_compatible=false: this function is called only from the RECOMMENDED-mode
                    // worker, whose palette is a single Full Spectrum PLA preset (same material by
                    // construction) — no cross-type pair can exist, so the filter is a no-op. It
                    // also keeps the worker off preset_bundle: build_compatibility_matrix reads
                    // preset_bundle->filament_presets unsynchronized, which would race the UI thread
                    // (data-race UB) from this worker call site. The slice gate still enforces real
                    // incompatibility at slice time.
                    double sum_de = 0.0;
                    int matched = 0;
                    for (const auto& mc : model_colors) {
                        auto recipe = build_best_color_match_recipe(combo_colors, mc.color, min_component_percent, max_component_percent,
                                                                     /*check_compatible=*/ false);
                        if (recipe.valid) {
                            sum_de += recipe.delta_e;
                            ++matched;
                        }
                    }
                    if (matched == 0) continue;

                    double avg_de = sum_de / double(matched);
                    double score = 1.0 / (1.0 + avg_de); // lower ΔE → higher score
                    combos.push_back({ranked[i0].second, ranked[i1].second,
                                      ranked[i2].second, ranked[i3].second, score});
                }
            }
        }
    }

    if (combos.empty()) return {};

    // Step 3: Return best combo
    std::sort(combos.begin(), combos.end(),
        [](const auto& a, const auto& b) { return a.score > b.score; });

    return {
        all_preset_colors[combos[0].i0],
        all_preset_colors[combos[0].i1],
        all_preset_colors[combos[0].i2],
        all_preset_colors[combos[0].i3]
    };
}

void apply_batch_match_to_model(const BatchMatchResult& result)
{
    if (!result.success || result.mappings.empty()) return;

    // Direct mapping: each mapping entry carries the model extruder IDs that
    // need remapping to the target_filament_id.  No color hex comparison needed.
    std::unordered_map<int, unsigned int> extruder_remap;
    for (const auto& mapping : result.mappings) {
        for (unsigned int src_eid : mapping.source_extruder_ids) {
            if (mapping.target_filament_id != src_eid)
                extruder_remap[static_cast<int>(src_eid)] = mapping.target_filament_id;
        }
    }
    if (extruder_remap.empty()) return;

    // Compute total filaments: physical + all mixed (including newly created).
    // Use project_config filament_colour — same source as Plater callback `colors`.
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (!pb) return;
    ConfigOptionStrings* co = pb->project_config.option<ConfigOptionStrings>("filament_colour");
    if (!co || co->values.empty()) return;
    const size_t num_physical    = co->values.size();
    const size_t total_filaments = pb->mixed_filaments.total_filaments(num_physical);

    // Build EnforcerBlockerStateMap: identity by default, remap where needed
    constexpr size_t MAX_EBT = static_cast<size_t>(EnforcerBlockerType::ExtruderMax);
    EnforcerBlockerStateMap state_map;
    for (size_t i = 0; i <= MAX_EBT; ++i)
        state_map[i] = static_cast<EnforcerBlockerType>(i);
    for (const auto& [eid, target] : extruder_remap) {
        if (eid <= static_cast<int>(MAX_EBT) && target <= static_cast<unsigned int>(MAX_EBT)) {
            state_map[static_cast<size_t>(eid)] = static_cast<EnforcerBlockerType>(target);
        } else if (target > static_cast<unsigned int>(MAX_EBT)) {
            BOOST_LOG_TRIVIAL(warning)
                << "apply_batch_match: cannot remap extruder " << eid
                << " -> " << target << " (target exceeds EnforcerBlockerType::ExtruderMax="
                << MAX_EBT << "), triangle-level remap skipped for this entry";
        }
    }

    // Apply remap at two levels independently:
    //   1) MMU-painted: triangle-level extruder data via remap_extruder_ids
    //      (only MODEL_PART volumes carry paint data)
    //   2) Config-level: volume/object config extruder assignment
    //      Applies to MODEL_PART AND PARAMETER_MODIFIER (modifier) volumes.
    //      A modifier keeps the colour the user picked in the object list; if
    //      that colour's extruder is remapped by the match (e.g. physical slot
    //      3 -> virtual mixed filament 7), the modifier must follow so its
    //      displayed/printed colour stays the same hue.  Previously the loop
    //      early-continued on non-MODEL_PART, dropping the modifier's colour
    //      (it then fell back to the object's extruder, which itself may have
    //      been remapped — surfacing as "colour reset to extruder 1").
    for (ModelObject* mo : wxGetApp().model().objects) {
        // Pre-read the object's effective extruder ONCE, before iterating volumes.
        // ModelVolume::extruder_id() falls back to the OBJECT config when a volume
        // has no own "extruder", and this loop rewrites that object config — so
        // calling extruder_id() again mid-loop would return the just-written value
        // for later inheriting volumes, making two volumes that share the same
        // source resolve different targets (order-dependent). Snapshotting the
        // original object extruder eliminates that hazard: every inheriting volume
        // (part or modifier) follows the same target, so their colours stay
        // consistent — which is the whole point of including modifiers here.
        const ConfigOption* obj_opt     = mo->config.option("extruder");
        const int           orig_obj_eid = (obj_opt ? obj_opt->getInt() : 0);
        auto                obj_it       = extruder_remap.find(orig_obj_eid);
        const bool          obj_remap    = (orig_obj_eid > 0 && obj_it != extruder_remap.end());
        // Remap the object-level filament independently of the volume loop.
        // Multi-part objects where EVERY volume owns its own extruder (e.g.
        // Creality 3mf exports) used to keep the parent row on its stale
        // filament: the object was only rewritten inside the "volume
        // inherits" branch below, which never runs when no volume inherits.
        // Write the object whenever its extruder is in the remap table so
        // the parent follows the match; inheriting volumes resolve through
        // the (already rewritten) object config.
        if (obj_remap)
            mo->config.set_key_value("extruder", new ConfigOptionInt(static_cast<int>(obj_it->second)));
        for (ModelVolume* mv : mo->volumes) {
            const ModelVolumeType vt = mv->type();
            const bool is_part      = (vt == ModelVolumeType::MODEL_PART);
            const bool is_modifier  = (vt == ModelVolumeType::PARAMETER_MODIFIER);

            // Level 1: triangle-level MMU data (MODEL_PART only — modifiers
            // carry no paint data).
            if (is_part && !mv->mmu_segmentation_facets.empty())
                mv->remap_extruder_ids(total_filaments, state_map);

            // Level 2: config-level extruder for parts and modifiers.
            if (!is_part && !is_modifier) continue;

            const ConfigOption* vol_opt = mv->config.option("extruder");
            if (vol_opt && vol_opt->getInt() > 0) {
                // Volume owns its extruder — remap it on the volume alone.
                auto it = extruder_remap.find(vol_opt->getInt());
                if (it != extruder_remap.end())
                    mv->config.set_key_value("extruder", new ConfigOptionInt(static_cast<int>(it->second)));
            }
            // Volumes without their own extruder inherit the object's extruder,
            // which was already remapped above — no per-volume action needed.
        }

        // Level 3: layer-object extruder (layer_config_ranges). A layer object
        // holds the user-assigned filament for a height range; remap it with
        // the same table used for volumes so it follows the match instead of
        // being stranded on a physical slot that cleanup deletes
        // (→ "layer filament >N reset to default").
        for (auto& lr : mo->layer_config_ranges) {
            ModelConfig&         lcfg = lr.second;
            const ConfigOption*  lopt = lcfg.option("extruder");
            if (!lopt) continue;
            const int old_eid = lopt->getInt();
            if (old_eid <= 0) continue;
            auto lit = extruder_remap.find(old_eid);
            if (lit == extruder_remap.end()) continue;
            lcfg.set_key_value("extruder", new ConfigOptionInt(static_cast<int>(lit->second)));
        }
    }

    BOOST_LOG_TRIVIAL(info)
        << "apply_batch_match: remapped " << extruder_remap.size()
        << " extruder IDs across all volumes (total_filaments=" << total_filaments << ")";
}

}} // namespace Slic3r::GUI

