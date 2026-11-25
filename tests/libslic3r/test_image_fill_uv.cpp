// Mesh UV at slice time (ImageFill.hpp, section 10): a textured model keeps its texture and its
// per-face UVs, and a wall point reads the texture at the nearest point of the part's surface.
// Design: tests/design_dual_wall_dithering.md, section 4.1 (P4).
#include <catch2/catch.hpp>

#include "libslic3r/ImageFill.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/GLTF.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// One UV per face corner: every corner of face f at (f / n, 0.5).
ImageFillUVs uvs_by_face(const indexed_triangle_set &mesh)
{
    ImageFillUVs uvs(mesh.indices.size());
    for (size_t f = 0; f < uvs.size(); ++f)
        for (size_t k = 0; k < 3; ++k)
            uvs[f][k] = Vec2f((float(f) + 0.5f) / float(uvs.size()), 0.5f);
    return uvs;
}

// A picture one pixel high: pixel x is (x * step, 255 - x * step, 7).
std::vector<uint8_t> ramp_png(size_t width)
{
    std::vector<uint8_t> rgb(width * 3);
    for (size_t x = 0; x < width; ++x) {
        rgb[x * 3]     = uint8_t(x * 255 / width);
        rgb[x * 3 + 1] = uint8_t(255 - x * 255 / width);
        rgb[x * 3 + 2] = 7;
    }
    return image_fill_encode_png(width, 1, rgb.data());
}

bool load_medallion(Model &model, GltfInfo &info)
{
    std::string message;
    return load_gltf((std::string(TEST_DATA_DIR) + "/image_fill/agent_medallion.glb").c_str(), &model, info, message);
}

} // namespace

TEST_CASE("Mesh UV: an encoded PNG decodes to the same pixels", "[imagefill][meshuv]")
{
    ImageAssetStore   store;
    const std::string sha = store.add(ramp_png(12));
    REQUIRE_FALSE(sha.empty());
    const ImageAsset *img = store.pixels(sha);
    REQUIRE(img != nullptr);
    REQUIRE(img->width == 12u);
    REQUIRE(img->height == 1u);
    CHECK(int(img->rgb[3 * 5]) == 5 * 255 / 12);
    CHECK(int(img->rgb[3 * 5 + 1]) == 255 - 5 * 255 / 12);
    CHECK(int(img->rgb[3 * 5 + 2]) == 7);
}

TEST_CASE("Mesh UV: the UV blob round-trips and is refused for another mesh", "[imagefill][meshuv]")
{
    const indexed_triangle_set mesh = make_cube(20., 20., 20.).its;
    const ImageFillUVs         uvs  = uvs_by_face(mesh);
    const std::vector<uint8_t> blob = image_fill_encode_uvs(uvs, mesh);
    REQUIRE_FALSE(blob.empty());

    ImageFillUVs back;
    REQUIRE(image_fill_decode_uvs(blob, mesh, back));
    REQUIRE(back.size() == uvs.size());
    for (size_t f = 0; f < uvs.size(); ++f)
        for (size_t k = 0; k < 3; ++k) {
            CHECK(back[f][k].x() == uvs[f][k].x());
            CHECK(back[f][k].y() == uvs[f][k].y());
        }

    // The same triangles in another order: the numbering the UVs are indexed by is gone.
    indexed_triangle_set renumbered = mesh;
    std::swap(renumbered.indices[0], renumbered.indices[5]);
    CHECK_FALSE(image_fill_decode_uvs(blob, renumbered, back));
    // Fewer triangles.
    indexed_triangle_set shorter = mesh;
    shorter.indices.pop_back();
    CHECK_FALSE(image_fill_decode_uvs(blob, shorter, back));
    // Not a blob at all.
    CHECK_FALSE(image_fill_decode_uvs(ramp_png(4), mesh, back));
    // UVs that do not fit the mesh are not encoded.
    ImageFillUVs wrong(uvs.begin(), uvs.begin() + 3);
    CHECK(image_fill_encode_uvs(wrong, mesh).empty());
}

TEST_CASE("Mesh UV: a point on a face reads that face's pixel", "[imagefill][meshuv]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    const indexed_triangle_set &mesh = volume->mesh().its;
    const size_t                n    = mesh.indices.size();

    REQUIRE(image_fill_set_volume_texture(*volume, model.image_assets, ramp_png(n), uvs_by_face(mesh)));
    REQUIRE(image_fill_volume_has_texture(*volume, model.image_assets));

    ImageFillParams params;
    REQUIRE(image_fill_params_of(*volume, params));
    CHECK(params.projection == ImageFillProjection::MeshUV);
    REQUIRE_FALSE(params.uv_asset.empty());
    const ImageAsset *img = model.image_assets.pixels(params.asset);
    REQUIRE(img != nullptr);

    const auto lookup = image_fill_uv_lookup(*volume, model.image_assets);
    REQUIRE(lookup != nullptr);
    // Asked again, the same lookup comes back.
    CHECK(image_fill_uv_lookup(*volume, model.image_assets) == lookup);

    for (size_t f = 0; f < n; ++f) {
        const Vec3f &a = mesh.vertices[mesh.indices[f](0)], &b = mesh.vertices[mesh.indices[f](1)],
                    &c = mesh.vertices[mesh.indices[f](2)];
        const Vec3f normal = (b - a).cross(c - a).normalized();
        // A point just inside the part, under the middle of the face - where a wall's centre line is.
        const Vec3f          p = (a + b + c) / 3.f - normal * 0.2f;
        std::array<float, 3> color{0.f, 0.f, 0.f};
        INFO("face " << f);
        REQUIRE(image_fill_sample_uv(*lookup, params, *img, p, color));
        CHECK(int(std::lround(color[0] * 255.f)) == int(f * 255 / n));
        CHECK(int(std::lround(color[2] * 255.f)) == 7);
    }
}

TEST_CASE("Mesh UV: a textured glTF keeps its texture and UVs on the part", "[imagefill][meshuv][glb]")
{
    Model    model;
    GltfInfo info;
    REQUIRE(load_medallion(model, info));
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    const ModelVolume          &volume = *model.objects.front()->volumes.front();
    const indexed_triangle_set &mesh   = volume.mesh().its;

    REQUIRE(image_fill_volume_has_texture(volume, model.image_assets));
    ImageFillParams params;
    REQUIRE(image_fill_params_of(volume, params));
    CHECK(params.projection == ImageFillProjection::MeshUV);
    const ImageAsset *img = model.image_assets.pixels(params.asset);
    REQUIRE(img != nullptr);
    CHECK(img->width == 64u);
    CHECK(img->height == 64u);

    // The reader's own per-triangle sample (the texture at the centroid UV) is what the lookup
    // returns for the triangle's centroid. A centroid within rounding of a texel edge may land on
    // the neighbouring texel, so nearly all, not all.
    REQUIRE(info.face_colors.size() == mesh.indices.size());
    const auto lookup = image_fill_uv_lookup(volume, model.image_assets);
    REQUIRE(lookup != nullptr);
    size_t same = 0;
    for (size_t f = 0; f < mesh.indices.size(); ++f) {
        const Vec3f p = (mesh.vertices[mesh.indices[f](0)] + mesh.vertices[mesh.indices[f](1)] +
                         mesh.vertices[mesh.indices[f](2)]) / 3.f;
        std::array<float, 3> color{0.f, 0.f, 0.f};
        if (!image_fill_sample_uv(*lookup, params, *img, p, color))
            continue;
        const RGBA &expect = info.face_colors[f];
        if (std::abs(color[0] - expect[0]) < 0.01f && std::abs(color[1] - expect[1]) < 0.01f &&
            std::abs(color[2] - expect[2]) < 0.01f)
            ++same;
    }
    INFO(same << " of " << mesh.indices.size() << " triangles");
    CHECK(same * 10 >= mesh.indices.size() * 9);
}

TEST_CASE("Mesh UV: the texture and the UVs survive a project save and reload", "[imagefill][meshuv][3mf]")
{
    Model    src_model;
    GltfInfo info;
    REQUIRE(load_medallion(src_model, info));
    src_model.objects.front()->add_instance();
    src_model.objects.front()->ensure_on_bed();
    ImageFillParams src_params;
    REQUIRE(image_fill_params_of(*src_model.objects.front()->volumes.front(), src_params));

    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string test_file = (tmp_root / "image_fill_mesh_uv.3mf").string();

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
    const bool loaded = stored && load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &plate_data,
                                               &project_presets, &is_bbl_3mf, &file_version, nullptr,
                                               LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                                                   LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    boost::filesystem::remove(test_file);

    REQUIRE(stored);
    REQUIRE(loaded);
    REQUIRE(dst_model.objects.size() == 1);
    REQUIRE(dst_model.objects[0]->volumes.size() == 1);
    const ModelVolume &dst_volume = *dst_model.objects[0]->volumes[0];

    ImageFillParams back;
    REQUIRE(image_fill_params_of(dst_volume, back));
    CHECK(back.uv_asset == src_params.uv_asset);
    CHECK(back.asset == src_params.asset);
    CHECK(dst_model.image_assets.contains(src_params.asset));
    CHECK(dst_model.image_assets.contains(src_params.uv_asset));
    // The reloaded mesh numbers its triangles the way the saved one did, so the UVs still fit.
    CHECK(image_fill_volume_has_texture(dst_volume, dst_model.image_assets));
}
