// Airbrush P0 (tests/design_airbrush_paint.md, section 5): the calibration card.
//
// A scripted, reproducible project the owner prints on the CC1 (Elegoo Centauri Carbon 0.4) with
// white + one translucent filament, to choose the airbrush defaults (grain, mix-ladder step, layer
// height) before any product code is written. Everything it paints is ordinary `paint_color`
// MMU paint written through the Image Fill encoder (image_fill_subdivide / image_fill_encode) and
// TriangleSelector, and the mix ladder uses ordinary mixed filaments (MixedFilamentManager rows
// with a manual layer pattern). No new paint format.
//
// The card, per layer height (one plate each: 0.08 / 0.12 / 0.16 mm):
//   * a triangular prism 73 x 39 x 39 mm: its front is the VERTICAL WALL face, its back the
//     45 DEGREE face (it leans back, so it faces up and needs no support);
//   * a 73 x 39 x 3 mm slab in front of it: its top is the TOP face.
// Every face carries the same layout (u = across, v = up the face; on the top face v runs away
// from the front edge):
//
//   v 34.5..39   top margin:   "08"/"12"/"16" (layer height, hundredths of a mm) at the left,
//                              then the mix step 0..8 above each column
//   v 29.5..34.5 MIX row:      column k is k/8 translucent, as a mixed filament (layer pattern);
//                              k = 0 is plain white, k = 8 plain translucent
//   v  4.5..29.5 grain rows:   0.4 / 0.6 / 0.8 / 1.0 / 1.5 mm specks (bottom to top), translucent
//                              on white, the coverage of the column
//   v  0  ..4.5  bottom margin: the coverage of each column, 5 10 15 20 25 30 35 40 50 (%)
//   u  0  ..8    left margin:  row labels "0.4" .. "1.5", "MX"
//   u  8  ..71   nine 7 mm columns
//
// Labels are raised (0.6 mm) pixel-font parts in the base colour, so they read without paint.
// Specks are placed by an ordered blue-noise threshold (design D6): a 32 x 32 void-and-cluster
// mask, one mask cell per speck (g x g), a fixed per-cell tile offset. Same input, same card.
//
// Run (hidden: it writes a file and needs the shipped profiles):
//   set AIRBRUSH_P0_OUT=C:\Dev\SnapmakerOrca\tests\airbrush_p0_card.3mf
//   libslic3r_tests "[AirbrushP0Card]"
#include <catch2/catch.hpp>

#include "libslic3r/ImageFill.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// ---------------------------------------------------------------------------------------------
// Blue noise: a 32 x 32 void-and-cluster threshold mask (Ulichney 1993), toroidal, deterministic.
// Returns rank / N + 0.5 / N per cell, so "cell painted when t < coverage" paints round(coverage
// * N) cells of a full tile and spreads them as evenly as the method can.
// ---------------------------------------------------------------------------------------------
constexpr int BN = 32;

std::vector<float> blue_noise_mask()
{
    const int    N     = BN * BN;
    const double sigma = 1.9;
    // Toroidal Gaussian kernel by offset.
    std::vector<double> kern(N);
    for (int dy = 0; dy < BN; ++dy)
        for (int dx = 0; dx < BN; ++dx) {
            const int    ddx = std::min(dx, BN - dx), ddy = std::min(dy, BN - dy);
            kern[dy * BN + dx] = std::exp(-double(ddx * ddx + ddy * ddy) / (2. * sigma * sigma));
        }
    auto add_energy = [&](std::vector<double> &e, int idx, double sign) {
        const int x0 = idx % BN, y0 = idx / BN;
        for (int y = 0; y < BN; ++y)
            for (int x = 0; x < BN; ++x)
                e[y * BN + x] += sign * kern[((y - y0 + BN) % BN) * BN + ((x - x0 + BN) % BN)];
    };
    // Tightest cluster: the set cell (value == want) with the highest energy; largest void: the
    // unset cell with the lowest. Ties go to the lowest index, so the result never depends on luck.
    auto extreme = [&](const std::vector<char> &bits, const std::vector<double> &e, char want, bool highest) {
        int best = -1;
        for (int i = 0; i < N; ++i)
            if (bits[i] == want && (best < 0 || (highest ? e[i] > e[best] : e[i] < e[best])))
                best = i;
        return best;
    };

    // Initial pattern: ~10% ones from a fixed LCG, then relaxed until stable.
    std::vector<char> proto(N, 0);
    uint32_t          lcg = 0x2545F491u;
    int               ones = 0;
    while (ones < N / 10) {
        lcg = lcg * 1664525u + 1013904223u;
        const int i = int((lcg >> 8) % uint32_t(N));
        if (!proto[i]) { proto[i] = 1; ++ones; }
    }
    std::vector<double> e(N, 0.);
    for (int i = 0; i < N; ++i)
        if (proto[i]) add_energy(e, i, 1.);
    for (int iter = 0; iter < 4 * N; ++iter) {
        const int c = extreme(proto, e, 1, true);
        proto[c]    = 0;
        add_energy(e, c, -1.);
        const int v = extreme(proto, e, 0, false);
        proto[v]    = 1;
        add_energy(e, v, 1.);
        if (v == c) break;
    }

    std::vector<int> rank(N, -1);
    // Phase 1: rank the prototype's ones by removing tightest clusters.
    {
        std::vector<char>   bits = proto;
        std::vector<double> en   = e;
        for (int r = ones - 1; r >= 0; --r) {
            const int c = extreme(bits, en, 1, true);
            bits[c]     = 0;
            add_energy(en, c, -1.);
            rank[c] = r;
        }
    }
    // Phase 2 and 3: fill the voids up to all ones. Past half the tile the "largest void" of the
    // zeros is the same cell as the "tightest cluster" of the zeros taken as minority pixels, so
    // a single rule (lowest energy among the zeros) covers both phases on a symmetric kernel.
    {
        std::vector<char>   bits = proto;
        std::vector<double> en   = e;
        for (int r = ones; r < N; ++r) {
            const int v = extreme(bits, en, 0, false);
            bits[v]     = 1;
            add_energy(en, v, 1.);
            rank[v] = r;
        }
    }
    std::vector<float> t(N);
    for (int i = 0; i < N; ++i)
        t[i] = (float(rank[i]) + 0.5f) / float(N);
    return t;
}

const std::vector<float> &mask()
{
    static const std::vector<float> m = blue_noise_mask();
    return m;
}

// ---------------------------------------------------------------------------------------------
// The layout.
// ---------------------------------------------------------------------------------------------
constexpr double W  = 73.;    // face width (u)
constexpr double H  = 39.;    // face height (v); also the prism's height and depth
constexpr double ST = 3.;     // slab thickness
constexpr double LM = 8.;     // left margin
constexpr double CW = 7.;     // column width
constexpr double CH = 5.;     // row height
constexpr double BM = 4.5;    // bottom margin
constexpr int    NCOL = 9;
constexpr int    NGRAIN = 5;
constexpr int    PAINT_DEPTH = 3; // 1 mm grid -> 0.125 mm leaf legs

const double GRAINS[NGRAIN]  = { 0.4, 0.6, 0.8, 1.0, 1.5 };
const char  *GRAIN_LABEL[NGRAIN] = { "0.4", "0.6", "0.8", "1.0", "1.5" };
const double COVERAGE[NCOL]  = { 0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40, 0.50 };
const char  *COVER_LABEL[NCOL] = { "5", "10", "15", "20", "25", "30", "35", "40", "50" };

enum FaceId { FaceNone = -1, FaceWall = 0, FaceSlope = 1, FaceTop = 2 };

struct Frame
{
    Vec3d O, U, V, N;
};

Frame face_frame(FaceId f)
{
    const double s = 1. / std::sqrt(2.);
    switch (f) {
    case FaceWall:  return { Vec3d(0, 0, 0), Vec3d(1, 0, 0), Vec3d(0, 0, 1), Vec3d(0, -1, 0) };
    case FaceSlope: return { Vec3d(W, H, 0), Vec3d(-1, 0, 0), Vec3d(0, -s, s), Vec3d(0, s, s) };
    default:        return { Vec3d(0, 0, ST), Vec3d(1, 0, 0), Vec3d(0, 1, 0), Vec3d(0, 0, 1) };
    }
}

// Stable per-cell tile offset, so neighbouring cells do not show the same speck pattern.
uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = 2166136261u;
    for (uint32_t v : { a, b, c }) {
        h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
        h *= 16777619u;
    }
    return h;
}

// The paint state at face point (u, v): 0 = unpainted (the object's white), 2 = translucent,
// 3..9 = mixed filament k/8 (k = 1..7).
int state_at(FaceId face, double u, double v)
{
    if (u < LM || v < BM)
        return 0;
    const int col = int(std::floor((u - LM) / CW));
    const int row = int(std::floor((v - BM) / CH));
    if (col < 0 || col >= NCOL || row < 0 || row > NGRAIN)
        return 0;
    if (row == NGRAIN) {
        // Mix ladder: k/8 of the translucent filament.
        const int k = col;
        return k == 0 ? 0 : k == 8 ? 2 : 2 + k;
    }
    const double g  = GRAINS[row];
    const double lu = u - (LM + col * CW), lv = v - (BM + row * CH);
    const int    i  = int(std::floor(lu / g)), j = int(std::floor(lv / g));
    const uint32_t h  = hash3(uint32_t(face), uint32_t(row), uint32_t(col));
    const int      ox = int(h & 31u), oy = int((h >> 5) & 31u);
    const float    t  = mask()[((j + oy) & (BN - 1)) * BN + ((i + ox) & (BN - 1))];
    return t < float(COVERAGE[col]) ? 2 : 0;
}

// ---------------------------------------------------------------------------------------------
// Meshes: gridded faces with shared, de-duplicated vertices (so the solids are closed), each
// triangle tagged with the face it paints.
// ---------------------------------------------------------------------------------------------
struct MeshBuilder
{
    indexed_triangle_set              its;
    std::vector<FaceId>               tag;
    std::map<std::array<long long, 3>, int> vmap;

    int vert(const Vec3d &p)
    {
        const std::array<long long, 3> k = { std::llround(p.x() * 1e4), std::llround(p.y() * 1e4), std::llround(p.z() * 1e4) };
        auto it = vmap.find(k);
        if (it != vmap.end())
            return it->second;
        its.vertices.emplace_back(p.cast<float>());
        const int idx = int(its.vertices.size()) - 1;
        vmap.emplace(k, idx);
        return idx;
    }
    void tri(int a, int b, int c, const Vec3d &outward, FaceId f)
    {
        const Vec3d pa = its.vertices[a].cast<double>(), pb = its.vertices[b].cast<double>(), pc = its.vertices[c].cast<double>();
        if ((pb - pa).cross(pc - pa).dot(outward) < 0.)
            std::swap(b, c);
        its.indices.emplace_back(a, b, c);
        tag.push_back(f);
    }
    void grid(const Vec3d &O, const Vec3d &U, const Vec3d &V, int nu, int nv, const Vec3d &outward, FaceId f)
    {
        for (int j = 0; j < nv; ++j)
            for (int i = 0; i < nu; ++i) {
                const int a = vert(O + U * (double(i) / nu) + V * (double(j) / nv));
                const int b = vert(O + U * (double(i + 1) / nu) + V * (double(j) / nv));
                const int c = vert(O + U * (double(i + 1) / nu) + V * (double(j + 1) / nv));
                const int d = vert(O + U * (double(i) / nu) + V * (double(j + 1) / nv));
                tri(a, b, c, outward, f);
                tri(a, c, d, outward, f);
            }
    }
    // A convex polygon given by its boundary points, fanned from its centroid.
    void fan(const std::vector<Vec3d> &boundary, const Vec3d &outward)
    {
        Vec3d c = Vec3d::Zero();
        for (const Vec3d &p : boundary) c += p;
        c /= double(boundary.size());
        const int ci = vert(c);
        for (size_t k = 0; k < boundary.size(); ++k)
            tri(ci, vert(boundary[k]), vert(boundary[(k + 1) % boundary.size()]), outward, FaceNone);
    }
};

// The prism: wall at y = 0 (x 0..W, z 0..H), 45 degree face from (y = H, z = 0) to (y = 0, z = H).
MeshBuilder make_prism()
{
    MeshBuilder m;
    const int nx = int(W), nh = int(H);
    m.grid(Vec3d(0, 0, 0), Vec3d(W, 0, 0), Vec3d(0, 0, H), nx, nh, Vec3d(0, -1, 0), FaceWall);
    m.grid(Vec3d(0, H, 0), Vec3d(W, 0, 0), Vec3d(0, -H, H), nx, nh, Vec3d(0, 1, 1), FaceSlope);
    m.grid(Vec3d(0, 0, 0), Vec3d(W, 0, 0), Vec3d(0, H, 0), nx, nh, Vec3d(0, 0, -1), FaceNone);
    for (double x : { 0., W }) {
        std::vector<Vec3d> b;
        for (int k = 0; k <= nh; ++k) b.emplace_back(x, 0, k);          // up the wall edge
        for (int k = 1; k <= nh; ++k) b.emplace_back(x, k, H - k);      // down the 45 degree edge
        for (int k = nh - 1; k >= 1; --k) b.emplace_back(x, k, 0);      // back along the bottom
        m.fan(b, Vec3d(x == 0. ? -1 : 1, 0, 0));
    }
    return m;
}

MeshBuilder make_slab()
{
    MeshBuilder m;
    const int nx = int(W), ny = int(H), nz = int(ST);
    m.grid(Vec3d(0, 0, ST), Vec3d(W, 0, 0), Vec3d(0, H, 0), nx, ny, Vec3d(0, 0, 1), FaceTop);
    m.grid(Vec3d(0, 0, 0), Vec3d(W, 0, 0), Vec3d(0, H, 0), nx, ny, Vec3d(0, 0, -1), FaceNone);
    m.grid(Vec3d(0, 0, 0), Vec3d(W, 0, 0), Vec3d(0, 0, ST), nx, nz, Vec3d(0, -1, 0), FaceNone);
    m.grid(Vec3d(0, H, 0), Vec3d(W, 0, 0), Vec3d(0, 0, ST), nx, nz, Vec3d(0, 1, 0), FaceNone);
    m.grid(Vec3d(0, 0, 0), Vec3d(0, H, 0), Vec3d(0, 0, ST), ny, nz, Vec3d(-1, 0, 0), FaceNone);
    m.grid(Vec3d(W, 0, 0), Vec3d(0, H, 0), Vec3d(0, 0, ST), ny, nz, Vec3d(1, 0, 0), FaceNone);
    return m;
}

// Paint every tagged triangle through the Image Fill encoder.
struct PaintStats
{
    size_t leaves_total   = 0;
    size_t leaves_painted = 0;
    std::map<int, size_t> per_state;
    // [face][row][col] -> (painted speck leaves, leaves in the cell)
    std::map<std::array<int, 3>, std::pair<size_t, size_t>> cells;
};

TriangleSelector::TriangleSplittingData paint(const MeshBuilder &m, PaintStats &stats)
{
    const size_t n   = m.its.indices.size();
    const size_t per = size_t(1) << (2 * PAINT_DEPTH);
    std::vector<int>           states(n * per, 0);
    std::vector<ImageFillLeaf> leaves;
    for (size_t t = 0; t < n; ++t) {
        if (m.tag[t] == FaceNone)
            continue;
        const Frame  fr = face_frame(m.tag[t]);
        const auto  &f  = m.its.indices[t];
        leaves.clear();
        image_fill_subdivide(m.its.vertices[f[0]], m.its.vertices[f[1]], m.its.vertices[f[2]], PAINT_DEPTH, leaves);
        for (size_t l = 0; l < per; ++l) {
            const Vec3d  c = leaves[l].centroid().cast<double>() - fr.O;
            const double u = c.dot(fr.U), v = c.dot(fr.V);
            const int    s = state_at(m.tag[t], u, v);
            states[t * per + l] = s;
            ++stats.per_state[s];
            if (u >= LM && v >= BM) {
                const int col = int(std::floor((u - LM) / CW)), row = int(std::floor((v - BM) / CH));
                if (col >= 0 && col < NCOL && row >= 0 && row < NGRAIN) {
                    auto &cell = stats.cells[{ int(m.tag[t]), row, col }];
                    ++cell.second;
                    if (s != 0) ++cell.first;
                }
            }
        }
    }
    TriangleSelector::TriangleSplittingData data = image_fill_encode(n, PAINT_DEPTH, states);
    for (size_t t = 0; t < n; ++t) {
        bool any = false;
        for (size_t l = 0; l < per; ++l) any |= states[t * per + l] != 0;
        if (any) {
            stats.leaves_total += per;
            for (size_t l = 0; l < per; ++l)
                stats.leaves_painted += states[t * per + l] != 0;
        }
    }
    return data;
}

// ---------------------------------------------------------------------------------------------
// Raised pixel-font labels.
// ---------------------------------------------------------------------------------------------
constexpr double PX     = 0.6;   // pixel size
constexpr double RELIEF = 0.6;   // how far a label stands off the face
constexpr double EMBED  = 0.3;   // and how far it reaches into the body

const std::vector<std::string> &glyph(char ch)
{
    static const std::map<char, std::vector<std::string>> g = {
        { '0', { "111", "101", "101", "101", "111" } }, { '1', { "010", "110", "010", "010", "111" } },
        { '2', { "111", "001", "111", "100", "111" } }, { '3', { "111", "001", "111", "001", "111" } },
        { '4', { "101", "101", "111", "001", "001" } }, { '5', { "111", "100", "111", "001", "111" } },
        { '6', { "111", "100", "111", "101", "111" } }, { '7', { "111", "001", "010", "010", "010" } },
        { '8', { "111", "101", "111", "101", "111" } }, { '9', { "111", "101", "111", "001", "111" } },
        { '.', { "0", "0", "0", "0", "1" } },
        { 'M', { "10001", "11011", "10101", "10001", "10001" } },
        { 'X', { "101", "101", "010", "101", "101" } },
    };
    return g.at(ch);
}

double text_width(const std::string &s)
{
    int cols = 0;
    for (size_t i = 0; i < s.size(); ++i)
        cols += int(glyph(s[i]).front().size()) + (i + 1 < s.size() ? 1 : 0);
    return cols * PX;
}

// One box in face coordinates: u0..u1, v0..v1, from -EMBED to RELIEF along the face normal.
void add_box(indexed_triangle_set &out, const Frame &fr, double u0, double u1, double v0, double v1)
{
    indexed_triangle_set box = its_make_cube(1., 1., 1.);   // [0,1]^3, outward-oriented
    for (stl_vertex &p : box.vertices) {
        const Vec3d q = fr.O + fr.U * (u0 + p.x() * (u1 - u0)) + fr.V * (v0 + p.y() * (v1 - v0)) +
                        fr.N * (-EMBED + p.z() * (RELIEF + EMBED));
        p = q.cast<float>();
    }
    // (U, V, N) is right-handed on every face, so the map keeps the cube's winding outward.
    its_merge(out, box);
}

// Text whose bottom-left corner is at (u0, v0); one box per horizontal run of lit pixels.
void add_text(indexed_triangle_set &out, const Frame &fr, const std::string &s, double u0, double v0)
{
    double x = u0;
    for (char ch : s) {
        const auto &gl = glyph(ch);
        for (int r = 0; r < 5; ++r) {
            const std::string &row = gl[r];
            const double       vb  = v0 + (4 - r) * PX;
            for (size_t c = 0; c < row.size();) {
                if (row[c] != '1') { ++c; continue; }
                size_t e = c;
                while (e < row.size() && row[e] == '1') ++e;
                add_box(out, fr, x + c * PX, x + e * PX, vb, vb + PX);
                c = e;
            }
        }
        x += (gl.front().size() + 1) * PX;
    }
}

indexed_triangle_set make_labels(FaceId face, const std::string &id)
{
    const Frame          fr = face_frame(face);
    indexed_triangle_set out;
    const double         glyph_h = 5 * PX;
    // Row labels, right-aligned in the left margin.
    for (int row = 0; row <= NGRAIN; ++row) {
        const std::string s = row < NGRAIN ? GRAIN_LABEL[row] : "MX";
        add_text(out, fr, s, LM - 0.5 - text_width(s), BM + row * CH + 0.5 * (CH - glyph_h));
    }
    for (int col = 0; col < NCOL; ++col) {
        const double cx = LM + (col + 0.5) * CW;
        add_text(out, fr, COVER_LABEL[col], cx - 0.5 * text_width(COVER_LABEL[col]), 0.5 * (BM - glyph_h));
        const std::string k = std::to_string(col);
        add_text(out, fr, k, cx - 0.5 * text_width(k), BM + (NGRAIN + 1) * CH + 0.5 * (BM - glyph_h));
    }
    add_text(out, fr, id, 0.8, BM + (NGRAIN + 1) * CH + 0.5 * (BM - glyph_h));
    return out;
}

// ---------------------------------------------------------------------------------------------
// The mixed filaments: physical 1 = white, 2 = translucent; virtual 3..9 = k/8 translucent.
// ---------------------------------------------------------------------------------------------
std::string pattern_for_eighths(int k)
{
    // Bresenham over 8 layers: B where floor((i + 1) k / 8) steps.
    std::string p;
    for (int i = 0; i < 8; ++i)
        p += ((i + 1) * k / 8 > i * k / 8) ? '2' : '1';
    return p;
}

std::string mixed_definitions(const std::vector<std::string> &colours)
{
    const bool saved = MixedFilamentManager::auto_generate_enabled();
    MixedFilamentManager::set_auto_generate_enabled(true);
    MixedFilamentManager mgr;
    mgr.auto_generate(colours);
    // The automatic 1+2 50% row is serialized first, deleted, so it keeps no virtual id and the
    // ladder's ids are 3..9 whatever the auto-generate preference is when the project is opened.
    for (MixedFilament &mf : mgr.mixed_filaments()) {
        mf.deleted = true;
        mf.enabled = false;
    }
    for (int k = 1; k <= 7; ++k) {
        mgr.add_custom_filament(1, 2, (k * 100 + 4) / 8, colours);
        MixedFilament &mf    = mgr.mixed_filaments().back();
        mf.manual_pattern    = MixedFilamentManager::normalize_manual_pattern(pattern_for_eighths(k));
        mf.distribution_mode = int(MixedFilament::LayerCycle);
        mf.ui_mode           = 1; // CYCLE
    }
    const std::string s = mgr.serialize_custom_entries();
    MixedFilamentManager::set_auto_generate_enabled(saved);
    return s;
}

struct CardObjects
{
    ModelObject *prism = nullptr;
    ModelObject *slab  = nullptr;
    PaintStats   stats;
};

// One plate's pair of objects at layer height `lh`, centred on plate-local (128, *) of a 256 bed.
CardObjects add_card(Model &model, double lh, const Vec2d &plate_origin)
{
    CardObjects out;
    char        idbuf[8];
    std::snprintf(idbuf, sizeof(idbuf), "%02d", int(std::lround(lh * 100.)));
    const std::string id = idbuf;
    char              namebuf[64];

    auto build = [&](const MeshBuilder &mb, const std::vector<FaceId> &faces, const char *what, const Vec2d &centre) {
        ModelObject *mo = model.add_object();
        std::snprintf(namebuf, sizeof(namebuf), "airbrush_p0_%s_%.2fmm", what, lh);
        mo->name = namebuf;
        mo->config.set("extruder", 1);
        mo->config.set_key_value("layer_height", new ConfigOptionFloat(lh));
        TriangleMesh body_mesh(mb.its);
        ModelVolume *body = mo->add_volume(std::move(body_mesh));
        body->name        = std::string(what) + "_body";
        TriangleSelector::TriangleSplittingData data = paint(mb, out.stats);
        {
            TriangleSelector sel(body->mesh());
            sel.deserialize(data, true);
            body->mmu_segmentation_facets.set(sel);
        }
        for (FaceId f : faces) {
            TriangleMesh lm(make_labels(f, id));
            ModelVolume *lv = mo->add_volume(std::move(lm));
            lv->name        = f == FaceWall ? "labels_wall" : f == FaceSlope ? "labels_45deg" : "labels_top";
        }
        ModelInstance *inst = mo->add_instance();
        (void) inst;
        mo->center_around_origin();
        mo->instances.front()->set_offset(Vec3d(plate_origin.x() + centre.x(), plate_origin.y() + centre.y(), 0.));
        mo->ensure_on_bed();
        return mo;
    };
    out.prism = build(make_prism(), { FaceWall, FaceSlope }, "prism", Vec2d(128., 150.));
    out.slab  = build(make_slab(), { FaceTop }, "top", Vec2d(128., 95.));
    return out;
}

const double LAYER_HEIGHTS[3] = { 0.08, 0.12, 0.16 };

} // namespace

// ---------------------------------------------------------------------------------------------
// Visible checks (fast): the mask, the ladder, the geometry and the paint.
// ---------------------------------------------------------------------------------------------
TEST_CASE("Airbrush P0: blue-noise mask is a deterministic permutation", "[AirbrushP0]")
{
    const std::vector<float> a = blue_noise_mask();
    const std::vector<float> b = blue_noise_mask();
    REQUIRE(a.size() == size_t(BN * BN));
    CHECK(a == b);
    std::vector<float> sorted = a;
    std::sort(sorted.begin(), sorted.end());
    for (int i = 0; i < BN * BN; ++i)
        CHECK(sorted[i] == Approx((i + 0.5) / (BN * BN)));
    // Blue, not white: at 10% the lit cells keep apart. No two of the first 102 ranks may touch
    // (8-neighbourhood, toroidal) - a white-noise pattern at this density almost always has some.
    int touching = 0;
    for (int i = 0; i < BN * BN; ++i) {
        if (a[i] >= 0.1f) continue;
        const int x = i % BN, y = i / BN;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if ((dx || dy) && a[((y + dy + BN) % BN) * BN + (x + dx + BN) % BN] < 0.1f) ++touching;
    }
    CHECK(touching == 0);
}

TEST_CASE("Airbrush P0: mix ladder patterns are exact eighths", "[AirbrushP0]")
{
    for (int k = 0; k <= 8; ++k) {
        const std::string p = pattern_for_eighths(k);
        CHECK(p.size() == 8);
        CHECK(std::count(p.begin(), p.end(), '2') == k);
    }
    const std::vector<std::string> colours = { "#FFFFFF", "#3A7BD5" };
    const std::string              defs    = mixed_definitions(colours);
    MixedFilamentManager           mgr;
    mgr.auto_generate(colours);
    mgr.load_custom_entries(defs, colours);
    REQUIRE(mgr.enabled_count() == 7);
    for (int k = 1; k <= 7; ++k) {
        const unsigned id = unsigned(2 + k);
        REQUIRE(mgr.is_mixed(id, 2));
        int b = 0;
        for (int layer = 0; layer < 8; ++layer)
            b += mgr.resolve(id, 2, layer) == 2;
        CHECK(b == k);
    }
}

TEST_CASE("Airbrush P0: card geometry is closed and the paint matches the layout", "[AirbrushP0]")
{
    for (const MeshBuilder &m : { make_prism(), make_slab() }) {
        CHECK(its_num_open_edges(m.its) == 0);
        CHECK(TriangleMesh(m.its).volume() > 0.);
    }
    const indexed_triangle_set lab = make_labels(FaceSlope, "12");
    CHECK(!lab.indices.empty());

    PaintStats s1, s2;
    const MeshBuilder prism = make_prism();
    const auto d1 = paint(prism, s1);
    const auto d2 = paint(prism, s2);
    CHECK(d1 == d2);   // deterministic
    // The mix row's seven mixed states and the translucent one are all present.
    for (int st = 2; st <= 9; ++st)
        CHECK(s1.per_state[st] > 0);
    // Achieved speck coverage per cell, on both prism faces. Small grains must land close to the
    // target; a 1.5 mm grain in a 7 x 5 mm cell is only ~15 specks, so it can only get near.
    for (const auto &kv : s1.cells) {
        const int    row = kv.first[1], col = kv.first[2];
        const double got = double(kv.second.first) / double(kv.second.second);
        const double tol = GRAINS[row] <= 0.6 ? 0.05 : GRAINS[row] <= 1.0 ? 0.10 : 0.15;
        INFO("face " << kv.first[0] << " grain " << GRAINS[row] << " coverage " << COVERAGE[col] << " got " << got);
        CHECK(std::abs(got - COVERAGE[col]) <= tol);
    }
}

// ---------------------------------------------------------------------------------------------
// The generator.
// ---------------------------------------------------------------------------------------------
TEST_CASE("Airbrush P0: write the calibration card", "[.][AirbrushP0Card]")
{
    const char *out_env = std::getenv("AIRBRUSH_P0_OUT");
    REQUIRE(out_env != nullptr);
    const std::string out_path = out_env;

    const std::string printer   = "Elegoo Centauri Carbon 0.4 nozzle";
    const std::string process   = "0.12mm Fine @Elegoo CC 0.4 nozzle";
    const std::string filament  = "Elegoo PLA @ECC";
    const std::vector<std::string> colours = { "#FFFFFF", "#3A7BD5" };

    // Keep anything the preset loader writes out of the user's data directory.
    const std::string             saved_data_dir = data_dir();
    const boost::filesystem::path scratch =
        boost::filesystem::temp_directory_path() / ("airbrush_p0_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(scratch / "datadir");
    set_data_dir((scratch / "datadir").string());
    set_temporary_dir(scratch.string());

    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    PresetBundle      library;
    library.load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent);
    PresetBundle bundle;
    bundle.load_vendor_configs_from_json(profiles, "Elegoo", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                         &library);
    REQUIRE(bundle.printers.select_preset_by_name(printer, true));
    REQUIRE(bundle.prints.select_preset_by_name(process, true));
    REQUIRE(bundle.filaments.select_preset_by_name(filament, true));
    bundle.filament_presets = { filament };
    bundle.set_num_filaments(2, colours);
    bundle.filament_presets = { filament, filament };
    DynamicPrintConfig cfg = bundle.full_config_secure();
    cfg.option<ConfigOptionStrings>("filament_colour", true)->values = colours;
    cfg.set_key_value("mixed_filament_definitions", new ConfigOptionString(mixed_definitions(colours)));

    // Three plates, laid out the way PartPlateList does for three (two columns, stride bed * 1.2).
    const BoundingBoxf bed(cfg.option<ConfigOptionPoints>("printable_area")->values);
    const double       sx = bed.size().x() * 1.2, sy = bed.size().y() * 1.2;
    const Vec2d        origins[3] = { Vec2d(0, 0), Vec2d(sx, 0), Vec2d(0, -sy) };

    Model         model;
    PlateDataPtrs plates;
    std::ostringstream report;
    for (int p = 0; p < 3; ++p) {
        const CardObjects co = add_card(model, LAYER_HEIGHTS[p], origins[p]);
        PlateData *pd   = new PlateData();
        pd->plate_index = p;
        char name[32];
        std::snprintf(name, sizeof(name), "%.2f mm layers", LAYER_HEIGHTS[p]);
        pd->plate_name = name;
        pd->objects_and_instances.emplace_back(int(model.objects.size()) - 2, 0);
        pd->objects_and_instances.emplace_back(int(model.objects.size()) - 1, 0);
        plates.push_back(pd);
        report << "plate " << p + 1 << " (" << name << "): painted leaves " << co.stats.leaves_painted << " of "
               << co.stats.leaves_total << " encoded; per state:";
        for (const auto &kv : co.stats.per_state)
            if (kv.first != 0) report << " " << kv.first << "=" << kv.second;
        report << "\n";
    }

    StoreParams sp;
    sp.path            = out_path;
    sp.model           = &model;
    sp.config          = &cfg;
    sp.plate_data_list = plates;
    sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    const bool stored  = store_bbs_3mf(sp);
    release_PlateData_list(plates);
    REQUIRE(stored);

    // Read it back: three plates, six objects, the paint and the ladder intact.
    {
        Model                     back;
        DynamicPrintConfig        back_cfg;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
        PlateDataPtrs             back_plates;
        std::vector<Preset *>     presets;
        bool                      is_bbl = false;
        Semver                    ver;
        REQUIRE(load_bbs_3mf(out_path.c_str(), &back_cfg, &ctxt, &back, &back_plates, &presets, &is_bbl, &ver, nullptr,
                             LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
        CHECK(back_plates.size() == 3);
        REQUIRE(back.objects.size() == model.objects.size());
        for (size_t o = 0; o < model.objects.size(); ++o)
            CHECK(back.objects[o]->volumes.front()->mmu_segmentation_facets.get_data() ==
                  model.objects[o]->volumes.front()->mmu_segmentation_facets.get_data());
        CHECK(back_cfg.opt_string("mixed_filament_definitions") == cfg.opt_string("mixed_filament_definitions"));
        release_PlateData_list(back_plates);
        for (Preset *p : presets) delete p;
    }

    const boost::filesystem::path out_file(out_path);
    report << "file " << out_path << " " << boost::filesystem::file_size(out_file) << " bytes\n";
    report << "presets: " << printer << " | " << process << " | " << filament << " x2 (filament 1 white, 2 translucent)\n";
    WARN(report.str());
    set_data_dir(saved_data_dir);
    boost::system::error_code ec;
    boost::filesystem::remove_all(scratch, ec);
}
