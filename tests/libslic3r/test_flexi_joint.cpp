#include <catch2/catch.hpp>

#include <libslic3r/AABBMesh.hpp>
#include <libslic3r/CutUtils.hpp>
#include <libslic3r/FlexiJoint.hpp>
#include <libslic3r/Format/3mf.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace Slic3r;

// The test article: a 20 mm diameter, 20 mm tall cylinder cut at mid height.
static const double CYL_R = 10.0;
static const double CYL_H = 20.0;
static const double CUT_Z = 10.0;

static FlexiJointParams ring_params()
{
    FlexiJointParams p;
    p.kind         = FlexiJointKind::DoubleRing;
    p.clearance    = 0.35f;
    p.outer_radius = 4.0f;      // = 0.4 x the 10 mm inscribed radius, i.e. the auto default
    p.ring_width   = 2.0f;
    p.ring_height  = 1.5f;
    p.tilt         = 0.20f;
    p.neck_ratio   = 0.45f;
    p.hub_radius   = flexi_default_hub_radius(p);
    return p;
}

static FlexiJointParams ball_params()
{
    FlexiJointParams p;
    p.kind         = FlexiJointKind::BallSocket;
    p.clearance    = 0.35f;
    p.outer_radius = 4.0f;
    p.open_angle   = 40.0f;
    p.tilt         = 0.20f;
    return p;
}

// Build the pre-cut object: one solid cylinder plus one flexi joint connector on the plane.
static ModelObject* make_jointed_cylinder(Model &model, const FlexiJointParams &p)
{
    ModelObject *mo = model.add_object();
    mo->name        = "flexi_cylinder";
    ModelVolume *v  = mo->add_volume(TriangleMesh(its_make_cylinder(CYL_R, CYL_H, 2. * PI / 180.)));
    v->set_type(ModelVolumeType::MODEL_PART);
    v->name = "cyl";

    ModelInstance *inst = mo->add_instance();
    inst->set_transformation(Geometry::Transformation());

    CutConnector connector;
    connector.pos        = Vec3d(0., 0., CUT_Z);
    connector.rotation_m = Transform3d::Identity();
    connector.z_angle    = 0.f;
    connector.radius     = flexi_outer_extent(p);
    connector.height     = flexi_protrusion_height(p);
    connector.attribs    = CutConnectorAttributes(CutConnectorType::FlexiJoint, CutConnectorStyle::Prism, CutConnectorShape::Circle);
    connector.flexi      = p;

    add_flexi_joint_volume(mo, connector, "Flexi joint-1");
    return mo;
}

// The two halves, in object coordinates.
struct CutHalves
{
    TriangleMesh upper;   // the female half (carries the groove / socket)
    TriangleMesh lower;   // the male half   (carries the lip / ball)
    size_t       volumes{ 0 };
    size_t       objects{ 0 };
};

static CutHalves cut_with_joint(const FlexiJointParams &p)
{
    Model model;
    ModelObject *mo = make_jointed_cylinder(model, p);

    Cut cut(mo, 0, Geometry::translation_transform(Vec3d(0., 0., CUT_Z)),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepAsParts);
    const ModelObjectPtrs &res = cut.perform_with_plane();

    CutHalves out;
    out.objects = res.size();
    if (res.empty())
        return out;
    ModelObject *ro = res.front();
    out.volumes     = ro->volumes.size();
    for (const ModelVolume *v : ro->volumes) {
        TriangleMesh m(v->mesh());
        m.transform(v->get_matrix());
        if (v->is_from_upper())
            out.upper = m;
        else
            out.lower = m;
    }
    // res is owned by `cut`; it frees the objects in its destructor.
    return out;
}

// Unsigned minimum distance between the surfaces of two disjoint meshes, sampled at the
// vertices of each mesh against the other's surface. Exact for the flat and cylindrical
// faces the clearance is actually defined on.
static double min_surface_distance(const TriangleMesh &a, const TriangleMesh &b)
{
    AABBMesh tree_b(b);
    AABBMesh tree_a(a);
    double   best = std::numeric_limits<double>::max();
    for (const Vec3f &v : a.its.vertices)
        best = std::min(best, std::sqrt(tree_b.squared_distance(v.cast<double>())));
    for (const Vec3f &v : b.its.vertices)
        best = std::min(best, std::sqrt(tree_a.squared_distance(v.cast<double>())));
    return best;
}

static double intersection_volume(const TriangleMesh &a, const TriangleMesh &b)
{
    std::vector<TriangleMesh> dst;
    if (!MeshBoolean::mfd::make_boolean(a, b, dst, "INTERSECTION")) {
        dst.clear();
        MeshBoolean::mcut::make_boolean(a, b, dst, "INTERSECTION");
    }
    double vol = 0.;
    for (const TriangleMesh &m : dst)
        vol += std::abs(double(its_volume(m.its)));
    return vol;
}

static TriangleMesh rotated_about_joint(const TriangleMesh &m, const Vec3d &axis, double angle)
{
    TriangleMesh out(m);
    const Vec3d c(0., 0., CUT_Z);
    out.transform(Geometry::translation_transform(c) * Geometry::rotation_transform(angle * axis) *
                  Geometry::translation_transform(-c));
    return out;
}

// ------------------------------------------------------------------- the revolve generator

TEST_CASE("its_make_revolved builds watertight rings and lathes", "[FlexiJoint]")
{
    // Ring topology: a square-section ring at mean radius 5.
    std::vector<Vec2d> ring = { {4., -1.}, {6., -1.}, {6., 1.}, {4., 1.} };
    indexed_triangle_set r = its_make_revolved(ring, 64);
    REQUIRE(its_num_open_edges(r) == 0);
    // Pappus: area 2x2 = 4, travelled distance 2*pi*5 -> 40*pi.
    REQUIRE(double(its_volume(r)) == Approx(4. * 2. * PI * 5.).epsilon(0.01));

    // Lathe topology: a half disc of radius 3 revolved into a sphere.
    std::vector<Vec2d> half;
    const int N = 64;
    for (int i = 0; i <= N; ++ i) {
        const double beta = PI * double(i) / double(N);
        half.emplace_back(3. * std::sin(beta), 3. * std::cos(beta));
    }
    indexed_triangle_set s = its_make_revolved(half, 64);
    REQUIRE(its_num_open_edges(s) == 0);
    REQUIRE(double(its_volume(s)) == Approx(4. / 3. * PI * 27.).epsilon(0.01));
}

// ------------------------------------------------------------------------ the joint bodies

TEST_CASE("Flexi joint bodies are watertight and captive", "[FlexiJoint]")
{
    SECTION("double ring") {
        const FlexiJointParams p = ring_params();
        REQUIRE(flexi_validate(p).empty());
        for (const indexed_triangle_set &its : flexi_male_bodies(p)) {
            REQUIRE(its_num_open_edges(its) == 0);
            REQUIRE(its_volume(its) > 0.f);
        }
        for (const indexed_triangle_set &its : flexi_female_cavities(p)) {
            REQUIRE(its_num_open_edges(its) == 0);
            REQUIRE(its_volume(its) > 0.f);
        }
        // The groove's mouth has to be narrower than the lip, or the joint pulls apart.
        REQUIRE(flexi_mouth_half_width(p) < flexi_lip_half_width(p));
        REQUIRE(flexi_is_captive(p));
    }
    SECTION("ball & socket") {
        const FlexiJointParams p = ball_params();
        REQUIRE(flexi_validate(p).empty());
        for (const indexed_triangle_set &its : flexi_male_bodies(p)) {
            REQUIRE(its_num_open_edges(its) == 0);
            REQUIRE(its_volume(its) > 0.f);
        }
        for (const indexed_triangle_set &its : flexi_female_cavities(p)) {
            REQUIRE(its_num_open_edges(its) == 0);
            REQUIRE(its_volume(its) > 0.f);
        }
        // The socket mouth has to be narrower than the ball.
        REQUIRE(flexi_mouth_half_width(p) < p.outer_radius);
        REQUIRE(flexi_is_captive(p));
    }
}

// ---------------------------------------------------------------------------- the full cut

TEST_CASE("Double ring cut of a 20 mm cylinder", "[FlexiJoint]")
{
    const FlexiJointParams p = ring_params();
    const CutHalves h = cut_with_joint(p);

    // One object, two parts - the joint only prints in place if the halves stay together.
    REQUIRE(h.objects == 1);
    REQUIRE(h.volumes == 2);
    REQUIRE_FALSE(h.upper.empty());
    REQUIRE_FALSE(h.lower.empty());

    REQUIRE(its_num_open_edges(h.upper.its) == 0);
    REQUIRE(its_num_open_edges(h.lower.its) == 0);

    // The clearance really is C on the closest faces.
    REQUIRE(min_surface_distance(h.upper, h.lower) == Approx(double(p.clearance)).margin(0.02));

    // The parts do not interpenetrate.
    REQUIRE(intersection_volume(h.upper, h.lower) == Approx(0.).margin(1e-3));

    // The lower half really did grow the lip: it is bigger than a plain half cylinder.
    REQUIRE(double(its_volume(h.lower.its)) > PI * CYL_R * CYL_R * CUT_Z);
    // ... and the upper half lost the groove plus the face gap.
    REQUIRE(double(its_volume(h.upper.its)) < PI * CYL_R * CYL_R * (CYL_H - CUT_Z));
}

TEST_CASE("Double ring joint rotates freely about its axis", "[FlexiJoint]")
{
    const FlexiJointParams p = ring_params();
    const CutHalves h = cut_with_joint(p);
    REQUIRE(h.volumes == 2);

    for (int deg = 15; deg < 360; deg += 15) {
        const TriangleMesh spun = rotated_about_joint(h.lower, Vec3d::UnitZ(), Geometry::deg2rad(double(deg)));
        INFO("rotation " << deg << " deg");
        REQUIRE(min_surface_distance(h.upper, spun) >= double(p.clearance) - 0.02);
        REQUIRE(intersection_volume(h.upper, spun) == Approx(0.).margin(1e-3));
    }
}

TEST_CASE("Double ring joint tilts within its allowance and jams past it", "[FlexiJoint]")
{
    const FlexiJointParams p = ring_params();
    const CutHalves h = cut_with_joint(p);
    REQUIRE(h.volumes == 2);

    // On a full-diameter cut the flat mating faces are what bound the rock:
    // the rim at radius R rises by R*sin(alpha), so alpha_max = asin(C / R).
    const double alpha_max = std::asin(double(p.clearance) / CYL_R);
    const TriangleMesh tilted = rotated_about_joint(h.lower, Vec3d::UnitX(), 0.7 * alpha_max);
    REQUIRE(intersection_volume(h.upper, tilted) == Approx(0.).margin(1e-3));

    // Well past the allowance the halves must actually collide, otherwise the joint is loose.
    const TriangleMesh over = rotated_about_joint(h.lower, Vec3d::UnitX(), 2.0 * alpha_max);
    REQUIRE(intersection_volume(h.upper, over) > 1e-2);
}

TEST_CASE("Ball & socket cut of a 20 mm cylinder", "[FlexiJoint]")
{
    const FlexiJointParams p = ball_params();
    const CutHalves h = cut_with_joint(p);

    REQUIRE(h.objects == 1);
    REQUIRE(h.volumes == 2);
    REQUIRE(its_num_open_edges(h.upper.its) == 0);
    REQUIRE(its_num_open_edges(h.lower.its) == 0);

    REQUIRE(min_surface_distance(h.upper, h.lower) == Approx(double(p.clearance)).margin(0.02));
    REQUIRE(intersection_volume(h.upper, h.lower) == Approx(0.).margin(1e-3));

    // The socket cannot shed the ball.
    REQUIRE(flexi_mouth_half_width(p) < p.outer_radius);

    for (int deg = 30; deg < 360; deg += 30) {
        const TriangleMesh spun = rotated_about_joint(h.lower, Vec3d::UnitZ(), Geometry::deg2rad(double(deg)));
        INFO("rotation " << deg << " deg");
        REQUIRE(min_surface_distance(h.upper, spun) >= double(p.clearance) - 0.02);
    }
}

// ------------------------------------------------------------------------- 3MF round trip

TEST_CASE("A flexi jointed object survives a 3MF round trip", "[FlexiJoint]")
{
    Model model;
    ModelObject *mo = make_jointed_cylinder(model, ring_params());
    Cut cut(mo, 0, Geometry::translation_transform(Vec3d(0., 0., CUT_Z)),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepAsParts);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 1);

    Model out;
    for (ModelObject *o : res)
        out.add_object(*o);
    REQUIRE(out.objects.size() == 1);
    REQUIRE(out.objects.front()->volumes.size() == 2);

    const boost::filesystem::path file = boost::filesystem::temp_directory_path() / "edgeslicer_flexi_roundtrip.3mf";
    REQUIRE(store_3mf(file.string().c_str(), &out, nullptr, false));

    Model back;
    DynamicPrintConfig cfg;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
    REQUIRE(load_3mf(file.string().c_str(), cfg, ctxt, &back, false));
    boost::filesystem::remove(file);

    REQUIRE(back.objects.size() == 1);
    REQUIRE(back.objects.front()->volumes.size() == 2);
    for (const ModelVolume *v : back.objects.front()->volumes) {
        REQUIRE(v->is_model_part());
        REQUIRE(its_num_open_edges(v->mesh().its) == 0);
    }
}

// ------------------------------------------------------------------------------- the guards

TEST_CASE("Flexi joint guards", "[FlexiJoint]")
{
    FlexiJointParams p = ring_params();

    // BambuStudio #182: the slicer's gap closing radius welds print-in-place clearances shut.
    REQUIRE_FALSE(flexi_gap_closing_conflict(p, 0.049));   // the shipped default is safe
    REQUIRE(flexi_gap_closing_conflict(p, 0.2));           // ... a hand-raised one is not
    REQUIRE(flexi_max_safe_gap_closing_radius(p) == Approx(0.175));

    // The clearance floor follows the nozzle.
    REQUIRE(flexi_clearance_floor(0.4) == Approx(0.30));
    REQUIRE(flexi_clearance_floor(0.6) == Approx(0.45));

    // Auto sizing from the cross section: 0.4 x the inscribed radius.
    FlexiJointParams a = flexi_auto_size(p, CYL_R);
    REQUIRE(a.outer_radius == Approx(4.0));
    REQUIRE(a.hub_radius == Approx(flexi_default_hub_radius(a)));

    // A clearance of zero prints as one solid part - refuse it.
    p.clearance = 0.f;
    REQUIRE_FALSE(flexi_validate(p).empty());

    // A neck as wide as the head is not captive.
    p = ring_params();
    p.neck_ratio = 0.94f;
    REQUIRE_FALSE(flexi_is_captive(p));
    REQUIRE_FALSE(flexi_validate(p).empty());
}

// --------------------------------------------------------------------- fixture exporter

// Hidden (Catch2 "[.]" tag: it does not run in the default suite). Writes the two jointed
// cylinders the CLI slicing proofs use. Run it explicitly with the output dir in the
// environment:
//   set SNORCA_FLEXI_OUT=C:	mp && libslic3r_tests.exe "Export flexi joint fixtures"
TEST_CASE("Export flexi joint fixtures", "[.][FlexiJointFixtures]")
{
    const char *dir = std::getenv("SNORCA_FLEXI_OUT");
    REQUIRE(dir != nullptr);

    struct { const char *name; FlexiJointParams p; } cases[] = {
        { "flexi_ring.3mf", ring_params() },
        { "flexi_ball.3mf", ball_params() },
    };
    for (const auto &c : cases) {
        Model model;
        ModelObject *mo = make_jointed_cylinder(model, c.p);
        Cut cut(mo, 0, Geometry::translation_transform(Vec3d(0., 0., CUT_Z)),
                ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepAsParts);
        const ModelObjectPtrs &res = cut.perform_with_plane();
        REQUIRE(res.size() == 1);
        Model out;
        for (ModelObject *o : res)
            out.add_object(*o);
        out.add_default_instances();
        const boost::filesystem::path file = boost::filesystem::path(dir) / c.name;
        REQUIRE(store_3mf(file.string().c_str(), &out, nullptr, false));
        WARN("wrote " << file.string());
    }
}
