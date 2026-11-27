// Support fins baked into real geometry (libslic3r/Support/FinBake.hpp, FinMesh.hpp).
//
// The bake's gate is the parity oracle: the mesh, sliced at the object's own layer heights, gives back
// the sections the live fins print. Then, baked as a part of the object and sliced again with the live
// fins off, the object's own layers are the part plus those sections: the tines fuse it to the walls (an
// extrusion crosses the gap at every tine) and the walls stay apart everywhere else. The rest: a separate
// object that lands where the live fins stood and never overlaps the part, the 3MF round trip, the live
// fins switched off, and the refusals.
//
// Credit: the technique is Slant3D's designed-in supports, as specified by Support Fins
// (gittrahan/support-fins, printfins.com, MIT); EdgeSlicer's own implementation.

#include <edge_test_scratch.hpp>
#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Support/FinBake.hpp"
#include "libslic3r/Support/FinMesh.hpp"
#include "libslic3r/Support/Fins.hpp"
#include "libslic3r/Support/SupportLayer.hpp"
#include "libslic3r/Support/SupportLayerAt.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;

namespace {

double fb_area(const ExPolygons &expolys)
{
    double a = 0.;
    for (const ExPolygon &e : expolys)
        a += e.area();
    return unscaled(unscaled(a));
}

bool fb_inside(const ExPolygons &islands, const Point &p)
{
    return std::any_of(islands.begin(), islands.end(), [&p](const ExPolygon &e) { return e.contains(p); });
}

// TiltedCube: a 20 mm cube balanced on an edge (fins on both sides). LeaningPlate: a 30 x 4 x 25 mm plate
// leaning 25 degrees (fins on its lean side). ThinPlate: tests/data/fins/tilted_thin_plate.stl, the hand
// test model.
enum class FbPart { TiltedCube, LeaningPlate, ThinPlate };

const char *fb_name(FbPart part)
{
    return part == FbPart::TiltedCube ? "tilted_cube" : part == FbPart::LeaningPlate ? "leaning_plate" : "tilted_thin_plate";
}

TriangleMesh fb_mesh(FbPart part)
{
    if (part == FbPart::TiltedCube) {
        TriangleMesh m = make_cube(20., 20., 20.);
        m.translate(-10.f, -10.f, -10.f);
        m.rotate_x(float(M_PI / 4.));
        return m;
    }
    if (part == FbPart::LeaningPlate) {
        TriangleMesh m = make_cube(30., 4., 25.);
        m.translate(-15.f, -2.f, 0.f);
        m.rotate_x(float(25. * M_PI / 180.));
        return m;
    }
    TriangleMesh m;
    const std::string path = std::string(TEST_DATA_DIR) + "/fins/tilted_thin_plate.stl";
    REQUIRE(m.ReadSTLFile(path.c_str()));
    return m;
}

using FbSettings = std::vector<std::pair<std::string, std::string>>;

DynamicPrintConfig fb_config(const FbSettings &extra = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "enable_support", "0" },
        { "support_fins", "auto" },
        { "elefant_foot_compensation", "0" },
    });
    for (const auto &kv : extra)
        config.set_deserialize_strict(kv.first, kv.second);
    return config;
}

struct FbPrint
{
    Model model;
    Print print;
    const PrintObject &po() const { return *print.objects().front(); }
};

ModelObject *fb_add(Model &model, FbPart part, double rotation_z = 0., const Vec3d &scale = Vec3d::Ones())
{
    ModelObject *object = model.add_object();
    object->name        = fb_name(part);
    object->add_volume(fb_mesh(part));
    ModelInstance *inst = object->add_instance();
    inst->set_rotation(Vec3d(0., 0., rotation_z));
    inst->set_scaling_factor(scale);
    object->ensure_on_bed();
    return object;
}

void fb_process(Print &print, Model &model, const DynamicPrintConfig &config)
{
    for (ModelObject *o : model.objects)
        print.auto_assign_extruders(o);
    print.apply(model, config);
    print.set_status_silent();
    print.process();
}

indexed_triangle_set fb_world(const ModelObject &object, const ModelVolume *only = nullptr)
{
    indexed_triangle_set out;
    for (const ModelVolume *v : object.volumes) {
        if (only != nullptr && v != only)
            continue;
        indexed_triangle_set its = v->mesh().its;
        its_transform(its, object.instances.front()->get_matrix() * v->get_matrix(), true);
        its_merge(out, its);
    }
    return out;
}

SupportFinsMode fb_mode(const ModelObject &object)
{
    const ConfigOption *opt = object.config.option("support_fins");
    return opt == nullptr ? sfmOff : SupportFinsMode(opt->getInt());
}

// The live fins' printed footprint at an object layer (support islands of the support layer there).
ExPolygons fb_live_islands(const PrintObject &po, double print_z)
{
    ExPolygons out;
    for (const SupportLayer *sl : po.support_layers())
        if (std::abs(sl->print_z - print_z) < EPSILON)
            expolygons_append(out, sl->support_islands);
    return union_ex(out);
}

double fb_support_area(const PrintObject &po)
{
    double a = 0.;
    for (const SupportLayer *sl : po.support_layers())
        a += fb_area(sl->support_islands);
    return a;
}

// The worst per-layer symmetric difference of two section stacks, relative to the first, over layers
// whose first section is over 1 mm^2; the summed one in `total`.
double fb_worst_xor(const std::vector<ExPolygons> &want, const std::vector<ExPolygons> &got, double &total, size_t &layers)
{
    double worst = 0., sum_x = 0., sum_a = 0.;
    layers = 0;
    for (size_t i = 0; i < want.size() && i < got.size(); ++i) {
        const double a = fb_area(want[i]);
        const double x = fb_area(diff_ex(want[i], got[i])) + fb_area(diff_ex(got[i], want[i]));
        sum_a += a;
        sum_x += x;
        if (a > 1.) {
            ++layers;
            worst = std::max(worst, x / a);
        }
    }
    total = sum_a > 0. ? sum_x / sum_a : 0.;
    return worst;
}

// Stores `model` as a project and loads it back.
bool fb_round_trip(Model &model, Model &loaded, const char *name)
{
    const boost::filesystem::path tmp_root = edge_test::snorca_tests_dir();
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string  file   = (tmp_root / name).string();
    DynamicPrintConfig config = fb_config();

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

TEST_CASE("Fin bake: the mesh slices back to the live sections", "[SupportFins][FinBake]")
{
    struct Case { FbPart part; const char *first_layer; };
    for (const Case c : { Case{ FbPart::TiltedCube, "0.2" }, Case{ FbPart::TiltedCube, "0.3" }, Case{ FbPart::LeaningPlate, "0.2" },
                          Case{ FbPart::ThinPlate, "0.2" } }) {
        DYNAMIC_SECTION(fb_name(c.part) << ", first layer " << c.first_layer)
        {
            FbPrint p;
            fb_add(p.model, c.part);
            fb_process(p.print, p.model, fb_config({ { "initial_layer_print_height", c.first_layer } }));
            const PrintObject &po = p.po();

            FinBakeOptions opts;
            opts.tines = FinBakeTines::OneLine;
            const FinBakeResult res = plan_fin_bake(po, opts);
            REQUIRE(res.error.empty());
            REQUIRE(! res.plan.fins.empty());
            CHECK(res.tines > 0);
            // The default 0.2 mm gap is above the part's minimum: the very sections the live fins print.
            CHECK(res.gap == res.sliced_gap);

            fins::FinMeshReport        mrep;
            const indexed_triangle_set mesh = fins::fin_mesh(res.bands, {}, &mrep);
            CHECK(mrep.unioned);
            CHECK(mrep.closed);
            CHECK(mrep.dropped == 0);
            CHECK(its_num_open_edges(mesh) == 0);

            std::vector<float> zs;
            for (const Layer *l : po.layers())
                zs.push_back(float(l->print_z - 0.5 * l->height));
            const std::vector<ExPolygons> baked = slice_mesh_ex(mesh, zs);
            std::vector<ExPolygons> planned, live;
            for (size_t i = 0; i < po.layers().size(); ++i) {
                ExPolygons s = res.slices[i].body;
                expolygons_append(s, res.slices[i].tine_areas);
                planned.emplace_back(union_ex(s));
                live.emplace_back(fb_live_islands(po, po.layers()[i]->print_z));
            }
            double total = 0.;
            size_t layers = 0;
            CHECK(fb_worst_xor(planned, baked, total, layers) < 0.02);
            CHECK(total < 0.01);
            CHECK(layers > 5);
            // What the live generator printed: the same, less specks too thin for a line.
            CHECK(fb_worst_xor(live, baked, total, layers) < 0.05);
            CHECK(total < 0.01);

            // Every tine is in the mesh, on its own layer only.
            for (size_t i = 0; i < res.slices.size(); ++i)
                if (! res.slices[i].tine_areas.empty()) {
                    INFO("tine layer at z " << po.layers()[i]->print_z);
                    CHECK(fb_area(diff_ex(res.slices[i].tine_areas, baked[i])) < 0.01);
                }
        }
    }
}

TEST_CASE("Fin bake: the wide tines cover the live tines", "[SupportFins][FinBake]")
{
    FbPrint p;
    fb_add(p.model, FbPart::TiltedCube);
    fb_process(p.print, p.model, fb_config());
    const FinBakeResult res = plan_fin_bake(p.po(), FinBakeOptions{});
    REQUIRE(res.error.empty());
    CHECK(std::abs(res.tine_width - FIN_BAKE_WIDE_TINE_LINES * fins::settings_of(p.po()).line_width) < 1e-6);
    size_t tine_layers = 0;
    for (size_t i = 0; i < res.slices.size(); ++i)
        if (! res.slices[i].tine_areas.empty()) {
            ++tine_layers;
            CHECK(fb_area(diff_ex(res.slices[i].tine_areas, res.bands[i].sections)) < 0.01);
        }
    CHECK(tine_layers > 2);
}

TEST_CASE("Fin bake as a part of the object: the tines fuse and the walls stay apart", "[SupportFins][FinBake]")
{
    struct Case { FbPart part; FinBakeTines tines; const char *walls; };
    for (const Case c : { Case{ FbPart::TiltedCube, FinBakeTines::Wide, "classic" }, Case{ FbPart::TiltedCube, FinBakeTines::Wide, "arachne" },
                          Case{ FbPart::TiltedCube, FinBakeTines::OneLine, "arachne" }, Case{ FbPart::ThinPlate, FinBakeTines::Wide, "classic" } }) {
        DYNAMIC_SECTION(fb_name(c.part) << ", " << (c.tines == FinBakeTines::Wide ? "wide" : "one-line") << " tines, " << c.walls << " walls")
        {
            const DynamicPrintConfig config = fb_config({ { "wall_generator", c.walls } });
            FbPrint      p;
            ModelObject *src = fb_add(p.model, c.part);
            fb_process(p.print, p.model, config);
            const PrintObject &po = p.po();
            REQUIRE(fb_support_area(po) > 1.);

            FinBakeOptions opts;
            opts.tines = c.tines;
            const FinBakeResult res = bake_fins(po, opts);
            REQUIRE(res.error.empty());
            CHECK(res.mesh_report.closed);
            // Kept from the first slice, in its print frame.
            std::vector<ExPolygons> part;
            std::vector<double>     zs;
            for (const Layer *l : po.layers()) {
                part.push_back(l->lslices);
                zs.push_back(l->print_z);
            }
            const Point shift0 = po.instances().front().shift;

            REQUIRE(apply_fin_bake(p.model, *src, res, opts) == src);
            REQUIRE(p.model.objects.size() == 1);
            REQUIRE(src->volumes.size() == 2);
            CHECK(src->volumes.back()->is_model_part());
            CHECK(src->volumes.back()->name == FIN_BAKE_PART_NAME);
            // No settings of its own, so it shares the part's region.
            CHECK(src->volumes.back()->config.empty());
            CHECK(fb_mode(*src) == sfmOff);

            Print again;
            fb_process(again, p.model, config);
            REQUIRE(again.objects().size() == 1);
            const PrintObject &po2 = *again.objects().front();
            CHECK(po2.num_printing_regions() == 1);
            // The live fins are off: nothing prints as support.
            CHECK(po2.support_layers().empty());
            REQUIRE(po2.layers().size() == zs.size());
            const Point d = shift0 - po2.instances().front().shift;

            std::vector<ExPolygons> want, got;
            for (size_t i = 0; i < zs.size(); ++i) {
                CHECK(std::abs(po2.layers()[i]->print_z - zs[i]) < EPSILON);
                ExPolygons w = part[i];
                expolygons_append(w, res.bands[i].sections);
                w = union_ex(w);
                for (ExPolygon &e : w)
                    e.translate(d);
                want.push_back(std::move(w));
                got.push_back(po2.layers()[i]->lslices);
            }
            double total  = 0.;
            size_t layers = 0;
            CHECK(fb_worst_xor(want, got, total, layers) < 0.02);
            CHECK(total < 0.01);

            size_t fused = 0;
            for (size_t i = 1; i < zs.size(); ++i)
                for (const Polyline &tine : res.slices[i].tines) {
                    // The middle of the tine's gap: the tine runs from half a line inside the wall to
                    // tine_depth inside the part.
                    const Vec2d a = unscaled(tine.first_point()), b = unscaled(tine.last_point());
                    const Vec2d u = (b - a).normalized();
                    const fins::FinSettings st = fins::settings_of(po);
                    const Vec2d  part_edge = b + u * (-st.tine_depth);
                    const Vec2d  mid       = part_edge - u * (0.5 * res.gap);
                    Point        m(scaled(mid.x()), scaled(mid.y()));
                    m += d;
                    INFO("tine at z " << zs[i]);
                    // Fused on the tine layer ...
                    CHECK(fb_inside(got[i], m));
                    // ... and apart on the layer below (never a tine layer), where the gap survived.
                    CHECK(! fb_inside(got[i - 1], m));
                    // An extrusion crosses the gap: the slicer printed the tine.
                    Polygon box;
                    const Vec2d pp(-u.y(), u.x());
                    for (const Vec2d &v : { Vec2d(mid - u * 0.05 - pp * (0.5 * res.tine_width)), Vec2d(mid + u * 0.05 - pp * (0.5 * res.tine_width)),
                                            Vec2d(mid + u * 0.05 + pp * (0.5 * res.tine_width)), Vec2d(mid - u * 0.05 + pp * (0.5 * res.tine_width)) })
                        box.points.emplace_back(Point(scaled(v.x()), scaled(v.y())) + d);
                    if (box.is_clockwise())
                        box.reverse();
                    Polylines paths;
                    for (const LayerRegion *r : po2.layers()[i]->regions()) {
                        r->perimeters.collect_polylines(paths);
                        r->fills.collect_polylines(paths);
                    }
                    const bool crossed = ! intersection_pl(paths, Polygons{ box }).empty();
                    CHECK(crossed);
                    if (crossed)
                        ++fused;
                }
            CHECK(fused > 0);
        }
    }
}

TEST_CASE("Fin bake as a separate object", "[SupportFins][FinBake]")
{
    // A rotated, stretched part as well: the bake must land where its live fins stand.
    for (const auto &[rot, scale] : std::vector<std::pair<double, Vec3d>>{ { 0., Vec3d::Ones() }, { 0.5, Vec3d(1.3, 1., 1.) } }) {
        DYNAMIC_SECTION("rotation " << rot << ", x scale " << scale.x())
        {
            const DynamicPrintConfig config = fb_config();
            FbPrint      p;
            ModelObject *src = fb_add(p.model, FbPart::TiltedCube, rot, scale);
            src->instances.front()->set_offset(Vec3d(40., 30., src->instances.front()->get_offset().z()));
            fb_process(p.print, p.model, config);
            const PrintObject &po = p.po();

            FinBakeOptions opts;
            opts.placement = FinBakePlacement::SeparateObject;
            const FinBakeResult res = bake_fins(po, opts);
            REQUIRE(res.error.empty());
            CHECK(res.mesh_report.closed);
            CHECK(res.gap == res.sliced_gap);
            // The same fins as a part, to compare in the world.
            FinBakeOptions part_opts;
            const FinBakeResult as_part = bake_fins(po, part_opts);
            REQUIRE(as_part.error.empty());

            ModelObject *fins_obj = apply_fin_bake(p.model, *src, res, opts);
            REQUIRE(fins_obj != nullptr);
            REQUIRE(p.model.objects.size() == 2);
            CHECK(fins_obj->name == "tilted_cube fins");
            CHECK(fins_obj->instances.size() == 1);
            CHECK(fb_mode(*src) == sfmOff);
            CHECK(fb_mode(*fins_obj) == sfmOff);
            CHECK(fins_obj->config.option("enable_support")->getBool() == false);
            CHECK(its_num_open_edges(fins_obj->volumes.front()->mesh().its) == 0);

            const indexed_triangle_set part_world = fb_world(*src);
            const indexed_triangle_set fins_world = fb_world(*fins_obj);
            // The part-placement fins (in the object's own frame), in the world through the source's instance.
            indexed_triangle_set part_fins_world = as_part.mesh;
            its_transform(part_fins_world, src->instances.front()->get_matrix(), true);

            std::vector<float> zs;
            for (const Layer *l : po.layers())
                zs.push_back(float(l->print_z - 0.5 * l->height));
            const std::vector<ExPolygons> pp = slice_mesh_ex(part_world, zs), ff = slice_mesh_ex(fins_world, zs),
                                          pf = slice_mesh_ex(part_fins_world, zs);
            double fins_area = 0., overlap = 0., touching_tines = 0.;
            std::vector<ExPolygons> want, got;
            for (size_t i = 0; i < zs.size(); ++i) {
                fins_area += fb_area(ff[i]);
                // Never inside the part: the tines stop at its outline.
                overlap += fb_area(intersection_ex(pp[i], ff[i]));
                if (! res.slices[i].tines.empty())
                    touching_tines += fb_area(intersection_ex(pp[i], offset_ex(ff[i], scaled<float>(0.05))));
                // Where the part's fins stand, less what of their tines is inside the part.
                want.push_back(diff_ex(pf[i], pp[i]));
                got.push_back(ff[i]);
            }
            CHECK(fins_area > 10.);
            CHECK(overlap < 0.05);
            CHECK(touching_tines > 0.);
            double total  = 0.;
            size_t layers = 0;
            CHECK(fb_worst_xor(want, got, total, layers) < 0.05);
            CHECK(total < 0.02);

            // Sliced again: the source prints no fins of its own, the fin object stands on the bed.
            Print again;
            fb_process(again, p.model, config);
            REQUIRE(again.objects().size() == 2);
            const PrintObject *src_po = nullptr, *fin_po = nullptr;
            for (const PrintObject *o : again.objects())
                (o->model_object()->name == "tilted_cube" ? src_po : fin_po) = o;
            REQUIRE(src_po != nullptr);
            REQUIRE(fin_po != nullptr);
            CHECK(fb_support_area(*src_po) < 0.01);
            CHECK(fin_po->support_layers().empty());
            REQUIRE_FALSE(fin_po->layers().empty());
            CHECK(fb_area(fin_po->layers().front()->lslices) > 1.);
        }
    }
}

TEST_CASE("Baked fins survive a 3MF round trip", "[SupportFins][FinBake][3mf]")
{
    for (int placement = 0; placement < 2; ++placement) {
        DYNAMIC_SECTION((placement == 0 ? "as a part" : "as a separate object"))
        {
            FbPrint      p;
            ModelObject *src = fb_add(p.model, FbPart::ThinPlate);
            fb_process(p.print, p.model, fb_config());
            FinBakeOptions opts;
            opts.placement = placement == 0 ? FinBakePlacement::PartOfObject : FinBakePlacement::SeparateObject;
            const FinBakeResult res = bake_fins(p.po(), opts);
            REQUIRE(res.error.empty());
            ModelObject *target = apply_fin_bake(p.model, *src, res, opts);
            REQUIRE(target != nullptr);
            const ModelVolume  *baked     = placement == 0 ? src->volumes.back() : target->volumes.front();
            const size_t        triangles = baked->mesh().its.indices.size();
            const BoundingBoxf3 before    = bounding_box(placement == 0 ? fb_world(*src, baked) : fb_world(*target));

            Model loaded;
            REQUIRE(fb_round_trip(p.model, loaded, placement == 0 ? "fin_bake_part.3mf" : "fin_bake_object.3mf"));
            REQUIRE(loaded.objects.size() == size_t(placement == 0 ? 1 : 2));
            const ModelObject &lsrc = *loaded.objects.front();
            CHECK(lsrc.name == "tilted_thin_plate");
            CHECK(fb_mode(lsrc) == sfmOff);
            const ModelVolume *lbaked = nullptr;
            if (placement == 0) {
                REQUIRE(lsrc.volumes.size() == 2);
                lbaked = lsrc.volumes[1];
                CHECK(lbaked->is_model_part());
                CHECK(lbaked->name == FIN_BAKE_PART_NAME);
            } else {
                const ModelObject &lfins = *loaded.objects[1];
                CHECK(lfins.name == "tilted_thin_plate fins");
                CHECK(fb_mode(lfins) == sfmOff);
                CHECK(lfins.config.option("enable_support")->getBool() == false);
                CHECK(std::abs(lfins.config.opt_float("layer_height") - 0.2) < 1e-9);
                lbaked = lfins.volumes.front();
            }
            CHECK(lbaked->mesh().its.indices.size() == triangles);
            CHECK(its_num_open_edges(lbaked->mesh().its) == 0);
            // Where it stood: the loader may recentre the mesh, so compare the world boxes.
            const BoundingBoxf3 after = bounding_box(placement == 0 ? fb_world(lsrc, lbaked) : fb_world(*loaded.objects[1]));
            CHECK((before.min - after.min).norm() < 1e-3);
            CHECK((before.max - after.max).norm() < 1e-3);
        }
    }
}

TEST_CASE("Fin bake: the part's minimum gap and the refusals", "[SupportFins][FinBake]")
{
    SECTION("a fin gap under the part's minimum is raised for a part only")
    {
        FbPrint p;
        fb_add(p.model, FbPart::TiltedCube);
        fb_process(p.print, p.model, fb_config({ { "support_fin_gap", "0.1" } }));
        FinBakeOptions opts;
        const FinBakeResult part = plan_fin_bake(p.po(), opts);
        REQUIRE(part.error.empty());
        CHECK(std::abs(part.sliced_gap - 0.1) < 1e-9);
        CHECK(std::abs(part.gap - FIN_BAKE_PART_MIN_GAP) < 1e-9);
        opts.placement = FinBakePlacement::SeparateObject;
        const FinBakeResult obj = plan_fin_bake(p.po(), opts);
        REQUIRE(obj.error.empty());
        CHECK(std::abs(obj.gap - 0.1) < 1e-9);
        // The wider gap keeps the walls further off the part on every layer.
        for (size_t i = 0; i < part.slices.size(); ++i) {
            const ExPolygons &isl = p.po().layers()[i]->lslices;
            CHECK(fb_area(intersection_ex(part.slices[i].body, offset_ex(isl, scaled<float>(FIN_BAKE_PART_MIN_GAP - 0.01)))) < 0.01);
        }
    }
    SECTION("fins off")
    {
        FbPrint p;
        fb_add(p.model, FbPart::TiltedCube);
        fb_process(p.print, p.model, fb_config({ { "support_fins", "off" } }));
        const FinBakeResult res = bake_fins(p.po(), {});
        CHECK(res.mesh.indices.empty());
        CHECK(res.error.find("off") != std::string::npos);
    }
    SECTION("a raft")
    {
        FbPrint p;
        fb_add(p.model, FbPart::TiltedCube);
        fb_process(p.print, p.model, fb_config({ { "raft_layers", "2" } }));
        const FinBakeResult res = bake_fins(p.po(), {});
        CHECK(res.mesh.indices.empty());
        CHECK(res.error.find("raft") != std::string::npos);
    }
    SECTION("painted mode without paint")
    {
        FbPrint p;
        fb_add(p.model, FbPart::TiltedCube);
        fb_process(p.print, p.model, fb_config({ { "support_fins", "painted" } }));
        const FinBakeResult res = bake_fins(p.po(), {});
        CHECK(res.mesh.indices.empty());
        CHECK(res.error.find("painted") != std::string::npos);
    }
    SECTION("an empty result builds no model geometry")
    {
        Model        model;
        ModelObject *src = fb_add(model, FbPart::TiltedCube);
        CHECK(apply_fin_bake(model, *src, FinBakeResult{}, FinBakeOptions{}) == nullptr);
        CHECK(src->volumes.size() == 1);
    }
}

// Fins win the space (owner, 2026-11-27): with normal or tree support under the part's lean side, the live
// fins are planned as the bake plans them - nothing dropped - and the support is cut back from them by the
// fin gap, so no support extrusion comes within the gap of a fin, on any support layer whose Z range
// overlaps a fin layer.
TEST_CASE("Support fins displace normal and tree support on the lean side", "[SupportFins][FinBake]")
{
    struct Case { const char *type; bool independent; };
    for (const Case c : { Case{ "normal(auto)", false }, Case{ "tree(auto)", false }, Case{ "normal(auto)", true } }) {
        DYNAMIC_SECTION(c.type << (c.independent ? ", independent support layer height" : ""))
        {
            FbSettings extra = { { "enable_support", "1" }, { "support_type", c.type }, { "support_threshold_angle", "50" } };
            if (c.independent)
                extra.insert(extra.end(), { { "independent_support_layer_height", "1" }, { "support_top_z_distance", "0.15" },
                                            { "support_bottom_z_distance", "0.15" } });
            FbPrint p;
            fb_add(p.model, FbPart::TiltedCube);
            fb_process(p.print, p.model, fb_config(extra));
            const PrintObject &po = p.po();

            // What the fins are with nothing in the way: the bake's plan, one-line tines as live.
            FinBakeOptions opts;
            opts.tines = FinBakeTines::OneLine;
            const FinBakeResult plan = plan_fin_bake(po, opts);
            REQUIRE(plan.error.empty());
            REQUIRE(plan.tines > 0);
            const fins::FinSettings st = fins::settings_of(po);

            // Nothing dropped: no warning, and every planned tine printed as its own two-point path on its
            // object layer.
            bool warned = false;
            for (const PrintStateBase::Warning &w : po.step_state_with_warnings(posSupportMaterial).warnings)
                warned |= w.message_id == PrintStateBase::SlicingFinsTinesSkipped;
            CHECK(! warned);
            auto same = [](const Point &a, const Point &b) { return (a - b).cast<double>().norm() < 10.; };
            size_t found = 0;
            for (size_t i = 0; i < plan.slices.size(); ++i)
                for (const Polyline &t : plan.slices[i].tines) {
                    bool hit = false;
                    for (const SupportLayer *sl : po.support_layers()) {
                        if (std::abs(sl->print_z - po.layers()[i]->print_z) > EPSILON)
                            continue;
                        Polylines pls;
                        sl->support_fills.collect_polylines(pls);
                        for (const Polyline &pl : pls)
                            if (pl.points.size() == 2 && ((same(pl.points[0], t.points[0]) && same(pl.points[1], t.points[1])) ||
                                                          (same(pl.points[0], t.points[1]) && same(pl.points[1], t.points[0]))))
                                hit = true;
                    }
                    found += hit ? 1 : 0;
                }
            CHECK(found == plan.tines);
            // The fin walls are printed whole: each object layer's support islands hold its planned walls.
            for (size_t i = 0; i < plan.slices.size(); ++i) {
                if (plan.slices[i].body.empty())
                    continue;
                CHECK(fb_area(diff_ex(plan.slices[i].body, fb_live_islands(po, po.layers()[i]->print_z))) < 0.05);
            }

            // The support is cut back: no support extrusion within the gap of the fins' footprint.
            double other_support = 0.;
            size_t overlapping   = 0;
            for (const SupportLayer *sl : po.support_layers()) {
                const double top = sl->print_z, bottom = support_layer_util::support_layer_bottom(*sl);
                ExPolygons   f;
                for (size_t i = 0; i < plan.slices.size(); ++i) {
                    const Layer &l = *po.layers()[i];
                    if (l.print_z <= bottom + EPSILON || l.print_z - l.height >= top - EPSILON)
                        continue;
                    expolygons_append(f, plan.slices[i].body);
                    expolygons_append(f, plan.slices[i].tine_areas);
                }
                ExPolygons islands = sl->support_islands;
                expolygons_append(islands, sl->tree_support_areas());
                f = union_ex(f);
                other_support += fb_area(diff_ex(islands, offset_ex(f, scaled<float>(0.05))));
                if (f.empty())
                    continue;
                // The ring the gap leaves round the fins: only a support extrusion can be in it (the fins'
                // own loops run inside their walls and the tines inside their footprints).
                const ExPolygons ring = diff_ex(offset_ex(f, scaled<float>(st.gap)), offset_ex(f, scaled<float>(0.01)));
                Polylines        pls;
                sl->support_fills.collect_polylines(pls);
                const Polylines in_ring = intersection_pl(pls, ring);
                double len = 0.;
                for (const Polyline &pl : in_ring)
                    len += unscaled(pl.length());
                if (len > 0.05) {
                    ++overlapping;
                    WARN("support in the fin gap at z " << sl->print_z << ": " << len << " mm");
                }
            }
            CHECK(overlapping == 0);
            // There is support besides the fins, on the lean side too: the case is real.
            CHECK(other_support > 20.);
        }
    }
}
