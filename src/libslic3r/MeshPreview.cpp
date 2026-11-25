#include "MeshPreview.hpp"

#include "libslic3r.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

namespace {

// Every pixel is drawn from SAMPLES x SAMPLES samples, which smooths the edges
const int    SAMPLES     = 2;
const int    MAX_SIDE    = 2048;
const double MARGIN      = 0.04; // of the picture, on every side
const float  AMBIENT     = 0.38f;

} // namespace

MeshPreviewImage render_mesh_preview(const std::vector<MeshPreviewItem> &items, int width, int height, double yaw_deg, double pitch_deg)
{
    MeshPreviewImage out;
    if (width <= 0 || height <= 0 || width > MAX_SIDE || height > MAX_SIDE)
        return out;
    out.width  = width;
    out.height = height;
    out.rgba.assign(size_t(width) * size_t(height) * 4, 0);

    const double yaw   = yaw_deg * PI / 180.;
    const double pitch = std::clamp(pitch_deg, 0., 90.) * PI / 180.;
    const float  cy = float(std::cos(yaw)), sy = float(std::sin(yaw));
    const float  cp = float(std::cos(pitch)), sp = float(std::sin(pitch));
    // x .. right, y .. up, z .. towards the viewer
    auto to_view = [cy, sy, cp, sp](const Vec3f &p) {
        float x = p.x() * cy - p.y() * sy;
        float y = p.x() * sy + p.y() * cy;
        return Vec3f(x, y * sp + p.z() * cp, -y * cp + p.z() * sp);
    };

    // Vertices in the frame of the view
    std::vector<std::vector<Vec3f>> points(items.size());
    Vec3f                           lo = Vec3f::Constant(std::numeric_limits<float>::max());
    Vec3f                           hi = Vec3f::Constant(std::numeric_limits<float>::lowest());
    bool                            any = false;
    for (size_t i = 0; i < items.size(); ++i) {
        const MeshPreviewItem &item = items[i];
        if (item.mesh == nullptr || item.mesh->indices.empty())
            continue;
        points[i].reserve(item.mesh->vertices.size());
        for (const stl_vertex &v : item.mesh->vertices) {
            Vec3f p = to_view(Vec3f(v.x(), v.y(), v.z()) + item.offset);
            if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || !std::isfinite(p.z()))
                p = Vec3f::Zero();
            points[i].push_back(p);
            lo  = lo.cwiseMin(p);
            hi  = hi.cwiseMax(p);
            any = true;
        }
    }
    if (!any)
        return out;

    const int   W = width * SAMPLES, H = height * SAMPLES;
    const float size_x = std::max(hi.x() - lo.x(), 1e-6f), size_y = std::max(hi.y() - lo.y(), 1e-6f);
    const float scale  = float(1. - 2. * MARGIN) * std::min(float(W) / size_x, float(H) / size_y);
    const float mid_x = 0.5f * (lo.x() + hi.x()), mid_y = 0.5f * (lo.y() + hi.y());
    auto        to_px = [scale, mid_x, mid_y, W, H](const Vec3f &p) {
        // the picture has Y down
        return Vec3f((p.x() - mid_x) * scale + 0.5f * float(W), 0.5f * float(H) - (p.y() - mid_y) * scale, p.z());
    };

    std::vector<float>   depth(size_t(W) * size_t(H), std::numeric_limits<float>::lowest());
    std::vector<uint8_t> color(size_t(W) * size_t(H) * 3, 0);
    std::vector<uint8_t> covered(size_t(W) * size_t(H), 0);
    const Vec3f          light = Vec3f(0.35f, 0.45f, 0.82f).normalized();

    for (size_t i = 0; i < items.size(); ++i) {
        const MeshPreviewItem &item = items[i];
        if (points[i].empty())
            continue;
        const size_t count = points[i].size();
        for (const stl_triangle_vertex_indices &face : item.mesh->indices) {
            if (face[0] < 0 || face[1] < 0 || face[2] < 0 || size_t(face[0]) >= count || size_t(face[1]) >= count || size_t(face[2]) >= count)
                continue;
            const Vec3f &va = points[i][size_t(face[0])], &vb = points[i][size_t(face[1])], &vc = points[i][size_t(face[2])];
            Vec3f normal = (vb - va).cross(vc - va);
            float length = normal.norm();
            if (length <= 0.f)
                continue;
            normal /= length;
            // the side turned to the viewer is the one which is seen
            if (normal.z() < 0.f)
                normal = -normal;
            const float   shade = AMBIENT + (1.f - AMBIENT) * std::max(0.f, normal.dot(light));
            const uint8_t rgb[3] = {uint8_t(std::min(255.f, float(item.color[0]) * shade)),
                                    uint8_t(std::min(255.f, float(item.color[1]) * shade)),
                                    uint8_t(std::min(255.f, float(item.color[2]) * shade))};

            const Vec3f a = to_px(va), b = to_px(vb), c = to_px(vc);
            const float area = (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
            if (std::abs(area) < 1e-9f)
                continue;
            const int x0 = std::max(0, int(std::floor(std::min({a.x(), b.x(), c.x()}))));
            const int x1 = std::min(W - 1, int(std::ceil(std::max({a.x(), b.x(), c.x()}))));
            const int y0 = std::max(0, int(std::floor(std::min({a.y(), b.y(), c.y()}))));
            const int y1 = std::min(H - 1, int(std::ceil(std::max({a.y(), b.y(), c.y()}))));
            const float inv = 1.f / area;
            for (int y = y0; y <= y1; ++y) {
                const float py = float(y) + 0.5f;
                for (int x = x0; x <= x1; ++x) {
                    const float px = float(x) + 0.5f;
                    // barycentric coordinates of the middle of the sample
                    const float wa = ((b.x() - px) * (c.y() - py) - (b.y() - py) * (c.x() - px)) * inv;
                    const float wb = ((c.x() - px) * (a.y() - py) - (c.y() - py) * (a.x() - px)) * inv;
                    const float wc = 1.f - wa - wb;
                    if (wa < 0.f || wb < 0.f || wc < 0.f)
                        continue;
                    const float  z   = wa * a.z() + wb * b.z() + wc * c.z();
                    const size_t idx = size_t(y) * size_t(W) + size_t(x);
                    if (z <= depth[idx])
                        continue;
                    depth[idx]         = z;
                    covered[idx]       = 1;
                    color[idx * 3]     = rgb[0];
                    color[idx * 3 + 1] = rgb[1];
                    color[idx * 3 + 2] = rgb[2];
                }
            }
        }
    }

    // Samples -> pixels
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            int sum[3] = {0, 0, 0}, hits = 0;
            for (int sy2 = 0; sy2 < SAMPLES; ++sy2)
                for (int sx2 = 0; sx2 < SAMPLES; ++sx2) {
                    const size_t idx = size_t(y * SAMPLES + sy2) * size_t(W) + size_t(x * SAMPLES + sx2);
                    if (covered[idx] == 0)
                        continue;
                    ++hits;
                    sum[0] += color[idx * 3];
                    sum[1] += color[idx * 3 + 1];
                    sum[2] += color[idx * 3 + 2];
                }
            if (hits == 0)
                continue;
            uint8_t *px = &out.rgba[(size_t(y) * size_t(width) + size_t(x)) * 4];
            px[0]       = uint8_t(sum[0] / hits);
            px[1]       = uint8_t(sum[1] / hits);
            px[2]       = uint8_t(sum[2] / hits);
            px[3]       = uint8_t(255 * hits / (SAMPLES * SAMPLES));
        }
    return out;
}

} // namespace Slic3r
