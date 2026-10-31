#include "AirbrushProcedural.hpp"
#include "BlueNoise.hpp"

#include "../AABBTreeIndirect.hpp"
#include "../TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <queue>
#include <sstream>

namespace Slic3r {
namespace Airbrush {

static constexpr float TWO_PI = 6.28318530717958647692f;

const char *fill_kind_name(FillKind kind)
{
    switch (kind) {
    case FillKind::Linear: return "linear";
    case FillKind::Radial: return "radial";
    case FillKind::AlongZ: return "z";
    case FillKind::Noise: return "noise";
    case FillKind::Speckle: return "speckle";
    case FillKind::Camo: return "camo";
    case FillKind::Marble: return "marble";
    default: return "?";
    }
}

static bool fill_kind_from_name(const std::string &s, FillKind &out)
{
    for (int k = 0; k < int(FillKind::Count); ++k)
        if (s == fill_kind_name(FillKind(k))) {
            out = FillKind(k);
            return true;
        }
    return false;
}

// =============================================================================================
// Recipe
// =============================================================================================

bool AirbrushFillRecipe::sanitize()
{
    std::vector<GradientStop> s;
    for (GradientStop st : stops) {
        if (st.filament < 0 || st.filament > int(EnforcerBlockerType::ExtruderMax) || !std::isfinite(st.pos))
            continue;
        st.pos = std::clamp(st.pos, 0.f, 1.f);
        s.push_back(st);
    }
    std::stable_sort(s.begin(), s.end(), [](const GradientStop &a, const GradientStop &b) { return a.pos < b.pos; });
    if (s.size() > AIRBRUSH_MAX_STOPS)
        s.resize(AIRBRUSH_MAX_STOPS);
    stops = std::move(s);
    auto fin = [](float v, float lo, float hi, float def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; };
    scale_mm = fin(scale_mm, 0.2f, 500.f, 8.f);
    contrast = fin(contrast, 0.f, 8.f, 1.f);
    amount   = fin(amount, 0.f, 1.f, 0.3f);
    opacity  = fin(opacity, 0.f, 1.f, 1.f);
    octaves  = std::clamp(octaves, 1, 8);
    if (!start.allFinite())
        start = Vec3f::Zero();
    if (!end.allFinite())
        end = Vec3f::Zero();
    if (!up.allFinite() || up.squaredNorm() <= 0.f)
        up = Vec3f::UnitZ();
    return stops.size() >= 2;
}

std::string AirbrushFillRecipe::to_string() const
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << "kind=" << fill_kind_name(kind) << ";stops=";
    for (size_t i = 0; i < stops.size(); ++i)
        ss << (i ? "," : "") << stops[i].filament << "@" << stops[i].pos;
    auto v3 = [&ss](const char *k, const Vec3f &v) { ss << ";" << k << "=" << v.x() << "," << v.y() << "," << v.z(); };
    v3("start", start);
    v3("end", end);
    v3("up", up);
    ss << ";scale=" << scale_mm << ";contrast=" << contrast << ";octaves=" << octaves << ";amount=" << amount << ";seed=" << seed
       << ";opacity=" << opacity;
    return ss.str();
}

bool AirbrushFillRecipe::from_string(const std::string &s, AirbrushFillRecipe &out)
{
    AirbrushFillRecipe r;
    std::istringstream ss(s);
    ss.imbue(std::locale::classic());
    std::string item;
    bool        have_kind = false;
    auto read_v3 = [](const std::string &v, Vec3f &o) {
        std::istringstream vs(v);
        vs.imbue(std::locale::classic());
        char c1 = 0, c2 = 0;
        float x = 0, y = 0, z = 0;
        if (!(vs >> x >> c1 >> y >> c2 >> z) || c1 != ',' || c2 != ',')
            return false;
        o = Vec3f(x, y, z);
        return true;
    };
    while (std::getline(ss, item, ';')) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
        std::istringstream vs(v);
        vs.imbue(std::locale::classic());
        bool ok = true;
        if (k == "kind")
            ok = have_kind = fill_kind_from_name(v, r.kind);
        else if (k == "stops") {
            std::istringstream ls(v);
            std::string        stop;
            while (ok && std::getline(ls, stop, ',')) {
                const size_t at = stop.find('@');
                if (at == std::string::npos) {
                    ok = false;
                    break;
                }
                std::istringstream fs(stop.substr(0, at)), ps(stop.substr(at + 1));
                fs.imbue(std::locale::classic());
                ps.imbue(std::locale::classic());
                GradientStop st;
                ok = bool(fs >> st.filament) && bool(ps >> st.pos);
                if (ok)
                    r.stops.push_back(st);
            }
        } else if (k == "start") ok = read_v3(v, r.start);
        else if (k == "end") ok = read_v3(v, r.end);
        else if (k == "up") ok = read_v3(v, r.up);
        else if (k == "scale") ok = bool(vs >> r.scale_mm);
        else if (k == "contrast") ok = bool(vs >> r.contrast);
        else if (k == "octaves") ok = bool(vs >> r.octaves);
        else if (k == "amount") ok = bool(vs >> r.amount);
        else if (k == "seed") { unsigned long sd = 0; ok = bool(vs >> sd); r.seed = uint32_t(sd); }
        else if (k == "opacity") ok = bool(vs >> r.opacity);
        if (!ok)
            return false;
    }
    if (!have_kind || !r.sanitize())
        return false;
    out = std::move(r);
    return true;
}

std::vector<GradientStop> two_stops(int a, int b) { return { GradientStop{ a, 0.f }, GradientStop{ b, 1.f } }; }

std::vector<GradientStop> even_stops(const std::vector<int> &filaments)
{
    std::vector<GradientStop> out;
    const size_t n = std::min(filaments.size(), AIRBRUSH_MAX_STOPS);
    for (size_t i = 0; i < n; ++i)
        out.push_back({ filaments[i], n > 1 ? float(i) / float(n - 1) : 0.f });
    return out;
}

// =============================================================================================
// Noise
// =============================================================================================

static inline float lattice(int32_t x, int32_t y, int32_t z, uint32_t seed)
{
    const uint32_t h = airbrush_hash(uint32_t(x), uint32_t(y), airbrush_hash(uint32_t(z), seed, 0x9E3779B9u));
    return float(h & 0xFFFFFFu) / float(0x1000000);
}

static inline float fade(float t) { return t * t * (3.f - 2.f * t); }

float value_noise(const Vec3f &p, uint32_t seed)
{
    const float   fx = std::floor(p.x()), fy = std::floor(p.y()), fz = std::floor(p.z());
    const int32_t x = int32_t(fx), y = int32_t(fy), z = int32_t(fz);
    const float   u = fade(p.x() - fx), v = fade(p.y() - fy), w = fade(p.z() - fz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = lerp(lattice(x, y, z, seed), lattice(x + 1, y, z, seed), u);
    const float x10 = lerp(lattice(x, y + 1, z, seed), lattice(x + 1, y + 1, z, seed), u);
    const float x01 = lerp(lattice(x, y, z + 1, seed), lattice(x + 1, y, z + 1, seed), u);
    const float x11 = lerp(lattice(x, y + 1, z + 1, seed), lattice(x + 1, y + 1, z + 1, seed), u);
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

float fbm_noise(const Vec3f &p, int octaves, uint32_t seed)
{
    octaves     = std::clamp(octaves, 1, 8);
    float sum   = 0.f, amp = 1.f, norm = 0.f;
    Vec3f q     = p;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * value_noise(q, airbrush_hash(seed, uint32_t(o), 0xF00Du));
        norm += amp;
        amp *= 0.5f;
        // Twice the frequency, and a fixed offset so the octaves' lattices do not line up.
        q = q * 2.f + Vec3f(17.13f, 31.71f, 7.37f);
    }
    return sum / norm;
}

float worley_f1(const Vec3f &p, uint32_t seed)
{
    const int32_t x = int32_t(std::floor(p.x())), y = int32_t(std::floor(p.y())), z = int32_t(std::floor(p.z()));
    float         best = std::numeric_limits<float>::max();
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int32_t cx = x + dx, cy = y + dy, cz = z + dz;
                const Vec3f   fp(float(cx) + lattice(cx, cy, cz, seed), float(cy) + lattice(cx, cy, cz, seed ^ 0x51EDu),
                                 float(cz) + lattice(cx, cy, cz, seed ^ 0xA5A5u));
                best = std::min(best, (fp - p).squaredNorm());
            }
    return std::sqrt(best);
}

float pattern_value(const AirbrushFillRecipe &r, const Vec3f &p_mesh)
{
    const float scale = std::max(r.scale_mm, 1e-3f);
    const Vec3f p     = p_mesh / scale;
    switch (r.kind) {
    case FillKind::Noise: {
        const float n = fbm_noise(p, r.octaves, r.seed);
        return std::clamp(0.5f + r.contrast * 2.f * (n - 0.5f), 0.f, 1.f);
    }
    case FillKind::Speckle: {
        // One spot (a ball of radius rad) per unit cell. A surface cuts the balls in disks that cover
        // about 4/3 pi rad^3 of it before overlaps, so rad = cbrt(3 amount / (4 pi)).
        const float rad  = std::clamp(std::cbrt(3.f * std::max(r.amount, 0.f) / (4.f * 3.14159265f)), 0.f, 0.62f);
        if (!(rad > 0.f))
            return 0.f;
        const float d    = worley_f1(p, r.seed);
        const float soft = 0.15f * rad / std::max(r.contrast, 0.1f);
        if (d <= rad - soft)
            return 1.f;
        if (d >= rad)
            return 0.f;
        return fade((rad - d) / soft);
    }
    case FillKind::Camo: {
        // Low frequency, stretched so the hard bands share the surface about evenly.
        const float n = fbm_noise(p, std::min(r.octaves, 3), r.seed);
        return std::clamp(0.5f + 2.2f * std::max(r.contrast, 0.1f) * (n - 0.5f), 0.f, 1.f);
    }
    case FillKind::Marble: {
        Vec3f up = r.up;
        if (up.squaredNorm() > 0.f)
            up.normalize();
        const float h     = p.dot(up);
        const float n     = fbm_noise(p, r.octaves, r.seed);
        const float phase = TWO_PI * h + r.contrast * TWO_PI * n;
        return 0.5f + 0.5f * std::sin(phase);
    }
    default: return 0.f;
    }
}

std::vector<float> ramp_weights(const std::vector<GradientStop> &stops, float t, bool hard)
{
    std::vector<float> w(stops.size(), 0.f);
    if (stops.empty())
        return w;
    if (!std::isfinite(t))
        t = 0.f;
    if (hard) {
        // One band per stop over 0..1.
        const size_t n = stops.size();
        w[std::min(n - 1, size_t(std::max(0.f, t) * float(n)))] = 1.f;
        return w;
    }
    if (t <= stops.front().pos) {
        w.front() = 1.f;
        return w;
    }
    if (t >= stops.back().pos) {
        w.back() = 1.f;
        return w;
    }
    for (size_t i = 0; i + 1 < stops.size(); ++i)
        if (t <= stops[i + 1].pos) {
            const float span = stops[i + 1].pos - stops[i].pos;
            const float u    = span > 0.f ? (t - stops[i].pos) / span : 1.f;
            w[i]             = 1.f - u;
            w[i + 1]         = u;
            break;
        }
    return w;
}

// =============================================================================================
// Surface distance
// =============================================================================================

std::vector<float> geodesic_distances(const indexed_triangle_set &its, const Vec3f &seed, std::vector<char> *seed_patch_out)
{
    const float        INF = std::numeric_limits<float>::infinity();
    std::vector<float> dist(its.vertices.size(), INF);
    std::vector<char>  patch(its.indices.size(), 0);
    if (its.indices.empty()) {
        if (seed_patch_out)
            *seed_patch_out = std::move(patch);
        return dist;
    }
    // The facet under the seed.
    size_t seed_facet = 0;
    {
        const AABBTreeIndirect::Tree3f tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(its.vertices, its.indices);
        Vec3f                          hit;
        if (AABBTreeIndirect::squared_distance_to_indexed_triangle_set(its.vertices, its.indices, tree, seed, seed_facet, hit) < 0.f)
            seed_facet = 0;
    }
    // The flat patch round it: straight-line distances are exact there.
    const std::vector<Vec3i32> nb = its_face_neighbors(its);
    const Vec3f                n0 = its_face_normal(its, int(seed_facet));
    const Vec3f                p0 = its.vertices[size_t(its.indices[seed_facet][0])];
    std::vector<size_t>        stack { seed_facet };
    patch[seed_facet] = 1;
    while (!stack.empty()) {
        const size_t f = stack.back();
        stack.pop_back();
        for (int e = 0; e < 3; ++e) {
            const int g = nb[f][e];
            if (g < 0 || patch[size_t(g)])
                continue;
            const Vec3f ng = its_face_normal(its, g);
            if (ng.dot(n0) < 0.99999f)
                continue;
            bool coplanar = true;
            for (int k = 0; k < 3 && coplanar; ++k)
                coplanar = std::abs((its.vertices[size_t(its.indices[size_t(g)][k])] - p0).dot(n0)) < 1e-4f;
            if (!coplanar)
                continue;
            patch[size_t(g)] = 1;
            stack.push_back(size_t(g));
        }
    }
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
    for (size_t f = 0; f < its.indices.size(); ++f)
        if (patch[f])
            for (int k = 0; k < 3; ++k) {
                const int   v = its.indices[f][k];
                const float d = (its.vertices[size_t(v)] - seed).norm();
                if (d < dist[size_t(v)]) {
                    dist[size_t(v)] = d;
                    q.emplace(d, v);
                }
            }
    // Vertex -> facets, for the edge relaxation.
    std::vector<std::vector<int>> vf(its.vertices.size());
    for (size_t f = 0; f < its.indices.size(); ++f)
        for (int k = 0; k < 3; ++k)
            vf[size_t(its.indices[f][k])].push_back(int(f));
    while (!q.empty()) {
        const auto [d, v] = q.top();
        q.pop();
        if (d > dist[size_t(v)])
            continue;
        for (int f : vf[size_t(v)])
            for (int k = 0; k < 3; ++k) {
                const int w = its.indices[size_t(f)][k];
                if (w == v)
                    continue;
                const float nd = d + (its.vertices[size_t(w)] - its.vertices[size_t(v)]).norm();
                if (nd < dist[size_t(w)]) {
                    dist[size_t(w)] = nd;
                    q.emplace(nd, w);
                }
            }
    }
    if (seed_patch_out)
        *seed_patch_out = std::move(patch);
    return dist;
}

float geodesic_at(const indexed_triangle_set &its, const std::vector<float> &vertex_dist, const std::vector<char> &seed_patch,
                  const Vec3f &seed, size_t facet, const Vec3f &p)
{
    if (facet < seed_patch.size() && seed_patch[facet])
        return (p - seed).norm();
    float best = std::numeric_limits<float>::infinity();
    if (facet >= its.indices.size())
        return best;
    for (int k = 0; k < 3; ++k) {
        const int v = its.indices[facet][k];
        if (size_t(v) < vertex_dist.size())
            best = std::min(best, vertex_dist[size_t(v)] + (p - its.vertices[size_t(v)]).norm());
    }
    return best;
}

// =============================================================================================
// Applying a recipe
// =============================================================================================

bool make_fill_function(AirbrushField &field, const indexed_triangle_set &its, const AirbrushFillRecipe &recipe_in,
                        AirbrushField::FillFunction &out, std::string *error)
{
    auto fail = [error](const char *why) {
        if (error)
            *error = why;
        return false;
    };
    AirbrushFillRecipe recipe = recipe_in;
    if (!recipe.sanitize())
        return fail("a gradient needs at least two stops");
    // Each stop's slot (-1: the part's own colour).
    std::vector<int> slot_of_stop;
    for (const GradientStop &st : recipe.stops) {
        if (st.filament == 0) {
            slot_of_stop.push_back(-1);
            continue;
        }
        const int s = field.add_slot(st.filament);
        if (s < 0)
            return fail("the field already holds the most filaments it can");
        slot_of_stop.push_back(s);
    }
    const bool hard = recipe.kind == FillKind::Camo;
    auto       to_cov = [slot_of_stop, stops = recipe.stops, hard](float t, AirbrushField::Coverage &cov) {
        const std::vector<float> w = ramp_weights(stops, t, hard);
        for (size_t i = 0; i < w.size(); ++i)
            if (slot_of_stop[i] >= 0 && w[i] > 0.f)
                cov[size_t(slot_of_stop[i])] += w[i];
    };

    switch (recipe.kind) {
    case FillKind::Linear: {
        const Vec3f d  = recipe.end - recipe.start;
        const float l2 = d.squaredNorm();
        if (!(l2 > 1e-6f))
            return fail("drag across the part to set the gradient's direction");
        const Vec3f s = recipe.start;
        out = [to_cov, s, d, l2](const Vec3f &p, size_t, int, AirbrushField::Coverage &cov) { to_cov((p - s).dot(d) / l2, cov); };
        return true;
    }
    case FillKind::AlongZ: {
        Vec3f up = recipe.up;
        if (up.squaredNorm() <= 0.f)
            up = Vec3f::UnitZ();
        float h0 = recipe.start.dot(up), h1 = recipe.end.dot(up);
        if (!(std::abs(h1 - h0) > 1e-3f)) {
            // A click without a drag: the part's whole height.
            h0 = std::numeric_limits<float>::max();
            h1 = -std::numeric_limits<float>::max();
            for (const stl_vertex &v : its.vertices) {
                h0 = std::min(h0, v.dot(up));
                h1 = std::max(h1, v.dot(up));
            }
            if (!(h1 > h0))
                return fail("the part has no height");
        }
        out = [to_cov, up, h0, h1](const Vec3f &p, size_t, int, AirbrushField::Coverage &cov) { to_cov((p.dot(up) - h0) / (h1 - h0), cov); };
        return true;
    }
    case FillKind::Radial: {
        const float radius = (recipe.end - recipe.start).norm();
        if (!(radius > 1e-3f))
            return fail("drag out from the centre to set the radius");
        auto patch = std::make_shared<std::vector<char>>();
        auto dist  = std::make_shared<const std::vector<float>>(geodesic_distances(its, recipe.start, patch.get()));
        const Vec3f seed = recipe.start;
        const indexed_triangle_set *pits = &its;
        out = [to_cov, dist, patch, seed, radius, pits](const Vec3f &p, size_t facet, int, AirbrushField::Coverage &cov) {
            const float d = geodesic_at(*pits, *dist, *patch, seed, facet, p);
            to_cov(std::isfinite(d) ? d / radius : 1.f, cov);
        };
        return true;
    }
    case FillKind::Noise:
    case FillKind::Speckle:
    case FillKind::Camo:
    case FillKind::Marble:
        out = [to_cov, recipe](const Vec3f &p, size_t, int, AirbrushField::Coverage &cov) { to_cov(pattern_value(recipe, p), cov); };
        return true;
    default: return fail("unknown fill");
    }
}

long apply_fill(AirbrushField &field, const AirbrushSurface &surface, const AirbrushFillRecipe &recipe, std::vector<uint64_t> *changed_keys,
                std::string *error)
{
    AirbrushField::FillFunction fn;
    if (!make_fill_function(field, surface.its(), recipe, fn, error))
        return -1;
    AirbrushFillRecipe stored = recipe;
    stored.sanitize();
    const size_t changed = field.fill(surface, fn, stored.opacity, changed_keys);
    field.set_fill_recipe(stored.to_string());
    return long(changed);
}

} // namespace Airbrush
} // namespace Slic3r
