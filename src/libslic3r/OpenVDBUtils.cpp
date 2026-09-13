#define NOMINMAX
#include "OpenVDBUtils.hpp"

#ifdef _MSC_VER
// Suppress warning C4146 in OpenVDB: unary minus operator applied to unsigned type, result still unsigned 
#pragma warning(push)
#pragma warning(disable : 4146)
#endif // _MSC_VER
#include <openvdb/tools/MeshToVolume.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif // _MSC_VER

#include <openvdb/tools/VolumeToMesh.h>
#include <openvdb/tools/Composite.h>
#include <openvdb/tools/LevelSetRebuild.h>
// Ultra: round_by_voxels() below - LevelSetFilter::offset() is the erode/dilate
// that makes the rolling-ball fillet.
#include <openvdb/tools/LevelSetFilter.h>

// Ultra: round_band_by_voxels() - the spatial hash over the band points, and the
// clamps the blend needs.
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

//#include "MTUtils.hpp"

namespace Slic3r {

class TriangleMeshDataAdapter {
public:
    const indexed_triangle_set &its;
    float voxel_scale;

    size_t polygonCount() const { return its.indices.size(); }
    size_t pointCount() const   { return its.vertices.size(); }
    size_t vertexCount(size_t) const { return 3; }

    // Return position pos in local grid index space for polygon n and vertex v
    // The actual mesh will appear to openvdb as scaled uniformly by voxel_size
    // And the voxel count per unit volume can be affected this way.
    void getIndexSpacePoint(size_t n, size_t v, openvdb::Vec3d& pos) const
    {
        auto vidx = size_t(its.indices[n](Eigen::Index(v)));
        Slic3r::Vec3d p = its.vertices[vidx].cast<double>() * voxel_scale;
        pos = {p.x(), p.y(), p.z()};
    }

    TriangleMeshDataAdapter(const indexed_triangle_set &m, float voxel_sc = 1.f)
        : its{m}, voxel_scale{voxel_sc} {};
};

// TODO: Do I need to call initialize? Seems to work without it as well but the
// docs say it should be called ones. It does a mutex lock-unlock sequence all
// even if was called previously.
openvdb::FloatGrid::Ptr mesh_to_grid(const indexed_triangle_set &    mesh,
                                     const openvdb::math::Transform &tr,
                                     float voxel_scale,
                                     float exteriorBandWidth,
                                     float interiorBandWidth,
                                     int   flags)
{
    openvdb::initialize();

    std::vector<indexed_triangle_set> meshparts = its_split(mesh);

    auto it = std::remove_if(meshparts.begin(), meshparts.end(),
                             [](auto &m) { return its_volume(m) < EPSILON; });

    meshparts.erase(it, meshparts.end());

    openvdb::FloatGrid::Ptr grid;
    for (auto &m : meshparts) {
        auto subgrid = openvdb::tools::meshToVolume<openvdb::FloatGrid>(
            TriangleMeshDataAdapter{m, voxel_scale}, tr, exteriorBandWidth,
            interiorBandWidth, flags);

        if (grid && subgrid) openvdb::tools::csgUnion(*grid, *subgrid);
        else if (subgrid) grid = std::move(subgrid);
    }

    if (grid) {
        grid = openvdb::tools::levelSetRebuild(*grid, 0., exteriorBandWidth,
                                               interiorBandWidth);
    } else if(meshparts.empty()) {
        // Splitting failed, fall back to hollow the original mesh
        grid = openvdb::tools::meshToVolume<openvdb::FloatGrid>(
            TriangleMeshDataAdapter{mesh}, tr, exteriorBandWidth,
            interiorBandWidth, flags);
    }

    grid->insertMeta("voxel_scale", openvdb::FloatMetadata(voxel_scale));

    return grid;
}

// declared in MeshRepair.hpp (kept free of OpenVDB includes)
indexed_triangle_set remesh_by_voxels(const indexed_triangle_set &mesh, double voxel_size)
{
    if (mesh.indices.empty() || voxel_size <= 0.)
        return {};
    try {
        const float voxel_scale = float(1. / voxel_size);
        auto grid = mesh_to_grid(mesh, {}, voxel_scale, 3.0f, 3.0f);
        if (!grid)
            return {};
        indexed_triangle_set its = grid_to_mesh(*grid, 0.0, 0.0);
        // OpenVDB's volumeToMesh emits faces wound for inward normals (which is what
        // the SLA hollowing interior wants); an exterior surface must be flipped or
        // it renders dark and confuses ray picking.
        its_flip_triangles(its);
        return its;
    } catch (const std::exception &) {
        return {};
    }
}

// declared in MeshRepair.hpp (kept free of OpenVDB includes)
//
// "Round all edges": the rolling-ball fillet by morphological opening and closing
// of the signed distance field. See MeshRound.hpp for the full rationale; the
// mechanics that matter here are:
//
//  * mesh_to_grid() SCALES the mesh by voxel_scale into index space, where one
//    voxel is one unit. So a world radius r is r * voxel_scale GRID units, and
//    every distance handed to LevelSetFilter has to be in those units too.
//  * LevelSetFilter::offset(d) moves the surface inward by d (a positive d
//    erodes, a negative one dilates) and renormalises as it goes. Erode-then-
//    dilate is a morphological OPEN, which rounds convex edges; dilate-then-erode
//    is a CLOSE, which rounds concave ones.
//  * the narrow band has to be wide enough to hold the whole offset or the
//    surface walks off the end of it. mesh_to_grid's band width is in voxels, so
//    it is asked for r_grid + a margin on each side.
indexed_triangle_set round_by_voxels(const indexed_triangle_set &mesh,
                                     double                      radius,
                                     double                      voxel_size,
                                     bool                        round_concave)
{
    if (mesh.indices.empty() || voxel_size <= 0. || radius <= 0.)
        return {};
    try {
        const float voxel_scale = float(1. / voxel_size);
        // The offset distance in GRID units - which is just "how many voxels" -
        // and the band that has to contain it.
        const float r_grid = float(radius) * voxel_scale;
        const float band   = r_grid + 4.0f;

        auto grid = mesh_to_grid(mesh, {}, voxel_scale, band, band);
        if (!grid)
            return {};

        {
            openvdb::tools::LevelSetFilter<openvdb::FloatGrid> filter(*grid);
            // OPEN: erode by r, dilate back by r. Every convex edge comes back as
            // a quarter-round of radius r; the flat faces return to where they
            // were because the two offsets cancel on them.
            filter.offset(r_grid);
            filter.offset(-r_grid);
            if (round_concave) {
                // CLOSE: the mirror image, for the inside corners.
                filter.offset(-r_grid);
                filter.offset(r_grid);
            }
        }

        // The four offsets leave the field only approximately signed-distance
        // away from the surface, and volumeToMesh is happier with a clean one.
        grid = openvdb::tools::levelSetRebuild(*grid, 0.f, 3.f, 3.f);
        if (!grid)
            return {};
        grid->insertMeta("voxel_scale", openvdb::FloatMetadata(voxel_scale));

        indexed_triangle_set its = grid_to_mesh(*grid, 0.0, 0.0);
        if (its.indices.empty())
            return {};
        // Same winding fix remesh_by_voxels needs, and for the same reason.
        its_flip_triangles(its);
        return its;
    } catch (const std::exception &) {
        return {};
    }
}

// Ultra: the bevel's localized fallback. See MeshRepair.hpp for the contract.
//
// THE MECHANISM IS A FIELD BLEND, not a cut and a stitch, and that is deliberate.
// Cutting the rounded mesh and the original against a band boundary and sewing the
// two together means reconciling two independent tessellations along a closed
// curve - the failure mode is a leak, and it is a leak in someone's printable
// part. Blending the two signed distance FIELDS and extracting once cannot leak:
// whatever the blend does, the result is the zero set of a single continuous
// function on a single grid, so it is closed.
//
//   original(x)                       for d(x) > band_radius + feather
//   lerp(rounded, original, t)        in the feather
//   rounded(x)                        for d(x) < band_radius
//
// where d(x) is the distance from x to the nearest of `band_points`. The two grids
// share a transform (both are built from the same mesh at the same voxel scale),
// so their voxels correspond and the lerp is per-voxel.
indexed_triangle_set round_band_by_voxels(const indexed_triangle_set &mesh,
                                          const std::vector<Vec3f>   &band_points,
                                          double                      band_radius,
                                          double                      radius,
                                          double                      voxel_size)
{
    if (mesh.indices.empty() || voxel_size <= 0. || radius <= 0. || band_points.empty() ||
        band_radius <= 0.)
        return {};
    try {
        const float voxel_scale = float(1. / voxel_size);
        const float r_grid      = float(radius) * voxel_scale;
        const float band        = r_grid + 4.0f;

        // The ORIGINAL field, kept to blend back toward.
        auto base = mesh_to_grid(mesh, {}, voxel_scale, band, band);
        if (!base)
            return {};
        // The ROUNDED field: a deep copy, so `base` keeps the original surface.
        auto rounded = base->deepCopy();
        {
            openvdb::tools::LevelSetFilter<openvdb::FloatGrid> filter(*rounded);
            // OPEN only. The concave half of round_by_voxels() is deliberately not
            // run here: this stands in for a BEVEL, which is a convex-edge
            // operation (MeshEdit drops concave edges and says so), and a CLOSE
            // would fill inside corners the user never selected.
            filter.offset(r_grid);
            filter.offset(-r_grid);
        }

        // Blend, in GRID index space - which is world space scaled by voxel_scale,
        // the same convention mesh_to_grid's adapter uses for the mesh itself.
        const float                      br_grid   = float(band_radius) * voxel_scale;
        const float                      feather   = std::max(br_grid, 1e-3f);
        const float                      outer     = br_grid + feather;
        std::vector<openvdb::Vec3s>      pts;
        pts.reserve(band_points.size());
        for (const Vec3f &p : band_points)
            pts.emplace_back(openvdb::Vec3s(p.x() * voxel_scale, p.y() * voxel_scale,
                                            p.z() * voxel_scale));

        // A uniform grid over the band points, so the nearest-point query is a
        // lookup in the 27 cells around a voxel rather than a scan of the whole
        // chain. Without it this is O(voxels x chain), which on a Benchy rim (650
        // points) over a band of a few hundred thousand voxels is the same
        // quadratic behaviour the geometric path was just cured of.
        const float cell = std::max(outer, 1.f);
        std::unordered_map<uint64_t, std::vector<int>> buckets;
        auto cell_key = [cell](float x, float y, float z) -> uint64_t {
            const int64_t i = int64_t(std::floor(x / cell));
            const int64_t j = int64_t(std::floor(y / cell));
            const int64_t k = int64_t(std::floor(z / cell));
            // A hash of the three cell indices; collisions only cost a longer
            // candidate list, never a wrong answer, because the distance is
            // recomputed exactly for every candidate.
            return (uint64_t(uint32_t(int32_t(i))) * 0x9E3779B97F4A7C15ull) ^
                   (uint64_t(uint32_t(int32_t(j))) * 0xC2B2AE3D27D4EB4Full) ^
                   (uint64_t(uint32_t(int32_t(k))) * 0x165667B19E3779F9ull);
        };
        for (int i = 0; i < int(pts.size()); ++i)
            buckets[cell_key(pts[size_t(i)].x(), pts[size_t(i)].y(), pts[size_t(i)].z())]
                .push_back(i);

        auto dist_to_band = [&](const openvdb::Vec3s &p) -> float {
            float best = std::numeric_limits<float>::max();
            for (int di = -1; di <= 1; ++di)
                for (int dj = -1; dj <= 1; ++dj)
                    for (int dk = -1; dk <= 1; ++dk) {
                        auto it = buckets.find(cell_key(p.x() + float(di) * cell,
                                                        p.y() + float(dj) * cell,
                                                        p.z() + float(dk) * cell));
                        if (it == buckets.end())
                            continue;
                        for (int idx : it->second) {
                            const openvdb::Vec3s d = pts[size_t(idx)] - p;
                            best = std::min(best, d.lengthSqr());
                        }
                    }
            return best == std::numeric_limits<float>::max() ? best : std::sqrt(best);
        };

        // Walk the ROUNDED grid's active voxels and pull each back toward the
        // original by its blend weight. Voxels the round did not touch are already
        // equal in both fields, so nothing needs doing to them.
        {
            openvdb::FloatGrid::Accessor  acc_r = rounded->getAccessor();
            openvdb::FloatGrid::ConstAccessor acc_b = base->getConstAccessor();
            for (auto it = rounded->beginValueOn(); it; ++it) {
                const openvdb::Coord   c = it.getCoord();
                const openvdb::Vec3s   p(float(c.x()), float(c.y()), float(c.z()));
                const float            d = dist_to_band(p);
                if (d <= br_grid)
                    continue;                       // fully rounded: leave it
                const float bv = acc_b.getValue(c);
                if (d >= outer) {
                    acc_r.setValue(c, bv);          // fully original
                    continue;
                }
                const float t = (d - br_grid) / feather;   // 0 at the band, 1 outside
                acc_r.setValue(c, it.getValue() * (1.f - t) + bv * t);
            }
        }

        // The blend leaves the field only approximately signed-distance, exactly as
        // the four offsets do in round_by_voxels(); rebuild before extracting.
        auto out_grid = openvdb::tools::levelSetRebuild(*rounded, 0.f, 3.f, 3.f);
        if (!out_grid)
            return {};
        out_grid->insertMeta("voxel_scale", openvdb::FloatMetadata(voxel_scale));

        indexed_triangle_set its = grid_to_mesh(*out_grid, 0.0, 0.0);
        if (its.indices.empty())
            return {};
        its_flip_triangles(its);
        return its;
    } catch (const std::exception &) {
        return {};
    }
}

bool voxel_ops_available() { return true; }

indexed_triangle_set grid_to_mesh(const openvdb::FloatGrid &grid,
                          double                    isovalue,
                          double                    adaptivity,
                          bool                      relaxDisorientedTriangles)
{
    openvdb::initialize();

    std::vector<openvdb::Vec3s> points;
    std::vector<openvdb::Vec3I> triangles;
    std::vector<openvdb::Vec4I> quads;

    openvdb::tools::volumeToMesh(grid, points, triangles, quads, isovalue,
                                 adaptivity, relaxDisorientedTriangles);

    float scale = 1.;
    try {
        scale = grid.template metaValue<float>("voxel_scale");
    }  catch (...) { }

    indexed_triangle_set ret;
    ret.vertices.reserve(points.size());
    ret.indices.reserve(triangles.size() + quads.size() * 2);

    for (auto &v : points) ret.vertices.emplace_back(to_vec3f(v) / scale);
    for (auto &v : triangles) ret.indices.emplace_back(to_vec3i(v));
    for (auto &quad : quads) {
        ret.indices.emplace_back(quad(0), quad(1), quad(2));
        ret.indices.emplace_back(quad(2), quad(3), quad(0));
    }

    return ret;
}

openvdb::FloatGrid::Ptr redistance_grid(const openvdb::FloatGrid &grid,
                                        double                    iso,
                                        double                    er,
                                        double                    ir)
{
    auto new_grid = openvdb::tools::levelSetRebuild(grid, float(iso),
                                                    float(er), float(ir));

    // Copies voxel_scale metadata, if it exists.
    new_grid->insertMeta(*grid.deepCopyMeta());

    return new_grid;
}

} // namespace Slic3r
