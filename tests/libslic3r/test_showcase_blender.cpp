// Showcase realism phase 4: the Blender package (ShowcaseBlender.hpp), its GLB writer and reader
// (ShowcaseGltf.hpp) and the toolpath tubes (ShowcaseTubes.hpp): the schema, the material mapping,
// the units, and that the same input always gives the same bytes.
//
// With EDGESLICER_BLENDER_FIXTURE_OUT set to a folder, the "[ShowcaseBlender][fixture]" case also
// writes a model package and a toolpath package there for tests/blender/run_blender_tests.py (and
// EDGESLICER_BLENDER_FIXTURE_HDRI to an .hdr file lights them with it).

#include <catch2/catch.hpp>

#include "libslic3r/Showcase/ShowcaseBlender.hpp"
#include "libslic3r/Showcase/ShowcaseGltf.hpp"
#include "libslic3r/Showcase/ShowcaseHdr.hpp"
#include "libslic3r/Showcase/ShowcasePath.hpp"
#include "libslic3r/Showcase/ShowcaseTubes.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r::Showcase;

namespace {

constexpr double SB_PI = 3.14159265358979323846;

// An axis-aligned box, world mm, outward winding.
void sb_box(float x0, float y0, float z0, float x1, float y1, float z1, std::vector<float> &v, std::vector<uint32_t> &f)
{
    const uint32_t b = uint32_t(v.size() / 3);
    const float c[8][3] = { { x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 },
                            { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 } };
    for (auto &p : c)
        v.insert(v.end(), { p[0], p[1], p[2] });
    const uint32_t q[6][4] = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
    for (auto &k : q)
        f.insert(f.end(), { b + k[0], b + k[1], b + k[2], b + k[0], b + k[2], b + k[3] });
}

// A sphere (latitude / longitude), world mm.
void sb_sphere(float cx, float cy, float cz, float r, int seg, std::vector<float> &v, std::vector<uint32_t> &f)
{
    const uint32_t b = uint32_t(v.size() / 3);
    const int rings = seg / 2;
    for (int i = 0; i <= rings; ++i)
        for (int j = 0; j < seg; ++j) {
            const double th = SB_PI * double(i) / double(rings), ph = 2. * SB_PI * double(j) / double(seg);
            v.insert(v.end(), { float(cx + r * std::sin(th) * std::cos(ph)), float(cy + r * std::sin(th) * std::sin(ph)), float(cz + r * std::cos(th)) });
        }
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < seg; ++j) {
            const uint32_t a = b + uint32_t(i * seg + j), c = b + uint32_t(i * seg + (j + 1) % seg);
            const uint32_t d = b + uint32_t((i + 1) * seg + j), e = b + uint32_t((i + 1) * seg + (j + 1) % seg);
            if (i > 0)
                f.insert(f.end(), { a, d, c });
            if (i + 1 < rings)
                f.insert(f.end(), { c, d, e });
        }
}

std::vector<uint8_t> sb_read_file(const std::string &path)
{
    boost::nowide::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

const PackageFile *sb_file(const PackageResult &r, const std::string &path)
{
    for (const PackageFile &f : r.files)
        if (f.path == path)
            return &f;
    return nullptr;
}

nlohmann::json sb_json(const PackageFile *f) { return f ? nlohmann::json::parse(std::string(f->bytes.begin(), f->bytes.end())) : nlohmann::json(); }

// A square loop of walls, `layers` layers of 0.2 mm, then a sparse infill line on each layer.
std::vector<TubeMove> sb_square_moves(int layers, int extruder = 0)
{
    std::vector<TubeMove> m;
    for (int l = 0; l < layers; ++l) {
        const float z = 0.2f * float(l + 1);
        TubeMove t;
        t.kind = TubeKind::Travel; t.x = 0.f; t.y = 0.f; t.z = z;
        m.push_back(t);
        const float pts[4][2] = { { 20.f, 0.f }, { 20.f, 20.f }, { 0.f, 20.f }, { 0.f, 0.f } };
        for (auto &p : pts) {
            TubeMove e;
            e.kind = TubeKind::Extrude; e.role = TubeRole::Wall; e.x = p[0]; e.y = p[1]; e.z = z; e.width = 0.45f; e.height = 0.2f;
            e.extruder = extruder;
            m.push_back(e);
        }
        TubeMove tr;
        tr.kind = TubeKind::Travel; tr.x = 2.f; tr.y = 10.f; tr.z = z;
        m.push_back(tr);
        TubeMove in;
        in.kind = TubeKind::Extrude; in.role = TubeRole::SparseInfill; in.x = 18.f; in.y = 10.f; in.z = z; in.width = 0.45f; in.height = 0.2f;
        in.extruder = extruder;
        m.push_back(in);
    }
    return m;
}

PackageInput sb_model_input()
{
    PackageInput in;
    in.edgeslicer_version = "test";
    in.width  = 640;
    in.height = 360;
    in.frame_height_px = 360.;
    in.scene.name = "Test scene";
    in.scene.preset_base = "studio_white";
    in.scene.camera.valid  = true;
    in.scene.camera.eye    = { 90., -140., 80. };
    in.scene.camera.target = { 10., 10., 10. };
    in.scene.camera.up     = { 0., 0., 1. };
    in.scene.camera.zoom   = 360. / 70.;   // 70 mm of the frame's height at the target
    in.scene.dof.enabled   = true;
    in.scene.dof.f_number  = 4.f;
    in.scene.lighting.environment = "hdri_studio";
    PackageFilament f1; f1.slot = 1; f1.color = "#C0C0C0"; f1.finish = Finish::Silk;        f1.preset_name = "Generic PLA Silk";
    PackageFilament f2; f2.slot = 2; f2.color = "#3070FF"; f2.finish = Finish::Transparent; f2.preset_name = "Generic PETG Transparent";
    PackageFilament f3; f3.slot = 3; f3.color = "#202020"; f3.finish = Finish::Sparkle;     f3.preset_name = "Galaxy PLA";
    in.filaments = { f1, f2, f3 };
    PackagePart cube;
    cube.name = "Cube"; cube.object_idx = 0; cube.volume_idx = 0; cube.filament = 1;
    sb_box(0.f, 0.f, 0.f, 20.f, 20.f, 20.f, cube.vertices, cube.indices);
    cube.layers.valid = true; cube.layers.uniform = true; cube.layers.first = 0.2f; cube.layers.height = 0.2f;
    PackagePart ball;
    ball.name = "Ball"; ball.object_idx = 1; ball.volume_idx = 0; ball.filament = 2;
    sb_sphere(40.f, 10.f, 10.f, 10.f, 32, ball.vertices, ball.indices);
    PackagePart bar;
    bar.name = "Bar"; bar.object_idx = 2; bar.volume_idx = 0; bar.filament = 3;
    sb_box(-25.f, 0.f, 0.f, -15.f, 10.f, 30.f, bar.vertices, bar.indices);
    in.parts = { cube, ball, bar };
    in.model_min = { -25., 0., 0. };
    in.model_max = { 50., 20., 30. };
    return in;
}

} // namespace

TEST_CASE("Units: millimetres Z up become glTF metres Y up and Blender metres Z up", "[ShowcaseBlender]")
{
    const std::array<float, 3> g = gltf_from_mm(10., 20., 30.);
    CHECK(g[0] == Approx(0.010f));
    CHECK(g[1] == Approx(0.030f));
    CHECK(g[2] == Approx(-0.020f));
    const std::array<double, 3> b = blender_from_mm({ 10., 20., 30. });
    CHECK(b[0] == Approx(0.010));
    CHECK(b[1] == Approx(0.020));
    CHECK(b[2] == Approx(0.030));
    CHECK(srgb_to_linear(0.f) == 0.f);
    CHECK(srgb_to_linear(1.f) == Approx(1.f));
    CHECK(srgb_to_linear(0.5f) == Approx(0.214f).margin(1e-3));
    CHECK(hex_to_linear("#FFFFFF")[1] == Approx(1.f));
    CHECK(hex_to_linear("nonsense", { 0.25f, 0.5f, 0.75f })[2] == 0.75f);
}

TEST_CASE("GLB: a written document validates, reads back and is deterministic", "[ShowcaseBlender][GLB]")
{
    GltfDocument doc;
    GltfMesh mesh;
    mesh.name = "box";
    std::vector<float> v;
    std::vector<uint32_t> f;
    sb_box(0.f, 0.f, 0.f, 1.f, 1.f, 1.f, v, f);
    mesh.positions = v;
    mesh.indices   = f;
    mesh.attributes.push_back({ "_BUILD", 1, std::vector<float>(8, 0.5f) });
    mesh.attributes.push_back({ "NOUNDERSCORE", 1, std::vector<float>(8, 0.5f) });   // dropped: not a custom name
    mesh.material = 0;
    doc.meshes.push_back(mesh);
    GltfMaterial mat;
    mat.name = "m";
    mat.clearcoat = 0.5f;
    mat.transmission = 0.9f;
    mat.thickness = 0.0016f;
    mat.ior = 1.57f;
    mat.anisotropy = 0.75f;
    mat.sheen_color = { 0.3f, 0.3f, 0.3f };
    mat.extras_json = R"({"edgeslicer":{"finish":"silk"}})";
    doc.materials.push_back(mat);
    GltfNode node;
    node.name = "box";
    node.mesh = 0;
    node.extras_json = R"({"edgeslicer":{"kind":"part"}})";
    doc.nodes.push_back(node);

    const std::vector<uint8_t> glb = write_glb(doc);
    REQUIRE(glb.size() % 4 == 0);
    GlbStats    st;
    std::string err;
    REQUIRE(validate_glb(glb.data(), glb.size(), &st, &err));
    CHECK(err.empty());
    CHECK(st.meshes == 1);
    CHECK(st.nodes == 1);
    CHECK(st.materials == 1);
    CHECK(st.vertices == 8);
    CHECK(st.triangles == 12);
    const std::set<std::string> ext(st.extensions_used.begin(), st.extensions_used.end());
    CHECK(ext.count("KHR_materials_clearcoat") == 1);
    CHECK(ext.count("KHR_materials_transmission") == 1);
    CHECK(ext.count("KHR_materials_volume") == 1);
    CHECK(ext.count("KHR_materials_ior") == 1);
    CHECK(ext.count("KHR_materials_anisotropy") == 1);
    CHECK(ext.count("KHR_materials_sheen") == 1);

    GlbChunks chunks;
    REQUIRE(read_glb(glb.data(), glb.size(), chunks));
    const nlohmann::json j = nlohmann::json::parse(chunks.json);
    CHECK(j["materials"][0]["extras"]["edgeslicer"]["finish"] == "silk");
    CHECK(j["nodes"][0]["extras"]["edgeslicer"]["kind"] == "part");
    CHECK(j["meshes"][0]["primitives"][0]["attributes"].contains("_BUILD"));
    CHECK_FALSE(j["meshes"][0]["primitives"][0]["attributes"].contains("NOUNDERSCORE"));
    CHECK(j["accessors"][0]["max"][0].get<double>() == Approx(1.));
    CHECK(j["materials"][0]["extensions"]["KHR_materials_ior"]["ior"].get<double>() == 1.57);   // written short, reads exact

    CHECK(write_glb(doc) == glb);

    SECTION("a broken file is refused")
    {
        std::vector<uint8_t> bad = glb;
        bad[0] = 'x';
        CHECK_FALSE(validate_glb(bad.data(), bad.size(), nullptr, &err));
        std::vector<uint8_t> cut(glb.begin(), glb.end() - 8);
        CHECK_FALSE(validate_glb(cut.data(), cut.size(), nullptr, &err));
        GltfDocument d2 = doc;
        d2.meshes[0].indices.push_back(99);   // a partial triangle is dropped by the writer...
        CHECK(validate_glb(write_glb(d2).data(), write_glb(d2).size()));
    }
}

TEST_CASE("Materials: every finish maps to sane Principled inputs", "[ShowcaseBlender][Materials]")
{
    const BlenderGlow no_glow;
    for (Finish f : all_finishes()) {
        const BlenderMaterial m = blender_material(f, "#808080", 0.5f, LayerLines::Subtle, false, no_glow, false);
        INFO("finish " << to_string(f));
        CHECK(m.finish == to_string(f));
        CHECK(m.roughness >= 0.f);
        CHECK(m.roughness <= 1.f);
        CHECK(m.ior >= 1.2f);
        CHECK(m.ior <= 3.f);
        CHECK(m.metallic >= 0.f);
        CHECK(m.metallic <= 1.f);
        CHECK(m.transmission_weight >= 0.f);
        CHECK(m.transmission_weight <= 1.f);
        CHECK(m.emission_strength == 0.f);
        CHECK(m.tangent == "radial_z");
        // The glTF fallback validates for every finish.
        GltfDocument doc;
        doc.materials.push_back(gltf_material(m, "m", ""));
        GltfMesh mesh;
        std::vector<float> v;
        std::vector<uint32_t> idx;
        sb_box(0, 0, 0, 1, 1, 1, v, idx);
        mesh.positions = v;
        mesh.indices = idx;
        mesh.material = 0;
        doc.meshes.push_back(mesh);
        doc.nodes.push_back({ "n", 0, { 0., 0., 0. }, "" });
        const std::vector<uint8_t> glb = write_glb(doc);
        CHECK(validate_glb(glb.data(), glb.size()));
        // The add-on's JSON parses.
        CHECK(nlohmann::json::parse(blender_material_json(m))["finish"] == to_string(f));
    }

    const BlenderMaterial silk = blender_material(Finish::Silk, "#C0C0C0", 0.f, LayerLines::Subtle, false, no_glow, false);
    CHECK(silk.anisotropic > 0.5f);
    CHECK(silk.specular_tint > 0.f);
    CHECK(silk.metallic == 0.f);
    CHECK(silk.layer_bump > 0.f);
    const BlenderMaterial silk_beads = blender_material(Finish::Silk, "#C0C0C0", 0.f, LayerLines::Subtle, true, no_glow, false);
    CHECK(silk_beads.tangent == "attribute");
    CHECK(silk_beads.layer_bump == 0.f);   // the beads are the layers
    CHECK(blender_material(Finish::Silk, "#C0C0C0", 0.f, LayerLines::Off, false, no_glow, false).layer_bump == 0.f);

    const BlenderMaterial clear = blender_material(Finish::Transparent, "#3070FF", 0.f, LayerLines::Off, false, no_glow, false);
    CHECK(clear.transmission_weight >= 0.9f);
    CHECK(clear.ior == Approx(1.57f));
    CHECK(clear.absorption_density > 0.f);
    CHECK(clear.thickness_m == Approx(DEFAULT_WALL_THICKNESS * 0.001f));
    const GltfMaterial clear_gltf = gltf_material(clear, "c", "");
    CHECK(clear_gltf.transmission >= 0.9f);
    CHECK(clear_gltf.attenuation_distance == Approx(1.f / clear.absorption_density));
    CHECK(clear_gltf.double_sided);

    const BlenderMaterial milky = blender_material(Finish::Translucent, "#F0F0E0", 0.f, LayerLines::Off, false, no_glow, false);
    CHECK(milky.subsurface_weight > 0.5f);
    CHECK(milky.subsurface_scale_m > 0.f);
    CHECK(milky.subsurface_scale_m < 0.01f);   // millimetres, in metres
    CHECK(milky.transmission_weight > 0.f);
    CHECK(milky.transmission_weight < clear.transmission_weight);

    // Translucency off: an ordinary PLA lets nothing through.
    CHECK(blender_material(Finish::Standard, "#808080", 0.f, LayerLines::Off, false, no_glow, false).transmission_weight == 0.f);
    CHECK(blender_material(Finish::Standard, "#808080", 0.f, LayerLines::Off, false, no_glow, false).subsurface_weight == 0.f);

    CHECK(blender_material(Finish::Metallic, "#B08040", 0.f, LayerLines::Off, false, no_glow, false).metallic > 0.5f);
    const BlenderMaterial cf = blender_material(Finish::CarbonFiber, "#202020", 0.f, LayerLines::Off, false, no_glow, false);
    CHECK(cf.fiber_noise > 0.f);
    CHECK(cf.sheen_weight > 0.f);
    const BlenderMaterial sparkle = blender_material(Finish::Sparkle, "#101030", 0.f, LayerLines::Off, false, no_glow, false);
    CHECK(sparkle.sparkle > 0.f);
    CHECK(sparkle.sparkle_cell_m == Approx(SPARKLE_CELL_MM * 0.001f));
    CHECK(sparkle.coat_weight > 0.f);
    CHECK(blender_material(Finish::Marble, "#D0D0D0", 0.f, LayerLines::Off, false, no_glow, false).speckle == "stone");
    CHECK(blender_material(Finish::Wood, "#A07040", 0.f, LayerLines::Off, false, no_glow, false).speckle == "wood");
    CHECK(blender_material(Finish::MatteTextured, "#404040", 0.f, LayerLines::Off, false, no_glow, false).sheen_weight >= 0.5f);
    CHECK(blender_material(Finish::Gloss, "#FF0000", 0.f, LayerLines::Off, false, no_glow, false).coat_weight > 0.f);
    CHECK(blender_material(Finish::Matte, "#FF0000", 0.f, LayerLines::Off, false, no_glow, false).roughness >
          blender_material(Finish::Gloss, "#FF0000", 0.f, LayerLines::Off, false, no_glow, false).roughness);

    // Glow: only with the lights off, in the glow colour.
    BlenderGlow glow;
    glow.on = true;
    glow.srgb = { 0.5f, 1.f, 0.4f };
    glow.intensity = 1.5f;
    CHECK(blender_material(Finish::Standard, "#FFFFFF", 0.f, LayerLines::Off, false, glow, false).emission_strength == 0.f);
    const BlenderMaterial lit = blender_material(Finish::Standard, "#FFFFFF", 0.f, LayerLines::Off, false, glow, true);
    CHECK(lit.emission_strength == Approx(3.f));
    CHECK(lit.emission_color[1] == Approx(1.f));
    CHECK(lit.emission_color[0] == Approx(srgb_to_linear(0.5f)));
    const GltfMaterial lit_gltf = gltf_material(lit, "g", "");
    CHECK(lit_gltf.emissive[1] == Approx(1.f));
    CHECK(lit_gltf.emissive_strength == Approx(3.f));
}

TEST_CASE("Camera: the pose's zoom and frame give Blender's angle of view and lens", "[ShowcaseBlender][Camera]")
{
    CameraPose pose;
    pose.eye    = { 0., -200., 150. };
    pose.target = { 0., 0., 0. };
    pose.up     = { 0., 0., 1. };
    pose.zoom   = 10.;   // 1000 px / 10 = 100 mm of the frame's height at the target, 250 mm away
    const BlenderCamera c = blender_camera(pose, Projection::Perspective, 1000., 16. / 9.);
    CHECK_FALSE(c.ortho);
    CHECK(c.location[1] == Approx(-0.2));
    CHECK(c.location[2] == Approx(0.15));
    CHECK(c.fov_y_deg == Approx(2. * std::atan(50. / 250.) * 180. / SB_PI));
    // The full-frame-equivalent sensor and lens give the same angle back.
    CHECK(2. * std::atan(0.5 * c.sensor_height_mm / c.lens_mm) * 180. / SB_PI == Approx(c.fov_y_deg).epsilon(1e-6));
    const BlenderCamera o = blender_camera(pose, Projection::Ortho, 1000., 16. / 9.);
    CHECK(o.ortho);
    CHECK(o.ortho_height == Approx(0.1));
}

TEST_CASE("Lights: the three-point rig as suns of matching strength", "[ShowcaseBlender][Lights]")
{
    Lighting li;
    li.key_intensity = 1.f;
    li.key_temperature = 6500.f;
    const std::vector<BlenderLight> l = blender_lights(li, { 0., -1., 0. }, false, 0.f);
    REQUIRE(l.size() == 3);
    CHECK(l[0].name == "EdgeSlicer Key");
    CHECK(l[1].name == "EdgeSlicer Fill");
    CHECK(l[2].name == "EdgeSlicer Rim");
    CHECK(l[0].strength == Approx(SB_PI).epsilon(0.02));
    CHECK(l[1].strength == Approx(SB_PI * 0.35).epsilon(0.05));
    CHECK(std::max({ l[0].color[0], l[0].color[1], l[0].color[2] }) == Approx(1.f));
    const std::array<double, 3> d = light_direction(li.azimuth_deg, li.elevation_deg);
    CHECK(l[0].direction[0] == Approx(d[0]));
    CHECK(l[0].direction[2] == Approx(d[2]));
    // The HDRI's sun stands in for the key; the lights off take every light away.
    CHECK(blender_lights(li, { 0., -1., 0. }, true, 0.f).size() == 2);
    for (const BlenderLight &x : blender_lights(li, { 0., -1., 0. }, false, 1.f))
        CHECK(x.strength == 0.f);
    li.fill = false;
    li.rim  = false;
    CHECK(blender_lights(li, { 0., -1., 0. }, false, 0.f).size() == 1);
    li.world_fixed = false;   // the key follows the camera
    CHECK(blender_lights(li, { 0., -1., 0. }, false, 0.f)[0].direction[1] == Approx(-1.));
}

TEST_CASE("Normals: faces meeting at a sharp edge are split, flat ones shared", "[ShowcaseBlender]")
{
    std::vector<float> v;
    std::vector<uint32_t> f;
    sb_box(0.f, 0.f, 0.f, 1.f, 1.f, 1.f, v, f);
    const NormalMesh box = mesh_with_normals(v, f, 30.f);
    CHECK(box.positions.size() / 3 == 24);   // each corner has three faces at 90 degrees
    CHECK(box.indices.size() == f.size());
    for (size_t i = 0; i < box.normals.size(); i += 3) {
        const float l = std::sqrt(box.normals[i] * box.normals[i] + box.normals[i + 1] * box.normals[i + 1] + box.normals[i + 2] * box.normals[i + 2]);
        CHECK(l == Approx(1.f));
    }
    // A finely tessellated sphere stays smooth: one normal per vertex.
    std::vector<float> sv;
    std::vector<uint32_t> sf;
    sb_sphere(0.f, 0.f, 0.f, 1.f, 32, sv, sf);
    const NormalMesh ball = mesh_with_normals(sv, sf, 30.f);
    CHECK(ball.positions.size() <= sv.size());
}

TEST_CASE("Tubes: walls become beads, internals are culled, the budget lowers the detail", "[ShowcaseBlender][Tubes]")
{
    const std::vector<TubeMove> moves = sb_square_moves(3);
    TubeOptions o;
    o.sides = 8;
    const TubeResult r = build_tubes(moves, o);
    REQUIRE(r.meshes.size() == 1);
    CHECK(r.runs == 3);          // one wall loop per layer; the infill lines are culled
    CHECK(r.culled == 3);
    CHECK(r.dropped == 0);
    CHECK(r.sides == 8);
    CHECK(r.triangles == 3 * tube_triangles(5, 8));
    CHECK(r.meshes[0].triangle_count() == r.triangles);
    const TubeMesh &m = r.meshes[0];
    CHECK(m.normals.size() == m.positions.size());
    CHECK(m.direction.size() == m.positions.size());
    CHECK(m.build.size() == m.vertex_count());
    // The bead hangs below the move's Z: between z - h and z.
    float zmin = 1e9f, zmax = -1e9f;
    for (size_t i = 2; i < m.positions.size(); i += 3) {
        zmin = std::min(zmin, m.positions[i]);
        zmax = std::max(zmax, m.positions[i]);
    }
    CHECK(zmin >= 0.f - 1e-4f);
    CHECK(zmax <= 0.6f + 1e-4f);
    // Build fractions run 0..1 in print order.
    CHECK(*std::min_element(m.build.begin(), m.build.end()) >= 0.f);
    CHECK(*std::max_element(m.build.begin(), m.build.end()) <= 1.f);

    TubeOptions with_infill = o;
    with_infill.internals = true;
    const TubeResult ri = build_tubes(moves, with_infill);
    CHECK(ri.runs == 6);
    CHECK(ri.culled == 0);

    // Collinear points are thinned: a straight line of 10 moves is one segment.
    std::vector<TubeMove> line;
    for (int i = 0; i <= 10; ++i) {
        TubeMove t;
        t.kind = i == 0 ? TubeKind::Travel : TubeKind::Extrude;
        t.role = TubeRole::Wall;
        t.x = float(i);
        t.z = 0.2f;
        t.width = 0.4f;
        t.height = 0.2f;
        line.push_back(t);
    }
    CHECK(build_tubes(line, o).points == 2);

    // Automatic detail: a tight budget picks fewer sides; past even that, the last runs are dropped.
    TubeOptions tight;
    tight.max_triangles = 3 * tube_triangles(5, 6);
    CHECK(build_tubes(moves, tight).sides == 6);
    tight.max_triangles = tube_triangles(5, 4) + 1;
    const TubeResult capped = build_tubes(moves, tight);
    CHECK(capped.sides == 4);
    CHECK(capped.runs == 1);
    CHECK(capped.dropped == 2);

    // Deterministic, and one mesh per filament.
    CHECK(build_tubes(moves, o).meshes[0].positions == m.positions);
    std::vector<TubeMove> two = sb_square_moves(1, 0);
    const std::vector<TubeMove> second = sb_square_moves(1, 2);
    two.insert(two.end(), second.begin(), second.end());
    const TubeResult r2 = build_tubes(two, o);
    REQUIRE(r2.meshes.size() == 2);
    CHECK(r2.meshes[0].extruder == 0);
    CHECK(r2.meshes[1].extruder == 2);
}

TEST_CASE("Package: the files, the schema and the bytes", "[ShowcaseBlender][Package]")
{
    const PackageInput in = sb_model_input();
    PackageResult r;
    std::string err;
    REQUIRE(build_blender_package(in, r, &err));
    CHECK(err.empty());
    std::vector<std::string> paths;
    for (const PackageFile &f : r.files)
        paths.push_back(f.path);
    CHECK(paths == std::vector<std::string>{ "README.txt", "scene.glb", "showcase.json" });
    CHECK(r.nodes == 3);

    const PackageFile *glb = sb_file(r, "scene.glb");
    REQUIRE(glb != nullptr);
    GlbStats st;
    REQUIRE(validate_glb(glb->bytes.data(), glb->bytes.size(), &st, &err));
    CHECK(st.nodes == 3);
    CHECK(st.materials == 3);
    GlbChunks chunks;
    REQUIRE(read_glb(glb->bytes.data(), glb->bytes.size(), chunks));
    const nlohmann::json g = nlohmann::json::parse(chunks.json);
    // The 20 mm cube spans 0.02 m; glTF's Y is EdgeSlicer's Z.
    const nlohmann::json &acc = g["accessors"][g["meshes"][0]["primitives"][0]["attributes"]["POSITION"].get<int>()];
    CHECK(acc["max"][0].get<double>() == Approx(0.02));
    CHECK(acc["max"][1].get<double>() == Approx(0.02));
    CHECK(acc["min"][2].get<double>() == Approx(-0.02));
    CHECK(g["meshes"][0]["primitives"][0]["attributes"].contains("NORMAL"));
    CHECK(g["nodes"][0]["extras"]["edgeslicer"]["filament"] == 1);
    CHECK(g["nodes"][0]["extras"]["edgeslicer"]["layers"]["height_m"].get<double>() == Approx(0.0002));
    CHECK(g["materials"][0]["extras"]["edgeslicer"]["finish"] == "silk");
    CHECK(g["scenes"][0]["extras"]["edgeslicer"]["format"] == BLENDER_PACKAGE_FORMAT);

    const nlohmann::json j = sb_json(sb_file(r, "showcase.json"));
    CHECK(j["format"] == BLENDER_PACKAGE_FORMAT);
    CHECK(j["version"] == BLENDER_PACKAGE_VERSION);
    CHECK(j["units"]["length"] == "m");
    CHECK(j["units"]["up"] == "+Z");
    CHECK(j["glb"] == "scene.glb");
    CHECK(j["source"] == "model");
    CHECK(j["output"]["width"] == 640);
    CHECK(j["output"]["height"] == 360);
    CHECK(j["camera"]["type"] == "persp");
    CHECK(j["camera"]["location"][0].get<double>() == Approx(0.09));
    CHECK(j["camera"]["dof"]["enabled"] == true);
    CHECK(j["camera"]["dof"]["f_number"].get<double>() == Approx(4.));
    CHECK(j["camera"]["dof"]["focus_distance"].get<double>() > 0.1);
    CHECK(j["lights"].size() == 3);
    CHECK(j["lights"][0]["type"] == "SUN");
    CHECK(j["environment"]["file"].is_null());   // no HDRI given
    CHECK(j["floor"]["type"] == "shadow_catcher");
    CHECK(j["materials"].size() == 3);
    CHECK(j["materials"]["EdgeSlicer F2 transparent"]["transmission_weight"].get<double>() >= 0.9);
    CHECK(j["objects"].size() == 3);
    CHECK(j["objects"][1]["node"] == "Ball");
    CHECK(j["camera_path"].is_null());
    CHECK(j["look"]["tone"] == "neutral");

    // Deterministic.
    PackageResult again;
    REQUIRE(build_blender_package(in, again));
    REQUIRE(again.files.size() == r.files.size());
    for (size_t i = 0; i < r.files.size(); ++i)
        CHECK(again.files[i].bytes == r.files[i].bytes);

    SECTION("an HDRI, a backplate, an overlay and a camera path")
    {
        PackageInput v = in;
        v.has_hdri = true;
        v.hdri.id = "hdri_studio";
        v.hdri.bytes = { 1, 2, 3, 4 };
        v.hdri.radiance_scale = 0.5f;
        v.hdri.sun_found = false;
        v.scene.background.type = BackgroundType::Backplate;
        v.background_image.ext = "jpg";
        v.background_image.bytes = { 0xFF, 0xD8 };
        v.background_image.width = 1600;
        v.background_image.height = 900;
        v.overlay_png = { 0x89, 'P', 'N', 'G' };
        v.scene.lighting.env_intensity = 0.8f;
        v.scene.lighting.env_rotation_deg = 45.f;
        PackageResult rv;
        REQUIRE(build_blender_package(v, rv));
        CHECK(sb_file(rv, "environment/hdri_studio.hdr") != nullptr);
        CHECK(sb_file(rv, "textures/background.jpg") != nullptr);
        CHECK(sb_file(rv, "overlays/overlay.png") != nullptr);
        const nlohmann::json jv = sb_json(sb_file(rv, "showcase.json"));
        CHECK(jv["environment"]["file"] == "environment/hdri_studio.hdr");
        CHECK(jv["environment"]["strength"].get<double>() == Approx(0.4));
        CHECK(jv["environment"]["rotation_deg"].get<double>() == Approx(45.));
        CHECK(jv["background"]["type"] == "backplate");
        CHECK(jv["background"]["plate"]["sx"].get<double>() > 0.);
        CHECK(jv["floor"]["type"] == "shadow_catcher");   // a backplate draws any floor as a catcher

        // A video: the overlay is for stills only; the path has one entry per frame.
        for (int i = 0; i < 4; ++i) {
            PackageFrame fr;
            fr.pose.eye = { 100. * std::cos(i * 0.5), 100. * std::sin(i * 0.5), 60. };
            fr.pose.zoom = 5.;
            fr.pose.spin_deg = 10. * i;
            fr.build_height_mm = i == 3 ? -1. : 5. * i;
            v.frames.push_back(fr);
        }
        PackageResult vid;
        REQUIRE(build_blender_package(v, vid));
        CHECK(sb_file(vid, "overlays/overlay.png") == nullptr);
        const nlohmann::json p = sb_json(sb_file(vid, "camera_path.json"));
        CHECK(p["format"] == BLENDER_CAMERA_PATH_FORMAT);
        CHECK(p["frames"].size() == 4);
        CHECK(p["frames"][2]["spin_deg"].get<double>() == Approx(20.));
        CHECK(p["frames"][2]["build_height"].get<double>() == Approx(0.01));
        CHECK(p["frames"][3]["build_height"].is_null());
        CHECK(sb_json(sb_file(vid, "showcase.json"))["camera_path"] == "camera_path.json");
    }

    SECTION("toolpath tubes carry the silk direction and the build fraction")
    {
        PackageInput t = in;
        t.toolpaths = true;
        TubeOptions o;
        o.sides = 6;
        t.tubes = build_tubes(sb_square_moves(4), o);
        PackageResult rt;
        REQUIRE(build_blender_package(t, rt, &err));
        const PackageFile *tg = sb_file(rt, "scene.glb");
        REQUIRE(tg != nullptr);
        REQUIRE(validate_glb(tg->bytes.data(), tg->bytes.size(), &st, &err));
        GlbChunks tc;
        REQUIRE(read_glb(tg->bytes.data(), tg->bytes.size(), tc));
        const nlohmann::json tj = nlohmann::json::parse(tc.json);
        CHECK(tj["meshes"][0]["primitives"][0]["attributes"].contains("_SILK_DIR"));
        CHECK(tj["meshes"][0]["primitives"][0]["attributes"].contains("_BUILD"));
        CHECK(tj["nodes"][0]["name"] == "Toolpaths F1");
        CHECK(tj["materials"][0]["extras"]["edgeslicer"]["blender"]["tangent"] == "attribute");
        CHECK(sb_json(sb_file(rt, "showcase.json"))["toolpaths"]["sides"] == 6);
    }

    SECTION("nothing to export is an error")
    {
        PackageInput e = in;
        e.parts.clear();
        PackageResult re;
        CHECK_FALSE(build_blender_package(e, re, &err));
        CHECK_FALSE(err.empty());
    }
}

TEST_CASE("Package: written as a folder and as a deterministic zip", "[ShowcaseBlender][Package]")
{
    namespace fs = boost::filesystem;
    PackageResult r;
    REQUIRE(build_blender_package(sb_model_input(), r));
    const fs::path tmp = fs::temp_directory_path() / fs::unique_path("sb_pkg_%%%%-%%%%");
    std::string err;
    REQUIRE(write_package_folder(r.files, (tmp / "folder").string(), &err));
    CHECK(fs::exists(tmp / "folder" / "scene.glb"));
    CHECK(sb_read_file((tmp / "folder" / "showcase.json").string()) == sb_file(r, "showcase.json")->bytes);

    REQUIRE(write_package_zip(r.files, (tmp / "a.zip").string(), &err));
    REQUIRE(write_package_zip(r.files, (tmp / "b.zip").string(), &err));
    CHECK(sb_read_file((tmp / "a.zip").string()) == sb_read_file((tmp / "b.zip").string()));
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(Slic3r::open_zip_reader(&zip, (tmp / "a.zip").string()));
    CHECK(mz_zip_reader_get_num_files(&zip) == r.files.size());
    CHECK(mz_zip_reader_locate_file(&zip, "scene.glb", nullptr, 0) >= 0);
    Slic3r::close_zip_reader(&zip);

    // The add-on's zip: a folder under a prefix, caches left out.
    fs::create_directories(tmp / "addon" / "__pycache__");
    { boost::nowide::ofstream((tmp / "addon" / "__init__.py").string()) << "x = 1\n"; }
    { boost::nowide::ofstream((tmp / "addon" / "__pycache__" / "x.pyc").string()) << "junk"; }
    REQUIRE(zip_folder((tmp / "addon").string(), "edgeslicer_render", (tmp / "addon.zip").string(), &err));
    mz_zip_zero_struct(&zip);
    REQUIRE(Slic3r::open_zip_reader(&zip, (tmp / "addon.zip").string()));
    CHECK(mz_zip_reader_get_num_files(&zip) == 1);
    CHECK(mz_zip_reader_locate_file(&zip, "edgeslicer_render/__init__.py", nullptr, 0) >= 0);
    Slic3r::close_zip_reader(&zip);
    fs::remove_all(tmp);
}

TEST_CASE("Environment atlas: a bundled environment becomes a panorama", "[ShowcaseBlender]")
{
    const int face = 8, levels = 2;
    const int w = env_atlas_width(face), h = env_atlas_height(face, levels);
    std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4, 0);
    // Face +Z of GL cube space is world -Y (world_to_cube); paint level 0's faces one colour each.
    for (int f = 0; f < 6; ++f) {
        const RectI r = env_atlas_face(face, levels, 0, f);
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x) {
                uint8_t *p = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
                p[0] = uint8_t(40 * (f + 1));
                p[1] = 0;
                p[2] = 0;
                p[3] = 255;
            }
    }
    HdrImage out;
    REQUIRE(atlas_to_equirect(rgba, w, h, face, levels, 16.f, 64, out));
    CHECK(out.width == 64);
    CHECK(out.height == 32);
    // Straight up (world +Z) is GL +Y, face 2.
    const float up = out.rgb[(size_t(0) * 64 + 32) * 3];
    CHECK(up == Approx(decode_rgbm(120, 0, 0, 255, 16.f)[0]));
    CHECK_FALSE(atlas_to_equirect(rgba, w + 1, h, face, levels, 16.f, 64, out));
}

TEST_CASE("Fixture packages for the Blender add-on test", "[ShowcaseBlender][fixture]")
{
    const char *dir = std::getenv("EDGESLICER_BLENDER_FIXTURE_OUT");
    if (dir == nullptr || *dir == 0)
        return;   // only when asked (tests/blender/run_blender_tests.py)
    namespace fs = boost::filesystem;
    PackageInput in = sb_model_input();
    in.width  = 480;
    in.height = 270;
    in.frame_height_px = 270.;
    in.scene.camera.zoom = 270. / 70.;
    if (const char *hdr = std::getenv("EDGESLICER_BLENDER_FIXTURE_HDRI"); hdr != nullptr && *hdr != 0) {
        in.has_hdri = true;
        in.hdri.id = "hdri_studio";
        in.hdri.name = "Studio softbox";
        in.hdri.bytes = sb_read_file(hdr);
        HdrImage img;
        if (decode_rgbe(in.hdri.bytes.data(), in.hdri.bytes.size(), img)) {
            const float peak = sh_peak_luminance(project_hdr_sh(img));
            in.hdri.radiance_scale = peak > 0.f ? 1.f / peak : 1.f;
            in.hdri.sun_found = find_hdr_sun(img).found;
        }
        in.scene.background.type = BackgroundType::Environment;
    }
    PackageResult model;
    std::string err;
    REQUIRE(build_blender_package(in, model, &err));
    REQUIRE(write_package_folder(model.files, (fs::path(dir) / "model").string(), &err));

    PackageInput tp = in;
    tp.toolpaths = true;
    TubeOptions o;
    tp.tubes = build_tubes(sb_square_moves(40), o);
    tp.scene.camera.target = { 10., 10., 4. };
    tp.model_min = { 0., 0., 0. };
    tp.model_max = { 20., 20., 8. };
    for (int i = 0; i < 12; ++i) {
        PackageFrame fr;
        const double a = 2. * SB_PI * i / 12.;
        fr.pose.eye    = { 10. + 90. * std::cos(a), 10. + 90. * std::sin(a), 60. };
        fr.pose.target = { 10., 10., 4. };
        fr.pose.zoom   = 270. / 50.;
        fr.build_height_mm = 8. * double(i + 1) / 12.;
        tp.frames.push_back(fr);
    }
    PackageResult paths;
    REQUIRE(build_blender_package(tp, paths, &err));
    REQUIRE(write_package_folder(paths.files, (fs::path(dir) / "toolpaths").string(), &err));
    REQUIRE(write_package_zip(paths.files, (fs::path(dir) / "toolpaths.zip").string(), &err));
}
