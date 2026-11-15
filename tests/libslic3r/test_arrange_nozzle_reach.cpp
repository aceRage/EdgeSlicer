// Arrange option "Avoid nozzle reach zones": on a printer whose nozzles each reach only part of
// the bed (extruder_printable_area: H2D, H2C, X2D), ArrangeJob and FillBedJob arrange on the part
// of the bed every nozzle reaches (arrangement::clip_bed_to_nozzle_reach), so the packing is the
// one of a whole bed, just narrower. The strips as fixed items (arrangement::nozzle_reach_blockers)
// are only the fallback for a shape that cannot be clipped: fixed items switch the packer to growing
// the pile around the bed centre, which on an H2 bed left a column short of the free width with one
// stray object in it (owner hand test of #1061). With the option off nothing changes.
// The GUI wiring (the checkbox, ArrangeJob, FillBedJob) has no unit test.

#include <catch2/catch.hpp>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtruderAreas.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::arrangement;

namespace {

constexpr int    BEDS  = 16;
constexpr double CUBE  = 40.;
constexpr double PITCH = CUBE + 2.; // 1 mm arrange clearance around each cube

Pointfs rect_mm(double x0, double y0, double x1, double y1) { return {Vec2d(x0, y0), Vec2d(x1, y0), Vec2d(x1, y1), Vec2d(x0, y1)}; }
Points  rect_pts(double x0, double y0, double x1, double y1)
{
    return {Point(scaled(x0), scaled(y0)), Point(scaled(x1), scaled(y0)), Point(scaled(x1), scaled(y1)), Point(scaled(x0), scaled(y1))};
}

struct Printer
{
    const char   *name;
    double        width, depth;
    ExtruderAreas areas;
};

// H2D: left nozzle X 0..325, right 25..350, bed 350 x 320.
Printer h2d() { return {"H2D", 350., 320., compute_extruder_areas(rect_mm(0, 0, 350, 320), {rect_mm(0, 0, 325, 320), rect_mm(25, 0, 350, 320)}, {})}; }
// H2C: left nozzle X 0..325, right 25..330, bed 330 x 320.
Printer h2c() { return {"H2C", 330., 320., compute_extruder_areas(rect_mm(0, 0, 330, 320), {rect_mm(0, 0, 325, 320), rect_mm(25, 0, 330, 320)}, {})}; }

Points bed_of(const Printer &p) { return rect_pts(0, 0, p.width, p.depth); }

ArrangePolygons cubes(int count)
{
    ArrangePolygons out;
    for (int i = 0; i < count; ++i) {
        ArrangePolygon ap;
        ap.poly.contour = Polygon(Points{Point(0, 0), Point(scaled(CUBE), 0), Point(scaled(CUBE), scaled(CUBE)), Point(0, scaled(CUBE))});
        ap.inflation    = scaled(1.);
        ap.bed_idx      = 0;
        ap.height       = CUBE;
        ap.extrude_ids  = {1};
        ap.name         = "cube" + std::to_string(i);
        out.emplace_back(std::move(ap));
    }
    return out;
}

ArrangeParams job_like_params()
{
    ArrangeParams params; // final align on, as ArrangeJob arranges
    params.parallel      = false;
    auto passes          = std::make_shared<int>(0);
    params.stopcondition = [passes]() { return ++*passes > 1000000; };
    return params;
}

// The option on, as ArrangeJob does it.
ArrangePolygons arrange_avoiding(const Printer &printer, int count)
{
    ArrangePolygons items = cubes(count);
    Points          bed   = bed_of(printer);
    REQUIRE(clip_bed_to_nozzle_reach(bed, printer.areas));
    arrange(items, ArrangePolygons{}, bed, job_like_params());
    return items;
}

double overlap_mm2(const Polygon &a, const Polygons &b)
{
    double area = 0.;
    for (const Polygon &p : intersection(Polygons{a}, b))
        area += std::abs(p.area());
    return area * SCALING_FACTOR * SCALING_FACTOR;
}

// The packing of bed `bed` is a dense block across X lo..hi: no column of cubes holds a single
// stray one while the others are full, and neither side leaves room for another column.
void check_dense(const ArrangePolygons &items, int bed, double lo, double hi, const std::string &what)
{
    std::map<long, int> columns; // left edge (0.5 mm steps) -> cubes
    double min_x = 1e9, max_x = -1e9;
    for (const ArrangePolygon &ap : items) {
        if (ap.bed_idx != bed)
            continue;
        const BoundingBox bb = ap.transformed_poly().contour.bounding_box();
        min_x                = std::min(min_x, unscaled(bb.min.x()));
        max_x                = std::max(max_x, unscaled(bb.max.x()));
        const long key       = std::lround(unscaled(bb.min.x()) * 2.);
        // Group left edges within 2 mm of each other.
        auto it = std::find_if(columns.begin(), columns.end(), [key](const auto &c) { return std::abs(c.first - key) <= 4; });
        if (it == columns.end())
            columns[key] = 1;
        else
            ++it->second;
    }
    REQUIRE(!columns.empty());
    int biggest = 0;
    for (const auto &c : columns)
        biggest = std::max(biggest, c.second);
    for (const auto &c : columns) {
        INFO(what << ": column at x " << c.first / 2. << " holds " << c.second << " of up to " << biggest);
        if (biggest >= 4)
            CHECK(c.second > 1);
    }
    INFO(what << ": pile x " << min_x << " - " << max_x << " in " << lo << " - " << hi);
    CHECK(min_x - lo < PITCH);
    CHECK(hi - max_x < PITCH);
}

} // namespace

TEST_CASE("Nozzle reach: the bed is clipped to what every nozzle reaches", "[Arrange]")
{
    for (const Printer &printer : {h2d(), h2c()}) {
        Points bed = bed_of(printer);
        REQUIRE(clip_bed_to_nozzle_reach(bed, printer.areas));
        const BoundingBox bb = BoundingBox(bed);
        INFO(printer.name);
        CHECK(bb.min.x() == scaled(25.));
        CHECK(bb.max.x() == scaled(325.));
        CHECK(bb.min.y() == 0);
        CHECK(bb.max.y() == scaled(320.));
        CHECK(std::abs(Polygon(bed).area()) == Approx(double(bb.size().x()) * double(bb.size().y())));
    }
    // One nozzle, or nozzles reaching the same bed: the bed stays as it is.
    Points bed = rect_pts(0, 0, 256, 256);
    CHECK_FALSE(clip_bed_to_nozzle_reach(bed, ExtruderAreas()));
    const ExtruderAreas same = compute_extruder_areas(rect_mm(0, 0, 256, 256), {rect_mm(0, 0, 256, 256), rect_mm(0, 0, 256, 256)}, {});
    CHECK_FALSE(clip_bed_to_nozzle_reach(bed, same));
    CHECK(bed == rect_pts(0, 0, 256, 256));
}

TEST_CASE("Avoid nozzle reach zones keeps objects out of both strips, packed densely, on H2D and H2C beds", "[Arrange]")
{
    for (const Printer &printer : {h2d(), h2c()}) {
        // 7 x 7 cubes of 40 mm (42 mm apart) fit the 300 x 320 mm both nozzles reach: two beds.
        const ArrangePolygons items = arrange_avoiding(printer, 60);

        Polygons strips;
        for (const Polygons &only : printer.areas.only)
            append(strips, only);
        REQUIRE(!strips.empty());

        std::vector<int> per_bed(BEDS, 0);
        for (const ArrangePolygon &ap : items) {
            INFO(printer.name << " " << ap.name << " on bed " << ap.bed_idx);
            REQUIRE(ap.bed_idx >= 0);
            REQUIRE(ap.bed_idx < BEDS);
            ++per_bed[ap.bed_idx];
            const Polygon     placed = ap.transformed_poly().contour;
            const BoundingBox bb     = placed.bounding_box();
            INFO("x " << unscaled(bb.min.x()) << " - " << unscaled(bb.max.x()));
            CHECK(overlap_mm2(placed, strips) < 0.01);
            CHECK(bb.min.x() >= scaled(25.) - SCALED_EPSILON);
            CHECK(bb.max.x() <= scaled(325.) + SCALED_EPSILON);
        }
        INFO(printer.name << " bed 0: " << per_bed[0] << ", bed 1: " << per_bed[1]);
        CHECK(per_bed[0] >= 36);
        CHECK(per_bed[1] > 0);
        check_dense(items, 0, 25., 325., std::string(printer.name) + " full bed");
    }
}

TEST_CASE("Avoid nozzle reach zones centres a small group in the area both nozzles reach", "[Arrange]")
{
    for (const Printer &printer : {h2d(), h2c()}) {
        const ArrangePolygons items = arrange_avoiding(printer, 4);
        BoundingBox           pile;
        for (const ArrangePolygon &ap : items) {
            REQUIRE(ap.bed_idx == 0);
            pile.merge(ap.transformed_poly().contour.bounding_box());
        }
        INFO(printer.name << " pile centre x " << unscaled(pile.center().x()));
        CHECK(std::abs(unscaled(pile.center().x()) - 175.) < 2.);
    }
}

TEST_CASE("Nozzle reach blockers (fallback): one fixed virtual item per strip and bed, none without strips", "[Arrange]")
{
    const ArrangePolygons blockers = nozzle_reach_blockers(h2d().areas, 3);
    REQUIRE(blockers.size() == 6); // left only and right only, on three beds
    for (const ArrangePolygon &ap : blockers) {
        CHECK(ap.is_virt_object);
        CHECK(ap.bed_idx >= 0);
        CHECK(ap.bed_idx < 3);
    }
    CHECK(nozzle_reach_blockers(ExtruderAreas(), BEDS).empty());
}

TEST_CASE("With Avoid nozzle reach zones off the arrangement is today's", "[Arrange]")
{
    // Off, ArrangeJob neither clips the bed nor adds blockers: the packer gets what it got before.
    for (const Printer &printer : {h2d(), h2c()}) {
        ArrangePolygons today = cubes(20);
        ArrangePolygons off   = today;
        arrange(today, {}, bed_of(printer), job_like_params());
        Points bed = bed_of(printer);
        CHECK_FALSE(clip_bed_to_nozzle_reach(bed, ExtruderAreas())); // the areas ArrangeJob reads when off
        arrange(off, ArrangePolygons{}, bed, job_like_params());
        for (size_t i = 0; i < today.size(); ++i) {
            INFO(printer.name << " item " << i);
            CHECK(off[i].bed_idx == today[i].bed_idx);
            CHECK(off[i].translation == today[i].translation);
        }
    }
}

// Diagnostics, allowed to fail: the same density check with the strips as fixed items (what #1061
// did first) and with a bed_exclude_area strip along the left edge (main, any printer with such a
// zone). They report whether fixed edge strips leave the stray-column packing; see the PR.
TEST_CASE("Diagnostic: density with the strips as fixed items", "[Arrange][!mayfail]")
{
    const Printer   printer = h2d();
    ArrangePolygons items   = cubes(60);
    ArrangePolygons fixed   = nozzle_reach_blockers(printer.areas, BEDS, scaled(1.));
    arrange(items, fixed, bed_of(printer), job_like_params());
    check_dense(items, 0, 26., 324., "H2D strips as fixed items");
}

TEST_CASE("Diagnostic: density beside a bed_exclude_area strip along the left edge", "[Arrange][!mayfail]")
{
    ArrangePolygons items = cubes(50);
    ArrangePolygons fixed;
    for (int bed = 0; bed < BEDS; ++bed) {
        ArrangePolygon ap;
        ap.poly.contour   = Polygon(rect_pts(0, 0, 25, 256));
        ap.is_virt_object = true;
        ap.bed_idx        = bed;
        ap.height         = 1;
        ap.inflation      = scaled(1.);
        fixed.emplace_back(std::move(ap));
    }
    arrange(items, fixed, rect_pts(0, 0, 256, 256), job_like_params());
    check_dense(items, 0, 26., 256., "256 bed, 25 mm exclusion strip");
}
