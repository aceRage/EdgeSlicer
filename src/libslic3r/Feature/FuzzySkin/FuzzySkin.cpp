#include <cmath>
#include <random>

#include "libslic3r/Algorithm/LineSplit.hpp"
#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "FuzzySkin.hpp"

#include "libnoise/noise.h"

// #define DEBUG_FUZZY

using namespace Slic3r;

namespace Slic3r::Feature::FuzzySkin {

// Produces a random value between 0 and 1. Thread-safe.
static double random_value() {
    thread_local std::random_device rd;
    // Hash thread ID for random number seed if no hardware rng seed is available
    thread_local std::mt19937 gen(rd.entropy() > 0 ? rd() : std::hash<std::thread::id>()(std::this_thread::get_id()));
    thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(gen);
}

class UniformNoise: public noise::module::Module {
    public:
        UniformNoise(): Module (GetSourceModuleCount ()) {};

        virtual int GetSourceModuleCount() const { return 0; }
        virtual double GetValue(double x, double y, double z) const { return random_value() * 2 - 1; }
};

static std::unique_ptr<noise::module::Module> get_noise_module(const FuzzySkinConfig& cfg) {
    if (cfg.noise_type == NoiseType::Perlin) {
        auto perlin_noise = noise::module::Perlin();
        perlin_noise.SetFrequency(1 / cfg.noise_scale);
        perlin_noise.SetOctaveCount(cfg.noise_octaves);
        perlin_noise.SetPersistence(cfg.noise_persistence);
        return std::make_unique<noise::module::Perlin>(perlin_noise);
    } else if (cfg.noise_type == NoiseType::Billow) {
        auto billow_noise = noise::module::Billow();
        billow_noise.SetFrequency(1 / cfg.noise_scale);
        billow_noise.SetOctaveCount(cfg.noise_octaves);
        billow_noise.SetPersistence(cfg.noise_persistence);
        return std::make_unique<noise::module::Billow>(billow_noise);
    } else if (cfg.noise_type == NoiseType::RidgedMulti) {
        auto ridged_multi_noise = noise::module::RidgedMulti();
        ridged_multi_noise.SetFrequency(1 / cfg.noise_scale);
        ridged_multi_noise.SetOctaveCount(cfg.noise_octaves);
        return std::make_unique<noise::module::RidgedMulti>(ridged_multi_noise);
    } else if (cfg.noise_type == NoiseType::Voronoi) {
        auto voronoi_noise = noise::module::Voronoi();
        voronoi_noise.SetFrequency(1 / cfg.noise_scale);
        voronoi_noise.SetDisplacement(1.0);
        return std::make_unique<noise::module::Voronoi>(voronoi_noise);
    } else {
        return std::make_unique<UniformNoise>();
    }
}

// Fuzzy skin over overhangs.
//
// The displacement of every sampled point is multiplied by a factor in [0, 1]: 1 where the wall is
// carried by the layer below, 0 where it hangs over air, and a linear ramp across the boundary so
// there is no step where the fuzz stops. The ramp is built by walking the samples twice: the first
// pass records support per sample, the second turns that into the factor. Because the sampling
// cadence itself is untouched (an unsupported sample is emitted at its exact un-jittered position,
// never dropped), the segment length statistics of the loop are the same as without the option.
//
// kBlendSamples is the half-width of the ramp in samples: the factor is 0 at every unsupported
// sample and climbs back to 1 over kBlendSamples+1 samples on either side of an unsupported run.
// Two samples is 1.5-2.5 * fuzzy_skin_point_distance by the resampler's own 3/4..5/4 jitter, i.e.
// the 1-2 sample distances the spec asks for. A supported run shorter than the ramp simply never
// reaches 1, which degrades gracefully to a shallower ramp.
static constexpr int kBlendSamples = 2;

// docs/superpowers/specs/2026-09-09-fuzzy-skin-overhang-research.md
// Turn the per-sample "is this point carried by the layer below" flags into per-sample displacement
// factors in [0, 1]. "closed" distinguishes a full loop (the ends wrap around) from an open segment
// produced by the per-region paint splitter.
static std::vector<double> support_blend_factors(const std::vector<char>& supported, bool closed)
{
    const int           n = int(supported.size());
    std::vector<double> factors(size_t(std::max(n, 0)), 1.);
    if (n == 0)
        return factors;

    // Distance, in samples, to the nearest unsupported sample. Two sweeps over the (optionally
    // circular) sequence, so O(n) rather than O(n * blend).
    // "far" and "near" are legacy MSVC keyword macros from <windows.h>; do not name anything that.
    const int        unreached = n + kBlendSamples + 1;
    std::vector<int> to_edge(size_t(n), unreached);
    for (int i = 0; i < n; ++i)
        if (! supported[size_t(i)])
            to_edge[size_t(i)] = 0;

    // A closed loop needs the sweeps to wrap; one extra lap of kBlendSamples+1 on each side is
    // enough to settle a window that narrow.
    const int extra = closed ? std::min(n, kBlendSamples + 1) : 0;
    for (int k = 0; k < n + extra; ++k) {
        const int i = k % n;
        if (! closed && i == 0)
            continue;
        const int prev = (i - 1 + n) % n;
        to_edge[size_t(i)] = std::min(to_edge[size_t(i)], to_edge[size_t(prev)] + 1);
    }
    for (int k = n + extra - 1; k >= 0; --k) {
        const int i = k % n;
        if (! closed && i == n - 1)
            continue;
        const int next = (i + 1) % n;
        to_edge[size_t(i)] = std::min(to_edge[size_t(i)], to_edge[size_t(next)] + 1);
    }

    for (int i = 0; i < n; ++i)
        factors[size_t(i)] = supported[size_t(i)] ?
            std::min(1., double(to_edge[size_t(i)]) / double(kBlendSamples + 1)) : 0.;
    return factors;
}

// Whether the support test is live for this call at all. Keeping it in one place is what makes the
// off-mode path provably the original one: every new branch below is behind this predicate.
static inline bool skip_overhangs_active(const FuzzySkinConfig& cfg, const Polygons* support)
{
    return cfg.skip_overhangs && support != nullptr && ! support->empty();
}

// Thanks Cura developers for this function.
void fuzzy_polyline(Points& poly, bool closed, coordf_t slice_z, const FuzzySkinConfig& cfg, const Polygons* support)
{
    std::unique_ptr<noise::module::Module> noise = get_noise_module(cfg);

    const double min_dist_between_points = cfg.point_distance * 3. / 4.; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const double range_random_point_dist = cfg.point_distance / 2.;
    double dist_left_over = random_value() * (min_dist_between_points / 2.); // the distance to be traversed on the line before making the first new point
    const bool skip_overhangs = skip_overhangs_active(cfg, support);
    const bool was_closed     = closed;
    Point* p0 = &poly.back();
    Points out;
    out.reserve(poly.size());
    // Only populated when the overhang skip is on: the un-jittered sample and its displacement
    // vector, so the displacement can be scaled once the blend factors are known. Left empty (and
    // never touched) with the option off.
    Points             base_points;
    std::vector<Vec2d> offsets;
    std::vector<char>  supported;
    if (skip_overhangs) {
        base_points.reserve(poly.size());
        offsets.reserve(poly.size());
        supported.reserve(poly.size());
    }
    for (Point &p1 : poly)
    {
        if (!closed) {
            // Skip the first point for open path
            closed = true;
            p0 = &p1;
            continue;
        }
        // 'a' is the (next) new point between p0 and p1
        Vec2d  p0p1      = (p1 - *p0).cast<double>();
        double p0p1_size = p0p1.norm();
        double p0pa_dist = dist_left_over;
        for (; p0pa_dist < p0p1_size;
            p0pa_dist += min_dist_between_points + random_value() * range_random_point_dist)
        {
            Point pa = *p0 + (p0p1 * (p0pa_dist / p0p1_size)).cast<coord_t>();
            double r = noise->GetValue(unscale_(pa.x()), unscale_(pa.y()), slice_z) * cfg.thickness;
            if (skip_overhangs) {
                // Defer the displacement: it has to be scaled by the blend factor, which is only
                // known once every sample support state has been collected. The point is emitted
                // here regardless, so the sampling cadence is identical either way.
                base_points.emplace_back(pa);
                offsets.emplace_back(perp(p0p1).cast<double>().normalized() * r);
                supported.emplace_back(Slic3r::contains(*support, pa) ? 1 : 0);
                out.emplace_back(pa);
            } else {
                out.emplace_back(pa + (perp(p0p1).cast<double>().normalized() * r).cast<coord_t>());
            }
        }
        dist_left_over = p0pa_dist - p0p1_size;
        p0 = &p1;
    }
    if (skip_overhangs && ! out.empty()) {
        const std::vector<double> factors = support_blend_factors(supported, was_closed);
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = base_points[i] + (offsets[i] * factors[i]).cast<coord_t>();
    }
    while (out.size() < 3) {
        size_t point_idx = poly.size() - 2;
        out.emplace_back(poly[point_idx]);
        if (point_idx == 0)
            break;
        -- point_idx;
    }
    if (out.size() >= 3)
        poly = std::move(out);
}

// Thanks Cura developers for this function.
void fuzzy_extrusion_line(Arachne::ExtrusionJunctions& ext_lines, coordf_t slice_z, coordf_t layer_height, const FuzzySkinConfig& cfg, const Polygons* support)
{
    std::unique_ptr<noise::module::Module> noise = get_noise_module(cfg);

    const double min_dist_between_points = cfg.point_distance * 3. / 4.; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const double range_random_point_dist = cfg.point_distance / 2.;
    // ExtrusionJunction::w is a scaled coord_t, so this floor must be scaled too.
    // Flow::rounded_rectangle_extrusion_spacing() requires width > height * (1 - 0.25 * PI); keep 5% above it.
    const double min_extrusion_width = scaled<double>(layer_height * (1. - 0.25 * M_PI) * 1.05);
    double dist_left_over = random_value() * (min_dist_between_points / 2.); // the distance to be traversed on the line before making the first new point
    const bool skip_overhangs = skip_overhangs_active(cfg, support);

    auto* p0 = &ext_lines.front();
    Arachne::ExtrusionJunctions out;
    out.reserve(ext_lines.size());
    // The overhang skip has to scale a displacement AND a width delta that are only known to be
    // final once every sample support state has been collected, so with the option on the noise
    // amplitude r is recorded per emitted junction and applied in a second pass below. An
    // unsupported junction gets neither displacement nor width change: over open air a fatter or
    // thinner bead reaches just as far past the wall as a sideways shift does. Entries are pushed
    // for every sample, including the endpoint-connecting ones, so the indices line up with "out".
    std::vector<char>  supported;
    std::vector<double> noise_r;
    std::vector<Vec2d>  dirs;
    if (skip_overhangs) {
        supported.reserve(ext_lines.size());
        noise_r.reserve(ext_lines.size());
        dirs.reserve(ext_lines.size());
    }
    const auto push_passthrough = [&supported, &noise_r, &dirs, skip_overhangs]() {
        if (skip_overhangs) {
            // A junction copied through unchanged: mark it supported so it does not drag the blend
            // ramp down, and give it a zero displacement so the second pass is a no-op for it.
            supported.emplace_back(1);
            noise_r.emplace_back(0.);
            dirs.emplace_back(Vec2d(0., 0.));
        }
    };
    for (auto& p1 : ext_lines) {
        if (p0->p == p1.p) { // Connect endpoints.
            out.emplace_back(p1.p, p1.w, p1.perimeter_index);
            push_passthrough();
            continue;
        }

        // 'a' is the (next) new point between p0 and p1
        Vec2d  p0p1 = (p1.p - p0->p).cast<double>();
        double p0p1_size = p0p1.norm();
        double p0pa_dist = dist_left_over;
        for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points + random_value() * range_random_point_dist) {
            Point pa = p0->p + (p0p1 * (p0pa_dist / p0p1_size)).cast<coord_t>();
            double r = noise->GetValue(unscale_(pa.x()), unscale_(pa.y()), slice_z) * cfg.thickness;
            if (skip_overhangs) {
                // Emit the un-jittered junction now; the second pass re-applies r scaled by the
                // blend factor, in whichever of the three modes is selected.
                out.emplace_back(pa, p1.w, p1.perimeter_index);
                supported.emplace_back(Slic3r::contains(*support, pa) ? 1 : 0);
                noise_r.emplace_back(r);
                dirs.emplace_back(perp(p0p1).cast<double>().normalized());
                continue;
            }
            switch (cfg.mode) { //the curly code for testing
                case FuzzySkinMode::Displacement :
                    out.emplace_back(pa + (perp(p0p1).cast<double>().normalized() * r).cast<coord_t>(), p1.w, p1.perimeter_index);
                    break;
                case FuzzySkinMode::Extrusion :
                    out.emplace_back(pa, std::max(p1.w + r + min_extrusion_width,  min_extrusion_width), p1.perimeter_index); 
                    break;
                case FuzzySkinMode::Combined :
                    double rad = std::max(p1.w + r + min_extrusion_width,  min_extrusion_width);
                    out.emplace_back(pa + (perp(p0p1).cast<double>().normalized() * ((rad  - p1.w) / 2)).cast<coord_t>(), rad, p1.perimeter_index); //0.05 - minimum width of extruded line
                    break;
            }
        }
        dist_left_over = p0pa_dist - p0p1_size;
        p0 = &p1;
    }
    if (skip_overhangs && ! out.empty()) {
        // An Arachne extrusion is handed to us as a closed loop when its first and last junction
        // coincide; an open segment (the paint splitter, or a genuinely open wall) is not wrapped.
        const bool                closed  = ext_lines.back().p == ext_lines.front().p;
        const std::vector<double> factors = support_blend_factors(supported, closed);
        for (size_t i = 0; i < out.size(); ++i) {
            const double f = factors[i];
            const double r = noise_r[i] * f;
            if (r == 0. && f == 0.)
                continue; // unsupported, or a junction that was copied through: leave it alone
            const double w0 = out[i].w;
            switch (cfg.mode) {
            case FuzzySkinMode::Displacement:
                out[i].p += (dirs[i] * r).cast<coord_t>();
                break;
            case FuzzySkinMode::Extrusion:
                out[i].w = coord_t(std::max(w0 + r + min_extrusion_width, min_extrusion_width));
                break;
            case FuzzySkinMode::Combined: {
                const double rad = std::max(w0 + r + min_extrusion_width, min_extrusion_width);
                out[i].p += (dirs[i] * ((rad - w0) / 2)).cast<coord_t>();
                out[i].w = coord_t(rad);
                break;
            }
            }
        }
    }

    while (out.size() < 3) {
        size_t point_idx = ext_lines.size() - 2;
        out.emplace_back(ext_lines[point_idx].p, ext_lines[point_idx].w, ext_lines[point_idx].perimeter_index);
        if (point_idx == 0)
            break;
        --point_idx;
    }

    if (ext_lines.back().p == ext_lines.front().p) { // Connect endpoints.
        // Ultra fix: the closing segment is the only one that can violate the
        // resampler's minimum point spacing (the seam junction passes through
        // unfuzzied and the last sampled point may land arbitrarily close to it).
        // A near-zero-length closing segment with a large random width step prints
        // as a blob right at the seam. Drop trailing points until the closure
        // respects the same spacing floor as the rest of the loop.
        const double min_closing_dist = min_dist_between_points;
        while (out.size() > 3 && (out.back().p - out[1].p).cast<double>().norm() < min_closing_dist)
            out.pop_back();
        out.front().p = out.back().p;
        out.front().w = out.back().w;
    }

    if (out.size() >= 3)
        ext_lines = std::move(out);
}

void group_region_by_fuzzify(PerimeterGenerator& g)
{
    g.regions_by_fuzzify.clear();
    g.has_fuzzy_skin = false;
    g.has_fuzzy_hole = false;

    std::unordered_map<FuzzySkinConfig, SurfacesPtr> regions;
    for (auto region : *g.compatible_regions) {
        const auto&           region_config = region->region().config();
        const FuzzySkinConfig cfg{region_config.fuzzy_skin,
                                  scaled<coord_t>(region_config.fuzzy_skin_thickness.value),
                                  scaled<coord_t>(region_config.fuzzy_skin_point_distance.value),
                                  region_config.fuzzy_skin_first_layer,
                                  region_config.fuzzy_skin_noise_type,
                                  region_config.fuzzy_skin_scale,
                                  region_config.fuzzy_skin_octaves,
                                  region_config.fuzzy_skin_persistence,
                                  region_config.fuzzy_skin_mode,
                                  region_config.fuzzy_skin_skip_overhangs};
        auto&                 surfaces = regions[cfg];
        for (const auto& surface : region->slices.surfaces) {
            surfaces.push_back(&surface);
        }

        if (cfg.type != FuzzySkinType::None) {
            g.has_fuzzy_skin = true;
            if (cfg.type != FuzzySkinType::External) {
                g.has_fuzzy_hole = true;
            }
        }
    }

    if (regions.size() == 1) { // optimization
        g.regions_by_fuzzify[regions.begin()->first] = {};
        return;
    }

    for (auto& it : regions) {
        g.regions_by_fuzzify[it.first] = offset_ex(it.second, ClipperSafetyOffset);
    }
}

bool should_fuzzify(const FuzzySkinConfig& config, const int layer_id, const size_t loop_idx, const bool is_contour)
{
    const auto fuzziy_type = config.type;

    if (fuzziy_type == FuzzySkinType::None) {
        return false;
    }
    if (!config.fuzzy_first_layer && layer_id <= 0) {
        // Do not fuzzy first layer unless told to
        return false;
    }

    const bool fuzzify_contours = loop_idx == 0 || fuzziy_type == FuzzySkinType::AllWalls;
    const bool fuzzify_holes    = fuzzify_contours && (fuzziy_type == FuzzySkinType::All || fuzziy_type == FuzzySkinType::AllWalls);

    return is_contour ? fuzzify_contours : fuzzify_holes;
}

// docs/superpowers/specs/2026-09-09-fuzzy-skin-overhang-research.md
// The region of this layer plane that is carried by the layer below, for the overhang skip: the
// lower slices grown by half a wall width, which is exactly the polygon set the perimeter generator
// already built (generate_lower_polygons_series, index 0 - the tight -0.5*width offset, the same
// one the erOverhangPerimeter split uses) for overhang detection. Returns nullptr whenever the
// option is off for every fuzzified region, whenever this layer has no lower layer to be
// unsupported by (the first printed layer sits on the bed and is never an overhang), or whenever
// overhang detection itself is off - and a nullptr makes every fuzz function below take its
// original code path verbatim.
static bool fuzzy_skip_overhangs_wanted(const PerimeterGenerator& g)
{
    if (g.lower_slices == nullptr || g.config == nullptr || ! g.config->detect_overhang_wall)
        return false;
    // Same gate as the overhang split itself (PerimeterGenerator.cpp): below the raft top there is
    // no lower object layer to be unsupported by, and the first printed layer sits on the bed.
    if (g.object_config != nullptr && g.layer_id <= g.object_config->raft_layers)
        return false;
    for (const auto& region : g.regions_by_fuzzify)
        if (region.first.type != FuzzySkinType::None && region.first.skip_overhangs)
            return true;
    return false;
}

// The classic generator built the width-indexed series; index 0 is the tight -0.5*width offset,
// the same set traverse_loops clips the loop against to decide erOverhangPerimeter.
static const Polygons* fuzzy_support_region(const PerimeterGenerator& g, const bool is_external)
{
    if (! fuzzy_skip_overhangs_wanted(g))
        return nullptr;
    const std::vector<Polygons>& series = is_external ? g.m_external_lower_polygons_series : g.m_lower_polygons_series;
    return series.empty() ? nullptr : &series.front();
}

Polygon apply_fuzzy_skin(const Polygon& polygon, const PerimeterGenerator& perimeter_generator, const size_t loop_idx, const bool is_contour)
{
    Polygon fuzzified;

    const auto  slice_z = perimeter_generator.slice_z;
    const auto& regions = perimeter_generator.regions_by_fuzzify;
    const Polygons* support = fuzzy_support_region(perimeter_generator, loop_idx == 0);
    if (regions.size() == 1) { // optimization
        const auto& config  = regions.begin()->first;
        const bool  fuzzify = should_fuzzify(config, perimeter_generator.layer_id, loop_idx, is_contour);
        if (!fuzzify) {
            return polygon;
        }

        fuzzified = polygon;
        fuzzy_polyline(fuzzified.points, true, slice_z, config, support);
        return fuzzified;
    }

    // Find all affective regions
    std::vector<std::pair<const FuzzySkinConfig&, const ExPolygons&>> fuzzified_regions;
    fuzzified_regions.reserve(regions.size());
    for (const auto& region : regions) {
        if (should_fuzzify(region.first, perimeter_generator.layer_id, loop_idx, is_contour)) {
            fuzzified_regions.emplace_back(region.first, region.second);
        }
    }
    if (fuzzified_regions.empty()) {
        return polygon;
    }

#ifdef DEBUG_FUZZY
    {
        int i = 0;
        for (const auto& r : fuzzified_regions) {
            BoundingBox bbox = get_extents(perimeter_generator.slices->surfaces);
            bbox.offset(scale_(1.));
            ::Slic3r::SVG svg(debug_out_path("fuzzy_traverse_loops_%d_%d_%d_region_%d.svg", perimeter_generator.layer_id,
                                             loop.is_contour ? 0 : 1, loop.depth, i)
                                  .c_str(),
                              bbox);
            svg.draw_outline(perimeter_generator.slices->surfaces);
            svg.draw_outline(loop.polygon, "green");
            svg.draw(r.second, "red", 0.5);
            svg.draw_outline(r.second, "red");
            svg.Close();
            i++;
        }
    }
#endif

    // Split the loops into lines with different config, and fuzzy them separately
    fuzzified = polygon;
    for (const auto& r : fuzzified_regions) {
        const auto splitted = Algorithm::split_line(fuzzified, r.second, true);
        if (splitted.empty()) {
            // No intersection, skip
            continue;
        }

        // Fuzzy splitted polygon
        if (std::all_of(splitted.begin(), splitted.end(), [](const Algorithm::SplitLineJunction& j) { return j.clipped; })) {
            // The entire polygon is fuzzified
            fuzzy_polyline(fuzzified.points, true, slice_z, r.first, support);
        } else {
            Points segment;
            segment.reserve(splitted.size());
            fuzzified.points.clear();

            const auto fuzzy_current_segment = [&segment, &fuzzified, &r, slice_z, support]() {
                fuzzified.points.push_back(segment.front());
                const auto back = segment.back();
                fuzzy_polyline(segment, false, slice_z, r.first, support);
                fuzzified.points.insert(fuzzified.points.end(), segment.begin(), segment.end());
                fuzzified.points.push_back(back);
                segment.clear();
            };

            for (const auto& p : splitted) {
                if (p.clipped) {
                    segment.push_back(p.p);
                } else {
                    if (segment.empty()) {
                        fuzzified.points.push_back(p.p);
                    } else {
                        segment.push_back(p.p);
                        fuzzy_current_segment();
                    }
                }
            }
            if (!segment.empty()) {
                // Close the loop
                segment.push_back(splitted.front().p);
                fuzzy_current_segment();
            }
        }
    }

    return fuzzified;
}

void apply_fuzzy_skin(Arachne::ExtrusionLine* extrusion, const PerimeterGenerator& perimeter_generator, const bool is_contour)
{
    const auto  slice_z = perimeter_generator.slice_z;
    const auto  layer_height = perimeter_generator.layer_height;
    const auto& regions = perimeter_generator.regions_by_fuzzify;
    // Arachne builds only m_lower_slices_polygons (the nozzle-radius growth it clips overhangs
    // against); process_arachne never fills the width-indexed series that the classic generator
    // uses, so the support region here is that same set. It is the polygon set the Arachne overhang
    // split itself uses, which is the point: one definition of "overhang" for both.
    const Polygons* support = fuzzy_support_region(perimeter_generator, extrusion->inset_idx == 0);
    Polygons        arachne_support;
    if (support == nullptr && fuzzy_skip_overhangs_wanted(perimeter_generator)) {
        arachne_support = perimeter_generator.lower_slices_polygons();
        if (! arachne_support.empty())
            support = &arachne_support;
    }
    if (regions.size() == 1) { // optimization
        const auto& config  = regions.begin()->first;
        const bool  fuzzify = should_fuzzify(config, perimeter_generator.layer_id, extrusion->inset_idx, is_contour);
        if (fuzzify)
            fuzzy_extrusion_line(extrusion->junctions, slice_z, layer_height, config, support);
    } else {
        // Find all affective regions
        std::vector<std::pair<const FuzzySkinConfig&, const ExPolygons&>> fuzzified_regions;
        fuzzified_regions.reserve(regions.size());
        for (const auto& region : regions) {
            if (should_fuzzify(region.first, perimeter_generator.layer_id, extrusion->inset_idx, is_contour)) {
                fuzzified_regions.emplace_back(region.first, region.second);
            }
        }
        if (!fuzzified_regions.empty()) {
            // Split the loops into lines with different config, and fuzzy them separately
            for (const auto& r : fuzzified_regions) {
                const auto splitted = Algorithm::split_line(*extrusion, r.second, false);
                if (splitted.empty()) {
                    // No intersection, skip
                    continue;
                }

                // Fuzzy splitted extrusion
                if (std::all_of(splitted.begin(), splitted.end(), [](const Algorithm::SplitLineJunction& j) { return j.clipped; })) {
                    // The entire polygon is fuzzified
                    fuzzy_extrusion_line(extrusion->junctions, slice_z, layer_height, r.first, support);
                } else {
                    const auto                              current_ext = extrusion->junctions;
                    std::vector<Arachne::ExtrusionJunction> segment;
                    segment.reserve(current_ext.size());
                    extrusion->junctions.clear();

                    const auto fuzzy_current_segment = [&segment, &extrusion, &r, slice_z, layer_height, support]() {
                        extrusion->junctions.push_back(segment.front());
                        const auto back = segment.back();
                        fuzzy_extrusion_line(segment, slice_z, layer_height, r.first, support);
                        extrusion->junctions.insert(extrusion->junctions.end(), segment.begin(), segment.end());
                        extrusion->junctions.push_back(back);
                        segment.clear();
                    };

                    const auto to_ex_junction = [&current_ext](const Algorithm::SplitLineJunction& j) -> Arachne::ExtrusionJunction {
                        Arachne::ExtrusionJunction res = current_ext[j.get_src_index()];
                        if (!j.is_src()) {
                            res.p = j.p;
                        }
                        return res;
                    };

                    for (const auto& p : splitted) {
                        if (p.clipped) {
                            segment.push_back(to_ex_junction(p));
                        } else {
                            if (segment.empty()) {
                                extrusion->junctions.push_back(to_ex_junction(p));
                            } else {
                                segment.push_back(to_ex_junction(p));
                                fuzzy_current_segment();
                            }
                        }
                    }
                    if (!segment.empty()) {
                        fuzzy_current_segment();
                    }
                }
            }
        }
    }
}

} // namespace Slic3r::Feature::FuzzySkin
