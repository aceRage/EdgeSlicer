#include <catch2/catch.hpp>

// SVG (and text) volumes that follow a curved surface as a part, a modifier or a cut
// (libslic3r/EmbossPlacement.hpp, the shell EmbossJob builds from the surface cut), and the
// Showcase surface colours a modifier on a filament of its own gives the part it covers
// (libslic3r/Showcase/ShowcaseSurfaceColours.hpp). Also the 3MF round trip of such a modifier.

#include <libslic3r/AABBMesh.hpp>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/CutSurface.hpp>
#include <libslic3r/Emboss.hpp>
#include <libslic3r/EmbossPlacement.hpp>
#include <libslic3r/EmbossShape.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/NSVGUtils.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Semver.hpp>
#include <libslic3r/Showcase/ShowcaseSurfaceColours.hpp>
#include <libslic3r/SvgSplit.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>
#include <libslic3r/Utils.hpp>

#include <boost/filesystem.hpp>

#include <cmath>
#include <cstdlib>
#include <string>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double RADIUS = 30.;
constexpr double LENGTH = 80.;

// Axis along X, top line at z = 0 (the surface a shape above it is projected down onto).
indexed_triangle_set lying_cylinder()
{
    indexed_triangle_set its = its_make_cylinder(RADIUS, LENGTH, PI / 90.);
    its_translate(its, Vec3f(0.f, 0.f, -float(LENGTH / 2.)));
    its_transform(its, Transform3d(Eigen::AngleAxisd(PI / 2., Vec3d::UnitY())));
    its_translate(its, Vec3f(0.f, 0.f, -float(RADIUS)));
    return its;
}

// Rectangle centred on the origin, mm -> scaled (shape scale SCALING_FACTOR, as an SVG has it)
ExPolygons rectangle(double size_x, double size_y)
{
    Polygon p({ Point(scale_(-size_x / 2.), scale_(-size_y / 2.)), Point(scale_(size_x / 2.), scale_(-size_y / 2.)),
                Point(scale_(size_x / 2.), scale_(size_y / 2.)), Point(scale_(-size_x / 2.), scale_(size_y / 2.)) });
    return { ExPolygon(p) };
}

// The volume EmbossJob makes with "Use surface": the shapes cut out of the surface below (looking
// down -Z), moved into the shell of the volume type. Same calls as cut_surface_to_its().
indexed_triangle_set wrapped_shell(const ExPolygons &shapes, const indexed_triangle_set &mesh, bool raised, float depth)
{
    const double  safe    = 1.;
    BoundingBoxf3 mesh_bb = bounding_box(mesh);
    const double  min_z   = mesh_bb.min.z() - safe;
    const double  max_z   = mesh_bb.max.z() + safe;
    Transform3d   tr      = Transform3d::Identity();
    tr.translate(Vec3d(0., 0., min_z));
    tr.scale(SCALING_FACTOR);
    Emboss::OrthoProject projection(tr, Vec3d(0., 0., max_z - min_z));
    const float ratio = float((-mesh_bb.min.z() + safe) / (mesh_bb.max.z() - mesh_bb.min.z() + 2. * safe));
    indexed_triangle_set aoi = its_cut_AoI(mesh, get_extents(shapes), projection);
    REQUIRE_FALSE(aoi.indices.empty());
    SurfaceCut cut = cut_surface(shapes, { aoi }, projection, ratio);
    REQUIRE_FALSE(cut.empty());
    const Emboss::SurfaceShell shell = Emboss::surface_shell(raised, depth);
    for (stl_vertex &v : cut.vertices)
        v.z() += shell.front;
    Emboss::OrthoProject3d emboss(Vec3d(0., 0., double(shell.back - shell.front)));
    return cut2model(cut, emboss);
}

// Height of the (faceted) mesh surface under (x, y)
double surface_z(const AABBMesh &mesh, double x, double y)
{
    const AABBMesh::hit_result hit = mesh.query_ray_hit(Vec3d(x, y, 10.), Vec3d(0., 0., -1.));
    REQUIRE(hit.is_hit());
    return hit.position().z();
}

double area(const indexed_triangle_set &its)
{
    double a = 0.;
    for (const stl_triangle_vertex_indices &t : its.indices) {
        const Vec3d p0 = its.vertices[size_t(t(0))].cast<double>(), p1 = its.vertices[size_t(t(1))].cast<double>(),
                    p2 = its.vertices[size_t(t(2))].cast<double>();
        a += 0.5 * (p1 - p0).cross(p2 - p0).norm();
    }
    return a;
}

// Area of the triangles facing up, seen from above
double area_seen_from_above(const indexed_triangle_set &its)
{
    double a = 0.;
    for (const stl_triangle_vertex_indices &t : its.indices) {
        const Vec3d p0 = its.vertices[size_t(t(0))].cast<double>(), p1 = its.vertices[size_t(t(1))].cast<double>(),
                    p2 = its.vertices[size_t(t(2))].cast<double>();
        const double z = 0.5 * (p1 - p0).cross(p2 - p0).z();
        if (z > 0.)
            a += z;
    }
    return a;
}

Showcase::ColourRegion volume_region(const indexed_triangle_set &mesh, int filament, bool override_paint = false)
{
    Showcase::ColourRegion r;
    r.kind           = Showcase::ColourRegion::Kind::Volume;
    r.filament       = filament;
    r.override_paint = override_paint;
    r.mesh           = mesh;
    return r;
}

double total_area(const std::vector<indexed_triangle_set> &per_filament)
{
    double a = 0.;
    for (const indexed_triangle_set &its : per_filament)
        a += area(its);
    return a;
}

std::string temp_3mf(const std::string &name)
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(dir);
    set_temporary_dir(dir.string());
    return (dir / name).string();
}

const char *STAR_SVG =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="40mm" height="30mm" viewBox="0 0 40 30">)"
    R"(<path fill="#F2C200" d="M20 1 L24.7 10.5 L35 11.6 L27.3 18.6 L29.4 29 L20 23.8 L10.6 29 L12.7 18.6 L5 11.6 L15.3 10.5 Z"/>)"
    R"(</svg>)";

} // namespace

TEST_CASE("Embossed volumes sit on the side of the surface their type asks for", "[SvgSurface]")
{
    using namespace Slic3r::Emboss;
    const SurfaceShell raised = surface_shell(true, 2.f);
    CHECK(raised.front == 2.f);
    CHECK(raised.back == -SURFACE_SAFE_OFFSET);
    const SurfaceShell sunk = surface_shell(false, 2.f);
    CHECK(sunk.front == SURFACE_SAFE_OFFSET);
    CHECK(sunk.back == -2.f);
    // flat: the back face of a part is a hair under the plane, of a modifier `depth` under it
    CHECK(flat_back_offset(true, 2.f) == -SURFACE_SAFE_OFFSET);
    CHECK_THAT(flat_back_offset(false, 2.f), WithinAbs(SURFACE_SAFE_OFFSET - 2.f, 1e-6));

    CHECK(is_raised(ModelVolumeType::MODEL_PART));
    CHECK_FALSE(is_raised(ModelVolumeType::PARAMETER_MODIFIER));
    CHECK_FALSE(is_raised(ModelVolumeType::NEGATIVE_VOLUME));
    // part <-> modifier / cut moves the volume to the other side: its mesh is made again
    CHECK(type_change_needs_new_mesh(ModelVolumeType::MODEL_PART, ModelVolumeType::PARAMETER_MODIFIER));
    CHECK(type_change_needs_new_mesh(ModelVolumeType::NEGATIVE_VOLUME, ModelVolumeType::MODEL_PART));
    CHECK_FALSE(type_change_needs_new_mesh(ModelVolumeType::PARAMETER_MODIFIER, ModelVolumeType::NEGATIVE_VOLUME));
    CHECK_FALSE(type_change_needs_new_mesh(ModelVolumeType::MODEL_PART, ModelVolumeType::MODEL_PART));
}

TEST_CASE("An SVG modifier with Use surface wraps a cylinder", "[SvgSurface]")
{
    const indexed_triangle_set cylinder = lying_cylinder();
    const AABBMesh             cyl(cylinder);
    const ExPolygons           shape = rectangle(20., 36.); // 36 mm across the curve: its sides drop ~6 mm
    const float                depth = 1.5f;

    for (bool raised : { false, true }) {
        CAPTURE(raised);
        const indexed_triangle_set shell = wrapped_shell(shape, cylinder, raised, depth);
        REQUIRE_FALSE(shell.indices.empty());
        CHECK(its_num_open_edges(shell) == 0);
        // every point of the shell is within its band round the surface, measured along the projection
        const Emboss::SurfaceShell band = Emboss::surface_shell(raised, depth);
        double lowest = 0.;
        for (const stl_vertex &v : shell.vertices) {
            const double dz = double(v.z()) - surface_z(cyl, v.x(), v.y());
            CHECK(dz <= band.front + 1e-3);
            CHECK(dz >= band.back - 1e-3);
            lowest = std::min(lowest, double(v.z()));
        }
        // it really bends: the shell's sides are far below its middle
        CHECK(lowest < -5.);

        if (raised)
            continue;
        // As a modifier it holds the surface it covers, from the middle to the edges of the shape.
        const AABBMesh inside(shell);
        for (double x : { -9., 0., 9. })
            for (double y : { -17., -12., -6., 0., 6., 12., 17. }) {
                const double z = surface_z(cyl, x, y);
                CAPTURE(x, y, z);
                CHECK(Showcase::point_inside_closed_mesh(inside, Vec3d(x, y, z - 0.05)));
                CHECK(Showcase::point_inside_closed_mesh(inside, Vec3d(x, y, z - double(depth) + 0.05)));
                CHECK_FALSE(Showcase::point_inside_closed_mesh(inside, Vec3d(x, y, z + 0.1)));
                CHECK_FALSE(Showcase::point_inside_closed_mesh(inside, Vec3d(x, y, z - double(depth) - 0.1)));
            }
        // A flat modifier of the same depth (no Use surface) cannot reach the sides: the surface at
        // the edge of the shape is far below its back face.
        CHECK(surface_z(cyl, 0., 17.) < Emboss::flat_back_offset(false, depth));
    }
}

TEST_CASE("Showcase colours the surface a filament modifier covers", "[SvgSurface][Showcase]")
{
    const TriangleMesh         part(lying_cylinder());
    const double               part_area = area(part.its);
    const indexed_triangle_set shell     = wrapped_shell(rectangle(20., 36.), part.its, false, 1.5f);
    // the curved area of the 20 x 36 footprint on a r = 30 cylinder
    const double half_angle = std::asin(18. / RADIUS);
    const double wrapped    = 20. * 2. * half_angle * RADIUS;

    SECTION("no paint: the covered surface turns to the modifier's filament") {
        std::vector<indexed_triangle_set> per;
        Showcase::surface_colour_facets(part, nullptr, { volume_region(shell, 2) }, 0.1f, per);
        REQUIRE(per.size() == 3);
        CHECK(per[1].indices.empty());
        // seen from above it is the footprint (the edge loses at most one 0.1 mm strip)
        CHECK_THAT(area_seen_from_above(per[2]), WithinRel(20. * 36., 0.03));
        CHECK_THAT(area(per[2]), WithinRel(wrapped, 0.03));
        // nothing lost, nothing doubled
        CHECK_THAT(total_area(per), WithinRel(part_area, 1e-4));
        // the pieces meet without cracks: together they are as closed as the part
        indexed_triangle_set all;
        for (const indexed_triangle_set &its : per)
            its_merge(all, its);
        its_merge_vertices(all);
        CHECK(its_num_open_edges(all) == its_num_open_edges(part.its));
    }

    // Paint everything on filament 3.
    TriangleSelector painted(part);
    for (int i = 0; i < int(part.its.indices.size()); ++i)
        painted.set_facet(i, EnforcerBlockerType(3));
    const TriangleSelector::TriangleSplittingData paint = painted.serialize();

    SECTION("painted colour stays on top of a modifier") {
        std::vector<indexed_triangle_set> per;
        Showcase::surface_colour_facets(part, &paint, { volume_region(shell, 2) }, 0.1f, per);
        REQUIRE(per.size() >= 4);
        CHECK(per[2].indices.empty());
        CHECK_THAT(area(per[3]), WithinRel(part_area, 1e-4));
    }
    SECTION("Override paint: the modifier's filament replaces the paint inside it") {
        std::vector<indexed_triangle_set> per;
        Showcase::surface_colour_facets(part, &paint, { volume_region(shell, 2, true) }, 0.1f, per);
        REQUIRE(per.size() >= 4);
        CHECK_THAT(area(per[2]), WithinRel(wrapped, 0.03));
        CHECK_THAT(area(per[3]), WithinRel(part_area - area(per[2]), 1e-3));
    }
    SECTION("a later modifier wins where two overlap") {
        const indexed_triangle_set left = wrapped_shell(rectangle(20., 36.), part.its, false, 1.5f);
        indexed_triangle_set       right = left;
        its_translate(right, Vec3f(10.f, 0.f, 0.f)); // overlaps the right half of `left`
        std::vector<indexed_triangle_set> per;
        Showcase::surface_colour_facets(part, nullptr, { volume_region(left, 2), volume_region(right, 4) }, 0.1f, per);
        REQUIRE(per.size() == 5);
        CHECK_THAT(area(per[2]), WithinRel(wrapped / 2., 0.04));
        CHECK_THAT(area(per[4]), WithinRel(wrapped, 0.03));
    }
    SECTION("a height range on a filament") {
        Showcase::ColourRegion range;
        range.kind     = Showcase::ColourRegion::Kind::HeightRange;
        range.filament = 2;
        // the lying cylinder's mesh placed with its bottom on the bed
        range.mesh_to_world = Transform3d(Eigen::Translation3d(0., 0., 2. * RADIUS));
        range.z_min         = 10.;
        range.z_max         = 20.;
        std::vector<indexed_triangle_set> per;
        Showcase::surface_colour_facets(part, nullptr, { range }, 0.1f, per);
        REQUIRE(per.size() == 3);
        // a band of the side between heights 10 and 20 (both sides of the axis), length LENGTH
        const double band = 2. * RADIUS * (std::acos((10. - RADIUS) / RADIUS) - std::acos((20. - RADIUS) / RADIUS)) * LENGTH;
        // and the two end caps between those heights
        auto segment = [](double h) { const double d = RADIUS - h; return RADIUS * RADIUS * std::acos(d / RADIUS) - d * std::sqrt(RADIUS * RADIUS - d * d); };
        const double caps = 2. * (segment(20.) - segment(10.));
        CHECK_THAT(area(per[2]), WithinRel(band + caps, 0.03));
    }
}

TEST_CASE("Which volumes colour a part in the Showcase", "[SvgSurface][Showcase]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->add_volume(TriangleMesh(lying_cylinder()), ModelVolumeType::MODEL_PART, false);
    ModelVolume *part = object->volumes.front();
    object->add_instance();
    const Transform3d instance = object->instances.front()->get_matrix();
    const indexed_triangle_set shell = wrapped_shell(rectangle(20., 36.), part->mesh().its, false, 1.5f);

    ModelVolume *speed = object->add_volume(TriangleMesh(shell), ModelVolumeType::PARAMETER_MODIFIER, false);
    speed->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(50));
    CHECK(Showcase::colour_regions_for(*part, instance, 4).empty());

    ModelVolume *svg = object->add_volume(TriangleMesh(shell), ModelVolumeType::PARAMETER_MODIFIER, false);
    svg->config.set_key_value("extruder", new ConfigOptionInt(2));
    const uint64_t key = Showcase::colour_regions_key(*part, instance, 4);
    std::vector<Showcase::ColourRegion> regions = Showcase::colour_regions_for(*part, instance, 4);
    REQUIRE(regions.size() == 1);
    CHECK(regions.front().filament == 2);
    CHECK_FALSE(regions.front().override_paint);
    // one filament: nothing to show; a filament the printer does not have: neither
    CHECK(Showcase::colour_regions_for(*part, instance, 1).empty());
    svg->config.set_key_value("extruder", new ConfigOptionInt(7));
    CHECK(Showcase::colour_regions_for(*part, instance, 4).empty());
    CHECK(Showcase::colour_regions_key(*part, instance, 4) != key);
    // the wall filament is what the outside shows
    svg->config.set_key_value("extruder", new ConfigOptionInt(2));
    svg->config.set_key_value("wall_filament", new ConfigOptionInt(3));
    svg->config.set_key_value("override_paint", new ConfigOptionBool(true));
    regions = Showcase::colour_regions_for(*part, instance, 4);
    REQUIRE(regions.size() == 1);
    CHECK(regions.front().filament == 3);
    CHECK(regions.front().override_paint);

    // the modifier's mesh moved into the part's coordinates
    svg->set_offset(Vec3d(5., 0., 0.));
    regions = Showcase::colour_regions_for(*part, instance, 4);
    REQUIRE(regions.size() == 1);
    CHECK_THAT(bounding_box(regions.front().mesh).center().x(), WithinAbs(bounding_box(shell).center().x() + 5., 1e-3));
    // far away: no region
    svg->set_offset(Vec3d(500., 0., 0.));
    CHECK(Showcase::colour_regions_for(*part, instance, 4).empty());
    // negative volumes and support blockers never colour
    svg->set_offset(Vec3d::Zero());
    svg->set_type(ModelVolumeType::NEGATIVE_VOLUME);
    CHECK(Showcase::colour_regions_for(*part, instance, 4).empty());
    svg->set_type(ModelVolumeType::SUPPORT_BLOCKER);
    CHECK(Showcase::colour_regions_for(*part, instance, 4).empty());

    // a height range on a filament, after the modifiers' place in the order (first)
    svg->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    object->layer_config_ranges[{ 1000., 1010. }].set("extruder", 2);   // above the part: nothing
    object->layer_config_ranges[{ -100., 1000. }].set("extruder", 4);
    regions = Showcase::colour_regions_for(*part, instance, 4);
    REQUIRE(regions.size() == 2);
    CHECK(regions[0].kind == Showcase::ColourRegion::Kind::HeightRange);
    CHECK(regions[0].filament == 4);
    CHECK(regions[1].kind == Showcase::ColourRegion::Kind::Volume);
}

TEST_CASE("Shapes of a split SVG are known by their path", "[SvgSurface]")
{
    CHECK(svg_split_group_of(svg_split_path_in_3mf("00ab12cd34ef5678", 0)) == "00ab12cd34ef5678");
    CHECK(svg_split_group_of(svg_split_path_in_3mf("a_b", 11)) == "a_b");
    CHECK(svg_split_group_of("3D/code_00ab_1.svg").empty());
    CHECK(svg_split_group_of("3D/svgsplit_.svg").empty());
    CHECK(svg_split_group_of("3D/svgsplit_abc_x.svg").empty());
    CHECK(svg_split_group_of("").empty());
}

namespace {

// Standing jar (r 30, h 60) with an SVG star on its side, wrapped as the SVG tool makes it, as a
// modifier on filament 2: what the hand test opens and what the round trip stores.
ModelObject *add_jar_with_svg_modifier(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "jar";
    object->add_volume(TriangleMesh(its_make_cylinder(30., 60., PI / 90.)), ModelVolumeType::MODEL_PART, false)->name = "jar";

    // The SVG tool's placement on the side: local Z out of the surface (+X), local Y up (+Z).
    Transform3d place = Transform3d::Identity();
    place.linear().col(0) = Vec3d(0., 1., 0.);
    place.linear().col(1) = Vec3d(0., 0., 1.);
    place.linear().col(2) = Vec3d(1., 0., 0.);
    place.translation()   = Vec3d(30., 0., 30.);

    EmbossShape es;
    es.projection.depth       = 1.2;
    es.projection.use_surface = true;
    EmbossShape::SvgFile svg;
    svg.path_in_3mf = "3D/jar_star.svg";
    svg.file_data   = std::make_shared<std::string>(STAR_SVG);
    NSVGimage_ptr image = nsvgParse(*svg.file_data);
    REQUIRE(image != nullptr);
    NSVGLineParams params(1e-2 / SCALING_FACTOR / SCALING_FACTOR);
    es.shapes_with_ids = create_shape_with_ids(*image, params);
    REQUIRE_FALSE(es.shapes_with_ids.empty());
    ExPolygons shapes;
    for (const ExPolygonsWithId &s : es.shapes_with_ids)
        append(shapes, s.expoly);
    shapes = union_ex(shapes);
    es.svg_file = std::move(svg);

    // the jar in the shape's coordinates, the surface cut there and sunk into the jar (a modifier)
    indexed_triangle_set jar_local = object->volumes.front()->mesh().its;
    its_transform(jar_local, place.inverse());
    const indexed_triangle_set shell = wrapped_shell(shapes, jar_local, false, float(es.projection.depth));

    ModelVolume *mod = object->add_volume(TriangleMesh(shell), ModelVolumeType::PARAMETER_MODIFIER, false);
    mod->name         = "star";
    mod->set_transformation(Geometry::Transformation(place));
    mod->emboss_shape = es;
    mod->config.set_key_value("extruder", new ConfigOptionInt(2));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();
    return object;
}

bool store_project(Model &model, DynamicPrintConfig &config, const std::string &path)
{
    PlateData plate;
    plate.plate_index = 0;
    StoreParams sp;
    sp.path     = path.c_str();
    sp.model    = &model;
    sp.config   = &config;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    sp.plate_data_list.push_back(&plate);
    return store_bbs_3mf(sp);
}

bool load_project(const std::string &path, Model &model, DynamicPrintConfig &config)
{
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
    PlateDataPtrs             plates;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool loaded = load_bbs_3mf(path.c_str(), &config, &ctxt, &model, &plates, &project_presets, &is_bbl_3mf, &file_version,
                                     nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence);
    release_PlateData_list(plates);
    for (Preset *preset : project_presets)
        delete preset;
    return loaded;
}

} // namespace

TEST_CASE("A wrapped SVG modifier round-trips through 3MF", "[SvgSurface][3mf]")
{
    const std::string path = temp_3mf("svg_surface_modifier.3mf");
    ScopeGuard        cleanup([&path]() {
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
    });
    Model model;
    add_jar_with_svg_modifier(model);
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    // two filaments, or the loader resets filament 2 on the modifier to the default
    cfg.option<ConfigOptionStrings>("filament_colour", true)->values = { "#FFFFFF", "#F2C200" };
    REQUIRE(store_project(model, cfg, path));

    Model              loaded;
    DynamicPrintConfig loaded_cfg;
    REQUIRE(load_project(path, loaded, loaded_cfg));
    REQUIRE(loaded.objects.size() == 1);
    const ModelObject *object = loaded.objects.front();
    REQUIRE(object->volumes.size() == 2);
    const ModelVolume *mod = object->volumes[1];
    CHECK(mod->type() == ModelVolumeType::PARAMETER_MODIFIER);
    REQUIRE(mod->emboss_shape.has_value());
    const EmbossShape &es = *mod->emboss_shape;
    CHECK(es.projection.use_surface);
    CHECK_THAT(es.projection.depth, WithinAbs(1.2, 1e-6)); // stored as a float
    REQUIRE(es.svg_file.has_value());
    CHECK(es.svg_file->path_in_3mf == "3D/jar_star.svg");
    REQUIRE(es.svg_file->file_data != nullptr);
    CHECK(*es.svg_file->file_data == STAR_SVG);
    CHECK(mod->config.opt_int("extruder") == 2);
    // the modifier still covers the same piece of the jar: its mesh comes back where it was
    const ModelVolume *jar = object->volumes[0];
    const Transform3d  inst = object->instances.front()->get_matrix();
    const std::vector<Showcase::ColourRegion> regions = Showcase::colour_regions_for(*jar, inst, 2);
    REQUIRE(regions.size() == 1);
    std::vector<indexed_triangle_set> per;
    Showcase::surface_colour_facets(jar->mesh(), nullptr, regions, 0.1f, per);
    REQUIRE(per.size() == 3);
    // the star's area (about 300 mm2 on a 40 x 30 mm drawing), wrapped onto the side of the jar
    CHECK(area(per[2]) > 200.);
    CHECK(area(per[2]) < 600.);
}

// Writes the owner's hand-test project: SVGSURF_OUT = path of the .3mf, SVGSURF_SRC = a project with
// two or more filaments whose printer and filament settings it takes (optional).
TEST_CASE("Write the SVG on a jar hand-test project", "[.][SvgSurfaceProject]")
{
    const char *out = std::getenv("SVGSURF_OUT");
    REQUIRE(out != nullptr);
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    if (const char *src = std::getenv("SVGSURF_SRC"); src != nullptr) {
        Model source;
        DynamicPrintConfig src_cfg;
        REQUIRE(load_project(src, source, src_cfg));
        cfg.apply(src_cfg, true);
    }
    Model model;
    add_jar_with_svg_modifier(model);
    REQUIRE(store_project(model, cfg, out));
}
