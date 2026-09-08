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
