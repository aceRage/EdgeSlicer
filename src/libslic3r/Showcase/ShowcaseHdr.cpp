#include "ShowcaseHdr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <limits>

#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

namespace Slic3r {
namespace Showcase {

namespace {

using hdr_json = nlohmann::json;
using hdr_vec3 = std::array<float, 3>;

constexpr float HDR_PI      = 3.14159265358979323846f;
constexpr float HDR_RGBM_RANGE = 16.f;

void hdr_fail(std::string *error, const char *what)
{
    if (error != nullptr)
        *error = what;
}

float hdr_luma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

hdr_vec3 hdr_normalized(const hdr_vec3 &v)
{
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(len > 1e-20f))
        return { 0.f, 0.f, 1.f };
    return { v[0] / len, v[1] / len, v[2] / len };
}

float hdr_dot(const hdr_vec3 &a, const hdr_vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

float hdr_smoothstep(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// ---------------------------------------------------------------------------------------------
// The panorama's pixel grid: per-row and per-column trigonometry, and the exact row solid angles
// (scripts/showcase/bake_showcase_assets.py equirect_dirs / equirect_solid_angles).
// ---------------------------------------------------------------------------------------------
struct HdrGrid
{
    int                width = 0, height = 0;
    std::vector<float> cos_theta, sin_theta, cos_phi, sin_phi, omega;   // omega per row (all columns equal)

    HdrGrid(int w, int h) : width(w), height(h)
    {
        cos_theta.resize(size_t(h));
        sin_theta.resize(size_t(h));
        omega.resize(size_t(h));
        cos_phi.resize(size_t(w));
        sin_phi.resize(size_t(w));
        for (int y = 0; y < h; ++y) {
            const double theta = (0.5 - (double(y) + 0.5) / double(h)) * double(HDR_PI);
            cos_theta[size_t(y)] = float(std::cos(theta));
            sin_theta[size_t(y)] = float(std::sin(theta));
            const double top = double(HDR_PI) / 2. - double(y) * double(HDR_PI) / double(h);
            const double bot = double(HDR_PI) / 2. - double(y + 1) * double(HDR_PI) / double(h);
            omega[size_t(y)] = float((std::sin(top) - std::sin(bot)) * (2. * double(HDR_PI) / double(w)));
        }
        for (int x = 0; x < w; ++x) {
            const double phi = (0.5 - (double(x) + 0.5) / double(w)) * 2. * double(HDR_PI);
            cos_phi[size_t(x)] = float(std::cos(phi));
            sin_phi[size_t(x)] = float(std::sin(phi));
        }
    }
    hdr_vec3 dir(int x, int y) const
    {
        return { cos_theta[size_t(y)] * cos_phi[size_t(x)], cos_theta[size_t(y)] * sin_phi[size_t(x)], sin_theta[size_t(y)] };
    }
};

std::array<float, 9> hdr_sh_basis(float x, float y, float z)
{
    return { 0.282095f, 0.488603f * y, 0.488603f * z, 0.488603f * x, 1.092548f * x * y, 1.092548f * y * z, 0.315392f * (3.f * z * z - 1.f),
             1.092548f * x * z, 0.546274f * (x * x - y * y) };
}

// ---------------------------------------------------------------------------------------------
// Cube maps in GL cube space, (face * n + j) * n + i texels of three floats (the bake script's layout).
// ---------------------------------------------------------------------------------------------
struct HdrCube
{
    int                n = 0;
    std::vector<float> rgb;
    explicit HdrCube(int n_ = 0) : n(n_), rgb(size_t(6) * size_t(n_) * size_t(n_) * 3, 0.f) {}
    float *at(int face, int j, int i) { return rgb.data() + ((size_t(face) * size_t(n) + size_t(j)) * size_t(n) + size_t(i)) * 3; }
    const float *at(int face, int j, int i) const { return rgb.data() + ((size_t(face) * size_t(n) + size_t(j)) * size_t(n) + size_t(i)) * 3; }
};

// GL-space unit direction of the centre of texel (i, j) of a face of an n x n cube.
hdr_vec3 hdr_cube_dir(int face, int n, int i, int j)
{
    const float sc = 2.f * (float(i) + 0.5f) / float(n) - 1.f;
    const float tc = 2.f * (float(j) + 0.5f) / float(n) - 1.f;
    hdr_vec3 d;
    switch (face) {
    case 0: d = { 1.f, -tc, -sc }; break;
    case 1: d = { -1.f, -tc, sc }; break;
    case 2: d = { sc, 1.f, tc }; break;
    case 3: d = { sc, -1.f, -tc }; break;
    case 4: d = { sc, -tc, 1.f }; break;
    default: d = { -sc, -tc, -1.f }; break;
    }
    return hdr_normalized(d);
}

// Standard GL face selection (the inverse of hdr_cube_dir).
void hdr_dir_to_texel(const hdr_vec3 &g, int n, int &face, int &i, int &j)
{
    const float x = g[0], y = g[1], z = g[2];
    const float ax = std::abs(x), ay = std::abs(y), az = std::abs(z);
    float       ma, sc, tc;
    if (ax >= ay && ax >= az) {
        face = x >= 0.f ? 0 : 1;
        ma   = ax;
        sc   = face == 0 ? -z : z;
        tc   = -y;
    } else if (ay >= az) {
        face = y >= 0.f ? 2 : 3;
        ma   = ay;
        sc   = x;
        tc   = face == 2 ? z : -z;
    } else {
        face = z >= 0.f ? 4 : 5;
        ma   = az;
        sc   = face == 4 ? x : -x;
        tc   = -y;
    }
    sc /= ma;
    tc /= ma;
    i = std::clamp(int(std::floor((sc + 1.f) * 0.5f * float(n))), 0, n - 1);
    j = std::clamp(int(std::floor((tc + 1.f) * 0.5f * float(n))), 0, n - 1);
}

// Solid angle of texel (i, j) of an n x n face.
float hdr_texel_solid_angle(int n, int i, int j)
{
    auto a = [](double x, double y) { return std::atan2(x * y, std::sqrt(x * x + y * y + 1.)); };
    const double x0 = -1. + 2. * double(i) / double(n), x1 = -1. + 2. * double(i + 1) / double(n);
    const double y0 = -1. + 2. * double(j) / double(n), y1 = -1. + 2. * double(j + 1) / double(n);
    return float(a(x0, y0) - a(x0, y1) - a(x1, y0) + a(x1, y1));
}

std::vector<float> hdr_face_solid_angles(int n)
{
    std::vector<float> w(size_t(n) * size_t(n));
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            w[size_t(j) * size_t(n) + size_t(i)] = hdr_texel_solid_angle(n, i, j);
    return w;
}

// Bilinear lookup of the panorama (wrapping in u), world direction.
hdr_vec3 hdr_sample_equirect(const HdrImage &img, const hdr_vec3 &w)
{
    float u, v;
    direction_to_equirect(w, u, v);
    const float fx = u * float(img.width) - 0.5f;
    const float fy = std::clamp(v * float(img.height) - 0.5f, 0.f, float(img.height - 1));
    const int   x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float tx = fx - float(x0), ty = fy - float(y0);
    const int   y1 = std::min(y0 + 1, img.height - 1);
    const int   xa = ((x0 % img.width) + img.width) % img.width, xb = (xa + 1) % img.width;
    auto px = [&](int x, int y, int c) { return img.rgb[(size_t(y) * size_t(img.width) + size_t(x)) * 3 + size_t(c)]; };
    hdr_vec3 out;
    for (int c = 0; c < 3; ++c)
        out[size_t(c)] = (px(xa, y0, c) * (1.f - tx) + px(xb, y0, c) * tx) * (1.f - ty) + (px(xa, y1, c) * (1.f - tx) + px(xb, y1, c) * tx) * ty;
    return out;
}

// Area-averaged cube map of the panorama: every source pixel, split into 2 x 2 pieces, is
// accumulated into the texel its direction falls in, weighted by its solid angle. A texel no
// piece reached (a very small panorama) is read from the panorama directly.
HdrCube hdr_equirect_to_cube(const HdrImage &img, int n)
{
    const HdrGrid      grid(img.width, img.height);
    const size_t       texels = size_t(6) * size_t(n) * size_t(n);
    std::vector<double> acc(texels * 3, 0.), wsum(texels, 0.);
    std::vector<float> cp(size_t(img.width)), sp(size_t(img.width));
    for (int sy = 0; sy < 2; ++sy)
        for (int sx = 0; sx < 2; ++sx) {
            // Sub-sample positions inside the pixel: (a + 0.5) / 2.
            const float dx = (float(sx) + 0.5f) / 2.f, dy = (float(sy) + 0.5f) / 2.f;
            for (int x = 0; x < img.width; ++x) {
                const double phi = (0.5 - (double(x) + double(dx)) / double(img.width)) * 2. * double(HDR_PI);
                cp[size_t(x)] = float(std::cos(phi));
                sp[size_t(x)] = float(std::sin(phi));
            }
            for (int y = 0; y < img.height; ++y) {
                const double theta = (0.5 - (double(y) + double(dy)) / double(img.height)) * double(HDR_PI);
                const float  ct = float(std::cos(theta)), st = float(std::sin(theta));
                const double w = double(grid.omega[size_t(y)]) / 4.;
                for (int x = 0; x < img.width; ++x) {
                    const hdr_vec3 gl{ ct * cp[size_t(x)], st, -ct * sp[size_t(x)] };
                    int face, i, j;
                    hdr_dir_to_texel(gl, n, face, i, j);
                    const size_t t = (size_t(face) * size_t(n) + size_t(j)) * size_t(n) + size_t(i);
                    wsum[t] += w;
                    const float *p = img.rgb.data() + (size_t(y) * size_t(img.width) + size_t(x)) * 3;
                    acc[t * 3 + 0] += double(p[0]) * w;
                    acc[t * 3 + 1] += double(p[1]) * w;
                    acc[t * 3 + 2] += double(p[2]) * w;
                }
            }
        }
    HdrCube cube(n);
    for (int face = 0; face < 6; ++face)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const size_t t = (size_t(face) * size_t(n) + size_t(j)) * size_t(n) + size_t(i);
                float       *o = cube.at(face, j, i);
                if (wsum[t] > 0.) {
                    for (int c = 0; c < 3; ++c)
                        o[c] = float(acc[t * 3 + size_t(c)] / wsum[t]);
                } else {
                    const hdr_vec3 gl = hdr_cube_dir(face, n, i, j);
                    const hdr_vec3 v  = hdr_sample_equirect(img, { gl[0], -gl[2], gl[1] });
                    o[0] = v[0];
                    o[1] = v[1];
                    o[2] = v[2];
                }
            }
    return cube;
}

// 2 x 2 solid-angle-weighted box filter.
HdrCube hdr_cube_downsample(const HdrCube &c)
{
    const int          n = c.n, h = n / 2;
    const std::vector<float> dw = hdr_face_solid_angles(n);
    HdrCube            out(h);
    for (int face = 0; face < 6; ++face)
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < h; ++i) {
                double num[3] = { 0., 0., 0. }, den = 0.;
                for (int b = 0; b < 2; ++b)
                    for (int a = 0; a < 2; ++a) {
                        const int    ii = i * 2 + a, jj = j * 2 + b;
                        const double w  = double(dw[size_t(jj) * size_t(n) + size_t(ii)]);
                        const float *p  = c.at(face, jj, ii);
                        num[0] += double(p[0]) * w;
                        num[1] += double(p[1]) * w;
                        num[2] += double(p[2]) * w;
                        den += w;
                    }
                float *o = out.at(face, j, i);
                for (int k = 0; k < 3; ++k)
                    o[k] = float(num[k] / den);
            }
    return out;
}

// The split-sum GGX prefilter with N = V = R, evaluated by quadrature over every texel of the source
// cube (the bake script's method; it is what importance sampling converges to, without noise):
//   P(n) = sum_l L(l) D(h) (n.l) / sum_l D(h) (n.l),  (n.h)^2 = (1 + n.l) / 2.
HdrCube hdr_ggx_prefilter(const HdrCube &src, int n_out, float roughness)
{
    const double alpha = double(roughness) * double(roughness);
    const double a2    = alpha * alpha;
    // The weight depends on n.l only: a table over [-1, 1], and the n.l below which it is negligible.
    constexpr int TABLE = 8192;
    std::vector<float> table(TABLE);
    float              wmax = 0.f;
    for (int k = 0; k < TABLE; ++k) {
        const double c    = -1. + 2. * double(k) / double(TABLE - 1);
        const double nh2  = 0.5 * (1. + c);
        const double den  = nh2 * (a2 - 1.) + 1.;
        const double d    = a2 / (double(HDR_PI) * den * den);
        table[size_t(k)]  = float(d * std::max(c, 0.));
        wmax              = std::max(wmax, table[size_t(k)]);
    }
    int first = TABLE - 1;
    for (int k = TABLE - 1; k >= 0 && table[size_t(k)] >= 1e-6f * wmax; --k)
        first = k;
    const float cutoff = -1.f + 2.f * float(first) / float(TABLE - 1);

    const int                ns = src.n;
    const size_t             count = size_t(6) * size_t(ns) * size_t(ns);
    const std::vector<float> dw    = hdr_face_solid_angles(ns);
    std::vector<float>       dirs(count * 3), wrgb(count * 4);   // wrgb: L * dw, dw
    for (int face = 0; face < 6; ++face)
        for (int j = 0; j < ns; ++j)
            for (int i = 0; i < ns; ++i) {
                const size_t t = (size_t(face) * size_t(ns) + size_t(j)) * size_t(ns) + size_t(i);
                const hdr_vec3 d = hdr_cube_dir(face, ns, i, j);
                const float    w = dw[size_t(j) * size_t(ns) + size_t(i)];
                const float   *p = src.at(face, j, i);
                dirs[t * 3 + 0] = d[0];
                dirs[t * 3 + 1] = d[1];
                dirs[t * 3 + 2] = d[2];
                wrgb[t * 4 + 0] = p[0] * w;
                wrgb[t * 4 + 1] = p[1] * w;
                wrgb[t * 4 + 2] = p[2] * w;
                wrgb[t * 4 + 3] = w;
            }

    HdrCube      out(n_out);
    const size_t out_count = size_t(6) * size_t(n_out) * size_t(n_out);
    const float  scale_t   = 0.5f * float(TABLE - 1);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, out_count, 16), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t t = range.begin(); t != range.end(); ++t) {
            const int face = int(t / (size_t(n_out) * size_t(n_out)));
            const int rem  = int(t % (size_t(n_out) * size_t(n_out)));
            const hdr_vec3 nd = hdr_cube_dir(face, n_out, rem % n_out, rem / n_out);
            double sum[4] = { 0., 0., 0., 0. };
            for (size_t s = 0; s < count; ++s) {
                const float c = nd[0] * dirs[s * 3 + 0] + nd[1] * dirs[s * 3 + 1] + nd[2] * dirs[s * 3 + 2];
                if (c < cutoff)
                    continue;
                const float  ft = (c + 1.f) * scale_t;
                const int    k0 = std::min(int(ft), TABLE - 2);
                const float  w  = table[size_t(k0)] + (table[size_t(k0) + 1] - table[size_t(k0)]) * (ft - float(k0));
                sum[0] += double(w * wrgb[s * 4 + 0]);
                sum[1] += double(w * wrgb[s * 4 + 1]);
                sum[2] += double(w * wrgb[s * 4 + 2]);
                sum[3] += double(w * wrgb[s * 4 + 3]);
            }
            float *o = out.rgb.data() + t * 3;
            if (sum[3] > 0.) {
                o[0] = float(sum[0] / sum[3]);
                o[1] = float(sum[1] / sum[3]);
                o[2] = float(sum[2] / sum[3]);
            } else {
                const float *p = src.rgb.data();   // unreachable for a real lobe; keep something sane
                o[0] = p[0];
                o[1] = p[1];
                o[2] = p[2];
            }
        }
    });
    return out;
}

void hdr_rgbm_encode(const float *rgb, uint8_t *out)
{
    const float peak = std::max(rgb[0], std::max(rgb[1], rgb[2]));
    const float m    = std::clamp(peak / HDR_RGBM_RANGE, 1e-6f, 1.f);
    const float a    = std::ceil(m * 255.f) / 255.f;
    for (int c = 0; c < 3; ++c) {
        const float stored = std::clamp(rgb[c] / (a * HDR_RGBM_RANGE), 0.f, 1.f);
        out[c]             = uint8_t(std::lround(stored * 255.f));
    }
    out[3] = uint8_t(std::lround(a * 255.f));
}

} // namespace

// =================================================================================================
// Radiance RGBE
// =================================================================================================

bool decode_rgbe(const uint8_t *data, size_t size, HdrImage &out, std::string *error)
{
    out = HdrImage();
    size_t pos = 0;
    auto read_line = [&](std::string &line) {
        line.clear();
        while (pos < size && data[pos] != '\n')
            line.push_back(char(data[pos++]));
        if (pos >= size)
            return false;
        ++pos;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        return true;
    };
    std::string line;
    if (!read_line(line) || line.compare(0, 2, "#?") != 0) {
        hdr_fail(error, "not a Radiance file");
        return false;
    }
    double exposure = 1.;
    for (;;) {
        if (!read_line(line)) {
            hdr_fail(error, "truncated header");
            return false;
        }
        if (line.empty())
            break;
        if (line.compare(0, 7, "FORMAT=") == 0 && line.compare(7, std::string::npos, "32-bit_rle_rgbe") != 0) {
            hdr_fail(error, "unsupported pixel format");
            return false;
        }
        if (line.compare(0, 9, "EXPOSURE=") == 0) {
            const double e = std::atof(line.c_str() + 9);
            if (e > 0. && std::isfinite(e))
                exposure *= e;
        }
    }
    if (!read_line(line)) {
        hdr_fail(error, "truncated header");
        return false;
    }
    char   ys[4] = { 0 }, xs[4] = { 0 };
    long   h = 0, w = 0;
    if (std::sscanf(line.c_str(), "%3s %ld %3s %ld", ys, &h, xs, &w) != 4 || std::strcmp(xs, "+X") != 0 ||
        (std::strcmp(ys, "-Y") != 0 && std::strcmp(ys, "+Y") != 0)) {
        hdr_fail(error, "unsupported orientation");
        return false;
    }
    if (w <= 0 || h <= 0 || w > HDR_MAX_SIDE || h > HDR_MAX_SIDE || w * h > long(HDR_MAX_PIXELS)) {
        hdr_fail(error, "bad or too large resolution");
        return false;
    }
    const bool flip = std::strcmp(ys, "+Y") == 0;
    const size_t W = size_t(w), H = size_t(h);
    std::vector<uint8_t> rgbe(W * H * 4, 0);
    size_t y = 0;
    for (; y < H; ++y) {
        const bool rle = W >= 8 && W < 32768 && pos + 4 <= size && data[pos] == 2 && data[pos + 1] == 2 && data[pos + 2] < 128 &&
                         ((size_t(data[pos + 2]) << 8) | data[pos + 3]) == W;
        if (!rle)
            break;   // flat or old-style data from here on
        pos += 4;
        std::vector<uint8_t> row(W * 4);
        for (size_t ch = 0; ch < 4; ++ch) {
            size_t i = 0;
            while (i < W) {
                if (pos >= size) {
                    hdr_fail(error, "truncated pixel data");
                    return false;
                }
                const uint8_t code = data[pos++];
                if (code > 128) {
                    const size_t n = size_t(code) - 128;
                    if (pos >= size || i + n > W) {
                        hdr_fail(error, "bad RLE run");
                        return false;
                    }
                    const uint8_t v = data[pos++];
                    for (size_t k = 0; k < n; ++k)
                        row[ch * W + i + k] = v;
                    i += n;
                } else {
                    const size_t n = code;
                    if (n == 0 || i + n > W || pos + n > size) {
                        hdr_fail(error, "bad RLE dump");
                        return false;
                    }
                    std::memcpy(row.data() + ch * W + i, data + pos, n);
                    pos += n;
                    i += n;
                }
            }
        }
        for (size_t x = 0; x < W; ++x)
            for (size_t ch = 0; ch < 4; ++ch)
                rgbe[(y * W + x) * 4 + ch] = row[ch * W + x];
    }
    if (y < H) {
        // Flat pixels with optional old-style (1,1,1,n) repeat runs.
        size_t i = y * W, shift = 0;
        const size_t total = W * H;
        while (i < total) {
            if (pos + 4 > size) {
                hdr_fail(error, "truncated pixel data");
                return false;
            }
            const uint8_t *px = data + pos;
            pos += 4;
            if (px[0] == 1 && px[1] == 1 && px[2] == 1) {
                const size_t n = size_t(px[3]) << shift;
                if (i == 0 || i + n > total || shift > 16) {
                    hdr_fail(error, "bad old-style run");
                    return false;
                }
                for (size_t k = 0; k < n; ++k)
                    std::memcpy(&rgbe[(i + k) * 4], &rgbe[(i - 1) * 4], 4);
                i += n;
                shift += 8;
            } else {
                std::memcpy(&rgbe[i * 4], px, 4);
                ++i;
                shift = 0;
            }
        }
    }
    out.width  = int(W);
    out.height = int(H);
    out.rgb.resize(W * H * 3);
    for (size_t row = 0; row < H; ++row) {
        const size_t dst_row = flip ? H - 1 - row : row;
        for (size_t x = 0; x < W; ++x) {
            const uint8_t *p = &rgbe[(row * W + x) * 4];
            float          f = p[3] > 0 ? float(std::ldexp(1.0, int(p[3]) - 136) / exposure) : 0.f;
            float         *o = &out.rgb[(dst_row * W + x) * 3];
            o[0] = float(p[0]) * f;
            o[1] = float(p[1]) * f;
            o[2] = float(p[2]) * f;
        }
    }
    return true;
}

bool load_hdr_file(const std::string &path, HdrImage &out, std::string *error)
{
    out = HdrImage();
    boost::nowide::ifstream f(path, std::ios::binary);
    if (!f) {
        hdr_fail(error, "cannot open the file");
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return decode_rgbe(bytes.data(), bytes.size(), out, error);
}

std::string encode_rgbe(const HdrImage &image, bool rle)
{
    std::string out = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(image.height) + " +X " + std::to_string(image.width) + "\n";
    if (image.empty())
        return out;
    const size_t W = size_t(image.width);
    const bool   use_rle = rle && W >= 8 && W < 32768;
    std::vector<uint8_t> row(W * 4);
    for (int y = 0; y < image.height; ++y) {
        for (size_t x = 0; x < W; ++x) {
            const float *p = &image.rgb[(size_t(y) * W + x) * 3];
            const float  m = std::max(p[0], std::max(p[1], p[2]));
            uint8_t      px[4] = { 0, 0, 0, 0 };
            if (m > 1e-32f && std::isfinite(m)) {
                int          e;
                const double f = std::frexp(double(m), &e) * 256.0 / double(m);
                px[0] = uint8_t(std::clamp(int(double(p[0]) * f), 0, 255));
                px[1] = uint8_t(std::clamp(int(double(p[1]) * f), 0, 255));
                px[2] = uint8_t(std::clamp(int(double(p[2]) * f), 0, 255));
                px[3] = uint8_t(std::clamp(e + 128, 0, 255));
            }
            std::memcpy(&row[x * 4], px, 4);
        }
        if (!use_rle) {
            out.append(reinterpret_cast<const char *>(row.data()), row.size());
            continue;
        }
        out.push_back(char(2));
        out.push_back(char(2));
        out.push_back(char(W >> 8));
        out.push_back(char(W & 255));
        for (size_t ch = 0; ch < 4; ++ch) {
            size_t i = 0;
            while (i < W) {
                // A run of four or more equal bytes, else a dump of up to 128 bytes that stops before the next run.
                size_t run = 1;
                while (i + run < W && run < 127 && row[(i + run) * 4 + ch] == row[i * 4 + ch])
                    ++run;
                if (run >= 4) {
                    out.push_back(char(128 + run));
                    out.push_back(char(row[i * 4 + ch]));
                    i += run;
                    continue;
                }
                size_t n = 0;
                while (i + n < W && n < 128) {
                    size_t r = 1;
                    while (i + n + r < W && r < 4 && row[(i + n + r) * 4 + ch] == row[(i + n) * 4 + ch])
                        ++r;
                    if (r >= 4)
                        break;
                    ++n;
                }
                if (n == 0)
                    n = 1;
                out.push_back(char(n));
                for (size_t k = 0; k < n; ++k)
                    out.push_back(char(row[(i + k) * 4 + ch]));
                i += n;
            }
        }
    }
    return out;
}

// =================================================================================================
// Equirect mapping
// =================================================================================================

std::array<float, 3> equirect_direction(float u, float v)
{
    const double phi = (0.5 - double(u)) * 2. * double(HDR_PI);
    const double theta = (0.5 - double(v)) * double(HDR_PI);
    return { float(std::cos(theta) * std::cos(phi)), float(std::cos(theta) * std::sin(phi)), float(std::sin(theta)) };
}

void direction_to_equirect(const std::array<float, 3> &dir, float &u, float &v)
{
    const hdr_vec3 d = hdr_normalized(dir);
    const double phi = std::atan2(double(d[1]), double(d[0]));
    const double theta = std::asin(std::clamp(double(d[2]), -1., 1.));
    u = float(0.5 - phi / (2. * double(HDR_PI)));
    v = float(0.5 - theta / double(HDR_PI));
}

// =================================================================================================
// Procedural panoramas
// =================================================================================================

const char *to_string(ProceduralHdr v)
{
    switch (v) {
    case ProceduralHdr::Studio: return "studio";
    case ProceduralHdr::Interior: return "interior";
    default:
    case ProceduralHdr::SkySun: return "sky_sun";
    }
}

bool from_string(const std::string &s, ProceduralHdr &v)
{
    if (s == "sky_sun") { v = ProceduralHdr::SkySun; return true; }
    if (s == "studio") { v = ProceduralHdr::Studio; return true; }
    if (s == "interior") { v = ProceduralHdr::Interior; return true; }
    return false;
}

namespace {

constexpr float HDR_DEG = HDR_PI / 180.f;

// The angular offset of a direction from (az0, el0) per axis, degrees.
void hdr_angles(const hdr_vec3 &d, float &az, float &el)
{
    az = std::atan2(d[1], d[0]) / HDR_DEG;
    el = std::asin(std::clamp(d[2], -1.f, 1.f)) / HDR_DEG;
}
float hdr_wrap_deg(float a) { return std::fmod(std::fmod(a + 180.f, 360.f) + 360.f, 360.f) - 180.f; }

// 1 inside the rectangle (half sizes in degrees), soft over its outer 15 %.
float hdr_panel(float az, float el, float az0, float el0, float half_az, float half_el)
{
    const float da = std::abs(hdr_wrap_deg(az - az0)) / half_az, de = std::abs(el - el0) / half_el;
    return (1.f - hdr_smoothstep(0.85f, 1.f, da)) * (1.f - hdr_smoothstep(0.85f, 1.f, de));
}

hdr_vec3 hdr_mixv(const hdr_vec3 &a, const hdr_vec3 &b, float t) { return { a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t }; }
hdr_vec3 hdr_scaled(const hdr_vec3 &a, float s) { return { a[0] * s, a[1] * s, a[2] * s }; }
void     hdr_add(hdr_vec3 &a, const hdr_vec3 &b) { a[0] += b[0]; a[1] += b[1]; a[2] += b[2]; }

hdr_vec3 hdr_sky_sun(const hdr_vec3 &d)
{
    static const hdr_vec3 sun_dir = []() {
        const float az = 40.f * HDR_DEG, el = 35.f * HDR_DEG;
        return hdr_vec3{ std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el) };
    }();
    hdr_vec3 c;
    if (d[2] >= 0.f) {
        const float t = std::sqrt(d[2]);
        c = hdr_mixv({ 0.80f, 0.88f, 1.00f }, { 0.20f, 0.42f, 0.90f }, t);
    } else {
        const float shade = 0.55f + 0.45f * (1.f + d[2]);
        c = hdr_scaled({ 0.24f, 0.21f, 0.18f }, shade);
    }
    const float horizon = hdr_smoothstep(-0.04f, 0.04f, d[2]);
    if (d[2] < 0.04f && d[2] >= -0.04f)
        c = hdr_mixv(hdr_scaled({ 0.24f, 0.21f, 0.18f }, 1.f), c, horizon);
    const float ang = std::acos(std::clamp(hdr_dot(d, sun_dir), -1.f, 1.f));
    hdr_add(c, hdr_scaled({ 1.f, 0.85f, 0.62f }, 15.f * std::exp(-ang / 0.05f)));
    hdr_add(c, hdr_scaled({ 1.f, 0.93f, 0.82f }, 40000.f * (1.f - hdr_smoothstep(0.010f, 0.014f, ang))));
    return c;
}

hdr_vec3 hdr_studio(const hdr_vec3 &d)
{
    float az, el;
    hdr_angles(d, az, el);
    hdr_vec3 c = d[2] < -0.1f ? hdr_vec3{ 0.030f, 0.030f, 0.032f } : hdr_mixv({ 0.045f, 0.046f, 0.05f }, { 0.09f, 0.092f, 0.10f }, std::clamp(d[2] + 0.1f, 0.f, 1.f));
    hdr_add(c, hdr_scaled({ 1.00f, 0.95f, 0.88f }, 90.f * hdr_panel(az, el, 120.f, 40.f, 18.f, 12.f)));    // key softbox
    hdr_add(c, hdr_scaled({ 0.85f, 0.92f, 1.00f }, 10.f * hdr_panel(az, el, -110.f, 15.f, 25.f, 15.f)));   // fill softbox
    hdr_add(c, hdr_scaled({ 1.00f, 1.00f, 1.00f }, 10.f * hdr_panel(az, el, 0.f, 76.f, 70.f, 7.f)));       // top strip
    return c;
}

hdr_vec3 hdr_interior(const hdr_vec3 &d)
{
    float az, el;
    hdr_angles(d, az, el);
    hdr_vec3 c;
    if (d[2] < -0.05f)
        c = hdr_scaled({ 0.12f, 0.08f, 0.05f }, 0.7f + 0.3f * (1.f + d[2]));
    else if (d[2] > 0.55f)
        c = { 0.40f, 0.38f, 0.34f };
    else
        c = hdr_scaled({ 0.30f, 0.24f, 0.18f }, 0.75f + 0.25f * std::cos((az - 20.f) * HDR_DEG));
    // The window: daylight in a frame with a cross of dark bars.
    const float win = hdr_panel(az, el, 200.f, 30.f, 22.f, 26.f);
    const float bar = std::max(1.f - hdr_smoothstep(0.8f, 1.6f, std::abs(hdr_wrap_deg(az - 200.f))), 1.f - hdr_smoothstep(0.8f, 1.6f, std::abs(el - 30.f)));
    hdr_add(c, hdr_scaled({ 0.85f, 0.92f, 1.00f }, 45.f * win * (1.f - 0.92f * bar)));
    // A warm ceiling lamp.
    const float lamp = 1.f - hdr_smoothstep(5.f, 7.f, std::sqrt(std::pow(hdr_wrap_deg(az - 20.f) * std::cos(el * HDR_DEG), 2.f) + std::pow(el - 78.f, 2.f)));
    hdr_add(c, hdr_scaled({ 1.00f, 0.70f, 0.40f }, 35.f * lamp));
    return c;
}

} // namespace

HdrImage make_procedural_hdr(ProceduralHdr kind, int width)
{
    HdrImage img;
    width = std::clamp(width, 16, 8192);
    width -= width % 2;
    img.width  = width;
    img.height = width / 2;
    img.rgb.resize(size_t(img.width) * size_t(img.height) * 3);
    const HdrGrid grid(img.width, img.height);
    tbb::parallel_for(tbb::blocked_range<int>(0, img.height, 8), [&](const tbb::blocked_range<int> &range) {
        for (int y = range.begin(); y != range.end(); ++y)
            for (int x = 0; x < img.width; ++x) {
                const hdr_vec3 d = grid.dir(x, y);
                hdr_vec3       c;
                switch (kind) {
                case ProceduralHdr::Studio: c = hdr_studio(d); break;
                case ProceduralHdr::Interior: c = hdr_interior(d); break;
                default: c = hdr_sky_sun(d); break;
                }
                float *o = &img.rgb[(size_t(y) * size_t(img.width) + size_t(x)) * 3];
                o[0] = c[0];
                o[1] = c[1];
                o[2] = c[2];
            }
    });
    return img;
}

// =================================================================================================
// SH, sun
// =================================================================================================

std::array<std::array<float, 3>, 9> project_hdr_sh(const HdrImage &image)
{
    std::array<std::array<double, 3>, 9> acc{};
    if (!image.empty()) {
        const HdrGrid grid(image.width, image.height);
        for (int y = 0; y < image.height; ++y) {
            const double w = double(grid.omega[size_t(y)]);
            for (int x = 0; x < image.width; ++x) {
                const hdr_vec3 d = grid.dir(x, y);
                const std::array<float, 9> basis = hdr_sh_basis(d[0], d[1], d[2]);
                const float *p = &image.rgb[(size_t(y) * size_t(image.width) + size_t(x)) * 3];
                for (size_t k = 0; k < 9; ++k) {
                    const double b = double(basis[k]) * w;
                    acc[k][0] += b * double(p[0]);
                    acc[k][1] += b * double(p[1]);
                    acc[k][2] += b * double(p[2]);
                }
            }
        }
    }
    // c_i = L_i A_l / pi with A_0 = pi, A_1 = 2 pi / 3, A_2 = pi / 4.
    static const double band[9] = { 1., 2. / 3., 2. / 3., 2. / 3., 0.25, 0.25, 0.25, 0.25, 0.25 };
    std::array<std::array<float, 3>, 9> out{};
    for (size_t k = 0; k < 9; ++k)
        for (size_t c = 0; c < 3; ++c)
            out[k][c] = float(acc[k][c] * band[k]);
    return out;
}

float sh_peak_luminance(const std::array<std::array<float, 3>, 9> &sh)
{
    constexpr int N = 4000;
    float peak = 0.f;
    for (int k = 0; k < N; ++k) {
        const float z   = 1.f - 2.f * (float(k) + 0.5f) / float(N);
        const float r   = std::sqrt(std::max(0.f, 1.f - z * z));
        const float phi = (float(k) + 0.5f) * HDR_PI * (3.f - std::sqrt(5.f));
        const std::array<float, 9> b = hdr_sh_basis(r * std::cos(phi), r * std::sin(phi), z);
        float rgb[3] = { 0.f, 0.f, 0.f };
        for (size_t i = 0; i < 9; ++i)
            for (size_t c = 0; c < 3; ++c)
                rgb[c] += sh[i][c] * b[i];
        peak = std::max(peak, hdr_luma(rgb[0], rgb[1], rgb[2]));
    }
    return peak;
}

namespace {

// The dominant light of a panorama: the brightest pixel and the connected pixels above 10 % of it
// (and well above the average), within 18 degrees. It is a light only when it delivers a quarter of
// the cosine-weighted light that arrives from its direction (the bake script's rule for the baked
// environments' sun_dir). `mask` (if given) marks the pixels of the light.
HdrSun hdr_find_sun(const HdrImage &img, std::vector<uint8_t> *mask)
{
    HdrSun sun;
    if (mask != nullptr)
        mask->assign(size_t(std::max(0, img.width)) * size_t(std::max(0, img.height)), 0);
    if (img.empty())
        return sun;
    const HdrGrid grid(img.width, img.height);
    const size_t  count = size_t(img.width) * size_t(img.height);
    std::vector<float> lum(count);
    double mean = 0., total_omega = 0.;
    size_t peak_index = 0;
    float  peak = -1.f;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            const size_t i = size_t(y) * size_t(img.width) + size_t(x);
            const float *p = &img.rgb[i * 3];
            const float  l = hdr_luma(p[0], p[1], p[2]);
            lum[i] = std::isfinite(l) ? std::max(l, 0.f) : 0.f;
            mean += double(lum[i]) * double(grid.omega[size_t(y)]);
            total_omega += double(grid.omega[size_t(y)]);
            if (lum[i] > peak) {
                peak = lum[i];
                peak_index = i;
            }
        }
    mean /= std::max(total_omega, 1e-12);
    if (!(peak > 0.f))
        return sun;
    const float thr = std::max(0.1f * peak, 8.f * float(mean));
    if (peak < thr)
        return sun;   // a flat panorama

    const hdr_vec3 peak_dir = grid.dir(int(peak_index % size_t(img.width)), int(peak_index / size_t(img.width)));
    const float    cos_cap  = std::cos(18.f * HDR_DEG);
    std::vector<uint8_t> in(count, 0);
    std::deque<size_t>   queue;
    queue.push_back(peak_index);
    in[peak_index] = 1;
    double e_rgb[3] = { 0., 0., 0. }, e_dir[3] = { 0., 0., 0. }, solid = 0.;
    while (!queue.empty()) {
        const size_t i = queue.front();
        queue.pop_front();
        const int x = int(i % size_t(img.width)), y = int(i / size_t(img.width));
        const double w = double(grid.omega[size_t(y)]);
        const hdr_vec3 d = grid.dir(x, y);
        for (int c = 0; c < 3; ++c)
            e_rgb[c] += double(img.rgb[i * 3 + size_t(c)]) * w;
        for (int c = 0; c < 3; ++c)
            e_dir[c] += double(lum[i]) * w * double(d[size_t(c)]);
        solid += w;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0)
                    continue;
                const int ny = y + dy;
                if (ny < 0 || ny >= img.height)
                    continue;
                const int nx = (x + dx + img.width) % img.width;
                const size_t n = size_t(ny) * size_t(img.width) + size_t(nx);
                if (in[n] || lum[n] < thr || hdr_dot(grid.dir(nx, ny), peak_dir) < cos_cap)
                    continue;
                in[n] = 1;
                queue.push_back(n);
            }
    }
    const hdr_vec3 dir = hdr_normalized({ float(e_dir[0]), float(e_dir[1]), float(e_dir[2]) });
    // The light the region delivers to a surface facing it, and the whole panorama's.
    double e_region = 0., e_all = 0.;
    for (int y = 0; y < img.height; ++y) {
        const double w = double(grid.omega[size_t(y)]);
        for (int x = 0; x < img.width; ++x) {
            const size_t i = size_t(y) * size_t(img.width) + size_t(x);
            const float  c = std::max(0.f, hdr_dot(grid.dir(x, y), dir));
            if (c <= 0.f)
                continue;
            const double e = double(lum[i]) * w * double(c);
            e_all += e;
            if (in[i])
                e_region += e;
        }
    }
    sun.dir         = dir;
    sun.solid_angle = float(solid);
    const float rgb_lum = hdr_luma(float(e_rgb[0]), float(e_rgb[1]), float(e_rgb[2]));
    if (rgb_lum > 0.f)
        sun.color = { float(e_rgb[0]) / rgb_lum, float(e_rgb[1]) / rgb_lum, float(e_rgb[2]) / rgb_lum };
    sun.irradiance = float(e_region);   // not scaled yet
    sun.share      = e_all > 0. ? float(e_region / e_all) : 0.f;
    sun.found      = sun.share >= 0.25f;
    if (sun.found && mask != nullptr)
        *mask = std::move(in);
    return sun;
}

// Takes the light out of the panorama: the region and a 2 pixel margin are clamped to the average of
// the ring around them, so the sun's glow does not stay behind as a bright patch.
void hdr_remove_region(HdrImage &img, const std::vector<uint8_t> &mask)
{
    if (img.empty() || mask.size() != size_t(img.width) * size_t(img.height))
        return;
    const HdrGrid grid(img.width, img.height);
    auto dilate = [&](const std::vector<uint8_t> &src) {
        std::vector<uint8_t> out(src);
        for (int y = 0; y < img.height; ++y)
            for (int x = 0; x < img.width; ++x) {
                if (!src[size_t(y) * size_t(img.width) + size_t(x)])
                    continue;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int ny = y + dy;
                        if (ny < 0 || ny >= img.height)
                            continue;
                        out[size_t(ny) * size_t(img.width) + size_t((x + dx + img.width) % img.width)] = 1;
                    }
            }
        return out;
    };
    const std::vector<uint8_t> inner = dilate(dilate(mask));
    const std::vector<uint8_t> outer = dilate(inner);
    double ring[3] = { 0., 0., 0. }, ring_w = 0.;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            const size_t i = size_t(y) * size_t(img.width) + size_t(x);
            if (outer[i] && !inner[i]) {
                const double w = double(grid.omega[size_t(y)]);
                for (int c = 0; c < 3; ++c)
                    ring[c] += double(img.rgb[i * 3 + size_t(c)]) * w;
                ring_w += w;
            }
        }
    if (!(ring_w > 0.))
        return;
    for (int c = 0; c < 3; ++c)
        ring[c] /= ring_w;
    for (size_t i = 0; i < inner.size(); ++i)
        if (inner[i])
            for (size_t c = 0; c < 3; ++c)
                img.rgb[i * 3 + c] = std::min(img.rgb[i * 3 + c], float(ring[c]));
}

} // namespace

HdrSun find_hdr_sun(const HdrImage &image) { return hdr_find_sun(image, nullptr); }

// =================================================================================================
// The bake
// =================================================================================================

bool bake_hdr_environment(const HdrImage &image, const HdrBakeOptions &options, HdrEnvironment &out)
{
    out = HdrEnvironment();
    if (image.empty() || options.face_size < 8 || options.levels < 1 || options.levels > 8 || (options.face_size >> (options.levels - 1)) < 2)
        return false;
    for (float v : image.rgb)
        if (!std::isfinite(v))
            return false;

    std::vector<uint8_t> mask;
    HdrSun sun = hdr_find_sun(image, &mask);
    // One scale for the whole panorama, from its full light (the sun included), so that choosing to
    // take the sun out does not change the sky's brightness.
    const std::array<std::array<float, 3>, 9> sh_full = project_hdr_sh(image);
    const float peak = sh_peak_luminance(sh_full);
    if (!(peak > 0.f) || !std::isfinite(peak))
        return false;
    const float k = options.target_peak / peak;

    HdrImage work;
    work.width  = image.width;
    work.height = image.height;
    work.rgb.resize(image.rgb.size());
    for (size_t i = 0; i < image.rgb.size(); ++i)
        work.rgb[i] = std::max(0.f, image.rgb[i]) * k;
    const bool removed = options.remove_sun && sun.found;
    if (removed)
        hdr_remove_region(work, mask);

    out.scale = k;
    sun.irradiance *= k;
    sun.key_strength = std::clamp(sun.irradiance / HDR_PI, 0.5f, 1.5f);
    out.sun = sun;
    if (removed) {
        out.sh = project_hdr_sh(work);
    } else {
        out.sh = sh_full;
        for (auto &row : out.sh)
            for (float &c : row)
                c *= k;
    }

    // The cube chain and the prefiltered levels. Level 1 reads the 64 px cube, level 2 the 64 px one
    // too and levels 3 and up the 32 px one (the bake script reads the 128 px cube for level 1; the
    // quadrature over 24k source texels instead of 98k keeps the load under a second).
    const int N = options.face_size, L = options.levels;
    std::vector<HdrCube> levels;
    levels.push_back(hdr_equirect_to_cube(work, N));
    const HdrCube c64 = N >= 128 ? hdr_cube_downsample(levels[0]) : levels[0];
    const HdrCube c32 = c64.n >= 64 ? hdr_cube_downsample(c64) : c64;
    for (int l = 1; l < L; ++l) {
        const HdrCube &src = l <= 2 ? c64 : c32;
        levels.push_back(hdr_ggx_prefilter(src, std::max(1, N >> l), float(l) / float(std::max(1, L - 1))));
    }

    out.face_size    = N;
    out.levels       = L;
    out.atlas_width  = env_atlas_width(N);
    out.atlas_height = env_atlas_height(N, L);
    out.atlas_rgba.assign(size_t(out.atlas_width) * size_t(out.atlas_height) * 4, 0);
    for (int l = 0; l < L; ++l)
        for (int f = 0; f < 6; ++f) {
            const RectI r = env_atlas_face(N, L, l, f);
            const HdrCube &cube = levels[size_t(l)];
            for (int j = 0; j < r.h; ++j)
                for (int i = 0; i < r.w; ++i)
                    hdr_rgbm_encode(cube.at(f, j, i), &out.atlas_rgba[(size_t(r.y + j) * size_t(out.atlas_width) + size_t(r.x + i)) * 4]);
        }
    return true;
}

// =================================================================================================
// Manifest
// =================================================================================================

const HdriInfo *HdriManifest::find(const std::string &id) const
{
    for (const HdriInfo &h : hdris)
        if (h.id == id)
            return &h;
    return nullptr;
}

bool parse_hdri_manifest(const std::string &text, HdriManifest &out, std::string *error)
{
    out = HdriManifest();
    const hdr_json j = hdr_json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("hdris") || !j["hdris"].is_array()) {
        hdr_fail(error, "hdri.json is not a manifest");
        return false;
    }
    auto str = [](const hdr_json &e, const char *key) {
        auto it = e.find(key);
        return (it != e.end() && it->is_string()) ? it->get<std::string>() : std::string();
    };
    HdriManifest m;
    for (const hdr_json &e : j["hdris"]) {
        if (!e.is_object())
            continue;
        HdriInfo info;
        info.id   = str(e, "id");
        info.name = str(e, "name");
        if (info.name.empty())
            info.name = info.id;
        if (info.id.empty() || m.find(info.id) != nullptr)
            continue;
        const std::string proc = str(e, "procedural");
        if (!proc.empty()) {
            if (!from_string(proc, info.kind))
                continue;
            info.procedural = true;
        } else {
            info.file = str(e, "file");
            // A plain file name next to the manifest: no folders, no "..".
            if (info.file.empty() || info.file.find('/') != std::string::npos || info.file.find('\\') != std::string::npos ||
                info.file.find("..") != std::string::npos || info.file.find(':') != std::string::npos)
                continue;
        }
        info.source  = str(e, "source");
        info.authors = str(e, "authors");
        info.licence = str(e, "licence");
        m.hdris.push_back(std::move(info));
    }
    out = std::move(m);
    return true;
}

} // namespace Showcase
} // namespace Slic3r
