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
//  - P2 (dowels, built): connector sites per cut face (SplitFace::sites, SplitPlan::sites), placed
//    by split_plan_grid() when SplitOptions::dowels is on and baked by split_apply().
//  - P3 (plane adjustment, built): with SplitOptions::adjust_planes, split_plan_grid() moves each
//    free plane up to adjust_limit mm away from SplitPlane::nominal to a cheaper place to cut
//    (split_section_cost(), design 3.5); pinned planes stay where the user put them. P3 also adds the
//    part checks (very small part, no flat face to stand on) to the preview and to split_apply().
//  - P5 (loose parts and cut-face labels, built; design 7.5 and "P5 as built"): every section (grid
//    cell) is split into its pieces, the connected pieces of material inside it (SplitPiece). Each
//    piece is its own part with its own size, volume and checks, and Apply makes each one an object
//    of its own. Where two pieces meet on a cut face they form a mating pair (SplitMate): dowels are
//    planned per pair, and with SplitOptions::labels both pieces get the same code recessed into
//    their patch of the face (SplitLabel), laid out so it reads right on each side.

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

// ---- automatic dowels (P2, design 3.6) ------------------------------------------------------------

// D16: only the loose, symmetric connectors (no Plug / Snap in v1).
enum class SplitConnectorType : int {
    Dowel = 0,   // the Cut tool's Dowel: a loose prism, half in each part
    Pin   = 1,   // the push-fit Pin (PushFit.hpp): a loose printed pin
};

// The Cut tool's connector shapes for a Dowel (a Pin is always round).
enum class SplitDowelShape : int { Round = 0, Square = 1, Hexagon = 2, Triangle = 3 };

struct SplitDowelOptions
{
    // The panel turns dowels on by default (D3); the planner's own default is off, so a plan made
    // without asking for them is exactly P1's.
    bool               enabled{ false };
    SplitConnectorType type{ SplitConnectorType::Dowel };
    SplitDowelShape    shape{ SplitDowelShape::Round };
    // D3: 6 mm diameter, 20 mm long (10 mm into each part), up to 3 per cut face.
    double diameter{ 6. };
    double length{ 20. };
    int    max_per_face{ 3 };
    // No dowel centre closer than wall_margin + footprint radius to the face's outline.
    double wall_margin{ 3. };
    // Dowels on one face at least this far apart, centre to centre.
    double min_spacing{ 20. };
    // Radial clearance of each hole (mm per side); the depth tolerance is the Cut tool's default.
    double tolerance{ 0.1 };
    double depth_tolerance{ 0.1 };
    // A face with no room for the full size tries 0.75 and 0.5 of it (never below 2 mm diameter).
    bool   scale_down{ true };
    // No dowel body (with its hole) comes closer than this to another cut plane (design 3.6, R1, R6).
    double corner_keepout{ 2. };
    // Material left beyond the end of a hole, along the dowel (the depth check).
    double min_material{ 1.5 };
};

// One dowel site on a cut face. The dowel straddles the plane: half_length into each part.
struct SplitSite
{
    int         face{ -1 };       // index in SplitPlan::faces
    int         plane{ -1 };      // index in SplitPlan::planes
    int         island{ 0 };      // which patch of the mating pair, 0 = the largest
    int         mate{ -1 };       // P5: index in SplitPlan::mates (the two pieces it joins)
    Vec3d       pos{ Vec3d::Zero() }; // grid frame, on the plane
    double      diameter{ 0. };   // nominal (the dowel itself; holes add the tolerance)
    double      half_length{ 0. };
    double      footprint_radius{ 0. }; // the hole's radius round the axis, mm (labels keep clear of it)
    bool        shortened{ false };     // the depth check made it shorter than asked
    bool        scaled_down{ false };   // smaller than asked, to fit a small face
    bool        manual{ false };        // P4: added by a click (SplitManualSite), not by the planner
    // "X1.2-1": the mating pair's code and the 1-based dowel number. Holes are "<name>_A" / "_B",
    // the dowel object "<object>-Dowel-<name>" ("-Pin-" for a Pin).
    std::string name;
};

// ---- plane adjustment (P3, design 3.5) ------------------------------------------------------------

// The weights of the section cost and the hard rules, in one place so tests can pin them and a retune
// is one edit. cost = small * n_small + nodowel * f_nodowel + busy * f_busy + ragged * f_ragged + away * f_away.
struct SplitAdjustConstants
{
    double w_small{ 1.0 };    // per island too small for a dowel and its wall margin
    double w_nodowel{ 2.0 };  // share of the section's material that cannot take a dowel (dowels on only)
    double w_busy{ 1.0 };     // how fast the section changes around the plane (detail, a sloped or flat face)
    double w_ragged{ 0.5 };   // how frilly the outline is compared with a circle of the same area
    double w_away{ 0.2 };     // distance from the plane's nominal position, as a share of the limit
    double step{ 1.0 };       // mm between candidate positions
    // Rule 3: no plane within `jump_distance` of a place where the section changes by more than
    // `jump_ratio` of itself (a cut that shaves a flat face leaves a paper-thin skin).
    double jump_ratio{ 0.30 };
    double jump_distance{ 1.0 };
    // An axis with more planes than this is left as laid out (the search slices every candidate).
    int    max_planes_per_axis{ 64 };
};

// The terms of one candidate position's cost (design 3.5), unweighted, and the weighted total.
struct SplitSectionCost
{
    int    small_islands{ 0 };
    double nodowel{ 0. };
    double busy{ 0. };
    double ragged{ 0. };
    double away{ 0. };
    double area{ 0. };   // mm2 of material on the section
    double total{ 0. };
};

// Why adjustment moved a plane (SplitWarning::detail of PlaneMoved).
enum class SplitAdjustReason : int {
    Better        = 0,   // a cheaper place overall
    ThinPieces    = 1,   // fewer small, thin pieces on the cut face
    DowelRoom     = 2,   // more of the face has room for a dowel
    FineDetail    = 3,   // away from fine detail (the section changes fast there)
    RaggedOutline = 4,   // a simpler outline
    FlatFace      = 5,   // the nominal place was too close to a flat face (rule 3)
    KeepFit       = 6,   // moved so a neighbouring part stays within the usable size
};

// ---- cut-face labels (P5, design 7.5) -------------------------------------------------------------

enum class SplitLabelStyle : int {
    Code    = 0,   // the pair's code, e.g. "X1.2" (default)
    Number  = 1,   // a running number: the pair's id + 1
    Pattern = 2,   // a 3 x 3 dot pattern of the running number, for people who prefer no text
};

struct SplitLabelOptions
{
    // The panel turns labels on by default; the planner's own default is off, so a plan made without
    // asking for them is exactly P3's.
    bool            enabled{ false };
    SplitLabelStyle style{ SplitLabelStyle::Code };
    // Text height (mm, the whole glyph including its stroke). Smaller patches get smaller text down to
    // min_height, then the dot pattern, then no label (each with a note).
    double          height{ 6. };
    double          min_height{ 4. };
    // Recessed this deep into each face (mm): three 0.2 mm layers.
    double          depth{ 0.6 };
    // Narrowest stroke / dot (mm): two 0.4 mm lines.
    double          min_stroke{ 0.8 };
    // Kept this far inside the patch's outline (mm), and this far from every dowel hole on the face.
    double          margin{ 1.5 };
    double          dowel_clearance{ 2. };
};

// P4: a dowel site the user added (a click on a cut face) or removed (a click on an automatic site).
// It belongs to plane `index` of `axis` and holds only while that plane is still at `plane_pos`: when
// the planes move the planner ignores it (and the gizmo drops it). An added site is checked by the
// same rules as the automatic ones (inside the face clear of its edge, clear of the other planes,
// spacing, material behind it); one that fails is not placed and SplitWarning::ManualSiteRefused
// says why. The cap per face does not limit added sites: the user asked for them.
struct SplitManualSite
{
    int    axis{ 0 };
    int    index{ 0 };
    double plane_pos{ 0. };
    Vec3d  pos{ Vec3d::Zero() };   // grid frame, on the plane
    bool   add{ true };            // false: remove the automatic site nearest `pos`
};

// SplitWarning::value of ManualSiteRefused: why an added dowel was not placed.
enum class SplitManualRefusal : int {
    NotOnFace       = 0,   // not on material of a cut face
    NearEdge        = 1,   // too close to the face's outline (wall margin)
    NearOtherPlane  = 2,   // too close to another cut plane (corner keep-out)
    NearDowel       = 3,   // closer than the spacing to another dowel on the face
    TooLittleMaterial = 4, // too little material behind it along the dowel
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
    // P2: automatic dowels on the cut faces.
    SplitDowelOptions dowels;
    // P3: move each plane (not the pinned ones) up to `adjust_limit` mm to a better place to cut
    // (design 3.5). The panel turns it on by default (D4); the planner's own default is off, so a
    // plan made without asking for it is exactly P2's.
    bool   adjust_planes{ false };
    double adjust_limit{ 10. };
    SplitAdjustConstants adjust;
    // P5: a piece below this volume (mm3) is very small: it is warned about, its name ends in "_tiny",
    // and with discard_small it is left out of the result altogether (default off: nothing vanishes
    // unless asked). One threshold for all three, so they always agree.
    double small_piece_volume{ 1000. };
    bool   discard_small{ false };
    // P5: matching labels recessed into both faces of every mating pair.
    SplitLabelOptions labels;
    // P4: dowel sites added or removed by hand (click-to-add / remove in the preview).
    std::vector<SplitManualSite> manual_sites;
    // P4, what Apply does with the parts (copied into the plan; they change nothing in the planning):
    //  - lay_flat: turn each part to rest on the cut face with the least overhang (split_choose_lay_flat()).
    //  - instance_dowels: identical dowels become instances of one object.
    //  - loose_own_plates: read by the job only (loose pieces arranged on plates of their own).
    // The planner's defaults are off, so a plan made without asking for them is exactly P5's; the
    // panel turns lay_flat and instance_dowels on.
    bool lay_flat{ false };
    bool instance_dowels{ false };
    bool loose_own_plates{ false };
};

// The cost of cutting through `section` (scaled ExPolygons, as slice_mesh_ex() gives them), with
// `area_before` / `area_after` the material areas one step before and after (mm2), `shift` mm from
// the plane's nominal place. Pure: the planner's search and the tests both call it.
SplitSectionCost split_section_cost(const ExPolygons &section, double area_before, double area_after, double shift,
                                    const SplitOptions &options);

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
    bool   adjusted{ false };   // P3: the automatic adjustment moved it (pos != nominal)
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
    // P3: material volume in the cell (mm3; exact for a watertight model), and the area it stands on
    // when dropped to the bed as it is: its section SPLIT_CONTACT_HEIGHT above its lowest point (mm2).
    double        volume{ 0. };
    double        contact_area{ 0. };

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
    // P2: the dowel sites of this face (indices in SplitPlan::sites), and its pieces of material.
    std::vector<int> sites;
    int              islands{ 0 };
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
        // Preview, dowels (P2). face / plane say where.
        NoDowelOnFace,       // value: SPLIT_NO_DOWEL_TOO_SMALL or SPLIT_NO_DOWEL_TOO_THIN
        IslandsWithoutDowel, // value: pieces of the face that got no dowel (the face has some)
        DowelCanTurn,        // one round dowel on the face: the parts can turn on it
        DowelShortened,      // value: the shortest dowel on the face, mm (whole length)
        DowelScaledDown,     // value: the dowel diameter used on the face, mm
        // Plane adjustment (P3, design 3.5). plane says which.
        PlaneMoved,          // value: the shift from the nominal place, mm (signed); detail: SplitAdjustReason
        AdjustRefused,       // value: the shift it wanted, mm (signed); refused, a part would exceed the usable size
        PlaneInDetail,       // value: the limit, mm; the plane still cuts fine detail, nothing better within it
        AdjustImpossible,    // value: the axis; no placement keeps every part within the rules, planes left as laid out
        // Preview and after Apply (P3, design 3.7). part says which.
        SmallPart,           // value: the volume, mm3 (below SplitOptions::small_piece_volume); P5: piece says which
        NoFlatFace,          // value: the area it stands on, mm2 (below SPLIT_MIN_CONTACT_AREA); P5: piece says which
        // P5, loose parts and labels.
        DiscardedPieces,     // value: how many pieces "Discard pieces smaller than" left out; value2: their volume, mm3
        LabelSmaller,        // mate: which pair; value: the text height used, mm (smaller than asked, to fit)
        LabelPattern,        // mate: which pair; no room for text: the dot pattern is used
        NoLabel,             // mate: which pair; value: SPLIT_NO_LABEL_TOO_SMALL
        // P4.
        ManualSiteRefused,   // detail: index in SplitOptions::manual_sites; value: SplitManualRefusal; plane / face / mate where known
    };
    Kind   kind{ Kind::NoUsableVolume };
    int    plane{ -1 };
    int    part{ -1 };       // index in SplitPlan::parts (the section)
    double value{ 0. };
    int    face{ -1 };       // P2: index in SplitPlan::faces, or -1
    int    detail{ 0 };      // P3: PlaneMoved's SplitAdjustReason
    int    piece{ -1 };      // P5: index in SplitPlan::pieces, or -1
    int    mate{ -1 };       // P5: index in SplitPlan::mates, or -1
    double value2{ 0. };
};

// SplitWarning::value of NoLabel.
constexpr double SPLIT_NO_LABEL_TOO_SMALL = 0.;   // no room on the patch, even for the dot pattern

// ---- pieces, mating pairs and labels (P5) --------------------------------------------------------

// One connected piece of material inside a section (grid cell): what Apply makes into one object.
// A section of one piece has one SplitPiece, named like the section.
struct SplitPiece
{
    int           section{ -1 };   // index in SplitPlan::parts
    int           index{ 0 };      // 0-based within its section, largest volume first
    // Stable id: the section name and a letter, "X1-Y2-Z1_b" ("X1-Y2-Z1_a" for a single piece).
    std::string   id;
    // The part's name: the section name, plus "_b" when the section has more than one piece, plus
    // "_tiny" below SplitOptions::small_piece_volume ("X1-Y2-Z1_b_tiny").
    std::string   name;
    BoundingBoxf3 bbox;            // grid frame
    bool          fits{ false };
    double        volume{ 0. };    // mm3
    double        contact_area{ 0. }; // mm2 (as SplitPart::contact_area)
    // P4: the most it can stand on: its contact area as it is, or the area of one of its cut faces
    // (flat by construction) when it is laid on that face and still fits the usable volume (mm2).
    // "No flat face" is warned only when this is below SPLIT_MIN_CONTACT_AREA.
    double        best_contact_area{ 0. };
    bool          tiny{ false };   // below the small-piece volume
    bool          discarded{ false }; // left out ("Discard pieces smaller than")
    std::vector<int> mates;        // indices in SplitPlan::mates
    std::vector<int> labels;       // indices in SplitPlan::labels on this piece

    Vec3d size() const { return bbox.defined ? bbox.size() : Vec3d::Zero(); }
};

// Two pieces that meet on a cut face, through one or more patches of it (a U cut across both legs
// is one pair with two patches). Dowels and labels are planned per pair.
struct SplitMate
{
    int         id{ -1 };          // index in SplitPlan::mates
    int         face{ -1 };        // index in SplitPlan::faces
    int         piece_below{ -1 }; // index in SplitPlan::pieces, on the negative side of the plane
    int         piece_above{ -1 };
    double      area{ 0. };        // mm2
    // The patches (the plane's coordinates, as SplitFace islands: scaled, largest first).
    ExPolygons  patches;
    // The code both pieces carry: the face's mate_code ("X1.2"), with a letter when the face joins
    // more than one pair ("X1.2A", "X1.2B").
    std::string code;
    std::vector<int> sites;        // indices in SplitPlan::sites
    std::vector<int> labels;       // indices in SplitPlan::labels (two, or none)
};

// One recessed label on one piece's patch of a cut face. The two labels of a pair sit on the same
// spot of the plane and carry the same text; each is laid out seen from outside its own piece (text
// upright along +Z on X / Y faces, along +Y on Z faces), so in space they are mirror images and both
// read right when each part is held face on.
struct SplitLabel
{
    int         mate{ -1 };
    int         piece{ -1 };
    bool        below{ true };     // on the piece below the plane (outward normal +axis)
    std::string text;              // what is written (also for the pattern: its number)
    bool        pattern{ false };  // the dot pattern instead of text
    int         number{ 0 };       // the pattern's number
    Vec3d       center{ Vec3d::Zero() }; // grid frame, on the plane
    Vec3d       normal{ Vec3d::UnitZ() }; // out of the piece through its face
    Vec3d       up{ Vec3d::UnitY() };
    Vec3d       right{ Vec3d::UnitX() };  // up x normal: the reading direction seen from outside
    double      height{ 0. };      // mm, along `up`
    double      width{ 0. };       // mm, along `right`
    double      stroke{ 0. };      // mm
    double      depth{ 0. };       // mm into the piece
};

// The label's 2D shapes (scaled coordinates, mm before scaling), centred on the origin, x to the
// right and y up: `text` in the built-in stroke font (digits, A-Z, '.', '-'; other characters are
// left as a gap), each glyph `height` tall including its `stroke`. `width` gets the total width.
ExPolygons split_label_text_shapes(const std::string &text, double height, double stroke, double *width = nullptr);
// The dot pattern of `number` (1..511): a 3 x 3 grid of square dots of side `stroke` and pitch
// 2 * stroke, the top-left dot always set (it shows which way up), the other eight the number's bits.
ExPolygons split_label_pattern_shapes(int number, double stroke, double *size = nullptr);
// The label's recess as a closed mesh in the grid frame: from 0.2 mm outside the face to `depth`
// inside the piece. Apply adds it to its piece as a negative volume.
indexed_triangle_set split_label_mesh(const SplitLabel &label);

// SplitWarning::value of NoDowelOnFace.
constexpr double SPLIT_NO_DOWEL_TOO_SMALL = 0.;   // no room inside the face's outline
constexpr double SPLIT_NO_DOWEL_TOO_THIN  = 1.;   // room on the face, but too little material behind it

// P3 part checks (design 3.7): a part below 1 cm3 is very small; a part whose section 0.3 mm above its
// lowest point is below 1 cm2 has no flat face to stand on (it needs supports or turning by hand, D11).
constexpr double SPLIT_SMALL_PART_VOLUME = 1000.;
constexpr double SPLIT_MIN_CONTACT_AREA  = 100.;
constexpr double SPLIT_CONTACT_HEIGHT    = 0.3;

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
    // P2: every dowel site, face by face (empty when dowels are off), and the options they were
    // placed with (split_apply() builds the connectors from them).
    std::vector<SplitSite>    sites;
    SplitDowelOptions         dowel_options;
    // P5: the pieces of every section (in section order, largest first within one), the mating pairs
    // and the labels; the options they were made with.
    std::vector<SplitPiece>   pieces;
    std::vector<SplitMate>    mates;
    std::vector<SplitLabel>   labels;
    SplitLabelOptions         label_options;
    double                    small_piece_volume{ 1000. };
    // P5: for every vertex of the planned mesh, the piece it lies in (-1: on no piece). Apply uses it to
    // tell which piece each separate shell of a cut part is.
    std::vector<int>          vertex_piece;
    // P4: what Apply does with the parts (SplitOptions::lay_flat, instance_dowels, loose_own_plates),
    // and the hand-made dowel edits the plan was made with.
    bool                      lay_flat{ false };
    bool                      instance_dowels{ false };
    bool                      loose_own_plates{ false };
    std::vector<SplitManualSite> manual_sites;

    size_t      dowel_count() const { return sites.size(); }
    // P4: a piece that broke off its section (not the section's largest piece): "_b", "_c", ...
    bool        is_loose_piece(int piece) const
    {
        return piece >= 0 && size_t(piece) < pieces.size() && pieces[size_t(piece)].index > 0;
    }
    size_t      cell_count() const { return parts.size(); }
    size_t      part_count() const;          // non-empty cells (sections)
    size_t      parts_not_fitting() const;   // sections; P5: see pieces_not_fitting()
    // P5: the parts Apply makes: pieces that are not discarded.
    size_t      piece_count() const;
    size_t      pieces_not_fitting() const;
    size_t      discarded_count() const;
    double      discarded_volume() const;
    // Pieces of section `section` (indices in `pieces`).
    std::vector<int> pieces_of(int section) const;
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

// P2, design 3.6: up to `max_count` dowel centres (mm) inside `island` (scaled, like every
// ExPolygon), each at least `inset` mm from its outline and inside `allowed` (mm; the corner
// keep-out bands are already taken off it), at least `min_spacing` apart. Greedy farthest-point
// picks on a square grid: the first is the point farthest from the outline, each next one the
// point that best keeps away from both the outline and the picks so far. `accept` (the depth check)
// may refuse a pick: the next best point is tried instead, up to `max_rejects` refusals.
std::vector<Vec2d> split_pick_sites(const ExPolygon &island, const BoundingBoxf &allowed, double inset, int max_count,
                                    double min_spacing, const std::function<bool(const Vec2d &)> &accept = nullptr,
                                    int max_rejects = 40);

// The plan for `mesh` (grid frame). `throw_on_cancel` is called now and then and may throw.
// `face_neighbors` (its_face_neighbors(mesh)) may be passed in by a caller that plans the same mesh
// many times (the gizmo); otherwise it is worked out here (P5's piece search needs it).
SplitPlan split_plan_grid(const indexed_triangle_set &mesh, const SplitUsableVolume &usable, const SplitOptions &options,
                          std::function<void()> throw_on_cancel = nullptr, const std::vector<Vec3i32> *face_neighbors = nullptr);

// The object's model parts merged into one mesh in the grid frame (instance `instance` applied).
indexed_triangle_set split_object_mesh(const ModelObject &object, int instance);

// ---- P4: lay a part on a cut face -----------------------------------------------------------------

// The overhang of `mesh` turned by `rotation` (mm2): the area of the facets that face down more steeply
// than `threshold_deg` from the horizontal (45: the usual limit), not counting the facets lying on the
// bed (at its lowest point). `contact` gets the area of those bed facets (mm2).
double split_overhang_area(const indexed_triangle_set &mesh, const Transform3d &rotation, double threshold_deg = 45.,
                           double *contact = nullptr);

struct SplitLayFlat
{
    bool        changed{ false };       // false: the part keeps its pose
    Transform3d rotation{ Transform3d::Identity() }; // apply before the part's own pose (about the origin)
    Vec3d       normal{ Vec3d::Zero() }; // the chosen face's outward normal before turning (zero: kept as it is)
    double      overhang{ 0. };        // mm2, in the chosen pose
    double      contact{ 0. };         // mm2, in the chosen pose
    bool        fits{ true };          // the chosen pose fits `usable`
};

// Chooses the pose of a part: resting on one of its cut faces (`cut_normals`: their outward normals,
// in the frame of `mesh`), or as it is when it already stands on a flat base (at least
// SPLIT_MIN_CONTACT_AREA). Least overhang wins (45 degrees); poses within 1 mm2 or 1 % of each other tie,
// and a tie goes to the larger contact area, then to the pose as it is. A pose must fit `usable` (X / Y
// may be swapped by a quarter turn about Z, which the rotation then includes); a pose that does not is
// passed over for the next best. With no fitting pose the part is kept as it is. `usable` zero: no fit
// check.
SplitLayFlat split_choose_lay_flat(const indexed_triangle_set &mesh, const std::vector<Vec3d> &cut_normals, const Vec3d &usable);

// ---- P4: click-to-add / remove a dowel site -------------------------------------------------------

struct SplitPickTarget
{
    enum class Kind : int { None, Site, Face };
    Kind  kind{ Kind::None };
    int   site{ -1 };     // Site: index in SplitPlan::sites
    int   plane{ -1 };    // Site / Face: index in SplitPlan::planes
    int   mate{ -1 };     // Face: index in SplitPlan::mates
    Vec3d pos{ Vec3d::Zero() }; // where the ray meets the plane (grid frame)
};

// What a click along the ray (`origin`, `dir`, grid frame) points at: the nearest dowel disc it passes
// through, else the nearest patch of material on a cut plane (the planes lie inside the model, so the
// ray is not stopped by the model's surface).
SplitPickTarget split_pick(const SplitPlan &plan, const Vec3d &origin, const Vec3d &dir);

// ---- apply ----------------------------------------------------------------------------------------

struct SplitApplyResult
{
    // Owns the parts. Each part is one object with one instance, sitting where it was in the
    // uncut object (world frame), with its assembled pose set.
    std::unique_ptr<Model>    model;
    std::vector<ModelObject*> parts;      // owned by `model`, in plan piece order (P5: one per piece)
    std::vector<int>          part_index; // per entry of `parts`: index in SplitPlan::parts (the section)
    std::vector<int>          piece_index; // P5: per entry of `parts`: index in SplitPlan::pieces (-1: not matched)
    size_t                    labels{ 0 };   // P5: labels added (negative volumes)
    size_t                    discarded{ 0 }; // P5: pieces left out, and their volume (mm3)
    double                    discarded_volume{ 0. };
    // P2: the loose dowels (or pins), one object each, owned by `model`, in SplitPlan::sites order,
    // named "<object>-Dowel-<site>" ("-Pin-"), standing where they sit in the assembled model.
    std::vector<ModelObject*> dowels;
    // P4: dowels to print in all (instances: with SplitPlan::instance_dowels identical dowels are one
    // object with one instance per dowel, so dowels.size() counts shapes, not dowels).
    size_t                    dowel_count{ 0 };
    // P4: per entry of `parts`: a loose piece (broke off its section), and whether it was turned onto
    // a cut face (SplitPlan::lay_flat). labels_on_bed: labels on a face a part now stands on (recessed,
    // so they print as a dent in the bottom).
    std::vector<bool>         part_loose;
    std::vector<bool>         part_laid_flat;
    size_t                    labels_on_bed{ 0 };
    std::vector<SplitWarning> warnings;   // the after-Apply ones (LoosePieces, OpenEdges, SmallPart, NoFlatFace)
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
// Cut links: a plain split is not a linked cut group (D10). With dowels (P2) every part and every
// dowel object shares one cut id, its check sum the number of objects, as the Cut tool does for a
// cut with connectors. An object that was already part of a linked group hands its id on: every
// part (and dowel) gets it, the check sum grown by the objects made - 1, which is what a series of
// cuts on that object would have done.
//
// Dowels (P2): each plane's sites are baked as Dowel / Pin connectors into the piece just before
// that plane cuts it, so every part gets its holes as negative volumes and each dowel becomes an
// object of its own. Earlier holes are handed on by the later cuts (by bounding box,
// process_modifier_cut()); the planner's corner keep-out keeps every hole clear of every other
// plane, so each hole lands in exactly one part.
//
// Pieces (P5): a cut part whose section has several pieces is split into one object per piece (each
// separate shell of the part is matched to its piece by the plan's vertex_piece; paint is carried
// over), named "<object name> <piece name>". Holes, labels and modifiers go with the piece they
// touch. Discarded pieces are left out (result.discarded / discarded_volume). Labels (P5) are added
// to their piece as negative volumes named "Label <text>".
SplitApplyResult split_apply(const ModelObject &object, int instance, const SplitPlan &plan,
                             std::function<bool()> canceled = nullptr,
                             std::function<void(int, int)> progress = nullptr);

} // namespace Slic3r

#endif // slic3r_SplitToVolume_hpp_
