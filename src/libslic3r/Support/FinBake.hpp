#pragma once

// "Bake fins": turns an object's live support fins (Support/Fins.hpp, printed as support by EdgeSlicer
// only) into real geometry that any slicer prints - Bambu Studio, OrcaSlicer, PrusaSlicer - because a
// project file loses the EdgeSlicer-only settings in all of them. Modelled on the stabilizer bake
// (StabilizerBake.hpp).
//
// Credit: the technique is Slant3D's "designed-in supports", as specified and popularised by Support
// Fins (gittrahan/support-fins, printfins.com, MIT), whose selling point is exactly this: the fins in the
// model file. EdgeSlicer's own implementation; no code was taken from that project or from the
// OrcaSlicer fin pull requests (#15839, #17234).
//
// The bake plans the fins exactly as the live generator does (plan_fins, painted fin spots included),
// slices them at the object's own layers (slice_fins), builds the sections as one closed mesh
// (FinMesh.hpp) and puts it in the model in one of two places:
//
//  * an extra part of the same object (the default): parts of one object are unioned layer by layer,
//    so the tines fuse into the part exactly as the live tines do, and the fins move with the part.
//    The fin gap is kept at FIN_BAKE_PART_MIN_GAP at least so the walls themselves stay apart. The part
//    has no settings of its own: it prints with the object's walls, infill and filament, in the same
//    region, so nothing separates the tines from the part.
//  * a separate object "<name> fins": its own settings (no supports, no brim, the support filament),
//    and parts of different objects never fuse, so the tines stop at the part's outline and only catch
//    it. It does not move with the part.
//
// Fins win the space from other support, live and baked alike: the live fins cut the support around
// them back by the fin gap (displace_supports_for_fins), and baked fins are model geometry, which the
// support generated afterwards keeps its distance from. So the bake plans the fins with nothing but
// support blockers in the way, as the live fins do; the blockers (volumes and paint) still remove the
// fins they touch.
//
// Either way the source object's live support_fins is switched off, so the fins are not printed twice.
// The geometry is planned from the sliced object, so the plate must be sliced first; nothing links the
// bake to its source afterwards. Its tines are one layer tall at the layer heights the object was sliced
// with, so the baked fins must be sliced at those heights.

#include "FinMesh.hpp"
#include "Fins.hpp"

#include "../TriangleMesh.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class Model;
class ModelObject;
class PrintObject;

enum class FinBakePlacement {
    PartOfObject = 0,
    SeparateObject,
};

// A part of the same object fuses with the part where it touches; its walls keep at least this gap.
// Twice the slicer's gap closing (2 x slice_closing_radius, about 0.1 mm) with margin.
static constexpr double FIN_BAKE_PART_MIN_GAP = 0.15; // mm
// The default baked tine width in lines: a one-line strip is dropped by the classic wall generator
// (Bambu Studio's default), which needs about two lines to put a loop round it.
static constexpr double FIN_BAKE_WIDE_TINE_LINES = 2.5;

enum class FinBakeTines {
    // FIN_BAKE_WIDE_TINE_LINES lines wide: prints with any wall generator, in any slicer.
    Wide = 0,
    // One line wide, exactly the live tine: needs Arachne walls.
    OneLine,
};

struct FinBakeOptions
{
    FinBakePlacement placement = FinBakePlacement::PartOfObject;
    FinBakeTines     tines     = FinBakeTines::Wide;
};

// The fin gap a placement bakes with, from the gap the object was sliced with.
double fin_bake_gap(FinBakePlacement placement, double sliced_gap);
// The tine width the options bake with, mm, for the object's support line width.
double fin_bake_tine_width(FinBakeTines tines, double line_width);

struct FinBakeResult
{
    // SeparateObject: around the origin, each instance offset puts it where its source instance's fins
    // stand (world frame with the instance offset taken out; rotation and scale are in the vertices).
    // PartOfObject: in the source object's own coordinates, for a new part.
    indexed_triangle_set          mesh;
    std::vector<Vec3d>            instance_offsets;
    // What was baked, in the print frame: the plan, its sections per object layer and the mesh bands.
    fins::FinPlan                 plan;
    std::vector<fins::FinSlice>   slices;
    std::vector<fins::FinMeshLayer> bands;
    fins::FinReport               report;
    fins::FinMeshReport           mesh_report;
    // Tines that made it into the mesh.
    size_t                        tines            = 0;
    double                        fin_top          = 0.;  // the tallest fin, mm
    // The settings baked with.
    double                        gap              = 0.;
    double                        sliced_gap       = 0.;
    double                        tine_width       = 0.;
    double                        layer_height     = 0.;
    // The source's support filament, 1-based; 0 = the object's own.
    int                           support_filament = 0;
    // Why there is no mesh (or, for plan_fin_bake, no fins), when there is none.
    std::string                   error;
};

// Plans and slices the fins of a sliced PrintObject (its layers built; posSlice is enough), without
// building the mesh: what the dialog's preview shows. Safe off the UI thread while nothing reslices the
// object.
FinBakeResult plan_fin_bake(const PrintObject &object, const FinBakeOptions &options);

// plan_fin_bake plus the mesh, in the frame the placement needs.
FinBakeResult bake_fins(const PrintObject &object, const FinBakeOptions &options);

// Puts a bake into `model`: a new part of `source` (PartOfObject) or a new object after `source`'s
// (SeparateObject), and switches `source`'s live fins off. Returns `source` for a part or the new object;
// null when the result has no mesh.
ModelObject *apply_fin_bake(Model &model, ModelObject &source, const FinBakeResult &result, const FinBakeOptions &options);

// The name the separate object gets, and the part's name.
std::string fin_bake_object_name(const ModelObject &source);
static constexpr const char *FIN_BAKE_PART_NAME = "Fins";

} // namespace Slic3r
