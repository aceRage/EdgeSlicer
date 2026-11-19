// Paint wire v0 (pen colouring spike S0): the app's quadtree converts to MMU painting exactly.
// Spec: EdgeSlicerApp docs/paint_wire_v0.md.
#include <catch2/catch.hpp>

#include "libslic3r/PaintWire.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Format/OBJ.hpp"

#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <map>
#include <random>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// The spec's children of (a, b, c), written from the spec, not from the slicer.
std::array<std::array<Vec3f, 3>, 4> spec_children(const Vec3f &a, const Vec3f &b, const Vec3f &c)
{
    const Vec3f m01 = (a + b) * 0.5f, m12 = (b + c) * 0.5f, m20 = (c + a) * 0.5f;
    return {{{a, m01, m20}, {m01, b, m12}, {m12, c, m20}, {m01, m12, m20}}};
}

float tri_area(const Vec3f &a, const Vec3f &b, const Vec3f &c) { return 0.5f * (b - a).cross(c - a).norm(); }

// Area per state of one tree on (a, b, c), from the spec's geometry.
size_t tree_area(const std::vector<uint8_t> &bytes, size_t pos, const Vec3f &a, const Vec3f &b, const Vec3f &c,
                 std::map<int, double> &area)
{
    const uint8_t n = bytes[pos++];
    if (n != PAINT_WIRE_SPLIT) {
        area[n] += tri_area(a, b, c);
        return pos;
    }
    for (const auto &ch : spec_children(a, b, c))
        pos = tree_area(bytes, pos, ch[0], ch[1], ch[2], area);
    return pos;
}

// A random tree in normal form.
void random_tree(std::mt19937 &rng, int depth, int max_depth, int n_states, std::vector<uint8_t> &out)
{
    std::uniform_int_distribution<int> state(0, n_states);
    if (depth >= max_depth || std::uniform_int_distribution<int>(0, 2)(rng) == 0) {
        out.push_back(uint8_t(state(rng)));
        return;
    }
    const size_t start = out.size();
    out.push_back(PAINT_WIRE_SPLIT);
    for (int i = 0; i < 4; ++i)
        random_tree(rng, depth + 1, max_depth, n_states, out);
    // Collapse four equal leaves, so the tree stays in normal form.
    if (out.size() == start + 5 && out[start + 1] == out[start + 2] && out[start + 1] == out[start + 3] &&
        out[start + 1] == out[start + 4]) {
        const uint8_t s = out[start + 1];
        out.resize(start);
        out.push_back(s);
    }
}

// Area per original triangle per state, as the PC's own selector sees the painting.
std::map<int, std::map<int, double>> selector_areas(const TriangleMesh &mesh, const TriangleSelector::TriangleSplittingData &data)
{
    std::map<int, std::map<int, double>> out;
    for (const PaintWireDisplay &d : paint_wire_display(mesh, data))
        for (size_t k = 0; k < d.source.size(); ++k)
            out[d.source[k]][d.state] += tri_area(d.vertices[3 * k], d.vertices[3 * k + 1], d.vertices[3 * k + 2]);
    return out;
}

TriangleMesh load_test_obj(const char *name)
{
    TriangleMesh mesh;
    ObjInfo      info;
    std::string  message;
    REQUIRE(load_obj((std::string(TEST_DATA_DIR) + "/" + name).c_str(), &mesh, info, message));
    return mesh;
}

std::vector<uint8_t> unbase64(const std::string &s)
{
    std::vector<uint8_t> out(boost::beast::detail::base64::decoded_size(s.size()));
    out.resize(boost::beast::detail::base64::decode(out.data(), s.data(), s.size()).first);
    return out;
}

// The PC's bitstream for one triangle, read back into the wire's pre-order bytes. Only the 4-way
// split (three sides, special side 0) is accepted, which is all the wire ever writes.
bool bits_to_tree(const TriangleSelector::TriangleSplittingData &data, int &ibit, std::vector<uint8_t> &out)
{
    int code = 0;
    if (!data.read_nibble(ibit, code))
        return false;
    const int sides = code & 0b11;
    if (sides == 0) {
        int state = 0;
        if (!data.read_leaf_state(code, ibit, state))
            return false;
        out.push_back(uint8_t(state));
        return true;
    }
    if (sides != 3 || (code >> 2) != 0)   // anything but the 4-way split with special side 0
        return false;
    std::vector<uint8_t> children[4];
    for (int i = 3; i >= 0; --i)
        if (!bits_to_tree(data, ibit, children[i]))
            return false;
    out.push_back(PAINT_WIRE_SPLIT);
    for (const auto &c : children)
        out.insert(out.end(), c.begin(), c.end());
    return true;
}

} // namespace

TEST_CASE("Paint wire: the spec's 4-way split is the PC's own split", "[PaintWire]")
{
    // One triangle; paint each child alone and ask the PC's selector where the paint landed.
    indexed_triangle_set its;
    its.vertices = {Vec3f(0, 0, 0), Vec3f(8, 0, 0), Vec3f(1, 6, 0)};
    its.indices  = {stl_triangle_vertex_indices(0, 1, 2)};
    const TriangleMesh mesh(its);
    const auto         kids = spec_children(its.vertices[0], its.vertices[1], its.vertices[2]);
    for (int child = 0; child < 4; ++child) {
        std::vector<uint8_t> tree{PAINT_WIRE_SPLIT, 0, 0, 0, 0};
        tree[1 + child] = 2;
        TriangleSelector::TriangleSplittingData data;
        std::string                             error;
        REQUIRE(paint_wire_apply(1, {{0, tree}}, {}, data, error));
        const auto shown = paint_wire_display(mesh, data);
        REQUIRE(shown.size() == 1);
        REQUIRE(shown[0].vertices.size() == 3);
        const Vec3f got  = (shown[0].vertices[0] + shown[0].vertices[1] + shown[0].vertices[2]) / 3.f;
        const Vec3f want = (kids[child][0] + kids[child][1] + kids[child][2]) / 3.f;
        INFO("child " << child);
        CHECK((got - want).norm() < 1e-5f);
    }
}

TEST_CASE("Paint wire: the spec's byte examples", "[PaintWire]")
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(0, 0, 0), Vec3f(4, 0, 0), Vec3f(0, 4, 0)};
    its.indices  = {stl_triangle_vertex_indices(0, 1, 2)};
    const TriangleMesh mesh(its);
    const double       whole = 8.0;
    struct Case { std::vector<uint8_t> bytes; int state; double share; };
    const Case cases[] = {
        {{0x02}, 2, 1.0},
        {{0xFF, 0x01, 0x00, 0x00, 0x00}, 1, 0.25},
        {{0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x03, 0x00, 0x00}, 3, 1.0 / 16.0},
    };
    for (const Case &c : cases) {
        TriangleSelector::TriangleSplittingData data;
        std::string                             error;
        REQUIRE(paint_wire_apply(1, {{0, c.bytes}}, {}, data, error));
        // The PC's reader and writer give back the same bits.
        TriangleSelector selector(mesh);
        selector.deserialize(data);
        CHECK(selector.serialize() == data);
        const auto areas = selector_areas(mesh, data);
        CHECK_THAT(areas.at(0).at(c.state), WithinRel(whole * c.share, 1e-6));
    }
}

TEST_CASE("Paint wire: bad trees are refused", "[PaintWire]")
{
    std::string error;
    CHECK_FALSE(paint_wire_check_tree({0xFF, 1, 1, 1, 1}, error));   // not normal form
    CHECK_FALSE(paint_wire_check_tree({0xFF, 1, 1}, error));         // truncated
    CHECK_FALSE(paint_wire_check_tree({1, 1}, error));               // trailing byte
    std::vector<uint8_t> deep;
    for (int i = 0; i <= PAINT_WIRE_MAX_DEPTH; ++i)
        deep.insert(deep.end(), {0xFF, 0, 0, 0});
    deep.push_back(1);
    CHECK_FALSE(paint_wire_check_tree(deep, error));
    CHECK(paint_wire_check_tree({0xFF, 1, 1, 1, 0xFF, 1, 2, 2, 2}, error));

    TriangleSelector::TriangleSplittingData out;
    CHECK_FALSE(paint_wire_apply(4, {{2, {1}}, {1, {1}}}, {}, out, error));   // not ascending
    CHECK_FALSE(paint_wire_apply(4, {{4, {1}}}, {}, out, error));             // out of range
}

TEST_CASE("Paint wire: random trees match the PC's selector exactly", "[PaintWire]")
{
    const TriangleMesh mesh = load_test_obj("pyramid.obj");
    const auto        &its  = mesh.its;
    std::mt19937       rng(20261101);
    for (int round = 0; round < 20; ++round) {
        std::vector<PaintWireTree> trees;
        for (size_t t = 0; t < its.indices.size(); ++t) {
            if (std::uniform_int_distribution<int>(0, 3)(rng) == 0)
                continue;
            std::vector<uint8_t> tree;
            random_tree(rng, 0, 5, 20, tree);
            trees.emplace_back(int(t), tree);
        }
        TriangleSelector::TriangleSplittingData data;
        std::string                             error;
        REQUIRE(paint_wire_apply(its.indices.size(), trees, {}, data, error));

        TriangleSelector selector(mesh);
        selector.deserialize(data);
        CHECK(selector.serialize() == data);

        const auto got = selector_areas(mesh, data);
        for (const PaintWireTree &tree : trees) {
            const auto           &f = its.indices[size_t(tree.first)];
            std::map<int, double> want;
            tree_area(tree.second, 0, its.vertices[f[0]], its.vertices[f[1]], its.vertices[f[2]], want);
            for (const auto &[state, area] : want) {
                if (state == 0)
                    continue;
                INFO("round " << round << " triangle " << tree.first << " state " << state);
                REQUIRE(got.count(tree.first));
                CHECK_THAT(got.at(tree.first).at(state), WithinRel(area, 1e-5));
            }
        }
    }
}

TEST_CASE("Paint wire: unlisted triangles keep the PC's paint bit for bit", "[PaintWire]")
{
    const TriangleMesh mesh = load_test_obj("pyramid.obj");
    const size_t       n    = mesh.its.indices.size();
    std::mt19937       rng(7);
    std::vector<PaintWireTree> before;
    for (size_t t = 0; t < n; ++t) {
        std::vector<uint8_t> tree;
        random_tree(rng, 0, 4, 6, tree);
        before.emplace_back(int(t), tree);
    }
    TriangleSelector::TriangleSplittingData existing, after;
    std::string                             error;
    REQUIRE(paint_wire_apply(n, before, {}, existing, error));
    // Repaint triangle 1, clear triangle 2, leave the rest.
    REQUIRE(paint_wire_apply(n, {{1, {5}}, {2, {0}}}, existing, after, error));

    std::vector<PaintWireTree> expected = before;
    expected[1].second = {5};
    expected[2].second = {0};
    TriangleSelector::TriangleSplittingData want;
    REQUIRE(paint_wire_apply(n, expected, {}, want, error));
    CHECK(after == want);
}

TEST_CASE("Paint wire: one triangle's bits and the highest state", "[PaintWire]")
{
    const TriangleMesh mesh = load_test_obj("pyramid.obj");
    const size_t       n    = mesh.its.indices.size();
    TriangleSelector::TriangleSplittingData data;
    std::string                             error;
    // Triangle 0 split (corner in 1), triangle 2 whole in 3, the rest unpainted.
    REQUIRE(paint_wire_apply(n, {{0, {0xFF, 1, 0, 0, 0}}, {2, {3}}}, {}, data, error));
    CHECK(paint_wire_triangle_bits(data, 1).empty()); // an unpainted leaf is not stored
    CHECK_FALSE(paint_wire_triangle_bits(data, 0).empty());
    CHECK_FALSE(paint_wire_triangle_bits(data, 2).empty());
    CHECK(paint_wire_triangle_bits(data, 0) != paint_wire_triangle_bits(data, 2));
    CHECK(paint_wire_triangle_bits(data, 99).empty());

    // The same painting made again has the same bits; a repaint of one triangle changes only its own.
    TriangleSelector::TriangleSplittingData again, repainted;
    REQUIRE(paint_wire_apply(n, {{0, {0xFF, 1, 0, 0, 0}}, {2, {3}}}, {}, again, error));
    REQUIRE(paint_wire_apply(n, {{2, {4}}}, data, repainted, error));
    for (int t = 0; t < int(n); ++t)
        CHECK(paint_wire_triangle_bits(again, t) == paint_wire_triangle_bits(data, t));
    CHECK(paint_wire_triangle_bits(repainted, 0) == paint_wire_triangle_bits(data, 0));
    CHECK(paint_wire_triangle_bits(repainted, 2) != paint_wire_triangle_bits(data, 2));

    CHECK(paint_wire_max_state({}, error) == 0);
    CHECK(paint_wire_max_state({{0, {0}}}, error) == 0);
    CHECK(paint_wire_max_state({{0, {0xFF, 1, 0, 7, 0}}, {1, {2}}}, error) == 7);
    CHECK(paint_wire_max_state({{3, {0xFF, 1}}}, error) == -1);
    CHECK(error.find("triangle 3") != std::string::npos);
}
