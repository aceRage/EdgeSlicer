// Support overhang highlight, see SupportOverhangHighlight.hpp and
// tests/plan_support_highlight_by_layer.md.

#include "SupportOverhangHighlight.hpp"
#include "SupportOverhangDetect.hpp"

#include "../ClipperUtils.hpp"
#include "../Config.hpp"
#include "../Flow.hpp"
#include "../Geometry.hpp"
#include "../Layer.hpp"
#include "../Model.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../Tesselate.hpp"
#include "../TriangleMeshSlicer.hpp"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <chrono>
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
        r.angle_deg = r.tree ? tree_support_threshold_angle(r.threshold_angle) : normal_support_threshold_angle(r.threshold_angle);
    } else if (r.tree) {
        // TreeSupport::detect_overhangs(): 30 degrees when the angle is 0; the overlap setting is
        // not read by any tree style (Orca #12367 hides it for trees).
        r.source    = SupportOverhangRule::Source::TreeFallback;
        r.angle_deg = 30.;
    } else {
        r.source = SupportOverhangRule::Source::Overlap;
        if (const auto *ov = cfg.option<ConfigOptionFloatOrPercent>("support_threshold_overlap")) {
            r.overlap_mm      = ov->get_abs_value(r.outer_wall_width);
            r.overlap_value   = ov->value;
            r.overlap_percent = ov->percent;
        } else
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

    // Phase 4: the generators' own per-layer decision (Support/SupportOverhangDetect.cpp), with the
    // same inputs they compute: the threshold, the line width, the external perimeter width.
    if (rule.tree) {
        // TreeSupport::detect_overhangs().
        const coordf_t extrusion_width        = rule.line_width;
        const coordf_t extrusion_width_scaled = scale_(extrusion_width);
        const double   threshold_rad          = Geometry::deg2rad(tree_support_threshold_angle(rule.threshold_angle));
        std::vector<ExPolygons> extrudable(n);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t i = range.begin(); i < range.end(); ++ i)
                extrudable[i] = tree_support_extrudable(lslices[i], extrusion_width_scaled);
        });
        tbb::parallel_for(tbb::blocked_range<size_t>(1, n), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t i = range.begin(); i < range.end(); ++ i) {
                const coordf_t lower_h               = i - 1 < heights.size() ? heights[i - 1] : heights.back();
                const coordf_t lower_layer_offset    = tree_support_lower_layer_offset(i, rule.enforce_layers, extrusion_width, lower_h, threshold_rad);
                const coordf_t support_offset_scaled = scale_(lower_layer_offset);
                ExPolygons     lower_offseted;
                out[i] = tree_support_layer_overhangs(extrudable[i], extrudable[i - 1], support_offset_scaled, lower_offseted);
            }
        });
    } else {
        // SupportMaterial.cpp detect_overhangs(), automatic normal supports, one region. The
        // external perimeter width as LayerRegion::flow(frExternalPerimeter).scaled_width() gives it.
        const float  width         = float(rule.outer_wall_width);
        const float  fw            = float(coord_t(scale_(width)));
        const double threshold_rad = Geometry::deg2rad(normal_support_threshold_angle(rule.threshold_angle));
        const ConfigOptionFloatOrPercent overlap(rule.overlap_value, rule.overlap_percent);
        static const Polygons no_buildplate_covered;
        tbb::parallel_for(tbb::blocked_range<size_t>(1, n), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t i = range.begin(); i < range.end(); ++ i) {
                const double   lower_h            = i - 1 < heights.size() ? heights[i - 1] : heights.back();
                const float    lower_layer_offset = normal_support_lower_layer_offset(i, rule.enforce_layers, threshold_rad, lower_h, fw, overlap);
                const Polygons d = normal_support_region_overhangs(to_polygons(lslices[i]), to_polygons(lslices[i - 1]), lower_layer_offset,
                                                                   /*auto_normal_support*/ true, /*buildplate_only*/ false, no_buildplate_covered);
                if (normal_support_overhang_too_thin(d, fw))
                    continue;
                out[i] = union_ex(d);
            }
        });
    }
    return out;
}

// Every object layer (index = layer id; layer 0 empty), triangulated or not.
static std::vector<SupportOverhangOverlayLayer> support_overhang_layers(const PrintObject &object, bool triangulate)
{
    const size_t n = object.layer_count();
    if (n < 2)
        return {};

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
            if (triangulate && ! l.supported.empty())
                l.supported_triangles = triangulate_expolygons_2f(l.supported, NORMALS_UP);
            if (triangulate && ! l.unsupported.empty())
                l.unsupported_triangles = triangulate_expolygons_2f(l.unsupported, NORMALS_UP);
        }
    });
    if (n > 0)
        layers.front().print_z = object.get_layer(0)->print_z;
    return layers;
}

std::vector<SupportOverhangOverlayLayer> support_overhang_overlay(const PrintObject &object)
{
    std::vector<SupportOverhangOverlayLayer> out;
    for (SupportOverhangOverlayLayer &l : support_overhang_layers(object, true))
        if (! l.supported.empty() || ! l.unsupported.empty())
            out.emplace_back(std::move(l));
    return out;
}


// ---------------------------------------------------------------------------------------------
// Phase 3: before slicing
// ---------------------------------------------------------------------------------------------

namespace support_hl_detail {

// Clip a triangle to lo <= z <= hi and return the XY outline of the remainder (Sutherland-Hodgman
// against the two planes; at most 5 points).
static void clip_triangle_to_slab(const Vec3f (&tri)[3], float lo, float hi, std::vector<Vec2d> &out)
{
    std::vector<Vec3d> poly { tri[0].cast<double>(), tri[1].cast<double>(), tri[2].cast<double>() };
    std::vector<Vec3d> next;
    auto clip = [&](double plane, bool keep_above) {
        next.clear();
        const size_t m = poly.size();
        for (size_t k = 0; k < m; ++ k) {
            const Vec3d &a = poly[k];
            const Vec3d &b = poly[(k + 1) % m];
            const bool   ina = keep_above ? a.z() >= plane : a.z() <= plane;
            const bool   inb = keep_above ? b.z() >= plane : b.z() <= plane;
            if (ina)
                next.push_back(a);
            if (ina != inb) {
                const double t = (plane - a.z()) / (b.z() - a.z());
                next.push_back(a + t * (b - a));
            }
        }
        poly.swap(next);
    };
    clip(lo, true);
    if (poly.size() >= 3)
        clip(hi, false);
    out.clear();
    if (poly.size() >= 3)
        for (const Vec3d &p : poly)
            out.emplace_back(p.x(), p.y());
}

// Point-in-polygons (nonzero winding) with the edges bucketed in horizontal rows, so one query
// only looks at the few edges crossing its row. A Clipper intersection per facet piece walks the
// whole layer outline every time and was the bottleneck on large meshes.
struct RowLocator
{
    BoundingBox bbox;
    double      y0 { 0. }, row_h { 1. };
    std::vector<std::vector<std::array<double, 4>>> rows;

    void build(const Polygons &polys)
    {
        rows.clear();
        bbox = get_extents(polys);
        if (! bbox.defined)
            return;
        y0 = double(bbox.min.y());
        const double height = double(bbox.max.y() - bbox.min.y()) + 1.;
        size_t nedges = 0;
        for (const Polygon &p : polys)
            nedges += p.points.size();
        const size_t nrows = std::clamp<size_t>(nedges / 4, 1, 4096);
        row_h = height / double(nrows);
        rows.assign(nrows, {});
        for (const Polygon &p : polys) {
            const size_t m = p.points.size();
            for (size_t k = 0; k < m; ++ k) {
                const Point &a = p.points[k];
                const Point &b = p.points[(k + 1) % m];
                if (a.y() == b.y())
                    continue;
                const double ylo = double(std::min(a.y(), b.y())), yhi = double(std::max(a.y(), b.y()));
                const size_t r0 = size_t(std::max(0., std::floor((ylo - y0) / row_h)));
                const size_t r1 = std::min(nrows - 1, size_t(std::max(0., std::floor((yhi - y0) / row_h))));
                for (size_t r = r0; r <= r1; ++ r)
                    rows[r].push_back({ double(a.x()), double(a.y()), double(b.x()), double(b.y()) });
            }
        }
    }

    bool inside(double x, double y) const
    {
        if (rows.empty() || x < bbox.min.x() || x > bbox.max.x() || y < bbox.min.y() || y > bbox.max.y())
            return false;
        const size_t r = std::min(rows.size() - 1, size_t(std::max(0., std::floor((y - y0) / row_h))));
        int winding = 0;
        for (const std::array<double, 4> &e : rows[r]) {
            const double ax = e[0], ay = e[1], bx = e[2], by = e[3];
            const double side = (bx - ax) * (y - ay) - (x - ax) * (by - ay);
            if (ay <= y) {
                if (by > y && side > 0.)
                    ++ winding;
            } else if (by <= y && side < 0.)
                -- winding;
        }
        return winding != 0;
    }
};

} // namespace support_hl_detail

std::vector<uint8_t> overhang_facets_from_layers(const indexed_triangle_set    &its,
                                                 const Transform3d             &trafo,
                                                 const std::vector<float>      &slice_z,
                                                 const std::vector<ExPolygons> &areas,
                                                 const std::vector<double>     &grow,
                                                 double                         min_coverage)
{
    std::vector<uint8_t> flags(its.indices.size(), 0);
    const size_t         n = std::min(slice_z.size(), areas.size());
    if (n < 2)
        return flags;

    bool any = false;
    for (size_t i = 1; i < n && ! any; ++ i)
        any = ! areas[i].empty();
    if (! any)
        return flags;

    std::vector<support_hl_detail::RowLocator> locators(n);
    tbb::parallel_for(tbb::blocked_range<size_t>(1, n), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++ i) {
            if (areas[i].empty())
                continue;
            const double g = i < grow.size() ? grow[i] : 0.;
            locators[i].build(g > 0. ? offset(areas[i], float(scale_(g)), SUPPORT_OVERHANG_HL_OFFSET_PARAMETERS) : to_polygons(areas[i]));
        }
    });

    // World vertices once; a mirroring transformation flips the winding, so the normal too.
    std::vector<Vec3f> vertices(its.vertices.size());
    for (size_t k = 0; k < its.vertices.size(); ++ k)
        vertices[k] = (trafo * its.vertices[k].cast<double>()).cast<float>();
    const bool mirrored = trafo.matrix().block<3, 3>(0, 0).determinant() < 0.;

    tbb::parallel_for(tbb::blocked_range<size_t>(0, its.indices.size()), [&](const tbb::blocked_range<size_t> &range) {
        std::vector<Vec2d> piece;
        for (size_t f = range.begin(); f < range.end(); ++ f) {
            const stl_triangle_vertex_indices &idx = its.indices[f];
            const Vec3f tri[3] = { vertices[idx(0)], vertices[idx(1)], vertices[idx(2)] };
            Vec3f       nrm    = (tri[1] - tri[0]).cross(tri[2] - tri[0]);
            if (mirrored)
                nrm = -nrm;
            const float len = nrm.norm();
            if (len <= 0.f || nrm.z() >= -1e-6f * len)
                continue; // degenerate, upward or vertical
            const float zmin = std::min({ tri[0].z(), tri[1].z(), tri[2].z() });
            const float zmax = std::max({ tri[0].z(), tri[1].z(), tri[2].z() });
            const bool  horizontal = zmax - zmin < 1e-5f;

            // Layer i's overhang is made by the facets between slice_z[i-1] and slice_z[i]. A
            // downward horizontal facet lying on a slicing plane belongs to the slab above it
            // (TriangleMeshSlicer convention).
            size_t i0 = size_t(std::upper_bound(slice_z.begin(), slice_z.begin() + n, zmin) - slice_z.begin());
            size_t i1 = horizontal ? i0 : size_t(std::lower_bound(slice_z.begin(), slice_z.begin() + n, zmax) - slice_z.begin());
            i0 = std::max<size_t>(i0, 1);
            i1 = std::min(i1, n - 1);
            double total = 0., covered = 0.;
            for (size_t i = i0; i <= i1 && i0 <= i1; ++ i) {
                if (horizontal) {
                    piece = { tri[0].head<2>().cast<double>(), tri[1].head<2>().cast<double>(), tri[2].head<2>().cast<double>() };
                } else {
                    support_hl_detail::clip_triangle_to_slab(tri, slice_z[i - 1], slice_z[i], piece);
                    if (piece.size() < 3)
                        continue;
                }
                // Area of the (convex) piece and its sample points: the centroid and the midpoints
                // between the centroid and each corner.
                double a = 0.;
                Vec2d  c = Vec2d::Zero();
                for (size_t k = 0; k < piece.size(); ++ k) {
                    const Vec2d &p = piece[k], &q = piece[(k + 1) % piece.size()];
                    a += p.x() * q.y() - q.x() * p.y();
                    c += p;
                }
                a = std::abs(0.5 * a);
                if (a <= 0.)
                    continue;
                total += a;
                const support_hl_detail::RowLocator &loc = locators[i];
                if (loc.rows.empty())
                    continue;
                c /= double(piece.size());
                size_t hits = 0;
                auto test = [&](const Vec2d &p) { hits += loc.inside(scale_(p.x()), scale_(p.y())) ? 1 : 0; };
                test(c);
                for (const Vec2d &p : piece)
                    test(0.5 * (c + p));
                covered += a * double(hits) / double(piece.size() + 1);
            }
            if (total > 0. && covered >= min_coverage * total)
                flags[f] = 1;
        }
    });
    return flags;
}

size_t SupportOverhangPreview::layers_with_overhangs() const
{
    size_t cnt = 0;
    for (size_t i = 0; i < supported.size(); ++ i)
        cnt += (! supported[i].empty() || (i < unsupported.size() && ! unsupported[i].empty())) ? 1 : 0;
    return cnt;
}

SupportOverhangPreview support_overhang_preview(const SupportOverhangPreviewInput &input, const std::function<void()> &throw_on_cancel)
{
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    auto cancel = [&throw_on_cancel]() { if (throw_on_cancel) throw_on_cancel(); };

    SupportOverhangPreview out;
    out.part_facets.assign(input.parts.size(), {});
    const size_t n = input.layer_zs.size() / 2;
    if (n < 2)
        return out;

    std::vector<double> heights(n);
    out.slice_z.resize(n);
    out.print_z.resize(n);
    for (size_t i = 0; i < n; ++ i) {
        const double lo = input.layer_zs[2 * i], hi = input.layer_zs[2 * i + 1];
        heights[i]     = hi - lo;
        out.slice_z[i] = float(0.5 * (lo + hi));
        out.print_z[i] = hi;
    }

    // 1. Slice the parts at the object's own layers, negative volumes cut away.
    const auto t0 = clock::now();
    std::vector<ExPolygons> lslices(n), negative(n);
    bool has_negative = false;
    MeshSlicingParamsEx params;
    params.closing_radius = input.closing_radius;
    for (const SupportOverhangPreviewInput::Part &part : input.parts) {
        if (! part.its || part.its->indices.empty())
            continue;
        params.trafo = part.trafo;
        std::vector<ExPolygons> s = slice_mesh_ex(*part.its, out.slice_z, params, [&cancel]() { cancel(); });
        std::vector<ExPolygons> &dst = part.negative ? negative : lslices;
        has_negative |= part.negative;
        for (size_t i = 0; i < n && i < s.size(); ++ i)
            append(dst[i], std::move(s[i]));
    }
    cancel();
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++ i) {
            lslices[i] = union_ex(lslices[i]);
            if (has_negative && ! negative[i].empty())
                lslices[i] = diff_ex(lslices[i], union_ex(negative[i]));
        }
    });
    const auto t1 = clock::now();
    cancel();

    // 2. The rule, floating islands, then who holds what up.
    std::vector<ExPolygons> overhangs = support_overhangs_from_slices(lslices, heights, input.rule);
    cancel();
    std::vector<Polygons> covered;
    if (input.build_plate_only && input.rule.auto_support) {
        covered.assign(n, {});
        for (size_t i = 1; i < n; ++ i)
            covered[i] = union_(covered[i - 1], to_polygons(lslices[i - 1]));
    }
    out.supported.assign(n, {});
    out.unsupported.assign(n, {});
    tbb::parallel_for(tbb::blocked_range<size_t>(1, n), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++ i) {
            ExPolygons o = std::move(overhangs[i]);
            // Sharp tails: an island with nothing under it falls without support, whatever its slope.
            bool floating = false;
            for (const ExPolygon &island : lslices[i])
                if (intersection_ex(ExPolygons{ island }, lslices[i - 1]).empty()) {
                    o.push_back(island);
                    floating = true;
                }
            if (floating)
                o = union_ex(o);
            if (o.empty())
                continue;
            if (! input.rule.auto_support) {
                out.unsupported[i] = std::move(o);
            } else if (! covered.empty() && ! covered[i].empty()) {
                out.unsupported[i] = intersection_ex(o, covered[i]);
                out.supported[i]   = diff_ex(o, covered[i]);
            } else {
                out.supported[i] = std::move(o);
            }
        }
    });
    const auto t2 = clock::now();
    cancel();

    // 3. Back onto the facets. A tree overhang is the band past the grown lower layer: grow it
    // back so the whole facet strip that makes it is covered.
    std::vector<double> grow;
    if (input.rule.tree) {
        grow.assign(n, 0.);
        for (size_t i = 1; i < n; ++ i)
            grow[i] = std::max(0., input.rule.lower_layer_offset(heights[i - 1]));
    }
    for (size_t p = 0; p < input.parts.size(); ++ p) {
        const SupportOverhangPreviewInput::Part &part = input.parts[p];
        if (part.negative || ! part.its)
            continue;
        std::vector<uint8_t> flags = overhang_facets_from_layers(*part.its, part.trafo, out.slice_z, out.supported, grow);
        cancel();
        const std::vector<uint8_t> amber = overhang_facets_from_layers(*part.its, part.trafo, out.slice_z, out.unsupported, grow);
        for (size_t f = 0; f < flags.size(); ++ f)
            if (flags[f] == 0 && amber[f] != 0)
                flags[f] = 2;
        out.part_facets[p] = std::move(flags);
        cancel();
    }
    const auto t3 = clock::now();
    out.slice_ms  = ms(t0, t1);
    out.detect_ms = ms(t1, t2);
    out.map_ms    = ms(t2, t3);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Phase 5: the generators' own decision before slicing
// ---------------------------------------------------------------------------------------------

void support_overhang_simulate_auto(DynamicPrintConfig &config, bool add_missing)
{
    if (add_missing || config.has("enable_support"))
        config.set_key_value("enable_support", new ConfigOptionBool(true));
    if (const auto *type = config.option<ConfigOptionEnum<SupportType>>("support_type")) {
        if (! is_auto(type->value))
            config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(is_tree(type->value) ? stTreeAuto : stNormalAuto));
    } else if (add_missing)
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
}

SupportOverhangPreview support_overhang_preview_exact(Print &print, const Model &model_in, const DynamicPrintConfig &config_in)
{
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };

    SupportOverhangPreview out;
    if (model_in.objects.empty() || model_in.objects.front()->instances.empty())
        return out;

    // Simulate automatic supports with every other support setting as configured, the object's
    // overrides included: the generators run their detection only when supports are on and auto.
    Model model(model_in);
    DynamicPrintConfig config = config_in;
    support_overhang_simulate_auto(config, true);
    for (ModelObject *mo : model.objects) {
        DynamicPrintConfig object_config = mo->config.get();
        support_overhang_simulate_auto(object_config, false);
        mo->config.assign_config(std::move(object_config));
    }

    const auto t0 = clock::now();
    print.apply(model, config);
    print.set_status_silent();
    if (print.objects().empty())
        return out;
    PrintObject &object = *print.get_object(0);
    object.detect_support_overhangs_only();
    const auto t1 = clock::now();

    // What the generator holds up and what the rule flags that it leaves, as the preview overlay.
    std::vector<SupportOverhangOverlayLayer> layers = support_overhang_layers(object, false);
    const size_t n = object.layer_count();
    const double zmin = object.slicing_parameters().object_print_z_min;
    const Point  shift = object.instances().empty() ? Point(0, 0) : object.instances().front().shift;
    out.slice_z.resize(n);
    out.print_z.resize(n);
    out.supported.assign(n, {});
    out.unsupported.assign(n, {});
    std::vector<double> heights(n);
    for (size_t i = 0; i < n; ++ i) {
        const Layer &layer = *object.get_layer(int(i));
        out.slice_z[i] = float(layer.slice_z);
        out.print_z[i] = layer.print_z - zmin;
        heights[i]     = layer.height;
        if (i < layers.size()) {
            out.supported[i]   = std::move(layers[i].supported);
            out.unsupported[i] = std::move(layers[i].unsupported);
            for (ExPolygon &e : out.supported[i])
                e.translate(shift);
            for (ExPolygon &e : out.unsupported[i])
                e.translate(shift);
        }
    }
    const auto t2 = clock::now();
    if (print.canceled()) throw CanceledException();

    // Back onto the faces of every model part, placed by the one instance.
    const SupportOverhangRule rule = support_overhang_rule(object);
    std::vector<double> grow;
    if (rule.tree) {
        // A tree overhang is the band past the grown lower layer: grow it back over its facet strip.
        grow.assign(n, 0.);
        for (size_t i = 1; i < n; ++ i)
            grow[i] = std::max(0., rule.lower_layer_offset(heights[i - 1]));
    }
    const ModelObject   &mo       = *model_in.objects.front();
    const Transform3d    instance = mo.instances.front()->get_matrix();
    out.part_facets.assign(mo.volumes.size(), {});
    for (size_t v = 0; v < mo.volumes.size(); ++ v) {
        const ModelVolume *mv = mo.volumes[v];
        if (! mv->is_model_part())
            continue;
        const Transform3d trafo = instance * mv->get_matrix();
        std::vector<uint8_t> flags = overhang_facets_from_layers(mv->mesh().its, trafo, out.slice_z, out.supported, grow);
        const std::vector<uint8_t> amber = overhang_facets_from_layers(mv->mesh().its, trafo, out.slice_z, out.unsupported, grow);
        for (size_t f = 0; f < flags.size(); ++ f)
            if (flags[f] == 0 && amber[f] != 0)
                flags[f] = 2;
        out.part_facets[v] = std::move(flags);
        if (print.canceled()) throw CanceledException();
    }
    const auto t3 = clock::now();
    out.slice_ms  = ms(t0, t1);
    out.detect_ms = ms(t1, t2);
    out.map_ms    = ms(t2, t3);
    return out;
}

} // namespace Slic3r
