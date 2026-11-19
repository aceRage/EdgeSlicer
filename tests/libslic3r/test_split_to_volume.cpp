// Split to Fit, phases 1 to 3 and 5: the planner, the apply, the automatic dowels, plane adjustment,
// loose parts and matching cut-face labels (tests/design_split_to_build_volume.md).
#include <catch2/catch.hpp>

#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/CutUtils.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Semver.hpp>
#include <libslic3r/SplitToVolume.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>
#include <libslic3r/Utils.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

SplitPrinterVolume square_printer(double side, double height)
{
    SplitPrinterVolume p;
    p.printable_area   = { Vec2d(0., 0.), Vec2d(side, 0.), Vec2d(side, side), Vec2d(0., side) };
    p.printable_height = height;
    return p;
}

// A usable volume of exactly `x` x `y` x `z` (no margins).
SplitUsableVolume usable_box(double x, double y, double z)
{
    SplitUsableVolume u;
    u.valid        = true;
    u.size         = Vec3d(x, y, z);
    u.bed_rect     = BoundingBoxf(Vec2d(0., 0.), Vec2d(x, y));
    u.printer_size = u.size;
    return u;
}

indexed_triangle_set box_at(double x, double y, double z, const Vec3d &origin = Vec3d::Zero())
{
    indexed_triangle_set its = its_make_cube(x, y, z);
    for (Vec3f &v : its.vertices)
        v += origin.cast<float>();
    return its;
}

// A "U" lying in the XZ plane, extruded 50 mm along Y: two 50 mm pillars 300 mm tall, 200 mm apart,
// on a 300 x 50 base. One closed, manifold mesh (built by hand: no boolean).
indexed_triangle_set u_shape()
{
    // CCW in (x, z). The two points on the outer side walls at z = 50 are where the base meets the
    // pillars in the triangulation below.
    const std::vector<std::array<double, 2>> poly = { { 0., 0. },     { 300., 0. },   { 300., 50. }, { 300., 300. }, { 250., 300. },
                                      { 250., 50. },  { 50., 50. },   { 50., 300. }, { 0., 300. },   { 0., 50. } };
    const std::vector<std::array<int, 3>> tris = {
        { 0, 1, 2 }, { 0, 2, 5 }, { 0, 5, 6 }, { 0, 6, 9 },   // base
        { 9, 6, 7 }, { 9, 7, 8 },                             // left pillar
        { 5, 2, 3 }, { 5, 3, 4 },                             // right pillar
    };
    const float          depth = 50.f;
    indexed_triangle_set its;
    const int            n = int(poly.size());
    for (const auto &p : poly)
        its.vertices.emplace_back(float(p[0]), 0.f, float(p[1]));
    for (const auto &p : poly)
        its.vertices.emplace_back(float(p[0]), depth, float(p[1]));
    for (const auto &t : tris) {
        its.indices.emplace_back(t[0], t[1], t[2]);              // front, y = 0, normal -Y
        its.indices.emplace_back(t[0] + n, t[2] + n, t[1] + n);  // back, y = depth, normal +Y
    }
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        its.indices.emplace_back(i, j + n, j);
        its.indices.emplace_back(i, i + n, j + n);
    }
    return its;
}

ModelObject *add_object(Model &model, const indexed_triangle_set &its, const std::string &name)
{
    ModelObject *mo = model.add_object();
    mo->name        = name;
    mo->add_volume(TriangleMesh(its));
    mo->add_instance();
    mo->invalidate_bounding_box();
    return mo;
}

double world_volume(const ModelObject &o)
{
    double v = 0.;
    for (const ModelVolume *mv : o.volumes) {
        if (!mv->is_model_part())
            continue;
        TriangleMesh m = mv->mesh();
        m.transform(o.instances.front()->get_transformation().get_matrix() * mv->get_matrix(), true);
        v += its_volume(m.its);
    }
    return v;
}

size_t open_edges(const ModelObject &o)
{
    size_t n = 0;
    for (const ModelVolume *mv : o.volumes)
        if (mv->is_model_part())
            n += its_num_open_edges(mv->mesh().its);
    return n;
}

bool has_warning(const std::vector<SplitWarning> &w, SplitWarning::Kind kind)
{
    return std::any_of(w.begin(), w.end(), [kind](const SplitWarning &x) { return x.kind == kind; });
}

} // namespace

// ---- usable volume ----------------------------------------------------------------------------------

TEST_CASE("Split to Fit: usable volume of a 256 mm printer with the default margins", "[SplitToVolume]")
{
    const SplitUsableVolume u = split_usable_volume(square_printer(256., 256.), SplitOptions{});
    REQUIRE(u.valid);
    REQUIRE_THAT(u.size.x(), WithinAbs(246., 1e-9));
    REQUIRE_THAT(u.size.y(), WithinAbs(246., 1e-9));
    REQUIRE_THAT(u.size.z(), WithinAbs(251., 1e-9));
    REQUIRE_FALSE(u.bed_not_rectangular);
    REQUIRE_FALSE(u.reduced_by_exclusion);
    REQUIRE_FALSE(u.has_reach_option);
}

TEST_CASE("Split to Fit: margins change the usable volume", "[SplitToVolume]")
{
    SplitOptions opt;
    opt.margin_xy = 0.;
    opt.margin_z  = 0.;
    SplitUsableVolume u = split_usable_volume(square_printer(256., 256.), opt);
    REQUIRE_THAT(u.size.x(), WithinAbs(256., 1e-9));
    REQUIRE_THAT(u.size.z(), WithinAbs(256., 1e-9));
    opt.margin_xy = 10.;
    opt.margin_z  = 20.;
    u             = split_usable_volume(square_printer(256., 256.), opt);
    REQUIRE_THAT(u.size.y(), WithinAbs(236., 1e-9));
    REQUIRE_THAT(u.size.z(), WithinAbs(236., 1e-9));
    opt.margin_xy = 200.;
    REQUIRE_FALSE(split_usable_volume(square_printer(256., 256.), opt).valid);
}

TEST_CASE("Split to Fit: an exclusion corner takes off the side that loses least", "[SplitToVolume]")
{
    SplitPrinterVolume p = square_printer(256., 256.);
    // 18 x 28 in the front-left corner: losing 18 mm of X keeps more than losing 28 mm of Y.
    p.exclude_boxes.emplace_back(Vec2d(0., 0.), Vec2d(18., 28.));
    const SplitUsableVolume u = split_usable_volume(p, SplitOptions{});
    REQUIRE(u.valid);
    REQUIRE(u.reduced_by_exclusion);
    REQUIRE_THAT(u.bed_rect.min.x(), WithinAbs(18., 1e-9));
    REQUIRE_THAT(u.size.x(), WithinAbs(256. - 18. - 10., 1e-9));
    REQUIRE_THAT(u.size.y(), WithinAbs(246., 1e-9));
    // An exclusion box outside the bed changes nothing.
    SplitPrinterVolume q = square_printer(256., 256.);
    q.exclude_boxes.emplace_back(Vec2d(300., 300.), Vec2d(320., 320.));
    REQUIRE_FALSE(split_usable_volume(q, SplitOptions{}).reduced_by_exclusion);
}

TEST_CASE("Split to Fit: a round bed uses its inscribed square", "[SplitToVolume]")
{
    SplitPrinterVolume p;
    const int          n = 64;
    for (int i = 0; i < n; ++i) {
        const double a = 2. * PI * double(i) / double(n);
        p.printable_area.emplace_back(100. * std::cos(a), 100. * std::sin(a));
    }
    p.printable_height        = 300.;
    const SplitUsableVolume u = split_usable_volume(p, SplitOptions{});
    REQUIRE(u.valid);
    REQUIRE(u.bed_round);
    REQUIRE(u.bed_not_rectangular);
    // The square inscribed in the polygon (its edges sit at 100 * cos(pi / 64) from the centre).
    const double side = 2. * 100. * std::cos(PI / double(n)) / std::sqrt(2.);
    REQUIRE_THAT(u.size.x(), WithinAbs(side - 10., 1e-6));
    REQUIRE_THAT(u.size.y(), WithinAbs(side - 10., 1e-6));
}

TEST_CASE("Split to Fit: H2D-style nozzle reach and height limits", "[SplitToVolume]")
{
    SplitPrinterVolume p;
    p.printable_area   = { Vec2d(0., 0.), Vec2d(350., 0.), Vec2d(350., 320.), Vec2d(0., 320.) };
    p.printable_height = 325.;
    p.extruder_areas   = { { Vec2d(0., 0.), Vec2d(325., 0.), Vec2d(325., 320.), Vec2d(0., 320.) },
                           { Vec2d(25., 0.), Vec2d(350., 0.), Vec2d(350., 320.), Vec2d(25., 320.) } };
    p.extruder_heights = { 320., 325. };

    SplitOptions            opt;
    const SplitUsableVolume whole = split_usable_volume(p, opt);
    REQUIRE(whole.has_reach_option);
    REQUIRE_FALSE(whole.reduced_by_reach);
    REQUIRE_THAT(whole.size.x(), WithinAbs(340., 1e-9));
    // The lowest nozzle limit wins.
    REQUIRE_THAT(whole.size.z(), WithinAbs(315., 1e-9));

    opt.nozzle_reach              = true;
    const SplitUsableVolume reach = split_usable_volume(p, opt);
    REQUIRE(reach.reduced_by_reach);
    REQUIRE_THAT(reach.bed_rect.min.x(), WithinAbs(25., 1e-6));
    REQUIRE_THAT(reach.bed_rect.max.x(), WithinAbs(325., 1e-6));
    REQUIRE_THAT(reach.size.x(), WithinAbs(290., 1e-6));
}

// ---- grid -------------------------------------------------------------------------------------------

TEST_CASE("Split to Fit: a 600 x 200 x 300 box on a 256 mm printer, even parts", "[SplitToVolume]")
{
    const SplitUsableVolume u    = split_usable_volume(square_printer(256., 256.), SplitOptions{});
    const SplitPlan         plan = split_plan_grid(box_at(600., 200., 300.), u, SplitOptions{});
    REQUIRE(plan.valid);
    REQUIRE(plan.counts[0] == 3);
    REQUIRE(plan.counts[1] == 1);
    REQUIRE(plan.counts[2] == 2);
    REQUIRE(plan.planes.size() == 3);
    REQUIRE(plan.part_count() == 6);
    REQUIRE(plan.parts_not_fitting() == 0);
    REQUIRE_FALSE(has_warning(plan.warnings, SplitWarning::Kind::PartDoesNotFit));

    const std::vector<double> xs = plan.plane_positions(0);
    REQUIRE(xs.size() == 2);
    REQUIRE_THAT(xs[0], WithinAbs(200., 1e-4));
    REQUIRE_THAT(xs[1], WithinAbs(400., 1e-4));
    const std::vector<double> zs = plan.plane_positions(2);
    REQUIRE(zs.size() == 1);
    REQUIRE_THAT(zs[0], WithinAbs(150., 1e-4));

    for (const SplitPart &part : plan.parts) {
        REQUIRE_FALSE(part.empty);
        REQUIRE(part.fits);
        REQUIRE_THAT(part.size().x(), WithinAbs(200., 1e-3));
        REQUIRE_THAT(part.size().y(), WithinAbs(200., 1e-3));
        REQUIRE_THAT(part.size().z(), WithinAbs(150., 1e-3));
    }
    // D7 names, Y left out; planes named by axis and index.
    REQUIRE(plan.parts[size_t(plan.cell_index({ { 1, 0, 1 } }))].name == "X2-Z2");
    REQUIRE(plan.planes[0].name() == "X1");
    REQUIRE(plan.planes[2].name() == "Z1");
    // Every cut face is between two parts: 2 X planes x 2 Z layers + 1 Z plane x 3 X columns.
    REQUIRE(plan.faces.size() == 7);
}

TEST_CASE("Split to Fit: full size first", "[SplitToVolume]")
{
    const SplitUsableVolume u = split_usable_volume(square_printer(256., 256.), SplitOptions{});
    SplitOptions            opt;
    opt.layout = SplitLayout::FullSizeFirst;

    SECTION("planes at the usable length")
    {
        const SplitPlan           plan = split_plan_grid(box_at(600., 200., 300.), u, opt);
        const std::vector<double> xs   = plan.plane_positions(0);
        REQUIRE(xs.size() == 2);
        REQUIRE_THAT(xs[0], WithinAbs(246., 1e-4));
        REQUIRE_THAT(xs[1], WithinAbs(492., 1e-4));
        REQUIRE_THAT(plan.plane_positions(2)[0], WithinAbs(251., 1e-4));
        REQUIRE(plan.parts_not_fitting() == 0);
    }
    SECTION("a remainder thinner than the minimum pulls the last plane in")
    {
        // 500 mm: planes at 246 and 492 would leave an 8 mm last part.
        const SplitPlan           plan = split_plan_grid(box_at(500., 100., 100.), u, opt);
        const std::vector<double> xs   = plan.plane_positions(0);
        REQUIRE(xs.size() == 2);
        REQUIRE_THAT(xs[1], WithinAbs(490., 1e-4));
        REQUIRE(plan.parts_not_fitting() == 0);
        for (const SplitPart &part : plan.parts)
            REQUIRE(part.size().x() >= 10. - 1e-3);
    }
}

TEST_CASE("Split to Fit: margins and exclusion areas change the grid", "[SplitToVolume]")
{
    const indexed_triangle_set box = box_at(500., 100., 100.);
    SplitOptions               opt;
    // 500 / 246 needs three parts; with no margins 500 / 256 needs two.
    REQUIRE(split_plan_grid(box, split_usable_volume(square_printer(256., 256.), opt), opt).counts[0] == 3);
    opt.margin_xy = 0.;
    REQUIRE(split_plan_grid(box, split_usable_volume(square_printer(256., 256.), opt), opt).counts[0] == 2);

    // 240 mm fits 246, but not the 228 left by an exclusion corner.
    const indexed_triangle_set narrow = box_at(240., 100., 100.);
    SplitPrinterVolume         p      = square_printer(256., 256.);
    REQUIRE(split_plan_grid(narrow, split_usable_volume(p, SplitOptions{}), SplitOptions{}).counts[0] == 1);
    p.exclude_boxes.emplace_back(Vec2d(0., 0.), Vec2d(18., 28.));
    const SplitPlan plan = split_plan_grid(narrow, split_usable_volume(p, SplitOptions{}), SplitOptions{});
    REQUIRE(plan.counts[0] == 2);
    REQUIRE(has_warning(plan.warnings, SplitWarning::Kind::BedExclusion));
}

TEST_CASE("Split to Fit: a model taller than the printer gets Z cuts", "[SplitToVolume]")
{
    const SplitUsableVolume u    = split_usable_volume(square_printer(256., 256.), SplitOptions{});
    const SplitPlan         plan = split_plan_grid(box_at(100., 100., 400.), u, SplitOptions{});
    REQUIRE(plan.needs_cut[2]);
    REQUIRE(plan.counts[0] == 1);
    REQUIRE(plan.counts[1] == 1);
    REQUIRE(plan.counts[2] == 2);
    REQUIRE(plan.part_count() == 2);
    REQUIRE(plan.parts_not_fitting() == 0);
    REQUIRE(plan.parts[0].name == "Z1");
    REQUIRE(plan.parts[1].name == "Z2");
}

TEST_CASE("Split to Fit: a part that cannot fit is warned about", "[SplitToVolume]")
{
    const SplitUsableVolume u = split_usable_volume(square_printer(256., 256.), SplitOptions{});
    SplitOptions            opt;
    opt.allow_axis[2]    = false;  // the user unticked Z
    const SplitPlan plan = split_plan_grid(box_at(100., 100., 400.), u, opt);
    REQUIRE(plan.valid);
    REQUIRE(plan.counts[2] == 1);
    REQUIRE(plan.part_count() == 1);
    REQUIRE(plan.parts_not_fitting() == 1);
    REQUIRE(has_warning(plan.warnings, SplitWarning::Kind::PartDoesNotFit));

    // A plane dragged too far makes the part beyond it too long.
    SplitOptions dragged;
    dragged.pinned.push_back({ 2, 0, 50. });
    const SplitPlan moved = split_plan_grid(box_at(100., 100., 400.), u, dragged);
    REQUIRE(moved.planes.size() == 1);
    REQUIRE(moved.planes[0].pinned);
    REQUIRE_THAT(moved.planes[0].pos, WithinAbs(50., 1e-9));
    REQUIRE_THAT(moved.planes[0].nominal, WithinAbs(200., 1e-4));
    REQUIRE(moved.parts_not_fitting() == 1);
    REQUIRE_FALSE(moved.parts[1].fits);
}

TEST_CASE("Split to Fit: a solid interior cell is a whole part", "[SplitToVolume]")
{
    // 3 x 3 x 3 cells of a cube: the middle one has no surface at all, yet it is full.
    const SplitPlan plan = split_plan_grid(box_at(300., 300., 300.), usable_box(110., 110., 110.), SplitOptions{});
    REQUIRE(plan.part_count() == 27);
    // P5: every cell is one piece, the middle one too (six caps and no surface).
    REQUIRE(plan.piece_count() == 27);
    REQUIRE(plan.mates.size() == 54);
    for (const SplitPiece &p : plan.pieces)
        REQUIRE_THAT(p.volume, WithinRel(1e6, 1e-6));
    const SplitPart &middle = plan.parts[size_t(plan.cell_index({ { 1, 1, 1 } }))];
    REQUIRE_FALSE(middle.empty);
    REQUIRE_THAT(middle.size().x(), WithinAbs(100., 1e-3));
    REQUIRE_THAT(middle.size().y(), WithinAbs(100., 1e-3));
    REQUIRE_THAT(middle.size().z(), WithinAbs(100., 1e-3));
    // A face per neighbouring pair: 3 axes x 2 planes x 9 cells.
    REQUIRE(plan.faces.size() == 54);
}

TEST_CASE("Split to Fit: empty cells and cut-face ids", "[SplitToVolume]")
{
    const indexed_triangle_set u_mesh = u_shape();
    REQUIRE(its_num_open_edges(u_mesh) == 0);
    REQUIRE(its_volume(u_mesh) > 0.f);

    // Usable 100 x 100 x 205: three columns in X, two layers in Z (plane at 150). The cell between the
    // pillars at the top is empty.
    const SplitPlan plan = split_plan_grid(u_mesh, usable_box(100., 100., 205.), SplitOptions{});
    REQUIRE(plan.counts[0] == 3);
    REQUIRE(plan.counts[2] == 2);
    REQUIRE(plan.part_count() == 5);
    REQUIRE(plan.parts[size_t(plan.cell_index({ { 1, 0, 1 } }))].empty);

    // Faces: X1, X2 at the bottom (2), Z1 under the two pillars (2); none next to the empty cell.
    REQUIRE(plan.faces.size() == 4);
    std::set<std::string> keys, codes;
    for (const SplitFace &f : plan.faces) {
        REQUIRE(f.id >= 0);
        REQUIRE_FALSE(f.key.empty());
        REQUIRE_FALSE(f.mate_code.empty());
        keys.insert(f.key);
        codes.insert(f.mate_code);
        // Both parts list the face, and the label names both.
        const SplitPart &below = plan.parts[size_t(f.part_below)];
        const SplitPart &above = plan.parts[size_t(f.part_above)];
        REQUIRE(std::count(below.faces.begin(), below.faces.end(), f.id) == 1);
        REQUIRE(std::count(above.faces.begin(), above.faces.end(), f.id) == 1);
        REQUIRE(f.mate_label == below.name + " | " + above.name);
        REQUIRE(f.area > 0.);
    }
    REQUIRE(keys.size() == plan.faces.size());
    REQUIRE(codes.size() == plan.faces.size());
    REQUIRE(plan.faces.front().key == "X1:Z1");
    REQUIRE(plan.faces.front().mate_code == "X1.1");

    // The same grid twice gives the same ids, keys and codes.
    const SplitPlan again = split_plan_grid(u_mesh, usable_box(100., 100., 205.), SplitOptions{});
    REQUIRE(again.faces.size() == plan.faces.size());
    for (size_t i = 0; i < plan.faces.size(); ++i) {
        REQUIRE(again.faces[i].key == plan.faces[i].key);
        REQUIRE(again.faces[i].mate_code == plan.faces[i].mate_code);
    }
}

TEST_CASE("Split to Fit: a thin-walled model is reported as hollow", "[SplitToVolume]")
{
    // A 200 mm box with 2 mm walls: the outer box and the inner one, reversed.
    indexed_triangle_set shell = box_at(200., 200., 200.);
    indexed_triangle_set inner = box_at(196., 196., 196., Vec3d(2., 2., 2.));
    for (auto &f : inner.indices)
        std::swap(f[1], f[2]);
    its_merge(shell, inner);
    const SplitPlan hollow = split_plan_grid(shell, usable_box(150., 150., 250.), SplitOptions{});
    REQUIRE(has_warning(hollow.warnings, SplitWarning::Kind::HollowModel));
    const auto it = std::find_if(hollow.warnings.begin(), hollow.warnings.end(),
                                 [](const SplitWarning &w) { return w.kind == SplitWarning::Kind::HollowModel; });
    REQUIRE(it->value > 0.5);
    REQUIRE(it->value < 5.);

    const SplitPlan solid = split_plan_grid(box_at(200., 200., 200.), usable_box(150., 150., 250.), SplitOptions{});
    REQUIRE_FALSE(has_warning(solid.warnings, SplitWarning::Kind::HollowModel));
}

TEST_CASE("Split to Fit: too many parts is refused", "[SplitToVolume]")
{
    const SplitPlan plan = split_plan_grid(box_at(1000., 1000., 1000.), usable_box(5., 5., 5.), SplitOptions{});
    REQUIRE_FALSE(plan.valid);
    REQUIRE(has_warning(plan.warnings, SplitWarning::Kind::TooManyParts));
}

// ---- apply ------------------------------------------------------------------------------------------

namespace {

void check_apply(const indexed_triangle_set &its, const SplitUsableVolume &usable, size_t expected_parts)
{
    Model        model;
    ModelObject *mo        = add_object(model, its, "Block");
    const double total     = world_volume(*mo);
    const size_t volumes_0 = mo->volumes.size();

    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable, SplitOptions{});
    REQUIRE(plan.part_count() == expected_parts);

    int                    last_done = -1, last_total = -1;
    const SplitApplyResult res = split_apply(*mo, 0, plan, nullptr, [&](int done, int all) {
        last_done  = done;
        last_total = all;
    });
    REQUIRE_FALSE(res.canceled);
    REQUIRE(res.parts.size() == expected_parts);
    REQUIRE(last_total == int(expected_parts) - 1);
    REQUIRE(last_done == last_total);

    double                sum = 0.;
    std::set<std::string> names;
    const BoundingBoxf3   whole = mo->instance_bounding_box(0);
    for (size_t i = 0; i < res.parts.size(); ++i) {
        const ModelObject *part = res.parts[i];
        REQUIRE(part->instances.size() == 1);
        INFO("part " << part->name);
        REQUIRE(open_edges(*part) == 0);
        const BoundingBoxf3 bb = part->instance_bounding_box(0);
        // Inside the usable box, and inside its own cell.
        REQUIRE(bb.size().x() <= usable.size.x() + 1e-3);
        REQUIRE(bb.size().y() <= usable.size.y() + 1e-3);
        REQUIRE(bb.size().z() <= usable.size.z() + 1e-3);
        const SplitPart &planned = plan.parts[size_t(res.part_index[i])];
        // Apply may move a plane by up to about 0.07 mm off the vertices it would run through.
        for (int a = 0; a < 3; ++a) {
            REQUIRE(bb.min[a] >= planned.bbox.min[a] - 0.1);
            REQUIRE(bb.max[a] <= planned.bbox.max[a] + 0.1);
        }
        REQUIRE(part->name == "Block " + planned.name);
        names.insert(part->name);
        sum += world_volume(*part);
        // The assembled pose puts the part back inside the uncut object.
        const Transform3d   asm_m  = part->instances.front()->get_assemble_transformation().get_matrix();
        BoundingBoxf3       asm_bb;
        for (const ModelVolume *v : part->volumes)
            asm_bb.merge(v->mesh().transformed_bounding_box(asm_m * v->get_matrix()));
        for (int a = 0; a < 3; ++a) {
            REQUIRE(asm_bb.min[a] >= whole.min[a] - 1e-3);
            REQUIRE(asm_bb.max[a] <= whole.max[a] + 1e-3);
        }
    }
    REQUIRE(names.size() == res.parts.size());
    REQUIRE_THAT(sum, WithinRel(total, 1e-3));
    REQUIRE(res.warnings.empty());
    // A plain split is not a linked cut group (D10).
    for (const ModelObject *part : res.parts)
        REQUIRE(part->cut_id.id().invalid());
    // The source is untouched.
    REQUIRE(mo->volumes.size() == volumes_0);
    REQUIRE_THAT(world_volume(*mo), WithinRel(total, 1e-9));
}

} // namespace

TEST_CASE("Split to Fit: apply a 2 x 2 x 1 split of a block", "[SplitToVolume]")
{
    check_apply(box_at(100., 100., 100.), usable_box(60., 60., 200.), 4);
}

TEST_CASE("Split to Fit: apply a 2 x 2 x 2 split of a block", "[SplitToVolume]")
{
    check_apply(box_at(100., 100., 100.), usable_box(60., 60., 60.), 8);
}

TEST_CASE("Split to Fit P5: loose pieces of a section become parts of their own", "[SplitToVolume]")
{
    // P5 (owner, 2026-11-16) replaces D15 for pieces that fell apart: one Z plane at 150 only, so the
    // top section holds both pillar tops, which only met through the base below.
    Model                      model;
    ModelObject               *mo   = add_object(model, u_shape(), "U");
    const indexed_triangle_set mesh = split_object_mesh(*mo, 0);
    const SplitPlan            plan = split_plan_grid(mesh, usable_box(400., 400., 205.), SplitOptions{});
    REQUIRE(plan.part_count() == 2);   // sections
    REQUIRE(plan.piece_count() == 3);  // parts
    REQUIRE(plan.pieces.size() == 3);
    REQUIRE(plan.pieces[0].name == "Z1");
    REQUIRE(plan.pieces[0].id == "Z1_a");
    REQUIRE(plan.pieces[1].name == "Z2_a");
    REQUIRE(plan.pieces[2].name == "Z2_b");
    REQUIRE(plan.pieces[2].id == "Z2_b");
    double sum = 0.;
    for (const SplitPiece &p : plan.pieces) {
        REQUIRE(p.fits);
        REQUIRE_FALSE(p.tiny);
        sum += p.volume;
    }
    // Each pillar top is 50 x 50 x 150, on its own.
    for (int i : { 1, 2 }) {
        REQUIRE_THAT(plan.pieces[size_t(i)].volume, WithinRel(50. * 50. * 150., 1e-6));
        REQUIRE_THAT(plan.pieces[size_t(i)].size().x(), WithinAbs(50., 1e-3));
        REQUIRE_THAT(plan.pieces[size_t(i)].size().z(), WithinAbs(150., 1e-3));
    }
    REQUIRE_THAT(sum, WithinRel(double(its_volume(mesh)), 1e-6));
    // One face, two pairs (one per pillar), with the face's code and a letter each.
    REQUIRE(plan.faces.size() == 1);
    REQUIRE(plan.mates.size() == 2);
    std::set<std::string> codes;
    std::set<int>         tops;
    for (const SplitMate &m : plan.mates) {
        REQUIRE(m.piece_below == 0);
        tops.insert(m.piece_above);
        codes.insert(m.code);
        REQUIRE(m.patches.size() == 1);
        REQUIRE_THAT(m.area, WithinRel(2500., 1e-6));
    }
    REQUIRE(tops == std::set<int>{ 1, 2 });
    REQUIRE(codes == std::set<std::string>{ "Z1.1A", "Z1.1B" });
    // Every vertex of the model lies in some piece.
    REQUIRE(std::none_of(plan.vertex_piece.begin(), plan.vertex_piece.end(), [](int p) { return p < 0; }));

    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.parts.size() == 3);
    double                applied = 0.;
    std::set<std::string> names;
    for (size_t i = 0; i < res.parts.size(); ++i) {
        const ModelObject *part = res.parts[i];
        REQUIRE(open_edges(*part) == 0);
        REQUIRE(res.piece_index[i] == int(i));
        REQUIRE_THAT(world_volume(*part), WithinRel(plan.pieces[i].volume, 1e-4));
        names.insert(part->name);
        applied += world_volume(*part);
        // Each part is one piece now.
        indexed_triangle_set merged;
        for (const ModelVolume *v : part->volumes)
            if (v->is_model_part())
                its_merge(merged, v->mesh().its);
        REQUIRE(its_number_of_patches(merged) == 1);
    }
    REQUIRE(names == std::set<std::string>{ "U Z1", "U Z2_a", "U Z2_b" });
    REQUIRE_THAT(applied, WithinRel(world_volume(*mo), 1e-4));
    REQUIRE_FALSE(has_warning(res.warnings, SplitWarning::Kind::LoosePieces));
}

TEST_CASE("Split to Fit: apply skips empty cells", "[SplitToVolume]")
{
    Model           model;
    ModelObject    *mo   = add_object(model, u_shape(), "U");
    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable_box(100., 100., 205.), SplitOptions{});
    REQUIRE(plan.part_count() == 5);
    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.parts.size() == 5);
    double sum = 0.;
    for (const ModelObject *p : res.parts)
        sum += world_volume(*p);
    REQUIRE_THAT(sum, WithinRel(world_volume(*mo), 1e-3));
}

TEST_CASE("Split to Fit: a rotated and scaled instance", "[SplitToVolume]")
{
    Model        model;
    ModelObject *mo = add_object(model, box_at(100., 50., 50.), "Turned");
    mo->instances.front()->set_rotation(Vec3d(0., 0., 0.5 * PI));
    mo->instances.front()->set_scaling_factor(Vec3d(2., 2., 2.));
    mo->invalidate_bounding_box();
    // World size 100 x 200 x 100 after the turn and the scale.
    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable_box(150., 150., 150.), SplitOptions{});
    REQUIRE(plan.counts[0] == 1);
    REQUIRE(plan.counts[1] == 2);
    REQUIRE(plan.counts[2] == 1);
    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.parts.size() == 2);
    double sum = 0.;
    for (const ModelObject *p : res.parts) {
        const BoundingBoxf3 bb = p->instance_bounding_box(0);
        REQUIRE_THAT(bb.size().x(), WithinAbs(100., 1e-2));
        REQUIRE_THAT(bb.size().y(), WithinAbs(100., 1e-2));
        sum += world_volume(*p);
    }
    REQUIRE_THAT(sum, WithinRel(100. * 200. * 100., 1e-3));
}

TEST_CASE("Split to Fit: cancel stops the apply and leaves the source alone", "[SplitToVolume]")
{
    Model           model;
    ModelObject    *mo    = add_object(model, box_at(100., 100., 100.), "Block");
    const double    total = world_volume(*mo);
    const SplitPlan plan  = split_plan_grid(split_object_mesh(*mo, 0), usable_box(60., 60., 60.), SplitOptions{});
    int             calls = 0;
    const SplitApplyResult res = split_apply(*mo, 0, plan, [&calls]() { return ++calls > 2; });
    REQUIRE(res.canceled);
    REQUIRE(res.parts.empty());
    REQUIRE(mo->volumes.size() == 1);
    REQUIRE_THAT(world_volume(*mo), WithinRel(total, 1e-9));
}

// =====================================================================================================
// Phase 2: automatic dowels
// =====================================================================================================

namespace {

// GLGizmoCut3D::apply_cut_connectors() and the Plug / Dowel / Snap branch of get_connector_mesh() as
// they were before P2 moved them to libslic3r (copied from main at 2999085364, gizmo snap defaults).
// `height_only` = the placement fix of #1006 (add_cut_connector_volume): the unit mesh is centred on
// its height only, so a shape whose box is off its axis (a triangle) is not moved sideways. With
// `height_only` false it is the pre-#1006 code, which let add_volume() centre the whole box.
void p2_old_gizmo_bake(ModelObject *mo, const std::string &connector_name, bool height_only = true)
{
    using namespace Geometry;
    const float snap_space = 0.3f, snap_bulge = 0.15f;
    size_t      connector_id = mo->cut_id.connectors_cnt();
    for (const CutConnector &connector : mo->cut_connectors) {
        int sectorCount{ 1 };
        switch (CutConnectorShape(connector.attribs.shape)) {
        case CutConnectorShape::Triangle: sectorCount = 3; break;
        case CutConnectorShape::Square: sectorCount = 4; break;
        case CutConnectorShape::Circle: sectorCount = 360; break;
        case CutConnectorShape::Hexagon: sectorCount = 6; break;
        default: break;
        }
        indexed_triangle_set connector_mesh;
        if (connector.attribs.type == CutConnectorType::Snap)
            connector_mesh = its_make_snap(1.0, 1.0, snap_space, snap_bulge);
        else if (connector.attribs.style == CutConnectorStyle::Prism)
            connector_mesh = its_make_cylinder(1.0, 1.0, (2 * PI / sectorCount));
        else if (connector.attribs.type == CutConnectorType::Plug)
            connector_mesh = its_make_frustum(1.0, 1.0, (2 * PI / sectorCount));
        else
            connector_mesh = its_make_frustum_dowel(1.0, 1.0, sectorCount);
        if (height_only)
            its_translate(connector_mesh, Vec3f(0.f, 0.f, -float(bounding_box(connector_mesh).center().z())));
        TriangleMesh mesh       = TriangleMesh(connector_mesh);
        ModelVolume *new_volume = mo->add_volume(std::move(mesh), ModelVolumeType::NEGATIVE_VOLUME, !height_only);
        new_volume->set_transformation(translation_transform(connector.pos) * connector.rotation_m *
                                       rotation_transform(-connector.z_angle * Vec3d::UnitZ()) *
                                       scale_transform(Vec3f(connector.radius, connector.radius, connector.height).cast<double>()));
        new_volume->cut_info = { connector.attribs.type, connector.radius_tolerance, connector.height_tolerance };
        new_volume->name     = connector_name + "-" + std::to_string(++connector_id);
    }
    mo->cut_id.increase_connectors_cnt(mo->cut_connectors.size());
    mo->cut_connectors.clear();
}

ModelObject *p2_cube(Model &model)
{
    TriangleMesh mesh = make_cube(40.0, 40.0, 40.0);
    mesh.translate(Vec3f(-20.f, -20.f, -20.f));
    ModelObject *obj = model.add_object();
    obj->name        = "cube";
    obj->add_volume(mesh);
    obj->add_instance();
    return obj;
}

bool p2_same_mesh(const indexed_triangle_set &a, const indexed_triangle_set &b)
{
    return a.vertices == b.vertices && a.indices == b.indices;
}

BoundingBoxf3 p2_world_bbox(const ModelObject &o, const ModelVolume &v)
{
    return v.mesh().transformed_bounding_box(o.instances.front()->get_transformation().get_matrix() * v.get_matrix());
}

SplitOptions p2_dowels_on()
{
    SplitOptions opt;
    opt.dowels.enabled = true;
    return opt;
}

const SplitWarning *p2_find(const std::vector<SplitWarning> &w, SplitWarning::Kind kind)
{
    const auto it = std::find_if(w.begin(), w.end(), [kind](const SplitWarning &x) { return x.kind == kind; });
    return it == w.end() ? nullptr : &*it;
}

// Every check of a split with dowels (design P2 tests 5 and 6).
void p2_check_dowel_apply(const indexed_triangle_set &its, const SplitUsableVolume &usable, size_t expected_parts)
{
    Model        model;
    ModelObject *mo   = add_object(model, its, "Block");
    const SplitOptions opt  = p2_dowels_on();
    const SplitPlan    plan = split_plan_grid(split_object_mesh(*mo, 0), usable, opt);
    REQUIRE(plan.part_count() == expected_parts);
    REQUIRE_FALSE(plan.sites.empty());

    // Every face of a solid box gets dowels; the total is the sum over the faces.
    size_t sum = 0;
    for (const SplitFace &f : plan.faces) {
        INFO("face " << f.key);
        REQUIRE_FALSE(f.sites.empty());
        REQUIRE(int(f.sites.size()) <= opt.dowels.max_per_face);
        sum += f.sites.size();
    }
    REQUIRE(sum == plan.dowel_count());

    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE_FALSE(res.canceled);
    REQUIRE(res.parts.size() == expected_parts);
    REQUIRE(res.dowels.size() == plan.sites.size());
    REQUIRE(res.warnings.empty());

    // One linked cut group (D10): every part and every dowel, the check sum counting them all.
    const CutObjectBase &group = res.parts.front()->cut_id;
    REQUIRE_FALSE(group.id().invalid());
    REQUIRE(group.check_sum() == res.parts.size() + res.dowels.size());
    // (CutObjectBase::is_equal() is not const.)
    auto same_group = [&group](const ModelObject *o) {
        return o->cut_id.id() == group.id() && o->cut_id.check_sum() == group.check_sum() &&
               o->cut_id.connectors_cnt() == group.connectors_cnt();
    };
    for (const ModelObject *o : res.parts)
        REQUIRE(same_group(o));
    for (const ModelObject *o : res.dowels)
        REQUIRE(same_group(o));

    // Parts closed, connector names unique within each part.
    std::map<std::string, std::pair<int, const ModelVolume *>> holes; // hole name -> (plan part, volume)
    std::map<std::string, const ModelObject *>                 hole_owner;
    for (size_t i = 0; i < res.parts.size(); ++i) {
        const ModelObject *part = res.parts[i];
        INFO("part " << part->name);
        REQUIRE(open_edges(*part) == 0);
        std::set<std::string> names;
        size_t                connectors = 0;
        for (const ModelVolume *v : part->volumes)
            if (v->cut_info.is_connector) {
                ++connectors;
                names.insert(v->name);
                REQUIRE(v->type() == ModelVolumeType::NEGATIVE_VOLUME);
                REQUIRE(holes.count(v->name) == 0);  // each hole in exactly one part
                holes[v->name]      = { res.part_index[i], v };
                hole_owner[v->name] = part;
            }
        REQUIRE(names.size() == connectors);
        // A hole on every one of its cut faces.
        for (int fid : plan.parts[size_t(res.part_index[i])].faces) {
            const SplitFace &f   = plan.faces[size_t(fid)];
            bool             any = false;
            for (int si : f.sites)
                any = any || std::any_of(part->volumes.begin(), part->volumes.end(), [&](const ModelVolume *v) {
                          return v->name.rfind(plan.sites[size_t(si)].name + "_", 0) == 0;
                      });
            REQUIRE(any);
        }
    }
    REQUIRE(holes.size() == 2 * plan.sites.size());

    for (size_t k = 0; k < plan.sites.size(); ++k) {
        const SplitSite  &s  = plan.sites[k];
        const SplitFace  &f  = plan.faces[size_t(s.face)];
        const SplitPlane &pl = plan.planes[size_t(s.plane)];
        const int a = pl.axis, ua = (a + 1) % 3, va = (a + 2) % 3;
        INFO("site " << s.name);
        REQUIRE(holes.count(s.name + "_A") == 1);
        REQUIRE(holes.count(s.name + "_B") == 1);
        // _A in the part above the plane, _B in the part below: the two mating parts.
        REQUIRE(holes[s.name + "_A"].first == f.part_above);
        REQUIRE(holes[s.name + "_B"].first == f.part_below);
        const BoundingBoxf3 up   = p2_world_bbox(*hole_owner[s.name + "_A"], *holes[s.name + "_A"].second);
        const BoundingBoxf3 down = p2_world_bbox(*hole_owner[s.name + "_B"], *holes[s.name + "_B"].second);
        // Both at the site, on either side of the plane (apply may move a plane by up to ~0.07 mm).
        for (const BoundingBoxf3 *bb : { &up, &down }) {
            REQUIRE_THAT(bb->center()[ua], WithinAbs(s.pos[ua], 1e-3));
            REQUIRE_THAT(bb->center()[va], WithinAbs(s.pos[va], 1e-3));
            // The hole is the dowel plus the tolerance all round.
            REQUIRE_THAT(0.5 * bb->size()[ua], WithinAbs(0.5 * s.diameter + opt.dowels.tolerance, 1e-3));
        }
        const double depth = s.half_length + 0.5 * opt.dowels.depth_tolerance;
        REQUIRE_THAT(up.max[a], WithinAbs(pl.pos + depth, 0.1));
        REQUIRE_THAT(up.min[a], WithinAbs(pl.pos - CONNECTOR_HOLE_OVERLAP, 0.1));
        REQUIRE_THAT(down.min[a], WithinAbs(pl.pos - depth, 0.1));
        REQUIRE_THAT(down.max[a], WithinAbs(pl.pos + CONNECTOR_HOLE_OVERLAP, 0.1));
        // No hole reaches another cut plane.
        for (const BoundingBoxf3 *bb : { &up, &down })
            for (const SplitPlane &other : plan.planes) {
                if (&other == &pl)
                    continue;
                const int b = other.axis;
                REQUIRE_FALSE((bb->min[b] < other.pos + 0.1 && bb->max[b] > other.pos - 0.1));
            }

        // The dowel: its own object, named after the site, the nominal size: it fits its holes with
        // the tolerance.
        const ModelObject *dowel = res.dowels[k];
        REQUIRE(dowel->name == "Block-Dowel-" + s.name);
        REQUIRE(dowel->volumes.size() == 1);
        REQUIRE(dowel->volumes.front()->is_model_part());
        const BoundingBoxf3 db = dowel->volumes.front()->mesh().transformed_bounding_box(dowel->volumes.front()->get_matrix());
        REQUIRE_THAT(db.size().x(), WithinAbs(s.diameter, 1e-3));
        REQUIRE_THAT(db.size().z(), WithinAbs(2. * s.half_length, 1e-3));
        REQUIRE_THAT(0.5 * up.size()[ua] - 0.5 * db.size().x(), WithinAbs(opt.dowels.tolerance, 1e-3));
    }
}

} // namespace

TEST_CASE("Split to Fit P2: the connector bake moved out of the Cut gizmo is unchanged", "[SplitToVolume][CutConnectors]")
{
    struct Kind { CutConnectorType type; CutConnectorStyle style; CutConnectorShape shape; };
    const std::vector<Kind> kinds = {
        { CutConnectorType::Dowel, CutConnectorStyle::Prism, CutConnectorShape::Circle },
        { CutConnectorType::Dowel, CutConnectorStyle::Prism, CutConnectorShape::Square },
        { CutConnectorType::Plug, CutConnectorStyle::Prism, CutConnectorShape::Hexagon },
        { CutConnectorType::Plug, CutConnectorStyle::Frustum, CutConnectorShape::Circle },
        { CutConnectorType::Snap, CutConnectorStyle::Prism, CutConnectorShape::Circle },
    };
    for (const Kind &kind : kinds) {
        INFO("type " << int(kind.type) << " style " << int(kind.style) << " shape " << int(kind.shape));
        // A turned connector off the middle, as the gizmo leaves it once it has doubled a Dowel's height.
        const Transform3d rot = Geometry::rotation_transform(Vec3d(0.1, -0.2, 0.3));
        CutConnector      c(Vec3d(4., -3., 0.5), rot, 3.f, 12.f, 0.1f, 0.2f, 0.4f,
                            CutConnectorAttributes(kind.type, kind.style, kind.shape));
        Model old_model, new_model;
        ModelObject *old_obj = p2_cube(old_model);
        ModelObject *new_obj = p2_cube(new_model);
        old_obj->cut_connectors.push_back(c);
        new_obj->cut_connectors.push_back(c);
        p2_old_gizmo_bake(old_obj, "Connector");
        bake_cut_connectors(new_obj, "Connector");

        REQUIRE(new_obj->cut_connectors.empty());
        REQUIRE(new_obj->cut_id.connectors_cnt() == old_obj->cut_id.connectors_cnt());
        REQUIRE(new_obj->volumes.size() == old_obj->volumes.size());
        for (size_t i = 0; i < old_obj->volumes.size(); ++i) {
            const ModelVolume *o = old_obj->volumes[i];
            const ModelVolume *n = new_obj->volumes[i];
            REQUIRE(n->name == o->name);
            REQUIRE(n->type() == o->type());
            REQUIRE(p2_same_mesh(n->mesh().its, o->mesh().its));
            REQUIRE(n->get_matrix().isApprox(o->get_matrix(), 1e-12));
            REQUIRE(n->cut_info.is_connector == o->cut_info.is_connector);
            REQUIRE(n->cut_info.is_processed == o->cut_info.is_processed);
            REQUIRE(n->cut_info.connector_type == o->cut_info.connector_type);
            REQUIRE(n->cut_info.radius_tolerance == o->cut_info.radius_tolerance);
            REQUIRE(n->cut_info.height_tolerance == o->cut_info.height_tolerance);
        }

        // These shapes are centred on their axis, so the #1006 placement leaves them where the
        // pre-#1006 code put them (only a shape off its axis, a triangle, moves).
        {
            Model        pre_model;
            ModelObject *pre_obj = p2_cube(pre_model);
            pre_obj->cut_connectors.push_back(c);
            p2_old_gizmo_bake(pre_obj, "Connector", false);
            REQUIRE(pre_obj->volumes.size() == new_obj->volumes.size());
            const ModelVolume  *p  = pre_obj->volumes.back();
            const ModelVolume  *n  = new_obj->volumes.back();
            const BoundingBoxf3 pb = p->mesh().transformed_bounding_box(p->get_matrix());
            const BoundingBoxf3 nb = n->mesh().transformed_bounding_box(n->get_matrix());
            for (int a = 0; a < 3; ++a) {
                REQUIRE_THAT(nb.min[a], WithinAbs(pb.min[a], 1e-4));
                REQUIRE_THAT(nb.max[a], WithinAbs(pb.max[a], 1e-4));
            }
        }

        // And the cut a single manual connector gives is the same.
        const bool                     dowel = kind.type == CutConnectorType::Dowel;
        const ModelObjectCutAttributes attrs = dowel ? (ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                                        ModelObjectCutAttribute::CreateDowels) :
                                                       (ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
        Cut                    old_cut(old_obj, 0, Transform3d::Identity(), attrs);
        Cut                    new_cut(new_obj, 0, Transform3d::Identity(), attrs);
        const ModelObjectPtrs &old_res = old_cut.perform_with_plane();
        const ModelObjectPtrs &new_res = new_cut.perform_with_plane();
        REQUIRE(new_res.size() == old_res.size());
        REQUIRE(new_res.size() == (dowel ? 3u : 2u));
        for (size_t k = 0; k < old_res.size(); ++k) {
            REQUIRE(new_res[k]->name == old_res[k]->name);
            REQUIRE(new_res[k]->volumes.size() == old_res[k]->volumes.size());
            for (size_t i = 0; i < old_res[k]->volumes.size(); ++i) {
                const ModelVolume *o = old_res[k]->volumes[i];
                const ModelVolume *n = new_res[k]->volumes[i];
                REQUIRE(n->name == o->name);
                REQUIRE(n->type() == o->type());
                REQUIRE(p2_same_mesh(n->mesh().its, o->mesh().its));
                REQUIRE(n->get_matrix().isApprox(o->get_matrix(), 1e-12));
            }
        }
    }
}

TEST_CASE("Split to Fit P2: sites on a 60 x 40 face", "[SplitToVolume]")
{
    ExPolygon face;
    face.contour.points = { Point::new_scale(0., 0.), Point::new_scale(60., 0.), Point::new_scale(60., 40.), Point::new_scale(0., 40.) };
    const BoundingBoxf all(Vec2d(-1., -1.), Vec2d(61., 41.));
    const double       inset = 3. + 3.1;   // wall margin + a 6 mm dowel's hole radius

    const std::vector<Vec2d> three = split_pick_sites(face, all, inset, 3, 20.);
    REQUIRE(three.size() == 3);
    for (size_t i = 0; i < three.size(); ++i) {
        const Vec2d &p = three[i];
        REQUIRE(p.x() >= inset - 1e-6);
        REQUIRE(p.x() <= 60. - inset + 1e-6);
        REQUIRE(p.y() >= inset - 1e-6);
        REQUIRE(p.y() <= 40. - inset + 1e-6);
        for (size_t j = 0; j < i; ++j)
            REQUIRE((p - three[j]).norm() >= 20. - 1e-6);
    }
    // The first is the point farthest from the outline: the middle.
    REQUIRE_THAT(three.front().x(), WithinAbs(30., 1.));
    REQUIRE_THAT(three.front().y(), WithinAbs(20., 1.));

    // The cap holds; a bigger cap still keeps the spacing.
    REQUIRE(split_pick_sites(face, all, inset, 1, 20.).size() == 1);
    const std::vector<Vec2d> many = split_pick_sites(face, all, inset, 10, 20.);
    REQUIRE(many.size() >= 3);
    for (size_t i = 0; i < many.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            REQUIRE((many[i] - many[j]).norm() >= 20. - 1e-6);

    // A keep-out band takes the right side off: nothing beyond it.
    const BoundingBoxf left(Vec2d(-1., -1.), Vec2d(25., 41.));
    for (const Vec2d &p : split_pick_sites(face, left, inset, 3, 10.))
        REQUIRE(p.x() <= 25. + 1e-6);

    // A refused site is replaced by the next best one.
    int refused = 0;
    const std::vector<Vec2d> after_refusal =
        split_pick_sites(face, all, inset, 1, 20., [&refused](const Vec2d &) { return refused++ > 0; });
    REQUIRE(after_refusal.size() == 1);
    REQUIRE(refused == 2);
    // A face too small for the inset gets none.
    ExPolygon small;
    small.contour.points = { Point::new_scale(0., 0.), Point::new_scale(10., 0.), Point::new_scale(10., 10.), Point::new_scale(0., 10.) };
    REQUIRE(split_pick_sites(small, all, inset, 3, 20.).empty());
}

TEST_CASE("Split to Fit P2: dowels off leaves the plan as in P1", "[SplitToVolume]")
{
    const SplitPlan plan = split_plan_grid(box_at(100., 100., 100.), usable_box(60., 60., 60.), SplitOptions{});
    REQUIRE(plan.sites.empty());
    for (const SplitFace &f : plan.faces)
        REQUIRE(f.sites.empty());
    REQUIRE_FALSE(plan.dowel_options.enabled);
}

TEST_CASE("Split to Fit P2: a small face gets one smaller dowel, or none", "[SplitToVolume]")
{
    // A 10 x 10 bar, cut once across.
    const indexed_triangle_set bar = box_at(300., 10., 10.);
    SplitOptions               opt = p2_dowels_on();
    const SplitPlan            plan = split_plan_grid(bar, usable_box(200., 200., 200.), opt);
    REQUIRE(plan.faces.size() == 1);
    REQUIRE(plan.sites.size() == 1);
    const SplitSite &s = plan.sites.front();
    REQUIRE(s.scaled_down);
    REQUIRE_THAT(s.diameter, WithinAbs(3., 1e-9));
    REQUIRE_THAT(s.pos.y(), WithinAbs(5., 0.6));
    REQUIRE_THAT(s.pos.z(), WithinAbs(5., 0.6));
    REQUIRE(has_warning(plan.warnings, SplitWarning::Kind::DowelScaledDown));
    // One round dowel: the parts can turn on it.
    REQUIRE(has_warning(plan.warnings, SplitWarning::Kind::DowelCanTurn));
    REQUIRE_FALSE(has_warning(plan.warnings, SplitWarning::Kind::NoDowelOnFace));

    // A square dowel is keyed: no turning warning.
    opt.dowels.shape = SplitDowelShape::Square;
    const SplitPlan square = split_plan_grid(bar, usable_box(200., 200., 200.), opt);
    REQUIRE(square.sites.size() == 1);
    REQUIRE_FALSE(has_warning(square.warnings, SplitWarning::Kind::DowelCanTurn));

    // Without scaling down: no dowel, and the face is reported as too small.
    opt.dowels.shape      = SplitDowelShape::Round;
    opt.dowels.scale_down = false;
    const SplitPlan none  = split_plan_grid(bar, usable_box(200., 200., 200.), opt);
    REQUIRE(none.sites.empty());
    const SplitWarning *w = p2_find(none.warnings, SplitWarning::Kind::NoDowelOnFace);
    REQUIRE(w != nullptr);
    REQUIRE(w->face == 0);
    REQUIRE(w->value == SPLIT_NO_DOWEL_TOO_SMALL);
}

TEST_CASE("Split to Fit P2: the depth check shortens or drops a dowel in thin material", "[SplitToVolume]")
{
    const indexed_triangle_set slab = box_at(300., 60., 60.);
    SplitOptions               opt  = p2_dowels_on();
    // The plane pinned 11.5 mm from the end: 10 mm half-length + 0.05 depth tolerance + 1.5 mm of
    // material does not fit, so the dowels get 11.5 - 1.5 - 0.05 = 9.95 mm on that side.
    opt.pinned.push_back({ 0, 0, 300. - 11.5 });
    const SplitPlan near_end = split_plan_grid(slab, usable_box(200., 200., 200.), opt);
    REQUIRE_FALSE(near_end.sites.empty());
    for (const SplitSite &s : near_end.sites) {
        REQUIRE(s.shortened);
        REQUIRE_THAT(s.half_length, WithinAbs(9.95, 1e-3));
    }
    const SplitWarning *shortened = p2_find(near_end.warnings, SplitWarning::Kind::DowelShortened);
    REQUIRE(shortened != nullptr);
    REQUIRE_THAT(shortened->value, WithinAbs(19.9, 1e-3));

    // 3 mm from the end: nothing holds there, at any size.
    opt.pinned.front().pos   = 300. - 3.;
    const SplitPlan thin     = split_plan_grid(slab, usable_box(200., 200., 200.), opt);
    REQUIRE(thin.sites.empty());
    const SplitWarning *w = p2_find(thin.warnings, SplitWarning::Kind::NoDowelOnFace);
    REQUIRE(w != nullptr);
    REQUIRE(w->value == SPLIT_NO_DOWEL_TOO_THIN);

    // A part 12 mm thick between two planes: the dowel stops 2 mm short of the next plane.
    SplitOptions between = p2_dowels_on();
    between.pinned.push_back({ 0, 0, 100. });
    between.pinned.push_back({ 0, 1, 112. });
    const SplitPlan three = split_plan_grid(slab, usable_box(110., 200., 200.), between);
    REQUIRE(three.planes.size() == 2);
    for (const SplitSite &s : three.sites)
        REQUIRE(s.half_length + 0.5 * between.dowels.depth_tolerance <= 12. - between.dowels.corner_keepout + 1e-6);
}

TEST_CASE("Split to Fit P2: dowels keep out of the other planes on a 2 x 2 x 2 grid", "[SplitToVolume]")
{
    const SplitOptions opt  = p2_dowels_on();
    const SplitPlan    plan = split_plan_grid(box_at(100., 100., 100.), usable_box(60., 60., 60.), opt);
    REQUIRE(plan.faces.size() == 12);
    REQUIRE_FALSE(plan.sites.empty());
    std::set<std::string> names;
    for (const SplitSite &s : plan.sites) {
        INFO("site " << s.name);
        names.insert(s.name);
        const SplitPlane &own = plan.planes[size_t(s.plane)];
        REQUIRE_THAT(s.pos[own.axis], WithinAbs(own.pos, 1e-9));
        for (const SplitPlane &other : plan.planes) {
            if (other.axis == own.axis)
                continue;
            // The hole's footprint plus the keep-out band stays off every crossing plane.
            REQUIRE(std::abs(s.pos[other.axis] - other.pos) >= s.footprint_radius + opt.dowels.corner_keepout - 1e-6);
        }
        // Inside the wall margin of the model's outside.
        for (int b = 0; b < 3; ++b)
            if (b != own.axis) {
                REQUIRE(s.pos[b] >= opt.dowels.wall_margin + s.footprint_radius - 1e-6);
                REQUIRE(s.pos[b] <= 100. - opt.dowels.wall_margin - s.footprint_radius + 1e-6);
            }
        REQUIRE_THAT(s.half_length, WithinAbs(10., 1e-9));
    }
    REQUIRE(names.size() == plan.sites.size());
}

TEST_CASE("Split to Fit P2: apply a 2 x 2 x 1 split of a block with dowels", "[SplitToVolume]")
{
    p2_check_dowel_apply(box_at(100., 100., 100.), usable_box(60., 60., 200.), 4);
}

TEST_CASE("Split to Fit P2: apply a 2 x 2 x 2 split of a block with dowels", "[SplitToVolume]")
{
    p2_check_dowel_apply(box_at(100., 100., 100.), usable_box(60., 60., 60.), 8);
}

TEST_CASE("Split to Fit P2: a hollow box warns and still splits", "[SplitToVolume]")
{
    // D6: warn and still split, with whatever dowels fit (none in 2 mm walls).
    indexed_triangle_set shell = box_at(200., 200., 200.);
    indexed_triangle_set inner = box_at(196., 196., 196., Vec3d(2., 2., 2.));
    for (auto &f : inner.indices)
        std::swap(f[1], f[2]);
    its_merge(shell, inner);

    Model           model;
    ModelObject    *mo   = add_object(model, shell, "Shell");
    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable_box(150., 150., 250.), p2_dowels_on());
    REQUIRE(plan.part_count() == 4);
    REQUIRE(plan.sites.empty());
    REQUIRE(has_warning(plan.warnings, SplitWarning::Kind::HollowModel));
    const size_t no_dowel = size_t(std::count_if(plan.warnings.begin(), plan.warnings.end(),
                                                 [](const SplitWarning &w) { return w.kind == SplitWarning::Kind::NoDowelOnFace; }));
    REQUIRE(no_dowel == plan.faces.size());

    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.parts.size() == 4);
    REQUIRE(res.dowels.empty());
    // No dowels: not a linked group (D10).
    for (const ModelObject *p : res.parts) {
        REQUIRE(p->cut_id.id().invalid());
        REQUIRE(open_edges(*p) == 0);
    }
}

TEST_CASE("Split to Fit P2: pins instead of dowels", "[SplitToVolume]")
{
    Model        model;
    ModelObject *mo  = add_object(model, box_at(300., 60., 60.), "Bar");
    SplitOptions opt = p2_dowels_on();
    opt.dowels.type  = SplitConnectorType::Pin;
    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable_box(200., 200., 200.), opt);
    REQUIRE(plan.part_count() == 2);
    REQUIRE_FALSE(plan.sites.empty());

    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.parts.size() == 2);
    REQUIRE(res.dowels.size() == plan.sites.size());
    for (size_t k = 0; k < res.dowels.size(); ++k) {
        REQUIRE(res.dowels[k]->name == "Bar-Pin-" + plan.sites[k].name);
        REQUIRE(res.dowels[k]->volumes.front()->cut_info.is_pushfit());
    }
    for (const SplitSite &s : plan.sites) {
        size_t a = 0, b = 0;
        for (const ModelObject *p : res.parts)
            for (const ModelVolume *v : p->volumes) {
                if (v->name == s.name + "_A" && v->is_negative_volume())
                    ++a;
                if (v->name == s.name + "_B" && v->is_negative_volume())
                    ++b;
            }
        REQUIRE(a == 1);
        REQUIRE(b == 1);
    }
    for (const ModelObject *p : res.parts)
        REQUIRE(open_edges(*p) == 0);
}

TEST_CASE("Split to Fit P2: a split with dowels survives a project round trip", "[SplitToVolume][3mf]")
{
    Model           model;
    ModelObject    *mo   = add_object(model, box_at(300., 60., 60.), "Bar");
    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable_box(200., 200., 200.), p2_dowels_on());
    SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.parts.size() == 2);
    REQUIRE_FALSE(res.dowels.empty());
    size_t holes = 0;
    for (const ModelObject *o : res.model->objects)
        for (const ModelVolume *v : o->volumes)
            if (v->is_negative_volume())
                ++holes;
    REQUIRE(holes == 2 * res.dowels.size());
    for (ModelObject *o : res.model->objects)
        o->ensure_on_bed();

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string  test_file    = (tmp_root / "split_to_fit_dowels.3mf").string();
    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    StoreParams        store_params;
    store_params.path     = test_file.c_str();
    store_params.model    = res.model.get();
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    const bool stored     = store_bbs_3mf(store_params);

    Model                     dst;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool loaded = stored && load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst, &plate_data, &project_presets, &is_bbl_3mf,
                                               &file_version, nullptr,
                                               LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                                                   LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    boost::filesystem::remove(test_file);
    REQUIRE(stored);
    REQUIRE(loaded);

    REQUIRE(dst.objects.size() == res.model->objects.size());
    size_t loaded_holes = 0;
    for (const ModelObject *o : dst.objects) {
        INFO("object " << o->name);
        // Still one linked group with the same check sum.
        REQUIRE(o->is_cut());
        REQUIRE(o->cut_id.check_sum() == res.parts.size() + res.dowels.size());
        REQUIRE(o->cut_id.id() == dst.objects.front()->cut_id.id());
        for (const ModelVolume *v : o->volumes)
            if (v->is_negative_volume()) {
                ++loaded_holes;
                REQUIRE(v->cut_info.is_connector);
            }
    }
    REQUIRE(loaded_holes == holes);
}

// =====================================================================================================
// Phase 3: plane adjustment and the part checks
// =====================================================================================================

namespace {

// A 60 x 60 bar along X, `length` long, with a 10 x 10 neck from `neck_lo` to `neck_hi` (three
// boxes touching end to end).
indexed_triangle_set p3_bar_with_neck(double length, double neck_lo, double neck_hi)
{
    indexed_triangle_set its = box_at(neck_lo, 60., 60.);
    its_merge(its, box_at(neck_hi - neck_lo, 10., 10., Vec3d(neck_lo, 25., 25.)));
    its_merge(its, box_at(length - neck_hi, 60., 60., Vec3d(neck_hi, 0., 0.)));
    return its;
}

SplitOptions p3_adjust_on()
{
    SplitOptions opt;
    opt.adjust_planes = true;
    return opt;
}

const SplitWarning *p3_find_plane(const std::vector<SplitWarning> &w, SplitWarning::Kind kind, int plane)
{
    const auto it = std::find_if(w.begin(), w.end(), [kind, plane](const SplitWarning &x) { return x.kind == kind && x.plane == plane; });
    return it == w.end() ? nullptr : &*it;
}

// Every part on `axis`, between its planes (or a plane and the model's end), as the hard rules see it.
std::vector<double> p3_spans(const SplitPlan &plan, int axis)
{
    std::vector<double> out;
    double              prev = plan.model_bbox.min[axis];
    for (double p : plan.plane_positions(axis)) {
        out.push_back(p - prev);
        prev = p;
    }
    out.push_back(plan.model_bbox.max[axis] - prev);
    return out;
}

ExPolygon p3_rect(double x0, double y0, double x1, double y1)
{
    ExPolygon ex;
    ex.contour.points = { Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1) };
    return ex;
}

} // namespace

TEST_CASE("Split to Fit P3: section costs worked by hand", "[SplitToVolume]")
{
    SplitOptions opt = p2_dowels_on();   // 6 mm dowels, 3 mm wall margin, limit 10 mm
    const SplitAdjustConstants &k = opt.adjust;
    REQUIRE(k.w_small == 1.0);
    REQUIRE(k.w_nodowel == 2.0);
    REQUIRE(k.w_busy == 1.0);
    REQUIRE(k.w_ragged == 0.5);
    REQUIRE(k.w_away == 0.2);

    SECTION("a 60 x 40 rectangle")
    {
        const ExPolygons       sec{ p3_rect(0., 0., 60., 40.) };
        const SplitSectionCost c = split_section_cost(sec, 2400., 2400., 0., opt);
        const double           ragged = 200. / (2. * std::sqrt(PI * 2400.)) - 1.;   // 0.1517
        REQUIRE_THAT(c.area, WithinAbs(2400., 1e-6));
        REQUIRE(c.small_islands == 0);
        REQUIRE_THAT(c.nodowel, WithinAbs(0., 1e-12));
        REQUIRE_THAT(c.busy, WithinAbs(0., 1e-12));
        REQUIRE_THAT(c.ragged, WithinAbs(ragged, 1e-6));
        REQUIRE_THAT(c.total, WithinAbs(0.5 * ragged, 1e-6));
        // The section grows from 2000 to 2600 mm2 across it: busy = 600 / 2400.
        REQUIRE_THAT(split_section_cost(sec, 2000., 2600., 0., opt).busy, WithinAbs(0.25, 1e-9));
        // 5 mm from the nominal place with a 10 mm limit: away = 0.5, weighted 0.1.
        REQUIRE_THAT(split_section_cost(sec, 2400., 2400., -5., opt).total, WithinAbs(0.5 * ragged + 0.1, 1e-6));
    }
    SECTION("a rectangle and a small island")
    {
        // The 5 x 5 island is under pi * (3 + 3)^2 = 113 mm2 and has no room for a dowel.
        const ExPolygons       sec{ p3_rect(0., 0., 60., 40.), p3_rect(100., 0., 105., 5.) };
        const SplitSectionCost c = split_section_cost(sec, 2425., 2425., 0., opt);
        REQUIRE_THAT(c.area, WithinAbs(2425., 1e-6));
        REQUIRE(c.small_islands == 1);
        REQUIRE_THAT(c.nodowel, WithinAbs(25. / 2425., 1e-6));
        const double ragged = 220. / (2. * std::sqrt(PI * 2425.)) - 1.;
        REQUIRE_THAT(c.ragged, WithinAbs(ragged, 1e-6));
        REQUIRE_THAT(c.total, WithinAbs(1. + 2. * 25. / 2425. + 0.5 * ragged, 1e-6));
    }
    SECTION("a ring with 2 mm walls")
    {
        ExPolygon ring = p3_rect(0., 0., 40., 40.);
        Polygon   hole;
        hole.points = { Point::new_scale(2., 2.), Point::new_scale(2., 38.), Point::new_scale(38., 38.), Point::new_scale(38., 2.) };
        ring.holes.push_back(hole);
        const ExPolygons       sec{ ring };
        const SplitSectionCost c = split_section_cost(sec, 304., 304., 0., opt);
        REQUIRE_THAT(c.area, WithinAbs(1600. - 1296., 1e-6));
        REQUIRE(c.small_islands == 0);
        // No room for a dowel anywhere, and an outline far longer than a circle's: both capped at 1.
        REQUIRE_THAT(c.nodowel, WithinAbs(1., 1e-12));
        REQUIRE_THAT(c.ragged, WithinAbs(1., 1e-12));
        REQUIRE_THAT(c.total, WithinAbs(2.5, 1e-9));
        // With dowels off the dowel term is not counted.
        opt.dowels.enabled = false;
        REQUIRE_THAT(split_section_cost(sec, 304., 304., 0., opt).total, WithinAbs(0.5, 1e-9));
    }
}

TEST_CASE("Split to Fit P3: a plane moves off a thin neck", "[SplitToVolume]")
{
    // 300 mm on a 200 mm printer: one plane, nominally at 150, through a 10 x 10 neck from 145.5 to 152.5.
    const indexed_triangle_set bar = p3_bar_with_neck(300., 145.5, 152.5);
    const SplitUsableVolume    u   = usable_box(200., 200., 200.);

    const SplitPlan off = split_plan_grid(bar, u, SplitOptions{});
    REQUIRE(off.planes.size() == 1);
    REQUIRE_THAT(off.planes[0].pos, WithinAbs(150., 1e-9));
    REQUIRE_FALSE(off.planes[0].adjusted);
    REQUIRE_FALSE(has_warning(off.warnings, SplitWarning::Kind::PlaneMoved));

    const SplitPlan on = split_plan_grid(bar, u, p3_adjust_on());
    REQUIRE(on.planes.size() == 1);
    const SplitPlane &pl = on.planes[0];
    INFO("plane at " << pl.pos);
    REQUIRE(pl.adjusted);
    REQUIRE_THAT(pl.nominal, WithinAbs(150., 1e-9));
    // Out of the neck, at least a millimetre clear of its ends, within the limit; the nearest such
    // place is 154 (4 mm) rather than 144 (6 mm).
    REQUIRE((pl.pos <= 145.5 - 1. + 1e-6 || pl.pos >= 152.5 + 1. - 1e-6));
    REQUIRE(std::abs(pl.pos - 150.) <= 10. + 1e-9);
    REQUIRE_THAT(pl.pos, WithinAbs(154., 1e-6));
    REQUIRE_THAT(on.planes[0].section_area, WithinAbs(3600., 1e-3));
    const SplitWarning *moved = p3_find_plane(on.warnings, SplitWarning::Kind::PlaneMoved, 0);
    REQUIRE(moved != nullptr);
    REQUIRE_THAT(moved->value, WithinAbs(4., 1e-6));
    REQUIRE(moved->detail == int(SplitAdjustReason::ThinPieces));
    REQUIRE_FALSE(has_warning(on.warnings, SplitWarning::Kind::PlaneInDetail));
    REQUIRE(on.parts_not_fitting() == 0);
}

TEST_CASE("Split to Fit P3: a move that would make a part too long is refused", "[SplitToVolume]")
{
    // Full size first on a 246 mm printer: planes at 246 and 490 (the 8 mm remainder pulled in to 10).
    // A neck from 240.5 to 248.5 sits on the first plane; leaving it outwards would make the first
    // part longer than 246 mm, so it goes inwards and the second plane follows.
    const indexed_triangle_set bar = p3_bar_with_neck(500., 240.5, 248.5);
    const SplitUsableVolume    u   = usable_box(246., 200., 200.);
    SplitOptions               opt = p3_adjust_on();
    opt.layout                     = SplitLayout::FullSizeFirst;

    const SplitPlan plan = split_plan_grid(bar, u, opt);
    REQUIRE(plan.planes.size() == 2);
    REQUIRE_THAT(plan.planes[0].nominal, WithinAbs(246., 1e-9));
    REQUIRE_THAT(plan.planes[1].nominal, WithinAbs(490., 1e-9));
    INFO("planes at " << plan.planes[0].pos << " and " << plan.planes[1].pos);
    // Moved inwards, out of the neck.
    REQUIRE(plan.planes[0].pos <= 240.5 - 1. + 1e-6);
    REQUIRE(plan.planes[0].pos >= 246. - 10. - 1e-9);
    const SplitWarning *refused = p3_find_plane(plan.warnings, SplitWarning::Kind::AdjustRefused, 0);
    REQUIRE(refused != nullptr);
    REQUIRE(refused->value > 0.);   // it wanted to go outwards
    // The second plane moved too, to keep the middle part within 246 mm.
    REQUIRE(plan.planes[1].adjusted);
    const SplitWarning *followed = p3_find_plane(plan.warnings, SplitWarning::Kind::PlaneMoved, 1);
    REQUIRE(followed != nullptr);
    REQUIRE(followed->detail == int(SplitAdjustReason::KeepFit));
    for (double s : p3_spans(plan, 0)) {
        REQUIRE(s <= 246. + 1e-6);
        REQUIRE(s >= 10. - 1e-6);
    }
    REQUIRE(plan.parts_not_fitting() == 0);
}

TEST_CASE("Split to Fit P3: a plane next to a flat face is pushed away", "[SplitToVolume]")
{
    // A 100 x 100 flange to x = 200.3, then a 50 x 50 bar to 400: the even plane at 200 would leave a
    // 0.3 mm skin of flange on the second part.
    indexed_triangle_set stepped = box_at(200.3, 100., 100.);
    its_merge(stepped, box_at(400. - 200.3, 50., 50., Vec3d(200.3, 25., 25.)));
    const SplitUsableVolume u = usable_box(300., 300., 300.);

    REQUIRE_THAT(split_plan_grid(stepped, u, SplitOptions{}).planes[0].pos, WithinAbs(200., 1e-9));

    const SplitPlan plan = split_plan_grid(stepped, u, p3_adjust_on());
    REQUIRE(plan.planes.size() == 1);
    INFO("plane at " << plan.planes[0].pos);
    REQUIRE(std::abs(plan.planes[0].pos - 200.3) >= 1. - 1e-6);
    REQUIRE_THAT(plan.planes[0].pos, WithinAbs(199., 1e-6));
    const SplitWarning *moved = p3_find_plane(plan.warnings, SplitWarning::Kind::PlaneMoved, 0);
    REQUIRE(moved != nullptr);
    REQUIRE(moved->detail == int(SplitAdjustReason::FlatFace));
}

TEST_CASE("Split to Fit P3: the smallest part thickness is never broken", "[SplitToVolume]")
{
    // The last plane (pulled in to 490 by full size first) sits in a neck from 487.5 to 492.5: leaving
    // it outwards would leave a last part under 10 mm, so it may only go inwards.
    const indexed_triangle_set bar = p3_bar_with_neck(500., 487.5, 492.5);
    SplitOptions               opt = p3_adjust_on();
    opt.layout                     = SplitLayout::FullSizeFirst;
    for (double min_t : { 10., 20. }) {
        opt.min_part_thickness = min_t;
        const SplitPlan plan   = split_plan_grid(bar, usable_box(246., 200., 200.), opt);
        INFO("smallest part " << min_t);
        REQUIRE(plan.planes.size() == 2);
        for (double s : p3_spans(plan, 0)) {
            REQUIRE(s >= min_t - 1e-6);
            REQUIRE(s <= 246. + 1e-6);
        }
        // Out of the neck.
        REQUIRE(plan.planes[1].pos <= 487.5 - 1. + 1e-6);
    }
}

TEST_CASE("Split to Fit P3: a pinned plane does not move", "[SplitToVolume]")
{
    const indexed_triangle_set bar = p3_bar_with_neck(300., 145.5, 152.5);
    SplitOptions               opt = p3_adjust_on();
    opt.pinned.push_back({ 0, 0, 150. });
    const SplitPlan plan = split_plan_grid(bar, usable_box(200., 200., 200.), opt);
    REQUIRE(plan.planes.size() == 1);
    REQUIRE(plan.planes[0].pinned);
    REQUIRE_FALSE(plan.planes[0].adjusted);
    REQUIRE_THAT(plan.planes[0].pos, WithinAbs(150., 1e-9));
    REQUIRE_FALSE(has_warning(plan.warnings, SplitWarning::Kind::PlaneMoved));
    REQUIRE_FALSE(has_warning(plan.warnings, SplitWarning::Kind::PlaneInDetail));

    // Two planes, one pinned: the other still adjusts.
    const indexed_triangle_set long_bar = p3_bar_with_neck(450., 297.5, 302.5);
    SplitOptions               two      = p3_adjust_on();
    two.pinned.push_back({ 0, 0, 140. });
    const SplitPlan both = split_plan_grid(long_bar, usable_box(200., 200., 200.), two);
    REQUIRE(both.planes.size() == 2);
    REQUIRE_THAT(both.planes[0].pos, WithinAbs(140., 1e-9));
    REQUIRE_FALSE(both.planes[0].adjusted);
    REQUIRE(both.planes[1].adjusted);
    REQUIRE((both.planes[1].pos <= 297.5 - 1. + 1e-6 || both.planes[1].pos >= 302.5 + 1. - 1e-6));
}

TEST_CASE("Split to Fit P3: adjustment off gives the plan as before; nothing to improve moves nothing", "[SplitToVolume]")
{
    const indexed_triangle_set box = box_at(600., 200., 300.);
    const SplitUsableVolume    u   = split_usable_volume(square_printer(256., 256.), SplitOptions{});
    const SplitPlan            off = split_plan_grid(box, u, SplitOptions{});
    const SplitPlan            on  = split_plan_grid(box, u, p3_adjust_on());
    REQUIRE(off.planes.size() == on.planes.size());
    for (size_t i = 0; i < off.planes.size(); ++i) {
        REQUIRE(off.planes[i].pos == off.planes[i].nominal);
        REQUIRE(on.planes[i].pos == off.planes[i].pos);
        REQUIRE_FALSE(on.planes[i].adjusted);
    }
    REQUIRE(on.parts.size() == off.parts.size());
    for (size_t i = 0; i < off.parts.size(); ++i) {
        REQUIRE((on.parts[i].bbox.min - off.parts[i].bbox.min).norm() == 0.);
        REQUIRE((on.parts[i].bbox.max - off.parts[i].bbox.max).norm() == 0.);
    }
    REQUIRE(on.faces.size() == off.faces.size());
    REQUIRE(on.warnings.size() == off.warnings.size());

    // With dowels too: the same sites.
    SplitOptions dowels_on = p2_dowels_on();
    SplitOptions both      = p2_dowels_on();
    both.adjust_planes     = true;
    const SplitPlan d_off  = split_plan_grid(box_at(100., 100., 100.), usable_box(60., 60., 60.), dowels_on);
    const SplitPlan d_on   = split_plan_grid(box_at(100., 100., 100.), usable_box(60., 60., 60.), both);
    REQUIRE(d_off.sites.size() == d_on.sites.size());
    for (size_t i = 0; i < d_off.sites.size(); ++i)
        REQUIRE((d_off.sites[i].pos - d_on.sites[i].pos).norm() < 1e-9);
}

TEST_CASE("Split to Fit P3: part volumes in the plan", "[SplitToVolume]")
{
    // 3 x 3 x 3 cells of a cube: every cell is 100 mm on a side, the middle one has no surface at all.
    const SplitPlan cube = split_plan_grid(box_at(300., 300., 300.), usable_box(110., 110., 110.), SplitOptions{});
    double          sum  = 0.;
    for (const SplitPart &p : cube.parts) {
        REQUIRE_THAT(p.volume, WithinRel(1e6, 1e-6));
        REQUIRE_THAT(p.contact_area, WithinRel(1e4, 1e-6));
        sum += p.volume;
    }
    REQUIRE_THAT(sum, WithinRel(2.7e7, 1e-6));

    // The U: five parts that add up to the model.
    const indexed_triangle_set u_mesh = u_shape();
    const SplitPlan            u      = split_plan_grid(u_mesh, usable_box(100., 100., 205.), SplitOptions{});
    double                     usum   = 0.;
    for (const SplitPart &p : u.parts)
        if (!p.empty)
            usum += p.volume;
    REQUIRE_THAT(usum, WithinRel(double(its_volume(u_mesh)), 1e-4));
    REQUIRE_FALSE(has_warning(u.warnings, SplitWarning::Kind::SmallPart));
    REQUIRE_FALSE(has_warning(u.warnings, SplitWarning::Kind::NoFlatFace));
}

TEST_CASE("Split to Fit P3: a very small part is warned about, before and after Apply", "[SplitToVolume]")
{
    // An 8 x 8 bar with a plane pinned 10 mm from its end: a 640 mm3 part.
    Model        model;
    ModelObject *mo  = add_object(model, box_at(300., 8., 8.), "Stick");
    SplitOptions opt;
    opt.pinned.push_back({ 0, 0, 10. });
    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable_box(290., 200., 200.), opt);
    REQUIRE(plan.part_count() == 2);
    const SplitWarning *w = p2_find(plan.warnings, SplitWarning::Kind::SmallPart);
    REQUIRE(w != nullptr);
    REQUIRE(plan.parts[size_t(w->part)].name == "X1");
    REQUIRE_THAT(w->value, WithinRel(640., 1e-4));
    REQUIRE(std::count_if(plan.warnings.begin(), plan.warnings.end(),
                          [](const SplitWarning &x) { return x.kind == SplitWarning::Kind::SmallPart; }) == 1);

    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.parts.size() == 2);
    const SplitWarning *after = p2_find(res.warnings, SplitWarning::Kind::SmallPart);
    REQUIRE(after != nullptr);
    REQUIRE(plan.parts[size_t(after->part)].name == "X1");
    REQUIRE_THAT(after->value, WithinRel(640., 1e-2));
}

TEST_CASE("Split to Fit P3: a part with nothing flat to stand on is warned about", "[SplitToVolume]")
{
    // A 40 x 40 bar turned 45 degrees about X, standing on an edge: 0.3 mm up it is 0.6 mm wide.
    indexed_triangle_set diamond = box_at(300., 40., 40., Vec3d(0., -20., -20.));
    const double         c = std::cos(0.25 * PI), s = std::sin(0.25 * PI);
    for (Vec3f &v : diamond.vertices) {
        const double y = v.y(), z = v.z();
        v.y()          = float(c * y - s * z);
        v.z()          = float(s * y + c * z + 20. * std::sqrt(2.));
    }
    Model           model;
    ModelObject    *mo   = add_object(model, diamond, "Edge");
    // P4 (item 1): each half could stand on its 40 x 40 cut face, so it is only warned about when that
    // pose does not fit either: here the usable height (60) is below the halves' length (150).
    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable_box(200., 200., 60.), SplitOptions{});
    REQUIRE(plan.part_count() == 2);
    size_t flagged = 0;
    for (const SplitWarning &w : plan.warnings)
        if (w.kind == SplitWarning::Kind::NoFlatFace) {
            ++flagged;
            REQUIRE_THAT(w.value, WithinAbs(0.6 * 150., 2.));
        }
    REQUIRE(flagged == 2);

    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.parts.size() == 2);
    REQUIRE(std::count_if(res.warnings.begin(), res.warnings.end(),
                          [](const SplitWarning &w) { return w.kind == SplitWarning::Kind::NoFlatFace; }) == 2);

    // With room to lay them on the cut face: no warning, before or after Apply.
    const SplitPlan roomy = split_plan_grid(split_object_mesh(*mo, 0), usable_box(200., 200., 200.), SplitOptions{});
    REQUIRE_FALSE(has_warning(roomy.warnings, SplitWarning::Kind::NoFlatFace));
    for (const SplitPiece &p : roomy.pieces) {
        REQUIRE_THAT(p.contact_area, WithinAbs(0.6 * 150., 2.));
        REQUIRE_THAT(p.best_contact_area, WithinRel(1600., 1e-3));
    }
    const SplitApplyResult roomy_res = split_apply(*mo, 0, roomy);
    REQUIRE_FALSE(has_warning(roomy_res.warnings, SplitWarning::Kind::NoFlatFace));

    // The same bar lying flat stands on a whole face: no warning.
    const SplitPlan flat = split_plan_grid(box_at(300., 40., 40.), usable_box(200., 200., 200.), SplitOptions{});
    REQUIRE_FALSE(has_warning(flat.warnings, SplitWarning::Kind::NoFlatFace));
}

TEST_CASE("Split to Fit P3: an open model is warned about before and after Apply", "[SplitToVolume]")
{
    // A bar with one triangle of its far end face missing.
    indexed_triangle_set bar = box_at(300., 60., 60.);
    const auto           end = std::find_if(bar.indices.begin(), bar.indices.end(), [&bar](const stl_triangle_vertex_indices &t) {
        return bar.vertices[t[0]].x() > 299.f && bar.vertices[t[1]].x() > 299.f && bar.vertices[t[2]].x() > 299.f;
    });
    REQUIRE(end != bar.indices.end());
    bar.indices.erase(end);
    REQUIRE(its_num_open_edges(bar) > 0);

    Model           model;
    ModelObject    *mo   = add_object(model, bar, "Open");
    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable_box(200., 200., 200.), SplitOptions{});
    REQUIRE(has_warning(plan.warnings, SplitWarning::Kind::ModelNotWatertight));
    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.parts.size() == 2);
    const SplitWarning *w = p2_find(res.warnings, SplitWarning::Kind::OpenEdges);
    REQUIRE(w != nullptr);
    REQUIRE(plan.parts[size_t(w->part)].name == "X2");
}

// =====================================================================================================
// Phase 5: loose parts and matching cut-face labels
// =====================================================================================================

namespace {

size_t p5_count(const std::vector<SplitWarning> &w, SplitWarning::Kind kind)
{
    return size_t(std::count_if(w.begin(), w.end(), [kind](const SplitWarning &x) { return x.kind == kind; }));
}

// The negative volumes of an object by name.
std::set<std::string> p5_negative_names(const ModelObject &o)
{
    std::set<std::string> out;
    for (const ModelVolume *v : o.volumes)
        if (v->is_negative_volume())
            out.insert(v->name);
    return out;
}

double p5_area(const ExPolygons &ex)
{
    double a = 0.;
    for (const ExPolygon &e : ex)
        a += e.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}

// Distance from point `p` to the box [lo, hi] (2D).
double p5_box_distance(const Vec2d &p, const Vec2d &lo, const Vec2d &hi)
{
    const double dx = std::max({ lo.x() - p.x(), 0., p.x() - hi.x() });
    const double dy = std::max({ lo.y() - p.y(), 0., p.y() - hi.y() });
    return std::hypot(dx, dy);
}

} // namespace

TEST_CASE("Split to Fit P5: dowels are planned per pair of pieces, holes in both", "[SplitToVolume]")
{
    Model        model;
    ModelObject *mo = add_object(model, u_shape(), "U");
    SplitOptions opt;
    opt.dowels.enabled   = true;
    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable_box(400., 400., 205.), opt);
    REQUIRE(plan.piece_count() == 3);
    REQUIRE(plan.mates.size() == 2);
    for (const SplitMate &m : plan.mates) {
        INFO("pair " << m.code);
        // Each pillar's patch gets its own dowels (the cap is per pair, not per face).
        REQUIRE_FALSE(m.sites.empty());
        for (int si : m.sites) {
            const SplitSite &s = plan.sites[size_t(si)];
            REQUIRE(s.mate == m.id);
            REQUIRE(s.name.rfind(m.code + "-", 0) == 0);
            REQUIRE(m.patches.front().contains(Point::new_scale(s.pos.x(), s.pos.y())));
        }
    }
    REQUIRE_FALSE(has_warning(plan.warnings, SplitWarning::Kind::NoDowelOnFace));

    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.parts.size() == 3);
    REQUIRE(res.dowels.size() == plan.sites.size());
    std::vector<const ModelObject *> of_piece(plan.pieces.size(), nullptr);
    for (size_t i = 0; i < res.parts.size(); ++i)
        of_piece[size_t(res.piece_index[i])] = res.parts[i];
    for (const SplitMate &m : plan.mates)
        for (int si : m.sites) {
            const std::string &name = plan.sites[size_t(si)].name;
            // The hole above the plane is in the pillar top of this pair, the one below in the base,
            // and the other pillar top has none of this pair's holes.
            REQUIRE(p5_negative_names(*of_piece[size_t(m.piece_above)]).count(name + "_A") == 1);
            REQUIRE(p5_negative_names(*of_piece[size_t(m.piece_below)]).count(name + "_B") == 1);
            const int other = m.piece_above == 1 ? 2 : 1;
            REQUIRE(p5_negative_names(*of_piece[size_t(other)]).count(name + "_A") == 0);
        }
    for (const ModelObject *p : res.parts)
        REQUIRE(open_edges(*p) == 0);
}

TEST_CASE("Split to Fit P5: tiny pieces are named, warned about, and discarded only when asked", "[SplitToVolume]")
{
    Model                      model;
    ModelObject               *mo   = add_object(model, u_shape(), "U");
    const indexed_triangle_set mesh = split_object_mesh(*mo, 0);
    const double               whole = world_volume(*mo);
    // A plane pinned 0.2 mm under the top: the two pillar tips are 50 x 50 x 0.2 = 500 mm3 each.
    SplitOptions opt;
    opt.pinned.push_back({ 2, 0, 299.8 });
    const SplitUsableVolume usable = usable_box(400., 400., 299.9);

    SECTION("kept (the default): named _tiny and warned about")
    {
        const SplitPlan plan = split_plan_grid(mesh, usable, opt);
        REQUIRE(plan.piece_count() == 3);
        REQUIRE(plan.discarded_count() == 0);
        std::set<std::string> names;
        for (const SplitPiece &p : plan.pieces)
            names.insert(p.name);
        REQUIRE(names == std::set<std::string>{ "Z1", "Z2_a_tiny", "Z2_b_tiny" });
        REQUIRE(p5_count(plan.warnings, SplitWarning::Kind::SmallPart) == 2);
        for (const SplitWarning &w : plan.warnings)
            if (w.kind == SplitWarning::Kind::SmallPart) {
                REQUIRE(w.piece >= 0);
                REQUIRE(plan.pieces[size_t(w.piece)].tiny);
                REQUIRE_THAT(w.value, WithinRel(500., 1e-3));
            }
        REQUIRE_FALSE(has_warning(plan.warnings, SplitWarning::Kind::DiscardedPieces));

        const SplitApplyResult res = split_apply(*mo, 0, plan);
        REQUIRE(res.parts.size() == 3);
        REQUIRE(res.discarded == 0);
        std::set<std::string> made;
        for (const ModelObject *p : res.parts)
            made.insert(p->name);
        REQUIRE(made == std::set<std::string>{ "U Z1", "U Z2_a_tiny", "U Z2_b_tiny" });
        REQUIRE(p5_count(res.warnings, SplitWarning::Kind::SmallPart) == 2);
    }
    SECTION("discarded: left out, counted, their volume reported")
    {
        SplitOptions discard  = opt;
        discard.discard_small = true;
        const SplitPlan plan  = split_plan_grid(mesh, usable, discard);
        REQUIRE(plan.pieces.size() == 3);
        REQUIRE(plan.piece_count() == 1);
        REQUIRE(plan.discarded_count() == 2);
        REQUIRE_THAT(plan.discarded_volume(), WithinRel(1000., 1e-3));
        const SplitWarning *w = p2_find(plan.warnings, SplitWarning::Kind::DiscardedPieces);
        REQUIRE(w != nullptr);
        REQUIRE_THAT(w->value, WithinAbs(2., 1e-9));
        REQUIRE_THAT(w->value2, WithinRel(1000., 1e-3));
        REQUIRE_FALSE(has_warning(plan.warnings, SplitWarning::Kind::SmallPart));
        // A pair with a discarded piece is no pair.
        REQUIRE(plan.mates.empty());

        const SplitApplyResult res = split_apply(*mo, 0, plan);
        REQUIRE(res.parts.size() == 1);
        REQUIRE(res.parts.front()->name == "U Z1");
        REQUIRE(res.discarded == 2);
        REQUIRE_THAT(res.discarded_volume, WithinRel(1000., 1e-3));
        REQUIRE(has_warning(res.warnings, SplitWarning::Kind::DiscardedPieces));
        REQUIRE_THAT(world_volume(*res.parts.front()), WithinRel(whole - 1000., 1e-4));
    }
    SECTION("one threshold: below it a piece is tiny, above it not")
    {
        SplitOptions lower       = opt;
        lower.small_piece_volume = 400.;
        lower.discard_small      = true;
        const SplitPlan plan     = split_plan_grid(mesh, usable, lower);
        REQUIRE(plan.piece_count() == 3);
        REQUIRE(plan.discarded_count() == 0);
        for (const SplitPiece &p : plan.pieces)
            REQUIRE(p.name.find("_tiny") == std::string::npos);
        REQUIRE_FALSE(has_warning(plan.warnings, SplitWarning::Kind::SmallPart));
    }
}

TEST_CASE("Split to Fit P5: overlapping bodies stay one piece", "[SplitToVolume]")
{
    // Two 100 mm bars overlapping by 50 mm, as two shells of one mesh (a model made of overlapping
    // bodies): split once across X, each section is one piece, not two.
    indexed_triangle_set two = box_at(100., 40., 40.);
    its_merge(two, box_at(100., 40., 40., Vec3d(50., 0., 0.)));
    const SplitPlan plan = split_plan_grid(two, usable_box(100., 100., 100.), SplitOptions{});
    REQUIRE(plan.part_count() == 2);
    REQUIRE(plan.piece_count() == 2);

    // Two bars side by side with a gap are two pieces in each section.
    indexed_triangle_set apart = box_at(150., 40., 40.);
    its_merge(apart, box_at(150., 40., 40., Vec3d(0., 60., 0.)));
    const SplitPlan sep = split_plan_grid(apart, usable_box(100., 120., 100.), SplitOptions{});
    REQUIRE(sep.part_count() == 2);
    REQUIRE(sep.piece_count() == 4);
}

TEST_CASE("Split to Fit P5: label shapes", "[SplitToVolume]")
{
    double           w = 0.;
    const ExPolygons s = split_label_text_shapes("X1.2", 6., 0.84, &w);
    // One shape per character: X, 1, the dot and 2.
    REQUIRE(s.size() == 4);
    const BoundingBox bb = get_extents(s);
    REQUIRE_THAT(unscale<double>(bb.max.y() - bb.min.y()), WithinAbs(6., 0.02));
    REQUIRE_THAT(unscale<double>(bb.max.x() - bb.min.x()), WithinAbs(w, 0.02));
    REQUIRE_THAT(unscale<double>(bb.min.x() + bb.max.x()), WithinAbs(0., 0.02));
    // Every letter and digit of a code has a glyph; anything else is a gap.
    for (char c : std::string("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-"))
        REQUIRE_FALSE(split_label_text_shapes(std::string(1, c), 4., 0.8).empty());
    REQUIRE(split_label_text_shapes("?", 4., 0.8).empty());

    // The dots: the marker and the number's bits, 5 strokes across.
    double           size = 0.;
    const ExPolygons five = split_label_pattern_shapes(5, 0.8, &size);
    REQUIRE_THAT(size, WithinAbs(4., 1e-9));
    REQUIRE(five.size() == 3);
    REQUIRE_THAT(p5_area(five), WithinRel(3. * 0.64, 1e-6));
    REQUIRE(split_label_pattern_shapes(255, 0.8).size() == 9);
}

TEST_CASE("Split to Fit P5: matching labels on both faces of every pair", "[SplitToVolume]")
{
    Model        model;
    ModelObject *mo = add_object(model, box_at(100., 100., 100.), "Block");
    SplitOptions opt;
    opt.dowels.enabled   = true;
    opt.labels.enabled   = true;
    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable_box(60., 60., 200.), opt);
    REQUIRE(plan.piece_count() == 4);
    REQUIRE(plan.mates.size() == plan.faces.size());
    REQUIRE(plan.labels.size() == 2 * plan.mates.size());
    REQUIRE_FALSE(has_warning(plan.warnings, SplitWarning::Kind::NoLabel));

    const SplitLabelOptions &lo = plan.label_options;
    for (const SplitMate &m : plan.mates) {
        INFO("pair " << m.code);
        REQUIRE(m.labels.size() == 2);
        const SplitLabel &b = plan.labels[size_t(m.labels[0])];
        const SplitLabel &t = plan.labels[size_t(m.labels[1])];
        // The same text on both, one on each piece.
        REQUIRE(b.text == m.code);
        REQUIRE(t.text == b.text);
        REQUIRE(b.below);
        REQUIRE_FALSE(t.below);
        REQUIRE(b.piece == m.piece_below);
        REQUIRE(t.piece == m.piece_above);
        REQUIRE((b.center - t.center).norm() < 1e-9);
        // Each reads right from outside its own piece: opposite normals, the same up, opposite right.
        REQUIRE((b.normal + t.normal).norm() < 1e-9);
        REQUIRE((b.up - t.up).norm() < 1e-9);
        REQUIRE((b.right + t.right).norm() < 1e-9);
        REQUIRE((b.right - b.up.cross(b.normal)).norm() < 1e-9);
        REQUIRE(b.height <= lo.height + 1e-9);
        REQUIRE(b.stroke >= lo.min_stroke - 1e-9);

        // Inside the patch with the margin, and clear of every dowel hole on the face.
        const SplitFace  &face = plan.faces[size_t(m.face)];
        const int         a = plan.planes[size_t(face.plane)].axis, ua = (a + 1) % 3, va = (a + 2) % 3;
        const ExPolygons  inner = offset_ex(m.patches.front(), -float(scale_(lo.margin - 0.01)));
        Vec2d             blo(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
        Vec2d             bhi(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest());
        for (int sx : { -1, 1 })
            for (int sy : { -1, 1 }) {
                const Vec3d c = b.center + 0.5 * double(sx) * b.width * b.right + 0.5 * double(sy) * b.height * b.up;
                const Vec2d q(c[ua], c[va]);
                blo = blo.cwiseMin(q);
                bhi = bhi.cwiseMax(q);
                bool inside = false;
                for (const ExPolygon &ex : inner)
                    inside = inside || ex.contains(Point::new_scale(q.x(), q.y()));
                REQUIRE(inside);
            }
        REQUIRE_FALSE(face.sites.empty());
        for (int si : face.sites) {
            const SplitSite &s = plan.sites[size_t(si)];
            REQUIRE(p5_box_distance(Vec2d(s.pos[ua], s.pos[va]), blo, bhi) >= s.footprint_radius + lo.dowel_clearance - 1e-3);
        }

        // The recesses: closed, as big as the shapes, `depth` into their own piece and 0.2 mm out.
        const indexed_triangle_set mb = split_label_mesh(b);
        const indexed_triangle_set mt = split_label_mesh(t);
        REQUIRE(its_num_open_edges(mb) == 0);
        REQUIRE(its_num_open_edges(mt) == 0);
        const ExPolygons shapes = b.pattern ? split_label_pattern_shapes(b.number, b.stroke) : split_label_text_shapes(b.text, b.height, b.stroke);
        REQUIRE_THAT(double(its_volume(mb)), WithinRel(p5_area(shapes) * (b.depth + 0.2), 1e-3));
        REQUIRE_THAT(double(its_volume(mt)), WithinRel(p5_area(shapes) * (t.depth + 0.2), 1e-3));
        const double  pos = plan.planes[size_t(face.plane)].pos;
        BoundingBoxf3 bbb, tbb;
        for (const Vec3f &v : mb.vertices)
            bbb.merge(v.cast<double>());
        for (const Vec3f &v : mt.vertices)
            tbb.merge(v.cast<double>());
        REQUIRE_THAT(bbb.min[a], WithinAbs(pos - b.depth, 1e-4));   // into the piece below
        REQUIRE_THAT(bbb.max[a], WithinAbs(pos + 0.2, 1e-4));
        REQUIRE_THAT(tbb.max[a], WithinAbs(pos + t.depth, 1e-4));   // into the piece above
        REQUIRE_THAT(tbb.min[a], WithinAbs(pos - 0.2, 1e-4));
        // Mirror images: the label above is the one below turned half a turn about `up` (a reflection in
        // the plane and one across the text), so each reads right from its own side.
        const Transform3d half_turn = Geometry::translation_transform(b.center) * Geometry::rotation_transform(PI * b.up) *
                                      Geometry::translation_transform(-b.center);
        for (size_t i = 0; i < mb.vertices.size(); i += 7) {
            const Vec3d p = half_turn * mb.vertices[i].cast<double>();
            double      d = std::numeric_limits<double>::max();
            for (const Vec3f &q : mt.vertices)
                d = std::min(d, (q.cast<double>() - p).norm());
            REQUIRE(d < 1e-3);
        }
    }

    // Apply: every label is a closed negative volume on its piece; the parts stay watertight.
    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.parts.size() == 4);
    REQUIRE(res.labels == plan.labels.size());
    for (size_t i = 0; i < res.parts.size(); ++i) {
        const ModelObject *part = res.parts[i];
        REQUIRE(open_edges(*part) == 0);
        const SplitPiece &piece = plan.pieces[size_t(res.piece_index[i])];
        size_t            labels = 0;
        for (const ModelVolume *v : part->volumes)
            if (v->is_negative_volume() && v->name.rfind("Label ", 0) == 0) {
                ++labels;
                REQUIRE(its_num_open_edges(v->mesh().its) == 0);
            }
        REQUIRE(labels == piece.labels.size());
        // Its faces' codes are what its labels say.
        for (int li : piece.labels)
            REQUIRE(p5_negative_names(*part).count("Label " + plan.labels[size_t(li)].text) == 1);
    }
}

TEST_CASE("Split to Fit P5: a small face gets the dots, a tiny one no label", "[SplitToVolume]")
{
    SplitOptions opt;
    opt.labels.enabled = true;
    // An 8 x 8 bar: no room for text, room for the 4 mm dot pattern.
    const SplitPlan bar = split_plan_grid(box_at(300., 8., 8.), usable_box(200., 200., 200.), opt);
    REQUIRE(bar.mates.size() == 1);
    REQUIRE(bar.labels.size() == 2);
    REQUIRE(bar.labels[0].pattern);
    REQUIRE(bar.labels[0].number == 1);
    REQUIRE(has_warning(bar.warnings, SplitWarning::Kind::LabelPattern));
    REQUIRE(its_num_open_edges(split_label_mesh(bar.labels[0])) == 0);

    // A 4 x 4 bar: nothing fits; it is said, not skipped silently.
    const SplitPlan thin = split_plan_grid(box_at(300., 4., 4.), usable_box(200., 200., 200.), opt);
    REQUIRE(thin.mates.size() == 1);
    REQUIRE(thin.labels.empty());
    const SplitWarning *w = p2_find(thin.warnings, SplitWarning::Kind::NoLabel);
    REQUIRE(w != nullptr);
    REQUIRE(w->mate == 0);
}

TEST_CASE("Split to Fit P5: time of a plan on a big mesh", "[SplitToVolume][slow]")
{
    // About a million triangles; the piece search runs once per plan (the gizmo plans again when a
    // plane is let go, not on every mouse move). Reported, not checked: the build machine is shared.
    const indexed_triangle_set sphere = its_make_sphere(150., 2. * PI / 1000.);
    using clock                       = std::chrono::steady_clock;
    auto t0                           = clock::now();
    const std::vector<Vec3i32> nb     = its_face_neighbors_par(sphere);
    auto t1                           = clock::now();
    const SplitPlan plain             = split_plan_grid(sphere, usable_box(200., 200., 200.), SplitOptions{}, nullptr, &nb);
    auto t2                           = clock::now();
    SplitOptions all;
    all.dowels.enabled   = true;
    all.labels.enabled   = true;
    const SplitPlan full = split_plan_grid(sphere, usable_box(200., 200., 200.), all, nullptr, &nb);
    auto t3              = clock::now();
    auto ms              = [](clock::duration d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
    WARN("Split to Fit P5 timing, " << sphere.indices.size() << " triangles: face neighbours " << ms(t1 - t0) << " ms, plan "
                                    << ms(t2 - t1) << " ms, plan with dowels and labels " << ms(t3 - t2) << " ms");
    REQUIRE(plain.piece_count() == 8);
    REQUIRE(full.piece_count() == 8);
    REQUIRE(full.labels.size() == 2 * full.mates.size());
}

// ---- P4: polish ---------------------------------------------------------------------------------------

namespace {

// A square pyramid standing on its apex: base 40 x 40 at z = `h`, apex at the origin.
indexed_triangle_set p4_pyramid_apex_down(float h = 40.f)
{
    indexed_triangle_set its;
    its.vertices = { Vec3f(0.f, 0.f, 0.f), Vec3f(-20.f, -20.f, h), Vec3f(20.f, -20.f, h), Vec3f(20.f, 20.f, h),
                     Vec3f(-20.f, 20.f, h) };
    its.indices.emplace_back(1, 2, 3);   // base, facing up
    its.indices.emplace_back(1, 3, 4);
    its.indices.emplace_back(0, 2, 1);   // sides, facing out and down
    its.indices.emplace_back(0, 3, 2);
    its.indices.emplace_back(0, 4, 3);
    its.indices.emplace_back(0, 1, 4);
    return its;
}

BoundingBoxf3 p4_turned_bbox(const indexed_triangle_set &its, const Transform3d &t)
{
    BoundingBoxf3 bb;
    for (const Vec3f &v : its.vertices)
        bb.merge(t * v.cast<double>());
    return bb;
}

// The P3 bar turned 45 degrees about X, standing on an edge.
indexed_triangle_set p4_diamond_bar()
{
    indexed_triangle_set diamond = box_at(300., 40., 40., Vec3d(0., -20., -20.));
    const double         c = std::cos(0.25 * PI), s = std::sin(0.25 * PI);
    for (Vec3f &v : diamond.vertices) {
        const double y = v.y(), z = v.z();
        v.y()          = float(c * y - s * z);
        v.z()          = float(s * y + c * z + 20. * std::sqrt(2.));
    }
    return diamond;
}

// The world box of an object's model parts (instance applied).
BoundingBoxf3 p4_world_bbox(const ModelObject &o, size_t instance = 0)
{
    BoundingBoxf3 bb;
    for (const ModelVolume *v : o.volumes)
        if (v->is_model_part())
            bb.merge(v->mesh().transformed_bounding_box(o.instances[instance]->get_matrix() * v->get_matrix()));
    return bb;
}

// Where every dowel stands (world box centres of every instance), and where it is assembled, sorted.
void p4_dowel_places(const std::vector<ModelObject *> &dowels, std::vector<Vec3d> &placed, std::vector<Vec3d> &assembled)
{
    for (const ModelObject *d : dowels)
        for (const ModelInstance *inst : d->instances) {
            const ModelVolume *v = d->volumes.front();
            placed.push_back(v->mesh().transformed_bounding_box(inst->get_matrix() * v->get_matrix()).center());
            assembled.push_back(v->mesh().transformed_bounding_box(inst->get_assemble_transformation().get_matrix() * v->get_matrix()).center());
        }
    auto less = [](const Vec3d &l, const Vec3d &r) {
        for (int a = 0; a < 3; ++a)
            if (std::abs(l[a] - r[a]) > 1e-3)
                return l[a] < r[a];
        return false;
    };
    std::sort(placed.begin(), placed.end(), less);
    std::sort(assembled.begin(), assembled.end(), less);
}

} // namespace

TEST_CASE("Split to Fit P4: overhang area", "[SplitToVolume]")
{
    double contact = -1.;
    REQUIRE_THAT(split_overhang_area(box_at(20., 20., 20.), Transform3d::Identity(), 45., &contact), WithinAbs(0., 1e-9));
    REQUIRE_THAT(contact, WithinRel(400., 1e-9));

    // A tall pyramid on its apex: its sides lean 26.6 degrees from vertical, inside the 45 degree limit.
    REQUIRE_THAT(split_overhang_area(p4_pyramid_apex_down(40.f), Transform3d::Identity(), 45.), WithinAbs(0., 1e-9));
    // A flat one (10 tall): all four sides overhang (they lean 63.4 degrees from vertical).
    const indexed_triangle_set pyramid = p4_pyramid_apex_down(10.f);
    const double               sides   = 4. * 0.5 * 40. * std::sqrt(10. * 10. + 20. * 20.);
    REQUIRE_THAT(split_overhang_area(pyramid, Transform3d::Identity(), 45., &contact), WithinRel(sides, 1e-6));
    REQUIRE_THAT(contact, WithinAbs(0., 1e-9));
    // Turned over onto its base: nothing overhangs, the base is on the bed.
    const Transform3d over(Eigen::AngleAxisd(PI, Vec3d::UnitX()));
    REQUIRE_THAT(split_overhang_area(pyramid, over, 45., &contact), WithinAbs(0., 1e-6));
    REQUIRE_THAT(contact, WithinRel(1600., 1e-6));
    // A 70 degree limit lets those sides through.
    REQUIRE_THAT(split_overhang_area(pyramid, Transform3d::Identity(), 70.), WithinAbs(0., 1e-9));
}

TEST_CASE("Split to Fit P4: the cut face to lay a part on", "[SplitToVolume]")
{
    // The pyramid on its apex lies on its base (the cut face): no overhang, the base on the bed.
    const indexed_triangle_set pyramid = p4_pyramid_apex_down();
    const SplitLayFlat         p       = split_choose_lay_flat(pyramid, { Vec3d::UnitZ() }, Vec3d::Zero());
    REQUIRE(p.changed);
    REQUIRE((p.normal - Vec3d::UnitZ()).norm() < 1e-9);
    REQUIRE_THAT(p.overhang, WithinAbs(0., 1e-6));
    REQUIRE_THAT(p.contact, WithinRel(1600., 1e-6));
    REQUIRE((p.rotation.linear() * Vec3d::UnitZ() + Vec3d::UnitZ()).norm() < 1e-9);

    // A cube standing on a face already: a tie everywhere, so it is left as it is.
    REQUIRE_FALSE(split_choose_lay_flat(box_at(20., 20., 20.), { Vec3d::UnitX(), -Vec3d::UnitZ() }, Vec3d::Zero()).changed);

    // A 10 x 20 x 30 block: no overhang either way, so the larger face wins (20 x 30 = 600 mm2).
    const indexed_triangle_set block = box_at(10., 20., 30.);
    const SplitLayFlat         b     = split_choose_lay_flat(block, { Vec3d::UnitX() }, Vec3d::Zero());
    REQUIRE(b.changed);
    REQUIRE((b.normal - Vec3d::UnitX()).norm() < 1e-9);
    REQUIRE_THAT(b.contact, WithinRel(600., 1e-6));

    // The pose must fit: on +X it is 30 x 20 (or 20 x 30), too wide for 32 x 15; on +Y it is 10 x 30,
    // which fits with a quarter turn.
    const Vec3d        usable(32., 15., 40.);
    const SplitLayFlat f = split_choose_lay_flat(block, { Vec3d::UnitX(), Vec3d::UnitY() }, usable);
    REQUIRE(f.changed);
    REQUIRE(f.fits);
    REQUIRE((f.normal - Vec3d::UnitY()).norm() < 1e-9);
    REQUIRE_THAT(f.contact, WithinRel(300., 1e-6));
    const Vec3d s = p4_turned_bbox(block, f.rotation).size();
    REQUIRE(s.x() <= usable.x() + 1e-6);
    REQUIRE(s.y() <= usable.y() + 1e-6);
    REQUIRE(s.z() <= usable.z() + 1e-6);

    // Nothing fits: kept as it is.
    const SplitLayFlat none = split_choose_lay_flat(block, { Vec3d::UnitX() }, Vec3d(5., 5., 5.));
    REQUIRE_FALSE(none.changed);
    REQUIRE_FALSE(none.fits);
}

TEST_CASE("Split to Fit P4: Apply lays each part on its cut face", "[SplitToVolume]")
{
    Model        model;
    ModelObject *mo = add_object(model, p4_diamond_bar(), "Edge");
    SplitOptions opt;
    opt.lay_flat       = true;
    opt.labels.enabled = true;
    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable_box(200., 200., 200.), opt);
    REQUIRE(plan.part_count() == 2);
    REQUIRE(plan.lay_flat);
    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.parts.size() == 2);
    REQUIRE(res.part_laid_flat == std::vector<bool>{ true, true });
    for (size_t i = 0; i < res.parts.size(); ++i) {
        const ModelObject *part = res.parts[i];
        INFO("part " << part->name);
        // Standing on the 40 x 40 diamond of the cut: 150 tall, the diamond's 56.6 across.
        const Vec3d s = p4_world_bbox(*part).size();
        REQUIRE_THAT(s.z(), WithinAbs(150., 0.2));
        REQUIRE_THAT(s.x(), WithinAbs(40. * std::sqrt(2.), 0.2));
        REQUIRE_THAT(s.y(), WithinAbs(40. * std::sqrt(2.), 0.2));
        REQUIRE_THAT(world_volume(*part), WithinRel(plan.pieces[size_t(res.piece_index[i])].volume, 1e-3));
        REQUIRE(open_edges(*part) == 0);
    }
    REQUIRE_FALSE(has_warning(res.warnings, SplitWarning::Kind::NoFlatFace));
    // Each half's label is on the face it now stands on.
    REQUIRE(res.labels == 2);
    REQUIRE(res.labels_on_bed == 2);

    // Off: the parts keep their pose.
    SplitOptions          keep = opt;
    keep.lay_flat              = false;
    const SplitApplyResult as_is = split_apply(*mo, 0, split_plan_grid(split_object_mesh(*mo, 0), usable_box(200., 200., 200.), keep));
    REQUIRE(as_is.part_laid_flat == std::vector<bool>{ false, false });
    for (const ModelObject *part : as_is.parts)
        REQUIRE_THAT(p4_world_bbox(*part).size().x(), WithinAbs(150., 0.2));
    REQUIRE(as_is.labels_on_bed == 0);

    // A pose on the cut face that does not fit (60 tall) is not taken.
    const SplitApplyResult low = split_apply(*mo, 0, split_plan_grid(split_object_mesh(*mo, 0), usable_box(200., 200., 60.), opt));
    REQUIRE(low.part_laid_flat == std::vector<bool>{ false, false });
    REQUIRE(p5_count(low.warnings, SplitWarning::Kind::NoFlatFace) == 2);
}

TEST_CASE("Split to Fit P4: identical dowels become instances of one object", "[SplitToVolume]")
{
    Model        model;
    ModelObject *mo = add_object(model, box_at(100., 100., 40.), "Block");
    SplitOptions opt;
    opt.dowels.enabled           = true;
    const SplitPlan        plain = split_plan_grid(split_object_mesh(*mo, 0), usable_box(60., 60., 60.), opt);
    const SplitApplyResult sep   = split_apply(*mo, 0, plain);
    REQUIRE(plain.sites.size() >= 4);
    REQUIRE(sep.dowels.size() == plain.sites.size());
    REQUIRE(sep.dowel_count == plain.sites.size());

    opt.instance_dowels          = true;
    const SplitPlan        plan  = split_plan_grid(split_object_mesh(*mo, 0), usable_box(60., 60., 60.), opt);
    const SplitApplyResult res   = split_apply(*mo, 0, plan);
    REQUIRE(plan.sites.size() == plain.sites.size());
    REQUIRE(res.dowel_count == plan.sites.size());
    // Every dowel here is the same 6 mm x 20 mm round dowel: one object.
    REQUIRE(res.dowels.size() == 1);
    REQUIRE(res.dowels.front()->instances.size() == plan.sites.size());
    REQUIRE(res.dowels.front()->name.find("-Dowel ") != std::string::npos);
    // The cut group counts objects: the parts and the one dowel object.
    for (const ModelObject *o : res.parts)
        REQUIRE(o->cut_id.check_sum() == res.parts.size() + res.dowels.size());
    REQUIRE(res.dowels.front()->cut_id.check_sum() == res.parts.size() + res.dowels.size());
    REQUIRE(res.dowels.front()->cut_id.connectors_cnt() == sep.dowels.front()->cut_id.connectors_cnt());

    // Every dowel stands, and is assembled, exactly where it did as a separate object.
    std::vector<Vec3d> placed_a, asm_a, placed_b, asm_b;
    p4_dowel_places(sep.dowels, placed_a, asm_a);
    p4_dowel_places(res.dowels, placed_b, asm_b);
    REQUIRE(placed_a.size() == placed_b.size());
    for (size_t i = 0; i < placed_a.size(); ++i) {
        REQUIRE((placed_a[i] - placed_b[i]).norm() < 1e-4);
        REQUIRE((asm_a[i] - asm_b[i]).norm() < 1e-4);
    }
}

TEST_CASE("Split to Fit P4: dowel sites added and removed by hand", "[SplitToVolume]")
{
    // A 100 x 40 x 40 bar cut once across X at 50: one 40 x 40 face, one automatic dowel in its middle.
    SplitOptions opt;
    opt.dowels.enabled      = true;
    opt.dowels.max_per_face = 1;
    const indexed_triangle_set bar  = box_at(100., 40., 40.);
    const SplitUsableVolume    vol  = usable_box(60., 60., 60.);
    const SplitPlan            auto_plan = split_plan_grid(bar, vol, opt);
    REQUIRE(auto_plan.planes.size() == 1);
    REQUIRE(auto_plan.sites.size() == 1);
    const double plane_pos = auto_plan.planes.front().pos;
    const Vec3d  middle    = auto_plan.sites.front().pos;
    REQUIRE_THAT(middle.y(), WithinAbs(20., 1.));
    REQUIRE_THAT(middle.z(), WithinAbs(20., 1.));
    auto edit = [&](const Vec3d &pos, bool add, double at = std::numeric_limits<double>::quiet_NaN()) {
        SplitManualSite m;
        m.axis      = 0;
        m.index     = 0;
        m.plane_pos = std::isnan(at) ? plane_pos : at;
        m.pos       = pos;
        m.add       = add;
        return m;
    };
    auto refusal = [](const SplitPlan &plan) {
        for (const SplitWarning &w : plan.warnings)
            if (w.kind == SplitWarning::Kind::ManualSiteRefused)
                return int(w.value);
        return -1;
    };

    // Removed: no dowel, and no "face too small" warning for it.
    SplitOptions removed = opt;
    removed.manual_sites = { edit(middle, false) };
    const SplitPlan r    = split_plan_grid(bar, vol, removed);
    REQUIRE(r.sites.empty());
    REQUIRE_FALSE(has_warning(r.warnings, SplitWarning::Kind::NoDowelOnFace));

    // Removed and one added elsewhere: the added one is kept, marked as the user's.
    SplitOptions moved = opt;
    moved.manual_sites = { edit(middle, false), edit(Vec3d(plane_pos, 12., 12.), true) };
    const SplitPlan m  = split_plan_grid(bar, vol, moved);
    REQUIRE(m.sites.size() == 1);
    REQUIRE(m.sites.front().manual);
    REQUIRE_THAT(m.sites.front().pos.y(), WithinAbs(12., 1e-6));
    REQUIRE_THAT(m.sites.front().pos.z(), WithinAbs(12., 1e-6));
    REQUIRE(refusal(m) == -1);

    // An added one beyond the cap is allowed when it keeps the spacing.
    SplitOptions extra        = opt;
    extra.dowels.min_spacing  = 10.;
    extra.manual_sites        = { edit(Vec3d(plane_pos, 32., 32.), true) };
    const SplitPlan e         = split_plan_grid(bar, vol, extra);
    REQUIRE(e.sites.size() == 2);
    REQUIRE(std::count_if(e.sites.begin(), e.sites.end(), [](const SplitSite &s) { return s.manual; }) == 1);

    // Refused, with the reason.
    SplitOptions near_dowel = opt;
    near_dowel.manual_sites = { edit(Vec3d(plane_pos, middle.y(), middle.z() - 8.), true) };
    const SplitPlan nd      = split_plan_grid(bar, vol, near_dowel);
    REQUIRE(nd.sites.size() == 1);
    REQUIRE(refusal(nd) == int(SplitManualRefusal::NearDowel));
    const SplitWarning *w = p2_find(nd.warnings, SplitWarning::Kind::ManualSiteRefused);
    REQUIRE(w != nullptr);
    REQUIRE(w->detail == 0);
    REQUIRE(w->plane == 0);

    SplitOptions near_edge = opt;
    near_edge.manual_sites = { edit(Vec3d(plane_pos, 3., 20.), true) };
    REQUIRE(refusal(split_plan_grid(bar, vol, near_edge)) == int(SplitManualRefusal::NearEdge));

    SplitOptions off_face = opt;
    off_face.manual_sites = { edit(Vec3d(plane_pos, 60., 20.), true) };
    REQUIRE(refusal(split_plan_grid(bar, vol, off_face)) == int(SplitManualRefusal::NotOnFace));

    // Edits made for a plane that has moved since are ignored.
    SplitOptions stale = opt;
    stale.manual_sites = { edit(middle, false, plane_pos + 1.), edit(Vec3d(plane_pos + 1., 12., 12.), true, plane_pos + 1.) };
    const SplitPlan st = split_plan_grid(bar, vol, stale);
    REQUIRE(st.sites.size() == 1);
    REQUIRE((st.sites.front().pos - middle).norm() < 1e-9);
    REQUIRE(refusal(st) == -1);

    // Close to another plane (the corner keep-out) on a 2 x 2 grid.
    const SplitPlan grid = split_plan_grid(box_at(100., 100., 40.), vol, opt);
    REQUIRE(grid.planes.size() == 2);
    SplitOptions corner = opt;
    SplitManualSite c   = edit(Vec3d(grid.planes[0].pos, 48., 20.), true, grid.planes[0].pos);
    corner.manual_sites = { c };
    REQUIRE(refusal(split_plan_grid(box_at(100., 100., 40.), vol, corner)) == int(SplitManualRefusal::NearOtherPlane));

    // Apply bakes the user's dowel like any other: a hole on each side, one dowel.
    Model        model;
    ModelObject *mo           = add_object(model, bar, "Bar");
    const SplitApplyResult res = split_apply(*mo, 0, m);
    REQUIRE(res.parts.size() == 2);
    REQUIRE(res.dowels.size() == 1);
    for (const ModelObject *part : res.parts)
        REQUIRE(p5_negative_names(*part).size() == 1);
}

TEST_CASE("Split to Fit P4: a click picks a dowel disc or a cut face", "[SplitToVolume]")
{
    SplitOptions opt;
    opt.dowels.enabled      = true;
    opt.dowels.max_per_face = 1;
    const SplitPlan plan = split_plan_grid(box_at(100., 40., 40.), usable_box(60., 60., 60.), opt);
    REQUIRE(plan.sites.size() == 1);
    const Vec3d s = plan.sites.front().pos;

    const SplitPickTarget site = split_pick(plan, Vec3d(-100., s.y(), s.z()), Vec3d::UnitX());
    REQUIRE(site.kind == SplitPickTarget::Kind::Site);
    REQUIRE(site.site == 0);
    REQUIRE(site.plane == 0);

    const SplitPickTarget face = split_pick(plan, Vec3d(-100., 5., 5.), Vec3d(1., 0., 0.));
    REQUIRE(face.kind == SplitPickTarget::Kind::Face);
    REQUIRE(face.plane == 0);
    REQUIRE(face.mate == 0);
    REQUIRE(face.pos.x() == plan.planes.front().pos);
    REQUIRE_THAT(face.pos.y(), WithinAbs(5., 1e-9));

    // Beside the model, or looking away from the plane: nothing.
    REQUIRE(split_pick(plan, Vec3d(-100., 60., 20.), Vec3d::UnitX()).kind == SplitPickTarget::Kind::None);
    REQUIRE(split_pick(plan, Vec3d(-100., 5., 5.), -Vec3d::UnitX()).kind == SplitPickTarget::Kind::None);
}

TEST_CASE("Split to Fit P4: loose pieces are marked for plates of their own", "[SplitToVolume]")
{
    Model           model;
    ModelObject    *mo   = add_object(model, u_shape(), "U");
    SplitOptions    opt;
    opt.loose_own_plates = true;
    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable_box(400., 400., 205.), opt);
    REQUIRE(plan.loose_own_plates);
    REQUIRE(plan.pieces.size() == 3);
    REQUIRE_FALSE(plan.is_loose_piece(0));
    REQUIRE_FALSE(plan.is_loose_piece(1));
    REQUIRE(plan.is_loose_piece(2));
    REQUIRE_FALSE(plan.is_loose_piece(-1));
    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.part_loose == std::vector<bool>{ false, false, true });
}

TEST_CASE("Split to Fit P4: grouped arrange steps put each group on plates of its own", "[SplitToVolume]")
{
    // The owner's case (2026-11-16): 17 parts already on plates 0-12, and 81 dowels parked off the
    // plates but still registered to plate 0 (where loading put them). The dowel step must take every
    // dowel off its plate before plate 0 is locked: a dowel registered to a locked plate is never moved.
    std::vector<SplitArrangeInstance> in;
    for (int i = 0; i < 17; ++i)
        in.push_back({ 0, std::min(i, 12), true });
    for (int i = 0; i < 81; ++i)
        in.push_back({ 2, 0, true });
    in.push_back({ -1, 3, true });   // something of the user's on plate 3
    std::vector<bool> locked(13, false);
    locked[5] = true;                // a plate the user locked

    // Step 0 (main parts): every part of the chain leaves its plate, the dowels wait (not printable),
    // nothing is locked.
    const SplitArrangeStep s0 = split_arrange_step(in, 0, locked);
    REQUIRE(s0.unregister.size() == 17 + 81);
    REQUIRE(s0.hold.size() == 81);
    for (size_t k : s0.hold)
        REQUIRE(in[k].group == 2);
    REQUIRE(s0.lock_plates.empty());

    // Step 2 (dowels): only the dowels leave their plates, none is held, and every plate still holding
    // something (the parts' plates and the user's plate 3) is locked, but not the user's own lock again.
    const SplitArrangeStep s2 = split_arrange_step(in, 2, locked);
    REQUIRE(s2.unregister.size() == 81);
    REQUIRE(s2.hold.empty());
    std::vector<int> expect;
    for (int p = 0; p < 13; ++p)
        if (p != 5)
            expect.push_back(p);
    REQUIRE(s2.lock_plates == expect);
    // After the step is applied, no dowel is registered to a plate, so none can be held back by a lock.
    std::vector<SplitArrangeInstance> after = in;
    for (size_t k : s2.unregister)
        after[k].plate = -1;
    for (const SplitArrangeInstance &a : after)
        if (a.group == 2)
            REQUIRE(a.plate < 0);

    // With loose parts on plates of their own: step 1 moves only the loose parts, holds the dowels and
    // locks the parts' plates; the dowels' step then locks the loose parts' plates too.
    std::vector<SplitArrangeInstance> lp = { { 0, 0, true }, { 0, 1, true }, { 1, 0, true }, { 1, 0, true }, { 2, 0, true } };
    const std::vector<bool>           l2(3, false);
    const SplitArrangeStep            s1 = split_arrange_step(lp, 1, l2);
    REQUIRE(s1.unregister == std::vector<size_t>{ 2, 3, 4 });
    REQUIRE(s1.hold == std::vector<size_t>{ 4 });
    REQUIRE(s1.lock_plates == std::vector<int>{ 0, 1 });
    lp[2].plate = 2;
    lp[3].plate = 2;
    const SplitArrangeStep d = split_arrange_step(lp, 2, l2);
    REQUIRE(d.unregister == std::vector<size_t>{ 4 });
    REQUIRE(d.lock_plates == std::vector<int>{ 0, 1, 2 });
}

TEST_CASE("Split to Fit P4: the grouped arrange report names unplaced items and shared plates", "[SplitToVolume]")
{
    const std::vector<SplitArrangeInstance> in = { { 0, 0, true }, { 0, 1, true }, { 2, 2, true }, { 2, 2, true }, { 2, -1, true }, { -1, 0, true } };
    const SplitArrangeReport ok = split_arrange_report(in, { 0, 1, 2, 2, 2, 0 }, 3);
    REQUIRE(ok.plates[0] == std::vector<int>{ 0, 1 });
    REQUIRE(ok.plates[1].empty());
    REQUIRE(ok.plates[2] == std::vector<int>{ 2 });
    REQUIRE(ok.unplaced[2].empty());
    REQUIRE(ok.shares == std::vector<bool>{ false, false, false });

    // A dowel left beside the plates is reported; one put on a parts plate makes the group "shared".
    const SplitArrangeReport bad = split_arrange_report(in, { 0, 1, 2, 1, -1, 0 }, 3);
    REQUIRE(bad.unplaced[2] == std::vector<size_t>{ 4 });
    REQUIRE(bad.plates[2] == std::vector<int>{ 1, 2 });
    REQUIRE(bad.shares[2]);
}

// Keep paint v2: Split to Fit cuts with KeepPaint (each plane cut paints by exact facet provenance)
// and then separates a section's loose pieces by facet lists, whose painting is copied facet by
// facet. Every paint kind comes through with its area, on the U whose top section falls apart.
TEST_CASE("Split to Fit: painting of every kind survives the cuts and the loose pieces", "[SplitToVolume][PaintTransfer]")
{
    using FacetsMember = FacetsAnnotation ModelVolume::*;
    struct Kind
    {
        const char         *name;
        FacetsMember        member;
        EnforcerBlockerType state;
    };
    const Kind kinds[] = {
        { "colour 2", &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder2 },
        { "colour 3", &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3 },
        { "support", &ModelVolume::supported_facets, EnforcerBlockerType::ENFORCER },
        { "seam", &ModelVolume::seam_facets, EnforcerBlockerType::ENFORCER },
        { "fuzzy skin", &ModelVolume::fuzzy_skin_facets, EnforcerBlockerType::FUZZY_SKIN },
    };
    auto area = [](const indexed_triangle_set &its) {
        double a = 0.;
        for (const Vec3i32 &f : its.indices)
            a += 0.5 * double((its.vertices[f[1]] - its.vertices[f[0]]).cross(its.vertices[f[2]] - its.vertices[f[0]]).norm());
        return a;
    };
    auto paint = [](ModelVolume &v, FacetsMember member, const std::function<EnforcerBlockerType(const Vec3f &)> &at) {
        TriangleSelector sel(v.mesh());
        sel.resample([&](const Vec3f &p) {
            TriangleSelector::SampledState s;
            s.state = at(p);
            return s;
        }, 0.5f);
        (v.*member).set(sel);
    };

    Model        model;
    ModelObject *mo = add_object(model, u_shape(), "U");
    ModelVolume *v  = mo->volumes.front();
    // The mesh is centred on its volume; the U's own frame is mesh + init shift.
    const Vec3f shift = v->mesh().get_init_shift().cast<float>();
    paint(*v, &ModelVolume::mmu_segmentation_facets, [&](const Vec3f &m) {
        const Vec3f p = m + shift;
        return p.x() < 23.f ? EnforcerBlockerType::Extruder2 : p.z() > 210.f ? EnforcerBlockerType::Extruder3 : EnforcerBlockerType::NONE;
    });
    paint(*v, &ModelVolume::supported_facets, [&](const Vec3f &m) {
        const Vec3f p = m + shift;
        return p.y() < 0.01f && p.x() > 140.f ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::NONE;
    });
    paint(*v, &ModelVolume::seam_facets, [&](const Vec3f &m) {
        const Vec3f p = m + shift;
        return p.x() > 277.f ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::NONE;
    });
    paint(*v, &ModelVolume::fuzzy_skin_facets, [&](const Vec3f &m) {
        const Vec3f p = m + shift;
        return std::abs(p.z() - 140.f) < 25.f ? EnforcerBlockerType::FUZZY_SKIN : EnforcerBlockerType::NONE;
    });

    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable_box(400., 400., 205.), SplitOptions{});
    REQUIRE(plan.piece_count() == 3);
    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.parts.size() == 3);

    for (const Kind &k : kinds) {
        const double in  = area((v->*k.member).get_facets(*v, k.state));
        double       out = 0.;
        for (const ModelObject *part : res.parts)
            for (const ModelVolume *pv : part->volumes)
                if (pv->is_model_part())
                    out += area((pv->*k.member).get_facets(*pv, k.state));
        INFO(k.name << ": source " << in << " mm2, parts " << out << " mm2");
        REQUIRE(in > 100.);
        CHECK(std::abs(out - in) <= 0.01 * in);
    }
    // Each pillar top got its own share of the fuzzy band (the band crosses the Z plane at 150).
    for (size_t i = 1; i < 3; ++i) {
        bool fuzzy = false;
        for (const ModelVolume *pv : res.parts[i]->volumes)
            fuzzy = fuzzy || (pv->is_model_part() && pv->is_fuzzy_skin_painted());
        CHECK(fuzzy);
    }
}
