// Split to Fit, phases 1 and 2: the planner, the apply and the automatic dowels
// (tests/design_split_to_build_volume.md).
#include <catch2/catch.hpp>

#include <libslic3r/CutUtils.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Semver.hpp>
#include <libslic3r/SplitToVolume.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Utils.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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

// =====================================================================================================
// Phase 2: automatic dowels
// =====================================================================================================

namespace {

// GLGizmoCut3D::apply_cut_connectors() and the Plug / Dowel / Snap branch of get_connector_mesh() as
// they were before P2 moved them to libslic3r (copied from main at 2999085364, gizmo snap defaults).
void p2_old_gizmo_bake(ModelObject *mo, const std::string &connector_name)
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
        TriangleMesh mesh       = TriangleMesh(connector_mesh);
        ModelVolume *new_volume = mo->add_volume(std::move(mesh), ModelVolumeType::NEGATIVE_VOLUME);
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
