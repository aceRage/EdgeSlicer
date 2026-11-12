// What a flat cut hands back beyond the meshes: which sides are results, where every copy of the
// object ends up, and where the height ranges of a half apply.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>

#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

const ModelObjectCutAttributes HALVES = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower;

// A 20 x 10 x 10 block centred on its object origin, so a turn about Z shows in its footprint.
ModelObject* add_block(Model& model)
{
    ModelObject* mo = model.add_object();
    TriangleMesh block(its_make_cube(20., 10., 10.));
    block.translate(-10.f, -5.f, -5.f);
    mo->add_volume(std::move(block));
    return mo;
}

// The world box of instance `idx` of `mo` (model parts only).
BoundingBoxf3 world_box(const ModelObject* mo, size_t idx) { return mo->instance_bounding_box(idx); }

// The world box of the source's own copy `inst`, clipped to z >= its own centre: what the upper
// half of that copy must cover after a cut through the middle of the block.
BoundingBoxf3 expected_upper_box(const Transform3d& inst)
{
    indexed_triangle_set block = its_make_cube(20., 10., 5.);
    its_transform(block, Geometry::translation_transform(Vec3d(-10., -5., 0.)));
    its_transform(block, inst);
    return bounding_box(block);
}

void require_same_box(const BoundingBoxf3& a, const BoundingBoxf3& b)
{
    for (int i = 0; i < 3; ++i) {
        CHECK_THAT(a.min[i], WithinAbs(b.min[i], 1e-3));
        CHECK_THAT(a.max[i], WithinAbs(b.max[i], 1e-3));
    }
}

bool has_model_part(const ModelObject* mo)
{
    return std::any_of(mo->volumes.begin(), mo->volumes.end(), [](const ModelVolume* v) { return v->is_model_part(); });
}

} // namespace

TEST_CASE("A cut just above the parts gives no object made of a modifier alone", "[Cut][CutHalves]")
{
    Model        model;
    ModelObject* mo = add_block(model);
    // A modifier from inside the block up past its top.
    TriangleMesh mod(its_make_cube(6., 6., 6.));
    mod.translate(-3.f, -3.f, 2.f);
    mo->add_volume(std::move(mod), ModelVolumeType::PARAMETER_MODIFIER);
    mo->add_instance()->set_offset(Vec3d(0., 0., 5.));

    // The cut matrix is taken about the instance's position: the block's top is at 5 there, the
    // modifier's at 8. Cut 0.3 mm above the block.
    Cut cut(mo, 0, Geometry::translation_transform(Vec3d(0., 0., 5.3)), HALVES);
    const ModelObjectPtrs& res = cut.perform_with_plane();

    REQUIRE(res.size() == 1);
    for (const ModelObject* o : res)
        CHECK(has_model_part(o));
    // The modifier stays with the part it overlaps.
    const ModelObject* lower = res.front();
    CHECK(std::count_if(lower->volumes.begin(), lower->volumes.end(), [](const ModelVolume* v) { return v->is_modifier(); }) == 1);
}

TEST_CASE("Every copy of a cut object keeps its own pose", "[Cut][CutHalves]")
{
    Model        model;
    ModelObject* mo = add_block(model);
    const Transform3d inst0 = Geometry::translation_transform(Vec3d(0., 0., 5.)) * Geometry::rotation_transform(Vec3d(0., 0., PI / 6.));
    const Transform3d inst1 = Geometry::translation_transform(Vec3d(50., 0., 5.));
    const Transform3d inst2 = Geometry::translation_transform(Vec3d(-50., 0., 7.5)) * Geometry::rotation_transform(Vec3d(0., 0., PI / 6.)) *
                              Geometry::scale_transform(1.5);
    mo->add_instance()->set_transformation(Geometry::Transformation(inst0));
    mo->add_instance()->set_transformation(Geometry::Transformation(inst1));
    mo->add_instance()->set_transformation(Geometry::Transformation(inst2));

    SECTION("the halves stay where they were")
    {
        // Through the middle of copy 0, which is turned 30 degrees about Z.
        Cut cut(mo, 0, Transform3d::Identity(), ModelObjectCutAttribute::KeepUpper);
        const ModelObjectPtrs& res = cut.perform_with_plane();
        REQUIRE(res.size() == 1);
        const ModelObject* upper = res.front();
        REQUIRE(upper->instances.size() == 3);

        {
            INFO("copy 0, the one cut");
            require_same_box(world_box(upper, 0), expected_upper_box(inst0));
        }
        {
            INFO("copy 1, not turned");
            require_same_box(world_box(upper, 1), expected_upper_box(inst1));
        }
        {
            INFO("copy 2, turned like copy 0 and scaled 1.5");
            require_same_box(world_box(upper, 2), expected_upper_box(inst2));
        }

        // The mesh carries copy 0's turn: copy 1 is turned back by it, copy 2 keeps only its scale.
        const auto linear = [upper](size_t i) { return upper->instances[i]->get_transformation().get_matrix_no_offset().matrix(); };
        CHECK(linear(0).isApprox(Transform3d::Identity().matrix(), 1e-9));
        CHECK(linear(1).isApprox(Geometry::rotation_transform(Vec3d(0., 0., -PI / 6.)).matrix(), 1e-9));
        CHECK(linear(2).isApprox(Geometry::scale_transform(1.5).matrix(), 1e-9));
        CHECK(upper->instances[1]->get_offset().isApprox(Vec3d(50., 0., 5.)));
    }

    SECTION("halves laid on the bed keep the copies' turns relative to each other")
    {
        Cut cut(mo, 0, Transform3d::Identity(), ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::FlipUpper);
        const ModelObjectPtrs& res = cut.perform_with_plane();
        REQUIRE(res.size() == 1);
        const ModelObject* upper = res.front();
        REQUIRE(upper->instances.size() == 3);
        // Before the cut copy 1 was turned -30 degrees from copy 0, copy 2 not at all.
        const Transform3d r0 = upper->instances[0]->get_transformation().get_rotation_matrix();
        const Transform3d r1 = upper->instances[1]->get_transformation().get_rotation_matrix();
        const Transform3d r2 = upper->instances[2]->get_transformation().get_rotation_matrix();
        CHECK((r1 * r0.inverse()).matrix().isApprox(Geometry::rotation_transform(Vec3d(0., 0., -PI / 6.)).matrix(), 1e-6));
        CHECK((r2 * r0.inverse()).matrix().isApprox(Transform3d::Identity().matrix(), 1e-6));
    }
}

TEST_CASE("The upper half's height ranges move down with it", "[Cut][CutHalves]")
{
    Model        model;
    ModelObject* mo = add_block(model);
    mo->add_instance()->set_offset(Vec3d(0., 0., 5.)); // on the bed, 10 mm tall
    auto range_config = [](double layer_height) {
        ModelConfig cfg;
        cfg.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
        return cfg;
    };
    mo->layer_config_ranges[{1., 3.}] = range_config(0.1);  // wholly below the cut
    mo->layer_config_ranges[{4., 7.}] = range_config(0.12); // across it
    mo->layer_config_ranges[{8., 9.}] = range_config(0.16); // wholly above it

    // At world z 6 (the cut matrix is taken about the instance's position).
    Cut cut(mo, 0, Geometry::translation_transform(Vec3d(0., 0., 1.)), HALVES);
    const ModelObjectPtrs& res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);
    const ModelObject* upper = res[0];
    const ModelObject* lower = res[1];
    REQUIRE(upper->instance_bounding_box(0).min.z() > 5.9);

    std::vector<std::pair<t_layer_height_range, double>> got;
    for (const auto& [range, cfg] : upper->layer_config_ranges)
        got.emplace_back(range, cfg.opt_float("layer_height"));
    REQUIRE(got.size() == 2);
    CHECK_THAT(got[0].first.first, WithinAbs(0., 1e-6));
    CHECK_THAT(got[0].first.second, WithinAbs(1., 1e-6));
    CHECK_THAT(got[0].second, WithinAbs(0.12, 1e-9));
    CHECK_THAT(got[1].first.first, WithinAbs(2., 1e-6));
    CHECK_THAT(got[1].first.second, WithinAbs(3., 1e-6));
    CHECK_THAT(got[1].second, WithinAbs(0.16, 1e-9));

    // The lower half keeps its bottom, and so its ranges.
    CHECK(lower->layer_config_ranges.size() == 3);
}
