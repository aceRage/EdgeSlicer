// Support fins (libslic3r/Support/Fins.hpp): the planner's geometry, tines on object layers (supports off,
// normal and tree supports with an independent support layer height, a different first layer and a
// height range), off = identical, refusals, painted placement and blockers, and the 3MF round trip of the
// settings and the paint.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Support/Fins.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

double area_mm2(const ExPolygons &e)
{
    double a = 0.;
    for (const ExPolygon &x : e)
        a += x.area();
    return unscaled(unscaled(a));
}

ExPolygon rect(double x0, double y0, double x1, double y1)
{
    return ExPolygon(Polygon({ { scaled(x0), scaled(y0) }, { scaled(x1), scaled(y0) }, { scaled(x1), scaled(y1) }, { scaled(x0), scaled(y1) } }));
}

// Plain layer outlines for the pure planner tests. `section(z)` gives the part's y range at slicing height z.
struct Synth
{
    std::vector<ExPolygons>     islands;
    std::vector<fins::FinLayer> layers;
};

template<class F> Synth synth(double x_half, double top, double first_h, double h, F section)
{
    Synth  s;
    double z = 0.;
    for (double lh = first_h; z + lh <= top + EPSILON; lh = h) {
        const double sz = z + 0.5 * lh;
        const std::pair<double, double> y = section(sz);
        s.islands.push_back(y.second - y.first > 0.01 ? ExPolygons{ rect(-x_half, y.first, x_half, y.second) } : ExPolygons{});
        z += lh;
        s.layers.push_back({ z, lh, nullptr });
    }
    for (size_t i = 0; i < s.layers.size(); ++i)
        s.layers[i].islands = &s.islands[i];
    return s;
}

// A 20 mm cube balanced on an edge along X: a diamond in YZ, 28.3 mm tall.
Synth cube_on_edge(double first_h = 0.2, double h = 0.2)
{
    const double top = 20. * M_SQRT2;
    return synth(10., top, first_h, h, [top](double z) {
        const double w = std::min(z, top - z);
        return std::make_pair(-w, w);
    });
}

// A 4 mm plate, 30 mm wide, leaning 25 degrees towards +Y, 22 mm tall: it rests on its full bottom.
Synth leaning_plate()
{
    const double k = std::tan(25. * M_PI / 180.), half = 2. / std::cos(25. * M_PI / 180.);
    return synth(15., 22., 0.2, 0.2, [k, half](double z) { return std::make_pair(z * k - half, z * k + half); });
}

fins::FinSettings auto_settings()
{
    fins::FinSettings st;
    st.mode = sfmAuto;
    return st;
}

double distance_to_contour(const ExPolygons &islands, const Point &p)
{
    double best = std::numeric_limits<double>::max();
    for (const ExPolygon &e : islands)
        for (const Line &l : e.contour.lines())
            best = std::min(best, unscaled(l.distance_to(p)));
    return best;
}

bool inside(const ExPolygons &islands, const Point &p)
{
    return std::any_of(islands.begin(), islands.end(), [&p](const ExPolygon &e) { return e.contains(p); });
}

// Each fin layer stands on the one below, keeps the gap off the part, and each tine reaches tine_depth
// into the part on its own layer and starts in the fin wall.
void check_slices(const Synth &s, const fins::FinSettings &st, const fins::FinPlan &plan, const std::vector<fins::FinSlice> &slices)
{
    REQUIRE(slices.size() == s.layers.size());
    CHECK(area_mm2(slices.front().body) > 1.);
    const std::set<size_t> tine_set(plan.tine_layers.begin(), plan.tine_layers.end());
    size_t tines = 0;
    for (size_t i = 0; i < slices.size(); ++i) {
        INFO("layer " << i << " at z " << s.layers[i].print_z);
        if (i > 0) {
            // Never wider than the layer below: nothing hangs over air or over the part.
            CHECK(area_mm2(diff_ex(slices[i].body, offset_ex(slices[i - 1].body, scaled<float>(0.001)))) < 1e-3);
        }
        // The wall keeps the gap off the part on its layer (the tines cross it, the wall does not).
        CHECK(area_mm2(intersection_ex(slices[i].body, offset_ex(s.islands[i], scaled<float>(st.gap - 0.02)))) < 1e-3);
        if (! slices[i].tines.empty())
            CHECK(tine_set.count(i) == 1);
        for (const Polyline &t : slices[i].tines) {
            REQUIRE(t.points.size() == 2);
            ++tines;
            const Point &fin_end = t.points.front(), &part_end = t.points.back();
            CHECK(inside(slices[i].body, fin_end));
            CHECK(inside(s.islands[i], part_end));
            CHECK(distance_to_contour(s.islands[i], part_end) > st.tine_depth - 0.02);
            // One bead across the gap, no more.
            CHECK(unscaled(t.length()) < st.gap + fins::TINE_MAX_EXTRA_SPAN + st.tine_depth + st.line_width + 0.01);
        }
    }
    CHECK(tines >= plan.fins.size());
}

// --- live slicing ---------------------------------------------------------------------------------

// TiltedCube: a 20 mm cube balanced on an edge along X (its 45 degree sides need no support at a 30 degree
// threshold). CubeWithLedge: the same with a horizontal 11 x 6 x 2 mm ledge off its +X side, which does: other
// support next to the fins, not in their place.
enum class Part { TiltedCube, LeaningPlate, CubeWithLedge };

TriangleMesh part_mesh(Part part)
{
    if (part == Part::TiltedCube || part == Part::CubeWithLedge) {
        TriangleMesh m = make_cube(20., 20., 20.);
        m.translate(-10.f, -10.f, -10.f);
        m.rotate_x(float(M_PI / 4.));
        if (part == Part::CubeWithLedge) {
            TriangleMesh ledge = make_cube(11., 6., 2.);
            ledge.translate(9.f, -3.f, -4.f);
            m.merge(ledge);
        }
        return m;
    }
    TriangleMesh m = make_cube(30., 4., 25.);
    m.translate(-15.f, -2.f, 0.f);
    m.rotate_x(float(25. * M_PI / 180.));
    return m;
}

using Settings = std::vector<std::pair<std::string, std::string>>;

struct FinPrint
{
    Model model;
    Print print;
    const PrintObject &po() const { return *print.objects().front(); }
};

DynamicPrintConfig fin_config(const Settings &extra)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "enable_support", "0" },
        { "support_fins", "auto" },
    });
    for (const auto &kv : extra)
        config.set_deserialize_strict(kv.first, kv.second);
    return config;
}

template<class Prepare> void slice_part(FinPrint &p, Part part, const Settings &extra, Prepare prepare)
{
    ModelObject *object = p.model.add_object();
    object->name        = part == Part::TiltedCube ? "tilted_cube" : part == Part::LeaningPlate ? "leaning_plate" : "cube_with_ledge";
    object->add_volume(part_mesh(part));
    object->add_instance();
    object->ensure_on_bed();
    prepare(*object);
    p.print.auto_assign_extruders(object);
    p.print.apply(p.model, fin_config(extra));
    p.print.set_status_silent();
    p.print.process();
}

void slice_part(FinPrint &p, Part part, const Settings &extra)
{
    slice_part(p, part, extra, [](ModelObject &) {});
}

// Paints `state` on every facet of the part whose normal (in the object's frame) satisfies `pick`.
template<class Pick> void paint(ModelObject &object, EnforcerBlockerType state, Pick pick)
{
    ModelVolume     *v = object.volumes.front();
    TriangleSelector selector(v->mesh());
    const auto      &its = v->mesh().its;
    for (int f = 0; f < int(its.indices.size()); ++f) {
        const Vec3f a = its.vertices[its.indices[f](0)], b = its.vertices[its.indices[f](1)], c = its.vertices[its.indices[f](2)];
        if (pick(Vec3d((b - a).cross(c - a).normalized().cast<double>())))
            selector.set_facet(f, state);
    }
    v->supported_facets.set(selector);
}

void leaves(const ExtrusionEntityCollection &coll, std::vector<const ExtrusionEntity *> &out)
{
    for (const ExtrusionEntity *e : coll.entities) {
        if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e))
            leaves(*c, out);
        else
            out.push_back(e);
    }
}

// Everything a support layer covers.
ExPolygons coverage(const SupportLayer &sl)
{
    ExPolygons c = sl.support_islands;
    expolygons_append(c, sl.tree_support_areas());
    return c.empty() ? c : union_ex(c);
}

struct AlignmentResult
{
    size_t candidates  = 0;  // tines the planner places without obstacles
    size_t found       = 0;  // of those, printed as a 2-point path somewhere
    size_t misplaced   = 0;  // printed on a support layer that is not their object layer, or with another height
    size_t off_grid    = 0;  // support layers not at any object layer's height (the independent heights at work)
    size_t overlap     = 0;  // tines printed where another support layer overlapping in Z prints too
    size_t duplicate_z = 0;  // two support layers at one print_z
    size_t other       = 0;  // support layers printing something besides the fins (so the case is not fins alone)
};

// The hard requirement: every tine on an object layer's print_z, with that layer's height, one support layer
// per Z, and never a fin where another support layer overlapping its Z range prints.
AlignmentResult check_alignment(const PrintObject &po)
{
    AlignmentResult r;
    const std::vector<const SupportLayer *> sls = po.support_layers().vector();
    for (size_t k = 1; k < sls.size(); ++k)
        if (sls[k]->print_z <= sls[k - 1]->print_z + EPSILON)
            ++r.duplicate_z;
    for (const SupportLayer *sl : sls) {
        const bool on_grid = std::any_of(po.layers().begin(), po.layers().end(),
                                         [sl](const Layer *l) { return std::abs(l->print_z - sl->print_z) < EPSILON; });
        if (! on_grid) {
            ++r.off_grid;
            if (area_mm2(coverage(*sl)) > 1.)
                ++r.other;
        }
    }

    // The fins as the planner places them with nothing in the way: their tines' paths, and their footprint.
    const std::vector<fins::FinLayer> layers = fins::layers_of(po);
    const fins::FinSettings           st     = fins::settings_of(po);
    const fins::FinPlan               plan   = fins::plan_fins(layers, st, fins::painted_fin_spots(po));
    const std::vector<fins::FinSlice> slices = fins::slice_fins(layers, st, plan, nullptr, nullptr, nullptr, {});

    auto same = [](const Point &a, const Point &b) { return (a - b).cast<double>().norm() < 10.; };
    for (size_t i = 0; i < slices.size(); ++i) {
        const Layer &layer = *po.layers()[i];
        std::vector<std::pair<const SupportLayer *, ExPolygons>> printed_tines;
        for (const Polyline &t : slices[i].tines) {
            ++r.candidates;
            for (const SupportLayer *sl : sls) {
                std::vector<const ExtrusionEntity *> ls;
                leaves(sl->support_fills, ls);
                for (const ExtrusionEntity *e : ls) {
                    const auto *path = dynamic_cast<const ExtrusionPath *>(e);
                    if (path == nullptr || path->polyline.points.size() != 2)
                        continue;
                    const Points &pp = path->polyline.points;
                    if (! ((same(pp[0], t.points[0]) && same(pp[1], t.points[1])) || (same(pp[0], t.points[1]) && same(pp[1], t.points[0]))))
                        continue;
                    ++r.found;
                    if (std::abs(sl->print_z - layer.print_z) > EPSILON || std::abs(double(path->height) - layer.height) > 1e-4)
                        ++r.misplaced;
                    printed_tines.emplace_back(sl, union_ex(offset(path->polyline, scaled<float>(0.5 * path->width))));
                }
            }
        }
        // Something besides the fins on the support layer at this object layer.
        for (const SupportLayer *sl : sls)
            if (std::abs(sl->print_z - layer.print_z) < EPSILON) {
                ExPolygons fins_here = slices[i].body;
                expolygons_append(fins_here, slices[i].tine_areas);
                if (area_mm2(diff_ex(coverage(*sl), offset_ex(fins_here, scaled<float>(0.5)))) > 1.)
                    ++r.other;
            }
        // A tine printed on its layer never shares its place with another support layer overlapping that layer's
        // Z range (the fin walls are checked against obstacles in the planner tests).
        for (const auto &[own, tine] : printed_tines) {
            for (const SupportLayer *sl : sls) {
                if (sl == own)
                    continue;
                const double bottom = sl->print_z - std::max({ double(sl->height), sl->base_slab_height, sl->interface_slab_height });
                if (sl->print_z <= layer.print_z - layer.height + EPSILON || bottom >= layer.print_z - EPSILON)
                    continue;
                if (area_mm2(intersection_ex(tine, coverage(*sl))) > 0.01)
                    ++r.overlap;
            }
        }
    }
    return r;
}

struct Signature
{
    std::vector<double> z, h;
    std::vector<size_t> entities;
    double              length = 0., area = 0.;
};

Signature signature(const PrintObject &po)
{
    Signature s;
    for (const SupportLayer *sl : po.support_layers()) {
        s.z.push_back(sl->print_z);
        s.h.push_back(sl->height);
        std::vector<const ExtrusionEntity *> ls;
        leaves(sl->support_fills, ls);
        s.entities.push_back(ls.size());
        for (const ExtrusionEntity *e : ls)
            s.length += e->length();
        s.area += area_mm2(coverage(*sl));
    }
    return s;
}

void check_same(const Signature &a, const Signature &b)
{
    REQUIRE(a.z.size() == b.z.size());
    for (size_t i = 0; i < a.z.size(); ++i) {
        CHECK(a.z[i] == b.z[i]);
        CHECK(a.h[i] == b.h[i]);
        CHECK(a.entities[i] == b.entities[i]);
    }
    CHECK(a.length == b.length);
    CHECK(a.area == b.area);
}

} // namespace

// --- planner geometry -----------------------------------------------------------------------------

TEST_CASE("Support fins: a cube balanced on its edge gets fins on both sides", "[SupportFins]")
{
    const Synth             s  = cube_on_edge();
    const fins::FinSettings st = auto_settings();
    fins::FinReport         rep;
    const fins::FinPlan     plan = fins::plan_fins(s.layers, st, {}, &rep);
    CHECK_FALSE(rep.leaning);
    // 20 mm along the edge: a fin near each end, on each side.
    REQUIRE(plan.fins.size() == 4);
    size_t plus = 0, minus = 0;
    for (const fins::Fin &f : plan.fins) {
        if (f.dir.y() > 0.99)
            ++plus;
        if (f.dir.y() < -0.99)
            ++minus;
        // Lateral positions inside the part, apart.
        CHECK(std::abs(f.origin.x()) < 10.);
        CHECK(std::abs(f.origin.x()) > 3.);
        CHECK_THAT(f.reach - f.edge, WithinAbs(st.gap + st.depth, 1e-9));
    }
    CHECK(plus == 2);
    CHECK(minus == 2);

    fins::FinReport                   srep;
    const std::vector<fins::FinSlice> slices = fins::slice_fins(s.layers, st, plan, nullptr, nullptr, &srep, {});
    check_slices(s, st, plan, slices);
    CHECK(srep.tines > 0);
    CHECK(srep.tines_skipped == 0);
    CHECK(srep.fins_standing == 4);
    // The 45 degree side covers the fin a little above its depth: no fin above the part's widest point.
    for (size_t i = 0; i < slices.size(); ++i)
        if (s.layers[i].print_z > 0.5 * 20. * M_SQRT2)
            CHECK(slices[i].body.empty());
}

TEST_CASE("Support fins: a leaning plate gets fins on its lean side only", "[SupportFins]")
{
    const Synth       s  = leaning_plate();
    fins::FinSettings st = auto_settings();
    SECTION("lean side only")
    {
        fins::FinReport     rep;
        const fins::FinPlan plan = fins::plan_fins(s.layers, st, {}, &rep);
        CHECK(rep.leaning);
        CHECK(rep.lean.y() > 1.);
        REQUIRE(! plan.fins.empty());
        for (const fins::Fin &f : plan.fins)
            CHECK(f.dir.y() > 0.99);
        // 30 mm wide at a 15 mm spacing: three fins.
        CHECK(plan.fins.size() == 3);
        check_slices(s, st, plan, fins::slice_fins(s.layers, st, plan, nullptr, nullptr, nullptr, {}));
    }
    SECTION("both sides")
    {
        st.lean_side_only       = false;
        const fins::FinPlan plan = fins::plan_fins(s.layers, st);
        CHECK(std::any_of(plan.fins.begin(), plan.fins.end(), [](const fins::Fin &f) { return f.dir.y() < -0.99; }));
        CHECK(std::any_of(plan.fins.begin(), plan.fins.end(), [](const fins::Fin &f) { return f.dir.y() > 0.99; }));
    }
    SECTION("the fin height caps the fins")
    {
        st.height                = 3.;
        const fins::FinPlan plan = fins::plan_fins(s.layers, st);
        const std::vector<fins::FinSlice> slices = fins::slice_fins(s.layers, st, plan, nullptr, nullptr, nullptr, {});
        for (size_t i = 0; i < slices.size(); ++i)
            if (s.layers[i].print_z > 3. + EPSILON)
                CHECK(slices[i].body.empty());
    }
}

TEST_CASE("Support fins: obstacles and blockers", "[SupportFins]")
{
    const Synth             s    = cube_on_edge();
    const fins::FinSettings st   = auto_settings();
    const fins::FinPlan     plan = fins::plan_fins(s.layers, st);
    REQUIRE(plan.fins.size() == 4);
    SECTION("other support in the way: the wall keeps out of it and its tines are left out, not moved")
    {
        // Something printing in the gap on the +Y side, just off the part, on every layer: the walls step back
        // from it and the tines would cross it.
        std::vector<ExPolygons> obstacles(s.layers.size());
        for (size_t i = 0; i < s.layers.size(); ++i) {
            const double w = s.layers[i].print_z - 0.5 * s.layers[i].height;
            obstacles[i]   = ExPolygons{ rect(-12., w + 0.03, 12., w + 0.08) };
        }
        fins::FinReport         rep;
        const std::vector<fins::FinSlice> slices = fins::slice_fins(s.layers, st, plan, &obstacles, nullptr, &rep, {});
        for (size_t i = 0; i < slices.size(); ++i) {
            CHECK(area_mm2(intersection_ex(slices[i].body, obstacles[i])) < 1e-3);
            CHECK(area_mm2(intersection_ex(slices[i].tine_areas, obstacles[i])) < 1e-3);
        }
        CHECK(rep.tines_skipped > 0);
    }
    SECTION("a painted blocker removes the fins it touches")
    {
        std::vector<ExPolygons> blockers(s.layers.size());
        blockers[10] = ExPolygons{ rect(-12., 0.5, 12., 2.5) };
        fins::FinReport rep;
        fins::slice_fins(s.layers, st, plan, nullptr, &blockers, &rep, {});
        CHECK(rep.blocked == 2);
    }
}

TEST_CASE("Support fins: tine layers", "[SupportFins]")
{
    fins::FinSettings st = auto_settings();
    SECTION("uniform 0.2 mm layers with base rows")
    {
        const Synth               s  = cube_on_edge();
        const std::vector<size_t> tl = fins::tine_layers(s.layers, st, 10.);
        REQUIRE(tl.size() >= 6);
        // Every other layer up to 2 mm: 0.4, 0.8, 1.2, 1.6, 2.0.
        CHECK(tl[0] == 1);
        for (size_t k = 0; k < 5; ++k)
            CHECK_THAT(s.layers[tl[k]].print_z, WithinAbs(0.4 + 0.4 * double(k), 1e-6));
        // Then one every tine spacing.
        CHECK_THAT(s.layers[tl[5]].print_z, WithinAbs(5.0, 1e-6));
    }
    SECTION("without base rows")
    {
        st.tine_base_rows             = false;
        const Synth               s  = cube_on_edge();
        const std::vector<size_t> tl = fins::tine_layers(s.layers, st, 10.);
        REQUIRE(! tl.empty());
        CHECK_THAT(s.layers[tl[0]].print_z, WithinAbs(3.0, 1e-6));
    }
    SECTION("a different first layer and fine layers")
    {
        const Synth               s  = cube_on_edge(0.3, 0.12);
        const std::vector<size_t> tl = fins::tine_layers(s.layers, st, 12.);
        REQUIRE(! tl.empty());
        CHECK(tl.front() >= 1);
        for (size_t k = 1; k < tl.size(); ++k) {
            CHECK(tl[k] >= tl[k - 1] + 2);
            if (s.layers[tl[k]].print_z > fins::TINE_BASE_ROWS_HEIGHT + EPSILON)
                CHECK(s.layers[tl[k]].print_z - s.layers[tl[k - 1]].print_z > st.tine_spacing - 1e-6);
        }
        const fins::FinPlan plan = fins::plan_fins(s.layers, st);
        check_slices(s, st, plan, fins::slice_fins(s.layers, st, plan, nullptr, nullptr, nullptr, {}));
    }
}

// --- live: the hard requirement -------------------------------------------------------------------

TEST_CASE("Support fins: tines land exactly on object layers", "[SupportFins]")
{
    auto check = [](const PrintObject &po, bool expect_all, bool other_support = false) {
        const AlignmentResult r = check_alignment(po);
        if (other_support)
            CHECK(r.other > 0);
        WARN("candidates " << r.candidates << ", found " << r.found << ", misplaced " << r.misplaced << ", off-grid support layers "
                           << r.off_grid << ", other support " << r.other << ", overlaps " << r.overlap << ", support layers "
                           << po.support_layers().size());
        CHECK(r.candidates > 0);
        CHECK(r.found > 0);
        CHECK(r.misplaced == 0);
        CHECK(r.overlap == 0);
        CHECK(r.duplicate_z == 0);
        if (expect_all)
            CHECK(r.found == r.candidates);
        // No zero-length beads on the support layers the fins inserted (a wall speck thinner than a line).
        size_t specks = 0;
        for (const SupportLayer *sl : po.support_layers()) {
            if (sl->support_type != stInnerTree)
                continue;
            std::vector<const ExtrusionEntity *> ls;
            leaves(sl->support_fills, ls);
            for (const ExtrusionEntity *e : ls)
                if (e->length() < scaled<double>(0.05))
                    ++specks;
        }
        CHECK(specks == 0);
    };
    SECTION("supports off")
    {
        FinPrint p;
        slice_part(p, Part::TiltedCube, {});
        check(p.po(), true);
        // Only fins print: every support layer is at an object layer.
        CHECK(check_alignment(p.po()).off_grid == 0);
    }
    SECTION("supports off, a thick first layer, fine layers and a height range")
    {
        FinPrint p;
        slice_part(p, Part::LeaningPlate, { { "initial_layer_print_height", "0.3" }, { "layer_height", "0.12" } }, [](ModelObject &o) {
            DynamicPrintConfig range;
            range.set_key_value("layer_height", new ConfigOptionFloat(0.24));
            o.layer_config_ranges[t_layer_height_range{ 2.5, 6. }].assign_config(std::move(range));
        });
        check(p.po(), true);
    }
    SECTION("normal supports with an independent support layer height")
    {
        FinPrint p;
        slice_part(p, Part::LeaningPlate,
                   { { "enable_support", "1" }, { "support_type", "normal(auto)" }, { "support_threshold_angle", "30" },
                     { "independent_support_layer_height", "1" }, { "support_top_z_distance", "0.15" },
                     { "support_bottom_z_distance", "0.15" }, { "layer_height", "0.2" } });
        check(p.po(), false, true);
    }
    SECTION("tree supports with an independent support layer height, next to the fins")
    {
        FinPrint p;
        slice_part(p, Part::CubeWithLedge,
                   { { "enable_support", "1" }, { "support_type", "tree(auto)" }, { "support_threshold_angle", "30" },
                     { "independent_support_layer_height", "1" }, { "support_top_z_distance", "0.15" },
                     { "support_bottom_z_distance", "0.15" }, { "layer_height", "0.2" } });
        check(p.po(), false, true);
    }
    SECTION("normal supports with an independent support layer height, next to the fins")
    {
        FinPrint p;
        slice_part(p, Part::CubeWithLedge,
                   { { "enable_support", "1" }, { "support_type", "normal(auto)" }, { "support_threshold_angle", "30" },
                     { "independent_support_layer_height", "1" }, { "support_top_z_distance", "0.15" },
                     { "support_bottom_z_distance", "0.15" }, { "layer_height", "0.2" } });
        check(p.po(), false, true);
    }
    SECTION("normal supports with the fins in their way")
    {
        FinPrint p;
        slice_part(p, Part::TiltedCube,
                   { { "enable_support", "1" }, { "support_type", "normal(auto)" }, { "support_threshold_angle", "50" },
                     { "independent_support_layer_height", "1" }, { "support_top_z_distance", "0.15" },
                     { "support_bottom_z_distance", "0.15" } });
        const AlignmentResult r = check_alignment(p.po());
        WARN("contested: candidates " << r.candidates << ", found " << r.found << ", off-grid " << r.off_grid << ", overlaps " << r.overlap);
        CHECK(r.misplaced == 0);
        CHECK(r.overlap == 0);
        CHECK(r.duplicate_z == 0);
    }
}

// --- off, refusals, painting ----------------------------------------------------------------------

TEST_CASE("Support fins: off leaves the support layers as they were", "[SupportFins]")
{
    SECTION("supports off: no support layers at all")
    {
        FinPrint p;
        slice_part(p, Part::TiltedCube, { { "support_fins", "off" } });
        CHECK(p.po().support_layers().empty());
    }
    SECTION("normal supports: off and Painted with nothing painted are the same")
    {
        const Settings base = { { "enable_support", "1" }, { "support_type", "normal(auto)" }, { "support_threshold_angle", "50" } };
        Settings       off = base, painted = base;
        off.push_back({ "support_fins", "off" });
        painted.push_back({ "support_fins", "painted" });
        FinPrint a, b;
        slice_part(a, Part::TiltedCube, off);
        slice_part(b, Part::TiltedCube, painted);
        REQUIRE(! a.po().support_layers().empty());
        check_same(signature(a.po()), signature(b.po()));
    }
    SECTION("a raft: refused, the raft and supports as without fins")
    {
        FinPrint a, b;
        slice_part(a, Part::TiltedCube, { { "raft_layers", "2" }, { "support_fins", "off" } });
        slice_part(b, Part::TiltedCube, { { "raft_layers", "2" }, { "support_fins", "auto" } });
        check_same(signature(a.po()), signature(b.po()));
    }
}

TEST_CASE("Support fins: painted fin spots", "[SupportFins]")
{
    // The lean side's face of the plate (it leans towards -Y in the mesh frame: rotate_x tips its top to -Y).
    auto lean_face = [](const Vec3d &n) { return n.y() < -0.7; };
    SECTION("Painted: one fin through the painted face only")
    {
        FinPrint p;
        slice_part(p, Part::LeaningPlate, { { "support_fins", "painted" } },
                   [&](ModelObject &o) { paint(o, EnforcerBlockerType::SUPPORT_FIN, lean_face); });
        const std::vector<stabilizers::PaintedSpot> spots = fins::painted_fin_spots(p.po());
        REQUIRE(spots.size() == 1);
        const fins::FinPlan plan = fins::plan_fins(fins::layers_of(p.po()), fins::settings_of(p.po()), spots);
        REQUIRE(plan.fins.size() == 1);
        CHECK(plan.fins.front().painted);
        // Every printed fin island lies along that one fin, near the spot's X.
        REQUIRE(! p.po().support_layers().empty());
        for (const SupportLayer *sl : p.po().support_layers())
            for (const ExPolygon &e : sl->support_islands) {
                const BoundingBox bb = get_extents(e.contour);
                CHECK(std::abs(unscaled(bb.center().x()) - spots.front().pos.x()) < 3.);
            }
    }
    SECTION("Painted with nothing painted: no fins")
    {
        FinPrint p;
        slice_part(p, Part::LeaningPlate, { { "support_fins", "painted" } });
        CHECK(p.po().support_layers().empty());
    }
    SECTION("Auto with the part painted as a support blocker: no fins")
    {
        FinPrint p;
        slice_part(p, Part::LeaningPlate, {}, [](ModelObject &o) { paint(o, EnforcerBlockerType::BLOCKER, [](const Vec3d &) { return true; }); });
        CHECK(p.po().support_layers().empty());
    }
    SECTION("Auto: fins on the lean side")
    {
        FinPrint p;
        slice_part(p, Part::LeaningPlate, {});
        fins::FinReport     rep;
        const fins::FinPlan plan = fins::plan_fins(fins::layers_of(p.po()), fins::settings_of(p.po()), {}, &rep);
        CHECK(rep.leaning);
        REQUIRE(! plan.fins.empty());
        for (const fins::Fin &f : plan.fins)
            CHECK(f.dir.dot(rep.lean.normalized()) > 0.99);
        CHECK(! p.po().support_layers().empty());
    }
}

// --- 3MF ------------------------------------------------------------------------------------------

TEST_CASE("Support fins: the settings and the paint survive a 3MF round trip", "[SupportFins]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "fins";
    object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();
    {
        DynamicPrintConfig c;
        c.set_deserialize_strict({ { "support_fins", "painted" }, { "support_fin_depth", "12" }, { "support_fin_gap", "0.3" },
                                   { "support_fin_tine_base_rows", "0" } });
        object->config.assign_config(std::move(c));
    }
    paint(*object, EnforcerBlockerType::SUPPORT_FIN, [](const Vec3d &n) { return n.x() > 0.7; });
    const size_t painted = object->volumes.front()->supported_facets.get_facets_strict(*object->volumes.front(), EnforcerBlockerType::SUPPORT_FIN).indices.size();
    REQUIRE(painted > 0);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "support_fins", "auto" }, { "support_fin_spacing", "22" } });

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() /
        ("snorca_tests_fins_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string path = (tmp_root / "fins.3mf").string();

    PlateData *plate   = new PlateData();
    plate->plate_index = 0;
    plate->objects_and_instances.emplace_back(0, 0);
    StoreParams sp;
    sp.path            = path.c_str();
    sp.model           = &model;
    sp.config          = &config;
    sp.plate_data_list = { plate };
    sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    const bool stored  = store_bbs_3mf(sp);
    delete plate;
    REQUIRE(stored);

    Model                     loaded;
    DynamicPrintConfig        loaded_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plates;
    std::vector<Preset *>     presets;
    bool                      is_bbl = false;
    Semver                    version;
    const bool ok = load_bbs_3mf(path.c_str(), &loaded_config, &ctxt, &loaded, &plates, &presets, &is_bbl, &version, nullptr,
                                 LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence);
    release_PlateData_list(plates);
    REQUIRE(ok);
    REQUIRE(loaded.objects.size() == 1);
    const ModelObject &lo = *loaded.objects.front();
    REQUIRE(lo.config.has("support_fins"));
    CHECK(lo.config.option("support_fins")->getInt() == int(sfmPainted));
    CHECK_THAT(lo.config.option("support_fin_depth")->getFloat(), WithinAbs(12., 1e-9));
    CHECK_THAT(lo.config.option("support_fin_gap")->getFloat(), WithinAbs(0.3, 1e-9));
    CHECK_FALSE(lo.config.option("support_fin_tine_base_rows")->getBool());
    REQUIRE(loaded_config.has("support_fins"));
    CHECK(loaded_config.option("support_fins")->getInt() == int(sfmAuto));
    CHECK_THAT(loaded_config.opt_float("support_fin_spacing"), WithinAbs(22., 1e-9));
    const ModelVolume &lv = *lo.volumes.front();
    CHECK(lv.supported_facets.has_facets(lv, EnforcerBlockerType::SUPPORT_FIN));
    CHECK(lv.supported_facets.get_facets_strict(lv, EnforcerBlockerType::SUPPORT_FIN).indices.size() == painted);
    // The other paint states stay empty: the fin spots do not turn into enforcers or stabilizer points.
    CHECK_FALSE(lv.supported_facets.has_facets(lv, EnforcerBlockerType::ENFORCER));
    CHECK_FALSE(lv.supported_facets.has_facets(lv, EnforcerBlockerType::STABILIZER));
    boost::system::error_code ec;
    boost::filesystem::remove_all(tmp_root, ec);
}
