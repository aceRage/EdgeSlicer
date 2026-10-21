#ifndef slic3r_SandwichLite_hpp_
#define slic3r_SandwichLite_hpp_

// Sandwich-lite (spike): stack up to 3 thin top-surface (optionally bottom) sub-passes
// inside one nominal layer. Single-tool only. Reuses ExtrusionPath::z_offset (Local-Z / offset
// layers) for in-layer Z and stadium Flow::mm3_per_mm() for thinner-pass flow.
// Off by default. Do not enable in profiles. Do not merge.

#include "../ExtrusionEntity.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Flow.hpp"
#include "../Geometry.hpp"
#include "../PrintConfig.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Slic3r {

static constexpr double SANDWICH_LITE_MIN_HEIGHT = 0.04;

struct SandwichLitePass
{
    double height { 0. };
    double print_z { 0. };
    double angle_rad { 0. };
    double speed_factor { 1. };
    int    fan_percent { -1 };
};

inline bool sandwich_lite_requested(const PrintRegionConfig &cfg, ExtrusionRole role)
{
    if (!cfg.sandwich_lite_enable)
        return false;
    if (role == erTopSolidInfill)
        return true;
    return role == erBottomSurface && cfg.sandwich_lite_include_bottom;
}

// Spike is single-tool only. Callers fall back to a normal fill when any of these is true.
inline const char *sandwich_lite_unsupported_reason(bool image_row, bool wipe_tower, size_t extruder_count)
{
    if (image_row)
        return "image-row top infill";
    if (wipe_tower)
        return "wipe tower";
    if (extruder_count > 1)
        return "more than one extruder";
    return nullptr;
}

// Equal-share fallback, then user ratios, then drop the feature if a pass would be < 0.04 mm.
inline std::vector<SandwichLitePass> sandwich_lite_plan(const PrintRegionConfig &cfg,
                                                        ExtrusionRole            role,
                                                        double                   layer_height,
                                                        double                   layer_print_z)
{
    std::vector<SandwichLitePass> out;
    if (!sandwich_lite_requested(cfg, role))
        return out;
    if (layer_height + EPSILON < 2. * SANDWICH_LITE_MIN_HEIGHT)
        return out;

    int n = std::clamp(cfg.sandwich_lite_pass_count.value, 2, 3);
    while (n > 1 && layer_height / double(n) + EPSILON < SANDWICH_LITE_MIN_HEIGHT)
        --n;
    if (n < 2)
        return out;

    std::vector<double> ratios(size_t(n), 1.);
    double              sum = 0.;
    for (int i = 0; i < n; ++i) {
        ratios[size_t(i)] = std::max(0., cfg.sandwich_lite_height_ratios.get_at(size_t(i)));
        sum += ratios[size_t(i)];
    }
    if (sum <= EPSILON) {
        for (double &r : ratios)
            r = 1.;
        sum = double(n);
    }
    for (double &r : ratios)
        r /= sum;

    auto too_thin = [layer_height](double ratio) {
        return ratio * layer_height + EPSILON < SANDWICH_LITE_MIN_HEIGHT;
    };
    while (n > 1 && too_thin(ratios[size_t(n - 1)])) {
        --n;
        ratios.resize(size_t(n));
        sum = 0.;
        for (double r : ratios)
            sum += r;
        if (sum <= EPSILON)
            return {};
        for (double &r : ratios)
            r /= sum;
    }
    if (n < 2)
        return out;
    for (double r : ratios)
        if (too_thin(r))
            return {};

    double cum = 0.;
    out.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        SandwichLitePass pass;
        if (i + 1 == n)
            pass.height = layer_height - cum;
        else
            pass.height = ratios[size_t(i)] * layer_height;
        if (pass.height + EPSILON < SANDWICH_LITE_MIN_HEIGHT)
            return {};
        cum += pass.height;
        pass.print_z      = layer_print_z - layer_height + cum;
        pass.angle_rad    = Geometry::deg2rad(cfg.sandwich_lite_angles.get_at(size_t(i)));
        const double pct  = cfg.sandwich_lite_speed_percents.get_at(size_t(i));
        pass.speed_factor = (pct <= 0.) ? 1. : pct * 0.01;
        pass.fan_percent  = cfg.sandwich_lite_fan_speeds.get_at(size_t(i));
        out.push_back(pass);
    }
    if (!out.empty()) {
        out.back().print_z = layer_print_z;
        out.back().height  = layer_height - (cum - out.back().height);
    }
    return out;
}

inline double sandwich_lite_stadium_mm3_per_mm(double width, double height, double nozzle)
{
    return Flow(float(width), float(height), float(nozzle)).mm3_per_mm();
}

inline void sandwich_lite_scale_path(ExtrusionPath          &path,
                                     const SandwichLitePass &pass,
                                     double                  layer_print_z,
                                     double                  nozzle)
{
    const double h0 = path.height;
    if (h0 > EPSILON && pass.height > EPSILON) {
        try {
            const double a_full = sandwich_lite_stadium_mm3_per_mm(path.width, h0, nozzle);
            const double a_sub  = sandwich_lite_stadium_mm3_per_mm(path.width, pass.height, nozzle);
            if (a_full > EPSILON)
                path.mm3_per_mm *= a_sub / a_full;
        } catch (...) {
            path.mm3_per_mm *= pass.height / h0;
        }
    }
    path.height                = float(pass.height);
    path.z_offset              = float((pass.print_z - layer_print_z) / pass.height);
    path.sandwich_speed_factor = float(pass.speed_factor);
    path.sandwich_fan_percent  = pass.fan_percent;
}

inline void sandwich_lite_tag_entity(ExtrusionEntity        *ee,
                                     const SandwichLitePass &pass,
                                     double                  layer_print_z,
                                     double                  nozzle)
{
    if (ee == nullptr)
        return;
    if (auto *path = dynamic_cast<ExtrusionPath *>(ee)) {
        sandwich_lite_scale_path(*path, pass, layer_print_z, nozzle);
        return;
    }
    if (auto *mp = dynamic_cast<ExtrusionMultiPath *>(ee)) {
        for (ExtrusionPath &p : mp->paths)
            sandwich_lite_scale_path(p, pass, layer_print_z, nozzle);
        return;
    }
    if (auto *loop = dynamic_cast<ExtrusionLoop *>(ee)) {
        for (ExtrusionPath &p : loop->paths)
            sandwich_lite_scale_path(p, pass, layer_print_z, nozzle);
        return;
    }
    if (auto *col = dynamic_cast<ExtrusionEntityCollection *>(ee)) {
        for (ExtrusionEntity *child : col->entities)
            sandwich_lite_tag_entity(child, pass, layer_print_z, nozzle);
    }
}

} // namespace Slic3r

#endif
