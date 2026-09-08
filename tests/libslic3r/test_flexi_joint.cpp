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

#include <cereal/archives/binary.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <set>
#include <sstream>
#include <utility>

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

static FlexiJointParams chain_params()
{
    FlexiJointParams p;
    p.kind        = FlexiJointKind::ChainLink;
    p.clearance   = 0.35f;
    p.gap         = 1.5f;
    p.link_length = 7.0f;
    p.link_width  = 5.5f;
    p.wire        = 1.0f;
    p.tilt_angle  = 7.0f;
    p.stem        = 1.5f;
    return p;
}

// The hinge test article: a 3-knuckle hinge sized for the 20 mm cylinder, parked at the
// cylinder's edge the way the gizmo's auto placement would park it.
static FlexiJointParams hinge_params()
{
    FlexiJointParams p;
    p.kind              = FlexiJointKind::Hinge;
    p.clearance         = 0.35f;
    p.gap               = 0.6f;
    p.hinge_knuckles    = 3;
    p.hinge_pin_dia     = 2.0f;
    p.hinge_barrel_dia  = 4.0f;
    p.hinge_length      = 12.0f;
    // Auto edge placement on a ROUND cut face, exactly as the gizmo computes it: the run's
    // ENDS have to be inside the rim too, not just its middle, so the barrel centreline goes
    // at the chord at half the run's length, less the barrel's own radius. For a 12 mm run in
    // a 20 mm circle that is sqrt(10^2 - 6^2) - 2 = 6 mm out.
    // (less the 0.1 mm bite into the wall that keeps the barrel's outer surface from landing
    // exactly on the part's own face, which would give the boolean two coplanar surfaces)
    p.hinge_edge_offset = float(std::sqrt(CYL_R * CYL_R - std::pow(0.5 * double(p.hinge_length), 2.))) -
                          0.5f * p.hinge_barrel_dia - 0.1f;
    p.hinge_fold_upper  = false;
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
static ModelObject* make_jointed_cylinder(Model &model, const FlexiJointParams &p, float z_angle = 0.f)
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
    connector.z_angle    = z_angle;
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

static CutHalves cut_with_joint(const FlexiJointParams &p, float z_angle = 0.f)
{
    Model model;
    ModelObject *mo = make_jointed_cylinder(model, p, z_angle);

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

// The distance between the two segments' FLAT CUT FACES, i.e. the gap. The lower half's face
// is its topmost vertex on the outer wall, the upper half's its bottommost one there; sampling
// on the wall (r close to CYL_R) keeps the joint bodies, which straddle the middle, out of it.
static double face_to_face_distance(const TriangleMesh &upper, const TriangleMesh &lower)
{
    double lo = -std::numeric_limits<double>::max();
    double hi =  std::numeric_limits<double>::max();
    for (const Vec3f &v : lower.its.vertices)
        if (std::hypot(double(v.x()), double(v.y())) > 0.9 * CYL_R)
            lo = std::max(lo, double(v.z()));
    for (const Vec3f &v : upper.its.vertices)
        if (std::hypot(double(v.x()), double(v.y())) > 0.9 * CYL_R)
            hi = std::min(hi, double(v.z()));
    return hi - lo;
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

    // The clearance really is C on the closest faces (the lip inside its groove).
    REQUIRE(min_surface_distance(h.upper, h.lower) == Approx(double(p.clearance)).margin(0.02));

    // ... and the two flat cut faces are the GAP apart, not the clearance.
    REQUIRE(face_to_face_distance(h.upper, h.lower) == Approx(double(flexi_effective_gap(p))).margin(0.02));

    // The parts do not interpenetrate.
    REQUIRE(intersection_volume(h.upper, h.lower) == Approx(0.).margin(1e-3));

    // The lower half really did grow the lip: it is bigger than a plain half cylinder
    // minus the half gap the cut took off it.
    REQUIRE(double(its_volume(h.lower.its)) > PI * CYL_R * CYL_R * (CUT_Z - double(flexi_effective_gap(p))));
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
    // the rim at radius R rises by R*sin(alpha), so alpha_max = asin(gap / R) - it is the
    // GAP between the faces that the rim has to climb, not the joint's clearance.
    const double alpha_max = std::asin(double(flexi_effective_gap(p)) / CYL_R);
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

// ============================================================ phase 2: the Chain link

// The swept-tube generator the chain link is built from.
TEST_CASE("its_make_swept_loop builds watertight tubes", "[FlexiJoint]")
{
    // A circular path of radius 5 swept with radius 1 is a torus: V = 2*pi^2*R*r^2.
    std::vector<Vec3d> circle;
    const int N = 96;
    for (int i = 0; i < N; ++ i) {
        const double a = 2. * PI * double(i) / double(N);
        circle.emplace_back(5. * std::cos(a), 5. * std::sin(a), 0.);
    }
    const indexed_triangle_set torus = its_make_swept_loop(circle, 1.0, 48);
    REQUIRE(its_num_open_edges(torus) == 0);
    REQUIRE(double(its_volume(torus)) == Approx(2. * PI * PI * 5. * 1. * 1.).epsilon(0.02));

    // A non-planar path still closes: the frame's residual twist is spread over the loop.
    std::vector<Vec3d> wobbly;
    for (int i = 0; i < N; ++ i) {
        const double a = 2. * PI * double(i) / double(N);
        wobbly.emplace_back(6. * std::cos(a), 4. * std::sin(a), 1.5 * std::sin(2. * a));
    }
    const indexed_triangle_set w = its_make_swept_loop(wobbly, 0.8, 32);
    REQUIRE(its_num_open_edges(w) == 0);
    REQUIRE(its_volume(w) > 0.f);

    // Degenerate inputs are refused rather than producing garbage.
    REQUIRE(its_make_swept_loop({ Vec3d(0,0,0), Vec3d(1,0,0) }, 1.0, 16).indices.empty());
    REQUIRE(its_make_swept_loop(circle, 1.0, 2).indices.empty());
}

// The two loops, before the cut: watertight, linked, and clearance-clean.
TEST_CASE("Chain link loops are watertight and interlocked", "[FlexiJoint]")
{
    const FlexiJointParams p = chain_params();
    REQUIRE(flexi_validate(p).empty());

    const std::vector<indexed_triangle_set> lower = flexi_lower_bodies(p);
    const std::vector<indexed_triangle_set> upper = flexi_upper_bodies(p);
    REQUIRE(lower.size() == 1);
    REQUIRE(upper.size() == 1);
    for (const indexed_triangle_set &its : { lower.front(), upper.front() }) {
        REQUIRE(its_num_open_edges(its) == 0);
        REQUIRE(its_volume(its) > 0.f);
    }

    // The loops do not touch, and they clear each other by at least the clearance.
    const TriangleMesh v(lower.front()), hz(upper.front());
    REQUIRE(intersection_volume(v, hz) == Approx(0.).margin(1e-3));
    REQUIRE(min_surface_distance(v, hz) >= double(p.clearance) - 0.01);

    // The sizing rule that makes the interlock possible at all: each loop's opening has to
    // pass the other's wire with the clearance to spare on both sides.
    REQUIRE(flexi_min_link_size(p) == Approx(4. * double(p.wire) + 2. * double(p.clearance)));
    REQUIRE(p.link_length >= flexi_min_link_size(p));
    REQUIRE(p.link_width  >= flexi_min_link_size(p));

    // Undersized loops are refused.
    FlexiJointParams bad = p;
    bad.link_width = 0.9f * flexi_min_link_size(p);
    REQUIRE_FALSE(flexi_validate(bad).empty());
}

TEST_CASE("Chain link cut of a 20 mm cylinder", "[FlexiJoint]")
{
    const FlexiJointParams p = chain_params();
    const CutHalves h = cut_with_joint(p);

    // One object, two watertight parts.
    REQUIRE(h.objects == 1);
    REQUIRE(h.volumes == 2);
    REQUIRE_FALSE(h.upper.empty());
    REQUIRE_FALSE(h.lower.empty());
    REQUIRE(its_num_open_edges(h.upper.its) == 0);
    REQUIRE(its_num_open_edges(h.lower.its) == 0);

    // Zero intersection: the two parts are genuinely separate solids.
    REQUIRE(intersection_volume(h.upper, h.lower) == Approx(0.).margin(1e-3));

    // The minimum clearance between them is C - that is what the relief bodies buy.
    REQUIRE(min_surface_distance(h.upper, h.lower) == Approx(double(p.clearance)).margin(0.03));

    // The gap is honoured: the two flat cut faces are `gap` apart.
    REQUIRE(face_to_face_distance(h.upper, h.lower) == Approx(double(p.gap)).margin(0.02));
}

// The point of a chain link: it cannot be pulled apart.
TEST_CASE("Chain link is non-separable", "[FlexiJoint]")
{
    const FlexiJointParams p = chain_params();
    const CutHalves h = cut_with_joint(p);
    REQUIRE(h.volumes == 2);

    // Pull test: translating one part along the cut normal by more than the slack the joint
    // actually has must drive the two loops into each other. gap + clearance is comfortably
    // past that slack, so the parts have to collide.
    const double pull = double(p.gap) + double(p.clearance);
    TriangleMesh pulled(h.lower);
    pulled.translate(0.f, 0.f, float(-pull));
    REQUIRE(intersection_volume(h.upper, pulled) > 1e-2);

    // The pull stays blocked over the WHOLE travel the loops could physically make. Beyond
    // that the two centrelines have passed straight through each other, which no rigid body
    // can do; a mesh intersection test on a pure translation stops reporting overlap there
    // even though the parts would have had to break to get that far, so the sweep is bounded
    // by the real travel. The vertical loop's hole spans z in vcz +- (link_length/2 - wire)
    // with vcz = -gap/2 - stem + link_length/2, and the horizontal loop's wire bottom sits at
    // gap/2 + wire - wire = gap/2; the loops can only start to unthread once the hole's top
    // has dropped past that.
    const double vcz        = -0.5 * double(p.gap) - double(p.stem) + 0.5 * double(p.link_length);
    const double hole_top   = vcz + 0.5 * double(p.link_length) - double(p.wire);
    const double wire_bot   = 0.5 * double(p.gap);
    const double free_travel = hole_top - wire_bot;
    REQUIRE(free_travel > pull);          // the owner's pull test is inside it
    for (int i = 1; i <= 8; ++ i) {
        const double d = pull + (free_travel - pull) * double(i) / 8.;
        TriangleMesh m(h.lower);
        m.translate(0.f, 0.f, float(-d));
        INFO("pulled " << d << " mm");
        REQUIRE(intersection_volume(h.upper, m) > 1e-2);
    }

    // A sideways pull is blocked too: the loops are threaded, not merely stacked.
    for (const Vec3d &dir : { Vec3d(1., 0., 0.), Vec3d(-1., 0., 0.), Vec3d(0., 1., 0.) }) {
        TriangleMesh m(h.lower);
        const Vec3d t = pull * dir;
        m.translate(float(t.x()), float(t.y()), float(t.z()));
        INFO("pulled sideways " << dir.transpose());
        REQUIRE(intersection_volume(h.upper, m) > 1e-2);
    }
}

// The swing: the segments hinge about the horizontal loop's axis.
TEST_CASE("Chain link swings about the horizontal loop's axis", "[FlexiJoint]")
{
    const FlexiJointParams p = chain_params();
    const CutHalves h = cut_with_joint(p);
    REQUIRE(h.volumes == 2);

    // The horizontal loop lies with its long axis along +X, so the hinge axis is +Y.
    // Sweep the lower segment about it and check the joint stays clearance-clean.
    // 6 degrees is the angle the two flat faces allow before the rim at radius R touches:
    // asin(gap / (2R)) with gap = 1.5, R = 10 is about 4.3 degrees, so 4 degrees is inside it.
    const double swing_deg = 4.0;
    for (int deg = 1; deg <= int(swing_deg); ++ deg) {
        for (int sign : { -1, +1 }) {
            const TriangleMesh swung = rotated_about_joint(h.lower, Vec3d::UnitY(),
                                                           double(sign) * Geometry::deg2rad(double(deg)));
            INFO("swing " << (sign * deg) << " deg about +Y");
            REQUIRE(intersection_volume(h.upper, swung) == Approx(0.).margin(1e-2));
        }
    }
}

// The gap is a real, independent parameter for every kind.
TEST_CASE("The Gap parameter", "[FlexiJoint]")
{
    // Defaults: a chain link needs room for the loops to swing through each other; the
    // revolved kinds only rotate and rock in place.
    REQUIRE(flexi_default_gap(FlexiJointKind::ChainLink)  == Approx(1.5));
    REQUIRE(flexi_default_gap(FlexiJointKind::DoubleRing) == Approx(0.6));
    REQUIRE(flexi_default_gap(FlexiJointKind::BallSocket) == Approx(0.6));

    // An unset gap falls back to the kind default; a set one is honoured.
    FlexiJointParams p = ring_params();
    p.gap = 0.f;
    REQUIRE(flexi_effective_gap(p) == Approx(0.6));
    p.gap = 2.0f;
    REQUIRE(flexi_effective_gap(p) == Approx(2.0));

    // The floor is the clearance: the faces can never be closer than the joint's own
    // clearance, or the slicer would weld them together.
    p.gap = 0.1f;
    p.clearance = 0.35f;
    REQUIRE(flexi_effective_gap(p) == Approx(0.35));
    REQUIRE_FALSE(flexi_validate(p).empty());

    // A wider gap really does move the faces apart in the cut result.
    FlexiJointParams wide = ring_params();
    wide.gap = 1.6f;
    const CutHalves h = cut_with_joint(wide);
    REQUIRE(h.volumes == 2);
    REQUIRE(face_to_face_distance(h.upper, h.lower) == Approx(1.6).margin(0.02));
    // ... and the joint still bridges it: the parts do not come apart and do not touch.
    REQUIRE(intersection_volume(h.upper, h.lower) == Approx(0.).margin(1e-3));
    REQUIRE(min_surface_distance(h.upper, h.lower) == Approx(double(wide.clearance)).margin(0.03));
}

// Auto sizing has to keep the chain link inside the cut cross-section.
TEST_CASE("Chain link auto sizing fits the cross-section", "[FlexiJoint]")
{
    for (double inscribed : { 5.0, 10.0, 20.0 }) {
        FlexiJointParams p = flexi_auto_size(chain_params(), inscribed);
        INFO("inscribed radius " << inscribed);
        REQUIRE(flexi_validate(p).empty());
        REQUIRE(double(flexi_outer_extent(p)) <= inscribed);
        // The loops stay big enough to thread each other whatever the scale.
        REQUIRE(p.link_length >= flexi_min_link_size(p));
        REQUIRE(p.link_width  >= flexi_min_link_size(p));
    }
}

// ==================================================== the connector-availability regression
//
// THE BUG (owner's phase 1 report): "it breaks the 'connector' option after being used once.
// Future cuts have a greyed-out connector option; the entire slicer has to be restarted."
//
// Cause: GLGizmoCut3D::m_keep_as_parts is a plain member of the gizmo, and the gizmo is a
// singleton owned by the canvas, so it outlives any one cut. The flexi branch of
// render_cut_plane_input_window() used to WRITE `m_keep_as_parts = true` whenever a flexi
// connector was placed. The "Add connectors" / "Edit connectors" button is disabled by
//     !m_keep_upper || !m_keep_lower || m_keep_as_parts || ...
// so once that member had been forced true nothing ever cleared it and every later cut - on
// any object - opened with the connector option greyed out until the app restarted.
//
// Fix, in two halves:
//   * the flexi branch no longer writes m_keep_as_parts; it renders a local forced value and
//     leaves the user's own setting alone,
//   * perform_cut() records m_flexi_forced_after_cut, and on_set_state() puts the after-cut
//     flags (and the connector type) back to their defaults the next time the gizmo opens.
//
// The gizmo itself needs a GL canvas and wxWidgets, so it cannot be instantiated here. What
// this test pins down is the model-level invariant the fix has to preserve: the FORCE lives
// in the cut, not in a sticky flag, so a flexi cut and a following plain connector cut both
// behave, and neither one depends on the other having run.

// Build a plain cylinder with an ordinary Plug connector on the cut plane.
static ModelObject* make_plug_cylinder(Model &model)
{
    ModelObject *mo = model.add_object();
    mo->name        = "plug_cylinder";
    ModelVolume *v  = mo->add_volume(TriangleMesh(its_make_cylinder(CYL_R, CYL_H, 2. * PI / 180.)));
    v->set_type(ModelVolumeType::MODEL_PART);
    v->name = "cyl";
    mo->add_instance()->set_transformation(Geometry::Transformation());

    CutConnector c;
    c.pos        = Vec3d(0., 0., CUT_Z);
    c.rotation_m = Transform3d::Identity();
    c.z_angle    = 0.f;
    c.radius     = 2.5f;
    c.height     = 3.0f;
    c.attribs    = CutConnectorAttributes(CutConnectorType::Plug, CutConnectorStyle::Prism,
                                          CutConnectorShape::Circle);
    mo->cut_connectors.push_back(c);
    return mo;
}

TEST_CASE("A flexi cut does not disable connectors on the next cut", "[FlexiJoint]")
{
    // 1. A flexi cut. It forces keep-as-parts internally: one object, two parts.
    {
        const CutHalves h = cut_with_joint(chain_params());
        REQUIRE(h.objects == 1);
        REQUIRE(h.volumes == 2);
    }

    // 2. A PLAIN connector cut on a NEW object, run exactly as the gizmo would run it with
    //    the default after-cut flags - keep upper, keep lower, NOT keep-as-parts. Before the
    //    fix the gizmo could not even reach this point, because the button that places the
    //    connector was greyed out; the invariant is that the plain path is untouched by the
    //    flexi one and still produces the ordinary two-object result.
    Model model;
    ModelObject *mo = make_plug_cylinder(model);
    REQUIRE(mo->cut_connectors.size() == 1);
    REQUIRE_FALSE(is_flexi_connector_type(mo->cut_connectors.front().attribs.type));
    // The object carries no flexi joint, so the cut must NOT take the flexi path.
    REQUIRE_FALSE(has_flexi_joint(mo));

    Cut cut(mo, 0, Geometry::translation_transform(Vec3d(0., 0., CUT_Z)),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
            ModelObjectCutAttribute::PlaceOnCutUpper);
    const ModelObjectPtrs &res = cut.perform_with_plane();

    // A plain connector cut yields TWO objects (not the flexi path's single two-part object),
    // which is the proof that the previous flexi cut left nothing forced behind.
    REQUIRE(res.size() == 2);
    for (const ModelObject *o : res) {
        REQUIRE_FALSE(o->volumes.empty());
        REQUIRE(o->volumes.front()->is_model_part());
    }

    // 3. And a flexi cut still works after a plain one, in the other order.
    const CutHalves again = cut_with_joint(ring_params());
    REQUIRE(again.objects == 1);
    REQUIRE(again.volumes == 2);
}

// ================================================================ phase 3: the Hinge
//
// A print-in-place pin hinge: N alternating knuckles along the in-plane axis (+X in the joint
// frame, spun by the connector's Rotation), one continuous pin through all of them, parked at
// the edge of the cut face so the two halves can fold shut.

// The number of connected components of a mesh, by union-find over its shared vertices.
static size_t component_count(const indexed_triangle_set &its)
{
    std::vector<int> parent(its.vertices.size());
    for (size_t i = 0; i < parent.size(); ++ i)
        parent[i] = int(i);
    std::function<int(int)> find = [&parent, &find](int a) {
        while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; }
        return a;
    };
    auto unite = [&find, &parent](int a, int b) {
        const int ra = find(a), rb = find(b);
        if (ra != rb) parent[ra] = rb;
    };
    for (const Vec3i32 &t : its.indices) {
        unite(t[0], t[1]);
        unite(t[1], t[2]);
    }
    std::set<int> roots;
    for (size_t i = 0; i < parent.size(); ++ i)
        roots.insert(find(int(i)));
    return roots.size();
}

TEST_CASE("Hinge knuckles alternate between the two halves", "[FlexiJoint]")
{
    for (int n : { 1, 2, 3, 4, 5, 9 }) {
        FlexiJointParams p = hinge_params();
        p.hinge_knuckles = n;
        INFO("N = " << n);
        REQUIRE(hinge_knuckle_count(p) == n);
        REQUIRE(flexi_validate(p).empty());

        // Every knuckle belongs to exactly one half, and consecutive ones alternate.
        for (int i = 0; i + 1 < n; ++ i)
            REQUIRE(hinge_knuckle_is_lower(p, i) != hinge_knuckle_is_lower(p, i + 1));

        // Index 0 is the lower half by default; the fold-side flag swaps every parity.
        REQUIRE(hinge_knuckle_is_lower(p, 0));
        FlexiJointParams f = p;
        f.hinge_fold_upper = true;
        for (int i = 0; i < n; ++ i)
            REQUIRE(hinge_knuckle_is_lower(f, i) == !hinge_knuckle_is_lower(p, i));

        // The two halves' knuckle lists partition the run: the pin-bearing half's bodies are
        // its knuckles plus ONE pin, so it has ceil(n/2) + 1 components.
        const size_t lower_knuckles = size_t((n + 1) / 2);
        const size_t upper_knuckles = size_t(n / 2);
        REQUIRE(flexi_male_bodies(p).size()     == lower_knuckles + 1);   // + the pin
        REQUIRE(flexi_female_cavities(p).size() == upper_knuckles);

        // The run is centred on the joint origin whatever N is: slot 0 and slot N-1 are
        // mirror images about it.
        const double mid = 0.5 * (hinge_slot_centre(p, 0) + hinge_slot_centre(p, n - 1));
        REQUIRE(mid == Approx(0.).margin(1e-9));
    }
}

TEST_CASE("The hinge pin is one continuous solid in one half", "[FlexiJoint]")
{
    const FlexiJointParams p = hinge_params();
    const std::vector<indexed_triangle_set> bodies = flexi_male_bodies(p);
    REQUIRE(bodies.size() == 3);   // 2 lower knuckles + the pin, for N = 3

    // Every body is watertight and solid.
    for (const indexed_triangle_set &its : bodies) {
        REQUIRE(its_num_open_edges(its) == 0);
        REQUIRE(its_volume(its) > 0.f);
    }

    // The pin is the LAST entry, and it is a single connected shell spanning the whole run:
    // one component, and its extent along the hinge axis reaches from the first knuckle's
    // outer face to the last one's.
    const indexed_triangle_set &pin = bodies.back();
    REQUIRE(component_count(pin) == 1);

    const int    n   = hinge_knuckle_count(p);
    const double x0  = hinge_slot_centre(p, 0)     - 0.5 * hinge_knuckle_length(p);
    const double x1  = hinge_slot_centre(p, n - 1) + 0.5 * hinge_knuckle_length(p);
    double lo = 1e9, hi = -1e9, rmax = 0.;
    for (const Vec3f &v : pin.vertices) {
        lo = std::min(lo, double(v.x()));
        hi = std::max(hi, double(v.x()));
        rmax = std::max(rmax, std::hypot(double(v.y()) - hinge_axis_y(p), double(v.z())));
    }
    REQUIRE(lo == Approx(x0).margin(1e-5));
    REQUIRE(hi == Approx(x1).margin(1e-5));
    REQUIRE(rmax == Approx(0.5 * double(p.hinge_pin_dia)).epsilon(0.01));

    // Volume of a cylinder: the pin really is solid all the way through, not a tube.
    const double want = PI * std::pow(0.5 * double(p.hinge_pin_dia), 2.) * (x1 - x0);
    REQUIRE(double(its_volume(pin)) == Approx(want).epsilon(0.02));

    // ... and the pin passes THROUGH the other half's knuckles - which is what makes it a
    // hinge rather than two combs side by side. Every upper knuckle's slot spans the pin.
    for (int i = 0; i < n; ++ i)
        if (!hinge_knuckle_is_lower(p, i)) {
            REQUIRE(hinge_slot_centre(p, i) > x0);
            REQUIRE(hinge_slot_centre(p, i) < x1);
        }
}

TEST_CASE("Hinge cut of a 20 mm cylinder", "[FlexiJoint]")
{
    const FlexiJointParams p = hinge_params();
    const CutHalves h = cut_with_joint(p);

    // One object, two watertight parts.
    REQUIRE(h.objects == 1);
    REQUIRE(h.volumes == 2);
    REQUIRE_FALSE(h.upper.empty());
    REQUIRE_FALSE(h.lower.empty());
    REQUIRE(its_num_open_edges(h.upper.its) == 0);
    REQUIRE(its_num_open_edges(h.lower.its) == 0);

    // THE proof the hinge prints in place: the two halves' solids do not intersect at all.
    REQUIRE(intersection_volume(h.upper, h.lower) == Approx(0.).margin(1e-3));

    // ... and they are at least the clearance apart, nowhere closer. The bore, the knuckle
    // end faces and the two flat cut faces are all held open by the same C.
    const double dist = min_surface_distance(h.upper, h.lower);
    INFO("min surface distance " << dist);
    REQUIRE(dist >= 0.2);
    REQUIRE(dist == Approx(double(p.clearance)).margin(0.05));

    // The gap between the two flat cut faces is honoured. face_to_face_distance() samples the
    // outer wall, and the hinge deliberately lives out there at the rim, so measure on the
    // side of the cylinder the barrel is NOT on: the topmost lower vertex and the bottommost
    // upper one at +y, well clear of the barrel at -y.
    {
        double lo = -1e9, hi = 1e9;
        for (const Vec3f &v : h.lower.its.vertices)
            if (double(v.y()) > 0.5 * CYL_R && std::hypot(double(v.x()), double(v.y())) > 0.9 * CYL_R)
                lo = std::max(lo, double(v.z()));
        for (const Vec3f &v : h.upper.its.vertices)
            if (double(v.y()) > 0.5 * CYL_R && std::hypot(double(v.x()), double(v.y())) > 0.9 * CYL_R)
                hi = std::min(hi, double(v.z()));
        INFO("faces at " << lo << " and " << hi);
        REQUIRE(hi - lo == Approx(double(p.gap)).margin(0.02));
    }
}

TEST_CASE("The fold side moves the pin to the other half", "[FlexiJoint]")
{
    // The cut's male/female split is a Z-order convention with nothing to say about which way
    // the user wants the part to open, so the fold-side flag has to be able to put the pin -
    // and the knuckles that carry it - on either half. The bodies are keyed by SEGMENT, and
    // the flag decides which knuckles land in which segment and where the pin travels.
    for (bool fold_upper : { false, true }) {
        FlexiJointParams p = hinge_params();
        p.hinge_fold_upper = fold_upper;
        INFO("fold_upper = " << fold_upper);

        // Whichever way round, the LOWER list has the knuckles that belong to the lower half,
        // the UPPER list the rest, and the pin appears in EXACTLY ONE of the two.
        const std::vector<indexed_triangle_set> lower = flexi_lower_bodies(p);
        const std::vector<indexed_triangle_set> upper = flexi_upper_bodies(p);
        const int n = hinge_knuckle_count(p);
        int lower_knuckles = 0, upper_knuckles = 0;
        for (int i = 0; i < n; ++ i)
            (hinge_knuckle_is_lower(p, i) ? lower_knuckles : upper_knuckles) ++;
        // + 1 for the pin, on the side that owns knuckle 0.
        const bool pin_is_lower = hinge_knuckle_is_lower(p, 0);
        REQUIRE(lower.size() == size_t(lower_knuckles) + (pin_is_lower ? 1u : 0u));
        REQUIRE(upper.size() == size_t(upper_knuckles) + (pin_is_lower ? 0u : 1u));
        REQUIRE(pin_is_lower == !fold_upper);

        // And the whole cut still works either way: two separate solids, clearance apart.
        const CutHalves h = cut_with_joint(p);
        REQUIRE(h.objects == 1);
        REQUIRE(h.volumes == 2);
        REQUIRE(its_num_open_edges(h.upper.its) == 0);
        REQUIRE(its_num_open_edges(h.lower.its) == 0);
        REQUIRE(intersection_volume(h.upper, h.lower) == Approx(0.).margin(1e-3));
        REQUIRE(min_surface_distance(h.upper, h.lower) >= 0.2);

        // The pin is a lot of solid material; the half that carries it has visibly more joint
        // material beyond its own cut face than the half that only carries bored knuckles.
        auto joint_volume_beyond_face = [&p](const TriangleMesh &m, bool is_upper) {
            // Count vertices past the half's own flat face - crude, but the two halves differ
            // by a whole pin, so the comparison is not close.
            size_t k = 0;
            for (const Vec3f &v : m.its.vertices)
                if (is_upper ? (double(v.z()) < CUT_Z - 0.5 * double(p.gap) - 1e-3)
                             : (double(v.z()) > CUT_Z + 0.5 * double(p.gap) + 1e-3))
                    ++ k;
            return k;
        };
        const size_t lo_beyond = joint_volume_beyond_face(h.lower, false);
        const size_t hi_beyond = joint_volume_beyond_face(h.upper, true);
        INFO("lower beyond " << lo_beyond << ", upper beyond " << hi_beyond);
        REQUIRE(lo_beyond > 0);
        REQUIRE(hi_beyond > 0);
        // With 3 knuckles the pin side has 2 knuckles + the pin and the other has 1 knuckle,
        // so the pin side is always the bigger of the two.
        if (pin_is_lower)
            REQUIRE(lo_beyond > hi_beyond);
        else
            REQUIRE(hi_beyond > lo_beyond);
    }
}

TEST_CASE("Hinge knuckle end faces are a Gap apart", "[FlexiJoint]")
{
    // The knuckles alternate, so every facing pair of end faces belongs to OPPOSITE halves -
    // which is what makes the arithmetic checkable: slot length minus knuckle length is the
    // gap, and the two cylinders inside adjacent slots are centred in them.
    for (float gap : { 0.4f, 0.6f, 1.2f }) {
        FlexiJointParams p = hinge_params();
        p.gap = gap;
        INFO("gap " << gap);
        REQUIRE(flexi_effective_gap(p) == Approx(gap));

        const double slot = hinge_slot_length(p);
        const double len  = hinge_knuckle_length(p);
        REQUIRE(slot - len == Approx(double(gap)).margin(1e-9));

        // Facing end faces of knuckles i and i+1: the right end of i and the left end of i+1.
        for (int i = 0; i + 1 < hinge_knuckle_count(p); ++ i) {
            const double right_of_i  = hinge_slot_centre(p, i)     + 0.5 * len;
            const double left_of_i1  = hinge_slot_centre(p, i + 1) - 0.5 * len;
            INFO("knuckles " << i << " and " << (i + 1));
            REQUIRE(left_of_i1 - right_of_i == Approx(double(gap)).margin(1e-9));
            REQUIRE(left_of_i1 - right_of_i >= double(gap) - 1e-9);
        }
    }
}

TEST_CASE("The hinge barrel sits at the cut contour's edge", "[FlexiJoint]")
{
    const FlexiJointParams p = hinge_params();

    // The barrel centreline is offset out along -e by the edge offset ...
    REQUIRE(hinge_axis_y(p) == Approx(-double(p.hinge_edge_offset)));

    // ... the barrel centre is inside the contour ...
    const double centre_r = std::abs(hinge_axis_y(p));
    REQUIRE(centre_r > 0.);
    REQUIRE(centre_r < CYL_R);

    // ... and so is EVERY corner of the footprint the gizmo tests, which is the property that
    // actually matters: a hinge parked tangent to the widest point of a ROUND cut face has its
    // knuckle-run ends hanging outside it. Auto placement backs off to the chord at half the
    // run's length precisely so this holds.
    for (const Vec2d &c : hinge_footprint_corners(p)) {
        INFO("corner " << c.transpose());
        REQUIRE(c.norm() <= CYL_R + 1e-6);
    }

    // It is parked as far OUT as it can go while that holds: the run's two far corners sit on
    // the rim itself, so there is as little material as possible behind the hinge line for a
    // folding half to collide with.
    const double far_corner = std::hypot(0.5 * double(p.hinge_length),
                                         centre_r + 0.5 * double(p.hinge_barrel_dia));
    REQUIRE(far_corner == Approx(CYL_R).margin(0.15));
    REQUIRE(far_corner < CYL_R);

    // Pushing it any further out puts those corners outside the contour - which is exactly
    // what the fixed out-of-contour check now catches and the old circle test did not.
    FlexiJointParams too_far = p;
    too_far.hinge_edge_offset += 0.5f;
    bool any_outside = false;
    for (const Vec2d &c : hinge_footprint_corners(too_far))
        any_outside |= c.norm() > CYL_R;
    REQUIRE(any_outside);
}

TEST_CASE("Rotation turns the hinge's knuckle run", "[FlexiJoint]")
{
    const FlexiJointParams p = hinge_params();

    // At 0 degrees the run lies along +X, so its footprint is long in x and short in y.
    const std::vector<Vec2d> corners = hinge_footprint_corners(p);
    double xlo = 1e9, xhi = -1e9, ylo = 1e9, yhi = -1e9;
    for (const Vec2d &c : corners) {
        xlo = std::min(xlo, c.x()); xhi = std::max(xhi, c.x());
        ylo = std::min(ylo, c.y()); yhi = std::max(yhi, c.y());
    }
    const double x_span = xhi - xlo;
    const double y_span = yhi - ylo;
    REQUIRE(x_span == Approx(double(p.hinge_length)));
    REQUIRE(y_span == Approx(double(p.hinge_barrel_dia)));
    REQUIRE(x_span > y_span);

    // The connector's Rotation is applied by the connector VOLUME's transform, not by the
    // geometry - so a 90 degree rotation turns the same footprint a quarter turn, it does not
    // make a bigger one. Rotating the corners is exactly what the gizmo's contour test does.
    const Transform3d rot = Geometry::rotation_transform(-0.5 * PI * Vec3d::UnitZ());
    double rx_lo = 1e9, rx_hi = -1e9, ry_lo = 1e9, ry_hi = -1e9;
    for (const Vec2d &c : corners) {
        const Vec3d q = rot * Vec3d(c.x(), c.y(), 0.);
        rx_lo = std::min(rx_lo, q.x()); rx_hi = std::max(rx_hi, q.x());
        ry_lo = std::min(ry_lo, q.y()); ry_hi = std::max(ry_hi, q.y());
    }
    // The long axis is now y and the short one x: the run has genuinely turned.
    REQUIRE(rx_hi - rx_lo == Approx(y_span).margin(1e-9));
    REQUIRE(ry_hi - ry_lo == Approx(x_span).margin(1e-9));

    // And the whole cut still works with a rotated connector: the knuckle run comes out along
    // +Y instead of +X, and the two halves are still separate solids the clearance apart.
    const CutHalves h = cut_with_joint(p, float(0.5 * PI));
    REQUIRE(h.objects == 1);
    REQUIRE(h.volumes == 2);
    REQUIRE(intersection_volume(h.upper, h.lower) == Approx(0.).margin(1e-3));
    REQUIRE(min_surface_distance(h.upper, h.lower) >= 0.2);

    // The knuckle run really did turn. Measure it where only the JOINT can reach: above the
    // cut plane, in the LOWER half - that half's own body stops at -gap/2, so anything it
    // still has up there is barrel or pin. The run's two signatures are its length (along the
    // axis) and where its centreline sits (out along -e); rotating by 90 degrees has to swap
    // which world axis carries which.
    auto barrel_extent = [&p](const TriangleMesh &lower) {
        double xlo = 1e9, xhi = -1e9, ylo = 1e9, yhi = -1e9;
        for (const Vec3f &v : lower.its.vertices) {
            if (double(v.z()) < CUT_Z + 0.5 * double(p.gap) + 1e-3)
                continue;   // below the lower half's own cut face: that is the cylinder
            xlo = std::min(xlo, double(v.x())); xhi = std::max(xhi, double(v.x()));
            ylo = std::min(ylo, double(v.y())); yhi = std::max(yhi, double(v.y()));
        }
        return BoundingBoxf(Vec2d(xlo, ylo), Vec2d(xhi, yhi));
    };

    // Unrotated: the run lies along +X, its centreline out at -Y.
    const CutHalves  h0   = cut_with_joint(p);
    const BoundingBoxf b0 = barrel_extent(h0.lower);
    INFO("unrotated barrel box " << b0.min.transpose() << " .. " << b0.max.transpose());
    // The run spans the whole hinge length LESS the gap: the two outermost knuckles are each
    // built gap/2 short at their outer end, the same shortening every knuckle gets, so the
    // spacing arithmetic stays uniform along the whole run.
    REQUIRE(b0.size().x() == Approx(double(p.hinge_length) - double(p.gap)).margin(0.15));
    REQUIRE(b0.size().y() == Approx(double(p.hinge_barrel_dia)).margin(0.15));
    // The centreline is out along -Y by the edge offset, nowhere near the origin.
    REQUIRE(0.5 * (b0.min.y() + b0.max.y()) == Approx(hinge_axis_y(p)).margin(0.2));

    // Rotated 90 degrees: length and width have swapped world axes, and the centreline has
    // moved from -Y to -X - i.e. both the run AND its edge placement turned with the joint.
    const BoundingBoxf b1 = barrel_extent(h.lower);
    INFO("rotated barrel box " << b1.min.transpose() << " .. " << b1.max.transpose());
    REQUIRE(b1.size().y() == Approx(b0.size().x()).margin(0.2));
    REQUIRE(b1.size().x() == Approx(b0.size().y()).margin(0.2));
    REQUIRE(0.5 * (b1.min.x() + b1.max.x()) == Approx(hinge_axis_y(p)).margin(0.2));
    REQUIRE(0.5 * (b1.min.y() + b1.max.y()) == Approx(0.).margin(0.2));
}

TEST_CASE("The hinge pin axis warning fires only off horizontal", "[FlexiJoint]")
{
    // The EASY case (spec section 1's correction of the brief): a pin lying flat on the bed.
    REQUIRE_FALSE(hinge_axis_needs_care(Vec3d(1., 0., 0.)));
    REQUIRE_FALSE(hinge_axis_needs_care(Vec3d(0., 1., 0.)));
    REQUIRE_FALSE(hinge_axis_needs_care(Vec3d(1., 1., 0.).normalized()));
    // A few degrees off flat is still fine - the threshold is |d.z| / |d| > 0.1, about 6 deg.
    REQUIRE_FALSE(hinge_axis_needs_care(Vec3d(1., 0., 0.05).normalized()));
    // The case that needs care: standing up, or steeply tilted.
    REQUIRE(hinge_axis_needs_care(Vec3d(0., 0., 1.)));
    REQUIRE(hinge_axis_needs_care(Vec3d(1., 0., 1.).normalized()));
    REQUIRE(hinge_axis_needs_care(Vec3d(1., 0., -0.5).normalized()));
    // A degenerate axis is not a reason to nag.
    REQUIRE_FALSE(hinge_axis_needs_care(Vec3d::Zero()));
}

TEST_CASE("Hinge guards", "[FlexiJoint]")
{
    FlexiJointParams p = hinge_params();
    REQUIRE(flexi_validate(p).empty());

    // A barrel that cannot hold the bore and a wall is refused.
    p.hinge_barrel_dia = p.hinge_pin_dia + 2.f * p.clearance;
    REQUIRE_FALSE(flexi_validate(p).empty());

    // Too many knuckles for the length: every knuckle would be shorter than the gap.
    p = hinge_params();
    p.hinge_knuckles = 9;
    p.hinge_length   = 4.f;
    REQUIRE_FALSE(flexi_validate(p).empty());

    // Out-of-range knuckle counts.
    p = hinge_params();
    p.hinge_knuckles = 0;
    REQUIRE_FALSE(flexi_validate(p).empty());
    p.hinge_knuckles = 10;
    REQUIRE_FALSE(flexi_validate(p).empty());

    // A pin hinge is captive because the bore wraps it: mouth (the bore) < lip (the barrel).
    p = hinge_params();
    REQUIRE(flexi_is_captive(p));
    REQUIRE(flexi_mouth_half_width(p) == Approx(hinge_bore_radius(p)));
    REQUIRE(flexi_lip_half_width(p)   == Approx(0.5 * double(p.hinge_barrel_dia)));

    // Auto sizing puts a hinge on a cut face without it poking out of the cross-section.
    for (double inscribed : { 5.0, 10.0, 20.0 }) {
        FlexiJointParams a = flexi_auto_size(hinge_params(), inscribed);
        INFO("inscribed radius " << inscribed);
        REQUIRE(flexi_validate(a).empty());
        REQUIRE(double(a.hinge_barrel_dia) > double(a.hinge_pin_dia) + 2. * double(a.clearance));
    }
}

// ------------------------------------------------------- the out-of-contour footprint fix
//
// THE BUG (owner's report): a chain link with plenty of room in the cut face still gets
// "1 connector is out of cut contour". GLGizmoCut3D::is_outside_of_cut_contour() sampled a
// CIRCLE of radius flexi_outer_extent() - the distance to the farthest point of the joint -
// so a long joint was tested as a disc big enough to swallow it, and that disc stuck out of
// contours the joint itself cleared easily. The fix samples flexi_footprint_corners() instead.
//
// The gizmo needs a GL canvas, so what is tested here is the footprint helper the fixed
// function calls, against the same "is every sample inside the contour?" question.

// Is every point of the joint's footprint inside a disc of this radius, i.e. inside the cut
// face of a cylinder of that radius? This is precisely the test the gizmo runs, minus GL.
static bool footprint_fits_in_disc(const FlexiJointParams &p, double contour_r)
{
    for (const Vec2d &c : flexi_footprint_corners(p))
        if (c.norm() > contour_r)
            return false;
    return true;
}

TEST_CASE("The out-of-contour footprint is the joint's real shape", "[FlexiJoint]")
{
    SECTION("chain link: the old circle test was the bug") {
        const FlexiJointParams p = chain_params();

        // The joint's real footprint is a SLOT, longer than it is wide.
        const std::vector<Vec2d> corners = chain_footprint_corners(p);
        double xlo = 1e9, xhi = -1e9, ylo = 1e9, yhi = -1e9;
        for (const Vec2d &c : corners) {
            xlo = std::min(xlo, c.x()); xhi = std::max(xhi, c.x());
            ylo = std::min(ylo, c.y()); yhi = std::max(yhi, c.y());
        }
        REQUIRE(xhi - xlo > yhi - ylo);

        // The farthest point of the real footprint from the joint origin ...
        double true_r = 0.;
        for (const Vec2d &c : corners)
            true_r = std::max(true_r, c.norm());

        // ... versus the radius the gizmo used to sample. flexi_outer_extent() measures to the
        // far end of the loops, which is what a cut face has to contain in ONE direction - not
        // in every direction at once, which is what a disc of that radius demands.
        const double old_circle_r = double(flexi_outer_extent(p));

        // A contour between the two: the joint fits, the old circle test did not.
        const double contour_r = 0.5 * (true_r + old_circle_r);
        INFO("true " << true_r << ", old circle " << old_circle_r << ", contour " << contour_r);
        if (old_circle_r > true_r + 1e-6) {
            REQUIRE(footprint_fits_in_disc(p, contour_r));       // the fix accepts it ...
            REQUIRE(old_circle_r > contour_r);                   // ... the old test rejected it
        }

        // And a contour that genuinely does NOT fit is still rejected.
        REQUIRE_FALSE(footprint_fits_in_disc(p, 0.5 * true_r));
    }

    SECTION("hinge: a long thin run fits a contour its length would not suggest") {
        FlexiJointParams p = hinge_params();
        p.hinge_length      = 16.f;
        p.hinge_edge_offset = 0.f;      // centred, so the maths is easy to read

        // The run is 16 mm long and 4 mm wide: its farthest corner is at sqrt(8^2 + 2^2).
        const double corner_r = std::hypot(8., 2.);
        REQUIRE(footprint_fits_in_disc(p, corner_r + 1e-6));
        // Anything smaller than that corner distance really is too small ...
        REQUIRE_FALSE(footprint_fits_in_disc(p, corner_r - 0.5));
        // ... and the joint still fits contours far smaller than a disc of its own LENGTH,
        // which a naive "radius = the length" circle test would have demanded.
        REQUIRE(corner_r < double(p.hinge_length));
    }

    SECTION("the revolved kinds are still tested as circles") {
        const FlexiJointParams p = ring_params();
        const std::vector<Vec2d> corners = flexi_footprint_corners(p);
        REQUIRE(corners.size() == 60);
        for (const Vec2d &c : corners)
            REQUIRE(c.norm() == Approx(double(flexi_outer_extent(p))));
        REQUIRE(footprint_fits_in_disc(p, double(flexi_outer_extent(p)) + 1e-6));
        REQUIRE_FALSE(footprint_fits_in_disc(p, double(flexi_outer_extent(p)) - 0.1));
    }
}

// ------------------------------------------------------------------- 3MF round trip, hinge

TEST_CASE("The hinge's parameters survive a serialization round trip", "[FlexiJoint]")
{
    // The connector's flexi parameters travel by cereal - into the 3MF's model file, and
    // through every undo/redo snapshot. Old files carry no hinge fields at all, so the
    // defaults have to be sane on their own.
    const FlexiJointParams defaults;
    REQUIRE(defaults.hinge_knuckles    == 3);
    REQUIRE(defaults.hinge_pin_dia     == Approx(2.0));
    REQUIRE(defaults.hinge_barrel_dia  == Approx(4.0));
    REQUIRE(defaults.hinge_length      == Approx(12.0));
    REQUIRE(defaults.hinge_edge_offset == Approx(0.0));
    REQUIRE_FALSE(defaults.hinge_fold_upper);
    // ... and they make a valid hinge as they stand, so a file that predates the hinge opens
    // with a hinge that would actually cut.
    FlexiJointParams as_hinge = defaults;
    as_hinge.kind = FlexiJointKind::Hinge;
    REQUIRE(flexi_validate(as_hinge).empty());

    FlexiJointParams p = hinge_params();
    p.hinge_knuckles    = 5;
    p.hinge_pin_dia     = 2.5f;
    p.hinge_barrel_dia  = 5.5f;
    p.hinge_length      = 18.f;
    p.hinge_edge_offset = 3.25f;
    p.hinge_fold_upper  = true;

    std::stringstream ss;
    {
        cereal::BinaryOutputArchive ar(ss);
        FlexiJointParams w = p;
        ar(w);
    }
    FlexiJointParams back;
    {
        cereal::BinaryInputArchive ar(ss);
        ar(back);
    }
    REQUIRE(back.kind == FlexiJointKind::Hinge);
    REQUIRE(back.hinge_knuckles    == 5);
    REQUIRE(back.hinge_pin_dia     == Approx(2.5));
    REQUIRE(back.hinge_barrel_dia  == Approx(5.5));
    REQUIRE(back.hinge_length      == Approx(18.0));
    REQUIRE(back.hinge_edge_offset == Approx(3.25));
    REQUIRE(back.hinge_fold_upper);
    // operator== has to see the new fields too, or an undo/redo would not notice a change.
    REQUIRE(back == p);
    FlexiJointParams other = p;
    other.hinge_knuckles = 3;
    REQUIRE(back != other);
}

TEST_CASE("A hinged object survives a 3MF round trip", "[FlexiJoint]")
{
    Model model;
    ModelObject *mo = make_jointed_cylinder(model, hinge_params());
    Cut cut(mo, 0, Geometry::translation_transform(Vec3d(0., 0., CUT_Z)),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepAsParts);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 1);

    Model out;
    for (ModelObject *o : res)
        out.add_object(*o);
    REQUIRE(out.objects.front()->volumes.size() == 2);

    const boost::filesystem::path file = boost::filesystem::temp_directory_path() / "edgeslicer_hinge_roundtrip.3mf";
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

// -------------------------------------------------------------------- the hinge demo export
//
// Hidden. Cuts a 40 mm cube with one 3-knuckle hinge and writes both halves as STL, so the
// owner can look at (and print) the thing this phase built:
//   set SNORCA_HINGE_OUT=<dir> && libslic3r_tests.exe "Export the hinge demo"
TEST_CASE("Export the hinge demo", "[.][HingeDemo]")
{
    const char *dir = std::getenv("SNORCA_HINGE_OUT");
    REQUIRE(dir != nullptr);

    const double S = 40.0;   // the cube's side

    FlexiJointParams p;
    p.kind              = FlexiJointKind::Hinge;
    p.clearance         = 0.30f;   // the spec's "free rotation" default on a 0.4 mm nozzle
    p.gap               = 0.6f;
    p.hinge_knuckles    = 3;
    p.hinge_pin_dia     = 3.0f;
    p.hinge_barrel_dia  = 6.0f;
    p.hinge_length      = 24.0f;
    // Auto edge placement on a 40 mm cube cut at mid height, exactly as the gizmo computes
    // it: the cut face is 40 x 40, so its edge is 20 mm out along -e; the barrel goes there
    // less its own radius, and less the 0.1 mm that keeps its outer wall from landing coplanar
    // with the cube's side face (which is enough to make the boolean fail outright).
    p.hinge_edge_offset = 0.5f * float(S) - 0.5f * p.hinge_barrel_dia - 0.1f;
    p.hinge_fold_upper  = false;
    REQUIRE(flexi_validate(p).empty());

    Model model;
    ModelObject *mo = model.add_object();
    mo->name        = "hinge_demo_cube";
    // its_make_cube spans [0, S]^3, and add_volume centres the geometry and records the offset
    // in the volume's own transform - so the cube ends up spanning [0, S] in z and [-S/2, S/2]
    // in x and y, exactly like the cylinder test article. Do NOT overwrite that transform.
    ModelVolume *v  = mo->add_volume(TriangleMesh(its_make_cube(S, S, S)));
    v->set_type(ModelVolumeType::MODEL_PART);
    v->name = "cube";
    mo->add_instance()->set_transformation(Geometry::Transformation());

    CutConnector connector;
    connector.pos        = Vec3d(0., 0., 0.5 * S);
    connector.rotation_m = Transform3d::Identity();
    connector.z_angle    = 0.f;
    connector.radius     = flexi_outer_extent(p);
    connector.height     = flexi_protrusion_height(p);
    connector.attribs    = CutConnectorAttributes(CutConnectorType::FlexiJoint, CutConnectorStyle::Prism,
                                                  CutConnectorShape::Circle);
    connector.flexi      = p;
    add_flexi_joint_volume(mo, connector, "Hinge-1");

    Cut cut(mo, 0, Geometry::translation_transform(Vec3d(0., 0., 0.5 * S)),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
            ModelObjectCutAttribute::KeepAsParts);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 1);
    ModelObject *ro = res.front();
    REQUIRE(ro->volumes.size() == 2);

    TriangleMesh upper, lower;
    for (const ModelVolume *mv : ro->volumes) {
        TriangleMesh m(mv->mesh());
        m.transform(mv->get_matrix());
        if (mv->is_from_upper())
            upper = m;
        else
            lower = m;
    }
    REQUIRE_FALSE(upper.empty());
    REQUIRE_FALSE(lower.empty());
    REQUIRE(its_num_open_edges(upper.its) == 0);
    REQUIRE(its_num_open_edges(lower.its) == 0);
    // The two halves really are separate solids, which is what makes it print in place.
    REQUIRE(intersection_volume(upper, lower) == Approx(0.).margin(1e-3));

    const boost::filesystem::path d(dir);
    upper.write_ascii((d / "hinge_demo_upper.stl").string().c_str());
    lower.write_ascii((d / "hinge_demo_lower.stl").string().c_str());

    // ... and the assembled pair, as one 3MF, so it can be opened in the slicer directly.
    Model out;
    for (ModelObject *o : res)
        out.add_object(*o);
    out.add_default_instances();
    REQUIRE(store_3mf((d / "hinge_demo.3mf").string().c_str(), &out, nullptr, false));

    WARN("wrote hinge_demo_upper.stl, hinge_demo_lower.stl and hinge_demo.3mf to " << dir);
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
        { "flexi_ring.3mf",  ring_params() },
        { "flexi_ball.3mf",  ball_params() },
        { "flexi_chain.3mf", chain_params() },
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
