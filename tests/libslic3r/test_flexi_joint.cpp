#include <catch2/catch.hpp>

#include <libslic3r/AABBMesh.hpp>
#include <libslic3r/CutUtils.hpp>
#include <libslic3r/FlexiJoint.hpp>
#include <libslic3r/Format/3mf.hpp>
// Metadata/cut_information.xml - where a Flexi joint's parameters live - is a
// BambuStudio-lineage part, handled only by the project reader/writer pair.
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/Utils.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <cereal/archives/binary.hpp>
// SelfAdjointEigenSolver: the ring-plane fit below is a principal component problem.
#include <Eigen/Eigenvalues>

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

// ------------------------------------------------------------- phase 3: the ring planes
//
// A ring is a flat closed loop swept with a tube, so its vertices cluster tightly around ONE
// plane. Fitting that plane is a principal component problem: the plane's normal is the
// eigenvector of the vertex covariance with the SMALLEST eigenvalue (the direction the cloud
// is thinnest in - the tube radius). `plane_fit_normal` returns it, `plane_fit_residual` the
// RMS thickness in that direction, which is what says "this really is a ring in a plane".

struct FittedPlane
{
    Vec3d  centroid{ Vec3d::Zero() };
    Vec3d  normal{ Vec3d::UnitZ() };
    double residual{ 0. };   // RMS distance of the vertices from the fitted plane
    double spread{ 0. };     // RMS distance in the widest direction, for scale
};

static FittedPlane fit_plane(const std::vector<Vec3d> &pts)
{
    FittedPlane out;
    REQUIRE(pts.size() >= 3);
    for (const Vec3d &p : pts)
        out.centroid += p;
    out.centroid /= double(pts.size());

    Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
    for (const Vec3d &p : pts) {
        const Vec3d d = p - out.centroid;
        cov += d * d.transpose();
    }
    cov /= double(pts.size());

    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(cov);
    // Eigenvalues come out ascending: [0] is the thinnest direction = the plane normal.
    out.normal   = solver.eigenvectors().col(0).normalized();
    out.residual = std::sqrt(std::max(0., solver.eigenvalues()(0)));
    out.spread   = std::sqrt(std::max(0., solver.eigenvalues()(2)));
    return out;
}

static FittedPlane fit_plane(const indexed_triangle_set &its)
{
    std::vector<Vec3d> pts;
    pts.reserve(its.vertices.size());
    for (const Vec3f &v : its.vertices)
        pts.emplace_back(v.cast<double>());
    return fit_plane(pts);
}

// The offset of each ring's centre from the cut plane, read straight off the centrelines -
// the tests derive it rather than duplicating chain_frame()'s clamping arithmetic.
static double chain_ring_offset(const FlexiJointParams &p)
{
    const std::vector<Vec3d> upper = flexi_chain_upper_centreline(p);
    REQUIRE_FALSE(upper.empty());
    double z = 0.;
    for (const Vec3d &v : upper)
        z += v.z();
    return z / double(upper.size());
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

// Phase 3: the joint's own parameters - `rotation` above all - survive a save/load of a
// project saved BEFORE the cut, when the connector is still an unprocessed negative volume.
// Without this the joint silently changed shape when a project was reopened.
TEST_CASE("Flexi joint parameters survive a 3MF round trip", "[FlexiJoint]")
{
    FlexiJointParams p = chain_params();
    p.rotation = 62.5f;
    p.wire     = 1.1f;
    p.stem     = 1.7f;
    REQUIRE(flexi_validate(p).empty());

    Model model;
    ModelObject *mo = make_jointed_cylinder(model, p);
    // The connector volume is there, unprocessed, and carries the parameters.
    REQUIRE(has_flexi_joint(mo));
    model.add_default_instances();

    // The PROJECT writer, not Format/3mf.cpp's generic one: Metadata/cut_information.xml is a
    // BambuStudio-lineage part and only store_bbs_3mf/load_bbs_3mf handle it. This is the pair
    // the application itself saves and opens projects with.
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string test_file = (tmp_root / "edgeslicer_flexi_params.3mf").string();

    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    StoreParams store_params;
    store_params.path     = test_file.c_str();
    store_params.model    = &model;
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store_params));

    Model                     back;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset*>      project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &back, &plate_data, &project_presets,
                         &is_bbl_3mf, &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                         LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
    release_PlateData_list(plate_data);
    // SNORCA_FLEXI_KEEP keeps the project on disk, for inspecting the written
    // Metadata/cut_information.xml by hand.
    if (!std::getenv("SNORCA_FLEXI_KEEP"))
        boost::filesystem::remove(test_file);

    REQUIRE(back.objects.size() == 1);
    const ModelVolume *joint = nullptr;
    for (const ModelVolume *v : back.objects.front()->volumes)
        if (v->cut_info.is_flexi_joint())
            joint = v;
    REQUIRE(joint != nullptr);
    // It comes back UNPROCESSED, so the cut still sees a joint to apply.
    REQUIRE_FALSE(joint->cut_info.is_processed);

    const FlexiJointParams &q = joint->cut_info.flexi;
    REQUIRE(q.kind == p.kind);
    REQUIRE(q.rotation    == Approx(p.rotation));
    REQUIRE(q.wire        == Approx(p.wire));
    REQUIRE(q.stem        == Approx(p.stem));
    REQUIRE(q.clearance   == Approx(p.clearance));
    REQUIRE(q.gap         == Approx(p.gap));
    REQUIRE(q.link_length == Approx(p.link_length));
    REQUIRE(q.link_width  == Approx(p.link_width));
    REQUIRE(q.tilt_angle  == Approx(p.tilt_angle));
    // The whole struct, so a field added later without a 3MF attribute is caught here.
    REQUIRE(q == p);

    // The rings that come back are the rings that went in.
    const FittedPlane a_in  = fit_plane(flexi_lower_bodies(p).front());
    const FittedPlane a_out = fit_plane(flexi_lower_bodies(q).front());
    REQUIRE(std::abs(a_in.normal.dot(a_out.normal)) == Approx(1.).margin(1e-6));
}

TEST_CASE("Hinge parameters survive a 3MF round trip", "[FlexiJoint]")
{
    // The same project writer/reader pair, for the hinge's own six fields plus the shared
    // Rotation. Everything non-default, so a field left out of the 3MF attributes shows up.
    FlexiJointParams p  = hinge_params();
    p.hinge_knuckles    = 5;
    p.hinge_pin_dia     = 2.5f;
    p.hinge_barrel_dia  = 5.5f;
    p.hinge_length      = 15.f;
    p.hinge_edge_offset = 2.75f;
    p.hinge_fold_upper  = true;
    p.rotation          = 37.5f;
    REQUIRE(flexi_validate(p).empty());

    Model model;
    ModelObject *mo = make_jointed_cylinder(model, p);
    REQUIRE(has_flexi_joint(mo));
    model.add_default_instances();

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string test_file = (tmp_root / "edgeslicer_hinge_params.3mf").string();

    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    StoreParams store_params;
    store_params.path     = test_file.c_str();
    store_params.model    = &model;
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store_params));

    Model                     back;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset*>      project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &back, &plate_data, &project_presets,
                         &is_bbl_3mf, &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                         LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
    release_PlateData_list(plate_data);
    if (!std::getenv("SNORCA_FLEXI_KEEP"))
        boost::filesystem::remove(test_file);

    REQUIRE(back.objects.size() == 1);
    const ModelVolume *joint = nullptr;
    for (const ModelVolume *v : back.objects.front()->volumes)
        if (v->cut_info.is_flexi_joint())
            joint = v;
    REQUIRE(joint != nullptr);
    REQUIRE_FALSE(joint->cut_info.is_processed);

    const FlexiJointParams &q = joint->cut_info.flexi;
    // The KIND above all: the reader clamps the enum against untrusted file content, and a
    // clamp that stopped at ChainLink would silently turn every saved hinge into a ring.
    REQUIRE(q.kind == FlexiJointKind::Hinge);
    REQUIRE(q.hinge_knuckles    == 5);
    REQUIRE(q.hinge_pin_dia     == Approx(p.hinge_pin_dia));
    REQUIRE(q.hinge_barrel_dia  == Approx(p.hinge_barrel_dia));
    REQUIRE(q.hinge_length      == Approx(p.hinge_length));
    REQUIRE(q.hinge_edge_offset == Approx(p.hinge_edge_offset));
    REQUIRE(q.hinge_fold_upper);
    REQUIRE(q.rotation          == Approx(p.rotation));
    // The whole struct, so a field added later without a 3MF attribute is caught here.
    REQUIRE(q == p);

    // And the joint that comes back builds the same bodies as the one that went in.
    REQUIRE(flexi_lower_bodies(q).size() == flexi_lower_bodies(p).size());
    REQUIRE(flexi_upper_bodies(q).size() == flexi_upper_bodies(p).size());
}

// An older 3MF has no flexi attributes at all; it must still load, with the defaults - and
// `rotation` defaulting to 0 is what keeps such a file's joint where it always was.
//
// The reader gates the whole flexi block on the `flexi_kind` attribute being present, and a
// pre-phase-3 file has none, so every volume it loads keeps the FlexiJointParams the CutInfo
// constructor gives it. What that leaves is the DEFAULT-CONSTRUCTED struct, so the property
// worth pinning is that the default is the phase 2 geometry: rotation 0, i.e. the reference
// direction d0 = the cut plane's own +X.
TEST_CASE("A flexi joint with no stored rotation is the unrotated one", "[FlexiJoint]")
{
    const FlexiJointParams def;
    REQUIRE(def.rotation == Approx(0.f));

    // A CutInfo built the way the 3MF reader builds it for a file with no flexi attributes.
    const ModelVolume::CutInfo legacy(CutConnectorType::FlexiJoint, 0.f, 0.1f, true);
    REQUIRE(legacy.flexi == def);
    REQUIRE(legacy.flexi.rotation == Approx(0.f));

    // ... and a chain link at rotation 0 is the reference orientation: ring A in the x-z
    // plane, ring B in the y-z plane.
    FlexiJointParams p = chain_params();
    p.rotation = 0.f;
    REQUIRE(flexi_validate(p).empty());
    const FittedPlane a = fit_plane(flexi_lower_bodies(p).front());
    const FittedPlane b = fit_plane(flexi_upper_bodies(p).front());
    REQUIRE(std::abs(a.normal.y()) == Approx(1.).margin(1e-3));
    REQUIRE(std::abs(b.normal.x()) == Approx(1.).margin(1e-3));
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

// ================================================ phase 3: the chain link's rotational axis
//
// THE BUG (owner's phase 2 report): "in the 3D view the chain link shows one ring lying flat
// in the cut plane and the other looking like a small horizontal torus - it's orienting the
// vertical ring to print horizontally."
//
// Cause: chain_horizontal_path() mapped the upper ring's stadium long axis to +X and its width
// to +Y, so the ring's plane was spanned by (d, n x d) - i.e. it WAS the cut plane. Only one
// of the two rings crossed the plane; the other lay in it. The joint then hinged about a
// single axis and the flat ring printed as a horizontal torus.
//
// The rule this pins down: for a chain link at a plane with normal n, BOTH ring planes contain
// n (each fitted plane's normal is perpendicular to n), the two ring planes are perpendicular
// to each other, and the two centroids sit on opposite sides of the cut plane.

TEST_CASE("Chain link rings both stand in planes containing the cut normal", "[FlexiJoint]")
{
    const FlexiJointParams p = chain_params();
    const Vec3d n = Vec3d::UnitZ();          // the cut normal, in the cut frame

    const std::vector<indexed_triangle_set> lower = flexi_lower_bodies(p);
    const std::vector<indexed_triangle_set> upper = flexi_upper_bodies(p);
    REQUIRE(lower.size() == 1);
    REQUIRE(upper.size() == 1);

    const FittedPlane a = fit_plane(lower.front());
    const FittedPlane b = fit_plane(upper.front());

    // Each ring really IS planar: its thickness in the fitted normal direction is the tube's,
    // an order of magnitude under its in-plane spread.
    for (const FittedPlane &f : { a, b }) {
        INFO("residual " << f.residual << " spread " << f.spread);
        REQUIRE(f.residual < 0.35 * f.spread);
    }

    // THE FIX: both ring planes CONTAIN n, i.e. each fitted normal is perpendicular to n.
    // Phase 2 failed exactly here - the upper ring's normal was n itself, so the dot was 1.
    REQUIRE(std::abs(a.normal.dot(n)) == Approx(0.).margin(1e-3));
    REQUIRE(std::abs(b.normal.dot(n)) == Approx(0.).margin(1e-3));

    // ... and the two ring planes are perpendicular to each other.
    REQUIRE(std::abs(a.normal.dot(b.normal)) == Approx(0.).margin(1e-3));

    // The centroids sit on OPPOSITE sides of the cut plane: the lower ring below, the upper
    // one above. (The cut frame puts the plane at z == 0.)
    REQUIRE(a.centroid.z() < 0.);
    REQUIRE(b.centroid.z() > 0.);

    // With the default parameters they are the mirror image of each other about the plane.
    REQUIRE(a.centroid.z() == Approx(-b.centroid.z()).margin(1e-6));
    REQUIRE(a.centroid.x() == Approx(0.).margin(1e-6));
    REQUIRE(a.centroid.y() == Approx(0.).margin(1e-6));

    // And the rings still do not touch: a Manifold intersection of zero volume with the gap
    // at 0.2 or more.
    for (float gap : { 0.2f, 0.35f, 1.5f, 2.5f }) {
        FlexiJointParams q = p;
        q.gap = std::max(gap, q.clearance);
        INFO("gap " << q.gap);
        if (!flexi_validate(q).empty())
            continue;
        const TriangleMesh la(flexi_lower_bodies(q).front());
        const TriangleMesh ub(flexi_upper_bodies(q).front());
        REQUIRE(intersection_volume(la, ub) == Approx(0.).margin(1e-3));
    }
}

// The new `rotation` field: it turns the whole pair about n, so at 90 degrees the two ring
// planes have swapped.
TEST_CASE("Chain link rotation turns both rings about the cut normal", "[FlexiJoint]")
{
    const Vec3d n = Vec3d::UnitZ();

    FlexiJointParams p0 = chain_params();
    p0.rotation = 0.f;
    REQUIRE(flexi_validate(p0).empty());
    const FittedPlane a0 = fit_plane(flexi_lower_bodies(p0).front());
    const FittedPlane b0 = fit_plane(flexi_upper_bodies(p0).front());

    // At 0 the lower ring stands in the x-z plane (normal +-Y) and the upper one in the y-z
    // plane (normal +-X) - d = the cut plane's own +X axis.
    REQUIRE(std::abs(a0.normal.y()) == Approx(1.).margin(1e-3));
    REQUIRE(std::abs(b0.normal.x()) == Approx(1.).margin(1e-3));

    // ROTATING BY 90 DEGREES SWAPS THE TWO RING PLANES.
    FlexiJointParams p90 = p0;
    p90.rotation = 90.f;
    REQUIRE(flexi_validate(p90).empty());
    const FittedPlane a90 = fit_plane(flexi_lower_bodies(p90).front());
    const FittedPlane b90 = fit_plane(flexi_upper_bodies(p90).front());

    REQUIRE(std::abs(a90.normal.dot(b0.normal)) == Approx(1.).margin(1e-3));   // A took B's plane
    REQUIRE(std::abs(b90.normal.dot(a0.normal)) == Approx(1.).margin(1e-3));   // B took A's plane

    // Rotation preserves every invariant the geometry has to keep.
    for (float deg : { 0.f, 17.f, 45.f, 90.f, 133.f, 180.f }) {
        FlexiJointParams q = p0;
        q.rotation = deg;
        INFO("rotation " << deg);
        REQUIRE(flexi_validate(q).empty());

        const TriangleMesh la(flexi_lower_bodies(q).front());
        const TriangleMesh ub(flexi_upper_bodies(q).front());
        REQUIRE(its_num_open_edges(la.its) == 0);
        REQUIRE(its_num_open_edges(ub.its) == 0);

        const FittedPlane a = fit_plane(la.its);
        const FittedPlane b = fit_plane(ub.its);
        // Both planes still contain n, and stay perpendicular to each other.
        REQUIRE(std::abs(a.normal.dot(n)) == Approx(0.).margin(1e-3));
        REQUIRE(std::abs(b.normal.dot(n)) == Approx(0.).margin(1e-3));
        REQUIRE(std::abs(a.normal.dot(b.normal)) == Approx(0.).margin(1e-3));
        // Still on opposite sides, still not touching.
        REQUIRE(a.centroid.z() < 0.);
        REQUIRE(b.centroid.z() > 0.);
        REQUIRE(intersection_volume(la, ub) == Approx(0.).margin(1e-3));

        // The rings turn by exactly `deg`. The lower ring's plane normal starts at +-Y, so
        // after the rotation it must be parallel to R(deg) * Y. A plane normal has no sign, so
        // compare the two as undirected lines: |n . expected| == 1.
        const double rad = double(deg) * PI / 180.;
        const Vec3d  want_normal(-std::sin(rad), std::cos(rad), 0.);
        INFO("normal " << a.normal.transpose() << " expected " << want_normal.transpose());
        REQUIRE(std::abs(a.normal.dot(want_normal)) == Approx(1.).margin(2e-3));
        // ... and the upper ring's normal, which starts at +-X, follows the same turn.
        const Vec3d  want_b(std::cos(rad), std::sin(rad), 0.);
        REQUIRE(std::abs(b.normal.dot(want_b)) == Approx(1.).margin(2e-3));

        // The joint's footprint does not grow with rotation: outer_extent is measured about
        // the axis, so it is rotation invariant.
        REQUIRE(double(flexi_outer_extent(q)) == Approx(double(flexi_outer_extent(p0))).margin(1e-4));
    }

    // The revolved kinds have no direction in the plane, so they IGNORE the field: the same
    // bodies come out whatever it is set to.
    for (const FlexiJointParams &base : { ring_params(), ball_params() }) {
        FlexiJointParams r0 = base, r90 = base;
        r0.rotation  = 0.f;
        r90.rotation = 90.f;
        const std::vector<indexed_triangle_set> b0v = flexi_male_bodies(r0);
        const std::vector<indexed_triangle_set> b90v = flexi_male_bodies(r90);
        REQUIRE(b0v.size() == b90v.size());
        for (size_t i = 0; i < b0v.size(); ++ i)
            REQUIRE(double(its_volume(b90v[i])) == Approx(double(its_volume(b0v[i]))).epsilon(1e-9));
    }
}

// A rotated chain link still cuts a real, non-separable, two-part object.
TEST_CASE("A rotated chain link still cuts and still holds", "[FlexiJoint]")
{
    FlexiJointParams p = chain_params();
    p.rotation = 55.f;
    REQUIRE(flexi_validate(p).empty());

    const CutHalves h = cut_with_joint(p);
    REQUIRE(h.objects == 1);
    REQUIRE(h.volumes == 2);
    REQUIRE(its_num_open_edges(h.upper.its) == 0);
    REQUIRE(its_num_open_edges(h.lower.its) == 0);
    REQUIRE(intersection_volume(h.upper, h.lower) == Approx(0.).margin(1e-3));
    REQUIRE(min_surface_distance(h.upper, h.lower) == Approx(double(p.clearance)).margin(0.05));
    REQUIRE(face_to_face_distance(h.upper, h.lower) == Approx(double(p.gap)).margin(0.02));

    // Still non-separable: a straight pull past the joint's slack drives the rings together.
    const double pull = double(p.gap) + double(p.clearance);
    TriangleMesh pulled(h.lower);
    pulled.translate(0.f, 0.f, float(-pull));
    REQUIRE(intersection_volume(h.upper, pulled) > 1e-2);
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

    // The pull stays blocked over the WHOLE travel the rings could physically make. Beyond
    // that the two centrelines have passed straight through each other, which no rigid body
    // can do; a mesh intersection test on a pure translation stops reporting overlap there
    // even though the parts would have had to break to get that far, so the sweep is bounded
    // by the real travel. Phase 3: both rings stand upright, centred -+ off from the plane, so
    // the LOWER ring's hole reaches up to (-off + L/2 - wire) and the UPPER ring's free end
    // hangs down to (off - L/2). They can only unthread once the former is pulled below the
    // latter, which is exactly the span between those two z values.
    const double off        = chain_ring_offset(p);
    const double hole_top   = -off + 0.5 * double(p.link_length) - double(p.wire);
    const double wire_bot   =  off - 0.5 * double(p.link_length);
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

// The swing. Phase 3: because the two ring planes are perpendicular, the joint hinges about
// BOTH in-plane axes, not just one - that is the whole point of a chain link over a hinge.
TEST_CASE("Chain link swings about both in-plane axes", "[FlexiJoint]")
{
    const FlexiJointParams p = chain_params();
    const CutHalves h = cut_with_joint(p);
    REQUIRE(h.volumes == 2);

    // Sweep the lower segment about +X and about +Y and check the joint stays clearance-clean
    // both ways. 4 degrees is inside what the two flat faces allow before the rim at radius R
    // touches: asin(gap / (2R)) with gap = 1.5, R = 10 is about 4.3 degrees.
    const double swing_deg = 4.0;
    for (const Vec3d &axis : { Vec3d::UnitX(), Vec3d::UnitY() })
    for (int deg = 1; deg <= int(swing_deg); ++ deg) {
        for (int sign : { -1, +1 }) {
            const TriangleMesh swung = rotated_about_joint(h.lower, axis,
                                                           double(sign) * Geometry::deg2rad(double(deg)));
            INFO("swing " << (sign * deg) << " deg about " << axis.transpose());
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

    // At 90 degrees the footprint helper returns the SAME rectangle a quarter turn round -
    // not a bigger one. The Rotation field is the one the chain link already uses, shared.
    FlexiJointParams r90 = p;
    r90.rotation = 90.f;
    double rx_lo = 1e9, rx_hi = -1e9, ry_lo = 1e9, ry_hi = -1e9;
    for (const Vec2d &c : hinge_footprint_corners(r90)) {
        rx_lo = std::min(rx_lo, c.x()); rx_hi = std::max(rx_hi, c.x());
        ry_lo = std::min(ry_lo, c.y()); ry_hi = std::max(ry_hi, c.y());
    }
    // The long axis is now y and the short one x: the run has genuinely turned.
    REQUIRE(rx_hi - rx_lo == Approx(y_span).margin(1e-6));
    REQUIRE(ry_hi - ry_lo == Approx(x_span).margin(1e-6));

    // And the whole cut still works rotated: the knuckle run comes out along +Y instead of
    // +X, and the two halves are still separate solids the clearance apart.
    const CutHalves h = cut_with_joint(r90);
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

    // Rotated 90 degrees: the run's length and width have swapped world axes, and the
    // centreline has moved off -Y onto +X - a rotation by +90 sends (x, y) to (-y, x), so the
    // negative edge offset on Y comes back as a positive X. Both the run AND its edge
    // placement turned with the joint, which is the point.
    const BoundingBoxf b1 = barrel_extent(h.lower);
    INFO("rotated barrel box " << b1.min.transpose() << " .. " << b1.max.transpose());
    REQUIRE(b1.size().y() == Approx(b0.size().x()).margin(0.2));
    REQUIRE(b1.size().x() == Approx(b0.size().y()).margin(0.2));
    REQUIRE(0.5 * (b1.min.x() + b1.max.x()) == Approx(-hinge_axis_y(p)).margin(0.2));
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

        // The joint's real footprint is the rectangle its two rings sweep through the cut
        // plane. Whatever its aspect ratio, the point is that a RECTANGLE is not a disc: its
        // corners are what reach furthest, and its edges are much closer in.
        const std::vector<Vec2d> corners = chain_footprint_corners(p);
        REQUIRE(corners.size() == 4);
        double xlo = 1e9, xhi = -1e9, ylo = 1e9, yhi = -1e9;
        for (const Vec2d &c : corners) {
            xlo = std::min(xlo, c.x()); xhi = std::max(xhi, c.x());
            ylo = std::min(ylo, c.y()); yhi = std::max(yhi, c.y());
        }
        REQUIRE(xhi > xlo);
        REQUIRE(yhi > ylo);

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
    p.rotation          = 37.5f;   // the field shared with the chain link

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
    REQUIRE(back.rotation == Approx(37.5));
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

// ============================================================ thread and bayonet (phases 1+2)
//
// A twist lock is not an articulated joint: the two halves are MEANT to come apart, by turning.
// So the proofs are shaped differently from the flexi ones. What has to hold is:
//   * at the REST (screwed-in / locked) pose the two halves do not intersect at all - i.e. the
//     clearance really is there, all the way round the helix or the track,
//   * and there is a MOTION that separates them - the helical unscrewing path for the thread,
//     the axial-then-rotational entry path for the bayonet - along which they also never
//     intersect, which is what "it actually unscrews" means geometrically.

// The test article for the twist locks: the same 20 mm cylinder, threaded at a diameter that
// leaves a real bore wall.
static FlexiJointParams thread_params()
{
    FlexiJointParams p;
    p.kind             = FlexiJointKind::Thread;
    p.clearance        = 0.25f;
    p.gap              = 0.4f;
    p.thread_major_dia = 12.0f;
    // 2 starts at pitch 6 is a crest spacing of 3 mm - the same thread profile a single-start
    // 3 mm pitch gives, which is the research's recommended default, done up in half a turn
    // instead of a whole one. A 2-start thread at pitch 3 has crests only 1.5 mm apart and is
    // a much shallower thread; see thread_crest_spacing().
    p.thread_pitch     = 6.0f;
    p.thread_starts    = 2;
    p.thread_turns     = 1.25f;
    p.thread_lead_turns= 0.5f;
    p.thread_left_hand = false;
    p.thread_lid_upper = true;
    return p;
}

static FlexiJointParams bayonet_params()
{
    FlexiJointParams p;
    p.kind                  = FlexiJointKind::Bayonet;
    p.clearance             = 0.25f;
    p.gap                   = 0.4f;
    p.thread_major_dia      = 12.0f;
    p.thread_lid_upper      = true;
    p.bayonet_lugs          = 3;
    p.bayonet_lug_height    = 1.6f;
    p.bayonet_lug_thickness = 2.4f;
    p.bayonet_lug_arc       = 30.0f;
    p.bayonet_lock_angle    = 75.0f;
    p.bayonet_entry_depth   = 4.0f;
    p.bayonet_detent        = 0.35f;
    return p;
}

// The lid half moved along the HELIX it unscrews on: turned by `theta` about the axis and
// lifted by the matching rise. A right-hand thread backs OUT (up, for a lid on top) when it is
// turned anticlockwise seen from above, and the rise per radian is lead / 2pi with
// lead = pitch x starts.
static TriangleMesh unscrewed(const TriangleMesh &m, const FlexiJointParams &p, double theta)
{
    // THE LEAD IS thread_pitch. The textbook "lead = pitch x starts" is written where "pitch"
    // means the crest-to-crest distance; here thread_pitch is the helix's own RISE PER TURN and
    // the crest spacing is pitch / starts, so one turn of the lid advances it by thread_pitch.
    // What the start count buys is fewer turns to full engagement, not a longer turn.
    const double lead = double(p.thread_pitch);
    const double rise = lead * theta / (2. * M_PI);
    const double sgn  = p.thread_left_hand ? -1. : 1.;
    TriangleMesh out(m);
    const Vec3d  c(0., 0., CUT_Z);
    out.transform(Geometry::translation_transform(c + Vec3d(0., 0., sgn * rise)) *
                  Geometry::rotation_transform(Vec3d(0., 0., sgn * theta)) *
                  Geometry::translation_transform(-c));
    return out;
}

static TriangleMesh twisted_lifted(const TriangleMesh &m, double theta, double dz)
{
    TriangleMesh out(m);
    const Vec3d  c(0., 0., CUT_Z);
    out.transform(Geometry::translation_transform(c + Vec3d(0., 0., dz)) *
                  Geometry::rotation_transform(Vec3d(0., 0., theta)) *
                  Geometry::translation_transform(-c));
    return out;
}

TEST_CASE("its_make_helical_sweep builds watertight strands", "[FlexiJoint]")
{
    const std::vector<Vec2d> prof = { Vec2d(-0.5, -0.6), Vec2d(-0.5, 0.6), Vec2d(0.5, 0.3), Vec2d(0.5, -0.3) };
    for (int starts : { 1, 2, 4 }) {
        const std::vector<indexed_triangle_set> strands =
            its_make_helical_sweep(prof, 5.0, 3.0, starts, 1.25, 120, false, 0., 0.5);
        REQUIRE(int(strands.size()) == starts);
        for (const indexed_triangle_set &s : strands) {
            INFO("starts = " << starts);
            REQUIRE_FALSE(s.vertices.empty());
            // WATERTIGHT: the lead-in taper closes both ends by itself, no cap mesh needed.
            REQUIRE(its_num_open_edges(s) == 0);
            // ONE CONNECTED COMPONENT per start: the strand is a single continuous ribbon, not
            // a string of disconnected coils.
            REQUIRE(component_count(s) == 1);
            REQUIRE(its_volume(s) > 0.f);
        }
        // The strands are copies of each other, so they all enclose the same volume.
        for (size_t i = 1; i < strands.size(); ++ i)
            REQUIRE(double(its_volume(strands[i])) == Approx(double(its_volume(strands[0]))).epsilon(0.02));
    }
    // Flat-capped ends (no lead-in) are watertight too.
    const std::vector<indexed_triangle_set> flat =
        its_make_helical_sweep(prof, 5.0, 3.0, 1, 1.0, 120, false, 0., 0.);
    REQUIRE(flat.size() == 1);
    REQUIRE(its_num_open_edges(flat.front()) == 0);
    // Degenerate inputs give nothing rather than garbage.
    REQUIRE(its_make_helical_sweep(prof, 5.0, 3.0, 0,  1.0, 120).empty());
    REQUIRE(its_make_helical_sweep(prof, 0.0, 3.0, 1,  1.0, 120).empty());
    REQUIRE(its_make_helical_sweep(prof, 5.0, 0.0, 1,  1.0, 120).empty());
    REQUIRE(its_make_helical_sweep(prof, 5.0, 3.0, 1, -1.0, 120).empty());
    REQUIRE(its_make_helical_sweep(prof, 5.0, 3.0, 1,  1.0,   2).empty());
}

TEST_CASE("A right-hand helix rises the right way, a left-hand one the other", "[FlexiJoint]")
{
    const std::vector<Vec2d> prof = { Vec2d(-0.4, -0.5), Vec2d(-0.4, 0.5), Vec2d(0.4, 0.25), Vec2d(0.4, -0.25) };
    // Take a point a quarter turn along each and compare where it landed in the x-y plane: a
    // right-hand helix turns anticlockwise as it rises, a left-hand one clockwise.
    auto quarter_turn_point = [&prof](bool left) {
        const std::vector<indexed_triangle_set> s =
            its_make_helical_sweep(prof, 5.0, 4.0, 1, 1.0, 120, left, 0., 0.);
        REQUIRE(s.size() == 1);
        // The vertex nearest z == 1 mm (a quarter of the 4 mm pitch): a quarter of the way up.
        Vec3f best = s.front().vertices.front();
        double bd  = 1e9;
        for (const Vec3f &v : s.front().vertices) {
            const double d = std::abs(double(v.z()) - 1.0);
            if (d < bd) { bd = d; best = v; }
        }
        return best;
    };
    const Vec3f r = quarter_turn_point(false);
    const Vec3f l = quarter_turn_point(true);
    // Right hand: a quarter turn up puts the point near +Y. Left hand: near -Y.
    REQUIRE(double(r.y()) > 3.0);
    REQUIRE(double(l.y()) < -3.0);
}

TEST_CASE("Thread bodies are watertight and the female is the male plus the clearance", "[FlexiJoint]")
{
    const FlexiJointParams p = thread_params();
    REQUIRE(flexi_validate(p).empty());

    // The male thread (on the LID, which is the upper half by default) and the relief the body
    // half is carved with.
    const std::vector<indexed_triangle_set> lid  = flexi_upper_bodies(p);
    const std::vector<indexed_triangle_set> rel  = flexi_lower_reliefs(p);
    REQUIRE_FALSE(lid.empty());
    REQUIRE_FALSE(rel.empty());
    // Core cylinder + one strand per start.
    REQUIRE(lid.size() == size_t(1 + thread_start_count(p)));
    for (const indexed_triangle_set &its : lid)
        REQUIRE(its_num_open_edges(its) == 0);
    for (const indexed_triangle_set &its : rel)
        REQUIRE(its_num_open_edges(its) == 0);

    // The GROOVE is a re-sweep of the SAME helix with the profile DILATED by the clearance -
    // not a Clipper offset of the male mesh, and not a copy slid outward (which would leave the
    // flanks touching). So it is strictly fatter than the thread it clears, and longer, and
    // there is one of it per start.
    REQUIRE(rel.size() == size_t(1 + thread_start_count(p)));
    REQUIRE(double(its_volume(rel[1])) > double(its_volume(lid[1])));
    // The dilation really is a dilation: the grown profile is wider than the plain one in BOTH
    // directions, which is the whole difference from a radial shift.
    {
        const std::vector<Vec2d> plain = thread_profile(p);
        const std::vector<Vec2d> grown = thread_profile_grown(p, 0., double(p.clearance));
        REQUIRE(plain.size() == grown.size());
        double plain_dr = 0., grown_dr = 0., plain_dz = 0., grown_dz = 0.;
        for (size_t i = 0; i < plain.size(); ++ i) {
            plain_dr = std::max(plain_dr, std::abs(plain[i].x()));
            grown_dr = std::max(grown_dr, std::abs(grown[i].x()));
            plain_dz = std::max(plain_dz, std::abs(plain[i].y()));
            grown_dz = std::max(grown_dz, std::abs(grown[i].y()));
        }
        REQUIRE(grown_dr == Approx(plain_dr + double(p.clearance)));
        REQUIRE(grown_dz > plain_dz + 0.5 * double(p.clearance));
    }

    // The lid is on top by default, so the male thread is the UPPER half's body - and it
    // REACHES DOWN through the cut plane into the bore, which is what a plug does. What has to
    // hold is that it is anchored in its own half (it reaches above the lid face) and that it
    // reaches down about as far as the plug is long.
    float zmin = 1e9f, zmax = -1e9f;
    for (const indexed_triangle_set &its : lid)
        for (const Vec3f &v : its.vertices) {
            zmin = std::min(zmin, v.z());
            zmax = std::max(zmax, v.z());
        }
    const double face = 0.5 * double(flexi_effective_gap(p));
    REQUIRE(double(zmax) > face);                                   // anchored in the lid
    REQUIRE(double(zmin) < face - double(thread_axial_length(p)));   // and reaching into the bore
    // The lower half gets no body of its own for a thread - all it gets is the relief.
    REQUIRE(flexi_lower_bodies(p).empty());
}

TEST_CASE("Thread cut of a 20 mm cylinder", "[FlexiJoint]")
{
    const FlexiJointParams p = thread_params();
    const CutHalves        h = cut_with_joint(p);
    REQUIRE(h.objects == 1);
    REQUIRE(h.volumes == 2);
    REQUIRE_FALSE(h.upper.empty());
    REQUIRE_FALSE(h.lower.empty());
    REQUIRE(its_num_open_edges(h.upper.its) == 0);
    REQUIRE(its_num_open_edges(h.lower.its) == 0);

    // THE REST POSE. Screwed all the way in, the two halves do not touch: everywhere the male
    // thread runs, the female groove has the clearance around it.
    REQUIRE(intersection_volume(h.upper, h.lower) == Approx(0.).margin(1e-3));

    // ... AND THE CLEARANCE IS WHERE IT IS SUPPOSED TO BE. Measuring it as a vertex-to-surface
    // distance between the two finished halves does not work here and is worth saying why: the
    // lid's plug was SUBTRACTED from the body half, so the bore's surface and the plug's are the
    // same surface, and along every seam the boolean re-triangulated they carry coincident
    // vertices by construction. Such a measurement reports microns and means nothing.
    //
    // The clearance is a property of the two PROFILES, and the swept solids inherit it exactly,
    // because both are swept along the same helix with the same sector count and the same phase
    // - which is the whole reason the groove is a RE-SWEEP rather than a dilation of the male
    // mesh. So measure it there: every edge of the male trapezoid against the female one.
    {
        const std::vector<Vec2d> male   = thread_profile(p);
        const std::vector<Vec2d> groove = thread_profile_grown(p, 0., double(p.clearance));
        REQUIRE(male.size() == 4);
        REQUIRE(groove.size() == 4);
        // THE CLEARANCE IS FACE TO FACE, and that is what a thread bears on. For each of the
        // male trapezoid's four EDGES - crest, root and the two flanks - the perpendicular
        // distance out to the matching edge of the groove is the clearance on that face. All
        // four have to be at least C.
        for (size_t i = 0; i < male.size(); ++ i) {
            const Vec2d a = male[i], b = male[(i + 1) % male.size()];
            const Vec2d mid = 0.5 * (a + b);
            const Vec2d e   = b - a;
            Vec2d       nrm(e.y(), -e.x());              // outward normal of a CCW polygon
            nrm.normalize();
            // How far out along that normal the groove's boundary is: the smallest positive
            // crossing of the ray from the edge's midpoint with any groove edge.
            double reach = std::numeric_limits<double>::max();
            for (size_t j = 0; j < groove.size(); ++ j) {
                const Vec2d c = groove[j], d = groove[(j + 1) % groove.size()];
                const Vec2d f = d - c;
                const double den = nrm.x() * f.y() - nrm.y() * f.x();
                if (std::abs(den) < 1e-12)
                    continue;
                const Vec2d  w = c - mid;
                const double t = (w.x() * f.y() - w.y() * f.x()) / den;   // along the ray
                const double s = (w.x() * nrm.y() - w.y() * nrm.x()) / -den;  // along the edge
                if (t > 1e-9 && s >= -1e-9 && s <= 1. + 1e-9)
                    reach = std::min(reach, t);
            }
            INFO("male edge " << i << " clears by " << reach);
            REQUIRE(reach >= double(p.clearance) - 1e-6);
        }
        // The MITRED CORNERS are the one place the clearance is legitimately less than C, and it
        // is worth pinning why rather than papering over it: a true offset of a polygon is its
        // Minkowski sum with a disc, which replaces every convex corner with an ARC of radius C.
        // A four-point profile cannot carry an arc, so the corners are mitred instead - the two
        // offset edges extended to meet - and the mitre point sits C / sin(theta/2) from the
        // original corner along the bisector, which is further out, while the ORIGINAL corner
        // point is only C x cos(30 deg) from the mitred flank. That is a corner-to-corner
        // distance, not a face-to-face one; nothing bears there, and the four face clearances
        // above are what the fit is made of.
        double corner = std::numeric_limits<double>::max();
        for (const Vec2d &q : male)
            for (size_t j = 0; j < groove.size(); ++ j) {
                const Vec2d c = groove[j], d = groove[(j + 1) % groove.size()];
                const Vec2d f = d - c;
                const double L2 = f.squaredNorm();
                double t = L2 > 0. ? (q - c).dot(f) / L2 : 0.;
                t = std::max(0., std::min(1., t));
                corner = std::min(corner, (q - (c + t * f)).norm());
            }
        INFO("tightest corner clearance " << corner);
        // With the root flat's extra extension the corners reach the full clearance too, so this
        // is the strong form: NOWHERE on the male profile is closer than C to the groove.
        REQUIRE(corner >= double(p.clearance) - 1e-6);
    }

    // The two flat faces are a Gap apart, exactly as for every other kind.
    REQUIRE(face_to_face_distance(h.upper, h.lower) == Approx(double(flexi_effective_gap(p))).margin(0.02));
}

TEST_CASE("The thread unscrews: no collision anywhere along the helix", "[FlexiJoint]")
{
    const FlexiJointParams p = thread_params();
    const CutHalves        h = cut_with_joint(p);
    REQUIRE_FALSE(h.upper.empty());

    // Eight poses along the unscrewing motion, from fully seated out to fully clear. The lid
    // has to rise pitch x turns / starts... no: the ENGAGEMENT is turns full revolutions, and
    // each revolution lifts the lid by lead = pitch x starts, so backing out takes `turns`
    // revolutions and lifts by pitch x starts x turns. Sample the whole of it.
    const double total = 2. * M_PI * double(p.thread_turns);
    for (int i = 0; i <= 8; ++ i) {
        const double theta = total * double(i) / 8.;
        const TriangleMesh moved = unscrewed(h.upper, p, theta);
        INFO("pose " << i << " theta = " << theta);
        REQUIRE(intersection_volume(moved, h.lower) == Approx(0.).margin(2e-3));
    }
}

TEST_CASE("A thread that is only lifted, not turned, still fouls its own groove", "[FlexiJoint]")
{
    // The converse of the test above, and what makes it mean anything: pulling the lid straight
    // up WITHOUT turning it drives the male crest into the female groove's flank. If this did
    // not collide, the thread would not be holding anything.
    const FlexiJointParams p = thread_params();
    const CutHalves        h = cut_with_joint(p);
    REQUIRE_FALSE(h.upper.empty());
    // The lift has to be measured against the LEAD - pitch x starts, the rise of one whole turn
    // of the lid - not against the pitch: the groove runs helically out of the mouth, so a short
    // pure lift just slides the crest up its own run-out channel and finds nothing. Half a lead
    // puts the crest squarely between two coils of the groove, where the female half is solid.
    // A pure lift of a fraction of the CREST SPACING is what puts the male crest between two
    // turns of the groove, where the female half is solid. (Lifting by a whole crest spacing
    // would land it back in the next groove turn and slide free again - which is exactly what a
    // thread does, and why the sample walks a range instead of picking one height.)
    const double lead = thread_crest_spacing(p);
    bool jammed = false;
    for (double f : { 0.25, 0.4, 0.5, 0.6, 0.75 }) {
        const TriangleMesh pulled = twisted_lifted(h.upper, 0., f * lead);
        if (intersection_volume(pulled, h.lower) > 1e-2)
            jammed = true;
    }
    REQUIRE(jammed);
}

TEST_CASE("The thread start angle follows Rotation", "[FlexiJoint]")
{
    // Rotation IS the thread start angle: it turns every strand about the axis at once, so the
    // lid ends up pointing somewhere else when it is done up. Proof: the strand's own centroid
    // in the x-y plane turns by exactly that angle.
    FlexiJointParams a = thread_params();
    FlexiJointParams b = a;
    b.rotation = 90.f;

    auto strand_dir = [](const FlexiJointParams &p) {
        const std::vector<indexed_triangle_set> bodies = flexi_upper_bodies(p);
        REQUIRE(bodies.size() >= 2);
        // bodies[0] is the core cylinder (rotationally symmetric, so it says nothing);
        // bodies[1] is the first strand. Take the centroid of its LOWEST ring of vertices -
        // the start of the helix, which is what the start angle names.
        float zmin = 1e9f;
        for (const Vec3f &v : bodies[1].vertices)
            zmin = std::min(zmin, v.z());
        Vec2d acc(0., 0.);
        int   n = 0;
        for (const Vec3f &v : bodies[1].vertices)
            if (double(v.z()) < double(zmin) + 0.3) {
                acc += Vec2d(double(v.x()), double(v.y()));
                ++ n;
            }
        REQUIRE(n > 0);
        acc /= double(n);
        return std::atan2(acc.y(), acc.x()) * 180. / M_PI;
    };

    const double da = strand_dir(a);
    const double db = strand_dir(b);
    double diff = db - da;
    while (diff < -180.) diff += 360.;
    while (diff >  180.) diff -= 360.;
    REQUIRE(diff == Approx(90.).margin(3.));

    // ... and a rotated thread still cuts, and still holds.
    const CutHalves h = cut_with_joint(b);
    REQUIRE(h.volumes == 2);
    REQUIRE(intersection_volume(h.upper, h.lower) == Approx(0.).margin(1e-3));
}

TEST_CASE("The lid side moves the male thread to the other half", "[FlexiJoint]")
{
    FlexiJointParams p = thread_params();
    p.thread_lid_upper = false;
    REQUIRE(flexi_validate(p).empty());

    // With the lid below, the male thread is the LOWER half's body and the bore is cut into the
    // upper one - the mirror image of the default.
    const std::vector<indexed_triangle_set> male = flexi_lower_bodies(p);
    REQUIRE(male.size() == size_t(1 + thread_start_count(p)));
    float zmin = 1e9f, zmax = -1e9f;
    for (const indexed_triangle_set &its : male)
        for (const Vec3f &v : its.vertices) {
            zmin = std::min(zmin, v.z());
            zmax = std::max(zmax, v.z());
        }
    const double face = 0.5 * double(flexi_effective_gap(p));
    // Mirrored: anchored BELOW its own face and reaching UP into the bore above.
    REQUIRE(double(zmin) < -face);
    REQUIRE(double(zmax) > -face + double(thread_axial_length(p)));
    REQUIRE(flexi_upper_bodies(p).empty());

    const CutHalves h = cut_with_joint(p);
    REQUIRE(h.volumes == 2);
    REQUIRE(intersection_volume(h.upper, h.lower) == Approx(0.).margin(1e-3));
    // The lid still unscrews the right way round: a right-hand thread stays right-handed when
    // the whole assembly is mirrored, because the hand is flipped back with it.
    const double total = 2. * M_PI * double(p.thread_turns);
    for (int i = 0; i <= 4; ++ i) {
        const double theta = total * double(i) / 4.;
        // Lid below: unscrewing takes it DOWN, so the rise is negated.
        const double lead = double(p.thread_pitch);
        const TriangleMesh moved = twisted_lifted(h.lower, theta, -lead * theta / (2. * M_PI));
        INFO("pose " << i);
        REQUIRE(intersection_volume(moved, h.upper) == Approx(0.).margin(2e-3));
    }
}

TEST_CASE("Bayonet bodies are watertight and the lugs sit on the plug", "[FlexiJoint]")
{
    const FlexiJointParams p = bayonet_params();
    REQUIRE(flexi_validate(p).empty());

    const std::vector<indexed_triangle_set> lid = flexi_upper_bodies(p);
    REQUIRE(lid.size() == size_t(1 + bayonet_lug_count(p)));
    for (const indexed_triangle_set &its : lid)
        REQUIRE(its_num_open_edges(its) == 0);
    for (const indexed_triangle_set &its : flexi_lower_reliefs(p))
        REQUIRE(its_num_open_edges(its) == 0);
    // The BODY half gets no solid body of its own: the detent is left standing by the track
    // relief rather than added back afterwards, because the cut pipeline unions bodies in
    // before it subtracts reliefs and a bump added that way would be carved off again.
    REQUIRE(flexi_lower_bodies(p).empty());

    // The lugs reach out to the major radius and no further.
    double rmax = 0.;
    for (size_t i = 1; i < lid.size(); ++ i)
        for (const Vec3f &v : lid[i].vertices)
            rmax = std::max(rmax, std::hypot(double(v.x()), double(v.y())));
    REQUIRE(rmax == Approx(thread_major_radius(p)).margin(0.05));
}

TEST_CASE("Bayonet cut of a 20 mm cylinder", "[FlexiJoint]")
{
    const FlexiJointParams p = bayonet_params();
    const CutHalves        h = cut_with_joint(p);
    REQUIRE(h.objects == 1);
    REQUIRE(h.volumes == 2);
    REQUIRE(its_num_open_edges(h.upper.its) == 0);
    REQUIRE(its_num_open_edges(h.lower.its) == 0);
    // Locked, the two halves are clear of each other all round.
    REQUIRE(intersection_volume(h.upper, h.lower) == Approx(0.).margin(1e-3));
    REQUIRE(face_to_face_distance(h.upper, h.lower) == Approx(double(flexi_effective_gap(p))).margin(0.02));
}

TEST_CASE("The bayonet lug goes in through its channel and is captured by the turn", "[FlexiJoint]")
{
    const FlexiJointParams p = bayonet_params();
    const CutHalves        h = cut_with_joint(p);
    REQUIRE_FALSE(h.upper.empty());

    // The REST pose is LOCKED - the lugs are built where they end up when the lid is done up -
    // so unlocking means turning BACK by the lock angle (that is -sgn), and only then lifting.
    const double lock = bayonet_effective_lock_angle(p) * M_PI / 180.;
    const double sgn  = p.thread_left_hand ? 1. : -1.;

    // 1. THE TURN. From locked back round to the entry angle, at the seated depth, the lug runs
    //    along its track without fouling - except right at the start, where it rides over the
    //    detent, which is the whole point of the detent. Sample from past the bump onward.
    // The detent bites near the LOCKED end - the lug has to click over it - so the free run of
    // the track is the far part of it. Sample there.
    for (int i = 0; i <= 6; ++ i) {
        const double th = -sgn * lock * (0.75 + 0.25 * double(i) / 6.);
        const TriangleMesh moved = twisted_lifted(h.upper, th, 0.);
        INFO("turn pose " << i << " theta = " << th);
        REQUIRE(intersection_volume(moved, h.lower) == Approx(0.).margin(4e-3));
    }

    // 2. THE ENTRY PATH. Turned all the way back to the entry angle, the lugs are lined up with
    //    the axial channels and the lid lifts straight out: sample the whole lift.
    for (int i = 0; i <= 6; ++ i) {
        const double dz = 1.2 * double(p.bayonet_entry_depth) * double(i) / 6.;
        const TriangleMesh moved = twisted_lifted(h.upper, -sgn * lock, dz);
        INFO("entry pose " << i << " dz = " << dz);
        REQUIRE(intersection_volume(moved, h.lower) == Approx(0.).margin(2e-3));
    }

    // 3. THE LOCK. At the LOCKED angle - the rest pose - pulling straight up drives the lugs
    //    into the track's roof, which IS the mechanism. If this did not collide the lid would
    //    lift straight off and there would be no bayonet at all.
    const TriangleMesh pulled = twisted_lifted(h.upper, 0., 0.6 * double(p.bayonet_entry_depth));
    REQUIRE(intersection_volume(pulled, h.lower) > 1e-2);
}

TEST_CASE("The bayonet detent has to be ridden over", "[FlexiJoint]")
{
    // With the detent on, the lug's own path is pinched just short of the end of the track, so
    // turning THROUGH that spot interferes; with the detent off it does not. That difference is
    // the click.
    FlexiJointParams with = bayonet_params();
    FlexiJointParams without = with;
    without.bayonet_detent = 0.f;
    REQUIRE(flexi_validate(with).empty());
    REQUIRE(flexi_validate(without).empty());

    const double lock = bayonet_effective_lock_angle(with) * M_PI / 180.;
    const double sgn  = with.thread_left_hand ? 1. : -1.;

    const CutHalves a = cut_with_joint(with);
    const CutHalves b = cut_with_joint(without);

    // Walk the whole track. WITHOUT the detent the lug runs the length of it free; WITH it,
    // there is a band of angles - the bump - where it does not. That difference is the click.
    double worst_with = 0., worst_without = 0.;
    int    biting     = 0;
    for (int i = 0; i <= 20; ++ i) {
        const double th = -sgn * lock * double(i) / 20.;
        const double va = intersection_volume(twisted_lifted(a.upper, th, 0.), a.lower);
        const double vb = intersection_volume(twisted_lifted(b.upper, th, 0.), b.lower);
        worst_with    = std::max(worst_with, va);
        worst_without = std::max(worst_without, vb);
        if (va > 1e-3)
            ++ biting;
    }
    INFO("worst with = " << worst_with << ", worst without = " << worst_without
         << ", poses biting = " << biting);
    // The plain track is clear end to end...
    REQUIRE(worst_without == Approx(0.).margin(4e-3));
    // ... and the detented one is not, somewhere along it.
    REQUIRE(worst_with > 1e-2);
    // ... but only over a SHORT stretch of it: a detent that fouled half the track would be a
    // jam, not a click.
    REQUIRE(biting >= 1);
    // ... but only over a stretch of the track, not the whole of it: the lug is as wide as its
    // own arc, so it touches a bump of b degrees over arc + b degrees of travel, and the test
    // has to allow for that while still catching a bump so wide it brakes the whole turn.
    REQUIRE(biting <= 14);
}

TEST_CASE("Twist lock guards", "[FlexiJoint]")
{
    // ---- thread
    {
        FlexiJointParams p = thread_params();
        REQUIRE(flexi_validate(p).empty());

        FlexiJointParams q = p; q.thread_major_dia = 3.f;
        REQUIRE_FALSE(flexi_validate(q).empty());

        q = p; q.thread_starts = 5;
        REQUIRE_FALSE(flexi_validate(q).empty());
        q = p; q.thread_starts = 0;
        REQUIRE_FALSE(flexi_validate(q).empty());

        q = p; q.thread_turns = 0.f;
        REQUIRE_FALSE(flexi_validate(q).empty());

        // The printability floor on the pitch.
        q = p; q.thread_pitch = 0.5f;
        REQUIRE_FALSE(flexi_validate(q).empty());

        // The lead-in cannot be longer than half the thread - it is tapered at both ends.
        q = p; q.thread_lead_turns = 1.0f;   // turns == 1.25, so half is 0.625
        REQUIRE_FALSE(flexi_validate(q).empty());

        // THE CREST-SPACING GUARD. The thread AND the groove grown around it have to fit
        // between one crest and the next; thread_depth() caps itself to keep that true, and
        // what the guard refuses is the case where even that cap cannot save it.
        {
            const double S = thread_crest_spacing(p);
            const double h = thread_profile_height(p, thread_depth(p)) + thread_groove_growth(p);
            INFO("crest spacing " << S << ", groove height " << h);
            REQUIRE(h < S);
            // ... and the wall left between one groove turn and the next is real, not a sliver.
            REQUIRE(S - h > 0.15);
        }
        // Too many starts for this pitch and clearance: the groove's turns would merge.
        q = p; q.thread_starts = 4; q.thread_pitch = 2.f; q.clearance = 0.4f;
        REQUIRE_FALSE(flexi_validate(q).empty());
        // A thread whose depth would eat the core is refused on the minor-radius rule.
        q = p; q.thread_major_dia = 4.2f; q.thread_pitch = 12.f;
        REQUIRE_FALSE(flexi_validate(q).empty());

        // Clearance and gap keep the family-wide rules.
        q = p; q.clearance = 0.f;
        REQUIRE_FALSE(flexi_validate(q).empty());
        q = p; q.gap = 0.1f; q.clearance = 0.25f;
        REQUIRE_FALSE(flexi_validate(q).empty());

        // The gap-closing guard applies unchanged: it is about the clearance, whatever kind
        // carries it.
        REQUIRE(flexi_gap_closing_conflict(p, 0.2));
        REQUIRE_FALSE(flexi_gap_closing_conflict(p, 0.049));
        REQUIRE(flexi_max_safe_gap_closing_radius(p) == Approx(0.125));
    }
    // ---- bayonet
    {
        FlexiJointParams p = bayonet_params();
        REQUIRE(flexi_validate(p).empty());

        FlexiJointParams q = p; q.bayonet_lugs = 5;
        REQUIRE_FALSE(flexi_validate(q).empty());
        q = p; q.bayonet_lugs = 1;
        REQUIRE_FALSE(flexi_validate(q).empty());

        q = p; q.bayonet_lug_height = 0.2f;    // < 2 x clearance
        REQUIRE_FALSE(flexi_validate(q).empty());
        q = p; q.bayonet_lug_thickness = 0.3f;
        REQUIRE_FALSE(flexi_validate(q).empty());

        // The lugs cannot be taller than the plug is wide.
        q = p; q.bayonet_lug_height = 7.f;
        REQUIRE_FALSE(flexi_validate(q).empty());

        // Lug width plus track has to fit in one lug's share of the bore.
        q = p; q.bayonet_lug_arc = 85.f;
        REQUIRE_FALSE(flexi_validate(q).empty());

        // The entry has to be at least as deep as the lug is thick.
        q = p; q.bayonet_entry_depth = 1.f;
        REQUIRE_FALSE(flexi_validate(q).empty());

        // A detent as tall as the lug would block the track.
        q = p; q.bayonet_detent = 2.f;
        REQUIRE_FALSE(flexi_validate(q).empty());
        q = p; q.bayonet_detent = -0.1f;
        REQUIRE_FALSE(flexi_validate(q).empty());
    }
}

TEST_CASE("The twist axis warning fires only off vertical", "[FlexiJoint]")
{
    // Upright is the good case for a thread - the exact mirror image of the hinge, whose pin
    // wants to lie flat.
    REQUIRE_FALSE(twist_axis_needs_care(Vec3d(0., 0., 1.)));
    REQUIRE_FALSE(twist_axis_needs_care(Vec3d(0., 0., -1.)));
    REQUIRE_FALSE(twist_axis_needs_care(Vec3d(0.05, 0., 1.)));      // ~3 degrees off: fine
    REQUIRE(twist_axis_needs_care(Vec3d(0.2, 0., 1.)));             // ~11 degrees off: warn
    REQUIRE(twist_axis_needs_care(Vec3d(1., 0., 0.)));              // flat on its side
    REQUIRE(twist_axis_needs_care(Vec3d(0., 0., 0.)));              // degenerate: warn
}

TEST_CASE("Twist lock auto sizing fits the cross-section", "[FlexiJoint]")
{
    for (double r_in : { 6.0, 10.0, 25.0 }) {
        for (FlexiJointKind k : { FlexiJointKind::Thread, FlexiJointKind::Bayonet }) {
            FlexiJointParams p = k == FlexiJointKind::Thread ? thread_params() : bayonet_params();
            p = flexi_auto_size(p, r_in);
            INFO("r_in = " << r_in << " kind = " << int(k));
            REQUIRE(flexi_validate(p).empty());
            // The bore AND its wall have to stay inside the cross-section.
            REQUIRE(double(flexi_outer_extent(p)) <= r_in);
            // ... and the fastener is not vanishingly small either.
            REQUIRE(double(p.thread_major_dia) > 0.4 * r_in);
        }
    }
}

TEST_CASE("The twist lock footprint is the bore's outer wall", "[FlexiJoint]")
{
    // What has to fit inside the cut contour is not the male thread - it is the FEMALE BORE
    // plus the wall around it, because that is the widest thing the joint puts on the plane.
    for (FlexiJointKind k : { FlexiJointKind::Thread, FlexiJointKind::Bayonet }) {
        FlexiJointParams p = k == FlexiJointKind::Thread ? thread_params() : bayonet_params();
        const double want = thread_bore_radius(p) + thread_bore_wall();
        REQUIRE(double(flexi_outer_extent(p)) == Approx(want));
        const std::vector<Vec2d> fp = flexi_footprint_corners(p);
        REQUIRE(fp.size() >= 3);
        double rmin = 1e9, rmax = 0.;
        for (const Vec2d &v : fp) { rmin = std::min(rmin, v.norm()); rmax = std::max(rmax, v.norm()); }
        // A circle, not a rectangle: every sample is at the same radius.
        REQUIRE(rmax == Approx(want).margin(1e-6));
        REQUIRE(rmin == Approx(want).margin(1e-6));
        // ... and it does fit the 10 mm cylinder the demo cuts.
        REQUIRE(footprint_fits_in_disc(p, CYL_R));
    }
}

TEST_CASE("Thread and bayonet parameters survive a 3MF round trip", "[FlexiJoint]")
{
    // Both kinds, every new field non-default, through the real project writer/reader pair.
    struct Case { const char *file; FlexiJointParams p; };
    std::vector<Case> cases;
    {
        FlexiJointParams p = thread_params();
        p.thread_major_dia  = 13.5f;
        p.thread_pitch      = 4.0f;
        p.thread_starts     = 3;
        p.thread_turns      = 1.75f;
        p.thread_lead_turns = 0.375f;
        p.thread_left_hand  = true;
        p.thread_lid_upper  = false;
        p.rotation          = 42.5f;
        REQUIRE(flexi_validate(p).empty());
        cases.push_back({ "edgeslicer_thread_params.3mf", p });
    }
    {
        FlexiJointParams p = bayonet_params();
        p.thread_major_dia      = 14.5f;
        p.thread_lid_upper      = false;
        p.bayonet_lugs          = 4;
        p.bayonet_lug_height    = 1.25f;
        p.bayonet_lug_thickness = 3.25f;
        p.bayonet_lug_arc       = 22.5f;
        p.bayonet_lock_angle    = 45.f;
        p.bayonet_entry_depth   = 5.5f;
        p.bayonet_detent        = 0.45f;
        p.rotation              = 17.5f;
        REQUIRE(flexi_validate(p).empty());
        cases.push_back({ "edgeslicer_bayonet_params.3mf", p });
    }

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());

    for (const Case &c : cases) {
        INFO(c.file);
        Model model;
        ModelObject *mo = make_jointed_cylinder(model, c.p);
        REQUIRE(has_flexi_joint(mo));
        model.add_default_instances();

        const std::string test_file = (tmp_root / c.file).string();
        DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
        StoreParams store_params;
        store_params.path     = test_file.c_str();
        store_params.model    = &model;
        store_params.config   = &store_config;
        store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
        REQUIRE(store_bbs_3mf(store_params));

        Model                     back;
        DynamicPrintConfig        dst_config;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
        PlateDataPtrs             plate_data;
        std::vector<Preset*>      project_presets;
        bool                      is_bbl_3mf = false;
        Semver                    file_version;
        REQUIRE(load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &back, &plate_data, &project_presets,
                             &is_bbl_3mf, &file_version, nullptr,
                             LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                             LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
        release_PlateData_list(plate_data);
        if (!std::getenv("SNORCA_FLEXI_KEEP"))
            boost::filesystem::remove(test_file);

        REQUIRE(back.objects.size() == 1);
        const ModelVolume *joint = nullptr;
        for (const ModelVolume *v : back.objects.front()->volumes)
            if (v->cut_info.is_flexi_joint())
                joint = v;
        REQUIRE(joint != nullptr);
        const FlexiJointParams &q = joint->cut_info.flexi;
        // THE KIND ABOVE ALL: the reader clamps the enum against untrusted file content, and a
        // clamp that stopped at Hinge would silently turn every saved thread into a ring.
        REQUIRE(q.kind == c.p.kind);
        // Every field, one struct comparison, so one added later without a 3MF attribute is
        // caught here rather than in the field.
        REQUIRE(q == c.p);
        REQUIRE(flexi_lower_bodies(q).size() == flexi_lower_bodies(c.p).size());
        REQUIRE(flexi_upper_bodies(q).size() == flexi_upper_bodies(c.p).size());
        REQUIRE(flexi_lower_reliefs(q).size() == flexi_lower_reliefs(c.p).size());
        REQUIRE(flexi_upper_reliefs(q).size() == flexi_upper_reliefs(c.p).size());
    }
}

TEST_CASE("The twist locks' parameters survive a serialization round trip", "[FlexiJoint]")
{
    // The cereal path, which is what undo/redo and the project's own volume snapshots use.
    for (FlexiJointKind k : { FlexiJointKind::Thread, FlexiJointKind::Bayonet }) {
        FlexiJointParams p = k == FlexiJointKind::Thread ? thread_params() : bayonet_params();
        p.thread_major_dia   = 15.25f;
        p.thread_lid_upper   = false;
        p.thread_pitch       = 3.75f;
        p.thread_starts      = 4;
        p.thread_turns       = 2.f;
        p.thread_left_hand   = true;
        p.thread_lead_turns  = 0.25f;
        p.bayonet_lugs       = 2;
        p.bayonet_lug_height = 2.f;
        p.bayonet_lug_arc    = 35.f;
        p.bayonet_lock_angle = 55.f;
        p.bayonet_entry_depth= 6.f;
        p.bayonet_detent     = 0.5f;
        p.rotation           = 123.f;

        std::stringstream ss;
        {
            cereal::BinaryOutputArchive ar(ss);
            ar(p);
        }
        FlexiJointParams q;
        {
            cereal::BinaryInputArchive ar(ss);
            ar(q);
        }
        REQUIRE(q == p);
        REQUIRE(q.kind == p.kind);
    }
}

// --------------------------------------------------------------------------- the demo

// Hidden (Catch2 "[.]"): the pill-bottle demo. A 30 mm diameter, 40 mm tall cylinder cut 10 mm
// down from the top, with a Thread and, separately, a Bayonet. Run it with the output dir in
// the environment:
//   set SNORCA_TWIST_OUT=<dir> && libslic3r_tests.exe "Export the twist lock demo"
TEST_CASE("Export the twist lock demo", "[.][TwistDemo]")
{
    const char *dir = std::getenv("SNORCA_TWIST_OUT");
    REQUIRE(dir != nullptr);
    const boost::filesystem::path d(dir);
    boost::filesystem::create_directories(d);

    const double R = 15.0;    // 30 mm diameter
    const double H = 40.0;    // 40 mm tall
    const double Z = H - 10.; // cut 10 mm down from the top: a lid 10 mm deep

    struct Case { const char *name; FlexiJointParams p; };
    std::vector<Case> cases;
    const double LID = H - Z;    // how deep the lid is: 10 mm
    {
        FlexiJointParams p = thread_params();
        p = flexi_auto_size(p, R);
        p.thread_starts = 2;
        p.thread_turns  = 1.25f;
        // THE LID'S OWN DEPTH is the constraint auto sizing cannot see: it knows how WIDE the
        // cut is, not how much material is behind the face. The thread's plug is
        // pitch x turns + one strand's offset + a collar, and all of that has to fit inside the
        // 10 mm lid with a wall left on top - so the pitch comes from the lid, not the diameter.
        // (2 starts at this pitch still gives a 3 mm crest spacing, the recommended profile.)
        // Walk the pitch down until the whole threaded band fits in 80% of the lid. The band is
        // pitch x (turns + (starts-1)/starts) plus the profile's own height, and that height is
        // itself a fraction of the crest spacing, so it is easier to step than to invert.
        for (double pitch = 12.; pitch >= 2.; pitch -= 0.25) {
            p.thread_pitch = float(pitch);
            if (thread_axial_length(p) <= 0.8 * LID)
                break;
        }
        REQUIRE(flexi_validate(p).empty());
        INFO("thread demo: major " << p.thread_major_dia << " pitch " << p.thread_pitch
             << " depth " << thread_depth(p) << " plug " << thread_axial_length(p));
        REQUIRE(thread_axial_length(p) < LID);
        cases.push_back({ "thread", p });
    }
    {
        FlexiJointParams p = bayonet_params();
        p = flexi_auto_size(p, R);
        // Same story for the bayonet, and it is a much smaller ask: the plug is the entry depth
        // plus the lug's thickness plus a seat, all of which has to live in the lid.
        REQUIRE(flexi_validate(p).empty());
        REQUIRE(bayonet_plug_length(p) < LID);
        cases.push_back({ "bayonet", p });
    }

    for (const Case &c : cases) {
        Model model;
        ModelObject *mo = model.add_object();
        mo->name        = std::string("pill_bottle_") + c.name;
        ModelVolume *v  = mo->add_volume(TriangleMesh(its_make_cylinder(R, H, 2. * PI / 180.)));
        v->set_type(ModelVolumeType::MODEL_PART);
        v->name = "bottle";
        mo->add_instance()->set_transformation(Geometry::Transformation());

        CutConnector connector;
        connector.pos        = Vec3d(0., 0., Z);
        connector.rotation_m = Transform3d::Identity();
        connector.z_angle    = 0.f;
        connector.radius     = flexi_outer_extent(c.p);
        connector.height     = flexi_protrusion_height(c.p);
        connector.attribs    = CutConnectorAttributes(CutConnectorType::FlexiJoint, CutConnectorStyle::Prism,
                                                      CutConnectorShape::Circle);
        connector.flexi      = c.p;
        add_flexi_joint_volume(mo, connector, std::string(c.name) + "-1");

        Cut cut(mo, 0, Geometry::translation_transform(Vec3d(0., 0., Z)),
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
        REQUIRE(intersection_volume(upper, lower) == Approx(0.).margin(1e-2));

        upper.write_ascii((d / (std::string(c.name) + "_demo_lid.stl")).string().c_str());
        lower.write_ascii((d / (std::string(c.name) + "_demo_body.stl")).string().c_str());

        Model out;
        for (ModelObject *o : res)
            out.add_object(*o);
        out.add_default_instances();
        REQUIRE(store_3mf((d / (std::string(c.name) + "_demo.3mf")).string().c_str(), &out, nullptr, false));
        WARN("wrote " << c.name << "_demo_lid.stl, " << c.name << "_demo_body.stl and "
                      << c.name << "_demo.3mf to " << dir);
    }
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

// Phase 3 demo: cut a plain BOX with a chain link and write the two halves, plus the two ring
// bodies on their own, as STL so the orientation can be checked by eye in any viewer. Hidden
// like the fixture exporter; run it with the output dir in the environment:
//   set SNORCA_FLEXI_OUT=<dir> && libslic3r_tests.exe "Export chain link demo"
TEST_CASE("Export chain link demo", "[.][FlexiChainDemo]")
{
    const char *dir = std::getenv("SNORCA_FLEXI_OUT");
    REQUIRE(dir != nullptr);
    const boost::filesystem::path out_dir(dir);
    boost::filesystem::create_directories(out_dir);

    // A 24 x 24 x 24 box cut through its middle.
    const double BOX = 24.0;
    const double BOX_CUT = 0.5 * BOX;

    struct Case { const char *tag; float rotation; };
    for (const Case &c : { Case{ "rot0", 0.f }, Case{ "rot45", 45.f }, Case{ "rot90", 90.f } }) {
        FlexiJointParams p = chain_params();
        p.rotation = c.rotation;
        REQUIRE(flexi_validate(p).empty());

        Model model;
        ModelObject *mo = model.add_object();
        mo->name = "flexi_box";
        // its_make_cube() spans [0, BOX] on every axis. Centre it on x/y IN THE MESH (not with
        // a volume transform, which add_volume's own re-centring would fight) so the joint,
        // which is built about the origin, sits in the middle of the cut face.
        TriangleMesh cube(its_make_cube(BOX, BOX, BOX));
        cube.translate(float(-0.5 * BOX), float(-0.5 * BOX), 0.f);
        ModelVolume *v = mo->add_volume(std::move(cube), ModelVolumeType::MODEL_PART, false);
        v->name = "box";
        mo->add_instance()->set_transformation(Geometry::Transformation());

        CutConnector connector;
        connector.pos        = Vec3d(0., 0., BOX_CUT);
        connector.rotation_m = Transform3d::Identity();
        connector.z_angle    = 0.f;
        connector.radius     = flexi_outer_extent(p);
        connector.height     = flexi_protrusion_height(p);
        connector.attribs    = CutConnectorAttributes(CutConnectorType::FlexiJoint,
                                                      CutConnectorStyle::Prism, CutConnectorShape::Circle);
        connector.flexi      = p;
        add_flexi_joint_volume(mo, connector, "Flexi joint-1");

        Cut cut(mo, 0, Geometry::translation_transform(Vec3d(0., 0., BOX_CUT)),
                ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                ModelObjectCutAttribute::KeepAsParts);
        const ModelObjectPtrs &res = cut.perform_with_plane();
        REQUIRE(res.size() == 1);
        REQUIRE(res.front()->volumes.size() == 2);

        for (const ModelVolume *vol : res.front()->volumes) {
            TriangleMesh m(vol->mesh());
            m.transform(vol->get_matrix());
            const std::string name = std::string("chain_") + c.tag +
                                     (vol->is_from_upper() ? "_upper.stl" : "_lower.stl");
            const boost::filesystem::path file = out_dir / name;
            REQUIRE(its_write_stl_ascii(file.string().c_str(), "flexi", m.its));
            WARN("wrote " << file.string());
        }

        // The two ring bodies on their own, so the two perpendicular ring planes are obvious
        // without having to see through the box.
        its_write_stl_ascii((out_dir / (std::string("chain_") + c.tag + "_ringA.stl")).string().c_str(),
                            "ringA", flexi_lower_bodies(p).front());
        its_write_stl_ascii((out_dir / (std::string("chain_") + c.tag + "_ringB.stl")).string().c_str(),
                            "ringB", flexi_upper_bodies(p).front());
    }
}
