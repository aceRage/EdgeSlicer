#pragma once

// Support fins: breakaway walls with one-layer tines for parts printed tilted onto an edge or a corner.
//
// Credit: the technique is Slant3D's "designed-in supports" (a part printed tipped onto an edge, held by
// thin walls standing beside it with one-layer horizontal tines fused into it), as specified and
// popularised by Support Fins (gittrahan/support-fins, printfins.com, MIT). This is EdgeSlicer's own
// native implementation, written from the published description and observed behaviour; no code was
// taken from that project or from the OrcaSlicer fin pull requests (#15839, #17234).
//
// A fin is a thin vertical wall in the plane of the direction the part would tip: it stands on the build
// plate beside the part, its inner edge following the part's side at the fin gap, and reaches `depth`
// out from the part on the bed. Every few millimetres a tine - one bead, one object layer tall - bridges
// the gap and runs `tine_depth` into the part's section on that layer, so the object's own perimeter
// fuses to it and the part cannot fall away from the wall. The tines snap off by bending.
//
// The fins print as support material (support filament, speed and flow), live, on the object's own
// layers: they are an addition like the side stabilizers (Stabilizers.hpp), with or without Enable
// supports, never a support type. Tines are always on an object layer's print_z with its height:
// with supports off, between supports or with an independent support layer height, the fins insert
// their own support layers at the object's heights, so nothing prints twice.
//
// Fins win the space (owner, 2026-11-27): fins hold most tall parts better than support does, so the
// normal and tree support the generators made is cut back from the fins' footprint by the fin gap
// (displace_supports_for_fins, run before the side stabilizers), and the fins are planned exactly as
// the bake plans them, with nothing but support blockers in their way. The side stabilizers are not
// cut back: the fins keep out of every stabilizer whose Z range overlaps theirs, and a tine that would
// cross one is left out (and counted), never moved to another height.
//
// The planner and the slicer work on plain layer outlines and a FinSettings, not on a PrintObject, so
// the bake (PR 2) and the tests run them the same way.

#include "../ExPolygon.hpp"
#include "../Polyline.hpp"
#include "../PrintConfig.hpp"
#include "Stabilizers.hpp"

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class Model;
class ModelObject;
class PrintObject;
class PrintObjectConfig;

namespace fins {

// Tines on every other layer up to this height when the base rows are on, mm.
static constexpr double TINE_BASE_ROWS_HEIGHT = 2.;
// The fin's foot: on its first FOOT_HEIGHT mm the fin grows by up to FOOT_GROWTH mm on each side (45 degrees).
static constexpr double FOOT_HEIGHT = 1.;
static constexpr double FOOT_GROWTH = 1.;
// A part whose centre of mass lies less than this beyond its bed contact is balanced: fins on both sides.
static constexpr double LEAN_MIN = 0.5;
// A tine bridges at most the fin gap plus this, mm; a wider opening (the part drawing away above a
// fin) gets no tine there.
static constexpr double TINE_MAX_EXTRA_SPAN = 1.;

struct FinSettings
{
    SupportFinsMode mode           = sfmOff;
    double          thickness      = 1.2;   // wall thickness, mm
    double          depth          = 8.;    // how far a fin reaches out from the part on the bed, mm
    double          height         = 0.;    // fin height above the bed, mm; 0 = the part's centre of mass height
    double          spacing        = 15.;   // largest distance between neighbouring fins along the part, mm
    double          gap            = 0.2;   // fin to part, beside it and under it, mm
    double          tine_spacing   = 3.;    // vertical distance between tines, mm
    double          tine_depth     = 0.3;   // how far a tine reaches into the part, mm
    bool            tine_base_rows = true;  // tines every other layer up to TINE_BASE_ROWS_HEIGHT
    bool            lean_side_only = true;
    double          line_width     = 0.42;  // the support line width, mm

    bool auto_fins() const { return mode == sfmAuto; }
    bool painted_fins() const { return mode != sfmOff; }

    // `support_line_width` is the object's support material flow width, mm.
    static FinSettings from_config(const PrintObjectConfig &cfg, double support_line_width);
};

FinSettings settings_of(const PrintObject &object);

// One object layer: its top, its height and its outline (Layer::lslices). Sorted by print_z.
struct FinLayer
{
    double            print_z = 0.;
    double            height  = 0.;
    const ExPolygons *islands = nullptr;
};

std::vector<FinLayer> layers_of(const PrintObject &object);

// One fin, in its own frame: its centre line runs from `origin` along `dir` (unit, away from the part);
// t is the distance along it. Nothing of the fin lies behind the origin.
struct Fin
{
    Vec2d  origin   = Vec2d::Zero();
    Vec2d  dir      = Vec2d(1., 0.);
    // The part's outermost point on the centre line at the lowest layer the fin's band meets the part.
    double edge     = 0.;
    // The fin's outer end on the bed: edge + gap + depth.
    double reach    = 0.;
    double top_z    = 0.;
    bool   painted  = false;
    // A painted fin must stand at this height (the painted spot's), mm; -1 for an automatic fin.
    double need_z   = -1.;

    Vec2d  perp() const { return Vec2d(-dir.y(), dir.x()); }
    Vec2d  at(double t) const { return origin + dir * t; }
    // The outer end at height z: straight up, then a 45 degree slope that meets the part's edge at top_z.
    double outer_at(double z) const;
    // Half the wall thickness at height z, the foot included.
    double half_thickness_at(const FinSettings &settings, double z) const;
};

struct FinPlan
{
    std::vector<Fin>    fins;
    // Indices of the object layers that carry tines (every fin that stands there gets one).
    std::vector<size_t> tine_layers;
};

// What the planner and the slicer did, for the warnings and the tests.
struct FinReport
{
    // Refused for the object: a raft (the fins would stand on it, not on the bed).
    bool               refused_raft          = false;
    bool               leaning               = false;
    Vec2d              lean                  = Vec2d::Zero(); // centre of mass minus the bed contact's centre, mm
    size_t             auto_fins             = 0;
    size_t             painted               = 0;  // painted fin spots
    size_t             painted_placed        = 0;
    size_t             painted_on_auto       = 0;  // an automatic fin already stands there
    bool               painted_without_paint = false;
    std::vector<Vec3d> unreachable;                // painted spots no fin standing on the bed reaches
    size_t             blocked               = 0;  // fins removed by painted support blockers
    size_t             tines                 = 0;
    size_t             tines_skipped         = 0;  // tines left out where other support prints
    size_t             fins_standing         = 0;  // fins that stand on the bed (have a first layer)
    // The live generator's count of what other support took (it slices the plan once without the other
    // support too): fins that no longer stand and tines that are not printed, whatever the reason.
    size_t             fins_lost             = 0;
    size_t             tines_lost            = 0;
    // How far the fins reach beyond the object's own footprint (its outline's bounding box), mm.
    double             beyond_footprint      = 0.;
};

// The object layers that carry tines, up to `top_z`: never the first layer and never two in a row; every
// other layer up to TINE_BASE_ROWS_HEIGHT with the base rows, then one every tine_spacing.
std::vector<size_t> tine_layers(const std::vector<FinLayer> &layers, const FinSettings &settings, double top_z);

// The fins for the layers: automatic fins (Auto) on the side the part leans to - both sides of a balanced
// part, or of any part without lean_side_only - spread along the part, plus one fin through every painted
// spot (Auto or Painted) that no automatic fin already holds.
FinPlan plan_fins(const std::vector<FinLayer> &layers, const FinSettings &settings,
                  const std::vector<stabilizers::PaintedSpot> &painted = {}, FinReport *report = nullptr);

// One object layer of fins: the wall sections and the tines (one-bead paths, with their footprints).
struct FinSlice
{
    ExPolygons body;
    Polylines  tines;
    ExPolygons tine_areas;
};

// The plan's sections at the layers. Each fin layer stands on the one below (it never grows), keeps the
// gap off the part on its layer and on the layers up to the gap above, and keeps out of `obstacles[i]`
// (other support on overlapping Z ranges, blocker volumes) grown by half a line. A fin whose band touches
// `blockers[i]` (painted support blockers) on any layer is left out whole. Either may be null.
std::vector<FinSlice> slice_fins(const std::vector<FinLayer> &layers, const FinSettings &settings, const FinPlan &plan,
                                 const std::vector<ExPolygons> *obstacles, const std::vector<ExPolygons> *blockers,
                                 FinReport *report, const std::function<void()> &throw_if_canceled);

// The painted fin spots of a PrintObject (paint state SUPPORT_FIN, one spot per patch).
std::vector<stabilizers::PaintedSpot> painted_fin_spots(const PrintObject &object);

// Per object layer: the support blocker volumes (`volumes`, obstacles for slice_fins) and the painted
// support blockers (`painted`, which remove the fins they touch).
void fin_blockers(const PrintObject &object, std::vector<ExPolygons> &volumes, std::vector<ExPolygons> &painted);

// The fins' footprint per object layer (walls and tines), planned and sliced with nothing but support
// blockers in the way: what the live fins print with no stabilizers about, and what the bake builds.
std::vector<ExPolygons> fin_footprints(const PrintObject &object, const std::function<void()> &throw_if_canceled);

// The objects whose support fins are on (by their own config or the print's): an export for Bambu Studio
// drops the setting, so they would print there without fins.
std::vector<std::string> objects_with_live_fins(const Model &model, const DynamicPrintConfig &print_config);

} // namespace fins

// Generates the support fins of `object` into its support layers, inserting support layers at object
// layer heights where none exist. No-op unless support_fins is on; refused (report.refused_raft) with a
// raft. Runs inside the support step, after the support generators and the side stabilizers.
fins::FinReport generate_fin_supports(PrintObject &object, const std::function<void()> &throw_if_canceled);

// Fins win the space: cuts the support the generators made (support_fills, interface partitions, islands
// and tree areas) back from the fins' footprint by the fin gap plus a support line, on every support
// layer whose Z range overlaps a fin layer. Run after the support generators and before the side
// stabilizers and generate_fin_supports. No-op unless support_fins is on (fins off: the support is
// untouched); nothing with a raft. Returns the number of support extrusions cut or dropped.
size_t displace_supports_for_fins(PrintObject &object, const std::function<void()> &throw_if_canceled);

} // namespace Slic3r
