#include "AirbrushField.hpp"
#include "BlueNoise.hpp"

#include "../AABBTreeIndirect.hpp"
#include "../TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace Slic3r {
namespace Airbrush {

// =============================================================================================
// Surface classes and keys
// =============================================================================================

int surface_class(const Vec3d &n)
{
    const double ax = std::abs(n.x()), ay = std::abs(n.y()), az = std::abs(n.z());
    const double m  = std::max(ax, std::max(ay, az));
    if (!(m > 0.))
        return -1;
    // Ties to the lower axis: test X first with >=.
    if (ax >= ay && ax >= az)
        return n.x() >= 0. ? 0 : 1;
    if (ay >= az)
        return n.y() >= 0. ? 2 : 3;
    return n.z() >= 0. ? 4 : 5;
}

int surface_class(const Vec3f &n) { return surface_class(Vec3d(n.cast<double>())); }

static Vec3d class_direction(int cls)
{
    switch (cls) {
    case 0: return Vec3d(1, 0, 0);
    case 1: return Vec3d(-1, 0, 0);
    case 2: return Vec3d(0, 1, 0);
    case 3: return Vec3d(0, -1, 0);
    case 4: return Vec3d(0, 0, 1);
    default: return Vec3d(0, 0, -1);
    }
}

static constexpr uint64_t KEY_FIELD_MASK = (uint64_t(1) << 20) - 1;

uint64_t AirbrushField::make_key(int64_t i, int64_t j, int64_t k, int cls)
{
    return ((uint64_t(i) & KEY_FIELD_MASK) << 43) | ((uint64_t(j) & KEY_FIELD_MASK) << 23) | ((uint64_t(k) & KEY_FIELD_MASK) << 3) |
           (uint64_t(cls) & 7u);
}

static int64_t sign_extend20(uint64_t v)
{
    v &= KEY_FIELD_MASK;
    return (v & (uint64_t(1) << 19)) ? int64_t(v) - int64_t(uint64_t(1) << 20) : int64_t(v);
}

void AirbrushField::split_key(uint64_t key, int64_t &i, int64_t &j, int64_t &k, int &cls)
{
    i   = sign_extend20(key >> 43);
    j   = sign_extend20(key >> 23);
    k   = sign_extend20(key >> 3);
    cls = int(key & 7u);
}

uint64_t AirbrushField::key_of(const Vec3d &p_abs, int cls) const
{
    const double h = double(m_cell);
    const Vec3d  p = p_abs - m_origin;
    return make_key(int64_t(std::floor(p.x() / h)), int64_t(std::floor(p.y() / h)), int64_t(std::floor(p.z() / h)), cls);
}

Vec3d AirbrushField::center_of(uint64_t key) const
{
    int64_t i, j, k;
    int     cls;
    split_key(key, i, j, k, cls);
    const double h = double(m_cell);
    return m_origin + Vec3d((double(i) + 0.5) * h, (double(j) + 0.5) * h, (double(k) + 0.5) * h);
}

// =============================================================================================
// Slots and cells
// =============================================================================================

int AirbrushField::slot_of(int filament_id) const
{
    for (size_t s = 0; s < m_slots.size(); ++s)
        if (m_slots[s] == filament_id)
            return int(s);
    return -1;
}

int AirbrushField::add_slot(int filament_id)
{
    if (filament_id < 1 || filament_id > int(EnforcerBlockerType::ExtruderMax))
        return -1;
    if (const int s = slot_of(filament_id); s >= 0)
        return s;
    if (m_slots.size() >= AIRBRUSH_MAX_SLOTS)
        return -1;
    m_slots.push_back(filament_id);
    return int(m_slots.size()) - 1;
}

void AirbrushField::set_cell(uint64_t key, const Weights &w)
{
    uint32_t s = 0;
    for (uint8_t v : w)
        s += v;
    if (s == 0)
        m_cells.erase(key);
    else
        m_cells[key].w = w;
}

const AirbrushField::Cell *AirbrushField::find(uint64_t key) const
{
    auto it = m_cells.find(key);
    return it == m_cells.end() ? nullptr : &it->second;
}

bool AirbrushField::sample(const Vec3d &p, int cls, Weights &out) const
{
    out.fill(0);
    if (cls < 0 || m_cells.empty())
        return false;
    const uint64_t key = key_of(p, cls);
    if (const Cell *c = find(key)) {
        out = c->w;
        return true;
    }
    int64_t i, j, k;
    int     cc;
    split_key(key, i, j, k, cc);
    static const int nb[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    std::array<uint32_t, AIRBRUSH_MAX_SLOTS> acc {};
    uint32_t                                 n = 0;
    for (const auto &d : nb)
        if (const Cell *c = find(make_key(i + d[0], j + d[1], k + d[2], cls))) {
            for (size_t s = 0; s < AIRBRUSH_MAX_SLOTS; ++s)
                acc[s] += c->w[s];
            ++n;
        }
    if (n == 0)
        return false;
    for (size_t s = 0; s < AIRBRUSH_MAX_SLOTS; ++s)
        out[s] = uint8_t((acc[s] + n / 2) / n);
    return true;
}

// =============================================================================================
// Painting
// =============================================================================================

Weights composite_over(const Weights &start, int slot, float alpha)
{
    alpha = std::clamp(alpha, 0.f, 1.f);
    Weights  out {};
    uint32_t others = 0;
    for (size_t s = 0; s < AIRBRUSH_MAX_SLOTS; ++s) {
        if (int(s) == slot)
            continue;
        out[s] = uint8_t(std::lround(float(start[s]) * (1.f - alpha)));
        others += out[s];
    }
    if (slot >= 0 && slot < int(AIRBRUSH_MAX_SLOTS)) {
        const long v = std::lround(float(start[size_t(slot)]) * (1.f - alpha) + 255.f * alpha);
        out[size_t(slot)] = uint8_t(std::min<long>(v, long(255u - std::min<uint32_t>(others, 255u))));
    }
    return out;
}

float dab_falloff(float d, float radius, float hardness)
{
    if (!(radius > 0.f) || d >= radius)
        return 0.f;
    const float inner = std::clamp(hardness, 0.f, 1.f) * radius;
    if (d <= inner)
        return 1.f;
    const float t = (radius - d) / (radius - inner);   // 0 at the rim, 1 at the inner radius
    return t * t * (3.f - 2.f * t);
}

void AirbrushField::begin_stroke(const StrokeParams &params)
{
    m_stroke_active = true;
    m_stroke        = params;
    m_stroke_dabs   = 0;
    m_stroke_cells.clear();
}

void AirbrushField::end_stroke()
{
    m_stroke_active = false;
    m_stroke_cells.clear();
    m_stroke_dabs = 0;
}

size_t AirbrushField::dab(const AirbrushSurface &surface, const Dab &dab_in, std::vector<uint64_t> *changed_keys)
{
    const bool implicit = !m_stroke_active;
    if (implicit)
        begin_stroke(StrokeParams());
    if (m_stroke.slot >= int(m_slots.size()) || dab_in.radius <= 0.f || dab_in.flow <= 0.f) {
        ++m_stroke_dabs;
        if (implicit)
            end_stroke();
        return 0;
    }

    Dab d = dab_in;
    if (m_stroke.scatter > 0.f) {
        const uint32_t h  = airbrush_hash(m_stroke.seed, m_stroke_dabs, 0x5CA77E2u);
        const uint32_t h2 = airbrush_hash(h, 1u);
        const uint32_t h3 = airbrush_hash(h, 2u);
        auto unit = [](uint32_t v) { return float(v & 0xFFFFFFu) / float(0xFFFFFF) * 2.f - 1.f; };
        d.center += Vec3f(unit(h), unit(h2), unit(h3)) * (m_stroke.scatter * d.radius);
    }
    ++m_stroke_dabs;

    const indexed_triangle_set &its  = surface.its();
    const float                 step = 0.5f * m_cell;
    // The strongest coverage this dab gives each cell.
    std::unordered_map<uint64_t, float> hit;
    auto deposit = [&](const Vec3f &p, int cls) {
        const float a = d.flow * dab_falloff((p - d.center).norm(), d.radius, d.hardness);
        if (a <= 0.f)
            return;
        const uint64_t key = key_of(p.cast<double>(), cls);
        auto           it  = hit.find(key);
        if (it == hit.end())
            hit.emplace(key, a);
        else
            it->second = std::max(it->second, a);
    };

    for (size_t f : surface.facets_in_radius(d.center, d.radius)) {
        const int cls = surface.classes()[f];
        if (cls < 0)
            continue;
        const Vec3f v0 = its.vertices[size_t(its.indices[f][0])];
        const Vec3f v1 = its.vertices[size_t(its.indices[f][1])];
        const Vec3f v2 = its.vertices[size_t(its.indices[f][2])];
        const Vec3f n  = surface.facet_normal(f);
        if (d.normal.squaredNorm() > 0.f && n.dot(d.normal) <= 0.f)
            continue;   // facing away from the brush: the far side of a wall or a slot
        const float dp = (d.center - v0).dot(n);
        if (std::abs(dp) >= d.radius)
            continue;
        // The dab's disk in the facet's plane.
        const Vec3f c2 = d.center - dp * n;
        const float r2 = std::sqrt(d.radius * d.radius - dp * dp);
        Vec3f       e1 = v1 - v0;
        if (e1.squaredNorm() <= 0.f)
            continue;
        e1.normalize();
        const Vec3f e2 = n.cross(e1);
        auto to2 = [&](const Vec3f &p) { return Vec2f((p - v0).dot(e1), (p - v0).dot(e2)); };
        const Vec2f a = to2(v0), b = to2(v1), c = to2(v2), cc = to2(c2);
        // Barycentric inside test with a tolerance of a hundredth of a sample step.
        const float det = (b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y());
        if (std::abs(det) <= 0.f)
            continue;
        const float tol = 0.01f * step;
        auto inside = [&](const Vec2f &q) {
            const float l1 = ((b.x() - q.x()) * (c.y() - q.y()) - (c.x() - q.x()) * (b.y() - q.y())) / det;
            const float l2 = ((c.x() - q.x()) * (a.y() - q.y()) - (a.x() - q.x()) * (c.y() - q.y())) / det;
            const float l3 = 1.f - l1 - l2;
            const float scale = std::max(std::sqrt(std::abs(det)), 1e-6f);
            return l1 * scale >= -tol && l2 * scale >= -tol && l3 * scale >= -tol;
        };
        const float minx = std::max(std::min({ a.x(), b.x(), c.x() }), cc.x() - r2);
        const float maxx = std::min(std::max({ a.x(), b.x(), c.x() }), cc.x() + r2);
        const float miny = std::max(std::min({ a.y(), b.y(), c.y() }), cc.y() - r2);
        const float maxy = std::min(std::max({ a.y(), b.y(), c.y() }), cc.y() + r2);
        if (minx > maxx || miny > maxy)
            continue;
        // A grid aligned to multiples of `step` in this facet's frame, so neighbouring dabs on one
        // facet sample the same points.
        const int i0 = int(std::floor(minx / step)), i1 = int(std::ceil(maxx / step));
        const int j0 = int(std::floor(miny / step)), j1 = int(std::ceil(maxy / step));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const Vec2f q(float(i) * step, float(j) * step);
                if ((q - cc).squaredNorm() > r2 * r2 || !inside(q))
                    continue;
                deposit(v0 + e1 * q.x() + e2 * q.y(), cls);
            }
        // A facet smaller than a sample step may contain no grid point at all.
        deposit((v0 + v1 + v2) / 3.f, cls);
    }

    // Apply in key order, so the result never depends on the hash map's iteration order.
    std::vector<std::pair<uint64_t, float>> ordered(hit.begin(), hit.end());
    std::sort(ordered.begin(), ordered.end());
    size_t changed = 0;
    for (const auto &[key, a] : ordered) {
        auto it = m_stroke_cells.find(key);
        if (it == m_stroke_cells.end()) {
            StrokeCell sc;
            if (const Cell *c = find(key))
                sc.start = c->w;
            it = m_stroke_cells.emplace(key, sc).first;
        }
        StrokeCell &sc = it->second;
        sc.alpha       = sc.alpha + a * (1.f - sc.alpha);
        if (m_stroke.opacity_cap > 0.f)
            sc.alpha = std::min(sc.alpha, m_stroke.opacity_cap);
        const Weights w   = composite_over(sc.start, m_stroke.slot, sc.alpha);
        const Cell   *old = find(key);
        if (old == nullptr ? true : old->w != w) {
            set_cell(key, w);
            ++changed;
            if (changed_keys)
                changed_keys->push_back(key);
        }
    }
    if (implicit)
        end_stroke();
    return changed;
}

// =============================================================================================
// Mesh edits
// =============================================================================================

void AirbrushField::resample(const Transform3d &t)
{
    const Eigen::Matrix3d L  = t.linear();
    const Eigen::Matrix3d Nm = L.inverse().transpose();
    // How far the image of one cell reaches along each world axis, in cells: an old cell is
    // splatted over that many neighbours so a scale-up or an off-axis rotation leaves no holes.
    // Capped, so a huge scale degrades to a sparser field instead of exhausting memory.
    static constexpr int MAX_SPREAD = 8;
    std::array<int, 3>   spread_axis {};
    for (int r = 0; r < 3; ++r) {
        const double reach = 0.5 * (std::abs(L(r, 0)) + std::abs(L(r, 1)) + std::abs(L(r, 2)));
        spread_axis[size_t(r)] = std::clamp(int(std::ceil(reach - 0.5 - 1e-6)), 0, MAX_SPREAD);
    }
    const double h          = double(m_cell);
    const Vec3d  new_origin = t * m_origin;
    struct Acc
    {
        std::array<uint32_t, AIRBRUSH_MAX_SLOTS> sum {};
        uint32_t                                 n = 0;
    };
    std::map<uint64_t, Acc> acc;
    for (const auto &[key, cell] : m_cells) {
        int64_t i, j, k;
        int     cls;
        split_key(key, i, j, k, cls);
        const int ncls = surface_class(Vec3d(Nm * class_direction(cls)));
        if (ncls < 0)
            continue;
        const Vec3d   cell_f = (t * center_of(key) - new_origin) / h;
        const int64_t ci = int64_t(std::floor(cell_f.x())), cj = int64_t(std::floor(cell_f.y())), ck = int64_t(std::floor(cell_f.z()));
        // A surface field: no splat along the normal axis of the new class.
        std::array<int, 3> sp = spread_axis;
        sp[size_t(ncls / 2)]  = 0;
        for (int dk = -sp[2]; dk <= sp[2]; ++dk)
            for (int dj = -sp[1]; dj <= sp[1]; ++dj)
                for (int di = -sp[0]; di <= sp[0]; ++di) {
                    Acc &a = acc[make_key(ci + di, cj + dj, ck + dk, ncls)];
                    for (size_t sl = 0; sl < AIRBRUSH_MAX_SLOTS; ++sl)
                        a.sum[sl] += cell.w[sl];
                    ++a.n;
                }
    }
    m_cells.clear();
    m_origin = new_origin;
    for (const auto &[key, a] : acc) {
        Cell     c;
        uint32_t tot = 0;
        for (size_t sl = 0; sl < AIRBRUSH_MAX_SLOTS; ++sl) {
            c.w[sl] = uint8_t((a.sum[sl] + a.n / 2) / a.n);
            tot += c.w[sl];
        }
        if (tot > 0)
            m_cells.emplace(key, c);
    }
}

void AirbrushField::transform(const Transform3d &t)
{
    if (t.linear().isIdentity(1e-12))
        m_origin += t.translation();   // a move: exact, nothing to resample
    else
        resample(t);
    if (m_anchor.defined)
        m_anchor = m_anchor.transformed(t);
    m_record.clear();
}

void AirbrushField::set_anchor(const indexed_triangle_set &its)
{
    m_anchor = BoundingBoxf3();
    for (const stl_vertex &v : its.vertices)
        m_anchor.merge(v.cast<double>());
}

Vec3d AirbrushField::attach_to_mesh(const indexed_triangle_set &its)
{
    BoundingBoxf3 now;
    for (const stl_vertex &v : its.vertices)
        now.merge(v.cast<double>());
    if (!now.defined)
        return Vec3d::Zero();
    if (!m_anchor.defined) {
        m_anchor = now;
        return Vec3d::Zero();
    }
    const Vec3d  d0  = now.size() - m_anchor.size();
    const double tol = 1e-4 * std::max(1., now.size().maxCoeff());
    const Vec3d  sh  = now.min - m_anchor.min;
    if (d0.cwiseAbs().maxCoeff() <= tol && sh.cwiseAbs().maxCoeff() > tol) {
        // Same mesh, moved: shift the field and keep the bake record (triangle indices still hold).
        m_origin += sh;
        m_anchor = now;
        return sh;
    }
    m_anchor = now;
    return Vec3d::Zero();
}

// =============================================================================================
// Persistence
// =============================================================================================

namespace {

static const char     BLOB_MAGIC[4]   = { 'E', 'A', 'B', 'F' };
static const uint16_t BLOB_MAJOR      = 1;
static const uint16_t BLOB_MINOR      = 0;

struct Writer
{
    std::string out;
    template<class T> void pod(const T &v)
    {
        const size_t n = out.size();
        out.resize(n + sizeof(T));
        std::memcpy(&out[n], &v, sizeof(T));
    }
    void varint(uint64_t v)
    {
        while (v >= 0x80) {
            out.push_back(char(uint8_t(v) | 0x80));
            v >>= 7;
        }
        out.push_back(char(uint8_t(v)));
    }
    void str(const std::string &s)
    {
        pod(uint32_t(s.size()));
        out += s;
    }
};

struct Reader
{
    const std::string &in;
    size_t             pos = 0;
    bool               ok  = true;
    explicit Reader(const std::string &s) : in(s) {}
    template<class T> T pod()
    {
        T v {};
        if (!ok || pos + sizeof(T) > in.size()) {
            ok = false;
            return v;
        }
        std::memcpy(&v, in.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    uint64_t varint()
    {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (!ok || pos >= in.size()) {
                ok = false;
                return 0;
            }
            const uint8_t b = uint8_t(in[pos++]);
            v |= uint64_t(b & 0x7F) << shift;
            if (!(b & 0x80))
                return v;
        }
        ok = false;
        return 0;
    }
    std::string str()
    {
        const uint32_t n = pod<uint32_t>();
        if (!ok || pos + n > in.size()) {
            ok = false;
            return {};
        }
        std::string s = in.substr(pos, n);
        pos += n;
        return s;
    }
    size_t left() const { return in.size() - pos; }
};

} // namespace

std::string AirbrushField::to_blob(const BoundingBoxf3 *anchor_override) const
{
    const BoundingBoxf3 &anchor = anchor_override ? *anchor_override : m_anchor;
    Writer w;
    w.out.append(BLOB_MAGIC, 4);
    w.pod(BLOB_MAJOR);
    w.pod(BLOB_MINOR);
    w.pod(m_cell);
    for (int i = 0; i < 3; ++i)
        w.pod(m_origin[i]);
    w.pod(uint8_t(m_slots.size()));
    for (int s : m_slots)
        w.pod(int32_t(s));
    w.pod(uint8_t(anchor.defined ? 1 : 0));
    for (int i = 0; i < 3; ++i)
        w.pod(anchor.defined ? anchor.min[i] : 0.);
    for (int i = 0; i < 3; ++i)
        w.pod(anchor.defined ? anchor.max[i] : 0.);
    w.str(m_bake_params);
    // The bake record.
    w.pod(m_record.facets);
    w.pod(m_record.paint_hash);
    w.pod(uint32_t(m_record.owned.size()));
    int prev = -1;
    for (int t : m_record.owned) {
        w.varint(uint64_t(t - prev));
        prev = t;
    }
    w.pod(uint32_t(m_record.under.triangles_to_split.size()));
    for (const auto &m : m_record.under.triangles_to_split) {
        w.pod(int32_t(m.triangle_idx));
        w.pod(int32_t(m.bitstream_start_idx));
    }
    w.pod(uint32_t(m_record.under.bitstream.size()));
    {
        uint8_t byte = 0;
        int     nb   = 0;
        for (bool b : m_record.under.bitstream) {
            byte |= uint8_t(b ? 1 : 0) << nb;
            if (++nb == 8) {
                w.out.push_back(char(byte));
                byte = 0;
                nb   = 0;
            }
        }
        if (nb > 0)
            w.out.push_back(char(byte));
    }
    // The cells, in key order (so the blob is deterministic), delta-coded.
    std::vector<uint64_t> keys;
    keys.reserve(m_cells.size());
    for (const auto &kv : m_cells)
        keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    w.pod(uint32_t(keys.size()));
    uint64_t pk = 0;
    for (uint64_t k : keys) {
        w.varint(k - pk);
        pk = k;
        const Cell &c = m_cells.at(k);
        for (size_t s = 0; s < m_slots.size(); ++s)
            w.out.push_back(char(c.w[s]));
    }
    return w.out;
}

std::shared_ptr<AirbrushField> AirbrushField::from_blob(const std::string &blob)
{
    if (blob.size() < 8 || std::memcmp(blob.data(), BLOB_MAGIC, 4) != 0)
        return nullptr;
    Reader r(blob);
    r.pos              = 4;
    const uint16_t maj = r.pod<uint16_t>();
    (void) r.pod<uint16_t>();   // minor: additive changes only
    if (!r.ok || maj != BLOB_MAJOR)
        return nullptr;
    auto f    = std::make_shared<AirbrushField>();
    f->m_cell = r.pod<float>();
    if (!r.ok || !(f->m_cell > 0.001f) || !(f->m_cell < 100.f))
        return nullptr;
    for (int i = 0; i < 3; ++i)
        f->m_origin[i] = r.pod<double>();
    if (!r.ok || !f->m_origin.allFinite())
        return nullptr;
    const uint8_t ns = r.pod<uint8_t>();
    if (ns > AIRBRUSH_MAX_SLOTS)
        return nullptr;
    for (uint8_t s = 0; s < ns; ++s) {
        const int32_t id = r.pod<int32_t>();
        if (id < 1 || id > int(EnforcerBlockerType::ExtruderMax))
            return nullptr;
        f->m_slots.push_back(id);
    }
    const uint8_t has_anchor = r.pod<uint8_t>();
    Vec3d         mn, mx;
    for (int i = 0; i < 3; ++i)
        mn[i] = r.pod<double>();
    for (int i = 0; i < 3; ++i)
        mx[i] = r.pod<double>();
    if (has_anchor && r.ok && mn.allFinite() && mx.allFinite())
        f->m_anchor = BoundingBoxf3(mn, mx);
    f->m_bake_params = r.str();
    // The bake record.
    f->m_record.facets     = r.pod<uint32_t>();
    f->m_record.paint_hash = r.pod<uint64_t>();
    const uint32_t nowned  = r.pod<uint32_t>();
    if (!r.ok || nowned > r.left() || nowned > f->m_record.facets)
        return nullptr;
    int64_t prev = -1;
    for (uint32_t i = 0; i < nowned; ++i) {
        const uint64_t d = r.varint();
        if (!r.ok || d == 0 || d > uint64_t(f->m_record.facets) || prev + int64_t(d) >= int64_t(f->m_record.facets))
            return nullptr;
        prev += int64_t(d);
        f->m_record.owned.push_back(int(prev));
    }
    const uint32_t ntts = r.pod<uint32_t>();
    if (!r.ok || uint64_t(ntts) * 8 > r.left())
        return nullptr;
    for (uint32_t i = 0; i < ntts; ++i) {
        const int32_t tri   = r.pod<int32_t>();
        const int32_t start = r.pod<int32_t>();
        f->m_record.under.triangles_to_split.emplace_back(tri, start);
    }
    const uint32_t nbits = r.pod<uint32_t>();
    if (!r.ok || (uint64_t(nbits) + 7) / 8 > r.left())
        return nullptr;
    f->m_record.under.bitstream.reserve(nbits);
    for (uint32_t i = 0; i < nbits; i += 8) {
        const uint8_t byte = r.pod<uint8_t>();
        for (uint32_t b = 0; b < 8 && i + b < nbits; ++b)
            f->m_record.under.bitstream.push_back((byte >> b) & 1);
    }
    // Validate the under-paint's map before anything trusts it.
    {
        int last_tri = -1;
        for (const auto &m : f->m_record.under.triangles_to_split) {
            if (m.triangle_idx <= last_tri || uint32_t(m.triangle_idx) >= f->m_record.facets || m.bitstream_start_idx < 0 || size_t(m.bitstream_start_idx) > f->m_record.under.bitstream.size())
                return nullptr;
            last_tri = m.triangle_idx;
        }
        f->m_record.under.reset_used_states();
        if (!f->m_record.under.triangles_to_split.empty() && !f->m_record.under.update_used_states(0))
            return nullptr;
    }
    const uint32_t ncells = r.pod<uint32_t>();
    if (!r.ok || uint64_t(ncells) * (1 + ns) > r.left())
        return nullptr;
    uint64_t pk = 0;
    for (uint32_t i = 0; i < ncells; ++i) {
        const uint64_t d = r.varint();
        if (!r.ok || (i > 0 && d == 0))
            return nullptr;
        pk += d;
        Cell c;
        for (uint8_t s = 0; s < ns; ++s)
            c.w[s] = r.pod<uint8_t>();
        if (!r.ok)
            return nullptr;
        if (c.sum() > 255 || (pk & 7u) > 5)
            return nullptr;
        if (c.sum() > 0)
            f->m_cells.emplace(pk, c);
    }
    return r.ok ? f : nullptr;
}

bool AirbrushField::operator==(const AirbrushField &rhs) const
{
    if (m_cell != rhs.m_cell || m_origin != rhs.m_origin || m_slots != rhs.m_slots || m_bake_params != rhs.m_bake_params)
        return false;
    if (m_anchor.defined != rhs.m_anchor.defined || (m_anchor.defined && (m_anchor.min != rhs.m_anchor.min || m_anchor.max != rhs.m_anchor.max)))
        return false;
    if (m_record.owned != rhs.m_record.owned || m_record.paint_hash != rhs.m_record.paint_hash || m_record.facets != rhs.m_record.facets ||
        !(m_record.under == rhs.m_record.under))
        return false;
    if (m_cells.size() != rhs.m_cells.size())
        return false;
    for (const auto &[k, c] : m_cells) {
        const Cell *o = rhs.find(k);
        if (o == nullptr || o->w != c.w)
            return false;
    }
    return true;
}

// =============================================================================================
// The surface helper
// =============================================================================================

struct AirbrushSurface::Tree
{
    AABBTreeIndirect::Tree3f tree;
};

AirbrushSurface::AirbrushSurface(const indexed_triangle_set &its) : m_its(its), m_tree(std::make_unique<Tree>())
{
    m_classes.resize(its.indices.size());
    for (size_t f = 0; f < its.indices.size(); ++f)
        m_classes[f] = surface_class(its_face_normal(its, int(f)));
    m_tree->tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(its.vertices, its.indices);
}

AirbrushSurface::~AirbrushSurface() = default;

std::vector<size_t> AirbrushSurface::facets_in_radius(const Vec3f &center, float radius) const
{
    std::vector<size_t> out =
        AABBTreeIndirect::all_triangles_in_radius(m_its.vertices, m_its.indices, m_tree->tree, center, radius * radius);
    std::sort(out.begin(), out.end());
    return out;
}

Vec3f AirbrushSurface::facet_normal(size_t idx) const { return its_face_normal(m_its, int(idx)); }

// =============================================================================================

uint64_t paint_hash(const TriangleSelector::TriangleSplittingData &data)
{
    uint64_t h   = 1469598103934665603ull;
    auto     mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    mix(data.triangles_to_split.size());
    for (const auto &m : data.triangles_to_split) {
        mix(uint64_t(uint32_t(m.triangle_idx)));
        mix(uint64_t(uint32_t(m.bitstream_start_idx)));
    }
    mix(data.bitstream.size());
    uint64_t word = 0;
    int      n    = 0;
    for (bool b : data.bitstream) {
        word |= uint64_t(b ? 1 : 0) << n;
        if (++n == 64) {
            mix(word);
            word = 0;
            n    = 0;
        }
    }
    if (n > 0)
        mix(word);
    return h;
}

} // namespace Airbrush
} // namespace Slic3r
