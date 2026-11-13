#include "ShowcaseSurfaceColours.hpp"

#include "../AABBMesh.hpp"
#include "../BoundingBox.hpp"
#include "../Geometry.hpp"
#include "../Model.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace Slic3r {
namespace Showcase {

namespace {

int config_int(const ModelConfig &config, const char *key)
{
    const ConfigOption *opt = config.option(key);
    return opt != nullptr ? opt->getInt() : 0;
}

// What the outside of a region prints in, as apply_to_print_region_config() (PrintObject.cpp) sets
// it: "extruder" sets every role, a wall filament set on its own wins for the walls, and the outer
// wall filament wins for the outermost wall (the one that is seen).
int config_visible_filament(const ModelConfig &config)
{
    if (int f = config_int(config, "outer_wall_filament"); f > 0)
        return f;
    if (int f = config_int(config, "wall_filament"); f > 0)
        return f;
    if (int f = config_int(config, "extruder"); f > 0)
        return f;
    return 0;
}

bool config_override_paint(const ModelConfig &config)
{
    const ConfigOption *opt = config.option("override_paint");
    return opt != nullptr && opt->getBool();
}

inline uint64_t hash_mix(uint64_t h, uint64_t v)
{
    // splitmix64 step
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= h >> 31;
    h *= 0xbf58476d1ce4e5b9ull;
    return h;
}
inline uint64_t hash_mix(uint64_t h, double v)
{
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return hash_mix(h, bits);
}
uint64_t hash_mix(uint64_t h, const Transform3d &tr)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            h = hash_mix(h, tr(r, c));
    return h;
}
uint64_t hash_mesh(uint64_t h, const TriangleMesh &mesh)
{
    h = hash_mix(h, uint64_t(mesh.its.vertices.size()));
    h = hash_mix(h, uint64_t(mesh.its.indices.size()));
    const BoundingBoxf3 bb = mesh.bounding_box();
    for (int i = 0; i < 3; ++i) {
        h = hash_mix(h, bb.min(i));
        h = hash_mix(h, bb.max(i));
    }
    return h;
}

bool boxes_overlap(const BoundingBoxf3 &a, const BoundingBoxf3 &b)
{
    for (int i = 0; i < 3; ++i)
        if (a.max(i) < b.min(i) || a.min(i) > b.max(i))
            return false;
    return true;
}

// The facets whose world height overlaps [z_min, z_max]
std::vector<int> facets_in_heights(const indexed_triangle_set &its, const Transform3d &to_world, double z_min, double z_max)
{
    std::vector<int> out;
    for (size_t i = 0; i < its.indices.size(); ++i) {
        double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
        for (int k = 0; k < 3; ++k) {
            const double z = (to_world * its.vertices[size_t(its.indices[i](k))].cast<double>()).z();
            lo = std::min(lo, z);
            hi = std::max(hi, z);
        }
        if (lo <= z_max && hi >= z_min)
            out.push_back(int(i));
    }
    return out;
}

std::vector<int> facets_in_box(const indexed_triangle_set &its, const BoundingBoxf3 &box)
{
    std::vector<int> out;
    for (size_t i = 0; i < its.indices.size(); ++i) {
        BoundingBoxf3 tb;
        for (int k = 0; k < 3; ++k)
            tb.merge(its.vertices[size_t(its.indices[i](k))].cast<double>());
        if (boxes_overlap(tb, box))
            out.push_back(int(i));
    }
    return out;
}

// Only the vertices and triangles of `its` that `indices` use.
indexed_triangle_set compact(const indexed_triangle_set &its)
{
    indexed_triangle_set out;
    std::vector<int>     map(its.vertices.size(), -1);
    out.indices.reserve(its.indices.size());
    for (const stl_triangle_vertex_indices &t : its.indices) {
        stl_triangle_vertex_indices n;
        for (int k = 0; k < 3; ++k) {
            int &m = map[size_t(t(k))];
            if (m < 0) {
                m = int(out.vertices.size());
                out.vertices.push_back(its.vertices[size_t(t(k))]);
            }
            n(k) = m;
        }
        out.indices.push_back(n);
    }
    return out;
}

} // namespace

int visible_filament(const ModelVolume &volume) { return config_visible_filament(volume.config); }

std::vector<ColourRegion> colour_regions_for(const ModelVolume &part, const Transform3d &instance_matrix, size_t num_filaments)
{
    std::vector<ColourRegion> out;
    const ModelObject *object = part.get_object();
    if (object == nullptr || !part.is_model_part() || num_filaments < 2)
        return out;
    const TriangleMesh &part_mesh = part.mesh();
    if (part_mesh.empty())
        return out;
    const Transform3d   part_matrix  = part.get_matrix();
    const Transform3d   object_to_part = part_matrix.inverse();
    const BoundingBoxf3 part_bb      = part_mesh.bounding_box();
    auto in_range = [num_filaments](int f) { return f >= 1 && size_t(f) <= num_filaments; };

    // Height range modifiers: their config applies to the object first (PrintApply), below modifiers.
    if (!object->layer_config_ranges.empty()) {
        const Transform3d   to_world = instance_matrix * part_matrix;
        const BoundingBoxf3 world_bb = part_bb.transformed(to_world);
        for (const auto &[range, config] : object->layer_config_ranges) {
            const int f = config_visible_filament(config);
            if (!in_range(f) || range.second <= world_bb.min.z() || range.first >= world_bb.max.z())
                continue;
            ColourRegion r;
            r.kind          = ColourRegion::Kind::HeightRange;
            r.filament      = f;
            r.z_min         = range.first;
            r.z_max         = range.second;
            r.mesh_to_world = to_world;
            out.push_back(std::move(r));
        }
    }

    // Modifiers, in the object's volume order (a later one wins where they overlap, as in the slice).
    for (const ModelVolume *v : object->volumes) {
        if (v == nullptr || !v->is_modifier())
            continue;
        const int f = config_visible_filament(v->config);
        if (!in_range(f))
            continue;
        const TriangleMesh &mesh = v->mesh();
        if (mesh.empty())
            continue;
        const Transform3d to_part = object_to_part * v->get_matrix();
        if (!boxes_overlap(mesh.bounding_box().transformed(to_part), part_bb))
            continue;
        ColourRegion r;
        r.kind           = ColourRegion::Kind::Volume;
        r.filament       = f;
        r.override_paint = config_override_paint(v->config);
        r.mesh           = mesh.its;
        // keep the outside out: a mirrored modifier gets its triangles flipped back
        its_transform(r.mesh, to_part, true);
        out.push_back(std::move(r));
    }
    return out;
}

uint64_t colour_regions_key(const ModelVolume &part, const Transform3d &instance_matrix, size_t num_filaments)
{
    uint64_t h = 0x5eed;
    h = hash_mix(h, uint64_t(num_filaments));
    h = hash_mix(h, uint64_t(part.id().id));
    h = hash_mesh(h, part.mesh());
    h = hash_mix(h, part.get_matrix());
    const ModelObject *object = part.get_object();
    if (object == nullptr)
        return h;
    if (!object->layer_config_ranges.empty()) {
        h = hash_mix(h, instance_matrix);
        for (const auto &[range, config] : object->layer_config_ranges) {
            h = hash_mix(h, range.first);
            h = hash_mix(h, range.second);
            h = hash_mix(h, uint64_t(int64_t(config_visible_filament(config)) + 1));
        }
    }
    for (const ModelVolume *v : object->volumes) {
        if (v == nullptr || !v->is_modifier())
            continue;
        h = hash_mix(h, uint64_t(v->id().id));
        h = hash_mix(h, uint64_t(int64_t(config_visible_filament(v->config)) + 1));
        h = hash_mix(h, uint64_t(config_override_paint(v->config)));
        h = hash_mix(h, v->get_matrix());
        h = hash_mesh(h, v->mesh());
    }
    return h;
}

float surface_colour_edge_limit(const Transform3d &mesh_to_world, float world_mm)
{
    const Vec3d sf    = Geometry::Transformation(mesh_to_world).get_scaling_factor();
    const double scale = std::max({ std::abs(sf.x()), std::abs(sf.y()), std::abs(sf.z()), 1e-6 });
    return float(double(world_mm) / scale);
}

bool point_inside_closed_mesh(const AABBMesh &mesh, const Vec3d &point)
{
    // Three directions off the axes, so a ray rarely runs along an edge or a face of a box.
    static const Vec3d dirs[3] = { Vec3d(0.31, 0.53, 0.79).normalized(), Vec3d(-0.71, 0.29, 0.64).normalized(),
                                   Vec3d(0.23, -0.83, -0.51).normalized() };
    int votes = 0;
    for (int i = 0; i < 3; ++i) {
        const AABBMesh::hit_result hit = mesh.query_ray_hit(point, dirs[i]);
        if (hit.is_inside())
            ++votes;
        // two of three decide
        if (votes == 2 || votes + (2 - i) < 2)
            break;
    }
    return votes >= 2;
}

struct SurfaceColourCursor::Impl
{
    std::unique_ptr<AABBMesh> aabb;
    struct KeyHash
    {
        size_t operator()(const Vec3f &p) const
        {
            uint32_t b[3];
            std::memcpy(b, p.data(), sizeof(b));
            return size_t(b[0]) * 73856093u ^ size_t(b[1]) * 19349663u ^ size_t(b[2]) * 83492791u;
        }
    };
    mutable std::unordered_map<Vec3f, bool, KeyHash> cache;
};

SurfaceColourCursor::SurfaceColourCursor(const ColourRegion &region)
    : TriangleSelector::Cursor(Vec3f::Zero(), 1.f, Transform3d::Identity(), TriangleSelector::ClippingPlane())
    , m_region(region)
    , m_impl(std::make_unique<Impl>())
{
    if (region.kind == ColourRegion::Kind::Volume && !region.mesh.indices.empty())
        m_impl->aabb = std::make_unique<AABBMesh>(region.mesh);
}

SurfaceColourCursor::~SurfaceColourCursor() = default;

bool SurfaceColourCursor::is_mesh_point_inside(const Vec3f &point) const
{
    if (m_region.kind == ColourRegion::Kind::HeightRange) {
        const double z = (m_region.mesh_to_world * point.cast<double>()).z();
        return z >= m_region.z_min && z < m_region.z_max;
    }
    if (m_impl->aabb == nullptr)
        return false;
    auto it = m_impl->cache.find(point);
    if (it != m_impl->cache.end())
        return it->second;
    const bool inside = point_inside_closed_mesh(*m_impl->aabb, point.cast<double>());
    m_impl->cache.emplace(point, inside);
    return inside;
}

int SurfaceColourCursor::vertices_inside(const TriangleSelector::Triangle &tr, const std::vector<TriangleSelector::Vertex> &vertices) const
{
    int inside = 0;
    for (int i = 0; i < 3; ++i)
        if (is_mesh_point_inside(vertices[size_t(tr.verts_idxs[i])].v))
            ++inside;
    return inside;
}

bool SurfaceColourCursor::is_edge_inside_cursor(const TriangleSelector::Triangle &tr, const std::vector<TriangleSelector::Vertex> &vertices) const
{
    // Called when no corner is inside: may the region's border still cross the triangle? Then it is
    // split further (down to the edge limit). Conservative is fine, a wrong "yes" only splits more.
    const Vec3f &a = vertices[size_t(tr.verts_idxs[0])].v;
    const Vec3f &b = vertices[size_t(tr.verts_idxs[1])].v;
    const Vec3f &c = vertices[size_t(tr.verts_idxs[2])].v;
    if (m_region.kind == ColourRegion::Kind::HeightRange) {
        const double za = (m_region.mesh_to_world * a.cast<double>()).z();
        const double zb = (m_region.mesh_to_world * b.cast<double>()).z();
        const double zc = (m_region.mesh_to_world * c.cast<double>()).z();
        return std::min({ za, zb, zc }) < m_region.z_max && std::max({ za, zb, zc }) >= m_region.z_min;
    }
    if (m_impl->aabb == nullptr)
        return false;
    // The ball round the triangle touches the region's surface.
    const Vec3d  centre = ((a + b + c) / 3.f).cast<double>();
    const double r2     = std::max({ (a.cast<double>() - centre).squaredNorm(), (b.cast<double>() - centre).squaredNorm(),
                                     (c.cast<double>() - centre).squaredNorm() });
    return m_impl->aabb->squared_distance(centre) <= r2 * (1. + 1e-6) + 1e-12;
}

void surface_colour_facets(const TriangleMesh                             &part_mesh,
                           const TriangleSelector::TriangleSplittingData *paint,
                           const std::vector<ColourRegion>               &regions,
                           float                                          edge_limit,
                           std::vector<indexed_triangle_set>             &facets_per_filament)
{
    facets_per_filament.clear();
    if (part_mesh.empty())
        return;

    TriangleSelector selector(part_mesh);
    int max_paint = 0;
    if (paint != nullptr && !paint->triangles_to_split.empty()) {
        selector.deserialize(*paint, false);
        for (EnforcerBlockerType s : TriangleSelector::extract_used_facet_states(*paint))
            max_paint = std::max(max_paint, int(s));
    }

    // state -> filament (0 = the part's own). Painted states are filaments already; every region
    // filament gets a state of its own above them, so the paint can be protected from the regions.
    const int        max_state = int(EnforcerBlockerType::ExtruderMax);
    std::vector<int> filament_of(size_t(max_state) + 1, -1);
    for (int s = 0; s <= max_paint; ++s)
        filament_of[size_t(s)] = s;
    std::vector<bool> paint_states(size_t(max_state) + 1, false);
    for (int s = 1; s <= max_paint; ++s)
        paint_states[size_t(s)] = true;
    std::vector<int> state_for_filament;
    int              next_state = max_paint + 1;
    std::vector<int> used_states;
    for (int s = 0; s <= max_paint; ++s)
        used_states.push_back(s);

    const float limit = std::max(edge_limit, 1e-4f);
    for (const ColourRegion &region : regions) {
        if (region.filament < 1)
            continue;
        if (size_t(region.filament) >= state_for_filament.size())
            state_for_filament.resize(size_t(region.filament) + 1, -1);
        int &state = state_for_filament[size_t(region.filament)];
        if (state < 0) {
            if (next_state > max_state)
                continue; // no state left, very unlikely
            state                       = next_state++;
            filament_of[size_t(state)] = region.filament;
            used_states.push_back(state);
        }
        std::vector<int> facets;
        if (region.kind == ColourRegion::Kind::HeightRange)
            facets = facets_in_heights(part_mesh.its, region.mesh_to_world, region.z_min, region.z_max);
        else {
            if (region.mesh.indices.empty())
                continue;
            BoundingBoxf3 box = bounding_box(region.mesh);
            box.min -= Vec3d::Constant(limit);
            box.max += Vec3d::Constant(limit);
            facets = facets_in_box(part_mesh.its, box);
        }
        if (facets.empty())
            continue;
        auto cursor = std::make_unique<SurfaceColourCursor>(region);
        if (region.override_paint)
            // replaces painted colour and earlier regions alike
            selector.select_triangles_with_cursor(facets, std::move(cursor), EnforcerBlockerType(state), limit, false);
        else
            // painted colour stays on top, earlier regions are replaced
            selector.select_triangles_with_cursor(facets, std::move(cursor), EnforcerBlockerType(state), limit, paint_states);
    }

    int max_filament = 0;
    for (int s : used_states)
        max_filament = std::max(max_filament, filament_of[size_t(s)]);
    facets_per_filament.assign(size_t(max_filament) + 1, indexed_triangle_set());
    for (int s : used_states) {
        indexed_triangle_set its = selector.get_facets_strict(EnforcerBlockerType(s));
        if (its.indices.empty())
            continue;
        its_merge(facets_per_filament[size_t(filament_of[size_t(s)])], compact(its));
    }
}

} // namespace Showcase
} // namespace Slic3r
