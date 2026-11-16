// Split to Fit, phase 1: the planner and the apply (tests/design_split_to_build_volume.md).
#include <catch2/catch.hpp>

#include <libslic3r/Model.hpp>
#include <libslic3r/SplitToVolume.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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

TEST_CASE("Split to Fit: apply keeps loose pieces in their part and warns", "[SplitToVolume]")
{
    Model        model;
    ModelObject *mo = add_object(model, u_shape(), "U");
    // One Z plane at 150 only: the top cell holds both pillar tops.
    const SplitPlan plan = split_plan_grid(split_object_mesh(*mo, 0), usable_box(400., 400., 205.), SplitOptions{});
    REQUIRE(plan.part_count() == 2);
    const SplitApplyResult res = split_apply(*mo, 0, plan);
    REQUIRE(res.parts.size() == 2);
    const auto it = std::find_if(res.warnings.begin(), res.warnings.end(),
                                 [](const SplitWarning &w) { return w.kind == SplitWarning::Kind::LoosePieces; });
    REQUIRE(it != res.warnings.end());
    REQUIRE(plan.parts[size_t(it->part)].name == "Z2");
    REQUIRE_THAT(it->value, WithinAbs(2., 1e-9));
    // The bottom part is one piece: no warning for it.
    REQUIRE(std::count_if(res.warnings.begin(), res.warnings.end(),
                          [](const SplitWarning &w) { return w.kind == SplitWarning::Kind::LoosePieces; }) == 1);
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
