// Image Fill - the shared service. See ImageFill.hpp for the contract and the spec at
// docs/superpowers/specs/2026-09-07-imagemap-phase2-imagefill.md for why it is shaped this way.
#include "ImageFill.hpp"

#include "Model.hpp"
#include "PNGReadWrite.hpp"

#include "ColorSolver.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <unordered_map>

namespace Slic3r {

// =============================================================================================
// 1. The asset store
// =============================================================================================

std::string image_fill_sha256_hex(const uint8_t *data, size_t len)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_CTX    ctx;
    SHA256_Init(&ctx);
    if (data != nullptr && len > 0)
        SHA256_Update(&ctx, data, len);
    SHA256_Final(digest, &ctx);
    char out[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        std::snprintf(&out[i * 2], 3, "%02x", (unsigned int) digest[i]);
    return std::string(out, SHA256_DIGEST_LENGTH * 2);
}

std::string ImageAssetStore::add(std::vector<uint8_t> bytes)
{
    if (bytes.empty())
        return std::string();
    const std::string sha = image_fill_sha256_hex(bytes);
    auto it = m_assets.find(sha);
    if (it != m_assets.end())
        return sha;   // same content, same entry - the point of a content key
    ImageAsset a;
    a.sha256 = sha;
    a.bytes  = std::move(bytes);
    m_assets.emplace(sha, std::move(a));
    return sha;
}

const ImageAsset *ImageAssetStore::find(const std::string &sha256) const
{
    auto it = m_assets.find(sha256);
    return it == m_assets.end() ? nullptr : &it->second;
}

void ImageAssetStore::retain(const std::vector<std::string> &keep)
{
    for (auto it = m_assets.begin(); it != m_assets.end();) {
        if (std::find(keep.begin(), keep.end(), it->first) == keep.end())
            it = m_assets.erase(it);
        else
            ++it;
    }
}

const ImageAsset *ImageAssetStore::pixels(const std::string &sha256) const
{
    const ImageAsset *a = this->find(sha256);
    if (a == nullptr)
        return nullptr;
    if (a->decoded)
        return a->decode_failed ? nullptr : a;
    a->decoded = true;
    png::ImageColorscale img;
    png::ReadBuf         rb{a->bytes.data(), a->bytes.size()};
    if (!png::is_png(rb) || !png::decode_colored_png(rb, img) || img.rows == 0 || img.cols == 0 ||
        (img.bytes_per_pixel != 3 && img.bytes_per_pixel != 4)) {
        // PNGReadWrite handles 8-bit RGB/RGBA only; a paletted, greyscale or 16-bit PNG lands
        // here. The GUI normalises through wxImage before storing, so this is only reachable
        // from a project written by something else - it degrades to "no image", not a crash.
        a->decode_failed = true;
        return nullptr;
    }
    a->width  = (unsigned) img.cols;
    a->height = (unsigned) img.rows;
    a->rgb.resize(size_t(img.cols) * size_t(img.rows) * 3);
    const int bpp = img.bytes_per_pixel;
    // png::decode_colored_png fills its buffer BOTTOM-UP: PNGReadWrite.cpp:163-166 walks the rows
    // backwards, so buf row 0 is the image's LAST row. Flip it here, once, so everything above
    // this line can say "row 0 is the top row" and mean it.
    for (size_t y = 0; y < size_t(img.rows); ++y) {
        const size_t src_row = size_t(img.rows) - 1 - y;
        for (size_t x = 0; x < size_t(img.cols); ++x) {
            const size_t si = (src_row * size_t(img.cols) + x) * size_t(bpp);
            const size_t di = (y * size_t(img.cols) + x) * 3;
            a->rgb[di + 0] = img.buf[si + 0];
            a->rgb[di + 1] = img.buf[si + 1];
            a->rgb[di + 2] = img.buf[si + 2];
        }
    }
    return a;
}

// =============================================================================================
// 2. Gradients and projections
// =============================================================================================

std::array<float, 3> ImageFillGradient::sample(float u, float v) const
{
    float t = 0.f;
    if (direction == 0)
        t = u;
    else if (direction == 1)
        t = v;
    else {
        const float dx = u - 0.5f, dy = v - 0.5f;
        t = std::min(1.f, std::sqrt(dx * dx + dy * dy) / 0.70710678f);
    }
    t = std::min(1.f, std::max(0.f, t));
    std::array<float, 3> out{};
    if (three_stop) {
        // Two halves, so the middle stop lands exactly at t = 0.5.
        if (t <= 0.5f) {
            const float s = t * 2.f;
            for (int i = 0; i < 3; ++i) out[i] = stop_a[i] + (stop_b[i] - stop_a[i]) * s;
        } else {
            const float s = (t - 0.5f) * 2.f;
            for (int i = 0; i < 3; ++i) out[i] = stop_b[i] + (stop_c[i] - stop_b[i]) * s;
        }
    } else {
        for (int i = 0; i < 3; ++i) out[i] = stop_a[i] + (stop_b[i] - stop_a[i]) * t;
    }
    return out;
}

bool image_fill_project(const ImageFillParams &params, const BoundingBoxf3 &box, const Vec3f &p,
                        float &u, float &v)
{
    const int  ax = int(params.axis);
    const Vec3d size = box.size();
    auto norm = [](double x, double lo, double sz) -> float {
        if (sz <= 0.) return 0.5f;
        return float(std::min(1., std::max(0., (x - lo) / sz)));
    };

    if (params.projection == ImageFillProjection::Planar) {
        // The two axes that are not `axis`, in ascending index order: axis Z gives (x, y),
        // axis X gives (y, z), axis Y gives (x, z). u is the first, v the second.
        const int a0 = (ax == 0) ? 1 : 0;
        const int a1 = (ax == 2) ? 1 : 2;
        u = norm(p[a0], box.min[a0], size[a0]);
        v = norm(p[a1], box.min[a1], size[a1]);
    } else if (params.projection == ImageFillProjection::Cylindrical) {
        const int a0 = (ax == 0) ? 1 : 0;
        const int a1 = (ax == 2) ? 1 : 2;
        const double cx = (box.min[a0] + box.max[a0]) * 0.5;
        const double cy = (box.min[a1] + box.max[a1]) * 0.5;
        const double dx = p[a0] - cx, dy = p[a1] - cy;
        if (dx * dx + dy * dy < 1e-12)
            return false;   // exactly on the axis: no angle to speak of
        // atan2 in [-pi, pi] -> [0, 1). THE SEAM IS PREDICTABLE AND FIXED: u = 0 and u = 1 meet
        // on the NEGATIVE side of the first of the two axes that are not `axis` - so for a wrap
        // about Z the seam is on -X and the middle of the image (u = 0.5) faces +X; about Y it is
        // on -X again with u = 0.5 facing +X; about X it is on -Y with u = 0.5 facing +Y. Turning
        // the part turns the seam with it, because the projection is in the part's own space.
        u = float(std::atan2(dy, dx) / (2.0 * M_PI) + 0.5);
        if (u >= 1.f) u -= 1.f;
        v = norm(p[ax], box.min[ax], size[ax]);
    } else {
        return false;   // MeshUV is supplied by the caller, not computed here
    }
    if (params.flip_u) u = 1.f - u;
    if (params.flip_v) v = 1.f - v;
    return true;
}

Vec3f image_fill_direction(const ImageFillParams &params)
{
    Vec3f d(0.f, 0.f, 0.f);
    d[int(params.axis)] = params.axis_negative ? -1.f : 1.f;
    return d;
}

bool image_fill_face_is_painted(const ImageFillParams &params, const BoundingBoxf3 &box,
                                const Vec3f &normal, const Vec3f &centroid)
{
    // The UVs decide coverage for a mesh-UV fill, and All is All.
    if (params.projection == ImageFillProjection::MeshUV || params.faces == ImageFillFaces::All)
        return true;
    const float n = normal.norm();
    if (n <= 0.f)
        return false;   // a degenerate facet faces nothing

    Vec3f dir;
    if (params.projection == ImageFillProjection::Cylindrical) {
        // "Facing" for a wrap means facing away from the axis: the direction is the facet's own
        // outward radial, so a cylinder's wall is painted and its end caps - whose normals are
        // parallel to the axis, radial component exactly 0 - are not. axis_negative flips it to
        // the inward-facing surfaces, which is what a bore wants.
        const int   ax = int(params.axis);
        const int   a0 = (ax == 0) ? 1 : 0;
        const int   a1 = (ax == 2) ? 1 : 2;
        const float cx = float((box.min[a0] + box.max[a0]) * 0.5);
        const float cy = float((box.min[a1] + box.max[a1]) * 0.5);
        Vec3f       r(0.f, 0.f, 0.f);
        r[a0] = centroid[a0] - cx;
        r[a1] = centroid[a1] - cy;
        const float rl = r.norm();
        if (rl <= 0.f)
            return false;   // the facet sits on the axis: no outward to speak of
        dir = (params.axis_negative ? -1.f : 1.f) * (r / rl);
    } else {
        dir = image_fill_direction(params);
    }

    const float cosine = normal.dot(dir) / n;
    return params.faces == ImageFillFaces::Through ? std::abs(cosine) > IMAGE_FILL_FACING_EPS
                                                   : cosine > IMAGE_FILL_FACING_EPS;
}

// =============================================================================================
// 3. Params serialisation
// =============================================================================================

static std::string f2s(float f)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.6g", double(f));
    return b;
}
static std::string rgb2s(const std::array<float, 3> &c)
{
    return f2s(c[0]) + "," + f2s(c[1]) + "," + f2s(c[2]);
}
static bool s2rgb(const std::string &s, std::array<float, 3> &c)
{
    float a = 0, b = 0, d = 0;
    if (std::sscanf(s.c_str(), "%f,%f,%f", &a, &b, &d) != 3) return false;
    c = {a, b, d};
    return true;
}

std::string ImageFillParams::to_string() const
{
    std::ostringstream os;
    os << "v=1";
    if (!asset.empty()) os << ";img=" << asset;
    os << ";proj=" << int(projection) << ";axis=" << int(axis);
    // Written always, not only when it is not the default: a params string that does not say
    // which faces it painted is a string from before this rule existed, and it must not be
    // mistaken for one that chose today's default on purpose.
    os << ";pf=" << int(faces);
    if (axis_negative) os << ";an=1";
    if (flip_u) os << ";fu=1";
    if (flip_v) os << ";fv=1";
    os << ";sub=" << subdivision;
    if (detail_mm > 0.f) os << ";det=" << f2s(detail_mm);
    if (background != 0) os << ";bg=" << background;
    if (selection_state != 0) os << ";sel=" << selection_state;
    if (!allowed.empty()) {
        os << ";f=";
        for (size_t i = 0; i < allowed.size(); ++i) os << (i ? "," : "") << allowed[i];
    }
    if (gradient.enabled) {
        os << ";g=1;ga=" << rgb2s(gradient.stop_a) << ";gb=" << rgb2s(gradient.stop_b);
        if (gradient.three_stop) os << ";gc=" << rgb2s(gradient.stop_c) << ";g3=1";
        os << ";gd=" << gradient.direction;
    }
    return os.str();
}

bool ImageFillParams::from_string(const std::string &s, ImageFillParams &out)
{
    out = ImageFillParams();
    if (s.empty()) return false;
    bool   seen_version = false;
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t next = s.find(';', pos);
        const std::string tok = s.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        pos = (next == std::string::npos) ? s.size() + 1 : next + 1;
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = tok.substr(0, eq), val = tok.substr(eq + 1);
        if (k == "v")          seen_version = true;
        else if (k == "img")   out.asset = val;
        else if (k == "proj")  out.projection = ImageFillProjection(std::max(0, std::min(2, std::atoi(val.c_str()))));
        else if (k == "axis")  out.axis = ImageFillAxis(std::max(0, std::min(2, std::atoi(val.c_str()))));
        else if (k == "pf")    out.faces = ImageFillFaces(std::max(0, std::min(2, std::atoi(val.c_str()))));
        else if (k == "an")    out.axis_negative = val != "0";
        else if (k == "fu")    out.flip_u = val != "0";
        else if (k == "fv")    out.flip_v = val != "0";
        else if (k == "sub")   out.subdivision = std::max(0, std::min(IMAGE_FILL_MAX_SUBDIVISION, std::atoi(val.c_str())));
        else if (k == "det")   out.detail_mm = float(std::atof(val.c_str()));
        else if (k == "bg")    out.background = std::atoi(val.c_str());
        else if (k == "sel")   out.selection_state = std::atoi(val.c_str());
        else if (k == "g")     out.gradient.enabled = val != "0";
        else if (k == "g3")    out.gradient.three_stop = val != "0";
        else if (k == "gd")    out.gradient.direction = std::atoi(val.c_str());
        else if (k == "ga")    s2rgb(val, out.gradient.stop_a);
        else if (k == "gb")    s2rgb(val, out.gradient.stop_b);
        else if (k == "gc")    s2rgb(val, out.gradient.stop_c);
        else if (k == "f") {
            size_t p = 0;
            while (p <= val.size()) {
                const size_t n = val.find(',', p);
                const std::string one = val.substr(p, n == std::string::npos ? std::string::npos : n - p);
                if (!one.empty()) out.allowed.push_back(std::atoi(one.c_str()));
                if (n == std::string::npos) break;
                p = n + 1;
            }
        }
    }
    return seen_version;
}

// =============================================================================================
// 4. Quantisation
// =============================================================================================

ImageFillPalette image_fill_quantise(const std::vector<std::array<float, 3>> &samples, size_t max_colors)
{
    ImageFillPalette out;
    if (samples.empty() || max_colors == 0)
        return out;

    // A fixed 5-bit-per-channel bucket. 32768 cells, so the pass is O(n) with no randomness and
    // no iteration count - the same image gives the same palette in the same order, every time,
    // which is what the determinism half of Bar B needs.
    struct Cell { double r = 0, g = 0, b = 0; size_t n = 0; uint32_t key = 0; };
    std::unordered_map<uint32_t, Cell> cells;
    cells.reserve(std::min<size_t>(samples.size(), 32768));
    auto q5 = [](float c) -> uint32_t {
        int v = int(std::lround(std::min(1.f, std::max(0.f, c)) * 31.f));
        return uint32_t(std::max(0, std::min(31, v)));
    };
    for (const auto &s : samples) {
        const uint32_t key = (q5(s[0]) << 10) | (q5(s[1]) << 5) | q5(s[2]);
        Cell &c = cells[key];
        c.key = key;
        c.r += s[0]; c.g += s[1]; c.b += s[2];
        ++c.n;
    }
    std::vector<Cell> v;
    v.reserve(cells.size());
    for (const auto &kv : cells) v.push_back(kv.second);
    // Busiest first; the bucket key breaks ties so the order does not depend on the hash map.
    std::sort(v.begin(), v.end(), [](const Cell &a, const Cell &b) {
        return a.n != b.n ? a.n > b.n : a.key < b.key;
    });

    const size_t keep = std::min(max_colors, v.size());
    out.colors.reserve(keep);
    out.counts.assign(keep, 0);
    for (size_t i = 0; i < keep; ++i)
        out.colors.push_back({float(v[i].r / double(v[i].n)), float(v[i].g / double(v[i].n)),
                              float(v[i].b / double(v[i].n))});
    // Anything past the cap is merged into its nearest kept representative, so every sample is
    // still accounted for and the counts add up to samples.size().
    for (size_t i = 0; i < keep; ++i)
        out.counts[i] = v[i].n;
    for (size_t i = keep; i < v.size(); ++i) {
        const std::array<float, 3> c{float(v[i].r / double(v[i].n)), float(v[i].g / double(v[i].n)),
                                     float(v[i].b / double(v[i].n))};
        size_t best = 0; float bestd = std::numeric_limits<float>::max();
        for (size_t j = 0; j < keep; ++j) {
            const float d = (c[0] - out.colors[j][0]) * (c[0] - out.colors[j][0]) +
                            (c[1] - out.colors[j][1]) * (c[1] - out.colors[j][1]) +
                            (c[2] - out.colors[j][2]) * (c[2] - out.colors[j][2]);
            if (d < bestd) { bestd = d; best = j; }
        }
        out.counts[best] += v[i].n;
    }
    out.filament.assign(keep, 0);
    return out;
}

// =============================================================================================
// 5. The solve
// =============================================================================================

void image_fill_solve(ImageFillPalette &palette, const std::vector<std::array<float, 3>> &filament_colors,
                      const std::vector<int> &filament_ids)
{
    palette.filament.assign(palette.colors.size(), 0);
    if (filament_colors.empty() || filament_colors.size() != filament_ids.size())
        return;

    // One component per facet: Phase 2's writer is mmu_segmentation_facets, which holds a single
    // id per triangle. The constraint is what makes the phase 1 solver answer the question this
    // phase is actually asking, and lifting it in Phase 3 is the whole of the change there.
    ColorSolverConstraints constraints;
    constraints.min_components = 1;
    constraints.max_components = 1;

    // Cached per palette (the component colours plus the constraints are the cache key), so a
    // batch of parts sharing filaments pays for the enumeration once - the plan's phase 1
    // finding about the 0.3 s per colour is about the batch dialog's 1 % sweep, but the
    // enumeration itself is the part worth caching whatever the caller does with it.
    static thread_local ColorSolverCandidateCache cache;
    const ColorSolverCandidateSet &set = color_solver_candidates(cache, filament_colors, 0, constraints);
    if (set.empty())
        return;

    for (size_t i = 0; i < palette.colors.size(); ++i) {
        const size_t cand = solve_color_solver_candidate_for_target(set, palette.colors[i],
                                                                    ColorSolverMode::OklabSoftCap4Dark4);
        if (cand == size_t(-1))
            continue;
        // With max_components == 1 exactly one weight is non-zero; that component is the answer.
        size_t best = 0; float bestw = -1.f;
        for (size_t c = 0; c < set.component_count; ++c) {
            const float w = set.weights[cand * set.component_count + c];
            if (w > bestw) { bestw = w; best = c; }
        }
        if (best < filament_ids.size())
            palette.filament[i] = filament_ids[best];
    }
}

// =============================================================================================
// 6. Subdivision and the bitstream
// =============================================================================================

void image_fill_subdivide(const Vec3f &a, const Vec3f &b, const Vec3f &c, int depth,
                          std::vector<ImageFillLeaf> &out)
{
    if (depth <= 0) {
        out.push_back(ImageFillLeaf{a, b, c});
        return;
    }
    // TriangleSelector::perform_split's three-side case, transcribed. With special_side == 0 the
    // vertex list becomes [v0, m01, v1, m12, v2, m20] and the four children are, in order:
    //   (v0, m01, m20), (m01, v1, m12), (m12, v2, m20), (m01, m12, m20)
    // Midpoints of shared edges are computed from the same two endpoints on both sides, so a
    // uniform depth over the whole volume leaves no T-joint anywhere - which is why the depth is
    // uniform rather than adaptive.
    const Vec3f m01 = (a + b) * 0.5f;
    const Vec3f m12 = (b + c) * 0.5f;
    const Vec3f m20 = (c + a) * 0.5f;
    image_fill_subdivide(a,   m01, m20, depth - 1, out);
    image_fill_subdivide(m01, b,   m12, depth - 1, out);
    image_fill_subdivide(m12, c,   m20, depth - 1, out);
    image_fill_subdivide(m01, m12, m20, depth - 1, out);
}

namespace {
// Emit one leaf or one internal node into the bitstream, mirroring TriangleSelector::serialize()
// exactly - including the reversed child order it keeps for PrusaSlicer 2.3.1 compatibility.
void encode_node(std::vector<bool> &bits, int depth, const int *states, size_t stride)
{
    if (depth == 0) {
        const int n = *states;
        bits.push_back(false);   // number_of_split_sides & 0b01
        bits.push_back(false);   // number_of_split_sides & 0b10
        if (n >= 3) {
            bits.push_back(true); bits.push_back(true);
            int m = n - 3;
            while (m >= 15) {
                for (int i = 0; i < 4; ++i) bits.push_back(true);
                m -= 15;
            }
            for (int i = 0; i < 4; ++i) bits.push_back((m & (1 << i)) != 0);
        } else {
            bits.push_back((n & 0b01) != 0);
            bits.push_back((n & 0b10) != 0);
        }
        return;
    }
    bits.push_back(true);    // 3 & 0b01
    bits.push_back(true);    // 3 & 0b10
    bits.push_back(false);   // special_side 0 & 0b01
    bits.push_back(false);   // special_side 0 & 0b10
    // Children in reverse: child 3, 2, 1, 0.
    const size_t child = stride / 4;
    for (int i = 3; i >= 0; --i)
        encode_node(bits, depth - 1, states + size_t(i) * child, child);
}
} // namespace

TriangleSelector::TriangleSplittingData image_fill_encode(
    size_t n_original_triangles, int depth, const std::vector<int> &states,
    const std::vector<bool> *selected, const TriangleSelector::TriangleSplittingData *existing)
{
    TriangleSelector::TriangleSplittingData data;
    size_t per = 1;
    for (int i = 0; i < depth; ++i) per *= 4;
    if (states.size() != n_original_triangles * per)
        return data;
    const bool merge = selected != nullptr && existing != nullptr &&
                       selected->size() == n_original_triangles;

    // Where each original triangle's subtree lives in `existing`. serialize() writes the entries in
    // ascending triangle order and the bitstream in that same order, so a triangle's bits run from
    // its own start index to the next entry's - which is what makes a verbatim copy possible.
    std::map<int, std::pair<size_t, size_t>> carry;
    if (merge) {
        const auto &tts = existing->triangles_to_split;
        for (size_t k = 0; k < tts.size(); ++k) {
            const size_t begin = size_t(tts[k].bitstream_start_idx);
            const size_t end   = (k + 1 < tts.size()) ? size_t(tts[k + 1].bitstream_start_idx)
                                                      : existing->bitstream.size();
            if (begin <= end && end <= existing->bitstream.size())
                carry.emplace(tts[k].triangle_idx, std::make_pair(begin, end));
        }
    }

    data.triangles_to_split.reserve(n_original_triangles);
    for (size_t t = 0; t < n_original_triangles; ++t) {
        if (merge && !(*selected)[t]) {
            // Not part of this fill: keep what was there, bit for bit.
            auto it = carry.find(int(t));
            if (it == carry.end())
                continue;   // it carried nothing before either
            data.triangles_to_split.emplace_back(int(t), int(data.bitstream.size()));
            data.bitstream.insert(data.bitstream.end(),
                                  existing->bitstream.begin() + it->second.first,
                                  existing->bitstream.begin() + it->second.second);
            continue;
        }
        const int *s = states.data() + t * per;
        bool any = false;
        for (size_t i = 0; i < per; ++i)
            if (s[i] != 0) { any = true; break; }
        // serialize() skips an original triangle that is neither split nor painted; matching that
        // keeps an untouched part's annotation byte-identical to what the fork writes today.
        if (!any)
            continue;
        data.triangles_to_split.emplace_back(int(t), int(data.bitstream.size()));
        encode_node(data.bitstream, depth, s, per);
    }
    for (size_t t = 0; t < n_original_triangles; ++t) {
        if (merge && !(*selected)[t])
            continue;
        const int *s = states.data() + t * per;
        for (size_t i = 0; i < per; ++i) {
            const int n = s[i];
            if (n >= 0 && size_t(n) < data.used_states.size())
                data.used_states[size_t(n)] = true;
        }
    }
    if (merge)
        for (size_t st = 0; st < existing->used_states.size() && st < data.used_states.size(); ++st)
            if (existing->used_states[st])
                data.used_states[st] = true;
    return data;
}

int image_fill_depth_for_detail(float max_edge_mm, float detail_mm, int cap, size_t n_triangles)
{
    int depth = std::max(0, std::min(IMAGE_FILL_MAX_SUBDIVISION, cap));
    if (detail_mm > 0.f && max_edge_mm > 0.f) {
        const double need = std::log2(double(max_edge_mm) / double(detail_mm));
        depth = std::max(0, std::min(depth, int(std::ceil(need))));
        if (need <= 0.) depth = 0;
    }
    // The leaf ceiling. 4^depth per triangle, so drop a level at a time until it fits.
    while (depth > 0) {
        size_t per = 1;
        bool   overflow = false;
        for (int i = 0; i < depth; ++i) {
            per *= 4;
            if (n_triangles != 0 && per > IMAGE_FILL_MAX_LEAVES / n_triangles) { overflow = true; break; }
        }
        if (!overflow)
            break;
        --depth;
    }
    return depth;
}

// =============================================================================================
// 7. The service
// =============================================================================================

// Nearest palette entry to a colour, in plain linear-sRGB distance. The perceptual work has
// already happened - the palette entries were solved to filament ids in Oklab - so this only has
// to decide which representative a sample belongs to, and it has to be fast: it runs once per
// leaf, and there can be millions of leaves.
static size_t nearest_palette(const ImageFillPalette &p, const std::array<float, 3> &c)
{
    size_t best = 0;
    float  bestd = std::numeric_limits<float>::max();
    for (size_t i = 0; i < p.colors.size(); ++i) {
        const float dr = c[0] - p.colors[i][0], dg = c[1] - p.colors[i][1], db = c[2] - p.colors[i][2];
        const float d = dr * dr + dg * dg + db * db;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

// The image sampler shared by every projection: nearest-neighbour, clamped, top row first.
// Nearest-neighbour rather than bilinear on purpose - a facet gets one filament, so smoothing
// the source before quantising it to at most a few filaments only blurs the edges the printer
// is about to reproduce hard.
static std::array<float, 3> sample_image(const ImageAsset &a, float u, float v)
{
    if (a.width == 0 || a.height == 0)
        return {0.f, 0.f, 0.f};
    int x = int(std::floor(u * float(a.width)));
    int y = int(std::floor((1.f - v) * float(a.height)));   // v runs up, image rows run down
    x = std::max(0, std::min(int(a.width) - 1, x));
    y = std::max(0, std::min(int(a.height) - 1, y));
    const size_t o = (size_t(y) * size_t(a.width) + size_t(x)) * 3;
    return {a.rgb[o] / 255.f, a.rgb[o + 1] / 255.f, a.rgb[o + 2] / 255.f};
}

ImageFillResult image_fill_compute(const indexed_triangle_set                    &mesh,
                                   const TriangleSelector::TriangleSplittingData &existing,
                                   const ImageFillParams                         &params,
                                   const ImageAssetStore                         &assets,
                                   const std::vector<std::array<float, 3>>       &filament_colors,
                                   const std::vector<int>                        &filament_ids,
                                   const ImageFillUVs                            &uvs)
{
    ImageFillResult res;
    if (mesh.indices.empty()) { res.error = "The part has no triangles."; return res; }
    if (filament_ids.empty() || filament_ids.size() != filament_colors.size()) {
        res.error = "No filaments were allowed for the image fill.";
        return res;
    }

    const ImageAsset *img = nullptr;
    if (!params.asset.empty()) {
        img = assets.pixels(params.asset);
        if (img == nullptr) { res.error = "The image could not be decoded."; return res; }
    } else if (!params.gradient.enabled) {
        res.error = "No image and no gradient was chosen.";
        return res;
    }
    if (params.projection == ImageFillProjection::MeshUV && uvs.size() != mesh.indices.size()) {
        res.error = "This part has no texture coordinates to project through.";
        return res;
    }

    // --- the box the projection is normalised over -------------------------------------------
    BoundingBoxf3 box;
    for (const Vec3f &v : mesh.vertices) box.merge(v.cast<double>());

    // --- which facets take part -------------------------------------------------------------
    // A face selection is the existing MMU paint: only facets whose current state matches
    // `selection_state` are filled. Read through TriangleSelector so a *partially* painted facet
    // counts the way the paint tool means it - by its own leaves, not by its whole self.
    std::vector<bool> selected(mesh.indices.size(), true);
    if (params.selection_state > 0) {
        TriangleMesh     tm(mesh);
        TriangleSelector sel(tm);
        sel.deserialize(existing, /*needs_reset=*/false);
        const indexed_triangle_set picked = sel.get_facets(EnforcerBlockerType(params.selection_state));
        if (picked.indices.empty()) { res.error = "Nothing is painted with the selected filament."; return res; }
        // A leaf belongs to its source triangle; the cheap and exact test is "does this original
        // facet contain any picked leaf", done by centroid containment against the source facet.
        std::fill(selected.begin(), selected.end(), false);
        // get_facets() does not report the source triangle, so fall back to a bbox-free test:
        // every picked leaf's centroid lies inside exactly one original facet's plane triangle.
        // For the common whole-facet selection the leaf IS the facet, so match on the vertices.
        std::map<std::array<float, 9>, size_t> by_verts;
        for (size_t i = 0; i < mesh.indices.size(); ++i) {
            const Vec3f &a = mesh.vertices[mesh.indices[i](0)];
            const Vec3f &b = mesh.vertices[mesh.indices[i](1)];
            const Vec3f &c = mesh.vertices[mesh.indices[i](2)];
            by_verts.emplace(std::array<float, 9>{a.x(), a.y(), a.z(), b.x(), b.y(), b.z(),
                                                  c.x(), c.y(), c.z()}, i);
        }
        size_t hit = 0;
        for (const Vec3i32 &t : picked.indices) {
            const Vec3f &a = picked.vertices[t(0)];
            const Vec3f &b = picked.vertices[t(1)];
            const Vec3f &c = picked.vertices[t(2)];
            auto it = by_verts.find(std::array<float, 9>{a.x(), a.y(), a.z(), b.x(), b.y(), b.z(),
                                                         c.x(), c.y(), c.z()});
            if (it != by_verts.end()) { selected[it->second] = true; ++hit; }
        }
        if (hit == 0) {
            res.error = "The face selection is finer than the part's own triangles; paint whole "
                        "facets, or apply the image to the whole part.";
            return res;
        }
    }

    // --- which facets the projection actually lands on ----------------------------------------
    // The fix for "a planar fill painted every face, including the far one and the sides": a
    // projection is a direction, so a facet is painted only when it faces that direction. Decided
    // once per ORIGINAL facet, because every leaf of a facet shares its plane and therefore its
    // normal - the subdivision refines the sampling, not the geometry.
    size_t facing_facets = 0;
    for (size_t t = 0; t < mesh.indices.size(); ++t) {
        if (!selected[t]) continue;
        const Vec3i32 &f = mesh.indices[t];
        const Vec3f   &a = mesh.vertices[f(0)], &b = mesh.vertices[f(1)], &c = mesh.vertices[f(2)];
        const Vec3f    n = (b - a).cross(c - a);
        if (image_fill_face_is_painted(params, box, n, (a + b + c) / 3.f))
            ++facing_facets;
        else
            selected[t] = false;
    }
    if (facing_facets == 0) {
        res.error = params.faces == ImageFillFaces::Through
                        ? "No face of the part is square-on to that axis; try another axis."
                        : "No face of the part faces that axis; try another axis, the other side "
                          "of it, or \"Project through (both sides)\".";
        return res;
    }

    // --- how deep ---------------------------------------------------------------------------
    float max_edge = 0.f;
    for (const Vec3i32 &t : mesh.indices) {
        const Vec3f &a = mesh.vertices[t(0)], &b = mesh.vertices[t(1)], &c = mesh.vertices[t(2)];
        max_edge = std::max({max_edge, (b - a).norm(), (c - b).norm(), (a - c).norm()});
    }
    const int depth = image_fill_depth_for_detail(max_edge, params.detail_mm, params.subdivision,
                                                  mesh.indices.size());
    res.subdivision_used = depth;
    size_t per = 1;
    for (int i = 0; i < depth; ++i) per *= 4;
    res.leaves_total = mesh.indices.size() * per;

    // --- sample -----------------------------------------------------------------------------
    std::vector<std::array<float, 3>> samples;
    samples.resize(res.leaves_total, {0.f, 0.f, 0.f});
    std::vector<bool> has_sample(res.leaves_total, false);

    std::vector<ImageFillLeaf> leaves;
    leaves.reserve(per);
    for (size_t t = 0; t < mesh.indices.size(); ++t) {
        if (!selected[t]) continue;
        const Vec3i32 &f = mesh.indices[t];
        leaves.clear();
        image_fill_subdivide(mesh.vertices[f(0)], mesh.vertices[f(1)], mesh.vertices[f(2)], depth, leaves);
        // Mesh UVs are per vertex; a leaf's UV is the same barycentric blend its position is, so
        // subdividing in UV space alongside the position is exact for a linear parameterisation -
        // which is what glTF's TEXCOORD_0 is inside a triangle.
        std::vector<ImageFillLeaf> uvleaves;
        if (params.projection == ImageFillProjection::MeshUV) {
            const std::array<Vec2f, 3> &uv = uvs[t];
            uvleaves.reserve(per);
            image_fill_subdivide(Vec3f(uv[0].x(), uv[0].y(), 0.f), Vec3f(uv[1].x(), uv[1].y(), 0.f),
                                 Vec3f(uv[2].x(), uv[2].y(), 0.f), depth, uvleaves);
        }
        for (size_t i = 0; i < leaves.size(); ++i) {
            float u = 0.f, v = 0.f;
            bool  ok = true;
            if (params.projection == ImageFillProjection::MeshUV) {
                const Vec3f cu = uvleaves[i].centroid();
                // REPEAT wrap, matching what GLTF.cpp's sampler does today.
                u = cu.x() - std::floor(cu.x());
                v = cu.y() - std::floor(cu.y());
                // glTF's v runs down from the top-left; sample_image's v runs up. Flip once here
                // so the picture lands the same way up as the exporter drew it.
                v = 1.f - v;
                if (params.flip_u) u = 1.f - u;
                if (params.flip_v) v = 1.f - v;
            } else {
                ok = image_fill_project(params, box, leaves[i].centroid(), u, v);
            }
            if (!ok) continue;
            const size_t idx = t * per + i;
            samples[idx] = img != nullptr ? sample_image(*img, u, v) : params.gradient.sample(u, v);
            has_sample[idx] = true;
        }
    }

    // --- quantise, solve, write -------------------------------------------------------------
    std::vector<std::array<float, 3>> present;
    present.reserve(res.leaves_total);
    for (size_t i = 0; i < res.leaves_total; ++i)
        if (has_sample[i]) present.push_back(samples[i]);
    if (present.empty()) { res.error = "The image did not cover any of the part."; return res; }

    res.palette = image_fill_quantise(present, 256);
    image_fill_solve(res.palette, filament_colors, filament_ids);

    std::vector<int> states(res.leaves_total, params.background);
    for (size_t i = 0; i < res.leaves_total; ++i) {
        if (!has_sample[i]) continue;
        const size_t p = nearest_palette(res.palette, samples[i]);
        states[i] = res.palette.filament[p];
    }
    for (size_t i = 0; i < states.size(); ++i)
        if (states[i] > 0) ++res.facets_painted;

    // A face selection is a MERGE: only the selected facets take the image, and everything the
    // part was already painted with survives untouched - and `selected` now also has the facets
    // the projection does not land on cleared, so those keep their old paint rather than being
    // wiped by a fill that never reached them. Filling the WHOLE part is still a replacement: the
    // faces the image misses come out unpainted, which is what "the image is on this face and
    // nowhere else" has to mean.
    res.painting = params.selection_state > 0
                       ? image_fill_encode(mesh.indices.size(), depth, states, &selected, &existing)
                       : image_fill_encode(mesh.indices.size(), depth, states);
    std::vector<int> used;
    for (int s : states)
        if (s > 0 && std::find(used.begin(), used.end(), s) == used.end()) used.push_back(s);
    std::sort(used.begin(), used.end());
    res.filaments_used = std::move(used);
    res.ok = true;
    return res;
}

ImageFillResult image_fill_from_face_colors(const indexed_triangle_set              &mesh,
                                            const std::vector<std::array<float, 3>> &face_colors,
                                            const std::vector<std::array<float, 3>> &filament_colors,
                                            const std::vector<int>                  &filament_ids,
                                            int                                      background,
                                            int                                      depth)
{
    ImageFillResult res;
    depth = std::max(0, std::min(IMAGE_FILL_MAX_SUBDIVISION, depth));
    size_t per = 1;
    for (int i = 0; i < depth; ++i) per *= 4;
    if (mesh.indices.empty() || face_colors.size() != mesh.indices.size() * per) {
        res.error = "The per-face colour list does not match the mesh.";
        return res;
    }
    if (filament_ids.empty() || filament_ids.size() != filament_colors.size()) {
        res.error = "No filaments were allowed for the image fill.";
        return res;
    }
    res.palette = image_fill_quantise(face_colors, 256);
    image_fill_solve(res.palette, filament_colors, filament_ids);

    std::vector<int> states(face_colors.size(), background);
    for (size_t i = 0; i < face_colors.size(); ++i)
        states[i] = res.palette.filament[nearest_palette(res.palette, face_colors[i])];
    for (int s : states)
        if (s > 0) ++res.facets_painted;
    res.leaves_total     = face_colors.size();
    res.subdivision_used = depth;
    res.painting         = image_fill_encode(mesh.indices.size(), depth, states);
    std::vector<int> used;
    for (int s : states)
        if (s > 0 && std::find(used.begin(), used.end(), s) == used.end()) used.push_back(s);
    std::sort(used.begin(), used.end());
    res.filaments_used = std::move(used);
    res.ok = true;
    return res;
}

// =============================================================================================
// 8. The model-level wrapper
// =============================================================================================

ImageFillResult image_fill_apply(ModelVolume &volume, const ImageFillParams &params,
                                 const ImageAssetStore                   &assets,
                                 const std::vector<std::array<float, 3>> &filament_colors,
                                 const std::vector<int>                  &filament_ids,
                                 const ImageFillUVs                      &uvs)
{
    ImageFillResult res = image_fill_compute(volume.mesh().its, volume.mmu_segmentation_facets.get_data(),
                                             params, assets, filament_colors, filament_ids, uvs);
    if (!res.ok)
        return res;
    // Write through TriangleSelector rather than assigning the bitstream: deserialize() is the
    // fork's own decoder, so this both validates the encoding and stores exactly what
    // serialize() would have produced, which is what makes the 3MF round trip bit-identical.
    {
        TriangleSelector sel(volume.mesh());
        sel.deserialize(res.painting, /*needs_reset=*/true);
        volume.mmu_segmentation_facets.set(sel);
        res.painting = volume.mmu_segmentation_facets.get_data();
    }
    // The annotation rides a key on the ModelConfigObject the volume already owns, so it costs
    // no ObjectID and the 3MF writer already round-trips it.
    ImageFillParams stored = params;
    stored.subdivision = res.subdivision_used;
    volume.config.set_key_value("image_fill_params", new ConfigOptionString(stored.to_string()));
    return res;
}

bool image_fill_params_of(const ModelVolume &volume, ImageFillParams &out)
{
    if (!volume.config.has("image_fill_params"))
        return false;
    const ConfigOption *opt = volume.config.option("image_fill_params");
    if (opt == nullptr)
        return false;
    return ImageFillParams::from_string(opt->serialize(), out);
}

} // namespace Slic3r
