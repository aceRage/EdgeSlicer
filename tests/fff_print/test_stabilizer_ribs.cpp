// Rib stabilizers (stabilizer_column_shape = rib; libslic3r/Support/Stabilizers.hpp, support fins PR 3):
// edge-on sway braces against the side of a tall part, with an evenly spaced comb of one-layer tines that
// fuse into it.
//
// The rib's profile (its depth at the bed down to the flat top, its thickness growing with its height),
// the tine comb (its spacing and the grip height), a wall that narrows upwards (the rib follows it at 45
// degrees, and a tine over an opening too wide to bridge is counted, never silently dropped), the live
// support extrusions, the bake (the mesh slices back to the live sections; as a part of the object the
// tines fuse), the 3MF round trip, and that the rib keys change nothing while the column shape is not Rib.
//
// Credit: sway braces with a tine comb are a Support Fins option (gittrahan/support-fins, printfins.com,
// MIT), after Slant3D's designed-in supports; EdgeSlicer's own implementation.

#include <edge_test_scratch.hpp>
#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Support/FinMesh.hpp"
#include "libslic3r/Support/StabilizerBake.hpp"
#include "libslic3r/Support/Stabilizers.hpp"
#include "libslic3r/Support/SupportLayer.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;
using namespace Slic3r::stabilizers;

namespace {

double sr_area(const ExPolygons &expolys)
{
    double a = 0.;
    for (const ExPolygon &e : expolys)
        a += e.area();
    return unscaled(unscaled(a));
}

bool sr_inside(const ExPolygons &islands, const Point &p)
{
    return std::any_of(islands.begin(), islands.end(), [&p](const ExPolygon &e) { return e.contains(p); });
}

Point sr_pt(const Vec2d &v) { return Point(scaled(v.x()), scaled(v.y())); }

// An enum (or int) option of an object's own config; -1 when it has none.
int sr_opt_int(const ModelConfigObject &config, const char *key)
{
    const ConfigOption *opt = config.option(key);
    return opt == nullptr ? -1 : opt->getInt();
}

// The 6 x 60 mm pin of the stabilizer tests, a cone (8 mm radius at the bed, narrowing to a point at 60 mm)
// and a stepped pin (10 mm across up to 20 mm, 4 mm above, 60 mm tall).
enum class SrPart { Pin, Cone, Step };

TriangleMesh sr_mesh(SrPart part)
{
    if (part == SrPart::Pin)
        return TriangleMesh(its_make_cylinder(3., 60., M_PI / 90.));
    if (part == SrPart::Cone)
        return TriangleMesh(its_make_cone(8., 60., M_PI / 90.));
    indexed_triangle_set lower = its_make_cylinder(5., 20., M_PI / 90.);
    indexed_triangle_set upper = its_make_cylinder(2., 40., M_PI / 90.);
    its_translate(upper, Vec3f(0.f, 0.f, 20.f));
    its_merge(lower, upper);
    return TriangleMesh(lower);
}

using SrSettings = std::vector<std::pair<std::string, std::string>>;

DynamicPrintConfig sr_config(const SrSettings &extra = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "enable_support", "0" },
        { "stabilizer_supports", "auto" },
        { "stabilizer_ring_spacing", "15" },
        { "stabilizer_points_per_ring", "3" },
        { "stabilizer_column_shape", "rib" },
        { "elefant_foot_compensation", "0" },
    });
    for (const auto &kv : extra)
        config.set_deserialize_strict(kv.first, kv.second);
    return config;
}

struct SrPrint
{
    Model model;
    Print print;
    const PrintObject &po() const { return *print.objects().front(); }
};

ModelObject *sr_add(Model &model, SrPart part)
{
    ModelObject *object = model.add_object();
    object->name        = part == SrPart::Pin ? "pin" : part == SrPart::Cone ? "cone" : "step";
    object->add_volume(sr_mesh(part));
    object->add_instance();
    object->ensure_on_bed();
    return object;
}

void sr_process(Print &print, Model &model, const DynamicPrintConfig &config)
{
    for (ModelObject *o : model.objects)
        print.auto_assign_extruders(o);
    print.apply(model, config);
    print.set_status_silent();
    print.process();
}

void sr_slice(SrPrint &p, SrPart part, const SrSettings &extra = {})
{
    sr_add(p.model, part);
    sr_process(p.print, p.model, sr_config(extra));
}

// The piece of `expolys` in the rib's own band (the rib's thickness and a line to either side).
ExPolygons sr_rib_piece(const Rib &r, const ExPolygons &expolys)
{
    const Vec2d  p    = r.perp();
    const double half = 0.5 * r.thickness + 0.4;
    Polygon      band;
    for (const Vec2d &v : { Vec2d(r.at(0.) - p * half), Vec2d(r.at(100.) - p * half), Vec2d(r.at(100.) + p * half), Vec2d(r.at(0.) + p * half) })
        band.points.push_back(sr_pt(v));
    if (band.is_clockwise())
        band.reverse();
    return intersection_ex(expolys, ExPolygons{ ExPolygon(band) });
}

// The range the pieces' contour points span along the rib (t) and across it (s). False when empty.
bool sr_extent(const Rib &r, const ExPolygons &pieces, double &tmin, double &tmax, double &smin, double &smax)
{
    tmin = smin = 1e9;
    tmax = smax = -1e9;
    for (const ExPolygon &e : pieces)
        for (const Point &pt : e.contour.points) {
            const Vec2d d = unscaled(pt) - r.origin;
            tmin = std::min(tmin, d.dot(r.dir));
            tmax = std::max(tmax, d.dot(r.dir));
            smin = std::min(smin, d.dot(r.perp()));
            smax = std::max(smax, d.dot(r.perp()));
        }
    return tmin <= tmax;
}

// The support islands printed at an object layer.
ExPolygons sr_live_islands(const PrintObject &po, double print_z)
{
    ExPolygons out;
    for (const SupportLayer *sl : po.support_layers())
        if (std::abs(sl->print_z - print_z) < EPSILON)
            expolygons_append(out, sl->support_islands);
    return union_ex(out);
}

// The worst per-layer symmetric difference of two section stacks, relative to the first, over layers whose
// first section is over 1 mm^2.
double sr_worst_xor(const std::vector<ExPolygons> &want, const std::vector<ExPolygons> &got, size_t &layers)
{
    double worst = 0.;
    layers       = 0;
    for (size_t i = 0; i < want.size() && i < got.size(); ++i) {
        const double a = sr_area(want[i]);
        if (a <= 1.)
            continue;
        ++layers;
        worst = std::max(worst, (sr_area(diff_ex(want[i], got[i])) + sr_area(diff_ex(got[i], want[i]))) / a);
    }
    return worst;
}

// A tine's middle of the gap: the tine runs from half a line inside the rib to RIB_TINE_DEPTH inside the part.
Vec2d sr_mid_gap(const Polyline &tine, double gap)
{
    const Vec2d a = unscaled(tine.first_point()), b = unscaled(tine.last_point());
    const Vec2d u = (b - a).normalized();
    return b - u * (RIB_TINE_DEPTH + 0.5 * gap);
}

bool sr_round_trip(Model &model, Model &loaded, const char *name)
{
    const boost::filesystem::path tmp_root = edge_test::snorca_tests_dir();
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string  file   = (tmp_root / name).string();
    DynamicPrintConfig config = sr_config();

    StoreParams store_params;
    store_params.path     = file.c_str();
    store_params.model    = &model;
    store_params.config   = &config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    if (! store_bbs_3mf(store_params))
        return false;

    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool loaded_ok = load_bbs_3mf(file.c_str(), &dst_config, &ctxt, &loaded, &plate_data, &project_presets, &is_bbl_3mf,
                                        &file_version, nullptr,
                                        LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                                            LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    boost::filesystem::remove(file);
    return loaded_ok;
}

} // namespace

TEST_CASE("Rib profile: depth at the bed, flat top, thickness growing with height", "[SupportMaterial][Fins][Stabilizer]")
{
    // 15 % of the height at the bed, never less than the flat top.
    CHECK(rib_bed_depth(45.) == Approx(6.75));
    CHECK(rib_bed_depth(100.) == Approx(15.));
    CHECK(rib_bed_depth(10.) == Approx(RIB_TOP_DEPTH));
    // 1.2 mm plus 0.004 mm per mm of height, at most 2.4 mm, never under two lines.
    CHECK(rib_thickness(0., 0.42) == Approx(1.2));
    CHECK(rib_thickness(45., 0.42) == Approx(1.38));
    CHECK(rib_thickness(100., 0.42) == Approx(1.6));
    CHECK(rib_thickness(300., 0.42) == Approx(2.4));
    CHECK(rib_thickness(1000., 0.42) == Approx(2.4));
    CHECK(rib_thickness(10., 1.0) == Approx(2.0));
    CHECK(rib_thickness(50., 0.42) < rib_thickness(150., 0.42));

    Rib r;
    r.top_z = 80.;
    r.edge  = 3.;
    r.gap   = 0.2;
    CHECK(r.depth_at(0.) == Approx(12.));
    CHECK(r.depth_at(40.) == Approx(8.));
    CHECK(r.depth_at(80.) == Approx(RIB_TOP_DEPTH));
    CHECK(r.depth_at(90.) == Approx(RIB_TOP_DEPTH));
    CHECK(r.outer_at(0.) == Approx(15.2));

    // The defaults: ribs only when asked for, a tine every 6 mm from the bed up.
    const DynamicPrintConfig defaults = DynamicPrintConfig::full_print_config();
    CHECK(defaults.opt_enum<StabilizerColumnShape>("stabilizer_column_shape") == scsRound);
    CHECK(defaults.opt_float("stabilizer_rib_tine_spacing") == Approx(6.));
    CHECK(defaults.opt_float("stabilizer_rib_grip_from") == Approx(0.));
    const StabilizerSettings st = StabilizerSettings::from_config(PrintObjectConfig(), 0.42);
    CHECK(! st.ribs());
    CHECK(st.rib_gap() == Approx(RIB_MIN_GAP));
}

TEST_CASE("Rib tine comb: even spacing from the grip height", "[SupportMaterial][Fins][Stabilizer]")
{
    std::vector<LayerOutline> layers;
    for (int k = 0; k < 300; ++k)
        layers.push_back({ float(0.1 + 0.2 * k), nullptr });
    StabilizerSettings st;

    auto heights = [&layers](const std::vector<size_t> &idx) {
        std::vector<double> z;
        for (size_t i : idx)
            z.push_back(layers[i].slice_z);
        return z;
    };
    SECTION("every 6 mm from the bed: never on the first layer")
    {
        st.rib_tine_spacing = 6.;
        st.rib_grip_from    = 0.;
        const std::vector<double> z = heights(rib_tine_layers(layers, st, 45.));
        REQUIRE(z.size() == 7);
        for (size_t k = 0; k < z.size(); ++k)
            CHECK(std::abs(z[k] - 6. * double(k + 1)) < 0.11);
    }
    SECTION("from the grip height up, evenly")
    {
        st.rib_tine_spacing = 8.;
        st.rib_grip_from    = 10.;
        const std::vector<double> z = heights(rib_tine_layers(layers, st, 45.));
        REQUIRE(z.size() == 5);
        for (size_t k = 0; k < z.size(); ++k)
            CHECK(std::abs(z[k] - (10. + 8. * double(k))) < 0.11);
        // Nothing above the rib's top.
        CHECK(heights(rib_tine_layers(layers, st, 20.)).size() == 2);
    }
    SECTION("no tines with a spacing of 0")
    {
        st.rib_tine_spacing = 0.;
        CHECK(rib_tine_layers(layers, st, 45.).empty());
    }
    SECTION("never two in a row, however fine the spacing")
    {
        st.rib_tine_spacing = 0.1;
        st.rib_grip_from    = 1.;
        const std::vector<size_t> idx = rib_tine_layers(layers, st, 10.);
        REQUIRE(idx.size() > 10);
        for (size_t k = 1; k < idx.size(); ++k)
            CHECK(idx[k] >= idx[k - 1] + 2);
    }
}

TEST_CASE("Ribs on a pin: one per direction, deep at the bed, a flat top, the gap kept", "[SupportMaterial][Fins][Stabilizer]")
{
    SrPrint p;
    sr_slice(p, SrPart::Pin);
    const PrintObject &po = p.po();

    PlanReport                      rep;
    const Plan                      plan     = plan_stabilizers(po, &rep);
    const StabilizerSettings        st       = settings_of(po);
    const std::vector<LayerOutline> outlines = outlines_of(po);
    REQUIRE(st.ribs());
    // Ribs instead of struts and pillars: three directions, the three rings sharing them.
    REQUIRE(plan.ribs.size() == 3);
    CHECK(plan.struts.empty());
    CHECK(plan.pillars.empty());
    CHECK(plan.braces.empty());
    CHECK(rep.ribs == 3);

    PlanReport                  srep;
    const std::vector<RibSlice> slices = slice_ribs(outlines, st, plan, &srep, nullptr);
    CHECK(srep.ribs_standing == 3);
    const double w = st.line_width;

    for (const Rib &r : plan.ribs) {
        INFO("rib along " << r.dir.transpose());
        // Up to the highest ring, 45 mm.
        CHECK(std::abs(r.top_z - 45.) < 0.15);
        CHECK(r.thickness == Approx(rib_thickness(r.top_z, w)));
        CHECK(std::abs(r.edge - 3.) < 0.05);
        CHECK(r.gap == Approx(RIB_MIN_GAP));

        size_t top_layer = 0;
        for (size_t i = 0; i < outlines.size(); ++i) {
            const ExPolygons piece = sr_rib_piece(r, slices[i].body);
            const double     z     = outlines[i].slice_z;
            if (z > r.top_z + EPSILON) {
                // A flat top: nothing above it.
                CHECK(piece.empty());
                continue;
            }
            REQUIRE(! piece.empty());
            top_layer = i;
            double tmin, tmax, smin, smax;
            REQUIRE(sr_extent(r, piece, tmin, tmax, smin, smax));
            // The depth beyond the gap tapers linearly, the inner edge keeps the gap off the round side.
            CHECK(std::abs((tmax - r.edge - r.gap) - r.depth_at(z)) < 0.1);
            CHECK(tmin > r.edge + r.gap - 0.15);
            CHECK(std::abs((smax - smin) - r.thickness) < 0.05);
            // Each layer stands on the one below, grown by at most the layer height.
            if (i > 0)
                CHECK(sr_area(diff_ex(piece, offset_ex(sr_rib_piece(r, slices[i - 1].body), scaled<float>(0.21)))) < 0.01);
        }
        double tmin, tmax, smin, smax;
        REQUIRE(sr_extent(r, sr_rib_piece(r, slices[top_layer].body), tmin, tmax, smin, smax));
        CHECK(std::abs((tmax - tmin) - RIB_TOP_DEPTH) < 0.2);
        REQUIRE(sr_extent(r, sr_rib_piece(r, slices.front().body), tmin, tmax, smin, smax));
        CHECK(std::abs((tmax - tmin) - rib_bed_depth(r.top_z)) < 0.2);
    }
    // Never within the gap of the part, on any layer (only the tines cross it).
    for (size_t i = 0; i < outlines.size(); ++i)
        CHECK(sr_area(intersection_ex(slices[i].body, offset_ex(*outlines[i].islands, scaled<float>(RIB_MIN_GAP - 0.02)))) < 0.01);
}

TEST_CASE("Rib tines: one bead, one layer, across the gap into the part", "[SupportMaterial][Fins][Stabilizer]")
{
    struct Case { const char *spacing; const char *grip; size_t layers; double first; double step; };
    for (const Case c : { Case{ "6", "0", 7, 6., 6. }, Case{ "8", "10", 5, 10., 8. }, Case{ "0", "0", 0, 0., 0. } }) {
        DYNAMIC_SECTION("tine spacing " << c.spacing << ", grip from " << c.grip)
        {
            SrPrint p;
            sr_slice(p, SrPart::Pin, { { "stabilizer_rib_tine_spacing", c.spacing }, { "stabilizer_rib_grip_from", c.grip } });
            const PrintObject              &po       = p.po();
            const Plan                      plan     = plan_stabilizers(po);
            const StabilizerSettings        st       = settings_of(po);
            const std::vector<LayerOutline> outlines = outlines_of(po);
            REQUIRE(plan.ribs.size() == 3);
            REQUIRE(plan.rib_tine_layers.size() == c.layers);
            for (size_t k = 0; k < c.layers; ++k)
                CHECK(std::abs(outlines[plan.rib_tine_layers[k]].slice_z - (c.first + c.step * double(k))) < 0.11);

            PlanReport                  rep;
            const std::vector<RibSlice> slices = slice_ribs(outlines, st, plan, &rep, nullptr);
            // Every rib gets its tine on every tine layer of a round pin.
            CHECK(rep.rib_tines == 3 * c.layers);
            CHECK(rep.rib_tines_skipped == 0);
            const std::set<size_t> tine_layers(plan.rib_tine_layers.begin(), plan.rib_tine_layers.end());
            for (size_t i = 0; i < slices.size(); ++i) {
                if (! tine_layers.count(i)) {
                    CHECK(slices[i].tines.empty());
                    continue;
                }
                REQUIRE(slices[i].tines.size() == 3);
                REQUIRE(slices[i].tine_areas.size() == 3);
                for (const Polyline &t : slices[i].tines) {
                    REQUIRE(t.points.size() == 2);
                    // From inside the rib to RIB_TINE_DEPTH into the part: the part's wall fuses to it.
                    CHECK(sr_inside(slices[i].body, t.first_point()));
                    CHECK(sr_inside(*outlines[i].islands, t.last_point()));
                    CHECK(std::abs(unscaled(t.length()) - (RIB_MIN_GAP + RIB_TINE_DEPTH + 0.5 * st.line_width)) < 0.06);
                    // The gap is open on the layers either side: the tine is one layer.
                    const Point mid = sr_pt(sr_mid_gap(t, RIB_MIN_GAP));
                    CHECK(sr_inside(slices[i].tine_areas, mid));
                    CHECK(! sr_inside(slices[i - 1].body, mid));
                    CHECK(! sr_inside(slices[i + 1].body, mid));
                }
            }
        }
    }
}

TEST_CASE("Ribs follow a part that narrows upwards, and count the tines they cannot place", "[SupportMaterial][Fins][Stabilizer]")
{
    SECTION("a cone: the rib closes in at the part's own slope, every tine placed")
    {
        SrPrint p;
        sr_slice(p, SrPart::Cone);
        const PrintObject              &po       = p.po();
        const Plan                      plan     = plan_stabilizers(po);
        const std::vector<LayerOutline> outlines = outlines_of(po);
        REQUIRE(plan.ribs.size() == 3);
        PlanReport                  rep;
        const std::vector<RibSlice> slices = slice_ribs(outlines, settings_of(po), plan, &rep, nullptr);
        CHECK(rep.rib_tines == 3 * plan.rib_tine_layers.size());
        CHECK(rep.rib_tines_skipped == 0);
        for (const Rib &r : plan.ribs) {
            CHECK(std::abs(r.edge - 8.) < 0.05);
            // At the top (45 mm) the cone is 2 mm across its radius: the rib stands there, not 8 mm out.
            size_t top = 0;
            for (size_t i = 0; i < outlines.size() && outlines[i].slice_z <= r.top_z + EPSILON; ++i)
                top = i;
            double tmin, tmax, smin, smax;
            REQUIRE(sr_extent(r, sr_rib_piece(r, slices[top].body), tmin, tmax, smin, smax));
            CHECK(tmin < 2. + RIB_MIN_GAP + 0.3);
        }
    }
    SECTION("a step: the rib needs 3 mm to close in, so a tine right above the step is skipped and counted")
    {
        for (const auto &[grip, placed] : std::vector<std::pair<const char *, bool>>{ { "20.6", false }, { "24.6", true } }) {
            DYNAMIC_SECTION("tine at " << grip << " mm")
            {
                SrPrint p;
                sr_slice(p, SrPart::Step, { { "stabilizer_rib_grip_from", grip }, { "stabilizer_rib_tine_spacing", "30" } });
                const PrintObject &po   = p.po();
                const Plan         plan = plan_stabilizers(po);
                REQUIRE(plan.ribs.size() == 3);
                REQUIRE(plan.rib_tine_layers.size() == 1);
                PlanReport rep;
                slice_ribs(outlines_of(po), settings_of(po), plan, &rep, nullptr);
                CHECK(rep.rib_tines == (placed ? 3 : 0));
                CHECK(rep.rib_tines_skipped == (placed ? 0 : 3));
            }
        }
    }
}

TEST_CASE("Live ribs print as support at the object's layers, supports on or off", "[SupportMaterial][Fins][Stabilizer]")
{
    for (const char *support : { "0", "1" }) {
        DYNAMIC_SECTION("enable supports " << support)
        {
            SrPrint p;
            sr_slice(p, SrPart::Pin, { { "enable_support", support }, { "support_type", "normal(auto)" } });
            const PrintObject &po   = p.po();
            const Plan         plan = plan_stabilizers(po);
            REQUIRE(plan.ribs.size() == 3);
            PlanReport                  rep;
            const std::vector<RibSlice> slices = slice_ribs(outlines_of(po), settings_of(po), plan, &rep, nullptr);
            REQUIRE(rep.rib_tines == 21);

            std::set<long long> object_z;
            for (const Layer *l : po.layers())
                object_z.insert(std::llround(l->print_z * 1e6));
            REQUIRE(! po.support_layers().empty());
            size_t tines = 0;
            auto same = [](const Point &a, const Point &b) { return (a - b).cast<double>().norm() < 10.; };
            for (const SupportLayer *sl : po.support_layers()) {
                // One support layer per object layer height, nothing between.
                CHECK(object_z.count(std::llround(sl->print_z * 1e6)) == 1);
                Polylines pls;
                sl->support_fills.collect_polylines(pls);
                for (size_t i = 0; i < slices.size(); ++i) {
                    if (std::abs(po.layers()[i]->print_z - sl->print_z) > EPSILON)
                        continue;
                    for (const Polyline &t : slices[i].tines)
                        for (const Polyline &pl : pls)
                            if (pl.points.size() == 2 && ((same(pl.points[0], t.points[0]) && same(pl.points[1], t.points[1])) ||
                                                          (same(pl.points[0], t.points[1]) && same(pl.points[1], t.points[0]))))
                                ++tines;
                }
            }
            CHECK(tines == 21);
            // The walls print whole.
            for (size_t i = 0; i < slices.size(); ++i)
                if (! slices[i].body.empty())
                    CHECK(sr_area(diff_ex(fins::printable_fin_islands(slices[i].body, settings_of(po).line_width),
                                          sr_live_islands(po, po.layers()[i]->print_z))) < 0.05);
        }
    }
}

TEST_CASE("The rib settings change nothing unless the column shape is Rib", "[SupportMaterial][Fins][Stabilizer]")
{
    auto areas = [](const SrSettings &extra) {
        SrPrint p;
        sr_slice(p, SrPart::Pin, extra);
        std::vector<std::pair<long long, double>> out;
        for (const SupportLayer *sl : p.po().support_layers()) {
            Polylines pls;
            sl->support_fills.collect_polylines(pls);
            double len = 0.;
            for (const Polyline &pl : pls)
                len += unscaled(pl.length());
            out.emplace_back(std::llround(sl->print_z * 1e6), sr_area(sl->support_islands) + 1000. * len);
        }
        CHECK(plan_stabilizers(p.po()).ribs.empty());
        return out;
    };
    for (const char *shape : { "round", "auto" }) {
        DYNAMIC_SECTION("column shape " << shape)
        {
            const auto plain = areas({ { "stabilizer_column_shape", shape } });
            const auto tuned = areas({ { "stabilizer_column_shape", shape }, { "stabilizer_rib_tine_spacing", "2.5" }, { "stabilizer_rib_grip_from", "7" } });
            REQUIRE(! plain.empty());
            REQUIRE(plain.size() == tuned.size());
            for (size_t i = 0; i < plain.size(); ++i) {
                CHECK(plain[i].first == tuned[i].first);
                CHECK(plain[i].second == tuned[i].second);
            }
        }
    }
}

TEST_CASE("Manual rib stabilizers without paint place nothing", "[SupportMaterial][Fins][Stabilizer]")
{
    SrPrint p;
    sr_slice(p, SrPart::Pin, { { "stabilizer_supports", "manual" } });
    PlanReport rep;
    CHECK(plan_stabilizers(p.po(), &rep).ribs.empty());
    CHECK(rep.manual_without_paint);
    const StabilizerBakeResult res = bake_stabilizers(p.po(), {});
    CHECK(res.mesh.indices.empty());
    CHECK(! res.error.empty());
}

TEST_CASE("Rib bake: the mesh slices back to the live ribs and keeps the one-layer tines", "[SupportMaterial][Fins][Stabilizer][StabilizerBake]")
{
    for (const StabilizerBakePlacement placement : { StabilizerBakePlacement::PartOfObject, StabilizerBakePlacement::SeparateObject }) {
        DYNAMIC_SECTION((placement == StabilizerBakePlacement::PartOfObject ? "as a part" : "as a separate object"))
        {
            SrPrint p;
            sr_slice(p, SrPart::Pin);
            const PrintObject &po = p.po();
            StabilizerBakeOptions opts;
            opts.placement = placement;
            const StabilizerBakeResult res = bake_stabilizers(po, opts);
            REQUIRE(res.error.empty());
            REQUIRE(res.ribs());
            CHECK(res.plan.ribs.size() == 3);
            CHECK(res.struts.empty());
            CHECK(res.rib_tines == 21);
            CHECK(res.mesh_report.unioned);
            CHECK(res.mesh_report.closed);
            CHECK(its_num_open_edges(res.mesh) == 0);
            const double lw = settings_of(po).line_width;
            CHECK(res.rib_tine_width == Approx(2.5 * lw));

            // The mesh in the planner's frame, sliced at the object's own layers.
            fins::FinMeshReport        mrep;
            const indexed_triangle_set mesh = fins::fin_mesh(res.rib_bands, {}, &mrep);
            CHECK(mrep.dropped == 0);
            std::vector<float> zs;
            for (const Layer *l : po.layers())
                zs.push_back(float(l->print_z - 0.5 * l->height));
            const std::vector<ExPolygons> baked = slice_mesh_ex(mesh, zs);
            REQUIRE(baked.size() == po.layers().size());

            const double lw1 = support_material_1st_layer_flow(&po, float(po.layers().front()->height)).width();
            std::vector<ExPolygons> bands, walls, live;
            const std::set<size_t> tine_layers(res.plan.rib_tine_layers.begin(), res.plan.rib_tine_layers.end());
            for (size_t i = 0; i < po.layers().size(); ++i) {
                bands.push_back(res.rib_bands[i].sections);
                walls.push_back(fins::printable_fin_islands(res.rib_slices[i].body, i == 0 ? lw1 : lw));
                ExPolygons l = walls.back();
                expolygons_append(l, res.rib_slices[i].tine_areas);
                live.push_back(union_ex(l));
            }
            size_t layers = 0;
            // The baked mesh gives back its bands ...
            CHECK(sr_worst_xor(bands, baked, layers) < 0.02);
            CHECK(layers > 100);
            // ... the bands are the live walls everywhere but at the (wider) tines ...
            for (size_t i = 0; i < bands.size(); ++i) {
                INFO("layer " << i << " at z " << po.layers()[i]->print_z);
                CHECK(sr_area(diff_ex(walls[i], bands[i])) < 0.01);
                if (! tine_layers.count(i))
                    CHECK(sr_area(diff_ex(bands[i], walls[i])) < 0.01);
            }
            // ... and the live generator printed exactly what was planned (supports off).
            std::vector<ExPolygons> printed;
            for (const Layer *l : po.layers())
                printed.push_back(sr_live_islands(po, l->print_z));
            CHECK(sr_worst_xor(live, printed, layers) < 0.02);

            // Every tine is in the mesh on its own layer only: the gap is open on the layers either side.
            size_t tines = 0;
            for (size_t i : res.plan.rib_tine_layers)
                for (const Polyline &t : res.rib_slices[i].tines) {
                    const Point mid = sr_pt(sr_mid_gap(t, RIB_MIN_GAP));
                    INFO("tine at z " << po.layers()[i]->print_z);
                    CHECK(sr_inside(baked[i], mid));
                    CHECK(! sr_inside(baked[i - 1], mid));
                    CHECK(! sr_inside(baked[i + 1], mid));
                    ++tines;
                }
            CHECK(tines == 21);
            double in_part = 0.;
            for (size_t i = 0; i < baked.size(); ++i)
                in_part += sr_area(intersection_ex(baked[i], po.layers()[i]->lslices));
            if (placement == StabilizerBakePlacement::SeparateObject)
                // A separate object's tines stop at the part's outline.
                CHECK(in_part < 0.05);
            else
                // A part's tines run into it to fuse.
                CHECK(in_part > 0.5);
        }
    }
}

TEST_CASE("Ribs baked as a part of the object fuse to it at the tines only", "[SupportMaterial][Fins][Stabilizer][StabilizerBake]")
{
    const DynamicPrintConfig config = sr_config();
    SrPrint      p;
    ModelObject *src = sr_add(p.model, SrPart::Pin);
    sr_process(p.print, p.model, config);
    const PrintObject &po = p.po();
    StabilizerBakeOptions opts;
    opts.placement = StabilizerBakePlacement::PartOfObject;
    const StabilizerBakeResult res = bake_stabilizers(po, opts);
    REQUIRE(res.error.empty());
    std::vector<double> zs;
    for (const Layer *l : po.layers())
        zs.push_back(l->print_z);
    const Point shift0 = po.instances().front().shift;

    REQUIRE(apply_stabilizer_bake(p.model, *src, res, opts) == src);
    REQUIRE(src->volumes.size() == 2);
    CHECK(src->volumes.back()->name == "Stabilizers");
    // No settings of its own: the ribs share the part's region.
    CHECK(src->volumes.back()->config.empty());
    CHECK(sr_opt_int(src->config, "stabilizer_supports") == int(smOff));

    Print again;
    sr_process(again, p.model, config);
    const PrintObject &po2 = *again.objects().front();
    CHECK(po2.num_printing_regions() == 1);
    CHECK(po2.support_layers().empty());
    REQUIRE(po2.layers().size() == zs.size());
    const Point d = shift0 - po2.instances().front().shift;
    size_t fused = 0;
    for (size_t i : res.plan.rib_tine_layers)
        for (const Polyline &t : res.rib_slices[i].tines) {
            Point mid = sr_pt(sr_mid_gap(t, RIB_MIN_GAP));
            mid += d;
            INFO("tine at z " << zs[i]);
            CHECK(std::abs(po2.layers()[i]->print_z - zs[i]) < EPSILON);
            // Fused on the tine layer, apart on the layers either side.
            const bool f = sr_inside(po2.layers()[i]->lslices, mid);
            CHECK(f);
            CHECK(! sr_inside(po2.layers()[i - 1]->lslices, mid));
            CHECK(! sr_inside(po2.layers()[i + 1]->lslices, mid));
            fused += f ? 1 : 0;
        }
    CHECK(fused == 21);
}

TEST_CASE("Rib stabilizers survive a 3MF round trip", "[SupportMaterial][Fins][Stabilizer][3mf]")
{
    SECTION("the settings")
    {
        Model        model;
        ModelObject *obj = sr_add(model, SrPart::Pin);
        obj->config.set_key_value("stabilizer_supports", new ConfigOptionEnum<StabilizerMode>(smAuto));
        obj->config.set_key_value("stabilizer_column_shape", new ConfigOptionEnum<StabilizerColumnShape>(scsRib));
        obj->config.set_key_value("stabilizer_rib_tine_spacing", new ConfigOptionFloat(4.5));
        obj->config.set_key_value("stabilizer_rib_grip_from", new ConfigOptionFloat(12.));
        Model loaded;
        REQUIRE(sr_round_trip(model, loaded, "stabilizer_ribs_settings.3mf"));
        REQUIRE(loaded.objects.size() == 1);
        const ModelConfigObject &cfg = loaded.objects.front()->config;
        REQUIRE(cfg.option("stabilizer_column_shape") != nullptr);
        CHECK(sr_opt_int(cfg, "stabilizer_column_shape") == int(scsRib));
        CHECK(cfg.opt_float("stabilizer_rib_tine_spacing") == Approx(4.5));
        CHECK(cfg.opt_float("stabilizer_rib_grip_from") == Approx(12.));
    }
    SECTION("the baked ribs")
    {
        SrPrint      p;
        ModelObject *src = sr_add(p.model, SrPart::Pin);
        sr_process(p.print, p.model, sr_config());
        StabilizerBakeOptions opts;
        opts.placement = StabilizerBakePlacement::PartOfObject;
        const StabilizerBakeResult res = bake_stabilizers(p.po(), opts);
        REQUIRE(res.error.empty());
        REQUIRE(apply_stabilizer_bake(p.model, *src, res, opts) != nullptr);
        const size_t triangles = src->volumes.back()->mesh().its.indices.size();
        Model loaded;
        REQUIRE(sr_round_trip(p.model, loaded, "stabilizer_ribs_baked.3mf"));
        REQUIRE(loaded.objects.size() == 1);
        const ModelObject &lo = *loaded.objects.front();
        REQUIRE(lo.volumes.size() == 2);
        CHECK(lo.volumes[1]->is_model_part());
        CHECK(lo.volumes[1]->name == "Stabilizers");
        CHECK(lo.volumes[1]->mesh().its.indices.size() == triangles);
        CHECK(its_num_open_edges(lo.volumes[1]->mesh().its) == 0);
        CHECK(sr_opt_int(lo.config, "stabilizer_supports") == int(smOff));
    }
}
