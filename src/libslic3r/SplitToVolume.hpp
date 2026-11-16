#ifndef slic3r_SplitToVolume_hpp_
#define slic3r_SplitToVolume_hpp_

// SPLIT TO FIT, phase 1 (tests/design_split_to_build_volume.md, owner decisions D1-D17 accepted
// 2026-11-15): plan an axis-aligned grid of cut planes that splits one object into parts that each
// fit the current printer's usable volume, and carry the plan out as plain planar cuts.
//
// Pure libslic3r: no GUI, no globals. The planner works on plain values (a mesh in the grid frame,
// a printer volume, options) so every rule is unit tested; the gizmo and the job only draw and apply
// what comes back.
//
// THE GRID FRAME. In phase 1 it is the world frame: the object's model parts with the instance
// transformation applied. The grid angle (P4) will turn it about Z; nothing below assumes it is not
// turned except split_apply(), which says so where it matters.
//
// LATER PHASES BUILD ON THIS OUTPUT WITHOUT RESHAPING IT:
//  - P2 (dowels) adds connector sites per cut face: SplitFace is already one plane's section clipped
//    to one pair of neighbouring cells, which is exactly where a site belongs.
//  - P3 (plane adjustment) moves SplitPlane::pos away from SplitPlane::nominal; pinned planes are
//    already kept where the user put them.
//  - The cut-face labels (later phase) emboss SplitFace::mate_code on both faces of a pair: every
//    face has a stable key and a code the two mating faces share.

#include "BoundingBox.hpp"
#include "ExPolygon.hpp"
#include "Point.hpp"

#include <admesh/stl.h>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;
class Model;
class ModelObject;

// ---- the printer's volume -------------------------------------------------------------------------

// The values the usable volume is worked out from, in mm (plate-local, unscaled).
struct SplitPrinterVolume
{
    Pointfs                   printable_area;           // bed outline
    std::vector<BoundingBoxf> exclude_boxes;            // bed_exclude_area, one box per excluded piece
    double                    printable_height{ 0. };
    std::vector<Pointfs>      extruder_areas;           // extruder_printable_area (H2D / H2C), may be empty
    std::vector<double>       extruder_heights;         // extruder_printable_height, may be empty
};

// printable_area, bed_exclude_area, printable_height, extruder_printable_area and
// extruder_printable_height of a printer (or full) config. Missing keys are left empty.
SplitPrinterVolume split_printer_volume_from_config(const ConfigBase &config);

enum class SplitLayout : int {
    // Every part on an axis the same length (default, D5): leaves slack in every part.
    Even = 0,
    // Full usable length first, the remainder last; a remainder thinner than
    // SplitOptions::min_part_thickness pulls the last plane in.
    FullSizeFirst = 1,
};

// A plane the user moved by hand. It is kept where it was put (clamped inside the model).
struct SplitPinnedPlane
{
    int    axis{ 0 };   // 0 X, 1 Y, 2 Z
    int    index{ 0 };  // 0-based, in order of position along the axis
    double pos{ 0. };   // grid frame, mm
};

struct SplitOptions
{
    // D2: 5 mm each side in X / Y (brim / skirt allowance and the arranger's own shrink), 5 mm at the top.
    double margin_xy{ 5. };
    double margin_z{ 5. };
    // Axes the grid may cut along. An allowed axis is only cut where the model is larger than the usable
    // size (D9: Z too, like X and Y). A disallowed axis that is too long gives parts that do not fit.
    std::array<bool, 3> allow_axis{ { true, true, true } };
    SplitLayout layout{ SplitLayout::Even };
    // Full size first: the last part on an axis is never thinner than this.
    double min_part_thickness{ 10. };
    // D14: keep to the bed area every nozzle reaches (H2D / H2C). Off by default.
    bool nozzle_reach{ false };
    // Planes the user moved; empty for a fresh plan.
    std::vector<SplitPinnedPlane> pinned;
};

// What a part may measure, and where on the bed that box came from.
struct SplitUsableVolume
{
    bool         valid{ false };
    // The bed rectangle after shape, exclusions and nozzle reach, before the margins (mm, plate-local).
    BoundingBoxf bed_rect;
    Vec3d        size{ Vec3d::Zero() };   // usable X, Y, Z after the margins
    Vec3d        printer_size{ Vec3d::Zero() }; // bounding box of the bed outline and the printable height
    bool         bed_not_rectangular{ false };   // circle: inscribed square; other shapes: bounding box
    bool         bed_round{ false };
    bool         reduced_by_exclusion{ false };
    bool         reduced_by_reach{ false };
    // True when the printer has nozzle-only strips, so the reach option is worth offering (D14).
    bool         has_reach_option{ false };
};

SplitUsableVolume split_usable_volume(const SplitPrinterVolume &printer, const SplitOptions &options);

// ---- the plan -------------------------------------------------------------------------------------

struct SplitPlane
{
    int    axis{ 0 };           // 0 X, 1 Y, 2 Z
    int    index{ 0 };          // 0-based within its axis, in order of position
    double pos{ 0. };           // grid frame, mm: where the cut goes
    double nominal{ 0. };       // where the layout put it (P3's adjustment moves pos away from this)
    bool   pinned{ false };     // the user moved it
    double section_area{ 0. };  // mm2 of material the plane cuts through (0: it misses the model)
    // "X1", "Y2", "Z1": the axis letter and the 1-based index.
    std::string name() const;
};

// A cell of the grid: 0-based index per axis.
using SplitCell = std::array<int, 3>;

struct SplitPart
{
    SplitCell     cell{ { 0, 0, 0 } };
    // D7: "X2-Y1-Z1", axes that are not cut left out; empty when nothing is cut.
    std::string   name;
    // The material of the model inside the cell (grid frame). Undefined for an empty cell.
    BoundingBoxf3 bbox;
    bool          empty{ true };
    bool          fits{ false };
    // Ids of the cut faces of this part (SplitFace::id).
    std::vector<int> faces;

    Vec3d size() const { return bbox.defined ? bbox.size() : Vec3d::Zero(); }
};

// One cut face pair: plane `plane`'s section clipped to the two cells it separates. Both parts get a
// face here; they mate. Stable for a given grid: the same planes over the same model give the same
// ids, keys and codes.
struct SplitFace
{
    int         id{ -1 };          // index in SplitPlan::faces
    int         plane{ -1 };       // index in SplitPlan::planes
    SplitCell   cell_below{ { 0, 0, 0 } }; // the cell on the negative side of the plane
    SplitCell   cell_above{ { 0, 0, 0 } }; // the cell on the positive side
    int         part_below{ -1 };  // index in SplitPlan::parts
    int         part_above{ -1 };
    double      area{ 0. };        // mm2 of material on the face
    // Stable, translation-independent key: plane name, then the cell along the other cut axes,
    // e.g. "X1:Y2-Z1" (just "X1" when no other axis is cut).
    std::string key;
    // The short code both mating faces carry, for the later embossed labels: plane name and the
    // 1-based face number on that plane, e.g. "X1.2".
    std::string mate_code;
    // Human readable: "X1-Y1 | X2-Y1" (the two part names either side).
    std::string mate_label;
    // P2 adds the connector sites of this face here.
};

struct SplitWarning
{
    enum class Kind : int {
        // Preview
        NoUsableVolume,      // the margins leave nothing (value: unused)
        TooManyParts,        // value: the number of cells; the plan is not made (see SPLIT_MAX_CELLS)
        PartDoesNotFit,      // part: which; the axis was not allowed to be cut, or a plane was moved too far
        BedNotRectangular,   // usable area reduced to a rectangle inside the bed
        BedExclusion,        // usable area reduced by the bed's exclusion areas
        NozzleReach,         // usable area reduced to what every nozzle reaches
        ModelNotWatertight,  // value: number of open edges in the model
        HollowModel,         // plane: where it was seen; value: estimated wall thickness, mm (D6)
        // After Apply
        LoosePieces,         // part: which; value: number of separate pieces (D15: kept, warned)
        OpenEdges,           // part: which; value: number of open edges after the cut
    };
    Kind   kind{ Kind::NoUsableVolume };
    int    plane{ -1 };
    int    part{ -1 };
    double value{ 0. };
};

struct SplitPlan
{
    bool               valid{ false };
    SplitUsableVolume  usable;
    BoundingBoxf3      model_bbox;           // grid frame
    std::array<int, 3> counts{ { 1, 1, 1 } };   // cells per axis
    std::array<bool, 3> needs_cut{ { false, false, false } }; // the model is longer than the usable size
    std::vector<SplitPlane>   planes;        // X planes first, then Y, then Z, each in order of position
    std::vector<SplitPart>    parts;         // every cell, flat index: x + nx * (y + ny * z)
    std::vector<SplitFace>    faces;
    std::vector<SplitWarning> warnings;

    size_t      cell_count() const { return parts.size(); }
    size_t      part_count() const;          // non-empty cells
    size_t      parts_not_fitting() const;
    int         cell_index(const SplitCell &cell) const { return cell[0] + counts[0] * (cell[1] + counts[1] * cell[2]); }
    // Positions of the planes of one axis, in order.
    std::vector<double> plane_positions(int axis) const;
    // Index into `planes` of plane `index` of `axis`, or -1.
    int         plane_index(int axis, int index) const;
};

// More cells than this (a tiny usable volume, a typo in the margins) is refused rather than planned.
constexpr int SPLIT_MAX_CELLS = 1000;

// D7 cell name for `counts` cells per axis: "X2-Y1-Z1", uncut axes left out.
std::string split_cell_name(const SplitCell &cell, const std::array<int, 3> &counts);

// The plan for `mesh` (grid frame). `throw_on_cancel` is called now and then and may throw.
SplitPlan split_plan_grid(const indexed_triangle_set &mesh, const SplitUsableVolume &usable, const SplitOptions &options,
                          std::function<void()> throw_on_cancel = nullptr);

// The object's model parts merged into one mesh in the grid frame (instance `instance` applied).
indexed_triangle_set split_object_mesh(const ModelObject &object, int instance);

// ---- apply ----------------------------------------------------------------------------------------

struct SplitApplyResult
{
    // Owns the parts. Each part is one object with one instance, sitting where it was in the
    // uncut object (world frame), with its assembled pose set.
    std::unique_ptr<Model>    model;
    std::vector<ModelObject*> parts;      // owned by `model`, in plan part order
    std::vector<int>          part_index; // per entry of `parts`: index in SplitPlan::parts
    std::vector<SplitWarning> warnings;   // the after-Apply ones (LoosePieces, OpenEdges)
    bool                      canceled{ false };

    SplitApplyResult();
    ~SplitApplyResult();
    SplitApplyResult(SplitApplyResult &&) noexcept;
    SplitApplyResult &operator=(SplitApplyResult &&) noexcept;
};

// Carries the plan out on a copy of `object` (only instance `instance` is kept): plain planar cuts
// through Cut::perform_with_plane(), in bisection order (the middle plane of the first axis that
// still crosses a piece, then both halves), painting carried. The source object is never modified.
// `progress` gets (cuts done, cuts in all); `canceled` is polled before every cut and stops the
// apply (result.canceled, no parts). Names per D7: "<object name> X2-Y1-Z1".
//
// Cut links: a plain split is not a linked cut group (D10, no dowels in P1). An object that was
// already part of a linked group hands its id on: every part gets it, the check sum grown by
// parts - 1, which is what a series of plain cuts on that object would have done.
SplitApplyResult split_apply(const ModelObject &object, int instance, const SplitPlan &plan,
                             std::function<bool()> canceled = nullptr,
                             std::function<void(int, int)> progress = nullptr);

} // namespace Slic3r

#endif // slic3r_SplitToVolume_hpp_
