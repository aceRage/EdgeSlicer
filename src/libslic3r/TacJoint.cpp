#include "TacJoint.hpp"
#include "AABBMesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

// Marks the validation messages for gettext; the gizmo shows them through _L(). Saved and
// restored round this file so a unity-build neighbour keeps its own L().
#pragma push_macro("L")
#undef L
#define L(s) (s)

namespace Slic3r {
namespace {

#include "TacJointData.inc"

constexpr double CONTAINMENT_EPSILON = 1e-7; // mm in the immutable reference frame; far below 0.0013 mm.
constexpr double ENGAGEMENT_EPSILON  = 1e-5;

template<size_t V, size_t F> indexed_triangle_set make_reference(const float (&vertices)[V][3], const int (&indices)[F][3])
{
    indexed_triangle_set result;
    result.vertices.reserve(V);
    result.indices.reserve(F);
    const Transform3d transform = tac_joint_source_transform();
    for (const auto& v : vertices)
        result.vertices.emplace_back((transform * Vec3d(v[0], v[1], v[2])).cast<float>());
    for (const auto& f : indices)
        result.indices.emplace_back(f[0], f[1], f[2]);
    return result;
}

BoundingBoxf3 bounds(const indexed_triangle_set& mesh)
{
    BoundingBoxf3 box;
    for (const auto& v : mesh.vertices)
        box.merge(v.cast<double>());
    return box;
}

using Edge = std::pair<int, int>;
std::vector<Edge> mesh_edges(const indexed_triangle_set& mesh)
{
    std::set<Edge> edges;
    for (const auto& f : mesh.indices)
        for (int i = 0; i < 3; ++i) {
            const int a = f[i], b = f[(i + 1) % 3];
            edges.emplace(std::min(a, b), std::max(a, b));
        }
    return {edges.begin(), edges.end()};
}

// The AABB trees refer to these meshes, which never move after construction.
// Validation transforms query points into their reference frames instead of
// repeatedly rebuilding spatial indices during mouse drags.
struct ReferenceGeometry
{
    const indexed_triangle_set connector       = make_reference(TAC_CONNECTOR_VERTICES, TAC_CONNECTOR_INDICES);
    const indexed_triangle_set cutter          = make_reference(TAC_CUTTER_VERTICES, TAC_CUTTER_INDICES);
    const BoundingBoxf3        connector_box   = bounds(connector);
    const BoundingBoxf3        cutter_box      = bounds(cutter);
    const std::vector<Edge>    connector_edges = mesh_edges(connector);
    const std::vector<Edge>    cutter_edges    = mesh_edges(cutter);
    const AABBMesh             connector_tree{connector};
    const AABBMesh             cutter_tree{cutter};
};

const ReferenceGeometry& reference()
{
    static const ReferenceGeometry geometry;
    return geometry;
}

Vec3d cutter_factors(const TacJointParams& p)
{
    const Vec3d size = reference().cutter_box.size();
    // Float defaults deliberately match the persisted/UI values exactly. Thus
    // the default is the original STL, including its measured 0.2387486 mm
    // width difference rather than an idealized 0.24 mm reconstruction.
    const Vec3d nominal(2. * double(.12f), 2. * double(.12f), 2. * double(.30f));
    const Vec3d desired(2. * double(p.clearance), 2. * double(p.clearance), 2. * double(p.end_clearance));
    return ((double(p.scale) * (size - nominal) + desired).array() / size.array()).matrix();
}

TacJointValidation validate_scalars(const TacJointParams& p)
{
    if (p.version != 1)
        return {false, L("Unsupported TAC Joint reference version.")};
    if (!std::isfinite(p.scale) || !std::isfinite(p.clearance) || !std::isfinite(p.end_clearance) || !std::isfinite(p.offset))
        return {false, L("TAC Joint parameters must be finite.")};
    if (p.scale <= 0 || p.clearance < 0 || p.end_clearance < 0)
        return {false, L("TAC Joint size must be positive and clearances must be nonnegative.")};
    const Vec3d factors          = cutter_factors(p);
    const Vec3d extent           = reference().cutter_box.size().cwiseProduct(factors);
    const Vec3d connector_extent = reference().connector_box.size() * double(p.scale);
    if ((extent.array() <= 1e-5).any() || (connector_extent.array() <= 1e-5).any() || (extent.array() > 1e6).any() ||
        std::abs(double(p.offset)) > 1e6)
        return {false, L("TAC Joint dimensions are outside the supported numerical range.")};
    return {true, {}};
}

// Look for any surface crossing, including crossings of the interiors of
// connector triangles by cutter edges. Vertex-only containment misses those
// intersections at the profile's concave/rounded portions.
bool edge_crosses_surface(const Vec3d& a, const Vec3d& b, const AABBMesh& surface)
{
    const Vec3d  delta  = b - a;
    const double length = delta.norm();
    if (length <= CONTAINMENT_EPSILON)
        return false;
    for (const auto& hit : surface.query_ray_hits(a, delta / length))
        if (hit.distance() > CONTAINMENT_EPSILON && hit.distance() < length - CONTAINMENT_EPSILON)
            return true;
    return false;
}

bool contains_connector(const TacJointParams& p)
{
    const auto&        r              = reference();
    const Vec3d        factors        = cutter_factors(p);
    const Vec3d        centre         = r.cutter_box.center();
    const Vec3d        shifted_centre = centre * double(p.scale);
    const Vec3d        ray_direction  = Vec3d(.3713906763541037, .529271389135, .763618841827).normalized();
    std::vector<Vec3d> connector_in_cutter;
    connector_in_cutter.reserve(r.connector.vertices.size());
    for (const auto& v : r.connector.vertices) {
        const Vec3d query = ((v.cast<double>() * double(p.scale) - shifted_centre).array() / factors.array()).matrix() + centre;
        if (r.cutter_tree.squared_distance(query) <= CONTAINMENT_EPSILON * CONTAINMENT_EPSILON ||
            !r.cutter_tree.query_ray_hit(query, ray_direction).is_inside())
            return false;
        connector_in_cutter.push_back(query);
    }
    for (const auto& edge : r.connector_edges)
        if (edge_crosses_surface(connector_in_cutter[edge.first], connector_in_cutter[edge.second], r.cutter_tree))
            return false;
    std::vector<Vec3d> cutter_in_connector;
    cutter_in_connector.reserve(r.cutter.vertices.size());
    for (const auto& v : r.cutter.vertices)
        cutter_in_connector.emplace_back(((v.cast<double>() - centre).cwiseProduct(factors) + shifted_centre) / double(p.scale));
    for (const auto& edge : r.cutter_edges)
        if (edge_crosses_surface(cutter_in_connector[edge.first], cutter_in_connector[edge.second], r.connector_tree))
            return false;
    return true;
}

bool cached_contains_connector(const TacJointParams& p)
{
    // Offset and cut kerf do not change the fit between these two meshes.
    struct Fit
    {
        float scale, clearance, end_clearance;
        bool  contains;
    };
    thread_local std::vector<Fit> cache;
    for (const auto& fit : cache)
        if (fit.scale == p.scale && fit.clearance == p.clearance && fit.end_clearance == p.end_clearance)
            return fit.contains;
    const bool contains = contains_connector(p);
    if (cache.size() >= 64)
        cache.erase(cache.begin());
    cache.push_back({p.scale, p.clearance, p.end_clearance, contains});
    return contains;
}

void throw_if_invalid(const TacJointValidation& result)
{
    if (!result.valid)
        throw std::invalid_argument(result.message);
}

} // namespace

Transform3d tac_joint_source_transform()
{
    Transform3d result = Transform3d::Identity();
    for (int row = 0; row < 3; ++row) {
        result.translation()[row] = TAC_TRANSLATION[row];
        for (int col = 0; col < 3; ++col)
            result.linear()(row, col) = TAC_ROTATION[row][col];
    }
    return result;
}

TacJointValidation tac_joint_validate(const TacJointParams& p, double kerf_lo, double kerf_hi)
{
    const auto scalars = validate_scalars(p);
    if (!scalars.valid)
        return scalars;
    if (!std::isfinite(kerf_lo) || !std::isfinite(kerf_hi) || kerf_lo > kerf_hi)
        return {false, L("TAC Joint cut gap must have finite, ordered boundaries.")};
    const auto& box = reference().connector_box;
    if (double(p.scale) * box.min.z() + p.offset >= kerf_lo - ENGAGEMENT_EPSILON ||
        double(p.scale) * box.max.z() + p.offset <= kerf_hi + ENGAGEMENT_EPSILON)
        return {false, L("TAC Joint needs positive engagement in both retained parts. Reduce the offset or cut gap.")};
    if (!cached_contains_connector(p))
        return {false, L("TAC Joint cutter intersects the connector profile. Increase the side or end clearance.")};
    return {true, {}};
}

indexed_triangle_set tac_joint_connector_mesh(const TacJointParams& p)
{
    throw_if_invalid(validate_scalars(p));
    indexed_triangle_set result = reference().connector;
    for (auto& v : result.vertices) {
        v *= p.scale;
        v.z() += p.offset;
    }
    return result;
}

indexed_triangle_set tac_joint_cutter_mesh(const TacJointParams& p)
{
    throw_if_invalid(validate_scalars(p));
    const auto&          r           = reference();
    const Vec3d          factors     = cutter_factors(p);
    const Vec3d          centre      = r.cutter_box.center();
    const Vec3d          translation = centre * double(p.scale) + Vec3d(0, 0, p.offset);
    indexed_triangle_set result      = r.cutter;
    for (auto& v : result.vertices)
        v = ((v.cast<double>() - centre).cwiseProduct(factors) + translation).cast<float>();
    return result;
}

std::vector<std::pair<std::string, double>> tac_joint_attributes(const TacJointParams& p)
{
    return {{"tac_joint_scale", p.scale},
            {"tac_joint_clearance", p.clearance},
            {"tac_joint_end_clearance", p.end_clearance},
            {"tac_joint_offset", p.offset},
            {"tac_joint_version", p.version}};
}

TacJointParams tac_joint_from_attributes(const std::function<std::optional<double>(const std::string&)>& get)
{
    TacJointParams p;
    const auto     read_float = [&](const char* key, float& value) {
        if (const auto attribute = get(key))
            value = std::isfinite(*attribute) && std::abs(*attribute) <= std::numeric_limits<float>::max() ?
                            float(*attribute) :
                            std::numeric_limits<float>::quiet_NaN();
    };
    read_float("tac_joint_scale", p.scale);
    read_float("tac_joint_clearance", p.clearance);
    read_float("tac_joint_end_clearance", p.end_clearance);
    read_float("tac_joint_offset", p.offset);
    if (const auto version = get("tac_joint_version"))
        p.version = std::isfinite(*version) && std::floor(*version) == *version && *version >= std::numeric_limits<int>::min() &&
                            *version <= std::numeric_limits<int>::max() ?
                        int(*version) :
                        -1;
    return p;
}

} // namespace Slic3r

#undef L
#pragma pop_macro("L")
