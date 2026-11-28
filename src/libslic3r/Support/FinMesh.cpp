// Support fins as a mesh (see FinMesh.hpp). The technique is Slant3D's designed-in supports, as specified
// by Support Fins (gittrahan/support-fins, printfins.com, MIT); this is an independent native implementation.

#include "FinMesh.hpp"

#include "../ClipperUtils.hpp"
#include "../Emboss.hpp"
#include "../MeshBoolean.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <exception>

namespace Slic3r { namespace fins {

namespace {

// A tine of `width` from its path's start (inside the fin wall) to half a live line past its end (inside
// the part, as the live tine's footprint).
ExPolygon fin_mesh_tine_rect(const Polyline &path, double live_width, double width)
{
    const Vec2d a = unscaled(path.first_point()), b = unscaled(path.last_point());
    Vec2d       d = b - a;
    if (d.norm() < EPSILON)
        return {};
    d.normalize();
    const Vec2d p(-d.y(), d.x());
    const Vec2d e    = b + d * (0.5 * live_width);
    const double h   = 0.5 * width;
    Polygon      poly;
    for (const Vec2d &v : { Vec2d(a - p * h), Vec2d(e - p * h), Vec2d(e + p * h), Vec2d(a + p * h) })
        poly.points.emplace_back(scaled(v.x()), scaled(v.y()));
    if (poly.is_clockwise())
        poly.reverse();
    return ExPolygon(std::move(poly));
}

// A vertical prism: the 2D shape at z0 (front) and z1 (back), in millimetres.
class FinPrismProjection : public Emboss::IProjection
{
public:
    FinPrismProjection(double z0, double z1) : m_z0(z0), m_z1(z1) {}
    std::pair<Vec3d, Vec3d> create_front_back(const Point &p) const override
    {
        const Vec2d q = unscaled(p);
        return { Vec3d(q.x(), q.y(), m_z0), Vec3d(q.x(), q.y(), m_z1) };
    }
    Vec3d project(const Vec3d &point) const override { return Vec3d(point.x(), point.y(), m_z1); }
    std::optional<Vec2d> unproject(const Vec3d &p, double *depth) const override
    {
        if (depth != nullptr)
            *depth = p.z() - m_z0;
        return Vec2d(p.x(), p.y());
    }

private:
    double m_z0, m_z1;
};

bool fin_mesh_same_sections(const ExPolygons &a, const ExPolygons &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].contour.points != b[i].contour.points || a[i].holes.size() != b[i].holes.size())
            return false;
        for (size_t j = 0; j < a[i].holes.size(); ++j)
            if (a[i].holes[j].points != b[i].holes[j].points)
                return false;
    }
    return true;
}

} // namespace

ExPolygons printable_fin_islands(const ExPolygons &islands, double line_width)
{
    // As generate_fin_supports: islands under 0.2 x 0.2 mm, and islands with no room for a loop half a line in.
    const double min_area = sqr(scaled<double>(0.2));
    const float  half_w   = scaled<float>(0.5 * line_width);
    ExPolygons   out;
    for (const ExPolygon &island : islands)
        if (island.area() >= min_area && ! offset(island, -half_w).empty())
            out.push_back(island);
    return out;
}

std::vector<FinMeshLayer> fin_mesh_layers(const std::vector<FinLayer> &layers, const std::vector<FinSlice> &slices,
                                          const FinSettings &st, const FinTineOptions &tines)
{
    std::vector<FinMeshLayer> out(layers.size());
    const double live_w  = st.line_width;
    const double width   = tines.width > EPSILON ? tines.width : live_w;
    const bool   rebuild = std::abs(width - live_w) > EPSILON || tines.stop_at_part;
    for (size_t i = 0; i < layers.size() && i < slices.size(); ++i) {
        FinMeshLayer &band = out[i];
        band.bottom_z      = layers[i].print_z - layers[i].height;
        band.top_z         = layers[i].print_z;
        const FinSlice &s  = slices[i];
        if (s.body.empty() && s.tines.empty())
            continue;
        ExPolygons all = printable_fin_islands(s.body, i == 0 && tines.first_layer_line_width > EPSILON ? tines.first_layer_line_width : live_w);
        if (! rebuild)
            expolygons_append(all, s.tine_areas);
        else {
            ExPolygons t;
            for (const Polyline &path : s.tines)
                if (ExPolygon r = fin_mesh_tine_rect(path, live_w, width); ! r.contour.empty())
                    t.emplace_back(std::move(r));
            // A separate object must not print inside the part: the tine ends at its outline.
            if (tines.stop_at_part && layers[i].islands != nullptr && ! layers[i].islands->empty())
                t = diff_ex(t, *layers[i].islands);
            expolygons_append(all, t);
        }
        band.sections = union_ex(all);
    }
    return out;
}

indexed_triangle_set fin_mesh(const std::vector<FinMeshLayer> &bands, const FinMeshOptions &opts, FinMeshReport *report)
{
    FinMeshReport                     rep;
    std::vector<indexed_triangle_set> prisms;
    const double                      min_area = sqr(scaled<double>(0.1));

    auto build = [&](const ExPolygons &sections, double z0, double z1) {
        for (const ExPolygon &island : sections) {
            if (std::abs(island.area()) < min_area)
                continue;
            indexed_triangle_set its;
            try {
                its = Emboss::polygons2model(ExPolygons{ island }, FinPrismProjection(z0, z1));
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(warning) << "Fin mesh: an island at z " << z1 << " could not be triangulated: " << e.what();
            }
            if (its.indices.empty()) {
                ++rep.dropped;
                continue;
            }
            // Outward normals, whatever winding the triangulation chose.
            if (its_volume(its) < 0.f)
                its_flip_triangles(its);
            prisms.emplace_back(std::move(its));
            ++rep.prisms;
        }
    };

    // Runs of identical sections share one prism.
    size_t i = 0;
    while (i < bands.size()) {
        if (bands[i].sections.empty()) {
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < bands.size() && ! bands[j].sections.empty() && std::abs(bands[j].bottom_z - bands[j - 1].top_z) < EPSILON &&
               fin_mesh_same_sections(bands[j].sections, bands[i].sections))
            ++j;
        rep.bands += j - i;
        // Into the next band by `cap`, unless nothing is there to overlap.
        const bool   above = j < bands.size() && ! bands[j].sections.empty();
        build(bands[i].sections, bands[i].bottom_z, bands[j - 1].top_z + (above ? opts.cap : 0.));
        i = j;
    }

    indexed_triangle_set out;
    if (! prisms.empty()) {
        if (opts.union_shells && MeshBoolean::mfd::union_all(prisms, out) && ! out.indices.empty())
            rep.unioned = true;
        else {
            out.clear();
            for (const indexed_triangle_set &p : prisms)
                its_merge(out, p);
        }
    }
    rep.triangles = out.indices.size();
    rep.closed    = ! out.indices.empty() && its_num_open_edges(out) == 0;
    rep.volume    = out.indices.empty() ? 0. : double(its_volume(out));
    if (report != nullptr)
        *report = rep;
    return out;
}

}} // namespace Slic3r::fins
