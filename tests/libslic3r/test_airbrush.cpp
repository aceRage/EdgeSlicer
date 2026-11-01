// Airbrush P1 (tests/design_airbrush_paint.md, section 5): the colour field, the brush stroke API,
// the bake to paint_color, 3MF persistence, mesh edits, the cost estimate, and the top-skin pitting
// fix in MultiMaterialSegmentation.cpp (P0 photo 03).
#include <catch2/catch.hpp>

#include "libslic3r/Airbrush/AirbrushBake.hpp"
#include "libslic3r/Airbrush/AirbrushField.hpp"
#include "libslic3r/Airbrush/BlueNoise.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ImageFill.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Airbrush;
namespace fs = boost::filesystem;
using Catch::Matchers::WithinAbs;

namespace {

// ---------------------------------------------------------------------------------------------
// Meshes: axis-aligned boxes whose six faces are regular grids of `step`, vertices shared, closed.
// ---------------------------------------------------------------------------------------------
struct GridBox
{
    indexed_triangle_set                    its;
    std::map<std::array<long long, 3>, int> vmap;

    int vert(const Vec3d &p)
    {
        const std::array<long long, 3> k = { std::llround(p.x() * 1e4), std::llround(p.y() * 1e4), std::llround(p.z() * 1e4) };
        auto                           it = vmap.find(k);
        if (it != vmap.end())
            return it->second;
        its.vertices.emplace_back(p.cast<float>());
        const int idx = int(its.vertices.size()) - 1;
        vmap.emplace(k, idx);
        return idx;
    }
    void tri(int a, int b, int c, const Vec3d &outward)
    {
        const Vec3d pa = its.vertices[a].cast<double>(), pb = its.vertices[b].cast<double>(), pc = its.vertices[c].cast<double>();
        if ((pb - pa).cross(pc - pa).dot(outward) < 0.)
            std::swap(b, c);
        its.indices.emplace_back(a, b, c);
    }
    void grid(const Vec3d &O, const Vec3d &U, const Vec3d &V, int nu, int nv, const Vec3d &outward)
    {
        nu = std::max(nu, 1);
        nv = std::max(nv, 1);
        for (int j = 0; j < nv; ++j)
            for (int i = 0; i < nu; ++i) {
                const int a = vert(O + U * (double(i) / nu) + V * (double(j) / nv));
                const int b = vert(O + U * (double(i + 1) / nu) + V * (double(j) / nv));
                const int c = vert(O + U * (double(i + 1) / nu) + V * (double(j + 1) / nv));
                const int d = vert(O + U * (double(i) / nu) + V * (double(j + 1) / nv));
                tri(a, b, c, outward);
                tri(a, c, d, outward);
            }
    }
};

indexed_triangle_set grid_box(double sx, double sy, double sz, double step)
{
    GridBox   m;
    const int nx = int(std::lround(sx / step)), ny = int(std::lround(sy / step)), nz = int(std::lround(sz / step));
    m.grid(Vec3d(0, 0, sz), Vec3d(sx, 0, 0), Vec3d(0, sy, 0), nx, ny, Vec3d(0, 0, 1));
    m.grid(Vec3d(0, 0, 0), Vec3d(sx, 0, 0), Vec3d(0, sy, 0), nx, ny, Vec3d(0, 0, -1));
    m.grid(Vec3d(0, 0, 0), Vec3d(sx, 0, 0), Vec3d(0, 0, sz), nx, nz, Vec3d(0, -1, 0));
    m.grid(Vec3d(0, sy, 0), Vec3d(sx, 0, 0), Vec3d(0, 0, sz), nx, nz, Vec3d(0, 1, 0));
    m.grid(Vec3d(0, 0, 0), Vec3d(0, sy, 0), Vec3d(0, 0, sz), ny, nz, Vec3d(-1, 0, 0));
    m.grid(Vec3d(sx, 0, 0), Vec3d(0, sy, 0), Vec3d(0, 0, sz), ny, nz, Vec3d(1, 0, 0));
    return m.its;
}

// One huge dab: every cell of the surface gets exactly `coverage` of `slot`.
void spray_everything(AirbrushField &field, const indexed_triangle_set &its, int slot, float coverage)
{
    AirbrushSurface surface(its);
    StrokeParams    sp;
    sp.slot = slot;
    field.begin_stroke(sp);
    Dab d;
    d.center   = Vec3f(0.f, 0.f, 0.f);
    d.radius   = 1000.f;
    d.hardness = 1.f;
    d.flow     = coverage;
    field.dab(surface, d);
    field.end_stroke();
}

// Painted sub-triangles of `state` whose source facet passes `facet_filter`, with their sources.
struct Painted
{
    indexed_triangle_set its;
    std::vector<int>     src;
};
Painted painted(const indexed_triangle_set &mesh_its, const TriangleSelector::TriangleSplittingData &data, int state)
{
    TriangleMesh     mesh(mesh_its);
    TriangleSelector sel(mesh);
    sel.deserialize(data, true);
    Painted p;
    p.its = sel.get_facets_strict(EnforcerBlockerType(state), &p.src);
    return p;
}

double tri_area(const Vec3f &a, const Vec3f &b, const Vec3f &c) { return 0.5 * double((b - a).cross(c - a).norm()); }

// Painted area fraction of `state` over the facets `filter` accepts.
template<class Filter> double painted_fraction(const indexed_triangle_set &its, const TriangleSelector::TriangleSplittingData &data, int state, Filter filter)
{
    double total = 0.;
    for (size_t f = 0; f < its.indices.size(); ++f)
        if (filter(its_face_normal(its, int(f))))
            total += tri_area(its.vertices[its.indices[f][0]], its.vertices[its.indices[f][1]], its.vertices[its.indices[f][2]]);
    const Painted p    = painted(its, data, state);
    double        area = 0.;
    for (size_t k = 0; k < p.its.indices.size(); ++k)
        if (filter(its_face_normal(its, p.src[k])))
            area += tri_area(p.its.vertices[p.its.indices[k][0]], p.its.vertices[p.its.indices[k][1]], p.its.vertices[p.its.indices[k][2]]);
    return total > 0. ? area / total : 0.;
}

const auto is_top  = [](const Vec3f &n) { return n.z() > 0.9f; };
const auto is_wall = [](const Vec3f &n) { return std::abs(n.z()) < 0.1f; };
const auto is_front_wall = [](const Vec3f &n) { return n.y() < -0.9f; };   // y = 0, faces -Y

// The painted islands of `state` on the facets `filter` accepts, projected with `proj` and merged.
template<class Filter, class Proj>
ExPolygons painted_islands(const indexed_triangle_set &its, const TriangleSelector::TriangleSplittingData &data, int state, Filter filter, Proj proj)
{
    const Painted p = painted(its, data, state);
    Polygons      polys;
    for (size_t k = 0; k < p.its.indices.size(); ++k) {
        if (!filter(its_face_normal(its, p.src[k])))
            continue;
        Polygon poly;
        for (int c = 0; c < 3; ++c) {
            const Vec2d q = proj(p.its.vertices[p.its.indices[k][c]].cast<double>());
            poly.points.emplace_back(coord_t(scale_(q.x())), coord_t(scale_(q.y())));
        }
        if (poly.area() < 0)
            poly.reverse();
        polys.push_back(std::move(poly));
    }
    // A hair of growth closes the cracks between neighbouring leaves before the union.
    return offset_ex(union_ex(offset(polys, float(scale_(0.002)))), -float(scale_(0.002)));
}

std::string airbrush_temp(const std::string &name)
{
    const fs::path dir = fs::temp_directory_path() / ("snorca_tests_airbrush_" + std::to_string(get_current_pid()));
    fs::create_directories(dir);
    Slic3r::set_temporary_dir(dir.string());
    return (dir / name).string();
}

bool zip_has_prefix(const std::string &zip_path, const std::string &prefix)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return false;
    bool         found = false;
    const mz_uint n    = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < n && !found; ++i) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&archive, i, &st))
            found = std::string(st.m_filename).rfind(prefix, 0) == 0;
    }
    close_zip_reader(&archive);
    return found;
}

// The painting itself: the triangle map and the bits (used_states is derived, and a loader may
// rebuild it differently).
bool same_paint(const TriangleSelector::TriangleSplittingData &a, const TriangleSelector::TriangleSplittingData &b)
{
    return a.triangles_to_split == b.triangles_to_split && a.bitstream == b.bitstream;
}

// The fake mix ladder of the tests: mixed filament 10 + step of filament 2 over filament 1.
int test_ladder(int a, int b, int step, int steps)
{
    return (a == 1 && b == 2 && steps == 4 && step > 0 && step < 4) ? 10 + step : 0;
}

} // namespace

// =============================================================================================
// Blue noise
// =============================================================================================

TEST_CASE("Airbrush: blue-noise mask is a deterministic, evenly spread permutation", "[Airbrush]")
{
    const std::vector<float> &m = blue_noise_mask();
    const int                 S = BLUE_NOISE_SIZE, N = S * S;
    REQUIRE(m.size() == size_t(N));
    CHECK(make_blue_noise_mask(16) == make_blue_noise_mask(16));
    std::vector<float> sorted = m;
    std::sort(sorted.begin(), sorted.end());
    bool exact = true;
    for (int i = 0; i < N; ++i)
        exact &= std::abs(sorted[size_t(i)] - (float(i) + 0.5f) / float(N)) < 1e-6f;
    CHECK(exact);
    // Blue, not white: at 10% the lit cells keep apart. White noise at this density gives about
    // N * 0.1 * 8 * 0.1 = 328 eight-neighbour contacts; this mask gives about 44.
    int touching = 0;
    for (int i = 0; i < N; ++i) {
        if (m[size_t(i)] >= 0.1f)
            continue;
        const int x = i % S, y = i / S;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if ((dx || dy) && m[size_t(((y + dy + S) % S) * S + (x + dx + S) % S)] < 0.1f)
                    ++touching;
    }
    CHECK(touching < 80);
    // Thresholds wrap with the tile and depend on the seed.
    CHECK(blue_noise_threshold(3, 5, 7) == blue_noise_threshold(3 + S, 5 - S, 7));
    int differ = 0;
    for (int i = 0; i < 32; ++i)
        differ += blue_noise_threshold(i, 0, 1) != blue_noise_threshold(i, 0, 2);
    CHECK(differ > 16);
}

// =============================================================================================
// The field: keys, write and read
// =============================================================================================

TEST_CASE("Airbrush field: keys, classes and slots", "[Airbrush]")
{
    for (const std::array<int64_t, 3> &ijk : std::vector<std::array<int64_t, 3>>{ { 0, 0, 0 }, { -1, 2, -3 }, { 524287, -524288, 17 } })
        for (int cls = 0; cls < 6; ++cls) {
            int64_t i, j, k;
            int     c;
            AirbrushField::split_key(AirbrushField::make_key(ijk[0], ijk[1], ijk[2], cls), i, j, k, c);
            CHECK(i == ijk[0]);
            CHECK(j == ijk[1]);
            CHECK(k == ijk[2]);
            CHECK(c == cls);
        }
    CHECK(surface_class(Vec3f(1, 0, 0)) == 0);
    CHECK(surface_class(Vec3f(0, -1, 0)) == 3);
    CHECK(surface_class(Vec3f(0, 0, -1)) == 5);
    CHECK(surface_class(Vec3f(0.7071f, 0.7071f, 0)) == 0);   // tie -> lower axis
    CHECK(surface_class(Vec3f(0, 0, 0)) == -1);

    AirbrushField f;
    CHECK(f.add_slot(2) == 0);
    CHECK(f.add_slot(5) == 1);
    CHECK(f.add_slot(2) == 0);
    CHECK(f.add_slot(0) == -1);
    for (int id = 10; id < 20; ++id)
        f.add_slot(id);
    CHECK(f.slots().size() == AIRBRUSH_MAX_SLOTS);
    CHECK(f.add_slot(99) == -1);
}

TEST_CASE("Airbrush field: blob round trip and damaged blobs", "[Airbrush]")
{
    AirbrushField f;
    f.add_slot(2);
    f.add_slot(7);
    f.set_cell(AirbrushField::make_key(1, 2, 3, 4), Weights{ 100, 50 });
    f.set_cell(AirbrushField::make_key(-5, 0, 9, 1), Weights{ 0, 255 });
    f.set_cell(AirbrushField::make_key(7, 7, 7, 2), Weights{});   // empty: not stored
    CHECK(f.size() == 2);
    f.set_bake_params("grain=0.8;seed=3");
    const indexed_triangle_set box = grid_box(10, 10, 10, 5);
    f.set_anchor(box);
    f.record().facets     = uint32_t(box.indices.size());
    f.record().owned      = { 1, 4, 30 };
    f.record().paint_hash = 0x1234567890ABCDEFull;
    {
        TriangleMesh     mesh(box);
        TriangleSelector sel(mesh);
        sel.set_facet(4, EnforcerBlockerType::Extruder3);
        f.record().under = sel.serialize();
    }

    const std::string blob = f.to_blob();
    const auto        back = AirbrushField::from_blob(blob);
    REQUIRE(back);
    CHECK(*back == f);
    CHECK(back->to_blob() == blob);

    Weights w;
    CHECK(back->sample(back->center_of(AirbrushField::make_key(1, 2, 3, 4)), 4, w));
    CHECK(w[0] == 100);
    CHECK(w[1] == 50);
    // A neighbour of a painted cell of the same class reads its neighbour; another class reads nothing.
    CHECK(back->sample(back->center_of(AirbrushField::make_key(1, 2, 4, 4)), 4, w));
    CHECK(w[0] == 100);
    CHECK_FALSE(back->sample(back->center_of(AirbrushField::make_key(1, 2, 3, 4)), 5, w));

    // Every truncation, a wrong magic and a wrong major version are refused, never a crash.
    bool all_refused = true;
    for (size_t n = 0; n < blob.size(); ++n)
        all_refused &= AirbrushField::from_blob(blob.substr(0, n)) == nullptr;
    CHECK(all_refused);
    std::string bad = blob;
    bad[0]          = 'X';
    CHECK(AirbrushField::from_blob(bad) == nullptr);
    bad    = blob;
    bad[4] = char(9);
    CHECK(AirbrushField::from_blob(bad) == nullptr);
}

TEST_CASE("Airbrush field: a large scale resamples without holes or a blow-up", "[Airbrush]")
{
    const indexed_triangle_set slab = grid_box(4, 4, 1, 1);
    AirbrushField              f;
    f.add_slot(2);
    spray_everything(f, slab, 0, 0.5f);
    const size_t before = f.size();
    AirbrushField big   = f;
    Transform3d   t     = Transform3d::Identity();
    t.scale(20.);
    big.transform(t);
    // Spread capped at 8 cells each way on the two in-plane axes: at most 17 x 17 per old cell.
    CHECK(big.size() > before);
    CHECK(big.size() <= before * 17 * 17);
    // At 3x the splat (one cell each way) closes every gap: the whole scaled top reads the paint.
    AirbrushField x3 = f;
    Transform3d   t3 = Transform3d::Identity();
    t3.scale(3.);
    x3.transform(t3);
    Weights w;
    int     covered = 0;
    for (int j = 0; j < 20; ++j)
        for (int i = 0; i < 20; ++i)
            covered += x3.sample(Vec3d(0.3 + 0.57 * i, 0.3 + 0.57 * j, 3.), 4, w) && int(w[0]) == 128;
    CHECK(covered == 400);
    // A 45 degree turn about Z fills its rotated lattice too.
    AirbrushField turned = f;
    turned.transform(Transform3d(Eigen::AngleAxisd(PI / 4., Vec3d::UnitZ())));
    int hits = 0;
    for (int i = 0; i < 20; ++i) {
        const Vec3d p = Eigen::AngleAxisd(PI / 4., Vec3d::UnitZ()) * Vec3d(0.5 + 0.15 * i, 2.0, 1.0);
        hits += turned.sample(p, 4, w);
    }
    CHECK(hits == 20);
}

TEST_CASE("Airbrush field: attach_to_mesh follows a re-centred mesh and keeps the record", "[Airbrush]")
{
    indexed_triangle_set box = grid_box(10, 10, 10, 1);
    AirbrushField        f;
    f.add_slot(2);
    spray_everything(f, box, 0, 0.5f);
    f.set_anchor(box);
    f.record().facets = uint32_t(box.indices.size());
    f.record().owned  = { 0 };
    const AirbrushField before = f;

    // The same mesh moved by a whole number of cells: the field moves with it, exactly.
    const Vec3d shift(-15 * double(AIRBRUSH_CELL_MM), 30 * double(AIRBRUSH_CELL_MM), 0.);
    for (stl_vertex &v : box.vertices)
        v += shift.cast<float>();
    const Vec3d got = f.attach_to_mesh(box);
    CHECK_THAT(got.x(), WithinAbs(shift.x(), 1e-4));
    CHECK_THAT(got.y(), WithinAbs(shift.y(), 1e-4));
    CHECK(f.size() == before.size());
    CHECK(f.record().owned == before.record().owned);
    Weights w;
    CHECK(f.sample(Vec3d(5, 5, 10) + shift, 4, w));
    CHECK(w[0] == 128);
    // A mesh of a different size is an edit, not a move: nothing shifts.
    indexed_triangle_set bigger = grid_box(12, 10, 10, 1);
    CHECK(f.attach_to_mesh(bigger).isZero());
}

// =============================================================================================
// Strokes
// =============================================================================================

TEST_CASE("Airbrush strokes: flow, build-up, opacity cap, eraser and scatter", "[Airbrush]")
{
    const indexed_triangle_set slab = grid_box(20, 20, 3, 1);
    AirbrushSurface            surface(slab);
    const Vec3d                centre(10.05, 10.05, 3.);
    auto weight_at = [&](const AirbrushField &f, const Vec3d &p, size_t slot = 0) {
        Weights w;
        f.sample(p, 4, w);
        return int(w[slot]);
    };
    Dab dab;
    dab.center   = centre.cast<float>();
    dab.radius   = 2.f;
    dab.hardness = 1.f;
    dab.flow     = 0.25f;

    SECTION("dabs build up within a stroke and across strokes")
    {
        AirbrushField f;
        f.add_slot(2);
        StrokeParams sp;
        f.begin_stroke(sp);
        CHECK(f.dab(surface, dab) > 0);
        CHECK(weight_at(f, centre) == 64);    // 0.25 * 255
        f.dab(surface, dab);
        CHECK(weight_at(f, centre) == 112);   // 0.25 + 0.25 * 0.75 = 0.4375
        f.end_stroke();
        f.begin_stroke(sp);
        f.dab(surface, dab);
        f.end_stroke();
        CHECK(weight_at(f, centre) == 148);   // 112 * 0.75 + 63.75
        // Holding still keeps building up towards full coverage.
        f.begin_stroke(sp);
        for (int i = 0; i < 40; ++i)
            f.dab(surface, dab);
        f.end_stroke();
        CHECK(weight_at(f, centre) == 255);
        // Nothing outside the radius, and nothing on the bottom face under it.
        CHECK(weight_at(f, Vec3d(10.05, 13.5, 3.)) == 0);
        Weights w;
        CHECK_FALSE(f.sample(Vec3d(10.05, 10.05, 0.), 5, w));
    }
    SECTION("hardness shapes the falloff")
    {
        CHECK(dab_falloff(0.f, 2.f, 0.5f) == 1.f);
        CHECK(dab_falloff(1.f, 2.f, 0.5f) == 1.f);
        CHECK(dab_falloff(2.f, 2.f, 0.5f) == 0.f);
        CHECK_THAT(dab_falloff(1.5f, 2.f, 0.5f), WithinAbs(0.5, 1e-6));
        AirbrushField f;
        f.add_slot(2);
        Dab soft     = dab;
        soft.hardness = 0.f;
        f.dab(surface, soft);
        CHECK(weight_at(f, centre) > weight_at(f, centre + Vec3d(1.2, 0, 0)));
        CHECK(weight_at(f, centre + Vec3d(1.2, 0, 0)) > 0);
    }
    SECTION("the opacity cap limits one stroke, not the next")
    {
        AirbrushField f;
        f.add_slot(2);
        StrokeParams sp;
        sp.opacity_cap = 0.3f;
        f.begin_stroke(sp);
        for (int i = 0; i < 20; ++i)
            f.dab(surface, dab);
        f.end_stroke();
        CHECK(weight_at(f, centre) == 77);    // round(0.3 * 255)
        f.begin_stroke(sp);
        for (int i = 0; i < 20; ++i)
            f.dab(surface, dab);
        f.end_stroke();
        CHECK(weight_at(f, centre) == 130);   // 77 * 0.7 + 76.5 = 130.4
    }
    SECTION("two slots share a cell and never exceed full coverage; the eraser removes both")
    {
        AirbrushField f;
        f.add_slot(2);
        f.add_slot(3);
        StrokeParams a, b, e;
        a.slot = 0;
        b.slot = 1;
        e.slot = -1;
        Dab full = dab;
        full.flow = 0.6f;
        f.begin_stroke(a);
        f.dab(surface, full);
        f.end_stroke();
        f.begin_stroke(b);
        f.dab(surface, full);
        f.end_stroke();
        Weights w;
        REQUIRE(f.sample(centre, 4, w));
        CHECK(int(w[0]) == 61);    // 153 * 0.4
        CHECK(int(w[1]) == 153);
        bool capped = true;
        for (const auto &kv : f.cells())
            capped &= kv.second.sum() <= 255;
        CHECK(capped);
        Dab wipe = dab;
        wipe.radius = 5.f;
        wipe.flow   = 1.f;
        f.begin_stroke(e);
        f.dab(surface, wipe);
        f.end_stroke();
        CHECK_FALSE(f.sample(centre, 4, w));
    }
    SECTION("scatter replays exactly from its seed")
    {
        auto run = [&](uint32_t seed) {
            AirbrushField f;
            f.add_slot(2);
            StrokeParams sp;
            sp.scatter = 0.5f;
            sp.seed    = seed;
            f.begin_stroke(sp);
            for (int i = 0; i < 6; ++i)
                f.dab(surface, dab);
            f.end_stroke();
            return f;
        };
        const AirbrushField a = run(1), b = run(1), c = run(2);
        CHECK(a == b);
        CHECK_FALSE(a == c);
    }
}

TEST_CASE("Airbrush field: paint never bleeds through a wall thinner than a cell", "[Airbrush]")
{
    // A 0.1 mm thick vertical sheet: its two big faces are 0.1 mm apart, closer than one 0.15 mm cell.
    const indexed_triangle_set sheet = grid_box(10, 0.1, 10, 1);
    AirbrushSurface            surface(sheet);
    AirbrushField              f;
    f.add_slot(2);
    Dab d;
    d.center   = Vec3f(5.f, 0.f, 5.f);   // on the -Y face
    d.radius   = 2.f;
    d.hardness = 1.f;
    d.flow     = 1.f;
    d.normal   = Vec3f(0.f, -1.f, 0.f);
    f.dab(surface, d);
    Weights w;
    // The near face is painted ...
    REQUIRE(f.sample(Vec3d(5, 0, 5), surface_class(Vec3f(0, -1, 0)), w));
    CHECK(int(w[0]) == 255);
    // ... the far face, 0.1 mm behind it and inside the dab's sphere, is not ...
    CHECK_FALSE(f.sample(Vec3d(5, 0.1, 5), surface_class(Vec3f(0, 1, 0)), w));
    // ... and could not read the near face's paint even if they shared a cell position: the class
    // is part of the key.
    CHECK(f.key_of(Vec3d(5, 0.05, 5), 3) != f.key_of(Vec3d(5, 0.05, 5), 2));
    // So the bake paints only the near face.
    AirbrushBakeParams bp;
    const AirbrushBakeResult r = airbrush_bake(sheet, f, {}, bp);
    REQUIRE(r.ok);
    CHECK(painted_fraction(sheet, r.painting, 2, [](const Vec3f &n) { return n.y() > 0.9f; }) == 0.);
    CHECK(painted_fraction(sheet, r.painting, 2, [](const Vec3f &n) { return n.y() < -0.9f; }) > 0.05);
    // Without a facing direction the sphere reaches through: that is the caller's job to avoid.
    d.normal = Vec3f::Zero();
    AirbrushField g;
    g.add_slot(2);
    g.dab(surface, d);
    CHECK(g.sample(Vec3d(5, 0.1, 5), surface_class(Vec3f(0, 1, 0)), w));
}

// =============================================================================================
// The bake
// =============================================================================================

TEST_CASE("Airbrush bake: the grain floor depends on face orientation", "[Airbrush]")
{
    AirbrushBakeParams p;
    p.grain_mm = 0.4f;
    CHECK(effective_grain(p, false).grain_mm == 0.6f);
    CHECK_FALSE(effective_grain(p, false).tone_fallback);
    CHECK(effective_grain(p, true).grain_mm == 0.8f);
    CHECK(effective_grain(p, true).tone_fallback);
    p.grain_mm = 0.7f;
    CHECK(effective_grain(p, false).grain_mm == 0.7f);
    CHECK(effective_grain(p, true).tone_fallback);
    p.grain_mm = 1.0f;
    CHECK(effective_grain(p, false).grain_mm == 1.0f);
    CHECK(effective_grain(p, true).grain_mm == 1.0f);
    CHECK_FALSE(effective_grain(p, true).tone_fallback);
    CHECK(coverage_reads_as_tone(0.4f));
    CHECK_FALSE(coverage_reads_as_tone(0.3f));

    const indexed_triangle_set slab = grid_box(19.2, 19.2, 3, 0.8);   // 24 x 24 cells of 0.8 on top

    SECTION("a fine request on a top falls back to tone, never to islands")
    {
        for (const float cov : { 0.3f, 0.7f }) {
            AirbrushField f;
            f.add_slot(2);
            spray_everything(f, slab, 0, cov);
            AirbrushBakeParams bp;
            bp.grain_mm               = 0.4f;
            const AirbrushBakeResult r = airbrush_bake(slab, f, {}, bp);
            REQUIRE(r.ok);
            CHECK(r.stats.top_tone_fallback);
            const double top = painted_fraction(slab, r.painting, 2, is_top);
            INFO("coverage " << cov << " top painted " << top);
            CHECK_THAT(top, WithinAbs(cov < 0.5f ? 0. : 1., 1e-6));
            // The walls still speckle, at the wall floor of 0.6 mm.
            CHECK(r.stats.grain_wall_mm == 0.6f);
            const double wall = painted_fraction(slab, r.painting, 2, is_wall);
            CHECK_THAT(wall, WithinAbs(cov, 0.08));
        }
    }
    SECTION("specks are never smaller than one grain cell")
    {
        AirbrushField f;
        f.add_slot(2);
        spray_everything(f, slab, 0, 0.25f);
        AirbrushBakeParams bp;
        bp.grain_mm               = 0.8f;
        const AirbrushBakeResult r = airbrush_bake(slab, f, {}, bp);
        REQUIRE(r.ok);
        CHECK_FALSE(r.stats.top_tone_fallback);
        CHECK_THAT(painted_fraction(slab, r.painting, 2, is_top), WithinAbs(0.25, 0.06));
        const ExPolygons tops = painted_islands(slab, r.painting, 2, is_top, [](const Vec3d &p) { return Vec2d(p.x(), p.y()); });
        REQUIRE(!tops.empty());
        double smallest = 1e9;
        for (const ExPolygon &e : tops)
            smallest = std::min(smallest, e.area() * SCALING_FACTOR * SCALING_FACTOR);
        INFO("smallest top speck " << smallest << " mm2");
        CHECK(smallest >= 0.8 * 0.8 * 0.98);

        // On the front wall (0.6 mm floor at a 0.4 request) - away from the edges, where a cell is cut.
        bp.grain_mm                 = 0.4f;
        const AirbrushBakeResult rw = airbrush_bake(slab, f, {}, bp);
        const ExPolygons walls = painted_islands(slab, rw.painting, 2, is_front_wall, [](const Vec3d &p) { return Vec2d(p.x(), p.z()); });
        REQUIRE(!walls.empty());
        double smallest_wall = 1e9;
        for (const ExPolygon &e : walls) {
            const BoundingBox bb = get_extents(e);
            if (unscale<double>(bb.min.x()) < 0.7 || unscale<double>(bb.max.x()) > 18.5 || unscale<double>(bb.max.y()) > 2.95)
                continue;
            smallest_wall = std::min(smallest_wall, e.area() * SCALING_FACTOR * SCALING_FACTOR);
        }
        INFO("smallest wall speck " << smallest_wall << " mm2");
        // The leaves do not line up with 0.6 mm cells on this 0.8 x 0.75 mm grid, so a cell is a
        // staircase of 0.1 mm leaves; allow for its ragged edge.
        CHECK(smallest_wall >= 0.6 * 0.6 * 0.85);
    }
}

TEST_CASE("Airbrush bake: mix steps go on walls and slopes, never on tops", "[Airbrush]")
{
    const indexed_triangle_set slab = grid_box(20, 20, 4, 1);
    AirbrushField              f;
    f.add_slot(2);
    spray_everything(f, slab, 0, 0.5f);
    AirbrushBakeParams bp;
    bp.base_filament           = 1;
    const AirbrushBakeResult r = airbrush_bake(slab, f, {}, bp, test_ladder);
    REQUIRE(r.ok);
    CHECK(r.stats.leaves_mix > 0);
    CHECK(r.stats.mix_on_flat_leaves == 0);
    // A uniform 50% wall is exactly the 2/4 step: all of it.
    CHECK_THAT(painted_fraction(slab, r.painting, 12, is_wall), WithinAbs(1., 0.02));
    // No ladder state anywhere on a top or bottom; the top is 50% specks of the slot instead.
    const auto is_flat = [](const Vec3f &n) { return std::abs(n.z()) > 0.9f; };
    for (int st = 11; st <= 13; ++st)
        CHECK(painted_fraction(slab, r.painting, st, is_flat) == 0.);
    CHECK_THAT(painted_fraction(slab, r.painting, 2, is_top), WithinAbs(0.5, 0.06));

    SECTION("between steps, the remainder is specks of the slot over the step")
    {
        AirbrushField g;
        g.add_slot(2);
        spray_everything(g, slab, 0, 0.6f);   // low band 0.6 -> step 2 (0.5), residual 0.1 of pure slot
        const AirbrushBakeResult r2 = airbrush_bake(slab, g, {}, bp, test_ladder);
        const double step  = painted_fraction(slab, r2.painting, 12, is_wall);
        const double speck = painted_fraction(slab, r2.painting, 2, is_wall);
        INFO("step " << step << " speck " << speck);
        CHECK_THAT(step * 0.5 + speck, WithinAbs(0.6, 0.04));
        CHECK(speck > 0.1);
        CHECK(speck < 0.3);
    }
    SECTION("no ladder: walls speckle too")
    {
        const AirbrushBakeResult r3 = airbrush_bake(slab, f, {}, bp);
        CHECK(r3.stats.leaves_mix == 0);
        CHECK_THAT(painted_fraction(slab, r3.painting, 2, is_wall), WithinAbs(0.5, 0.06));
    }
}

TEST_CASE("Airbrush bake: deterministic, seeded and local", "[Airbrush]")
{
    const indexed_triangle_set slab = grid_box(30, 20, 3, 1);
    AirbrushSurface            surface(slab);
    AirbrushField              f;
    f.add_slot(2);
    Dab d;
    d.center   = Vec3f(8.f, 10.f, 3.f);
    d.radius   = 5.f;
    d.hardness = 0.3f;
    d.flow     = 0.6f;
    f.dab(surface, d);
    AirbrushBakeParams bp;
    const AirbrushBakeResult a = airbrush_bake(slab, f, {}, bp);
    const AirbrushBakeResult b = airbrush_bake(slab, f, {}, bp);
    REQUIRE(a.ok);
    CHECK(a.painting == b.painting);
    CHECK(a.record.owned == b.record.owned);
    CHECK(a.record.paint_hash == b.record.paint_hash);
    CHECK(a.stats.triangles_owned > 0);

    // The same cells inserted in the opposite order bake to the same paint.
    {
        std::vector<std::pair<uint64_t, Weights>> cells;
        for (const auto &kv : f.cells())
            cells.emplace_back(kv.first, kv.second.w);
        std::sort(cells.begin(), cells.end(), [](const auto &l, const auto &r) { return l.first > r.first; });
        AirbrushField rev;
        rev.add_slot(2);
        for (const auto &kv : cells)
            rev.set_cell(kv.first, kv.second);
        CHECK(airbrush_bake(slab, rev, {}, bp).painting == a.painting);
    }
    // A record made on another mesh is refused: its old specks could not be told from real paint.
    {
        AirbrushField stale = f;
        stale.record().facets = 5;
        stale.record().owned  = { 0 };
        const AirbrushBakeResult s = airbrush_bake(slab, stale, a.painting, bp);
        CHECK_FALSE(s.ok);
        CHECK_FALSE(s.error.empty());
    }

    AirbrushBakeParams other = bp;
    other.seed               = 99;
    CHECK_FALSE(airbrush_bake(slab, f, {}, other).painting == a.painting);

    // A second stroke far away leaves every facet near the first one exactly as it was.
    AirbrushField g = f;
    d.center        = Vec3f(25.f, 10.f, 3.f);
    d.radius        = 3.f;
    g.dab(surface, d);
    const AirbrushBakeResult c = airbrush_bake(slab, g, {}, bp);
    auto subtree = [](const TriangleSelector::TriangleSplittingData &data, int t) {
        std::vector<bool> out;
        const auto       &tts = data.triangles_to_split;
        for (size_t k = 0; k < tts.size(); ++k)
            if (tts[k].triangle_idx == t) {
                const size_t end = k + 1 < tts.size() ? size_t(tts[k + 1].bitstream_start_idx) : data.bitstream.size();
                out.assign(data.bitstream.begin() + tts[k].bitstream_start_idx, data.bitstream.begin() + end);
            }
        return out;
    };
    int same = 0, close_by = 0;
    for (int t : a.record.owned) {
        const Vec3f cen = (slab.vertices[slab.indices[t][0]] + slab.vertices[slab.indices[t][1]] + slab.vertices[slab.indices[t][2]]) / 3.f;
        if (cen.x() > 14.f)
            continue;
        ++close_by;
        same += subtree(a.painting, t) == subtree(c.painting, t);
    }
    CHECK(close_by > 0);
    CHECK(same == close_by);
}

TEST_CASE("Airbrush bake: merges with existing paint and owns only its own", "[Airbrush]")
{
    Model        model;
    ModelObject *obj = model.add_object();
    ModelVolume *vol = obj->add_volume(TriangleMesh(grid_box(20, 20, 3, 1)));
    obj->add_instance();
    const indexed_triangle_set &its = vol->mesh().its;
    // add_volume() centres the mesh; work from its corner.
    const Vec3f o = bounding_box(its).min.cast<float>();

    // Plain brush paint: one facet far from the airbrush, one right under it.
    int far_facet = -1, under_facet = -1;
    for (size_t t = 0; t < its.indices.size(); ++t) {
        if (its_face_normal(its, int(t)).z() < 0.9f)
            continue;
        const Vec3f cen = (its.vertices[its.indices[t][0]] + its.vertices[its.indices[t][1]] + its.vertices[its.indices[t][2]]) / 3.f - o;
        if (far_facet < 0 && cen.x() > 17.f && cen.y() > 17.f)
            far_facet = int(t);
        if (under_facet < 0 && std::abs(cen.x() - 5.f) < 0.5f && std::abs(cen.y() - 5.f) < 0.5f)
            under_facet = int(t);
    }
    REQUIRE(far_facet >= 0);
    REQUIRE(under_facet >= 0);
    {
        TriangleSelector sel(vol->mesh());
        sel.set_facet(far_facet, EnforcerBlockerType::Extruder3);
        sel.set_facet(under_facet, EnforcerBlockerType::Extruder3);
        vol->mmu_segmentation_facets.set(sel);
    }
    const TriangleSelector::TriangleSplittingData original = vol->mmu_segmentation_facets.get_data();

    auto field = std::make_shared<AirbrushField>();
    field->add_slot(2);
    {
        AirbrushSurface surface(its);
        Dab             d;
        d.center   = o + Vec3f(5.f, 5.f, 3.f);
        d.radius   = 4.f;
        d.hardness = 1.f;
        d.flow     = 0.3f;
        field->dab(surface, d);
    }
    vol->airbrush_field = field;
    CHECK_FALSE(airbrush_paint_in_sync(*vol));

    AirbrushBakeParams bp;
    AirbrushBakeStats  st;
    REQUIRE(airbrush_bake_volume(*vol, bp, {}, &st));
    const TriangleSelector::TriangleSplittingData first = vol->mmu_segmentation_facets.get_data();
    CHECK(airbrush_paint_in_sync(*vol));
    const BakeRecord &rec = vol->airbrush_field->record();
    CHECK(std::binary_search(rec.owned.begin(), rec.owned.end(), under_facet));
    CHECK_FALSE(std::binary_search(rec.owned.begin(), rec.owned.end(), far_facet));
    // The far facet is untouched; the facet under the stroke is all airbrush (2) or its old 3 -
    // the bake never left a hole of 0 in paint that was there.
    auto area_of = [&](int state, int facet) {
        const Painted p = painted(its, first, state);
        double        a = 0.;
        for (size_t k = 0; k < p.src.size(); ++k)
            if (p.src[k] == facet)
                a += tri_area(p.its.vertices[p.its.indices[k][0]], p.its.vertices[p.its.indices[k][1]], p.its.vertices[p.its.indices[k][2]]);
        return a;
    };
    const double facet_area = tri_area(its.vertices[its.indices[under_facet][0]], its.vertices[its.indices[under_facet][1]],
                                       its.vertices[its.indices[under_facet][2]]);
    CHECK_THAT(area_of(3, far_facet), WithinAbs(facet_area, 1e-6));
    CHECK_THAT(area_of(3, under_facet) + area_of(2, under_facet), WithinAbs(facet_area, 1e-6));
    CHECK(painted(its, first, 2).its.indices.size() > 0);

    // Re-baking starts from the "under" paint, so it is idempotent: no specks pile up.
    REQUIRE(airbrush_bake_volume(*vol, bp));
    CHECK(same_paint(vol->mmu_segmentation_facets.get_data(), first));

    // A paint edit made elsewhere is detected.
    {
        TriangleSelector sel(vol->mesh());
        sel.deserialize(vol->mmu_segmentation_facets.get_data(), true);
        sel.set_facet(0, EnforcerBlockerType::Extruder4);
        vol->mmu_segmentation_facets.set(sel);
        CHECK_FALSE(airbrush_paint_in_sync(*vol));
        // ... and a re-bake waits for the user's answer unless told to overwrite. Overwrite keeps the
        // edit outside the airbrush's own facets.
        CHECK_FALSE(airbrush_bake_volume(*vol, bp));
        REQUIRE(airbrush_bake_volume(*vol, bp, {}, nullptr, OnForeignPaintEdit::Overwrite));
        CHECK(!painted(its, vol->mmu_segmentation_facets.get_data(), 4).its.indices.empty());
        CHECK(airbrush_paint_in_sync(*vol));
        vol->mmu_segmentation_facets.set_data(TriangleSelector::TriangleSplittingData(first));
        auto restored = std::make_shared<AirbrushField>(*vol->airbrush_field);
        restored->record().paint_hash = paint_hash(first);
        vol->airbrush_field = restored;
        CHECK(airbrush_paint_in_sync(*vol));
    }

    // Erasing the whole field and re-baking gives back exactly the paint from before the airbrush.
    {
        auto empty = std::make_shared<AirbrushField>(*vol->airbrush_field);
        for (const auto &kv : vol->airbrush_field->cells())
            empty->set_cell(kv.first, Weights{});
        vol->airbrush_field = empty;
        REQUIRE(airbrush_bake_volume(*vol, bp));
        CHECK(same_paint(vol->mmu_segmentation_facets.get_data(), original));
        CHECK(vol->airbrush_field->record().owned.empty());
    }
}

// =============================================================================================
// 3MF
// =============================================================================================

TEST_CASE("Airbrush 3MF: the field round-trips natively and stays out of a Bambu export", "[Airbrush][3mf]")
{
    Model        model;
    ModelObject *obj = model.add_object();
    obj->name        = "airbrushed";
    ModelVolume *vol = obj->add_volume(TriangleMesh(grid_box(20, 20, 10, 1)));
    obj->add_instance();
    obj->ensure_on_bed();
    auto field = std::make_shared<AirbrushField>();
    field->add_slot(2);
    {
        AirbrushSurface surface(vol->mesh().its);
        const Vec3f     o = bounding_box(vol->mesh().its).min.cast<float>();   // add_volume() centres the mesh
        Dab             d;
        d.center   = o + Vec3f(10.f, 0.f, 5.f);
        d.radius   = 4.f;
        d.hardness = 0.5f;
        d.flow     = 0.7f;
        field->dab(surface, d);
    }
    vol->airbrush_field = field;
    AirbrushBakeParams bp;
    bp.seed = 5;
    REQUIRE(airbrush_bake_volume(*vol, bp));
    REQUIRE(!vol->mmu_segmentation_facets.empty());

    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_num_extruders(1);
    cfg.set_num_filaments(2);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.4 };
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab P1S"));

    auto load = [](const std::string &path, Model &out) {
        DynamicPrintConfig        back;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
        PlateDataPtrs             plates;
        std::vector<Preset *>     presets;
        bool                      is_bbl = false;
        Semver                    version;
        const bool ok = load_bbs_3mf(path.c_str(), &back, &ctxt, &out, &plates, &presets, &is_bbl, &version, nullptr,
                                     LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);
        release_PlateData_list(plates);
        for (Preset *p : presets)
            delete p;
        return ok;
    };

    SECTION("native")
    {
        const std::string path = airbrush_temp("airbrush_native.3mf");
        StoreParams       sp;
        sp.path     = path.c_str();
        sp.model    = &model;
        sp.config   = &cfg;
        sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
        REQUIRE(store_bbs_3mf(sp));
        CHECK(zip_has_prefix(path, "Metadata/airbrush/"));
        Model back;
        REQUIRE(load(path, back));
        REQUIRE(back.objects.size() == 1);
        const ModelVolume *bv = back.objects[0]->volumes[0];
        REQUIRE(bv->airbrush_field);
        CHECK(same_paint(bv->mmu_segmentation_facets.get_data(), vol->mmu_segmentation_facets.get_data()));
        CHECK(bv->airbrush_field->slots() == vol->airbrush_field->slots());
        CHECK(bv->airbrush_field->bake_params() == bp.to_string());
        CHECK(bv->airbrush_field->record().owned == vol->airbrush_field->record().owned);
        CHECK(airbrush_paint_in_sync(*bv));
        // The cells come back where the loaded mesh is (a re-centred mesh moves them with it).
        const Vec3d shift = bv->airbrush_field->anchor().min - vol->airbrush_field->anchor().min;
        size_t      found = 0;
        for (const auto &kv : vol->airbrush_field->cells()) {
            int64_t i, j, k;
            int     cls;
            AirbrushField::split_key(kv.first, i, j, k, cls);
            Weights w;
            found += bv->airbrush_field->sample(vol->airbrush_field->center_of(kv.first) + shift, cls, w) && w == kv.second.w;
        }
        // The origin moves by the exact shift, so every cell comes back.
        CHECK(found == vol->airbrush_field->size());
        // A re-bake of the loaded project reproduces the saved paint.
        Model again;
        REQUIRE(load(path, again));
        ModelVolume *av = again.objects[0]->volumes[0];
        AirbrushBakeParams loaded;
        REQUIRE(AirbrushBakeParams::from_string(av->airbrush_field->bake_params(), loaded));
        REQUIRE(airbrush_bake_volume(*av, loaded));
        CHECK(same_paint(av->mmu_segmentation_facets.get_data(), vol->mmu_segmentation_facets.get_data()));
        fs::remove(path);
    }
    SECTION("Bambu-compatible")
    {
        const std::string   path = airbrush_temp("airbrush_bambu.3mf");
        BambuExport::Report report;
        StoreParams         sp;
        sp.path         = path.c_str();
        sp.model        = &model;
        sp.config       = &cfg;
        sp.strategy     = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
        sp.bambu_compat = true;
        sp.bambu_report = &report;
        REQUIRE(store_bbs_3mf(sp));
        CHECK_FALSE(zip_has_prefix(path, "Metadata/airbrush/"));
        Model back;
        REQUIRE(load(path, back));
        REQUIRE(back.objects.size() == 1);
        const ModelVolume *bv = back.objects[0]->volumes[0];
        CHECK_FALSE(bv->airbrush_field);
        CHECK(same_paint(bv->mmu_segmentation_facets.get_data(), vol->mmu_segmentation_facets.get_data()));
        fs::remove(path);
    }
}

// =============================================================================================
// Mesh edits (D15)
// =============================================================================================

TEST_CASE("Airbrush mesh edit: the field follows a moved mesh and re-bakes the same paint", "[Airbrush]")
{
    Model        model;
    ModelObject *obj = model.add_object();
    ModelVolume *vol = obj->add_volume(TriangleMesh(grid_box(20, 20, 5, 1)));
    obj->add_instance();
    auto field = std::make_shared<AirbrushField>();
    field->add_slot(2);
    {
        AirbrushSurface surface(vol->mesh().its);
        const Vec3f     o = bounding_box(vol->mesh().its).min.cast<float>();   // add_volume() centres the mesh
        Dab             d;
        d.center   = o + Vec3f(10.f, 10.f, 5.f);
        d.radius   = 6.f;
        d.hardness = 0.4f;
        d.flow     = 0.5f;
        field->dab(surface, d);
        d.center = o + Vec3f(10.f, 0.f, 2.5f);
        field->dab(surface, d);
    }
    vol->airbrush_field = field;
    // Some plain paint as well, which must survive the edit.
    {
        TriangleSelector sel(vol->mesh());
        sel.set_facet(0, EnforcerBlockerType::Extruder3);
        vol->mmu_segmentation_facets.set(sel);
    }
    AirbrushBakeParams bp;
    REQUIRE(airbrush_bake_volume(*vol, bp));
    const indexed_triangle_set                    old_its   = vol->mesh().its;
    const TriangleSelector::TriangleSplittingData old_paint = vol->mmu_segmentation_facets.get_data();

    SECTION("a move by whole grain tiles reproduces the paint")
    {
        // 192 mm is a whole number of 0.15 mm cells and of 64-cell 1 mm blue-noise tiles.
        const Transform3d    t(Eigen::Translation3d(192., 0., 0.));
        indexed_triangle_set moved = old_its;
        for (stl_vertex &v : moved.vertices)
            v = (t * v.cast<double>()).cast<float>();
        vol->set_mesh(TriangleMesh(moved));
        REQUIRE(airbrush_rebake_after_mesh_edit(*vol, old_its, old_paint, t, bp));
        const TriangleSelector::TriangleSplittingData &now = vol->mmu_segmentation_facets.get_data();
        // Float rounding of the moved vertices can flip a leaf sitting exactly on a cell border, so
        // compare painted areas per state rather than bits.
        for (int st : { 2, 3 }) {
            const double before = painted_fraction(old_its, old_paint, st, [](const Vec3f &) { return true; });
            const double after  = painted_fraction(moved, now, st, [](const Vec3f &) { return true; });
            INFO("state " << st << " before " << before << " after " << after);
            CHECK_THAT(after, WithinAbs(before, st == 2 ? 0.002 : 0.01));
        }
        CHECK(airbrush_paint_in_sync(*vol));
    }
    SECTION("a uniform scale keeps the painted share of the surface")
    {
        Transform3d t = Transform3d::Identity();
        t.scale(1.5);
        indexed_triangle_set scaled = old_its;
        for (stl_vertex &v : scaled.vertices)
            v = (t * v.cast<double>()).cast<float>();
        vol->set_mesh(TriangleMesh(scaled));
        REQUIRE(airbrush_rebake_after_mesh_edit(*vol, old_its, old_paint, t, bp));
        const double before = painted_fraction(old_its, old_paint, 2, [](const Vec3f &) { return true; });
        const double after  = painted_fraction(scaled, vol->mmu_segmentation_facets.get_data(), 2, [](const Vec3f &) { return true; });
        INFO("before " << before << " after " << after);
        CHECK(after > 0.);
        CHECK_THAT(after, WithinAbs(before, 0.25 * before));
        // The plain paint came across too.
        CHECK(painted_fraction(scaled, vol->mmu_segmentation_facets.get_data(), 3, [](const Vec3f &) { return true; }) > 0.);
    }
}

// =============================================================================================
// Cost (D8)
// =============================================================================================

TEST_CASE("Airbrush cost: swaps, purge and swap time from a painting", "[Airbrush]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);   // facets {2,3} top, {4,5} +X side
    TriangleMesh               mesh(cube);
    AirbrushCostParams         cp;
    cp.first_layer_height = 0.2;
    cp.layer_height       = 0.2;
    cp.base_filament      = 1;
    cp.flush_mm3          = 280.;
    cp.flush_multiplier   = 0.3;
    cp.density_g_cm3      = 1.24;
    cp.swap_seconds       = 57.;

    SECTION("unpainted: one filament, no swaps")
    {
        const AirbrushCost c = airbrush_estimate_cost(cube, {}, cp);
        CHECK(c.layers == 50);
        CHECK(c.swaps == 0);
        CHECK(c.purge_g == 0.);
    }
    SECTION("a painted top adds one swap on the top layer")
    {
        TriangleSelector sel(mesh);
        sel.set_facet(2, EnforcerBlockerType::Extruder2);
        sel.set_facet(3, EnforcerBlockerType::Extruder2);
        const AirbrushCost c = airbrush_estimate_cost(cube, sel.serialize(), cp);
        CHECK(c.multi_tool_layers == 1);
        CHECK(c.swaps == 1);
        CHECK_THAT(c.purge_mm3, WithinAbs(84., 1e-9));
        CHECK_THAT(c.purge_g, WithinAbs(0.10416, 1e-9));
        CHECK_THAT(c.swap_seconds, WithinAbs(57., 1e-9));
    }
    SECTION("a painted side swaps once per layer, never twice")
    {
        TriangleSelector sel(mesh);
        sel.set_facet(4, EnforcerBlockerType::Extruder2);
        sel.set_facet(5, EnforcerBlockerType::Extruder2);
        const AirbrushCost c = airbrush_estimate_cost(cube, sel.serialize(), cp);
        CHECK(c.multi_tool_layers == 50);
        CHECK(c.swaps == 50);   // layer 0: 1 -> 2, then each layer starts on the last and changes once
        CHECK_THAT(c.swap_seconds, WithinAbs(50. * 57., 1e-6));
    }
    SECTION("a mixed state resolves per layer")
    {
        TriangleSelector sel(mesh);
        sel.set_facet(4, EnforcerBlockerType(10));
        sel.set_facet(5, EnforcerBlockerType(10));
        cp.resolve = [](int state, int layer) { return state == 10 ? (layer % 2 ? 2 : 1) : state; };
        const AirbrushCost c = airbrush_estimate_cost(cube, sel.serialize(), cp);
        CHECK(c.multi_tool_layers == 25);
        CHECK(c.swaps == 49);   // from layer 1 on, every layer changes once
    }
}

// =============================================================================================
// The top-skin pitting regression (P0 photo 03)
// =============================================================================================

namespace {

// A 20 x 20 x 2 slab whose top carries square specks of Extruder2: 0.375 mm ones in the left half
// (x < 10) and 1.25 mm ones in the right half, written as ordinary paint at 0.125 mm leaves.
std::pair<int, int> paint_speck_halves(ModelVolume &vol)
{
    const indexed_triangle_set &its   = vol.mesh().its;
    const int                   depth = 3;   // 1 mm grid -> 0.125 mm leaves
    const size_t                per   = size_t(1) << (2 * depth);
    std::vector<int>            states(its.indices.size() * per, 0);
    std::vector<ImageFillLeaf>  leaves;
    std::set<std::pair<int, int>> small, big;
    // add_volume() centres the mesh; work from its corner.
    const Vec3f o = bounding_box(its).min.cast<float>();
    for (size_t t = 0; t < its.indices.size(); ++t) {
        if (its_face_normal(its, int(t)).z() < 0.9f)
            continue;
        leaves.clear();
        image_fill_subdivide(its.vertices[its.indices[t][0]], its.vertices[its.indices[t][1]], its.vertices[its.indices[t][2]], depth, leaves);
        for (size_t l = 0; l < per; ++l) {
            const Vec3f c = leaves[l].centroid() - o;
            if (c.x() < 10.f) {
                // 0.375 mm specks on a 1.5 mm pitch, away from the slab's edge.
                const float lx = std::fmod(c.x(), 1.5f), ly = std::fmod(c.y(), 1.5f);
                if (c.x() > 1.5f && c.y() > 1.5f && c.y() < 18.5f && lx >= 0.5f && lx < 0.875f && ly >= 0.5f && ly < 0.875f) {
                    states[t * per + l] = 2;
                    small.insert({ int(c.x() / 1.5f), int(c.y() / 1.5f) });
                }
            } else {
                // 1.25 mm specks on a 2.5 mm pitch.
                const float lx = std::fmod(c.x(), 2.5f), ly = std::fmod(c.y(), 2.5f);
                if (c.x() > 12.5f && c.x() < 17.5f && c.y() > 2.5f && c.y() < 17.5f && lx >= 0.5f && lx < 1.75f && ly >= 0.5f && ly < 1.75f) {
                    states[t * per + l] = 2;
                    big.insert({ int(c.x() / 2.5f), int(c.y() / 2.5f) });
                }
            }
        }
    }
    vol.mmu_segmentation_facets.set_data(image_fill_encode(its.indices.size(), depth, states));
    return { int(small.size()), int(big.size()) };
}

DynamicPrintConfig speck_config(PerimeterGeneratorType gen)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FFFFFF", "#3A7BD5" };
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = { 0.4, 0.4 };
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value   = 0.42;
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->percent = false;
    config.option<ConfigOptionFloat>("layer_height")->value                     = 0.2;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value       = 0.2;
    config.option<ConfigOptionEnum<PerimeterGeneratorType>>("wall_generator")->value = gen;
    return config;
}

// The Extruder2 claim on one layer.
ExPolygons extruder2_islands(const PrintObject &object, size_t layer_idx)
{
    ExPolygons   out;
    const Layer *layer = object.get_layer(int(layer_idx));
    for (size_t r = 0; r < object.num_printing_regions(); ++r) {
        const PrintRegion &region = object.printing_region(r);
        if (region.config().wall_filament.value != 2)
            continue;
        const int local = region.print_object_region_id();
        if (local < 0 || local >= layer->region_count())
            continue;
        for (const Surface &s : layer->get_region(local)->slices.surfaces)
            out.emplace_back(s.expolygon);
    }
    return union_ex(out);
}

} // namespace

TEST_CASE("Airbrush P1: painted top islands too small to print never punch holes in the top skin", "[Airbrush][MMSegmentation]")
{
    auto run = [](PerimeterGeneratorType gen, int &small_islands, int &big_islands, int &thin_islands, int &expected_big) {
        Model        model;
        ModelObject *obj = model.add_object();
        obj->name        = "speck_slab";
        ModelVolume *vol = obj->add_volume(TriangleMesh(grid_box(20, 20, 2, 1)));
        obj->add_instance();
        obj->ensure_on_bed();
        const auto counts = paint_speck_halves(*vol);
        REQUIRE(counts.first > 20);
        expected_big = counts.second;
        Print print;
        print.set_status_silent();
        print.apply(model, speck_config(gen));
        REQUIRE(print.objects().size() == 1);
        PrintObject *po = print.objects_mutable().front();
        po->slice();
        REQUIRE(po->layer_count() == 10);
        const ExPolygons isl   = extruder2_islands(*po, po->layer_count() - 1);
        const BoundingBox slab = get_extents(po->get_layer(0)->lslices);
        const coord_t     mid  = (slab.min.x() + slab.max.x()) / 2;
        small_islands = big_islands = thin_islands = 0;
        for (const ExPolygon &e : isl) {
            const BoundingBox bb = get_extents(e.contour);
            (bb.max.x() < mid ? small_islands : big_islands) += 1;
            // Narrower than the classic generator's first loop (0.42 + 0.6 * 0.39 = 0.65 mm wide).
            thin_islands += offset_ex(e, -float(scale_(0.3))).empty();
        }
    };
    int small_i = 0, big_i = 0, thin_i = 0, expected_big = 0;
    SECTION("classic walls: the small specks go back to the base colour, the big ones stay")
    {
        run(PerimeterGeneratorType::Classic, small_i, big_i, thin_i, expected_big);
        INFO("small " << small_i << " big " << big_i << " of " << expected_big << " thin " << thin_i);
        CHECK(small_i == 0);
        CHECK(thin_i == 0);
        CHECK(big_i == expected_big);
    }
    SECTION("Arachne walls are unchanged: they print a bead in a small island")
    {
        run(PerimeterGeneratorType::Arachne, small_i, big_i, thin_i, expected_big);
        INFO("small " << small_i << " big " << big_i << " of " << expected_big);
        CHECK(small_i > 0);
        CHECK(big_i == expected_big);
    }
}

// =============================================================================================
// The owner's hand-test project (hidden: it writes a file and needs the shipped profiles)
//   set AIRBRUSH_P1_OUT=C:\Dev\EdgeSlicerTest\airbrush1\airbrush_p1_test.3mf
//   libslic3r_tests "[AirbrushP1Card]"
// =============================================================================================

TEST_CASE("Airbrush P1: write the hand-test project", "[.][AirbrushP1Card]")
{
    const char *out_env = std::getenv("AIRBRUSH_P1_OUT");
    REQUIRE(out_env != nullptr);
    const std::string out_path = out_env;

    const std::string              printer  = "Elegoo Centauri Carbon 0.4 nozzle";
    const std::string              process  = "0.12mm Fine @Elegoo CC 0.4 nozzle";
    const std::string              filament = "Elegoo PLA @ECC";
    const std::vector<std::string> colours  = { "#FFFFFF", "#3A7BD5" };

    const std::string             saved_data_dir = data_dir();
    const fs::path                scratch        = fs::temp_directory_path() / ("airbrush_p1_" + std::to_string(get_current_pid()));
    fs::create_directories(scratch / "datadir");
    set_data_dir((scratch / "datadir").string());
    set_temporary_dir(scratch.string());

    const std::string profiles = (fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    PresetBundle      library;
    library.load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent);
    PresetBundle bundle;
    bundle.load_vendor_configs_from_json(profiles, "Elegoo", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, &library);
    REQUIRE(bundle.printers.select_preset_by_name(printer, true));
    REQUIRE(bundle.prints.select_preset_by_name(process, true));
    REQUIRE(bundle.filaments.select_preset_by_name(filament, true));
    bundle.filament_presets = { filament };
    bundle.set_num_filaments(2, colours);
    bundle.filament_presets = { filament, filament };
    DynamicPrintConfig cfg = bundle.full_config_secure();
    cfg.option<ConfigOptionStrings>("filament_colour", true)->values = colours;

    // The mix ladder: mixed filaments 3, 4, 5 = 2/8, 4/8, 6/8 of filament 2 over filament 1, as layer
    // patterns (the P0 card's MX row, steps 2, 4 and 6).
    {
        const bool saved = MixedFilamentManager::auto_generate_enabled();
        MixedFilamentManager::set_auto_generate_enabled(true);
        MixedFilamentManager mgr;
        mgr.auto_generate(colours);
        for (MixedFilament &mf : mgr.mixed_filaments()) {
            mf.deleted = true;
            mf.enabled = false;
        }
        for (int k : { 2, 4, 6 }) {
            std::string pattern;
            for (int i = 0; i < 8; ++i)
                pattern += ((i + 1) * k / 8 > i * k / 8) ? '2' : '1';
            mgr.add_custom_filament(1, 2, (k * 100 + 4) / 8, colours);
            MixedFilament &mf    = mgr.mixed_filaments().back();
            mf.manual_pattern    = MixedFilamentManager::normalize_manual_pattern(pattern);
            mf.distribution_mode = int(MixedFilament::LayerCycle);
            mf.ui_mode           = 1;
        }
        cfg.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));
        MixedFilamentManager::set_auto_generate_enabled(saved);
    }
    const LadderLookup ladder = [](int a, int b, int step, int steps) { return (a == 1 && b == 2 && steps == 4 && step >= 1 && step <= 3) ? 2 + step : 0; };

    // A 40 x 40 x 25 block on a 1 mm grid: a soft blue band sprayed across the front wall and a
    // speckled patch on the top, both from the field.
    Model        model;
    ModelObject *obj = model.add_object();
    obj->name        = "airbrush_p1_block";
    obj->config.set("extruder", 1);
    ModelVolume *vol = obj->add_volume(TriangleMesh(grid_box(40, 40, 25, 1)));
    vol->name        = "airbrush_body";
    obj->add_instance();
    auto field = std::make_shared<AirbrushField>();
    field->add_slot(2);
    {
        AirbrushSurface surface(vol->mesh().its);
        const Vec3f     o = bounding_box(vol->mesh().its).min.cast<float>();   // add_volume() centres the mesh
        StrokeParams    sp;
        field->begin_stroke(sp);
        for (int i = 0; i <= 40; ++i) {
            Dab d;
            d.center   = o + Vec3f(float(i), 0.f, 12.5f);
            d.radius   = 9.f;
            d.hardness = 0.2f;
            d.flow     = 0.12f;
            field->dab(surface, d);
        }
        field->end_stroke();
        sp.seed = 7;
        sp.scatter = 0.6f;
        field->begin_stroke(sp);
        for (int i = 0; i < 30; ++i) {
            Dab d;
            d.center   = o + Vec3f(20.f + 8.f * std::cos(i * 0.4f), 22.f + 8.f * std::sin(i * 0.4f), 25.f);
            d.radius   = 4.f;
            d.hardness = 0.6f;
            d.flow     = 0.15f;
            field->dab(surface, d);
        }
        field->end_stroke();
    }
    vol->airbrush_field = field;
    AirbrushBakeParams bp;
    bp.base_filament = 1;
    bp.seed          = 1;
    AirbrushBakeStats st;
    REQUIRE(airbrush_bake_volume(*vol, bp, ladder, &st));
    CHECK(st.leaves_mix > 0);
    CHECK(st.leaves_speck > 0);
    CHECK(st.mix_on_flat_leaves == 0);
    // Place the instance, not the mesh: the field is keyed to the mesh as it is.
    const BoundingBoxf bed(cfg.option<ConfigOptionPoints>("printable_area")->values);
    obj->instances.front()->set_offset(Vec3d(bed.center().x(), bed.center().y(), 0.));
    obj->ensure_on_bed();

    PlateDataPtrs plates;
    PlateData    *pd = new PlateData();
    pd->plate_index  = 0;
    pd->plate_name   = "airbrush P1";
    pd->objects_and_instances.emplace_back(0, 0);
    plates.push_back(pd);
    StoreParams sp;
    sp.path            = out_path.c_str();
    sp.model           = &model;
    sp.config          = &cfg;
    sp.plate_data_list = plates;
    sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    const bool stored  = store_bbs_3mf(sp);
    release_PlateData_list(plates);
    REQUIRE(stored);
    CHECK(zip_has_prefix(out_path, "Metadata/airbrush/"));

    AirbrushCostParams cp;
    cp.first_layer_height = 0.2;
    cp.layer_height       = 0.12;
    cp.flush_multiplier   = 0.3;
    cp.swap_seconds       = 57.;
    cp.resolve            = [](int state, int layer) {
        static const char *pat[] = { "", "", "", "11211121", "12121212", "12221222" };   // approximate
        if (state < 3 || state > 5)
            return state;
        return pat[state][layer % 8] - '0';
    };
    const AirbrushCost cost = airbrush_estimate_cost(vol->mesh().its, vol->mmu_segmentation_facets.get_data(), cp,
                                                     obj->instances.front()->get_matrix() * vol->get_matrix());
    WARN("wrote " << out_path << ": " << st.leaves_speck << " speck leaves, " << st.leaves_mix << " mix leaves, " << st.triangles_owned
                  << " facets; estimate " << cost.swaps << " swaps, " << cost.purge_g << " g purge, " << cost.swap_seconds / 3600. << " h swapping");
    set_data_dir(saved_data_dir);
    boost::system::error_code ec;
    fs::remove_all(scratch, ec);
}
