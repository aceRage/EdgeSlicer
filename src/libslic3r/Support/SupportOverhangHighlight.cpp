// Support overhang highlight, see SupportOverhangHighlight.hpp and
// tests/plan_support_highlight_by_layer.md.

#include "SupportOverhangHighlight.hpp"

#include "../ClipperUtils.hpp"
#include "../Config.hpp"
#include "../Flow.hpp"
#include "../Geometry.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../Tesselate.hpp"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>

namespace Slic3r {

double SupportOverhangRule::lower_layer_offset(double lower_layer_height) const
{
    switch (source) {
    case Source::Overlap:
        // SupportMaterial.cpp: fw - support_threshold_overlap.get_abs_value(fw)
        return outer_wall_width - overlap_mm;
    case Source::Angle:
    case Source::TreeFallback:
    default:
        // SupportMaterial.cpp: lower_layer.height / tan(threshold_rad)
        // TreeSupport.cpp:     lower_layer->height / tan(threshold_rad)
        return lower_layer_height / std::tan(Geometry::deg2rad(angle_deg));
    }
}

double SupportOverhangRule::effective_angle_deg(double layer_height) const
{
    if (layer_height <= 0.)
        return angle_deg;
    // A planar slope at alpha from horizontal steps h / tan(alpha) per layer. The generator
    // supports the step when it is wider than the lower-layer offset, and normal supports also
    // drop it when it is narrower than the sliver filter.
    const double step_needed = std::max(this->lower_layer_offset(layer_height), this->min_band_width());
    if (step_needed <= 0.)
        return 90.;
    return Geometry::rad2deg(std::atan(layer_height / step_needed));
}

bool SupportOverhangRule::capped_by_min_band(double layer_height) const
{
    return layer_height > 0. && this->min_band_width() > this->lower_layer_offset(layer_height);
}

const std::vector<std::string> &support_overhang_rule_keys()
{
    static const std::vector<std::string> keys {
        "enable_support", "support_type", "support_threshold_angle", "support_threshold_overlap", "enforce_support_layers",
        "layer_height", "outer_wall_line_width", "line_width", "wall_filament", "outer_wall_filament",
    };
    return keys;
}

SupportOverhangRule support_overhang_rule(const ConfigBase &cfg)
{
    SupportOverhangRule r;
    const auto *opt_type = cfg.option<ConfigOptionEnum<SupportType>>("support_type");
    const SupportType type = opt_type ? opt_type->value : stNormalAuto;
    const auto *opt_enable = cfg.option<ConfigOptionBool>("enable_support");
    r.enable_support  = opt_enable && opt_enable->value;
    r.auto_support    = r.enable_support && is_auto(type);
    r.tree            = is_tree(type);
    const auto *opt_angle = cfg.option<ConfigOptionInt>("support_threshold_angle");
    r.threshold_angle = opt_angle ? opt_angle->value : 30;
    const auto *opt_enforce = cfg.option<ConfigOptionInt>("enforce_support_layers");
    r.enforce_layers  = opt_enforce ? opt_enforce->value : 0;
    const auto *opt_lh = cfg.option<ConfigOptionFloat>("layer_height");
    r.layer_height    = opt_lh ? opt_lh->value : 0.2;

    // Nozzles. Normal supports measure with the region's external perimeter flow
    // (PrintRegion::flow(frExternalPerimeter)): the outer wall filament (outer_wall_filament, 0 =
    // follow wall_filament), mapped to the physical extruder it prints through
    // (physical_extruder_for_filament, H2D/H2C: more filaments than nozzles).
    const auto  *nozzles      = cfg.option<ConfigOptionFloats>("nozzle_diameter");
    const size_t nozzle_count = nozzles ? nozzles->values.size() : 0;
    const double nozzle0      = nozzle_count > 0 ? nozzles->get_at(0) : 0.4;
    double       nozzle_wall  = nozzle0;
    if (nozzle_count > 0) {
        const auto  *filaments      = cfg.option<ConfigOptionFloats>("filament_diameter");
        const size_t filament_count = filaments ? filaments->values.size() : nozzle_count;
        const auto  *opt_wall       = cfg.option<ConfigOptionInt>("wall_filament");
        const auto  *opt_outer      = cfg.option<ConfigOptionInt>("outer_wall_filament");
        int          wall           = opt_wall ? opt_wall->value : 1;
        if (opt_outer && opt_outer->value > 0)
            wall = opt_outer->value;
        // PrintObject's clamp_exturder_to_default(): an out-of-range filament prints with the first.
        if (wall < 1 || size_t(wall) > std::max(filament_count, size_t(1)))
            wall = 1;
        const auto *fmap = cfg.option<ConfigOptionInts>("filament_map");
        static const std::vector<int> no_map;
        nozzle_wall = nozzles->get_at(physical_extruder_for_filament(nozzle_count, filament_count, fmap ? fmap->values : no_map,
                                                                     (unsigned int) wall));
    }

    ConfigOptionFloatOrPercent width(0., false);
    if (const auto *ow = cfg.option<ConfigOptionFloatOrPercent>("outer_wall_line_width"))
        width = *ow;
    if (width.value == 0)
        if (const auto *lw = cfg.option<ConfigOptionFloatOrPercent>("line_width"))
            width = *lw;
    // Flow::new_from_config_width(): 0 = automatic width.
    r.outer_wall_width = (! width.percent && width.value <= 0.) ?
        double(Flow::auto_extrusion_width(frExternalPerimeter, float(nozzle_wall))) :
        width.get_abs_value(nozzle_wall);
    // TreeSupport::detect_overhangs(): config.get_abs_value("line_width", nozzle_diameter.get_at(0)).
    if (const auto *lw = cfg.option<ConfigOptionFloatOrPercent>("line_width"))
        r.line_width = lw->get_abs_value(nozzle0);
    else
        r.line_width = nozzle0;

    if (r.threshold_angle > 0) {
        // Both generators: "+1 makes the threshold inclusive", and below 90.
        r.source    = SupportOverhangRule::Source::Angle;
        r.angle_deg = std::min(double(r.threshold_angle) + 1., 89.);
    } else if (r.tree) {
        // TreeSupport::detect_overhangs(): 30 degrees when the angle is 0; the overlap setting is
        // not read by any tree style (Orca #12367 hides it for trees).
        r.source    = SupportOverhangRule::Source::TreeFallback;
        r.angle_deg = 30.;
    } else {
        r.source = SupportOverhangRule::Source::Overlap;
        if (const auto *ov = cfg.option<ConfigOptionFloatOrPercent>("support_threshold_overlap"))
            r.overlap_mm = ov->get_abs_value(r.outer_wall_width);
        else
            r.overlap_mm = 0.5 * r.outer_wall_width;
        // For display only: the angle at the config's layer height. The real value depends on
        // the layer height, see effective_angle_deg().
        r.angle_deg = r.effective_angle_deg(r.layer_height);
    }
    return r;
}

DynamicPrintConfig support_overhang_rule_config(const DynamicPrintConfig &print_config,
                                                const DynamicPrintConfig &printer_config,
                                                const DynamicPrintConfig &project_config,
                                                size_t                    filament_count,
                                                const DynamicPrintConfig *object_config)
{
    DynamicPrintConfig out;
    for (const std::string &key : support_overhang_rule_keys())
        if (const ConfigOption *opt = print_config.option(key))
            out.set_key_value(key, opt->clone());
    if (const ConfigOption *opt = printer_config.option("nozzle_diameter"))
        out.set_key_value("nozzle_diameter", opt->clone());
    if (const ConfigOption *opt = project_config.option("filament_map"))
        out.set_key_value("filament_map", opt->clone());
    // Only the count matters (physical_extruder_for_filament).
    out.set_key_value("filament_diameter", new ConfigOptionFloats(std::max(filament_count, size_t(1)), 1.75));

    if (object_config != nullptr) {
        // PrintObject.cpp apply_to_print_region_config(): "extruder" assigns every wall filament
        // and the outer wall follows it, then the explicit filament keys win when > 0.
        if (const auto *ext = object_config->option<ConfigOptionInt>("extruder"); ext && ext->value != 0) {
            out.set_key_value("wall_filament", new ConfigOptionInt(ext->value));
            out.set_key_value("outer_wall_filament", new ConfigOptionInt(0));
        }
        for (const std::string &key : support_overhang_rule_keys()) {
            const ConfigOption *opt = object_config->option(key);
            if (opt == nullptr)
                continue;
            // keys_extruders: a 0 wall_filament means "default" and is ignored. outer_wall_filament
            // is not in that list, so its 0 ("follow the walls") is applied like any value.
            if (key == "wall_filament" && opt->getInt() <= 0)
                continue;
            out.set_key_value(key, opt->clone());
        }
    }
    return out;
}

SupportOverhangRule support_overhang_rule(const PrintObject &object)
{
    DynamicPrintConfig cfg;
    cfg.apply(object.print()->config(), true);
    cfg.apply(object.config(), true);
    if (object.num_printing_regions() > 0)
        cfg.apply(object.printing_region(0).config(), true);
    return support_overhang_rule(cfg);
}

// Same as SupportMaterial.cpp / TreeSupport.cpp (SUPPORT_SURFACES_OFFSET_PARAMETERS).
#define SUPPORT_OVERHANG_HL_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.

std::vector<ExPolygons> support_overhangs_from_slices(const std::vector<ExPolygons> &lslices,
                                                      const std::vector<double>     &heights,
                                                      const SupportOverhangRule     &rule)
{
    const size_t            n = lslices.size();
    std::vector<ExPolygons> out(n);
    if (n < 2 || heights.empty())
        return out;

    // Tree: "Filter out areas whose diameter that is smaller than extrusion_width".
    std::vector<ExPolygons> extrudable;
    if (rule.tree) {
        extrudable.resize(n);
        const float w = float(scale_(rule.line_width));
        tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t i = range.begin(); i < range.end(); ++ i)
                extrudable[i] = intersection_ex(lslices[i], offset2_ex(lslices[i], -w / 2, w));
        });
    }
    const std::vector<ExPolygons> &src = rule.tree ? extrudable : lslices;

    tbb::parallel_for(tbb::blocked_range<size_t>(1, n), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++ i) {
            const bool   enforced = int(i) < rule.enforce_layers;
            const double lower_h  = i - 1 < heights.size() ? heights[i - 1] : heights.back();
            if (rule.tree) {
                // TreeSupport::detect_overhangs(), "normal overhang".
                const double off = enforced ? -0.15 * rule.line_width : rule.lower_layer_offset(lower_h);
                out[i] = diff_ex(src[i], offset_ex(src[i - 1], float(scale_(off)), SUPPORT_OVERHANG_HL_OFFSET_PARAMETERS));
            } else {
                // SupportMaterial.cpp detect_overhangs(), auto normal support, one region.
                const double   off   = enforced ? 0. : rule.lower_layer_offset(lower_h);
                const Polygons cur   = to_polygons(src[i]);
                const Polygons lower = to_polygons(src[i - 1]);
                Polygons d;
                if (off <= 0.) {
                    d = diff(cur, lower);
                } else {
                    const float off_scaled = float(scale_(off));
                    d = diff(cur, expand(lower, off_scaled, SUPPORT_OVERHANG_HL_OFFSET_PARAMETERS));
                    if (! d.empty())
                        // "Offset the support regions back to a full overhang".
                        d = diff(intersection(expand(d, off_scaled, SUPPORT_OVERHANG_HL_OFFSET_PARAMETERS), cur), lower);
                }
                if (d.empty() || offset(d, -0.1f * float(scale_(rule.outer_wall_width))).empty())
                    continue;
                out[i] = union_ex(d);
            }
        }
    });
    return out;
}

std::vector<SupportOverhangOverlayLayer> support_overhang_overlay(const PrintObject &object)
{
    std::vector<SupportOverhangOverlayLayer> out;
    const size_t n = object.layer_count();
    if (n < 2)
        return out;

    const SupportOverhangRule rule = support_overhang_rule(object);
    std::vector<ExPolygons>   lslices(n);
    std::vector<double>       heights(n);
    for (size_t i = 0; i < n; ++ i) {
        lslices[i] = object.get_layer(int(i))->lslices;
        heights[i] = object.get_layer(int(i))->height;
    }
    const std::vector<ExPolygons> by_rule = support_overhangs_from_slices(lslices, heights, rule);

    // Tree supports + on build plate only: what lies above any lower layer gets no branch.
    const bool plate_only_tree = rule.tree && object.config().support_on_build_plate_only.value;
    std::vector<Polygons> covered;
    if (plate_only_tree) {
        covered.assign(n, {});
        for (size_t i = 1; i < n; ++ i)
            covered[i] = union_(covered[i - 1], to_polygons(lslices[i - 1]));
    }

    std::vector<SupportOverhangOverlayLayer> layers(n);
    const float tol = 0.1f * float(scale_(rule.outer_wall_width));
    tbb::parallel_for(tbb::blocked_range<size_t>(1, n), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++ i) {
            const Layer &layer = *object.get_layer(int(i));
            SupportOverhangOverlayLayer &l = layers[i];
            l.print_z   = layer.print_z;
            l.supported = layer.support_overhangs;
            if (plate_only_tree && ! l.supported.empty())
                l.supported = diff_ex(l.supported, covered[i]);
            if (! by_rule[i].empty()) {
                l.unsupported = l.supported.empty() ? by_rule[i] : diff_ex(by_rule[i], offset(l.supported, tol, SUPPORT_OVERHANG_HL_OFFSET_PARAMETERS));
                if (! l.unsupported.empty())
                    l.unsupported = opening_ex(l.unsupported, tol);
            }
            if (! l.supported.empty())
                l.supported_triangles = triangulate_expolygons_2f(l.supported, NORMALS_UP);
            if (! l.unsupported.empty())
                l.unsupported_triangles = triangulate_expolygons_2f(l.unsupported, NORMALS_UP);
        }
    });
    for (SupportOverhangOverlayLayer &l : layers)
        if (! l.supported.empty() || ! l.unsupported.empty())
            out.emplace_back(std::move(l));
    return out;
}

} // namespace Slic3r
