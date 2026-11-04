// paint_v1 (pen colouring H1), the read side of tablet paint sessions: the JSON of GET
// /api/paint/objects and GET /api/paint/sessions/{id}, the psmesh1 / pspaint1 bodies, the hashes and
// the revision. The hub's allow-list and /pair entries are in remote_hub_tests.cpp ([paint_v1]).
//
// The fixtures in tests/slic3rutils/paint_v1/ are what the producers answer for one painted cube;
// the app copies them verbatim to EdgeSlicerApp tools/paint_v1/fixtures/hub. To rewrite them after a
// deliberate change: PAINT_V1_WRITE_FIXTURES=1 slic3rutils_tests "[paint_v1]".

#include <catch2/catch.hpp>

#include "slic3r/GUI/RemotePaintV1.hpp"

#include "libslic3r/ImageFill.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <map>
#include <cstring>
#include <set>
#include <sstream>
#include <string>

using namespace Slic3r;
using namespace Slic3r::GUI;
using json = nlohmann::json;
using Catch::Matchers::WithinAbs;

namespace {

const std::string FIXTURES = PAINT_V1_FIXTURES_DIR;

bool write_fixtures()
{
    const char* v = boost::nowide::getenv("PAINT_V1_WRITE_FIXTURES");
    return v != nullptr && std::string(v) == "1";
}

std::string read_file(const std::string& name)
{
    boost::nowide::ifstream in(FIXTURES + "/" + name, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const std::string& name, const std::string& data)
{
    boost::nowide::ofstream out(FIXTURES + "/" + name, std::ios::binary);
    REQUIRE(out.good());
    out << data;
    WARN("rewrote fixture " << name);
}

// Structural equality as in slice_v1_tests.cpp: the same keys, array lengths, strings and booleans,
// numbers within 1e-6.
void require_same(const json& got, const json& want, const std::string& where = "$")
{
    INFO(where << ": got " << got.dump() << ", want " << want.dump());
    if (want.is_number()) {
        REQUIRE(got.is_number());
        REQUIRE(std::abs(got.get<double>() - want.get<double>()) < 1e-6);
        return;
    }
    REQUIRE(got.type() == want.type());
    if (want.is_object()) {
        for (auto it = want.begin(); it != want.end(); ++it) {
            INFO("missing key " << it.key());
            REQUIRE(got.contains(it.key()));
            require_same(got.at(it.key()), it.value(), where + "." + it.key());
        }
        for (auto it = got.begin(); it != got.end(); ++it) {
            INFO("unexpected key " << it.key());
            REQUIRE(want.contains(it.key()));
        }
    } else if (want.is_array()) {
        REQUIRE(got.size() == want.size());
        for (size_t i = 0; i < want.size(); ++i)
            require_same(got.at(i), want.at(i), where + "[" + std::to_string(i) + "]");
    } else {
        REQUIRE(got == want);
    }
}

void compare_json_fixture(const json& got, const std::string& name)
{
    if (write_fixtures())
        write_file(name, got.dump(2) + "\n");
    require_same(got, json::parse(read_file(name)));
}

uint32_t u32_at(const std::string& b, size_t at)
{
    REQUIRE(at + 4 <= b.size());
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= uint32_t(uint8_t(b[at + i])) << (8 * i);
    return v;
}

float f32_at(const std::string& b, size_t at)
{
    const uint32_t u = u32_at(b, at);
    float          f;
    std::memcpy(&f, &u, 4);
    return f;
}

// The facet of the cube's top (z = 20) that contains (x, y).
int top_facet_at(const indexed_triangle_set& its, float x, float y)
{
    for (size_t t = 0; t < its.indices.size(); ++t) {
        if (its_face_normal(its, int(t)).z() < 0.9f)
            continue;
        const Vec3f a = its.vertices[its.indices[t][0]], b = its.vertices[its.indices[t][1]], c = its.vertices[its.indices[t][2]];
        auto side = [x, y](const Vec3f& p, const Vec3f& q) { return (q.x() - p.x()) * (y - p.y()) - (q.y() - p.y()) * (x - p.x()); };
        const float s0 = side(a, b), s1 = side(b, c), s2 = side(c, a);
        if ((s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0))
            return int(t);
    }
    return -1;
}

// One object "Cube": a 20 mm cube (volume 0, filament 1) painted with filament 2 on facet 0 and a
// 2 mm brush dot of filament 3 on its top, and a 5 mm modifier cube (volume 1). One instance at
// (100, 100, 0).
struct PaintedCube
{
    Model        model;
    ModelObject* object { nullptr };
    int          dot_facet { -1 };

    PaintedCube()
    {
        object       = model.add_object();
        object->name = "Cube";
        object->config.set_key_value("extruder", new ConfigOptionInt(1));
        ModelVolume* part = object->add_volume(TriangleMesh(its_make_cube(20, 20, 20)), ModelVolumeType::MODEL_PART, false);
        part->name        = "Cube";
        ModelVolume* mod  = object->add_volume(TriangleMesh(its_make_cube(5, 5, 5)), ModelVolumeType::PARAMETER_MODIFIER, false);
        mod->name         = "Modifier";
        ModelInstance* inst = object->add_instance();
        inst->set_offset(Vec3d(100, 100, 0));

        const indexed_triangle_set& its = part->mesh().its;
        dot_facet                       = top_facet_at(its, 5.f, 6.f);
        REQUIRE(dot_facet >= 0);
        REQUIRE(dot_facet != 0);
        TriangleSelector sel(part->mesh());
        sel.set_facet(0, EnforcerBlockerType::Extruder2);
        const Vec3f dot(5.f, 6.f, 20.f);
        sel.select_patch(dot_facet,
                         TriangleSelector::SinglePointCursor::cursor_factory(dot, Vec3f(5.f, 6.f, 100.f), 2.f,
                                                                             TriangleSelector::CursorType::CIRCLE, Transform3d::Identity(),
                                                                             TriangleSelector::ClippingPlane()),
                         EnforcerBlockerType::Extruder3, Transform3d::Identity(), true);
        part->mmu_segmentation_facets.set(sel);
    }

    PaintV1::ObjectSnapshot snapshot(bool with_paint = true) const { return PaintV1::snapshot(*object, 0, 0, with_paint); }
};

std::vector<PaintV1::PaletteSlot> palette()
{
    return { { "#FFFFFF", "PLA", "Generic PLA" }, { "#FF0000", "PLA", "Generic PLA" }, { "#0000FF", "PETG", "Generic PETG" } };
}

json paint_depth()
{
    DynamicPrintConfig print = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig object;
    object.set_key_value("paint_depth_mm", new ConfigOptionFloat(1.2));
    return PaintV1::paint_depth_json(print, object);
}

// Keys are ObjectIDs, which depend on what else the test run made: the fixtures say "o?".
json without_keys(json j)
{
    if (j.contains("objects"))
        for (json& o : j["objects"])
            o["key"] = "o?";
    if (j.contains("object"))
        j["object"]["key"] = "o?";
    return j;
}

} // namespace

TEST_CASE("paint_v1: the object list", "[paint_v1]")
{
    PaintedCube c;
    const json  j = PaintV1::objects_json({ c.snapshot(false) });
    REQUIRE(j["objects"].size() == 1);
    const json& o = j["objects"][0];
    CHECK(o["key"].get<std::string>() == PaintV1::object_key(*c.object));
    CHECK(o["key"].get<std::string>().front() == 'o');
    CHECK(o["triangles"] == 12);  // the modifier is not counted
    CHECK(o["painted"] == true);
    CHECK(o["airbrushed"] == false);
    CHECK(o["paintable"] == true);
    CHECK(o["size_estimate"] == 12 + 8 * 12 + 12 * 12);
    CHECK(o["volumes"][1]["type"] == "modifier");
    CHECK(j["max_triangles"] == PaintV1::MAX_TRIANGLES);
    compare_json_fixture(without_keys(j), "objects.json");
}

TEST_CASE("paint_v1: an object without its own filament reports filament 1", "[paint_v1]")
{
    PaintedCube c;
    c.object->config.erase("extruder");
    const PaintV1::ObjectSnapshot s = c.snapshot(false);
    CHECK(s.extruder == 1);
    CHECK(s.volumes[0].extruder == 1);
    c.object->config.set_key_value("extruder", new ConfigOptionInt(3));
    CHECK(c.snapshot(false).volumes[0].extruder == 3);
    c.object->volumes[0]->config.set_key_value("extruder", new ConfigOptionInt(2));
    CHECK(c.snapshot(false).volumes[0].extruder == 2);
}

TEST_CASE("paint_v1: an object over the triangle cap is listed but not paintable", "[paint_v1]")
{
    TriangleMesh big;
    big.its.vertices.resize(3, Vec3f::Zero());
    big.its.indices.assign(PaintV1::MAX_TRIANGLES + 1, stl_triangle_vertex_indices(0, 1, 2));
    PaintV1::ObjectSnapshot o;
    o.key = "o1";
    PaintV1::VolumeSnapshot v;
    v.type = ModelVolumeType::MODEL_PART;
    v.mesh = std::make_shared<const TriangleMesh>(std::move(big));
    o.volumes.push_back(v);
    json j = PaintV1::objects_json({ o });
    CHECK(j["objects"][0]["paintable"] == false);
    CHECK(j["objects"][0]["reason"] == "too many triangles");

    PaintV1::ObjectSnapshot empty;
    empty.key = "o2";
    j         = PaintV1::objects_json({ empty });
    CHECK(j["objects"][0]["paintable"] == false);
    CHECK(j["objects"][0]["reason"] == "no model parts");
}

TEST_CASE("paint_v1: the session answer", "[paint_v1]")
{
    PaintedCube                   c;
    const PaintV1::ObjectSnapshot s   = c.snapshot();
    const std::string             rev = PaintV1::revision(s, palette());
    CHECK(rev.size() == 16);
    const json j = PaintV1::session_json("0123456789abcdef", rev, s, palette(), paint_depth());
    REQUIRE(j["volumes"].size() == 1); // model parts only
    CHECK(j["volumes"][0]["index"] == 0);
    CHECK(j["volumes"][0]["mesh_hash"] == PaintV1::mesh_hash(c.object->volumes[0]->mesh().its));
    CHECK(j["instances"][0][12] == 100); // column-major: the offset is entries 12..14
    CHECK(j["palette"][2]["id"] == 3);
    CHECK_THAT(j["paint_depth"]["values"]["paint_depth_mm"].get<double>(), WithinAbs(1.2, 1e-9)); // the object's own
    CHECK(j["paint_depth"]["values"].contains("paint_depth_mode"));     // else the preset's
    CHECK(j["caps"]["leaves"] == PaintV1::MAX_LEAVES);
    compare_json_fixture(without_keys(j), "session.json");
}

TEST_CASE("paint_v1: the mesh body and its hash", "[paint_v1]")
{
    PaintedCube                 c;
    const indexed_triangle_set& its  = c.object->volumes[0]->mesh().its;
    const std::string           body = PaintV1::encode_mesh(its);
    REQUIRE(body.size() == 12 + its.vertices.size() * 12 + its.indices.size() * 12);
    CHECK(body.substr(0, 4) == "PSM1");
    CHECK(u32_at(body, 4) == its.vertices.size());
    CHECK(u32_at(body, 8) == its.indices.size());
    for (size_t i = 0; i < its.vertices.size(); ++i)
        for (int k = 0; k < 3; ++k)
            CHECK(f32_at(body, 12 + 12 * i + 4 * k) == its.vertices[i][k]);
    const size_t at = 12 + 12 * its.vertices.size();
    for (size_t t = 0; t < its.indices.size(); ++t)
        for (int k = 0; k < 3; ++k)
            CHECK(u32_at(body, at + 12 * t + 4 * k) == uint32_t(its.indices[t][k]));

    // The hash is SHA-256 of the body after the header, streamed or not, cached or not.
    const std::string h = image_fill_sha256_hex(reinterpret_cast<const uint8_t*>(body.data()) + 12, body.size() - 12);
    CHECK(PaintV1::mesh_hash(its) == h);
    CHECK(PaintV1::mesh_hash(c.object->volumes[0]->mesh_ptr()) == h);
    CHECK(PaintV1::mesh_hash(c.object->volumes[0]->mesh_ptr()) == h);
    CHECK(PaintV1::mesh_hash(c.object->volumes[1]->mesh_ptr()) != h);

    if (write_fixtures())
        write_file("cube_mesh.psmesh", body);
    CHECK(read_file("cube_mesh.psmesh") == body);
}

TEST_CASE("paint_v1: the painting body", "[paint_v1]")
{
    PaintedCube                   c;
    const PaintV1::ObjectSnapshot s    = c.snapshot();
    const std::string             body = PaintV1::encode_paint(*s.volumes[0].mesh, s.volumes[0].paint);
    CHECK(body.substr(0, 4) == "PSP1");
    REQUIRE(u32_at(body, 4) == 2);
    size_t                at = 8;
    std::map<uint32_t, std::set<uint32_t>> sources;
    std::map<uint32_t, double>             area;
    for (int k = 0; k < 2; ++k) {
        const uint32_t state = u32_at(body, at), n = u32_at(body, at + 4);
        at += 8;
        REQUIRE(n > 0);
        for (uint32_t t = 0; t < n; ++t) {
            Vec3f p[3];
            for (int v = 0; v < 3; ++v)
                p[v] = Vec3f(f32_at(body, at + 36 * t + 12 * v), f32_at(body, at + 36 * t + 12 * v + 4), f32_at(body, at + 36 * t + 12 * v + 8));
            area[state] += 0.5 * (p[1] - p[0]).cross(p[2] - p[0]).norm();
        }
        at += 36 * n;
        for (uint32_t t = 0; t < n; ++t)
            sources[state].insert(u32_at(body, at + 4 * t));
        at += 4 * n;
    }
    CHECK(at == body.size());
    // Filament 2: facet 0 whole (half of a 20 mm face).
    CHECK(sources[2] == std::set<uint32_t> { 0 });
    CHECK_THAT(area[2], WithinAbs(200., 1e-3));
    // Filament 3: the dot, split pieces of top facets only, roughly a 2 mm disc.
    REQUIRE(sources[3].count(uint32_t(c.dot_facet)) == 1);
    for (uint32_t f : sources[3])
        CHECK(its_face_normal(s.volumes[0].mesh->its, int(f)).z() > 0.9f);
    CHECK(area[3] > 0.5 * M_PI * 4.);
    CHECK(area[3] < 2.0 * M_PI * 4.);

    // Unpainted: just the header.
    const std::string none = PaintV1::encode_paint(*s.volumes[0].mesh, TriangleSelector::TriangleSplittingData());
    CHECK(none.size() == 8);
    CHECK(u32_at(none, 4) == 0);

    // Compared by value: splitting is float work, and the Windows and Mac builds need not agree to
    // the last bit on every corner.
    if (write_fixtures())
        write_file("cube_paint.pspaint", body);
    const std::string want = read_file("cube_paint.pspaint");
    REQUIRE(want.size() == body.size());
    for (size_t i = 0; i < body.size(); i += 4) {
        if (u32_at(want, i) == u32_at(body, i))
            continue;
        INFO("byte " << i);
        CHECK_THAT(f32_at(body, i), WithinAbs(f32_at(want, i), 1e-5));
    }
}

TEST_CASE("paint_v1: the revision follows paint, meshes and the palette, not transforms", "[paint_v1]")
{
    PaintedCube       c;
    const std::string rev = PaintV1::revision(c.snapshot(), palette());
    CHECK(PaintV1::revision(c.snapshot(), palette()) == rev);

    // moving the instance: same
    c.object->instances[0]->set_offset(Vec3d(50, 50, 0));
    CHECK(PaintV1::revision(c.snapshot(), palette()) == rev);

    // a filament colour: changed
    auto other = palette();
    other[1].color = "#00FF00";
    CHECK(PaintV1::revision(c.snapshot(), other) != rev);

    // the paint: changed
    const std::string painted = PaintV1::paint_hash(c.object->volumes[0]->mmu_segmentation_facets.get_data());
    c.object->volumes[0]->mmu_segmentation_facets.reset();
    CHECK(PaintV1::paint_hash(c.object->volumes[0]->mmu_segmentation_facets.get_data()) != painted);
    CHECK(PaintV1::revision(c.snapshot(), palette()) != rev);

    // the volume set: changed
    PaintedCube d;
    const std::string rev_d = PaintV1::revision(d.snapshot(), palette());
    d.object->delete_volume(1);
    CHECK(PaintV1::revision(d.snapshot(), palette()) != rev_d);
}

TEST_CASE("paint_v1: session ids", "[paint_v1]")
{
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i) {
        const std::string id = PaintV1::new_session_id();
        CHECK(PaintV1::is_session_id(id));
        seen.insert(id);
    }
    CHECK(seen.size() == 100);
    CHECK_FALSE(PaintV1::is_session_id(""));
    CHECK_FALSE(PaintV1::is_session_id("0123456789ABCDEF"));
    CHECK_FALSE(PaintV1::is_session_id("0123456789abcde"));
    CHECK_FALSE(PaintV1::is_session_id("0123456789abcdeg"));
    CHECK(PaintV1::volume_type_name(ModelVolumeType::MODEL_PART) == "model_part");
    CHECK(PaintV1::volume_type_name(ModelVolumeType::PRECISE_SEAM_LEFT) == "seam_modifier");
}

// ---- H2: apply ----

namespace {

PaintV1::ApplyRequest cube_request(const PaintV1::ObjectSnapshot& s, const std::string& revision, std::vector<PaintWireTree> trees)
{
    PaintV1::ApplyRequest req;
    req.revision = revision;
    PaintV1::ApplyVolume v;
    v.index     = 0;
    v.triangles = uint32_t(s.volumes[0].triangles());
    v.mesh_hash = PaintV1::mesh_hash(s.volumes[0].mesh);
    v.trees     = std::move(trees);
    req.volumes.push_back(std::move(v));
    return req;
}

} // namespace

TEST_CASE("paint_v1 apply: the body round-trips and bad bodies are refused", "[paint_v1]")
{
    PaintedCube                   c;
    const PaintV1::ObjectSnapshot s   = c.snapshot();
    const PaintV1::ApplyRequest   req = cube_request(s, PaintV1::revision(s, palette()), { { 3, { 2 } }, { 5, { 0xFF, 1, 0, 3, 0 } } });
    const std::string             body = PaintV1::encode_apply(req);
    CHECK(body.substr(0, 4) == "PSA1");
    PaintV1::ApplyRequest back;
    std::string           error;
    REQUIRE(PaintV1::decode_apply(body, back, error));
    CHECK(back.revision == req.revision);
    REQUIRE(back.volumes.size() == 1);
    CHECK(back.volumes[0].mesh_hash == req.volumes[0].mesh_hash);
    CHECK(back.volumes[0].trees == req.volumes[0].trees);

    CHECK_FALSE(PaintV1::decode_apply("", back, error));
    CHECK_FALSE(PaintV1::decode_apply("PSA2" + body.substr(4), back, error));
    CHECK_FALSE(PaintV1::decode_apply(body.substr(0, body.size() - 1), back, error));
    CHECK_FALSE(PaintV1::decode_apply(body + "x", back, error));
    std::string upper = body;
    upper[4] = 'A'; // revisions are lowercase hex
    CHECK_FALSE(PaintV1::decode_apply(upper, back, error));
    PaintV1::ApplyRequest twice = req;
    twice.volumes.push_back(req.volumes[0]);
    CHECK_FALSE(PaintV1::decode_apply(PaintV1::encode_apply(twice), back, error));
    CHECK(error.find("twice") != std::string::npos);
    std::string huge = body.substr(0, 20) + std::string("\xff\xff\xff\x7f", 4);
    CHECK_FALSE(PaintV1::decode_apply(huge, back, error)); // a lying count is refused before allocating

    // The app's encoder is checked against this body (tools/paint_v1/fixtures/hub/apply_cube.psapply).
    if (write_fixtures())
        write_file("apply_cube.psapply", body);
    CHECK(read_file("apply_cube.psapply") == body);
}

TEST_CASE("paint_v1 apply: on the current revision the touched triangles change and nothing else", "[paint_v1]")
{
    PaintedCube                   c;
    const PaintV1::ObjectSnapshot s   = c.snapshot();
    const std::string             rev = PaintV1::revision(s, palette());
    const auto plan = PaintV1::plan_apply(cube_request(s, rev, { { 0, { 0 } }, { 3, { 2 } } }), s, rev, nullptr, 3);
    REQUIRE(plan.status == 200);
    REQUIRE(plan.paint.size() == 1);
    CHECK(plan.triangles == 2);
    const auto& before = s.volumes[0].paint;
    const auto& after  = plan.paint[0].second;
    for (int t = 0; t < 12; ++t) {
        INFO("triangle " << t);
        if (t == 0 || t == 3)
            continue;
        CHECK(paint_wire_triangle_bits(after, t) == paint_wire_triangle_bits(before, t));
    }
    CHECK(paint_wire_triangle_bits(after, 0).empty()); // cleared
    CHECK_FALSE(paint_wire_triangle_bits(after, 3).empty());
    CHECK(after.used_states[3]); // the brush dot (filament 3) stays
    CHECK(after.used_states[2]); // triangle 3 is filament 2 now

    // An empty diff changes nothing at all.
    const auto none = PaintV1::plan_apply(cube_request(s, rev, {}), s, rev, nullptr, 3);
    REQUIRE(none.status == 200);
    CHECK(none.paint[0].second == before);
}

TEST_CASE("paint_v1 apply: refusals change nothing", "[paint_v1]")
{
    PaintedCube                   c;
    const PaintV1::ObjectSnapshot s   = c.snapshot();
    const std::string             rev = PaintV1::revision(s, palette());
    auto status = [&](const PaintV1::ApplyRequest& req, size_t filaments = 3) {
        const auto plan = PaintV1::plan_apply(req, s, rev, nullptr, filaments);
        if (plan.status != 200) CHECK(plan.paint.empty());
        return plan.status;
    };
    CHECK(status(cube_request(s, rev, { { 3, { 4 } } })) == 400);                  // filament 4 of 3
    CHECK(status(cube_request(s, rev, { { 3, { 4 } } }), 4) == 200);
    CHECK(status(cube_request(s, rev, { { 3, { 0xFF, 1, 1, 1, 1 } } })) == 400);   // not in normal form
    CHECK(status(cube_request(s, rev, { { 12, { 1 } } })) == 400);                 // past the mesh
    CHECK(status(cube_request(s, rev, { { 5, { 1 } }, { 3, { 1 } } })) == 400);    // not ascending
    auto modifier = cube_request(s, rev, { { 3, { 1 } } });
    modifier.volumes[0].index = 1;
    CHECK(status(modifier) == 400);                                                // not a model part
    auto changed = cube_request(s, rev, { { 3, { 1 } } });
    changed.volumes[0].mesh_hash = std::string(64, 'a');
    CHECK(status(changed) == 409);                                                 // the mesh changed
    PaintV1::ApplyRequest empty;
    empty.revision = rev;
    CHECK(status(empty) == 400);
}

TEST_CASE("paint_v1 apply: the per-triangle merge after the PC painted too", "[paint_v1]")
{
    PaintedCube                   c;
    const PaintV1::ObjectSnapshot then     = c.snapshot();
    const std::string             then_rev = PaintV1::revision(then, palette());
    const PaintV1::PaintByVolume  base     = PaintV1::paint_by_volume(then);

    // Meanwhile the PC repaints triangle 0 (filament 2 -> 1).
    {
        TriangleSelector sel(c.object->volumes[0]->mesh());
        sel.deserialize(c.object->volumes[0]->mmu_segmentation_facets.get_data());
        sel.set_facet(0, EnforcerBlockerType::Extruder1);
        c.object->volumes[0]->mmu_segmentation_facets.set(sel);
    }
    const PaintV1::ObjectSnapshot now     = c.snapshot();
    const std::string             now_rev = PaintV1::revision(now, palette());
    REQUIRE(now_rev != then_rev);

    // The tablet painted triangle 3 only: merged onto the PC's new paint.
    const auto merged = PaintV1::plan_apply(cube_request(then, then_rev, { { 3, { 2 } } }), now, now_rev, &base, 3);
    REQUIRE(merged.status == 200);
    CHECK(paint_wire_triangle_bits(merged.paint[0].second, 0) == paint_wire_triangle_bits(now.volumes[0].paint, 0));

    // The tablet also painted triangle 0: refused, naming it.
    const auto clash = PaintV1::plan_apply(cube_request(then, then_rev, { { 0, { 3 } }, { 3, { 2 } } }), now, now_rev, &base, 3);
    REQUIRE(clash.status == 409);
    CHECK(clash.paint.empty());
    CHECK(clash.answer["revision"] == now_rev);
    CHECK(clash.answer["conflicts"] == json::parse(R"([{"volume":0,"triangles":[0]}])"));

    // A revision this hub never served: it cannot tell, so it asks for a re-read.
    const auto unknown = PaintV1::plan_apply(cube_request(then, then_rev, { { 3, { 2 } } }), now, now_rev, nullptr, 3);
    CHECK(unknown.status == 409);
    CHECK_FALSE(unknown.answer.contains("conflicts"));
}
