#include <catch2/catch.hpp>

#include <libslic3r/AABBMesh.hpp>
#include <libslic3r/CurvedCut.hpp>
#include <libslic3r/CutUtils.hpp>
#include <libslic3r/DrawCut.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/PushFit.hpp>
#include <libslic3r/TacJoint.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>

using namespace Slic3r;

// Pin and TAC Joint connectors on CURVED and DRAWN cuts (owner, 2026-10-31): the connector fit
// (cut_connector_fit), the curvature limit, the drawn-cut kerf faces and orientation, and the
// planar cut staying exactly what it was.

namespace {

constexpr double CUBE = 30.0;

ModelObject* surface_cube(Model& model)
{
    indexed_triangle_set its = its_make_cube(CUBE, CUBE, CUBE);
    for (Vec3f& v : its.vertices)
        v -= Vec3f::Constant(float(0.5 * CUBE));
    ModelObject* mo = model.add_object();
    mo->name        = "cube";
    mo->add_volume(TriangleMesh(its))->name = "part";
    mo->add_instance();
    mo->cut_id.init();
    return mo;
}

ModelObjectCutAttributes surface_attrs()
{
    return ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::CreateDowels;
}

CutConnector surface_tac(const Vec3d& pos, const Transform3d& rot)
{
    CutConnector c;
    c.pos        = pos;
    c.rotation_m = rot;
    c.radius     = 2.7f;
    c.height     = 11.f;
    c.attribs    = CutConnectorAttributes(CutConnectorType::TacJoint, CutConnectorStyle::Prism, CutConnectorShape::Circle);
    return c;
}

CutConnector surface_pin(const Vec3d& pos, const Transform3d& rot)
{
    CutConnector c;
    c.pos        = pos;
    c.rotation_m = rot;
    c.pushfit    = pushfit_apply_preset(PushFitParams{}, PushFitPreset::M);
    c.radius     = 0.5f * c.pushfit.diameter;
    c.height     = c.pushfit.length_upper + c.pushfit.length_lower;
    c.attribs    = CutConnectorAttributes(CutConnectorType::PushFit, CutConnectorStyle::Prism, CutConnectorShape::Circle);
    return c;
}

// Every volume of every result, model part or hole, is a closed manifold mesh.
bool all_closed(const ModelObjectPtrs& res)
{
    for (const ModelObject* o : res)
        for (const ModelVolume* v : o->volumes)
            if (v->mesh().empty() || its_num_open_edges(v->mesh().its) != 0)
                return false;
    return true;
}

size_t count_holes(const ModelObject* o, CutConnectorType type)
{
    size_t n = 0;
    for (const ModelVolume* v : o->volumes)
        n += v->is_negative_volume() && v->cut_info.is_connector && v->cut_info.connector_type == type;
    return n;
}

// The Pin's own hole in a half (not its mouth relief): a Pin volume whose name ends in `suffix`.
const ModelVolume* named_hole(const ModelObject* o, const std::string& suffix)
{
    for (const ModelVolume* v : o->volumes)
        if (v->is_negative_volume() && v->cut_info.is_pushfit() && v->name.size() >= suffix.size() &&
            v->name.compare(v->name.size() - suffix.size(), suffix.size(), suffix) == 0)
            return v;
    return nullptr;
}

Vec3d volume_centroid(const ModelVolume* v)
{
    REQUIRE(v != nullptr);
    Vec3d c = Vec3d::Zero();
    for (const Vec3f& p : v->mesh().its.vertices)
        c += v->get_matrix() * p.cast<double>();
    return c / double(std::max<size_t>(1, v->mesh().its.vertices.size()));
}

const ModelObject* separate_part(const ModelObjectPtrs& res)
{
    for (const ModelObject* o : res)
        if (o->volumes.size() == 1 && o->volumes.front()->is_model_part() && o->volumes.front()->cut_info.is_connector)
            return o;
    return nullptr;
}

// A gentle dome (radius of curvature ~18 mm at the top) and a tight one (~4 mm).
CurvedCutSheet gentle_dome()
{
    CurvedCutSheet sheet(5);
    sheet.set_half_size(40.0);
    sheet.at(2, 2) = 6.0;
    return sheet;
}
CurvedCutSheet tight_dome()
{
    CurvedCutSheet sheet(5);
    sheet.set_half_size(20.0);
    sheet.at(2, 2) = 10.0;
    return sheet;
}

// An open stroke across the cube's top face, bowed into an arc of `radius` mm through the
// middle of the face, with the face's +Z normal everywhere (what the raycast would give).
DrawCutStroke top_arc(double radius, double half_angle)
{
    DrawCutStroke stroke;
    const int     n = 61;
    for (int i = 0; i < n; ++i) {
        const double a = -half_angle + 2.0 * half_angle * double(i) / double(n - 1);
        stroke.append(Vec3d(radius * std::sin(a), radius * (1.0 - std::cos(a)), 0.5 * CUBE), Vec3d::UnitZ(), size_t(i));
    }
    return stroke;
}

DrawCutParams through_params()
{
    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 5.0;
    params.through_all = true;
    return params;
}

std::string refusal_message(const std::function<void()>& f)
{
    try {
        f();
    } catch (const std::invalid_argument& e) {
        return e.what();
    }
    return {};
}

} // namespace

// ------------------------------------------------------------------------ the fit itself

TEST_CASE("Connector fit: a flat cut has no deviation and its kerf faces sit either side", "[CutConnectorSurface]")
{
    const CutSurfaceProbe probe = CutSurfaceProbe::plane(1.0);
    const CutConnectorFit fit   = cut_connector_fit(probe, Transform3d::Identity(), 3.0);
    REQUIRE(fit.on_surface);
    REQUIRE(fit.upper_along_z);
    REQUIRE(fit.fits());
    REQUIRE(fit.deviation < 1e-9);
    REQUIRE(std::abs(fit.face_lo + 0.5) < 1e-3);
    REQUIRE(std::abs(fit.face_hi - 0.5) < 1e-3);

    // Upside down: the fit says so, and still measures the faces along the axis into the upper half.
    const CutConnectorFit flipped = cut_connector_fit(probe, Geometry::rotation_transform(PI * Vec3d::UnitX()), 3.0);
    REQUIRE(flipped.on_surface);
    REQUIRE(!flipped.upper_along_z);
    REQUIRE(std::abs(flipped.face_lo + 0.5) < 1e-3);
    REQUIRE(std::abs(flipped.face_hi - 0.5) < 1e-3);

    // A connector tilted on a flat cut stands off its own plane by r * tan(tilt).
    const CutConnectorFit tilted = cut_connector_fit(probe, Geometry::rotation_transform(0.2 * Vec3d::UnitX()), 3.0);
    REQUIRE(tilted.on_surface);
    REQUIRE(std::abs(tilted.deviation - 3.0 * std::tan(0.2)) < 1e-3);
    REQUIRE(!tilted.fits());
}

TEST_CASE("Connector fit: a footprint radius sets how much of a dome it sees", "[CutConnectorSurface]")
{
    const CurvedCutSheet  sheet = gentle_dome();
    const CutSurfaceProbe probe = CutSurfaceProbe::curved(sheet);
    const Transform3d     frame = Geometry::translation_transform(curved_cut_sheet_point(sheet, 0., 0.)) * curved_cut_sheet_frame(sheet, 0., 0.);
    const CutConnectorFit small = cut_connector_fit(probe, frame, 2.0);
    const CutConnectorFit large = cut_connector_fit(probe, frame, 6.0);
    REQUIRE(small.on_surface);
    REQUIRE(large.on_surface);
    REQUIRE(std::abs(small.centre_offset) < 1e-6);
    REQUIRE(small.deviation > 0.0);
    // Roughly the sagitta, which grows with the square of the radius.
    REQUIRE(large.deviation > 6.0 * small.deviation);
    REQUIRE(small.fits());
    REQUIRE(!large.fits());
}

// ------------------------------------------------------------------------ curved (dome) cut

TEST_CASE("Curved cut: TAC Joint and Pin on a dome give closed halves, sockets and parts", "[CutConnectorSurface][CurvedCut]")
{
    const CurvedCutSheet sheet = gentle_dome();
    Model                model;
    ModelObject*         mo = surface_cube(model);
    const double tx = 5., ty = -3., px = -6., py = 4.;
    add_tac_joint_volume(mo, surface_tac(curved_cut_sheet_point(sheet, tx, ty), curved_cut_sheet_frame(sheet, tx, ty)), "TAC Joint-1");
    add_pushfit_volume(mo, surface_pin(curved_cut_sheet_point(sheet, px, py), curved_cut_sheet_frame(sheet, px, py)), "Connector-2");

    Cut                    cut(mo, 0, Transform3d::Identity(), surface_attrs());
    const ModelObjectPtrs& res = cut.perform_with_curved_sheet(sheet);
    REQUIRE(res.size() == 4); // two halves, the TAC connector, the pin
    REQUIRE(all_closed(res));
    // Each half: the whole TAC cutter, and the Pin's own hole plus its mouth relief.
    for (size_t i = 0; i < 2; ++i) {
        REQUIRE(count_holes(res[i], CutConnectorType::TacJoint) == 1);
        REQUIRE(count_holes(res[i], CutConnectorType::PushFit) == 2);
    }
    const double cutter_volume = double(its_volume(tac_joint_cutter_mesh(TacJointParams{})));
    for (const ModelVolume* v : res[0]->volumes)
        if (v->is_negative_volume() && v->cut_info.is_tac_joint())
            REQUIRE(std::abs(double(its_volume(v->mesh().its)) - cutter_volume) < 1e-3 * cutter_volume);
    // The upper half's Pin hole is above the sheet, the lower half's below it.
    const CutSurfaceProbe probe = CutSurfaceProbe::curved(sheet);
    REQUIRE(probe.is_upper_mid(volume_centroid(named_hole(res[0], "_A"))));
    REQUIRE(!probe.is_upper_mid(volume_centroid(named_hole(res[1], "_B"))));
}

TEST_CASE("Curved cut: a Dowel on a dome gets the whole dowel as a socket in each half", "[CutConnectorSurface][CurvedCut]")
{
    const CurvedCutSheet sheet = gentle_dome();
    Model                model;
    ModelObject*         mo = surface_cube(model);
    const double         x = 3., y = 2.;
    ModelVolume* v = mo->add_volume(TriangleMesh(its_make_cylinder(1.0, 1.0)), ModelVolumeType::NEGATIVE_VOLUME);
    // Unit cylinder from z = 0 to 1, centred on the sheet: radius 2.5, 10 mm long.
    v->set_transformation(Geometry::translation_transform(curved_cut_sheet_point(sheet, x, y)) * curved_cut_sheet_frame(sheet, x, y) *
                          Geometry::scale_transform(Vec3d(2.5, 2.5, 10.)) * Geometry::translation_transform(Vec3d(0., 0., -0.5)));
    v->cut_info = ModelVolume::CutInfo(CutConnectorType::Dowel, 0.f, 0.f);
    v->name     = "Connector-dowel";

    Cut                    cut(mo, 0, Transform3d::Identity(), surface_attrs());
    const ModelObjectPtrs& res = cut.perform_with_curved_sheet(sheet);
    REQUIRE(res.size() == 3);
    REQUIRE(all_closed(res));
    const double whole = PI * 2.5 * 2.5 * 10.;
    for (size_t i = 0; i < 2; ++i) {
        REQUIRE(count_holes(res[i], CutConnectorType::Dowel) == 1);
        for (const ModelVolume* h : res[i]->volumes)
            if (h->is_negative_volume())
                REQUIRE(std::abs(double(its_volume(h->mesh().its)) - whole) < 0.02 * whole);
    }
}

// ------------------------------------------------------------------------ drawn cut

TEST_CASE("Drawn cut: TAC Joint and Pin on a drawn strip give closed halves, sockets and parts", "[CutConnectorSurface][DrawCut]")
{
    DrawCutStroke stroke = top_arc(40.0, std::asin(15.0 / 40.0));
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(stroke.valid());
    REQUIRE(!stroke.is_closed());
    const DrawCutParams params = through_params();

    // Both connectors halfway down the strip: TAC a third of the way along, the Pin two thirds.
    // The Pin is deliberately built upside down (frame turned over): the cut must turn it back.
    const double      w  = 0.5 * CUBE;
    const double      s1 = stroke.length() / 3.0, s2 = 2.0 * stroke.length() / 3.0;
    const Transform3d upside_down = Geometry::rotation_transform(PI * Vec3d::UnitX());

    for (bool flip_pin : { false, true }) {
        Model        model;
        ModelObject* mo = surface_cube(model);
        add_tac_joint_volume(mo, surface_tac(draw_cut_surface_point(stroke, params, s1, w), draw_cut_surface_frame(stroke, params, s1, w)),
                             "TAC Joint-1");
        add_pushfit_volume(mo,
                           surface_pin(draw_cut_surface_point(stroke, params, s2, w),
                                       draw_cut_surface_frame(stroke, params, s2, w) * (flip_pin ? upside_down : Transform3d::Identity())),
                           "Connector-2");

        Cut                    cut(mo, 0, Transform3d::Identity(), surface_attrs());
        const ModelObjectPtrs& res = cut.perform_with_draw_stroke(stroke, params);
        INFO("flip_pin " << flip_pin);
        REQUIRE(res.size() == 4);
        REQUIRE(all_closed(res));
        for (size_t i = 0; i < 2; ++i) {
            REQUIRE(count_holes(res[i], CutConnectorType::TacJoint) == 1);
            REQUIRE(count_holes(res[i], CutConnectorType::PushFit) == 2);
        }
        REQUIRE(separate_part(res) != nullptr);
        // Each Pin hole is on its own half's side of the strip, whichever way the Pin was built.
        indexed_triangle_set solid = its_make_cube(CUBE, CUBE, CUBE);
        for (Vec3f& v : solid.vertices)
            v -= Vec3f::Constant(float(0.5 * CUBE));
        const CutSurfaceProbe probe = CutSurfaceProbe::drawn(stroke, params, solid);
        REQUIRE(probe.valid());
        REQUIRE(probe.is_upper_mid(volume_centroid(named_hole(res[0], "_A"))));
        REQUIRE(!probe.is_upper_mid(volume_centroid(named_hole(res[1], "_B"))));
    }
}

TEST_CASE("Drawn cut: a Pin across a kerf takes its faces from the drawn strip", "[CutConnectorSurface][DrawCut]")
{
    DrawCutStroke stroke = top_arc(40.0, std::asin(15.0 / 40.0));
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    DrawCutParams params = through_params();
    params.thickness     = 1.0;

    indexed_triangle_set solid = its_make_cube(CUBE, CUBE, CUBE);
    for (Vec3f& v : solid.vertices)
        v -= Vec3f::Constant(float(0.5 * CUBE));
    const CutSurfaceProbe probe = CutSurfaceProbe::drawn(stroke, params, solid);
    REQUIRE(probe.valid());
    const double      s     = 0.5 * stroke.length(), w = 0.5 * CUBE;
    const Transform3d frame = Geometry::translation_transform(draw_cut_surface_point(stroke, params, s, w)) *
                              draw_cut_surface_frame(stroke, params, s, w);
    const CutConnectorFit fit = cut_connector_fit(probe, frame, 3.5);
    REQUIRE(fit.fits());
    // The kerf is centred: half a millimetre to each face, along the connector's axis.
    REQUIRE(std::abs(fit.face_lo + 0.5) < 0.1);
    REQUIRE(std::abs(fit.face_hi - 0.5) < 0.1);

    Model        model;
    ModelObject* mo = surface_cube(model);
    add_pushfit_volume(mo, surface_pin(draw_cut_surface_point(stroke, params, s, w), draw_cut_surface_frame(stroke, params, s, w)),
                       "Connector-1");
    Cut                    cut(mo, 0, Transform3d::Identity(), surface_attrs());
    const ModelObjectPtrs& res = cut.perform_with_draw_stroke(stroke, params);
    REQUIRE(res.size() == 3);
    REQUIRE(all_closed(res));
    REQUIRE(count_holes(res[0], CutConnectorType::PushFit) == 2);
    REQUIRE(count_holes(res[1], CutConnectorType::PushFit) == 2);
}

// ------------------------------------------------------------------------ the curvature limit

TEST_CASE("A connector where the surface curves too much is refused before anything is cut", "[CutConnectorSurface]")
{
    SECTION("TAC Joint on a tight dome") {
        const CurvedCutSheet sheet = tight_dome();
        Model                model;
        ModelObject*         mo = surface_cube(model);
        add_tac_joint_volume(mo, surface_tac(curved_cut_sheet_point(sheet, 0., 0.), curved_cut_sheet_frame(sheet, 0., 0.)), "TAC Joint-1");
        Cut cut(mo, 0, Transform3d::Identity(), surface_attrs());
        const std::string msg = refusal_message([&]() { cut.perform_with_curved_sheet(sheet); });
        INFO(msg);
        REQUIRE(msg.find("curves too much") != std::string::npos);
        REQUIRE(mo->volumes.size() == 2); // the source object is untouched
    }
    SECTION("Pin on a tightly drawn bend") {
        DrawCutStroke stroke = top_arc(5.0, 0.6 * PI);
        REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
        REQUIRE(stroke.valid());
        const DrawCutParams params = through_params();
        const double        s      = 0.5 * stroke.length(), w = 0.5 * CUBE;
        Model               model;
        ModelObject*        mo = surface_cube(model);
        add_pushfit_volume(mo, surface_pin(draw_cut_surface_point(stroke, params, s, w), draw_cut_surface_frame(stroke, params, s, w)),
                           "Connector-1");
        Cut cut(mo, 0, Transform3d::Identity(), surface_attrs());
        const std::string msg = refusal_message([&]() { cut.perform_with_draw_stroke(stroke, params); });
        INFO(msg);
        REQUIRE(msg.find("curves too much") != std::string::npos);
        REQUIRE(mo->volumes.size() == 2);
    }
}

TEST_CASE("The footprint radius follows each connector's real size", "[CutConnectorSurface]")
{
    const double tac = cut_connector_footprint_radius(surface_tac(Vec3d::Zero(), Transform3d::Identity()));
    // The TAC Joint's cutter is about 4.4 x 3.8 mm across: a circle of about 2.9 mm round it.
    REQUIRE(tac > 2.5);
    REQUIRE(tac < 3.5);
    const CutConnector pin = surface_pin(Vec3d::Zero(), Transform3d::Identity());
    const double       r   = cut_connector_footprint_radius(pin);
    REQUIRE(r >= 0.5 * double(pin.pushfit.diameter) + double(pin.pushfit.clearance_upper));
    REQUIRE(r <= 0.5 * double(pin.pushfit.diameter) + 2.0);
    CutConnector plug = pin;
    plug.attribs.type     = CutConnectorType::Plug;
    plug.radius           = 4.f;
    plug.radius_tolerance = 0.2f;
    REQUIRE(std::abs(cut_connector_footprint_radius(plug) - 4.2) < 1e-6);
}

// ------------------------------------------------------------------------ planar regression

TEST_CASE("A flat sheet with a TAC Joint and a Pin is exactly the planar cut", "[CutConnectorSurface]")
{
    auto run = [](bool via_sheet) {
        Model        model;
        ModelObject* mo = surface_cube(model);
        add_tac_joint_volume(mo, surface_tac(Vec3d(5., -3., 0.), Transform3d::Identity()), "TAC Joint-1");
        add_pushfit_volume(mo, surface_pin(Vec3d(-6., 4., 0.), Transform3d::Identity()), "Connector-2");
        Cut                     cut(mo, 0, Transform3d::Identity(), surface_attrs());
        const ModelObjectPtrs&  res = via_sheet ? cut.perform_with_curved_sheet(CurvedCutSheet(5), 0.6) : cut.perform_with_plane(0.6);
        std::vector<BoundingBoxf3> boxes;
        std::vector<size_t>        counts;
        for (const ModelObject* o : res) {
            counts.push_back(o->volumes.size());
            for (const ModelVolume* v : o->volumes)
                boxes.push_back(v->mesh().transformed_bounding_box(v->get_matrix()));
        }
        REQUIRE(all_closed(res));
        // The planar path keeps its clipped sockets and its single Pin hole: no mouth relief.
        REQUIRE(res.size() == 4);
        REQUIRE(count_holes(res[0], CutConnectorType::PushFit) == 1);
        REQUIRE(count_holes(res[1], CutConnectorType::PushFit) == 1);
        return std::make_pair(counts, boxes);
    };
    const auto plane = run(false);
    const auto sheet = run(true);
    REQUIRE(plane.first == sheet.first);
    REQUIRE(plane.second.size() == sheet.second.size());
    for (size_t i = 0; i < plane.second.size(); ++i) {
        REQUIRE((plane.second[i].min - sheet.second[i].min).norm() < 1e-9);
        REQUIRE((plane.second[i].max - sheet.second[i].max).norm() < 1e-9);
    }
}

// ------------------------------------------------------------------ flat cut with a kerf

// The Plug and Dowel sizing GLGizmoCut3D::apply_connectors_in_model() does once perform_cut() has
// captured the kerf (the gizmo itself closes, and zeroes the thickness, before the connectors are
// baked): a Dowel (already doubled) gets `kerf` longer, a Plug gets `kerf` longer and its centre
// moves to face_lo + height / 2 along the cut normal. The cut then runs with the same kerf.
namespace {

constexpr double KERF_BASE_LEN = 10.0;

void add_flat_kerf_connector(ModelObject* mo, CutConnectorType type, double kerf)
{
    const double face_lo = -0.5 * kerf; // Centred
    double       height  = KERF_BASE_LEN;
    double       centre  = 0.0;
    if (type == CutConnectorType::Dowel) {
        height += kerf;
    } else {
        height += kerf;
        centre = face_lo + 0.5 * height;
    }
    ModelVolume* v = mo->add_volume(TriangleMesh(its_make_cylinder(1.0, 1.0)), ModelVolumeType::NEGATIVE_VOLUME);
    v->set_transformation(Geometry::translation_transform(Vec3d(2., 1., centre)) * Geometry::scale_transform(Vec3d(2.5, 2.5, height)));
    v->cut_info = ModelVolume::CutInfo(type, 0.f, 0.f);
    v->name     = type == CutConnectorType::Dowel ? "Connector-dowel" : "Connector-plug";
}

const ModelVolume* body_part(const ModelObject* o)
{
    for (const ModelVolume* v : o->volumes)
        if (v->is_model_part() && !v->cut_info.is_connector)
            return v;
    return nullptr;
}

const ModelVolume* connector_part(const ModelObject* o)
{
    for (const ModelVolume* v : o->volumes)
        if (v->is_model_part() && v->cut_info.is_connector)
            return v;
    return nullptr;
}

BoundingBoxf3 volume_box(const ModelVolume* v)
{
    REQUIRE(v != nullptr);
    return v->mesh().transformed_bounding_box(v->get_matrix());
}

} // namespace

TEST_CASE("Flat kerf cut: a Plug bridges the gap and is attached to its half", "[CutConnectorSurface][CutKerf]")
{
    for (const double kerf : { 1.0, 0.0 }) {
        Model        model;
        ModelObject* mo = surface_cube(model);
        add_flat_kerf_connector(mo, CutConnectorType::Plug, kerf);
        Cut                    cut(mo, 0, Transform3d::Identity(), ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
        const ModelObjectPtrs& res = cut.perform_with_plane(kerf);
        REQUIRE(res.size() == 2);
        REQUIRE(all_closed(res));

        const BoundingBoxf3 lower_body = volume_box(body_part(res[1]));
        const BoundingBoxf3 upper_body = volume_box(body_part(res[0]));
        const BoundingBoxf3 plug       = volume_box(connector_part(res[1]));

        // The halves' faces are the kerf's two faces.
        REQUIRE(std::abs(lower_body.max.z() + 0.5 * kerf) < 1e-4);
        REQUIRE(std::abs(upper_body.min.z() - 0.5 * kerf) < 1e-4);
        // The plug starts at (touches) the lower half's face, never floating above it ...
        REQUIRE(plug.min.z() <= lower_body.max.z() + 1e-4);
        REQUIRE(plug.min.z() >= lower_body.max.z() - 1e-4);
        // ... spans the gap, and reaches the full base length into the upper half.
        REQUIRE(plug.max.z() >= upper_body.min.z() + KERF_BASE_LEN - 1e-4);
        REQUIRE(std::abs((plug.max.z() - plug.min.z()) - (KERF_BASE_LEN + kerf)) < 1e-4);
        // The upper half carries the matching hole.
        REQUIRE(count_holes(res[0], CutConnectorType::Plug) == 1);
    }
}

TEST_CASE("Flat kerf cut: a Dowel is as long as its base length plus the kerf", "[CutConnectorSurface][CutKerf]")
{
    for (const double kerf : { 1.0, 0.0 }) {
        Model        model;
        ModelObject* mo = surface_cube(model);
        add_flat_kerf_connector(mo, CutConnectorType::Dowel, kerf);
        Cut                    cut(mo, 0, Transform3d::Identity(), surface_attrs());
        const ModelObjectPtrs& res = cut.perform_with_plane(kerf);
        REQUIRE(res.size() == 3);
        REQUIRE(all_closed(res));

        const ModelObject* dowel = separate_part(res);
        REQUIRE(dowel != nullptr);
        const BoundingBoxf3 box = volume_box(dowel->volumes.front());
        REQUIRE(std::abs((box.max.z() - box.min.z()) - (KERF_BASE_LEN + kerf)) < 1e-4);
        // It spans both sockets: each half's face is inside the dowel's length.
        REQUIRE(box.min.z() < -0.5 * kerf);
        REQUIRE(box.max.z() > 0.5 * kerf);
        REQUIRE(count_holes(res[0], CutConnectorType::Dowel) == 1);
        REQUIRE(count_holes(res[1], CutConnectorType::Dowel) == 1);
    }
}

// ------------------------------------------------------------------------ the cross-section (picking)

namespace {
indexed_triangle_set plane_cube()
{
    indexed_triangle_set its = its_make_cube(CUBE, CUBE, CUBE);
    for (Vec3f& v : its.vertices)
        v -= Vec3f::Constant(float(0.5 * CUBE));
    return its;
}
bool section_hit(const AABBMesh& tree, const Vec3d& from, const Vec3d& dir)
{
    for (const AABBMesh::hit_result& h : tree.query_ray_hits(from, dir))
        if (h.is_hit())
            return true;
    return false;
}
} // namespace

TEST_CASE("Curved cut: a pick outside the part's cross-section is refused", "[CutConnectorSurface][CurvedCut]")
{
    const CurvedCutSheet       sheet   = gentle_dome(); // 80 mm across, the cube 30 mm
    const indexed_triangle_set section = cut_surface_section(sheet.sample_sheet(CurvedCutSheet::DefaultSamples), CutSolidTest(plane_cube()), 1.0);
    REQUIRE(!section.empty());
    // The section is the sheet INSIDE the cube only, to within one split edge.
    for (const Vec3f& v : section.vertices) {
        REQUIRE(std::abs(v.x()) <= 0.5 * CUBE + 1.0);
        REQUIRE(std::abs(v.y()) <= 0.5 * CUBE + 1.0);
    }
    const AABBMesh tree(section);
    REQUIRE(section_hit(tree, Vec3d(0.37, 0.53, 50.), -Vec3d::UnitZ())); // over the part: picks (off the sample grid lines)
    REQUIRE(section_hit(tree, Vec3d(12.3, -9.1, 50.), -Vec3d::UnitZ()));
    REQUIRE(!section_hit(tree, Vec3d(25.3, 0.4, 50.), -Vec3d::UnitZ()));  // the sheet out past the part: no pick
    REQUIRE(!section_hit(tree, Vec3d(-30.3, 22.1, 50.), -Vec3d::UnitZ()));

    // And Cut refuses a connector standing on the sheet out there.
    Model        model;
    ModelObject* mo = surface_cube(model);
    add_tac_joint_volume(mo, surface_tac(curved_cut_sheet_point(sheet, 25., 0.), curved_cut_sheet_frame(sheet, 25., 0.)), "TAC Joint-1");
    Cut               cut(mo, 0, Transform3d::Identity(), surface_attrs());
    const std::string msg = refusal_message([&]() { cut.perform_with_curved_sheet(sheet); });
    INFO(msg);
    REQUIRE(msg.find("outside the part") != std::string::npos);
    REQUIRE(mo->volumes.size() == 2);
}

TEST_CASE("Drawn cut: a pick outside the part's cross-section is refused", "[CutConnectorSurface][DrawCut]")
{
    DrawCutStroke stroke = top_arc(40.0, std::asin(15.0 / 40.0));
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    const DrawCutParams        params = through_params(); // 5 mm of extension above the top face
    const indexed_triangle_set cube   = plane_cube();
    BoundingBoxf3              bbox;
    for (const Vec3f& v : cube.vertices)
        bbox.merge(v.cast<double>());
    const indexed_triangle_set cutter = draw_cut_cutter_solid(stroke, params, bbox, 0.0, &cube);
    REQUIRE(!cutter.empty());
    const indexed_triangle_set section = cut_surface_section(cutter, CutSolidTest(plane_cube()), 1.0);
    REQUIRE(!section.empty());
    REQUIRE(section.indices.size() < 200000);
    for (const Vec3f& v : section.vertices)
        REQUIRE(v.z() <= 0.5 * CUBE + 1.0); // nothing of the extension above the top face

    const AABBMesh tree(section);
    const double   s = 0.5 * stroke.length();
    const Vec3d    n = draw_cut_surface_normal(stroke, params, s, 0.0);
    // Halfway down the strip, inside the part: picks.
    const Vec3d inside = draw_cut_surface_point(stroke, params, s, 0.5 * CUBE);
    REQUIRE(section_hit(tree, inside + 10.0 * n, -n));
    // On the extension, 3 mm above the top face: the strip is there, the part is not - no pick.
    const Vec3d outside = draw_cut_surface_point(stroke, params, s, -3.0);
    REQUIRE(outside.z() > 0.5 * CUBE);
    REQUIRE(!section_hit(tree, outside + 10.0 * n, -n));

    // And Cut refuses a Pin standing on the extension.
    Model        model;
    ModelObject* mo = surface_cube(model);
    add_pushfit_volume(mo, surface_pin(outside, draw_cut_surface_frame(stroke, params, s, -3.0)), "Connector-1");
    Cut               cut(mo, 0, Transform3d::Identity(), surface_attrs());
    const std::string msg = refusal_message([&]() { cut.perform_with_draw_stroke(stroke, params); });
    INFO(msg);
    REQUIRE((msg.find("outside the part") != std::string::npos || msg.find("not on the cut surface") != std::string::npos));
    REQUIRE(mo->volumes.size() == 2);
}

TEST_CASE("Curved cut: the split's own cut face is pickable past the sheet's control grid", "[CutConnectorSurface][CurvedCut]")
{
    // A sheet whose control grid (16 mm across) is smaller than the part (30 mm): the cut runs on
    // past the grid, and that is the face the user sees and clicks.
    CurvedCutSheet sheet(5);
    sheet.set_half_size(8.0);
    sheet.at(2, 2) = 2.0;
    const indexed_triangle_set cube = plane_cube();
    indexed_triangle_set       lower;
    REQUIRE(curved_cut_split(cube, sheet, nullptr, &lower));
    REQUIRE(!lower.empty());

    const indexed_triangle_set face = curved_cut_face_of(lower, sheet, 0.0);
    REQUIRE(!face.empty());
    REQUIRE(face.indices.size() < lower.indices.size()); // the face, not the whole half
    const AABBMesh face_tree(face);
    for (const Vec3d& p : { Vec3d(0.37, 0.53, 50.), Vec3d(5.3, -4.1, 50.), Vec3d(12.3, 9.1, 50.), Vec3d(-13.7, -12.2, 50.) })
        REQUIRE(section_hit(face_tree, p, -Vec3d::UnitZ()));
    // Off the part: nothing.
    REQUIRE(!section_hit(face_tree, Vec3d(20.3, 0.4, 50.), -Vec3d::UnitZ()));
    // The sheet's own sample mesh stops at its grid, so the old pick target missed out there.
    const indexed_triangle_set grid_section = cut_surface_section(sheet.sample_sheet(CurvedCutSheet::DefaultSamples), CutSolidTest(plane_cube()), 1.0);
    const AABBMesh             grid_tree(grid_section);
    REQUIRE(!section_hit(grid_tree, Vec3d(12.3, 9.1, 50.), -Vec3d::UnitZ()));

    // With a kerf the lower half's face sits half the thickness below the sheet.
    indexed_triangle_set lower_kerf;
    REQUIRE(curved_cut_split(cube, sheet, nullptr, &lower_kerf, CurvedCutSheet::CutSamples, 1.0));
    const indexed_triangle_set face_kerf = curved_cut_face_of(lower_kerf, sheet, -0.5);
    REQUIRE(!face_kerf.empty());
    REQUIRE(section_hit(AABBMesh(face_kerf), Vec3d(12.3, 9.1, 50.), -Vec3d::UnitZ()));
}
