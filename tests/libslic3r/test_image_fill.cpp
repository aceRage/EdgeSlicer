// Image Fill (Phase 2) - the four things the acceptance bar names: projection maths, asset hash
// stability, subdivision producing a watertight mesh with no T-joints, and the 3MF round trip.
//
// Spec: docs/superpowers/specs/2026-09-07-imagemap-phase2-imagefill.md
#include <catch2/catch.hpp>

#include "libslic3r/ImageFill.hpp"
#include "libslic3r/ColorSplit.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/GLTF.hpp"
#include "libslic3r/ObjColorMatch.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <array>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

std::vector<uint8_t> read_fixture(const std::string &name)
{
    const std::string path = std::string(TEST_DATA_DIR) + "/image_fill/" + name;
    boost::nowide::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The four-filament palette the projection tests use. Deliberately saturated and far apart, so
// "which filament did the solver pick" has one obvious right answer and the test is about the
// projection, not about the solver's tie-breaking.
const std::vector<std::array<float, 3>> kFilamentColors = {
    {1.f, 0.f, 0.f},   // 1 red
    {0.f, 1.f, 0.f},   // 2 green
    {0.f, 0.f, 1.f},   // 3 blue
    {1.f, 1.f, 1.f},   // 4 white
};
const std::vector<int> kFilamentIds = {1, 2, 3, 4};

// The state a given leaf of a given original facet came out with, read back through the fork's
// own decoder rather than out of my encoder - so the test measures what the slicer will see.
std::vector<int> leaf_states(const TriangleMesh &mesh, const TriangleSelector::TriangleSplittingData &data,
                             size_t n_facets, int depth)
{
    size_t per = 1;
    for (int i = 0; i < depth; ++i) per *= 4;
    std::vector<int> out(n_facets * per, 0);
    TriangleSelector sel(mesh);
    sel.deserialize(data, /*needs_reset=*/true);
    // get_facets() reports leaves per state; match them back by their vertex coordinates, which
    // is exact because both sides compute the same midpoints from the same floats.
    std::map<std::array<float, 9>, size_t> index;
    for (size_t f = 0; f < n_facets; ++f) {
        std::vector<ImageFillLeaf> leaves;
        const Vec3i32 &t = mesh.its.indices[f];
        image_fill_subdivide(mesh.its.vertices[t(0)], mesh.its.vertices[t(1)], mesh.its.vertices[t(2)],
                             depth, leaves);
        for (size_t i = 0; i < leaves.size(); ++i)
            index.emplace(std::array<float, 9>{leaves[i].a.x(), leaves[i].a.y(), leaves[i].a.z(),
                                               leaves[i].b.x(), leaves[i].b.y(), leaves[i].b.z(),
                                               leaves[i].c.x(), leaves[i].c.y(), leaves[i].c.z()},
                          f * per + i);
    }
    // Up to OBJ_COLOR_MAX_SLOTS: the headless colour matcher can allocate 16 slots, so a scan
    // that stopped at 8 would silently report the leaves above it as unpainted.
    for (int state = 1; state <= 16; ++state) {
        const indexed_triangle_set its = sel.get_facets(EnforcerBlockerType(state));
        for (const Vec3i32 &t : its.indices) {
            const Vec3f &a = its.vertices[t(0)], &b = its.vertices[t(1)], &c = its.vertices[t(2)];
            auto it = index.find(std::array<float, 9>{a.x(), a.y(), a.z(), b.x(), b.y(), b.z(),
                                                      c.x(), c.y(), c.z()});
            if (it != index.end())
                out[it->second] = state;
        }
    }
    return out;
}

// A single axis-aligned quad in the XY plane at z = 0, as two triangles wound CCW. Enough for a
// projection test and small enough that every leaf can be named.
indexed_triangle_set quad_xy(float sx, float sy)
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(0, 0, 0), Vec3f(sx, 0, 0), Vec3f(sx, sy, 0), Vec3f(0, sy, 0)};
    its.indices  = {Vec3i32(0, 1, 2), Vec3i32(0, 2, 3)};
    return its;
}

} // namespace

// =============================================================================================
// 1. Asset hash stability
// =============================================================================================

TEST_CASE("Image Fill: the asset hash is stable and the store is content addressed", "[imagefill]")
{
    const std::vector<uint8_t> quad = read_fixture("quad_rgbw.png");

    SECTION("the hash of a fixture is the value the fixture generator printed")
    {
        // Pinned against tests/data/image_fill/make_fixtures.py's own output. If this moves, one
        // of the two changed and the round trip is no longer reproducible.
        CHECK(image_fill_sha256_hex(quad) ==
              "3d27b4ed2fdfdb12b533f2ddf6e113f5f6ad516b1acd9ebb3ed1de5476ec51c6");
        CHECK(image_fill_sha256_hex(read_fixture("stripes3.png")) ==
              "2d43f8c92b2d9da66a1bcac7dc453e87386edce930a992f616dc9704cbf19f2c");
        CHECK(image_fill_sha256_hex(read_fixture("ramp_kw.png")) ==
              "e621c916dce53449738f6d26208410c1e0000d26aaefc1e2947e462e57aa6055");
    }

    SECTION("the empty hash is the well-known SHA-256 of nothing")
    {
        CHECK(image_fill_sha256_hex(nullptr, 0) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }

    SECTION("storing the same bytes twice makes one entry with one hash")
    {
        ImageAssetStore store;
        const std::string a = store.add(quad);
        const std::string b = store.add(quad);
        CHECK(a == b);
        CHECK(store.size() == 1);
        REQUIRE(store.find(a) != nullptr);
        CHECK(store.find(a)->bytes == quad);
    }

    SECTION("different bytes make different entries, and retain() drops the unreferenced one")
    {
        ImageAssetStore store;
        const std::string a = store.add(quad);
        const std::string b = store.add(read_fixture("stripes3.png"));
        REQUIRE(a != b);
        CHECK(store.size() == 2);
        store.retain({a});
        CHECK(store.size() == 1);
        CHECK(store.contains(a));
        CHECK_FALSE(store.contains(b));
    }

    SECTION("the fixture decodes to the pixels it was written with")
    {
        ImageAssetStore   store;
        const std::string sha = store.add(quad);
        const ImageAsset *px  = store.pixels(sha);
        REQUIRE(px != nullptr);
        CHECK(px->width == 2);
        CHECK(px->height == 2);
        REQUIRE(px->rgb.size() == 12);
        // Row 0 is the top row: red, green. Row 1: blue, white.
        CHECK(int(px->rgb[0]) == 255); CHECK(int(px->rgb[1]) == 0);   CHECK(int(px->rgb[2]) == 0);
        CHECK(int(px->rgb[3]) == 0);   CHECK(int(px->rgb[4]) == 255); CHECK(int(px->rgb[5]) == 0);
        CHECK(int(px->rgb[6]) == 0);   CHECK(int(px->rgb[7]) == 0);   CHECK(int(px->rgb[8]) == 255);
        CHECK(int(px->rgb[9]) == 255); CHECK(int(px->rgb[10]) == 255); CHECK(int(px->rgb[11]) == 255);
    }
}

// =============================================================================================
// 2. Projection maths - known points map to known pixels
// =============================================================================================

TEST_CASE("Image Fill: each projection maps known points to known image coordinates", "[imagefill]")
{
    BoundingBoxf3 box(Vec3d(0, 0, 0), Vec3d(10, 20, 40));

    SECTION("planar along Z takes u from x and v from y")
    {
        ImageFillParams p;
        p.projection = ImageFillProjection::Planar;
        p.axis       = ImageFillAxis::Z;
        float u = 0, v = 0;
        REQUIRE(image_fill_project(p, box, Vec3f(0, 0, 7), u, v));
        CHECK(u == Approx(0.f)); CHECK(v == Approx(0.f));
        REQUIRE(image_fill_project(p, box, Vec3f(10, 20, 7), u, v));
        CHECK(u == Approx(1.f)); CHECK(v == Approx(1.f));
        REQUIRE(image_fill_project(p, box, Vec3f(2.5f, 15, 7), u, v));
        CHECK(u == Approx(0.25f)); CHECK(v == Approx(0.75f));
    }

    SECTION("planar along X takes u from y and v from z, and along Y u from x and v from z")
    {
        ImageFillParams p;
        p.projection = ImageFillProjection::Planar;
        float u = 0, v = 0;
        p.axis = ImageFillAxis::X;
        REQUIRE(image_fill_project(p, box, Vec3f(3, 5, 10), u, v));
        CHECK(u == Approx(0.25f)); CHECK(v == Approx(0.25f));
        p.axis = ImageFillAxis::Y;
        REQUIRE(image_fill_project(p, box, Vec3f(2.5f, 3, 30), u, v));
        CHECK(u == Approx(0.25f)); CHECK(v == Approx(0.75f));
    }

    SECTION("the flips are exactly one minus the unflipped answer")
    {
        ImageFillParams p;
        p.projection = ImageFillProjection::Planar;
        p.axis       = ImageFillAxis::Z;
        p.flip_u = true;
        p.flip_v = true;
        float u = 0, v = 0;
        REQUIRE(image_fill_project(p, box, Vec3f(2.5f, 15, 7), u, v));
        CHECK(u == Approx(0.75f)); CHECK(v == Approx(0.25f));
    }

    SECTION("cylindrical about Z: u is the turn angle from +x, v runs up the axis")
    {
        BoundingBoxf3  cyl(Vec3d(-5, -5, 0), Vec3d(5, 5, 20));
        ImageFillParams p;
        p.projection = ImageFillProjection::Cylindrical;
        p.axis       = ImageFillAxis::Z;
        float u = 0, v = 0;
        REQUIRE(image_fill_project(p, cyl, Vec3f(5, 0, 5), u, v));
        CHECK(u == Approx(0.5f));  CHECK(v == Approx(0.25f));   // +x is the middle of the image
        REQUIRE(image_fill_project(p, cyl, Vec3f(0, 5, 10), u, v));
        CHECK(u == Approx(0.75f)); CHECK(v == Approx(0.5f));    // +y, a quarter turn on
        REQUIRE(image_fill_project(p, cyl, Vec3f(-5, 0, 20), u, v));
        CHECK(u == Approx(0.f));   CHECK(v == Approx(1.f));     // -x is the seam
        REQUIRE(image_fill_project(p, cyl, Vec3f(0, -5, 0), u, v));
        CHECK(u == Approx(0.25f)); CHECK(v == Approx(0.f));     // -y
        // Exactly on the axis there is no angle, and the projection says so rather than guessing.
        CHECK_FALSE(image_fill_project(p, cyl, Vec3f(0, 0, 10), u, v));
    }

    SECTION("cylindrical about X wraps in the y/z plane")
    {
        BoundingBoxf3   cyl(Vec3d(0, -5, -5), Vec3d(30, 5, 5));
        ImageFillParams p;
        p.projection = ImageFillProjection::Cylindrical;
        p.axis       = ImageFillAxis::X;
        float u = 0, v = 0;
        REQUIRE(image_fill_project(p, cyl, Vec3f(15, 5, 0), u, v));
        CHECK(u == Approx(0.5f));  CHECK(v == Approx(0.5f));
        REQUIRE(image_fill_project(p, cyl, Vec3f(0, 0, 5), u, v));
        CHECK(u == Approx(0.75f)); CHECK(v == Approx(0.f));
    }

    SECTION("MeshUV is not computed here; the caller supplies it")
    {
        ImageFillParams p;
        p.projection = ImageFillProjection::MeshUV;
        float u = 0, v = 0;
        CHECK_FALSE(image_fill_project(p, box, Vec3f(1, 1, 1), u, v));
    }
}

TEST_CASE("Image Fill: a four-quadrant image lands on the four quadrants of the part", "[imagefill]")
{
    // The end-to-end statement of the projection: a 2x2 picture on a 4x4 mm quad, subdivided
    // twice, must put red in the quadrant the image's top-left pixel points at and so on. This is
    // the "known points map to known pixels" bar with the pixels actually read.
    ImageAssetStore store;
    const std::string sha = store.add(read_fixture("quad_rgbw.png"));

    const indexed_triangle_set mesh = quad_xy(4.f, 4.f);
    TriangleSelector::TriangleSplittingData empty;

    ImageFillParams p;
    p.asset       = sha;
    p.projection  = ImageFillProjection::Planar;
    p.axis        = ImageFillAxis::Z;
    p.subdivision = 3;
    p.allowed     = kFilamentIds;

    ImageFillResult res = image_fill_compute(mesh, empty, p, store, kFilamentColors, kFilamentIds);
    REQUIRE(res.ok);
    CHECK(res.subdivision_used == 3);
    CHECK(res.leaves_total == 2u * 64u);
    CHECK(res.facets_painted == res.leaves_total);

    TriangleMesh     tm(mesh);
    const std::vector<int> states = leaf_states(tm, res.painting, mesh.indices.size(), 3);

    // Walk the leaves and assert on the quadrant each centroid lands in. v = y/4, and the image's
    // row 0 (red, green) is at v = 1, i.e. the HIGH-y half of the part.
    size_t checked = 0;
    for (size_t f = 0; f < mesh.indices.size(); ++f) {
        std::vector<ImageFillLeaf> leaves;
        const Vec3i32 &t = mesh.indices[f];
        image_fill_subdivide(mesh.vertices[t(0)], mesh.vertices[t(1)], mesh.vertices[t(2)], 3, leaves);
        for (size_t i = 0; i < leaves.size(); ++i) {
            const Vec3f c = leaves[i].centroid();
            // Skip leaves whose centroid sits on the halfway line; the nearest-neighbour sampler
            // is allowed either pixel there and the test is not about that.
            if (std::abs(c.x() - 2.f) < 1e-3f || std::abs(c.y() - 2.f) < 1e-3f)
                continue;
            const int expect = (c.y() > 2.f) ? ((c.x() < 2.f) ? 1 : 2)    // top row: red, green
                                             : ((c.x() < 2.f) ? 3 : 4);   // bottom row: blue, white
            INFO("leaf centroid " << c.x() << "," << c.y());
            CHECK(states[f * 64 + i] == expect);
            ++checked;
        }
    }
    CHECK(checked > 100);
    CHECK(res.filaments_used == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE("Image Fill: mesh UVs put the picture where the exporter drew it", "[imagefill]")
{
    // The glTF projection: the same quad, but the image is placed by TEXCOORD_0 rather than by
    // position - and with v running DOWN from the top-left, which is glTF's convention. The UVs
    // here rotate the picture a quarter turn against the planar case, so a projection that
    // quietly ignored them could not pass both this and the test above.
    ImageAssetStore   store;
    const std::string sha = store.add(read_fixture("quad_rgbw.png"));

    indexed_triangle_set mesh = quad_xy(4.f, 4.f);
    // (x, y) -> (u, v) = (y/4, x/4): u from y, v from x, and glTF's v grows downwards. Given
    // per FACE, three corners each, in the triangle's own vertex order - the corners of
    // quad_xy() are (0,0), (4,0), (4,4), (0,4) and the faces are (0,1,2) and (0,2,3).
    const Vec2f uv0(0.f, 0.f), uv1(0.f, 1.f), uv2(1.f, 1.f), uv3(1.f, 0.f);
    ImageFillUVs uvs = {{uv0, uv1, uv2}, {uv0, uv2, uv3}};

    ImageFillParams p;
    p.asset       = sha;
    p.projection  = ImageFillProjection::MeshUV;
    p.subdivision = 3;
    p.allowed     = kFilamentIds;

    TriangleSelector::TriangleSplittingData empty;
    ImageFillResult res = image_fill_compute(mesh, empty, p, store, kFilamentColors, kFilamentIds, uvs);
    REQUIRE(res.ok);

    TriangleMesh           tm(mesh);
    const std::vector<int> states = leaf_states(tm, res.painting, mesh.indices.size(), 3);
    size_t checked = 0;
    for (size_t f = 0; f < mesh.indices.size(); ++f) {
        std::vector<ImageFillLeaf> leaves;
        const Vec3i32 &t = mesh.indices[f];
        image_fill_subdivide(mesh.vertices[t(0)], mesh.vertices[t(1)], mesh.vertices[t(2)], 3, leaves);
        for (size_t i = 0; i < leaves.size(); ++i) {
            const Vec3f c = leaves[i].centroid();
            if (std::abs(c.x() - 2.f) < 1e-3f || std::abs(c.y() - 2.f) < 1e-3f)
                continue;
            // u = y/4, glTF v = x/4 measured downwards, so the image's top row (red, green) is at
            // LOW x, and its left column (red, blue) is at LOW y.
            const int expect = (c.x() < 2.f) ? ((c.y() < 2.f) ? 1 : 2)
                                             : ((c.y() < 2.f) ? 3 : 4);
            INFO("leaf centroid " << c.x() << "," << c.y());
            CHECK(states[f * 64 + i] == expect);
            ++checked;
        }
    }
    CHECK(checked > 100);
}

// =============================================================================================
// 3. Subdivision: watertight, no T-joints
// =============================================================================================

TEST_CASE("Image Fill: subdivision is a conforming, T-joint-free refinement", "[imagefill]")
{
    SECTION("one triangle at depth n has 4^n leaves whose areas sum to the original")
    {
        const Vec3f a(0, 0, 0), b(6, 0, 0), c(0, 9, 0);
        const float area0 = 0.5f * (b - a).cross(c - a).norm();
        for (int depth = 0; depth <= 4; ++depth) {
            std::vector<ImageFillLeaf> leaves;
            image_fill_subdivide(a, b, c, depth, leaves);
            size_t expect = 1;
            for (int i = 0; i < depth; ++i) expect *= 4;
            REQUIRE(leaves.size() == expect);
            float sum = 0.f;
            for (const ImageFillLeaf &l : leaves)
                sum += 0.5f * (l.b - l.a).cross(l.c - l.a).norm();
            INFO("depth " << depth);
            CHECK(sum == Approx(area0).epsilon(1e-4));
        }
    }

    SECTION("a painted cube stays watertight with zero open edges after retriangulation")
    {
        // ColorSplit::extract_color_patches is the fork's own T-joint-free retriangulation: it
        // concatenates get_facets_strict() over every used state and then refuses a surface with
        // any open edge. Running the painted cube through it is the sharpest available statement
        // that the subdivision left no T-joint and no crack.
        ImageAssetStore   store;
        const std::string sha = store.add(read_fixture("quad_rgbw.png"));

        TriangleMesh cube = make_cube(10., 10., 10.);
        cube.its.indices.size();
        ImageFillParams p;
        p.asset       = sha;
        p.projection  = ImageFillProjection::Planar;
        p.axis        = ImageFillAxis::Z;
        p.allowed     = kFilamentIds;

        for (int depth : {0, 1, 2, 3}) {
            p.subdivision = depth;
            TriangleSelector::TriangleSplittingData empty;
            ImageFillResult res = image_fill_compute(cube.its, empty, p, store, kFilamentColors, kFilamentIds);
            REQUIRE(res.ok);
            INFO("depth " << depth);
            ColorPatches patches = extract_color_patches(cube.its, res.painting);
            CHECK(its_num_open_edges(patches.surface) == 0);
            // Every leaf accounted for: the retriangulation must not lose or invent a facet's
            // worth of area either.
            double area = 0.;
            for (const Vec3i32 &t : patches.surface.indices) {
                const Vec3f &a = patches.surface.vertices[t(0)];
                const Vec3f &b = patches.surface.vertices[t(1)];
                const Vec3f &c = patches.surface.vertices[t(2)];
                area += 0.5 * double((b - a).cross(c - a).norm());
            }
            CHECK(area == Approx(600.).epsilon(1e-4));   // a 10 mm cube
        }
    }

    SECTION("the encoder round-trips through the fork's own decoder")
    {
        // image_fill_encode writes TriangleSelector's bitstream by hand. The proof that it writes
        // the right thing is that deserialize() reads it and serialize() gives the same bytes
        // back - i.e. the encoding is the canonical one, not merely one the decoder tolerates.
        TriangleMesh cube = make_cube(10., 10., 10.);
        const size_t n = cube.its.indices.size();
        const int    depth = 2;
        std::vector<int> states(n * 16);
        for (size_t i = 0; i < states.size(); ++i)
            states[i] = int(i % 5);      // 0..4, so both the 2-bit and the nibble-chain forms appear
        const auto encoded = image_fill_encode(n, depth, states);

        TriangleSelector sel(cube);
        sel.deserialize(encoded, /*needs_reset=*/true);
        const auto reserialized = sel.serialize();
        CHECK(reserialized.triangles_to_split == encoded.triangles_to_split);
        CHECK(reserialized.bitstream == encoded.bitstream);
    }

    SECTION("a high state uses the nibble chain and still round-trips")
    {
        TriangleMesh cube = make_cube(4., 4., 4.);
        const size_t n = cube.its.indices.size();
        std::vector<int> states(n, 0);
        states[0] = 20;      // > 3 + 15, so two chunks
        states[1] = 3;       // exactly the boundary into the chain form
        states[2] = 2;       // the plain 2-bit form
        const auto encoded = image_fill_encode(n, 0, states);
        TriangleSelector sel(cube);
        sel.deserialize(encoded, /*needs_reset=*/true);
        CHECK(sel.serialize().bitstream == encoded.bitstream);
        CHECK(sel.num_facets(EnforcerBlockerType(20)) == 1);
        CHECK(sel.num_facets(EnforcerBlockerType(3)) == 1);
        CHECK(sel.num_facets(EnforcerBlockerType(2)) == 1);
    }

    SECTION("the depth is capped so a dense mesh cannot explode")
    {
        // 6 is the ceiling; a target so fine it would need more is clamped there...
        CHECK(image_fill_depth_for_detail(100.f, 0.001f, IMAGE_FILL_MAX_SUBDIVISION, 12) == 6);
        // ...and the leaf budget wins over the level when the mesh is already large.
        CHECK(image_fill_depth_for_detail(100.f, 0.001f, IMAGE_FILL_MAX_SUBDIVISION, 2000000) < 6);
        // A part already finer than the target is not subdivided at all.
        CHECK(image_fill_depth_for_detail(0.2f, 1.0f, IMAGE_FILL_MAX_SUBDIVISION, 12) == 0);
        // Two millimetres of triangle against a half-millimetre target is two halvings.
        CHECK(image_fill_depth_for_detail(2.f, 0.5f, IMAGE_FILL_MAX_SUBDIVISION, 12) == 2);
    }
}

// =============================================================================================
// 4. The solver and the palette
// =============================================================================================

TEST_CASE("Image Fill: the palette is deterministic and the solver picks the nearest filament", "[imagefill]")
{
    SECTION("quantising the same samples twice gives the same palette in the same order")
    {
        std::vector<std::array<float, 3>> samples;
        for (int i = 0; i < 500; ++i) {
            const float t = float(i) / 499.f;
            samples.push_back({t, 1.f - t, 0.5f});
        }
        const ImageFillPalette a = image_fill_quantise(samples, 16);
        const ImageFillPalette b = image_fill_quantise(samples, 16);
        REQUIRE(a.colors.size() == b.colors.size());
        CHECK(a.colors.size() <= 16);
        for (size_t i = 0; i < a.colors.size(); ++i) {
            CHECK(a.colors[i][0] == b.colors[i][0]);
            CHECK(a.colors[i][1] == b.colors[i][1]);
            CHECK(a.colors[i][2] == b.colors[i][2]);
            CHECK(a.counts[i] == b.counts[i]);
        }
        // Nothing is thrown away: the counts still add up to the number of samples, even for the
        // buckets that were merged into a kept representative.
        size_t total = 0;
        for (size_t c : a.counts) total += c;
        CHECK(total == samples.size());
    }

    SECTION("the solver returns the obvious filament for an obvious colour")
    {
        ImageFillPalette pal;
        pal.colors = {{0.95f, 0.05f, 0.05f}, {0.05f, 0.9f, 0.1f}, {0.1f, 0.1f, 0.95f},
                      {0.98f, 0.98f, 0.98f}};
        image_fill_solve(pal, kFilamentColors, kFilamentIds);
        REQUIRE(pal.filament.size() == 4);
        CHECK(pal.filament[0] == 1);
        CHECK(pal.filament[1] == 2);
        CHECK(pal.filament[2] == 3);
        CHECK(pal.filament[3] == 4);
    }

    SECTION("only the allowed filaments are ever chosen")
    {
        ImageFillPalette pal;
        pal.colors = {{0.95f, 0.05f, 0.05f}, {0.05f, 0.9f, 0.1f}};
        const std::vector<std::array<float, 3>> two = {{0.f, 0.f, 1.f}, {1.f, 1.f, 1.f}};
        const std::vector<int>                  ids = {3, 4};
        image_fill_solve(pal, two, ids);
        for (int f : pal.filament)
            CHECK((f == 3 || f == 4));
    }

    SECTION("per-face colours become painting, and the same input twice gives the same painting")
    {
        TriangleMesh cube = make_cube(10., 10., 10.);
        std::vector<std::array<float, 3>> face_colors;
        for (size_t i = 0; i < cube.its.indices.size(); ++i)
            face_colors.push_back(kFilamentColors[i % 4]);
        const ImageFillResult a = image_fill_from_face_colors(cube.its, face_colors, kFilamentColors, kFilamentIds);
        const ImageFillResult b = image_fill_from_face_colors(cube.its, face_colors, kFilamentColors, kFilamentIds);
        REQUIRE(a.ok);
        CHECK(a.facets_painted == cube.its.indices.size());
        CHECK(a.painting.bitstream == b.painting.bitstream);
        CHECK(a.painting.triangles_to_split == b.painting.triangles_to_split);
        CHECK(a.filaments_used == std::vector<int>{1, 2, 3, 4});
    }

    SECTION("a run of image_fill_compute is deterministic")
    {
        ImageAssetStore   store;
        const std::string sha = store.add(read_fixture("stripes3.png"));
        TriangleMesh      cube = make_cube(10., 10., 10.);
        ImageFillParams   p;
        p.asset = sha;
        p.axis  = ImageFillAxis::Y;
        p.subdivision = 3;
        p.allowed = kFilamentIds;
        TriangleSelector::TriangleSplittingData empty;
        const ImageFillResult a = image_fill_compute(cube.its, empty, p, store, kFilamentColors, kFilamentIds);
        const ImageFillResult b = image_fill_compute(cube.its, empty, p, store, kFilamentColors, kFilamentIds);
        REQUIRE(a.ok);
        REQUIRE(b.ok);
        CHECK(a.painting.bitstream == b.painting.bitstream);
        CHECK(a.painting.triangles_to_split == b.painting.triangles_to_split);
    }
}

TEST_CASE("Image Fill: the params string round-trips", "[imagefill]")
{
    ImageFillParams p;
    p.asset           = "3d27b4ed2fdfdb12b533f2ddf6e113f5f6ad516b1acd9ebb3ed1de5476ec51c6";
    p.projection      = ImageFillProjection::Cylindrical;
    p.axis            = ImageFillAxis::X;
    p.flip_u          = true;
    p.subdivision     = 4;
    p.detail_mm       = 0.35f;
    p.background      = 2;
    p.selection_state = 3;
    p.allowed         = {1, 3, 5};

    ImageFillParams back;
    REQUIRE(ImageFillParams::from_string(p.to_string(), back));
    CHECK(back.asset == p.asset);
    CHECK(back.projection == p.projection);
    CHECK(back.axis == p.axis);
    CHECK(back.flip_u);
    CHECK_FALSE(back.flip_v);
    CHECK(back.subdivision == 4);
    CHECK(back.detail_mm == Approx(0.35f));
    CHECK(back.background == 2);
    CHECK(back.selection_state == 3);
    CHECK(back.allowed == std::vector<int>{1, 3, 5});
    CHECK(back.to_string() == p.to_string());

    SECTION("a gradient survives too")
    {
        ImageFillParams g;
        g.gradient.enabled    = true;
        g.gradient.three_stop = true;
        g.gradient.stop_a     = {0.f, 0.f, 0.f};
        g.gradient.stop_b     = {0.5f, 0.25f, 0.125f};
        g.gradient.stop_c     = {1.f, 1.f, 1.f};
        g.gradient.direction  = 2;
        ImageFillParams gb;
        REQUIRE(ImageFillParams::from_string(g.to_string(), gb));
        CHECK(gb.gradient.enabled);
        CHECK(gb.gradient.three_stop);
        CHECK(gb.gradient.direction == 2);
        CHECK(gb.gradient.stop_b[1] == Approx(0.25f));
        CHECK(gb.to_string() == g.to_string());
    }

    SECTION("garbage is refused rather than silently accepted")
    {
        ImageFillParams bad;
        CHECK_FALSE(ImageFillParams::from_string("", bad));
        CHECK_FALSE(ImageFillParams::from_string("proj=1;axis=0", bad));   // no version marker
    }

    SECTION("a gradient with no image paints without an asset")
    {
        ImageAssetStore store;
        TriangleMesh    cube = make_cube(10., 10., 10.);
        ImageFillParams g;
        g.gradient.enabled = true;
        g.gradient.stop_a  = {0.f, 0.f, 0.f};
        g.gradient.stop_b  = {1.f, 1.f, 1.f};
        g.gradient.direction = 1;
        g.subdivision = 2;
        g.allowed     = kFilamentIds;
        TriangleSelector::TriangleSplittingData empty;
        const ImageFillResult r = image_fill_compute(cube.its, empty, g, store, kFilamentColors, kFilamentIds);
        REQUIRE(r.ok);
        CHECK(r.facets_painted > 0);
        CHECK(store.empty());   // a gradient costs the project no payload at all
    }
}

// =============================================================================================
// 5. The 3MF round trip
// =============================================================================================

TEST_CASE("Image Fill: apply, save, reload - the painting and the asset are bit-identical", "[imagefill][3mf]")
{
    const std::vector<uint8_t> png = read_fixture("quad_rgbw.png");

    Model        src_model;
    ModelObject *src_object = src_model.add_object();
    src_object->name = "cube";
    ModelVolume *src_volume = src_object->add_volume(make_cube(20., 20., 20.));
    src_volume->name = "cube";
    src_object->add_instance();
    src_object->ensure_on_bed();

    const std::string sha = src_model.image_assets.add(png);
    REQUIRE(src_model.image_assets.size() == 1);

    ImageFillParams p;
    p.asset       = sha;
    p.projection  = ImageFillProjection::Planar;
    p.axis        = ImageFillAxis::Z;
    p.subdivision = 2;
    p.allowed     = kFilamentIds;

    const ImageFillResult applied = image_fill_apply(*src_volume, p, src_model.image_assets,
                                                     kFilamentColors, kFilamentIds);
    REQUIRE(applied.ok);
    REQUIRE(applied.facets_painted > 0);
    const auto src_painting = src_volume->mmu_segmentation_facets.get_data();
    REQUIRE_FALSE(src_painting.bitstream.empty());

    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string test_file = (tmp_root / "image_fill_project.3mf").string();

    StoreParams store_params;
    store_params.path     = test_file.c_str();
    store_params.model    = &src_model;
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    const bool stored = store_bbs_3mf(store_params);

    Model                     dst_model;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::EnableSilent};
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool loaded = stored && load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model,
                                               &plate_data, &project_presets, &is_bbl_3mf, &file_version,
                                               nullptr,
                                               LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                                                   LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    boost::filesystem::remove(test_file);

    REQUIRE(stored);
    REQUIRE(loaded);
    REQUIRE(dst_model.objects.size() == 1);
    REQUIRE(dst_model.objects[0]->volumes.size() == 1);
    const ModelVolume *dst_volume = dst_model.objects[0]->volumes[0];

    THEN("the painted facets come back bit-identical")
    {
        const auto dst_painting = dst_volume->mmu_segmentation_facets.get_data();
        CHECK(dst_painting.triangles_to_split == src_painting.triangles_to_split);
        CHECK(dst_painting.bitstream == src_painting.bitstream);
    }

    THEN("the image asset comes back byte-identical, under the same hash")
    {
        REQUIRE(dst_model.image_assets.size() == 1);
        const ImageAsset *a = dst_model.image_assets.find(sha);
        REQUIRE(a != nullptr);
        CHECK(a->bytes == png);
        // ...and the hash of what came back is still the key it was filed under, which is the
        // statement that the archive did not transform the bytes on the way through.
        CHECK(image_fill_sha256_hex(a->bytes) == sha);
    }

    THEN("the annotation comes back, so the dialog can re-open on it")
    {
        ImageFillParams back;
        REQUIRE(image_fill_params_of(*dst_volume, back));
        CHECK(back.asset == sha);
        CHECK(back.projection == ImageFillProjection::Planar);
        CHECK(back.axis == ImageFillAxis::Z);
        CHECK(back.subdivision == 2);
        CHECK(back.allowed == kFilamentIds);
    }

    THEN("re-applying the reloaded annotation reproduces the same painting")
    {
        // The point of storing the params at all: the fill is reproducible, not just preserved.
        ImageFillParams back;
        REQUIRE(image_fill_params_of(*dst_volume, back));
        Model         redo_model;
        ModelObject  *redo_object = redo_model.add_object();
        ModelVolume  *redo_volume = redo_object->add_volume(make_cube(20., 20., 20.));
        redo_object->add_instance();
        redo_model.image_assets.add(png);
        const ImageFillResult r = image_fill_apply(*redo_volume, back, redo_model.image_assets,
                                                   kFilamentColors, kFilamentIds);
        REQUIRE(r.ok);
        CHECK(redo_volume->mmu_segmentation_facets.get_data().bitstream == src_painting.bitstream);
    }
}

// =============================================================================================
// 6. glTF import: the before/after
// =============================================================================================
//
// The change to GLB import in this phase is that a baseColorTexture is sampled at 16 points per
// triangle instead of one, and that the ImageFill service - not ObjColorMatch's CIE76 pass -
// decides which of the chosen filaments each sample belongs to. This measures it, on two files the
// reader has never seen (tests/data/image_fill/agent_*.glb, written by make_glb.py from struct and
// zlib, deliberately low-poly with a detailed texture).
//
// Both paths run in ONE binary on ONE import, which is a sharper comparison than two builds: the
// only thing that differs is which of the two Model::import_multi_volume_face_color_deal overloads
// is called. The filaments are chosen by obj_color_auto_match, exactly as a headless import does,
// so both sides get the same spools.
//
// The metric is the OLD path's own: CIE76 dE between each leaf's true texture colour and the
// colour of the filament it was painted with, area-weighted (every leaf of a uniform subdivision
// has the same area). Measuring the new path with the incumbent's metric is the conservative way
// round.
namespace {

struct GlbBeforeAfter
{
    size_t triangles = 0;
    size_t leaves    = 0;
    size_t painted_old = 0, painted_new = 0;
    size_t filaments_old = 0, filaments_new = 0;
    double mean_de_old = 0., mean_de_new = 0.;
    double max_de_old  = 0., max_de_new  = 0.;
};

bool measure_glb(const std::string &file, GlbBeforeAfter &out)
{
    const std::string path = std::string(TEST_DATA_DIR) + "/image_fill/" + file;

    Model       probe;
    GltfInfo    info;
    std::string message;
    if (!load_gltf(path.c_str(), &probe, info, message))
        return false;
    if (info.face_colors.empty() || info.sub_face_colors.empty() || info.sub_face_depth <= 0)
        return false;

    size_t per = 1;
    for (int i = 0; i < info.sub_face_depth; ++i) per *= 4;
    out.triangles = info.face_colors.size();
    out.leaves    = out.triangles * per;

    // The spools a user would have loaded: four, none of them a match for the texture, so the
    // matcher has to add slots - which is what a real import does.
    const std::vector<RGBA> existing = {RGBA{1.f, 1.f, 1.f, 1.f}, RGBA{0.f, 0.f, 0.f, 1.f},
                                        RGBA{0.85f, 0.1f, 0.1f, 1.f}, RGBA{0.1f, 0.2f, 0.8f, 1.f}};
    ObjColorMatchResult match;
    if (!obj_color_auto_match(info.face_colors, false, existing, match))
        return false;
    if (match.filament_ids.size() != info.face_colors.size())
        return false;

    auto slot_color = [&](int id) -> RGBA {
        if (id >= 1 && size_t(id) <= existing.size())
            return existing[size_t(id) - 1];
        const size_t k = size_t(id) - 1 - existing.size();
        return k < match.added_colors.size() ? match.added_colors[k] : RGBA{0.5f, 0.5f, 0.5f, 1.f};
    };

    // --- the old path: one filament per triangle -------------------------------------------
    {
        Model       m;
        GltfInfo    i2;
        std::string msg2;
        if (!load_gltf(path.c_str(), &m, i2, msg2))
            return false;
        if (!Model::import_multi_volume_face_color_deal(match.filament_ids, match.first_extruder_id, &m))
            return false;
        const ModelVolume *v = m.objects.front()->volumes.front();
        TriangleSelector   sel(v->mesh());
        sel.deserialize(v->mmu_segmentation_facets.get_data(), true);
        std::vector<int> used;
        double sum = 0.;
        for (size_t t = 0; t < out.triangles; ++t) {
            const int id = int(match.filament_ids[t]);
            if (id > 1) {
                ++out.painted_old;
                if (std::find(used.begin(), used.end(), id) == used.end()) used.push_back(id);
            }
            const RGBA c = slot_color(id);
            for (size_t k = 0; k < per; ++k) {
                const float d = obj_color_distance(info.sub_face_colors[t * per + k], c);
                sum += double(d);
                out.max_de_old = std::max(out.max_de_old, double(d));
            }
        }
        out.painted_old *= per;   // in leaves, so the two sides are comparable
        out.filaments_old = used.size();
        out.mean_de_old   = sum / double(out.leaves);
    }

    // --- the new path: one filament per sub-facet --------------------------------------------
    {
        Model       m;
        GltfInfo    i2;
        std::string msg2;
        if (!load_gltf(path.c_str(), &m, i2, msg2))
            return false;
        if (!Model::import_multi_volume_face_color_deal(match.filament_ids, i2.face_colors,
                                                        i2.sub_face_colors, i2.sub_face_depth,
                                                        match.first_extruder_id, &m))
            return false;
        const ModelVolume     *v = m.objects.front()->volumes.front();
        TriangleMesh           tm(v->mesh());
        const std::vector<int> states = leaf_states(tm, v->mmu_segmentation_facets.get_data(),
                                                    out.triangles, info.sub_face_depth);
        std::vector<int> used;
        double           sum = 0.;
        for (size_t i = 0; i < out.leaves; ++i) {
            const int id = states[i];
            if (id > 1) {
                ++out.painted_new;
                if (std::find(used.begin(), used.end(), id) == used.end()) used.push_back(id);
            }
            const float d = obj_color_distance(info.sub_face_colors[i], slot_color(id));
            sum += double(d);
            out.max_de_new = std::max(out.max_de_new, double(d));
        }
        out.filaments_new = used.size();
        out.mean_de_new   = sum / double(out.leaves);
    }
    return true;
}

} // namespace

TEST_CASE("Image Fill: a textured GLB is at least as good through the new path", "[imagefill][glb]")
{
    for (const char *file : {"agent_plaque.glb", "agent_medallion.glb"}) {
        GlbBeforeAfter r;
        INFO("file " << file);
        REQUIRE(measure_glb(file, r));

        // Printed so the numbers land in the run log and can be quoted in the status document.
        WARN("GLB before/after " << file << ": triangles=" << r.triangles << " leaves=" << r.leaves
             << " | OLD painted=" << r.painted_old << " filaments=" << r.filaments_old
             << " meanDE=" << r.mean_de_old << " maxDE=" << r.max_de_old
             << " | NEW painted=" << r.painted_new << " filaments=" << r.filaments_new
             << " meanDE=" << r.mean_de_new << " maxDE=" << r.max_de_new);

        // "At least as good" means the colour error, not the filament count: a search that
        // reproduces a texture with SIX spools where the incumbent needed eight has done better,
        // not worse, so counting filaments is not the bar. What is:
        //   - every leaf still gets a filament (nothing is left unpainted that used to be),
        //   - the mean error against the true texture colour does not rise,
        //   - and neither does the worst single leaf.
        CHECK(r.painted_new >= r.painted_old);
        CHECK(r.mean_de_new <= r.mean_de_old);
        CHECK(r.max_de_new <= r.max_de_old);
        CHECK(r.filaments_new >= 2);
    }
}

// =============================================================================================
// 7. Bar B: the project a real slice is run on
// =============================================================================================
//
// Bar B is "a real slice of an applied image on a 3-filament setup, previewed via --export-3mf".
// The slicer's CLI has no Image fill dialog - Snapmaker_Orca.cpp hands read_from_file a null
// ObjImportColorFn - so the project has to be made here, by the real image_fill_apply, and sliced
// from disk afterwards. This case is the maker: it writes the project and asserts the painting is
// what the slice is supposed to show, so the file the CLI slices is never a mystery.
//
// Run it on its own with:  libslic3r_tests.exe "[barb]"
// It leaves %TEMP%\snorca_tests\image_fill_bar_b.3mf behind on purpose.
TEST_CASE("Image Fill: write the Bar B project - a cube with a three-colour image on its top face",
          "[imagefill][barb]")
{
    // Three filaments, the three the image is made of, so the answer is unambiguous and a tool
    // change has to happen wherever the picture changes band.
    const std::vector<std::array<float, 3>> three = {{0.85f, 0.15f, 0.15f},
                                                     {0.15f, 0.75f, 0.25f},
                                                     {0.15f, 0.25f, 0.85f}};
    const std::vector<int> ids = {1, 2, 3};

    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "image fill cube";
    // 30 mm so the bands are several millimetres wide and survive the slicer's own resolution.
    ModelVolume *volume = object->add_volume(make_cube(30., 30., 30.));
    volume->name        = "cube";
    object->add_instance();
    object->ensure_on_bed();

    const std::string sha = model.image_assets.add(read_fixture("stripes3.png"));

    ImageFillParams p;
    p.asset       = sha;
    p.projection  = ImageFillProjection::Planar;
    p.axis        = ImageFillAxis::Z;   // the picture lies on the top face, u from x, v from y
    p.subdivision = 4;                  // 16 x 16 leaves per facet: the bands land cleanly
    p.allowed     = ids;

    const ImageFillResult res = image_fill_apply(*volume, p, model.image_assets, three, ids);
    REQUIRE(res.ok);
    // stripes3.png is three horizontal bands, so all three filaments must appear...
    CHECK(res.filaments_used == std::vector<int>{1, 2, 3});
    // ...and the whole surface is painted, because every facet of a cube gets a sample.
    CHECK(res.facets_painted == res.leaves_total);
    CHECK(res.leaves_total == volume->mesh().its.indices.size() * 256);

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string out = (tmp_root / "image_fill_bar_b.3mf").string();

    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    StoreParams        sp;
    sp.path     = out.c_str();
    sp.model    = &model;
    sp.config   = &cfg;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(sp));
    WARN("Bar B project written to " << out << " (" << res.facets_painted << " painted facets, "
         << res.filaments_used.size() << " filaments, subdivision " << res.subdivision_used << ")");
}
