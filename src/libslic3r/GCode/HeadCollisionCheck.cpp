#include "HeadCollisionCheck.hpp"

// EdgeSlicer: check-only print-head collision pass. Idea after PancakeSlicer's ClearanceVerifier
// (AGPL-3.0, https://github.com/ZimengXiong/PancakeSlicer); independent implementation.

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "../BoundingBox.hpp"
#include "../HeadClearance.hpp"

namespace Slic3r {

namespace {

constexpr float NO_MATERIAL = -1e9f;
constexpr int   TILE        = 16; // cells per tile side for the coarse max

// Top of the material already printed, per cell of a regular XY grid, plus a coarse per-tile max
// for early rejection.
class HeightMap
{
public:
    HeightMap(const BoundingBoxf &bb, double cell) : m_x0(bb.min.x()), m_y0(bb.min.y()), m_cell(cell)
    {
        m_nx  = std::max(1, int(std::ceil((bb.max.x() - bb.min.x()) / cell)));
        m_ny  = std::max(1, int(std::ceil((bb.max.y() - bb.min.y()) / cell)));
        m_tnx = (m_nx + TILE - 1) / TILE;
        m_tny = (m_ny + TILE - 1) / TILE;
        m_top.assign(size_t(m_nx) * size_t(m_ny), NO_MATERIAL);
        m_tile_max.assign(size_t(m_tnx) * size_t(m_tny), NO_MATERIAL);
    }

    int    nx() const { return m_nx; }
    int    ny() const { return m_ny; }
    float  global_max() const { return m_global_max; }
    double cx(int ix) const { return m_x0 + (ix + 0.5) * m_cell; }
    double cy(int iy) const { return m_y0 + (iy + 0.5) * m_cell; }
    int    ix_floor(double x) const { return int(std::floor((x - m_x0) / m_cell)); }
    int    iy_floor(double y) const { return int(std::floor((y - m_y0) / m_cell)); }
    float  top(int ix, int iy) const { return m_top[size_t(iy) * m_nx + ix]; }

    // Clamp an inclusive cell rectangle to the grid. False if nothing is left.
    bool clamp(int &ix0, int &iy0, int &ix1, int &iy1) const
    {
        ix0 = std::max(ix0, 0);
        iy0 = std::max(iy0, 0);
        ix1 = std::min(ix1, m_nx - 1);
        iy1 = std::min(iy1, m_ny - 1);
        return ix0 <= ix1 && iy0 <= iy1;
    }

    // Upper bound of the material top over an (already clamped) cell rectangle.
    float coarse_max(int ix0, int iy0, int ix1, int iy1) const
    {
        float m = NO_MATERIAL;
        for (int ty = iy0 / TILE; ty <= iy1 / TILE; ++ty)
            for (int tx = ix0 / TILE; tx <= ix1 / TILE; ++tx)
                m = std::max(m, m_tile_max[size_t(ty) * m_tnx + tx]);
        return m;
    }

    // Visit every tile of the rectangle whose max exceeds threshold, cell by cell.
    template<typename Fn> void for_cells_above(int ix0, int iy0, int ix1, int iy1, float threshold, Fn &&fn) const
    {
        for (int ty = iy0 / TILE; ty <= iy1 / TILE; ++ty)
            for (int tx = ix0 / TILE; tx <= ix1 / TILE; ++tx) {
                if (m_tile_max[size_t(ty) * m_tnx + tx] <= threshold)
                    continue;
                const int cx0 = std::max(ix0, tx * TILE), cx1 = std::min(ix1, tx * TILE + TILE - 1);
                const int cy0 = std::max(iy0, ty * TILE), cy1 = std::min(iy1, ty * TILE + TILE - 1);
                for (int iy = cy0; iy <= cy1; ++iy)
                    for (int ix = cx0; ix <= cx1; ++ix) {
                        const float t = m_top[size_t(iy) * m_nx + ix];
                        if (t > threshold)
                            fn(ix, iy, t);
                    }
            }
    }

    // A bead from a to b (tip heights za, zb) of the given half width: every cell whose centre is
    // within half_width of the segment gets its top raised to the tip height at its projection.
    void deposit(const Vec2d &a, double za, const Vec2d &b, double zb, double half_width)
    {
        int ix0 = ix_floor(std::min(a.x(), b.x()) - half_width), ix1 = ix_floor(std::max(a.x(), b.x()) + half_width);
        int iy0 = iy_floor(std::min(a.y(), b.y()) - half_width), iy1 = iy_floor(std::max(a.y(), b.y()) + half_width);
        if (!clamp(ix0, iy0, ix1, iy1))
            return;
        const Vec2d  ab   = b - a;
        const double len2 = ab.squaredNorm();
        const double hw2  = half_width * half_width;
        for (int iy = iy0; iy <= iy1; ++iy)
            for (int ix = ix0; ix <= ix1; ++ix) {
                const Vec2d  p(cx(ix), cy(iy));
                const double t = len2 > 0. ? std::clamp((p - a).dot(ab) / len2, 0., 1.) : 0.;
                if ((a + ab * t - p).squaredNorm() > hw2)
                    continue;
                const float z = float(za + (zb - za) * t);
                float      &c = m_top[size_t(iy) * m_nx + ix];
                if (z > c) {
                    c        = z;
                    float &m = m_tile_max[size_t(iy / TILE) * m_tnx + ix / TILE];
                    m        = std::max(m, z);
                    m_global_max = std::max(m_global_max, z);
                }
            }
    }

private:
    double             m_x0, m_y0, m_cell;
    int                m_nx { 1 }, m_ny { 1 }, m_tnx { 1 }, m_tny { 1 };
    std::vector<float> m_top;
    std::vector<float> m_tile_max;
    float              m_global_max { NO_MATERIAL };
};

struct Contact
{
    float              depth { 0.f };
    const std::string *part { nullptr };
    Vec3f              position { Vec3f::Zero() };
};

const std::string NOZZLE_PART = "nozzle";

// Deepest contact of the head with its tip at p, or depth 0.
// surface_tan > 0 (curved tops, extrusion moves only, HeadCollisionParams::extrusion_surface_slope_deg):
// where the nozzle's underside is still within tol of the tip (the tip land, and a chamfer no higher
// than tol), material up to d * surface_tan above the tip is the sloped surface being followed - the
// neighbouring bead of a line laid on the slope - not an obstacle. Beyond the land nothing changes.
// part_depth (curved tops P3, may be null): the deepest contact of each head part at this point, by
// part index (HeadCollisionReport::parts order: nozzle tip, nozzle flank, then the boxes).
Contact probe(const HeightMap &map, const HeadClearance &head, const Vec3d &p, double tol, bool check_nozzle, double surface_tan,
              std::vector<float> *part_depth = nullptr)
{
    Contact best;
    auto    consider = [&](float depth, const std::string &part, size_t part_idx) {
        if (part_depth != nullptr && part_idx < part_depth->size())
            (*part_depth)[part_idx] = std::max((*part_depth)[part_idx], depth);
        if (depth > best.depth) {
            best.depth    = depth;
            best.part     = &part;
            best.position = p.cast<float>();
        }
    };

    // Nozzle land and cone.
    const double R = head.nozzle_max_radius();
    if (check_nozzle && R > 0.) {
        int ix0 = map.ix_floor(p.x() - R), ix1 = map.ix_floor(p.x() + R);
        int iy0 = map.iy_floor(p.y() - R), iy1 = map.iy_floor(p.y() + R);
        const float thr = float(p.z() + tol);
        if (map.clamp(ix0, iy0, ix1, iy1) && map.coarse_max(ix0, iy0, ix1, iy1) > thr) {
            map.for_cells_above(ix0, iy0, ix1, iy1, thr, [&](int ix, int iy, float top) {
                const double d = std::hypot(map.cx(ix) - p.x(), map.cy(iy) - p.y());
                double u = head.nozzle_underside_at(d);
                if (std::isinf(u))
                    return;
                // The tip land (underside within tol of the tip) or the flank above it.
                const size_t part_idx = u <= tol ? 0 : 1;
                // Curved tops, extrusions only: see surface_tan above.
                if (surface_tan > 0. && u <= tol)
                    u = std::max(u, d * surface_tan);
                const double excess = double(top) - (p.z() + u);
                if (excess > tol)
                    consider(float(excess), NOZZLE_PART, part_idx);
            });
        }
    }

    // Boxes. Nothing printed reaches the lowest box: done.
    if (head.boxes.empty() || double(map.global_max()) <= p.z() + head.lowest_box_z() + tol)
        return best;
    for (size_t box_idx = 0; box_idx < head.boxes.size(); ++box_idx) {
        const HeadClearanceBox &box    = head.boxes[box_idx];
        const float             bottom = float(p.z() + box.min.z());
        if (map.global_max() <= bottom + float(tol))
            continue;
        int ix0 = box.spans_x ? 0 : map.ix_floor(p.x() + box.min.x());
        int ix1 = box.spans_x ? map.nx() - 1 : map.ix_floor(p.x() + box.max.x());
        int iy0 = map.iy_floor(p.y() + box.min.y()), iy1 = map.iy_floor(p.y() + box.max.y());
        if (!map.clamp(ix0, iy0, ix1, iy1))
            continue;
        const float thr = bottom + float(tol);
        if (map.coarse_max(ix0, iy0, ix1, iy1) <= thr)
            continue;
        float deepest = 0.f;
        map.for_cells_above(ix0, iy0, ix1, iy1, thr, [&](int, int, float top) { deepest = std::max(deepest, top - bottom); });
        if (deepest > 0.f)
            consider(deepest, box.name, 2 + box_idx);
    }
    return best;
}

// Curved tops P3 (measurement only, never a contact): with the tip at p, lower `best` to the smallest
// gap between a box's bottom and the material top under it. Only cells that would lower it are visited.
void measure_box_clearance(const HeightMap &map, const HeadClearance &head, const Vec3d &p, HeadCollisionReport &report)
{
    for (const HeadClearanceBox &box : head.boxes) {
        const double bottom = p.z() + box.min.z();
        const double best   = report.min_box_clearance;
        const float  thr    = std::isfinite(best) ? float(bottom - best) : NO_MATERIAL + 1.f;
        if (map.global_max() <= thr)
            continue;
        int ix0 = box.spans_x ? 0 : map.ix_floor(p.x() + box.min.x());
        int ix1 = box.spans_x ? map.nx() - 1 : map.ix_floor(p.x() + box.max.x());
        int iy0 = map.iy_floor(p.y() + box.min.y()), iy1 = map.iy_floor(p.y() + box.max.y());
        if (!map.clamp(ix0, iy0, ix1, iy1) || map.coarse_max(ix0, iy0, ix1, iy1) <= thr)
            continue;
        float top_max = NO_MATERIAL;
        map.for_cells_above(ix0, iy0, ix1, iy1, thr, [&](int, int, float top) { top_max = std::max(top_max, top); });
        if (top_max > NO_MATERIAL && bottom - double(top_max) < report.min_box_clearance) {
            report.min_box_clearance          = bottom - double(top_max);
            report.min_box_clearance_part     = box.name;
            report.min_box_clearance_position = p.cast<float>();
        }
    }
}

} // namespace

HeadCollisionReport check_head_collisions(const std::vector<GCodeProcessorResult::MoveVertex> &moves,
                                          const HeadClearance                                  &head,
                                          const HeadCollisionParams                            &params,
                                          const std::function<void()>                          &throw_if_canceled)
{
    using MV = GCodeProcessorResult::MoveVertex;
    HeadCollisionReport report;

    auto skipped = [](const MV &m) { return m.prepare_stage || m.actual_speed_point; };

    // Grid over everything that is extruded, plus room for the nozzle around it.
    BoundingBoxf bb;
    {
        const MV *last = nullptr;
        for (const MV &m : moves) {
            if (skipped(m))
                continue;
            if (m.type == EMoveType::Extrude) {
                // Both ends of the bead: the start is the previous vertex.
                if (last != nullptr)
                    bb.merge(Vec2d(last->position.x(), last->position.y()));
                bb.merge(Vec2d(m.position.x(), m.position.y()));
            }
            last = &m;
        }
    }
    if (!bb.defined)
        return report;
    const double margin = std::max(1., head.nozzle_max_radius()) + 1.;
    bb.min -= Vec2d(margin, margin);
    bb.max += Vec2d(margin, margin);
    const Vec2d  size = bb.size();
    double       cell = params.cell > 0. ? params.cell : 0.1;
    if (size.x() * size.y() / (cell * cell) > double(params.max_cells))
        cell = std::sqrt(size.x() * size.y() / double(params.max_cells));
    HeightMap map(bb, cell);

    const double tol  = params.tolerance;
    const double step = 2. * cell;

    report.ran = true;
    // Per head part: moves that touch with it, and the deepest contact (curved tops P3: which part touched).
    report.parts.resize(2 + head.boxes.size());
    report.parts[0].part = "nozzle tip";
    report.parts[1].part = "nozzle flank";
    for (size_t b = 0; b < head.boxes.size(); ++b)
        report.parts[2 + b].part = head.boxes[b].name;
    std::vector<float> move_part_depth(report.parts.size(), 0.f);
    bool  have_prev = false;
    Vec3d prev      = Vec3d::Zero();
    for (size_t i = 0; i < moves.size(); ++i) {
        const MV &m = moves[i];
        if ((i & 4095) == 0 && throw_if_canceled)
            throw_if_canceled();
        if (skipped(m))
            continue;
        const Vec3d cur = m.position.cast<double>();
        if (!have_prev) {
            prev      = cur;
            have_prev = true;
            continue;
        }
        const Vec3d a = prev;
        prev          = cur;
        if ((cur - a).squaredNorm() < 1e-12)
            continue;

        const bool extrude = m.type == EMoveType::Extrude && m.delta_extruder > 0.f;
        ++report.moves_checked;

        // Ironing rubs the surface on purpose: only the boxes are checked for it.
        const bool check_nozzle = !(extrude && m.extrusion_role == erIroning);
        const bool   relaxed     = extrude && params.extrusion_surface_slope_deg > 0. &&
                                   m.layer_id >= params.extrusion_surface_slope_min_layer;
        const double surface_tan = relaxed ? std::tan(std::min(params.extrusion_surface_slope_deg, 60.) * M_PI / 180.) : 0.;

        Contact      worst;
        const double len_xy  = Vec2d(cur.x() - a.x(), cur.y() - a.y()).norm();
        const int    n       = len_xy < 1e-6 ? 1 : std::max(1, int(std::ceil(len_xy / step)));
        const bool   measure = m.layer_id >= params.measure_clearance_from_layer;
        std::fill(move_part_depth.begin(), move_part_depth.end(), 0.f);
        for (int k = 1; k <= n; ++k) {
            const double t = double(k) / double(n);
            const Contact c = probe(map, head, a + (cur - a) * t, tol, check_nozzle, surface_tan, &move_part_depth);
            if (c.depth > worst.depth)
                worst = c;
            if (measure)
                measure_box_clearance(map, head, a + (cur - a) * t, report);
        }
        for (size_t pi = 0; pi < move_part_depth.size(); ++pi)
            if (move_part_depth[pi] > 0.f) {
                ++report.parts[pi].hit_moves;
                report.parts[pi].max_depth = std::max(report.parts[pi].max_depth, move_part_depth[pi]);
            }
        if (worst.depth > 0.f) {
            ++report.hit_moves;
            report.max_depth = std::max(report.max_depth, worst.depth);
            if (report.hits.size() < params.max_reported) {
                HeadCollisionHit h;
                h.layer_id = m.layer_id;
                h.gcode_id = m.gcode_id;
                h.position = worst.position;
                h.depth    = worst.depth;
                h.travel   = !extrude;
                h.part     = *worst.part;
                report.hits.push_back(std::move(h));
            }
        }

        if (extrude) {
            const double w = m.width > 0.f ? double(m.width) : 0.4;
            map.deposit(Vec2d(a.x(), a.y()), a.z(), Vec2d(cur.x(), cur.y()), cur.z(), 0.5 * w);
        }
    }
    return report;
}

} // namespace Slic3r
