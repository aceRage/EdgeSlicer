// ImageMap Phase 5c: where the picture sits, and that it stays there.
//
// Spec: docs/superpowers/specs/2026-10-16-imagemap-phase5-completion.md, criteria I and J.
#include <catch2/catch.hpp>

#include <boost/nowide/fstream.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/ImageFill.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

std::vector<uint8_t> read_fixture(const std::string &name)
{
    const std::string path = std::string(TEST_DATA_DIR) + "/image_fill/" + name;
    boost::nowide::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("Image Fill 5c: placement moves, turns and scales the picture about its centre", "[imagefill][p5c]")
{
    SECTION("the default placement is the identity, bit for bit")
    {
        const ImageFillParams p;
        for (float u : {0.f, 0.123f, 0.5f, 0.999f, 1.f})
            for (float v : {0.f, 0.377f, 1.f}) {
                float pu = u, pv = v;
                REQUIRE(image_fill_place_uv(p, pu, pv));
                CHECK(pu == u);
                CHECK(pv == v);
            }
    }
    SECTION("offset and scale")
    {
        ImageFillParams p;
        p.offset_u = 0.25f;
        p.scale_u  = 0.5f;
        p.scale_v  = 0.5f;
        float u = 0.75f, v = 0.5f;   // the picture's centre has moved here
        REQUIRE(image_fill_place_uv(p, u, v));
        CHECK(u == Approx(0.5f));
        CHECK(v == Approx(0.5f));
        u = 0.6f, v = 0.5f;          // the picture's left edge: 0.75 - 0.5 * 0.5 / 2 ... = 0.5
        REQUIRE(image_fill_place_uv(p, u, v));
        CHECK(u == Approx(0.2f));
        u = 0.1f, v = 0.5f;          // left of the picture: nothing there
        CHECK_FALSE(image_fill_place_uv(p, u, v));
    }
    SECTION("rotation turns about the centre")
    {
        ImageFillParams p;
        p.rotation_deg = 90.f;
        float u = 0.75f, v = 0.5f;
        REQUIRE(image_fill_place_uv(p, u, v));
        CHECK(u == Approx(0.5f).margin(1e-5));
        CHECK(v == Approx(0.25f).margin(1e-5));
    }
    SECTION("tiling repeats instead of leaving a gap")
    {
        ImageFillParams p;
        p.scale_u = 0.5f;
        p.scale_v = 0.5f;
        p.tile    = true;
        float u = 0.1f, v = 0.1f;   // (0.1 - 0.5) / 0.5 + 0.5 = -0.3 -> 0.7
        REQUIRE(image_fill_place_uv(p, u, v));
        CHECK(u == Approx(0.7f));
        CHECK(v == Approx(0.7f));
    }
    SECTION("the keys round-trip, and the box and frame keep their precision")
    {
        ImageFillParams p;
        p.asset        = "abc";
        p.offset_u     = -0.125f;
        p.offset_v     = 0.25f;
        p.scale_u      = 0.5f;
        p.scale_v      = 0.75f;
        p.rotation_deg = 30.f;
        p.tile         = true;
        p.keep_aspect  = true;
        p.has_box      = true;
        p.box          = {-10.123456789, -5., 0., 10.5, 5.25, 20.000000001};
        p.has_frame    = true;
        p.frame        = Geometry::translation_transform(Vec3d(1.5, -2.25, 3.)) *
                  Geometry::rotation_transform(Vec3d(0., 0., 0.3));
        ImageFillParams back;
        REQUIRE(ImageFillParams::from_string(p.to_string(), back));
        CHECK(back.offset_u == Approx(p.offset_u));
        CHECK(back.offset_v == Approx(p.offset_v));
        CHECK(back.scale_u == Approx(p.scale_u));
        CHECK(back.scale_v == Approx(p.scale_v));
        CHECK(back.rotation_deg == Approx(p.rotation_deg));
        CHECK(back.tile);
        CHECK(back.keep_aspect);
        REQUIRE(back.has_box);
        for (size_t i = 0; i < 6; ++i)
            CHECK(back.box[i] == Approx(p.box[i]).epsilon(1e-11));
        REQUIRE(back.has_frame);
        CHECK((back.frame.matrix() - p.frame.matrix()).cwiseAbs().maxCoeff() < 1e-9);
        // And a params string with none of it reads as before phase 5c.
        ImageFillParams plain;
        plain.asset = "abc";
        const std::string s = plain.to_string();
        for (const char *key : {"pu=", "pv=", "su=", "sv=", "pr=", "pt=", "pk=", "bx=", "fr="})
            CHECK(s.find(key) == std::string::npos);
    }
}

TEST_CASE("Image Fill 5c: a picture half the size of the part paints only the middle", "[imagefill][p5c]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(make_cube(40., 40., 2.));
    object->add_instance();

    ImageFillParams p;
    p.asset       = model.image_assets.add(read_fixture("quad_rgbw.png"));
    p.projection  = ImageFillProjection::Planar;
    p.axis        = ImageFillAxis::Z;
    p.subdivision = 4;
    p.scale_u     = 0.5f;
    p.scale_v     = 0.5f;
    p.allowed     = {1, 2, 3, 4};
    const std::vector<std::array<float, 3>> colors = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 1.f, 1.f}};

    const ImageFillResult whole = [&] {
        ImageFillParams full = p;
        full.scale_u = full.scale_v = 1.f;
        return image_fill_compute(volume->mesh().its, {}, full, model.image_assets, colors, {1, 2, 3, 4});
    }();
    const ImageFillResult half = image_fill_compute(volume->mesh().its, {}, p, model.image_assets, colors, {1, 2, 3, 4});
    REQUIRE(whole.ok);
    REQUIRE(half.ok);
    INFO("painted leaves: whole " << whole.facets_painted << ", half " << half.facets_painted);
    // A quarter of the top's area (half by half), give or take the leaves the edge cuts through.
    CHECK(half.facets_painted < whole.facets_painted / 2);
    CHECK(half.facets_painted > whole.facets_painted / 8);
    // All four quadrants still come through: the picture is smaller, not cropped.
    CHECK(half.filaments_used.size() == 4);

    // image_fill_apply records the box the picture was laid over.
    REQUIRE(image_fill_apply(*volume, p, model.image_assets, colors, {1, 2, 3, 4}).ok);
    ImageFillParams stored;
    REQUIRE(image_fill_params_of(*volume, stored));
    CHECK(stored.has_box);
    CHECK_FALSE(stored.has_frame);
}

TEST_CASE("Image Fill 5c: after a cut each half's frame maps it back onto the picture", "[imagefill][p5c][cut]")
{
    for (const double turn_deg : {0., 90.}) {
        Model        model;
        ModelObject *object = model.add_object();
        ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
        object->add_instance();
        object->instances.front()->set_rotation(Vec3d(0., 0., turn_deg * PI / 180.));
        object->ensure_on_bed();

        ImageFillParams p;
        p.asset      = model.image_assets.add(read_fixture("quad_rgbw.png"));
        p.projection = ImageFillProjection::Box;
        p.allowed    = {1, 2};
        REQUIRE(image_fill_apply(*volume, p, model.image_assets, {{0.f, 0.f, 0.f}, {1.f, 1.f, 1.f}}, {1, 2}).ok);
        ImageFillParams before;
        REQUIRE(image_fill_params_of(*volume, before));
        REQUIRE(before.has_box);
        // The original mesh is centred: -10..10 on every axis.
        CHECK(before.box[0] == Approx(-10.));
        CHECK(before.box[5] == Approx(10.));

        // The cube stands on the bed, 0..20 in world Z; cut it at world Z = 15.
        const Transform3d cut_matrix = Geometry::translation_transform(Vec3d(0., 0., 15.));
        Cut cut(object, 0, cut_matrix, ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
        const ModelObjectPtrs &res = cut.perform_with_plane();
        REQUIRE(res.size() == 2);

        for (const ModelObject *half : res) {
            REQUIRE(half->volumes.size() == 1);
            const ModelVolume *hv = half->volumes.front();
            ImageFillParams     after;
            REQUIRE(image_fill_params_of(*hv, after));
            INFO("turn " << turn_deg << " half with " << hv->mesh().its.vertices.size() << " vertices");
            REQUIRE(after.has_box);
            REQUIRE(after.has_frame);
            // The box is still the whole part's.
            for (size_t i = 0; i < 6; ++i)
                CHECK(after.box[i] == Approx(before.box[i]));
            // Every vertex of the half, carried by its frame, lands inside the whole part's box -
            // on its surface - and the half's extremes along Z map to the cut height and an end.
            BoundingBoxf3 mapped;
            for (const Vec3f &vtx : hv->mesh().its.vertices) {
                const Vec3d q = after.frame * vtx.cast<double>();
                mapped.merge(q);
                CHECK(q.x() >= -10. - 1e-3);
                CHECK(q.x() <= 10. + 1e-3);
                CHECK(q.y() >= -10. - 1e-3);
                CHECK(q.y() <= 10. + 1e-3);
            }
            // World Z 15 is mesh Z 5 in the original (it stood -10..10, on the bed at 0..20).
            const bool upper = mapped.max.z() > 9.;
            if (upper) {
                CHECK(mapped.min.z() == Approx(5.).margin(1e-3));
                CHECK(mapped.max.z() == Approx(10.).margin(1e-3));
            } else {
                CHECK(mapped.min.z() == Approx(-10.).margin(1e-3));
                CHECK(mapped.max.z() == Approx(5.).margin(1e-3));
            }
            // And the full XY extent survives, turned instance or not.
            CHECK(mapped.min.x() == Approx(-10.).margin(1e-3));
            CHECK(mapped.max.x() == Approx(10.).margin(1e-3));
            CHECK(mapped.min.y() == Approx(-10.).margin(1e-3));
            CHECK(mapped.max.y() == Approx(10.).margin(1e-3));
        }
    }
}
