// Support fins (see Fins.hpp). The technique is Slant3D's designed-in supports, as specified by Support
// Fins (gittrahan/support-fins, printfins.com, MIT); this is an independent native implementation.

#include "Fins.hpp"

#include "../ClipperUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Flow.hpp"
#include "../Layer.hpp"
#include "../Model.hpp"
#include "../Print.hpp"
#include "../TriangleSelector.hpp"
#include "SupportLayerAt.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace Slic3r {

namespace fins {

// --- settings and layers -------------------------------------------------------------------------

FinSettings FinSettings::from_config(const PrintObjectConfig &cfg, double support_line_width)
{
    FinSettings s;
    s.mode           = cfg.support_fins.value;
    s.line_width     = support_line_width > EPSILON ? support_line_width : 0.42;
    // Two lines at least, or the wall is a single wobbly bead.
    s.thickness      = std::max(cfg.support_fin_thickness.value, 2. * s.line_width);
    s.depth          = std::max(1., cfg.support_fin_depth.value);
    s.height         = std::max(0., cfg.support_fin_height.value);
    s.spacing        = std::max(2., cfg.support_fin_spacing.value);
    s.gap            = std::max(0.1, cfg.support_fin_gap.value);
    s.tine_spacing   = std::max(0.5, cfg.support_fin_tine_spacing.value);
    s.tine_depth     = std::clamp(cfg.support_fin_tine_depth.value, 0., 2.);
    s.tine_base_rows = cfg.support_fin_tine_base_rows.value;
    s.lean_side_only = cfg.support_fin_lean_side_only.value;
    return s;
}

FinSettings settings_of(const PrintObject &object)
{
    return FinSettings::from_config(object.config(), support_material_flow(&object).width());
}

std::vector<FinLayer> layers_of(const PrintObject &object)
{
    std::vector<FinLayer> out;
    out.reserve(object.layers().size());
    for (const Layer *layer : object.layers())
        out.push_back({ layer->print_z, layer->height, &layer->lslices });
    return out;
}

double Fin::outer_at(double z) const
{
    // The 45 degree slope starts where it reaches the part's edge exactly at top_z.
    return reach - std::max(0., z - (top_z - (reach - edge)));
}

double Fin::half_thickness_at(const FinSettings &settings, double z) const
{
    return 0.5 * settings.thickness + std::clamp(FOOT_HEIGHT - z, 0., FOOT_GROWTH);
}

// --- geometry helpers ----------------------------------------------------------------------------

// The fin's band from t0 to t1 along its centre line, `half` to either side of it.
static ExPolygon band(const Fin &f, double t0, double t1, double half)
{
    const Vec2d p = f.perp();
    Polygon     poly;
    for (const Vec2d &v : { Vec2d(f.at(t0) - p * half), Vec2d(f.at(t1) - p * half), Vec2d(f.at(t1) + p * half),
                            Vec2d(f.at(t0) + p * half) })
        poly.points.emplace_back(scaled(v.x()), scaled(v.y()));
    if (poly.is_clockwise())
        poly.reverse();
    return ExPolygon(std::move(poly));
}

// The range of t (along the fin's centre line) the outlines' contour points span. False when empty.
static bool t_range(const Fin &f, const ExPolygons &expolys, double &tmin, double &tmax)
{
    tmin = std::numeric_limits<double>::max();
    tmax = std::numeric_limits<double>::lowest();
    for (const ExPolygon &e : expolys)
        for (const Point &pt : e.contour.points) {
            const double t = (unscaled(pt) - f.origin).dot(f.dir);
            tmin = std::min(tmin, t);
            tmax = std::max(tmax, t);
        }
    return tmin <= tmax;
}

// Area (mm^2) and area centroid (mm) of the outlines, holes taken out.
static double area_centroid(const ExPolygons &expolys, Vec2d &centroid)
{
    double a_sum = 0.;
    Vec2d  c_sum = Vec2d::Zero();
    auto   add   = [&](const Polygon &poly, double sign) {
        if (poly.size() < 3)
            return;
        const double a = sign * std::abs(unscaled(unscaled(poly.area())));
        a_sum += a;
        c_sum += a * unscaled(poly.centroid());
    };
    for (const ExPolygon &e : expolys) {
        add(e.contour, 1.);
        for (const Polygon &h : e.holes)
            add(h, -1.);
    }
    centroid = a_sum > EPSILON ? Vec2d(c_sum / a_sum) : Vec2d::Zero();
    return a_sum;
}

// The minor axis (unit) of the points' spread and how elongated they are (major / minor variance).
static Vec2d minor_axis(const std::vector<Vec2d> &pts, double &ratio)
{
    ratio = 1.;
    if (pts.size() < 2)
        return Vec2d(0., 1.);
    Vec2d mean = Vec2d::Zero();
    for (const Vec2d &p : pts)
        mean += p;
    mean /= double(pts.size());
    double cxx = 0., cxy = 0., cyy = 0.;
    for (const Vec2d &p : pts) {
        const Vec2d d = p - mean;
        cxx += d.x() * d.x();
        cxy += d.x() * d.y();
        cyy += d.y() * d.y();
    }
    const double theta = 0.5 * std::atan2(2. * cxy, cxx - cyy);
    const double tr    = cxx + cyy;
    const double det   = cxx * cyy - cxy * cxy;
    const double disc  = std::sqrt(std::max(0., 0.25 * tr * tr - det));
    const double l1    = 0.5 * tr + disc;
    const double l2    = std::max(0.5 * tr - disc, 1e-12);
    ratio              = l1 / l2;
    // theta is the major axis; the minor one is square to it.
    return Vec2d(-std::sin(theta), std::cos(theta));
}

static void append_points(const ExPolygons &expolys, std::vector<Vec2d> &pts)
{
    for (const ExPolygon &e : expolys)
        for (const Point &p : e.contour.points)
            pts.push_back(unscaled(p));
}

// Where the fin's band first meets the part from the bed up (to top_z): its edge there sets the fin's
// reach. False when the band never meets the part.
static bool complete_fin(const std::vector<FinLayer> &layers, const FinSettings &st, Fin &f, double top_z)
{
    const ExPolygons b{ band(f, 0., 1000., 0.5 * st.thickness) };
    for (const FinLayer &l : layers) {
        if (l.print_z > top_z + EPSILON)
            break;
        if (l.islands == nullptr || l.islands->empty())
            continue;
        const ExPolygons hit = intersection_ex(*l.islands, b);
        double           tmin, tmax;
        if (hit.empty() || ! t_range(f, hit, tmin, tmax))
            continue;
        f.edge  = tmax;
        f.reach = tmax + st.gap + st.depth;
        f.top_z = top_z;
        return true;
    }
    return false;
}

// --- planner -------------------------------------------------------------------------------------

std::vector<size_t> tine_layers(const std::vector<FinLayer> &layers, const FinSettings &st, double top_z)
{
    std::vector<size_t> out;
    long   last_i = -2;
    double last_z = 0.;
    for (size_t i = 1; i < layers.size(); ++i) {
        const double z = layers[i].print_z;
        if (z > top_z + EPSILON)
            break;
        if (long(i) - last_i < 2)
            continue;
        const bool take = st.tine_base_rows && z <= TINE_BASE_ROWS_HEIGHT + EPSILON ? true : z - last_z >= st.tine_spacing - EPSILON;
        if (take) {
            out.push_back(i);
            last_i = long(i);
            last_z = z;
        }
    }
    return out;
}

FinPlan plan_fins(const std::vector<FinLayer> &layers, const FinSettings &st, const std::vector<stabilizers::PaintedSpot> &painted,
                  FinReport *report)
{
    FinPlan   plan;
    FinReport rep;
    // The bed contact: the first layer with an outline (a part balanced on an edge may slice to nothing on
    // its very first layer).
    auto first = std::find_if(layers.begin(), layers.end(), [](const FinLayer &l) { return l.islands != nullptr && ! l.islands->empty(); });
    if (layers.size() < 2 || first == layers.end() || ! st.painted_fins()) {
        if (report != nullptr)
            *report = rep;
        return plan;
    }

    // The bed contact, and the part's centre of mass (each layer's area times its height).
    const ExPolygons &contact = *first->islands;
    Vec2d             c0;
    area_centroid(contact, c0);
    double vol = 0.;
    Vec3d  cg  = Vec3d::Zero();
    for (const FinLayer &l : layers) {
        if (l.islands == nullptr || l.islands->empty())
            continue;
        Vec2d        c;
        const double a = area_centroid(*l.islands, c) * l.height;
        vol += a;
        cg += a * Vec3d(c.x(), c.y(), l.print_z - 0.5 * l.height);
    }
    if (vol <= EPSILON) {
        if (report != nullptr)
            *report = rep;
        return plan;
    }
    cg /= vol;
    const double part_top = layers.back().print_z;
    // As tall as asked, or up to the centre of mass (at least 5 mm, never above the part).
    const double top_z = st.height > EPSILON ? std::min(st.height, part_top) :
                                               std::clamp(cg.z(), std::min(5., part_top), part_top);
    rep.lean    = Vec2d(cg.x(), cg.y()) - c0;
    rep.leaning = rep.lean.norm() > LEAN_MIN;

    if (st.auto_fins()) {
        std::vector<Vec2d> dirs;
        if (rep.leaning) {
            const Vec2d l = rep.lean.normalized();
            dirs.push_back(l);
            if (! st.lean_side_only)
                dirs.push_back(-l);
        } else {
            // Balanced on an edge: it can tip either way across the edge, so both sides, square to it.
            std::vector<Vec2d> pts;
            append_points(contact, pts);
            double ratio = 1.;
            Vec2d  n     = minor_axis(pts, ratio);
            if (ratio < 2.) {
                // A corner or a round contact says nothing: the part's spread above it up to the fin height.
                pts.clear();
                for (const FinLayer &l : layers)
                    if (l.print_z <= top_z + EPSILON && l.islands != nullptr)
                        append_points(*l.islands, pts);
                n = minor_axis(pts, ratio);
                if (ratio < 1.5)
                    n = Vec2d(0., 1.);
            }
            dirs.push_back(n);
            dirs.push_back(-n);
        }
        for (const Vec2d &d : dirs) {
            Fin proto;
            proto.dir    = d;
            const Vec2d p = proto.perp();
            // The part's extent across the fins, up to the fin height.
            double pmin = std::numeric_limits<double>::max(), pmax = std::numeric_limits<double>::lowest();
            for (const FinLayer &l : layers) {
                if (l.print_z > top_z + EPSILON)
                    break;
                if (l.islands != nullptr)
                    for (const ExPolygon &e : *l.islands)
                        for (const Point &pt : e.contour.points) {
                            const double s = unscaled(pt).dot(p);
                            pmin = std::min(pmin, s);
                            pmax = std::max(pmax, s);
                        }
            }
            if (pmin > pmax)
                continue;
            // One fin in the middle of a short part; otherwise one near each end and more between, at most
            // `spacing` apart.
            const double span   = pmax - pmin;
            const double inset  = std::min(0.25 * span, 2. + 0.5 * st.thickness);
            const double usable = span - 2. * inset;
            std::vector<double> positions;
            if (usable < 0.5 * st.spacing)
                positions.push_back(0.5 * (pmin + pmax));
            else {
                const size_t n = size_t(std::ceil(usable / st.spacing - EPSILON)) + 1;
                for (size_t k = 0; k < n; ++k)
                    positions.push_back(pmin + inset + usable * double(k) / double(n - 1));
            }
            for (const double s : positions) {
                Fin f    = proto;
                // On the line through the contact's centre (along the fin), at lateral position s.
                f.origin = c0 + p * (s - c0.dot(p));
                if (complete_fin(layers, st, f, top_z)) {
                    plan.fins.push_back(f);
                    ++rep.auto_fins;
                }
            }
        }
    }

    // Painted spots: a fin through each, along the painted surface's own direction.
    rep.painted = painted.size();
    for (const stabilizers::PaintedSpot &spot : painted) {
        const Vec2d  nxy(spot.normal.x(), spot.normal.y());
        const Vec2d  pos(spot.pos.x(), spot.pos.y());
        // A flat top or bottom has no side to stand a fin beside.
        if (nxy.norm() < 0.3 || spot.pos.z() > part_top + EPSILON) {
            rep.unreachable.push_back(spot.pos);
            continue;
        }
        const Vec2d d = nxy.normalized();
        // An automatic fin already holds the part there.
        if (std::any_of(plan.fins.begin(), plan.fins.end(), [&](const Fin &a) {
                return ! a.painted && a.dir.dot(d) > 0.9 && std::abs((pos - a.origin).dot(a.perp())) < 0.25 * st.spacing;
            })) {
            ++rep.painted_on_auto;
            continue;
        }
        Fin f;
        f.dir     = d;
        f.origin  = pos - d * 2.;
        f.painted = true;
        f.need_z  = spot.pos.z();
        if (complete_fin(layers, st, f, std::clamp(std::max(top_z, spot.pos.z() + 1.), 0., part_top))) {
            plan.fins.push_back(f);
            ++rep.painted_placed;
        } else
            rep.unreachable.push_back(spot.pos);
    }
    rep.painted_without_paint = st.mode == sfmPainted && painted.empty();

    double top = 0.;
    for (const Fin &f : plan.fins)
        top = std::max(top, f.top_z);
    plan.tine_layers = tine_layers(layers, st, top);
    if (report != nullptr)
        *report = rep;
    return plan;
}

// --- slicer --------------------------------------------------------------------------------------

// The tine of fin `f` on a layer: a single bead along the centre line from inside the fin wall, across
// the opening, `tine_depth` into the part's section. False when the fin has no wall on its centre line
// there, the part is not in front of it, or the opening is wider than a tine bridges.
static bool make_tine(const Fin &f, const FinLayer &layer, const ExPolygons &section, const FinSettings &st, Polyline &path,
                      ExPolygon &area)
{
    if (layer.islands == nullptr || layer.islands->empty())
        return false;
    const double     w     = st.line_width;
    const double     outer = f.outer_at(layer.print_z);
    const ExPolygons wall  = intersection_ex(section, ExPolygons{ band(f, 0., outer, 0.5 * w) });
    double           wall_min, wall_max;
    if (wall.empty() || ! t_range(f, wall, wall_min, wall_max))
        return false;
    const ExPolygons part = intersection_ex(*layer.islands, ExPolygons{ band(f, 0., wall_min, 0.5 * w) });
    double           part_min, part_max;
    if (part.empty() || ! t_range(f, part, part_min, part_max))
        return false;
    const double span = wall_min - part_max;
    if (span <= 0. || span > st.gap + TINE_MAX_EXTRA_SPAN)
        return false;
    // From the wall's outer loop (half a line in) to tine_depth inside the part.
    const double t_fin  = std::min(wall_min + 0.5 * w, wall_max);
    const double t_part = part_max - st.tine_depth;
    const Vec2d  a = f.at(t_fin), b = f.at(t_part);
    path = Polyline(Point(scaled(a.x()), scaled(a.y())), Point(scaled(b.x()), scaled(b.y())));
    area = band(f, t_part - 0.5 * w, t_fin, 0.5 * w);
    return true;
}

std::vector<FinSlice> slice_fins(const std::vector<FinLayer> &layers, const FinSettings &st, const FinPlan &plan,
                                 const std::vector<ExPolygons> *obstacles, const std::vector<ExPolygons> *blockers,
                                 FinReport *report, const std::function<void()> &throw_if_canceled)
{
    const size_t          n = layers.size();
    std::vector<FinSlice> out(n);
    if (plan.fins.empty())
        return out;

    const double w          = st.line_width;
    const float  half_w     = scaled<float>(0.5 * w);
    const double min_area   = sqr(scaled<double>(0.3));

    // What a fin keeps the gap off: the part on its layer and on the layers up to the gap above (the next
    // layer always, or an overhang would print straight onto the fin's top).
    std::vector<ExPolygons> clip(n);
    for (size_t i = 0; i < n; ++i) {
        ExPolygons part;
        for (size_t j = i; j < n && (j <= i + 1 || layers[j].print_z <= layers[i].print_z + st.gap + EPSILON); ++j)
            if (layers[j].islands != nullptr)
                expolygons_append(part, *layers[j].islands);
        if (! part.empty())
            clip[i] = offset_ex(union_ex(part), scaled<float>(st.gap));
    }
    std::vector<ExPolygons> keep_out(n);
    if (obstacles != nullptr)
        for (size_t i = 0; i < n && i < obstacles->size(); ++i)
            if (! (*obstacles)[i].empty())
                keep_out[i] = offset_ex((*obstacles)[i], half_w);
    const std::set<size_t> tine_set(plan.tine_layers.begin(), plan.tine_layers.end());

    for (const Fin &f : plan.fins) {
        if (throw_if_canceled)
            throw_if_canceled();
        // A painted support blocker anywhere along the fin removes it.
        if (blockers != nullptr) {
            bool blocked = false;
            for (size_t i = 0; i < n && i < blockers->size() && ! blocked; ++i) {
                if (layers[i].print_z > f.top_z + EPSILON)
                    break;
                if (! (*blockers)[i].empty())
                    blocked = ! intersection_ex((*blockers)[i],
                                                ExPolygons{ band(f, 0., f.reach, f.half_thickness_at(st, layers[i].print_z) + st.gap + w) })
                                    .empty();
            }
            if (blocked) {
                if (report != nullptr)
                    ++report->blocked;
                continue;
            }
        }

        ExPolygons prev;
        bool       reached = ! f.painted;
        for (size_t i = 0; i < n; ++i) {
            const double z = layers[i].print_z;
            if (z > f.top_z + EPSILON)
                break;
            const double outer = f.outer_at(z);
            if (outer <= 0.)
                break;
            ExPolygons sec = diff_ex(ExPolygons{ band(f, 0., outer, f.half_thickness_at(st, z)) }, clip[i]);
            if (! keep_out[i].empty())
                sec = diff_ex(sec, keep_out[i]);
            if (i == 0) {
                // Standing on the bed: only the piece that reaches the fin's outer end (not bits behind the
                // part's contact).
                sec.erase(std::remove_if(sec.begin(), sec.end(),
                                         [&](const ExPolygon &e) {
                                             double tmin, tmax;
                                             return ! t_range(f, ExPolygons{ e }, tmin, tmax) || tmax < outer - w;
                                         }),
                          sec.end());
            } else
                // Each layer stands on the one below: a fin never grows, so it never hangs over anything.
                sec = intersection_ex(sec, prev);
            // No slivers thinner than half a line.
            sec = offset2_ex(sec, -0.5f * half_w, 0.5f * half_w);
            sec.erase(std::remove_if(sec.begin(), sec.end(), [min_area](const ExPolygon &e) { return e.area() < min_area; }), sec.end());
            if (sec.empty())
                break;
            if (i == 0 && report != nullptr)
                ++report->fins_standing;
            if (f.painted && z >= f.need_z - 2. - EPSILON)
                reached = true;

            if (tine_set.count(i)) {
                Polyline  path;
                ExPolygon area;
                if (make_tine(f, layers[i], sec, st, path, area)) {
                    // Never print twice where other support is: leave the tine out instead.
                    if (! keep_out[i].empty() && ! intersection_ex(ExPolygons{ area }, keep_out[i]).empty()) {
                        if (report != nullptr)
                            ++report->tines_skipped;
                    } else {
                        out[i].tines.push_back(std::move(path));
                        out[i].tine_areas.push_back(std::move(area));
                        if (report != nullptr)
                            ++report->tines;
                    }
                }
            }
            expolygons_append(out[i].body, sec);
            prev = std::move(sec);
        }
        if (! reached && report != nullptr)
            report->unreachable.push_back(Vec3d(f.at(2.).x(), f.at(2.).y(), f.need_z));
    }
    for (FinSlice &s : out)
        if (s.body.size() > 1)
            s.body = union_ex(s.body);
    return out;
}

// --- painted spots and the export warning --------------------------------------------------------

std::vector<stabilizers::PaintedSpot> painted_fin_spots(const PrintObject &object)
{
    if (object.model_object() == nullptr)
        return {};
    return stabilizers::painted_spots(*object.model_object(), object.trafo_centered(), 0., EnforcerBlockerType::SUPPORT_FIN);
}

std::vector<std::string> objects_with_live_fins(const Model &model, const DynamicPrintConfig &print_config)
{
    // support_fins is an enum whose 0 is Off.
    auto on       = [](const ConfigOption *opt) { return opt != nullptr && opt->getInt() != 0; };
    auto resolved = [&print_config, &on](const ModelObject &obj) {
        if (const ConfigOption *opt = obj.config.option("support_fins"); opt != nullptr)
            return on(opt);
        return on(print_config.option("support_fins"));
    };
    std::vector<std::string> out;
    for (const ModelObject *obj : model.objects)
        if (obj != nullptr && resolved(*obj))
            out.push_back(obj->name);
    return out;
}

} // namespace fins

// --- live generation -----------------------------------------------------------------------------

fins::FinReport generate_fin_supports(PrintObject &object, const std::function<void()> &throw_if_canceled)
{
    fins::FinReport report;
    if (! object.has_fins() || object.layers().size() < 2)
        return report;
    // On a raft the fins would stand on the raft, not on the bed, and their tines would no longer be
    // where the planner put them: refuse rather than misplace them.
    if (object.has_raft()) {
        report.refused_raft = true;
        return report;
    }

    const std::vector<fins::FinLayer> layers = fins::layers_of(object);
    const fins::FinSettings           st     = fins::settings_of(object);
    const std::vector<stabilizers::PaintedSpot> spots = fins::painted_fin_spots(object);
    const fins::FinPlan plan = fins::plan_fins(layers, st, spots, &report);
    if (plan.fins.empty())
        return report;
    throw_if_canceled();

    const size_t n = layers.size();
    // Keep out of: support blocker volumes, and every support layer whose Z range overlaps this object
    // layer's (the support generators' and the stabilizers'), whatever its print_z.
    std::vector<ExPolygons> obstacles(n);
    {
        const std::vector<Polygons> volumes = object.slice_support_blockers();
        for (size_t i = 0; i < n; ++i) {
            const Layer &layer = *object.layers()[i];
            ExPolygons   o     = support_layer_util::support_coverage(object, layer.print_z - layer.height, layer.print_z);
            if (i < volumes.size() && ! volumes[i].empty())
                expolygons_append(o, union_ex(volumes[i]));
            obstacles[i] = o.empty() ? o : union_ex(o);
        }
    }
    // Painted support blockers remove the fins they touch.
    std::vector<ExPolygons> blockers(n);
    {
        std::vector<Polygons> painted;
        object.project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER, painted);
        for (size_t i = 0; i < n && i < painted.size(); ++i)
            if (! painted[i].empty())
                blockers[i] = union_ex(painted[i]);
    }

    std::vector<fins::FinSlice> slices = fins::slice_fins(layers, st, plan, &obstacles, &blockers, &report, throw_if_canceled);
    // What the other support took: the plan sliced without it, so a fin or tine that disappears is never silent.
    {
        fins::FinReport free_report;
        fins::slice_fins(layers, st, plan, nullptr, &blockers, &free_report, throw_if_canceled);
        report.fins_lost  = free_report.fins_standing > report.fins_standing ? free_report.fins_standing - report.fins_standing : 0;
        report.tines_lost = free_report.tines > report.tines ? free_report.tines - report.tines : 0;
    }

    // How far the fins reach beyond the object's own footprint.
    {
        BoundingBox part_bb, fin_bb;
        for (const Layer *l : object.layers())
            for (const ExPolygon &e : l->lslices)
                part_bb.merge(get_extents(e.contour));
        for (const fins::FinSlice &s : slices)
            for (const ExPolygon &e : s.body)
                fin_bb.merge(get_extents(e.contour));
        if (part_bb.defined && fin_bb.defined)
            report.beyond_footprint = unscaled(std::max<coord_t>({ coord_t(0), part_bb.min.x() - fin_bb.min.x(), part_bb.min.y() - fin_bb.min.y(),
                                                                   fin_bb.max.x() - part_bb.max.x(), fin_bb.max.y() - part_bb.max.y() }));
    }

    const double min_island_area = sqr(scaled<double>(0.2));
    bool         inserted        = false;
    for (size_t i = 0; i < n; ++i) {
        fins::FinSlice &s = slices[i];
        if (s.body.empty() && s.tines.empty())
            continue;
        const Layer &layer = *object.layers()[i];
        // The fins' own extrusions carry the object layer's height, whatever support layer they join.
        const Flow flow = i == 0 ? support_material_1st_layer_flow(&object, float(layer.height)) :
                                   support_material_flow(&object, float(layer.height));
        const float half_w  = 0.5f * float(flow.scaled_width());
        const float spacing = float(flow.scaled_spacing());

        auto *coll = new ExtrusionEntityCollection();
        ExPolygons printed;
        for (ExPolygon &island : s.body) {
            if (island.area() < min_island_area)
                continue;
            // Solid concentric loops, outside in: a fin wall a few lines thick is all wall. A speck too thin
            // for one line (what other support left of a wall) is not printed: it would be a zero-length bead.
            Polygons loop = offset(island, -half_w);
            if (loop.empty())
                continue;
            while (! loop.empty()) {
                Polygons next = offset(loop, -spacing);
                extrusion_entities_append_loops(coll->entities, std::move(loop), erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height());
                loop = std::move(next);
            }
            printed.emplace_back(std::move(island));
        }
        // Tines: one bead each, an open path (a loop would trace the thin footprint twice).
        if (! s.tines.empty()) {
            extrusion_entities_append_paths(coll->entities, std::move(s.tines), erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height());
            expolygons_append(printed, s.tine_areas);
        }
        if (coll->entities.empty()) {
            delete coll;
            continue;
        }
        SupportLayer *sl = support_layer_util::support_layer_at(object, layer, inserted);
        // By construction: a fin layer, tines included, is always the object layer's own height.
        assert(std::abs(sl->print_z - layer.print_z) < EPSILON);
        sl->support_fills.entities.push_back(coll);
        expolygons_append(sl->support_islands, printed);
        if (sl->support_type == stInnerTree)
            expolygons_append(sl->lslices, printed);
    }
    if (inserted)
        support_layer_util::relink_support_layers(object);

    BOOST_LOG_TRIVIAL(debug) << "Support fins: " << plan.fins.size() << " fins (" << report.auto_fins << " auto, " << report.painted_placed
                             << " painted), " << report.tines << " tines, " << report.tines_skipped << " tines skipped, "
                             << report.blocked << " blocked, " << report.unreachable.size() << " unreachable, lean "
                             << report.lean.norm() << " mm";
    return report;
}

} // namespace Slic3r
