// Arrange scoring and rotation (arrangement::arrange): the smallest-box turn of an object already
// turned, fixed items in the "how much is left" ratio, the order items are placed in, and the
// final centring of a pile beside a fixed object.

#include <catch2/catch.hpp>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

using namespace Slic3r;
using namespace Slic3r::arrangement;

namespace {

const BoundingBox BED{Point(0, 0), Point(scaled(250.), scaled(250.))};

ArrangePolygon rect(double w_mm, double h_mm, double rotation = 0.)
{
    ArrangePolygon ap;
    Polygon        outline;
    outline.points = {Point(0, 0), Point(scaled(w_mm), 0), Point(scaled(w_mm), scaled(h_mm)), Point(0, scaled(h_mm))};
    ap.poly        = ExPolygon(outline);
    ap.rotation    = rotation;
    ap.inflation   = scaled(3.);
    ap.bed_idx     = 0;
    ap.height      = 10.;
    ap.extrude_ids = {1};
    return ap;
}

ArrangeParams bounded_params()
{
    ArrangeParams params;
    params.parallel      = false;
    auto passes          = std::make_shared<int>(0);
    params.stopcondition = [passes]() { return ++*passes > 1000000; };
    return params;
}

} // namespace

TEST_CASE("Auto-rotate gives an object already turned its smallest box", "[Arrange]")
{
    // The outline comes unturned with the object's turn in rotation, as ModelInstance hands it over.
    // The smallest-box turn was measured on the outline as already turned and then set as the whole turn.
    for (double turn : {0.5, 1.1, 2.0}) {
        ArrangePolygons items = {rect(100., 20., turn)};
        ArrangeParams   params = bounded_params();
        params.allow_rotations = true;
        arrange(items, {}, BED, params);

        REQUIRE(items[0].bed_idx == 0);
        const BoundingBox bb = items[0].transformed_poly().contour.bounding_box();
        INFO("turn " << turn << " -> " << items[0].rotation << ", box " << unscaled(bb.size().x()) << " x " << unscaled(bb.size().y()));
        // 100 x 20 when square to the axes; any other turn of this bar has a box of at least 2900 mm2.
        CHECK(unscaled(bb.size().x()) * unscaled(bb.size().y()) < 2000. + 1.);
    }
}

TEST_CASE("A fixed item on a bed the pile never reaches does not change the arrangement", "[Arrange]")
{
    // Fixed items were counted (twice) among the items "left to place", so their mere presence
    // changed the scoring of every bed.
    ArrangePolygons plain;
    for (int i = 0; i < 3; ++i)
        plain.push_back(rect(170., 12.));
    for (int i = 0; i < 3; ++i) {
        ArrangePolygon disc = rect(1., 1.);
        disc.poly.contour.points.clear();
        for (int k = 0; k < 48; ++k)
            disc.poly.contour.points.emplace_back(scaled(22. * std::cos(2. * M_PI * k / 48)), scaled(22. * std::sin(2. * M_PI * k / 48)));
        plain.push_back(disc);
    }
    for (int i = 0; i < 3; ++i) {
        ArrangePolygon ell = rect(1., 1.);
        ell.poly.contour.points = {Point(0, 0), Point(scaled(60.), 0), Point(scaled(60.), scaled(15.)), Point(scaled(15.), scaled(60.)), Point(0, scaled(60.))};
        plain.push_back(ell);
    }
    for (int i = 0; i < 3; ++i)
        plain.push_back(rect(15., 15.));
    // Parts alike in area, where the scoring weighs how much is left to place.
    for (int i = 0; i < 12; ++i)
        plain.push_back(rect(30., 30.));

    ArrangePolygons with_fixed = plain;
    ArrangePolygon  far_tower  = rect(30., 30.);
    far_tower.translation      = {scaled(200.), scaled(200.)};
    far_tower.bed_idx          = 5;
    far_tower.is_virt_object   = true;
    far_tower.is_wipe_tower    = true;

    arrange(plain, {}, BED, bounded_params());
    arrange(with_fixed, {far_tower}, BED, bounded_params());

    for (size_t i = 0; i < plain.size(); ++i) {
        INFO("item " << i);
        CHECK(with_fixed[i].bed_idx == plain[i].bed_idx);
        CHECK(with_fixed[i].translation == plain[i].translation);
    }
}

TEST_CASE("Items are placed in a strict order: first filament, then area, then input order", "[Arrange]")
{
    // The order compared whole filament lists for "different" but only their first filament for
    // "before": {1} and {1, 2} compared neither way, which is not a strict weak ordering.
    ArrangePolygons items;
    for (int i = 0; i < 12; ++i) {
        items.push_back(rect(25., 25.));
        items.back().extrude_ids = i % 3 == 0 ? std::vector<int>{1} : i % 3 == 1 ? std::vector<int>{1, 2} : std::vector<int>{2};
    }
    for (int i = 0; i < 4; ++i) {
        items.push_back(rect(30., 30.));
        items.back().extrude_ids = {1, 3};
    }
    // Identical parts, more than a small sort keeps in order.
    for (int i = 0; i < 40; ++i) {
        items.push_back(rect(12., 12.));
        items.back().extrude_ids = {3};
    }

    arrange(items, {}, BED, bounded_params());

    std::vector<size_t> by_itemid(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        REQUIRE(items[i].bed_idx >= 0);
        by_itemid[i] = i;
    }
    std::sort(by_itemid.begin(), by_itemid.end(), [&items](size_t a, size_t b) { return items[a].itemid < items[b].itemid; });

    auto key = [&items](size_t i) { return std::make_tuple(items[i].extrude_ids.front(), -items[i].poly.area(), i); };
    for (size_t k = 1; k < by_itemid.size(); ++k) {
        INFO("placed " << by_itemid[k - 1] << " then " << by_itemid[k]);
        CHECK(key(by_itemid[k - 1]) < key(by_itemid[k]));
    }
}

TEST_CASE("The arranged pile is centred on the bed, not the pile together with a fixed object", "[Arrange]")
{
    // A fixed object along the left edge (not arranged, it stays where it is).
    ArrangePolygon fixed = rect(40., 200.);
    fixed.translation    = {scaled(10.), scaled(25.)};

    ArrangePolygons items;
    for (int i = 0; i < 6; ++i)
        items.push_back(rect(30., 30.));

    arrange(items, {fixed}, BED, bounded_params());

    BoundingBox pile;
    for (const ArrangePolygon &ap : items) {
        REQUIRE(ap.bed_idx == 0);
        pile.merge(ap.transformed_poly().contour.bounding_box());
    }
    const BoundingBox fixed_bb = fixed.transformed_poly().contour.bounding_box();
    CHECK_FALSE(pile.overlap(fixed_bb));
    INFO("pile centre " << unscaled(pile.center().x()) << ", " << unscaled(pile.center().y()));
    CHECK(std::abs(unscaled(pile.center().x()) - 125.) < 1.);
    CHECK(std::abs(unscaled(pile.center().y()) - 125.) < 1.);
}
