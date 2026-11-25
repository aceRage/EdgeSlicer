// Carry the painting of a mesh over to another one: the result of a boolean, the pieces of a split
// or a cut. Colour, support, seam and fuzzy-skin FacetsAnnotation layers only — not paint-depth,
// texture-displacement masks, or the model UV channel.
//
// Adapted from NeotkoCM (sebsucmor-alt/OrcaFS-NeotkoCM, AGPL-3.0, an OrcaSlicer fork), commits
// 8f5eae374 (cut) and keep-painting-sm240 498617ad1 (boolean / split); first ported in EdgeSlicer
// PRs #776 and #605, re-ported onto the current cut and boolean code as keep paint v2.
#ifndef slic3r_PaintTransfer_hpp_
#define slic3r_PaintTransfer_hpp_

#include "Point.hpp"
#include "TriangleMesh.hpp"

#include <memory>
#include <vector>

namespace Slic3r {

class ModelObject;
class ModelVolume;

namespace PaintTransfer {

// One piece the painting comes from: a model part of one of the inputs of the boolean / cut.
struct Source
{
    const ModelVolume *volume   = nullptr;
    Transform3d        to_world = Transform3d::Identity(); // volume mesh coords -> world
    // Filament the unpainted skin of this piece prints with (1-based). The unpainted skin of the
    // result is coloured with it, so the result keeps the colour each input had.
    int filament = 1;
    // The tool of a Difference: what is left of its skin in the result is the wall of the cut,
    // which stays unpainted (prints with the result's own filament).
    bool cutter = false;
};

struct Stats
{
    size_t layers       = 0; // paint layers written on the target
    size_t samples      = 0; // points looked up on the sources
    size_t searches     = 0; // of those, the ones that needed a search over a whole source mesh
    size_t tie_searches = 0; // searches over earlier sources for the tie check
    // Where the time goes (ms): preparing the sources, loading their painting, resampling, storing.
    long   prepare_ms = 0, load_ms = 0, resample_ms = 0, store_ms = 0;
};

// Whether the volume has any painting this module carries (support, seam, colour, fuzzy skin).
bool has_painting(const ModelVolume &volume);
// Whether transfer() from these sources has anything to do: some painting, or inputs with
// different filaments (whose unpainted skin then gets painted). Used to decide whether to tell
// the user to wait.
bool worth_transferring(const std::vector<Source> &sources);

// Filament a model part prints with: its own, else its object's, else the first one.
int effective_filament(const ModelVolume &volume);

// Every model part of `object`, placed with its first instance.
void append_object_sources(const ModelObject &object, bool cutter, std::vector<Source> &out);

// In a facet map of Transfer::to(): a new facet that stays unpainted.
constexpr int FACET_UNPAINTED = -2;

// Which paint layers a transfer writes; the others are left as they are on the target.
enum LayerMask : unsigned {
    LAYER_SUPPORT = 1,
    LAYER_SEAM    = 2,
    LAYER_COLOUR  = 4,
    LAYER_FUZZY   = 8,
    ALL_LAYERS    = LAYER_SUPPORT | LAYER_SEAM | LAYER_COLOUR | LAYER_FUZZY,
};

// Opt-in tuning of Transfer::to() for a target that is the SAME surface rebuilt (repair, remesh,
// simplify: RepairPaint.hpp) rather than the result of a boolean. The defaults keep the boolean /
// cut behaviour exactly.
struct Options
{
    // > 0: the farthest (world mm) a point of the target may lie from the sources and still take
    // their painting. 0 keeps the boolean's own limit (a few microns of nudging).
    float max_dist = 0.f;
    // Take the nearest source facet that faces the same way as the target facet (normals less
    // than 90 degrees apart), searching up to max_dist around the point, before the plain nearest
    // facet. Stops a rebuilt thin wall taking the painting of its other side.
    bool  normal_agreement = false;
    // > 0: the smallest edge (target mesh coords) a target facet is split down to along a border;
    // 0 keeps the default (0.005 mm).
    float edge_limit = 0.f;
};

// The sources prepared once (search trees, painting loaded) for several targets, as when one
// mesh is split into many pieces.
class Transfer
{
public:
    explicit Transfer(std::vector<Source> sources);
    ~Transfer();
    // Applies to every later to().
    void set_options(const Options &options);
    // See transfer() below. With one source, `facet_map` (from FacetMatcher::match()) may say
    // which facets of the target are facets of the source: their painting is copied as it is and
    // only the rest is resampled (a cut, where only the facets along the plane are new).
    // FACET_UNPAINTED in the map leaves that facet unpainted without sampling it (the cap of a cut).
    Stats to(ModelVolume &target, const Transform3d &target_to_world, int base_filament, const std::vector<int> *facet_map = nullptr,
             unsigned layers = ALL_LAYERS);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Repaints `target` (support, seam, colour and fuzzy skin painting) from `sources`.
// `target_to_world` maps the target mesh into the same world as the sources. Where the skins of
// two sources coincide, the one listed first wins. `base_filament` is the filament the target
// prints with: skin that would be coloured with it is left unpainted.
Stats transfer(const std::vector<Source> &sources, ModelVolume &target, const Transform3d &target_to_world, int base_filament,
               unsigned layers = ALL_LAYERS);

// Split / cut: a piece cut out of a mesh keeps the very same triangles (same vertices, same corner
// order), so its painting is copied triangle by triangle, exactly and without any search.
// Finds the facets of the pieces in the source. Built once per source, used for every piece.
class FacetMatcher
{
public:
    // `source` is kept by reference: it must outlive the matcher.
    explicit FacetMatcher(const indexed_triangle_set &source);
    ~FacetMatcher();
    // For each facet of `piece`, the facet of the source it is, or -1 when there is none.
    std::vector<int> match(const indexed_triangle_set &piece) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
// Copies the painting of `source` onto `target` through `facet_map` (from FacetMatcher::match() on
// their meshes). Returns false, without touching `target`, when some facet has no match.
bool copy_painting(const ModelVolume &source, ModelVolume &target, const std::vector<int> &facet_map, unsigned layers = ALL_LAYERS);

// Mesh Boolean. The inputs of the boolean, in the object's own coordinates (each volume's mesh by
// its own matrix, as the booleans take them): every model part, and every negative part as a
// cutter (object list "Boolean"), ...
std::vector<Source> object_boolean_sources(const ModelObject &object);
// ... or the gizmo's source and tool parts (the tool is a cutter in a Difference).
std::vector<Source> part_boolean_sources(const ModelVolume &source, const ModelVolume &tool, bool difference);
// Paints a boolean's result (its matrix maps its mesh into the sources' frame) when there is
// anything to carry (worth_transferring()). The colour is left alone when an input is airbrushed:
// its field is re-baked from the source instead (Airbrush P3). Returns whether it painted.
bool paint_boolean_result(const std::vector<Source> &sources, ModelVolume &result);

// Split (to objects or to parts): each piece TriangleMesh::split() cuts out of a mesh keeps that
// mesh's facets with the very same corners, so its painting is copied facet by facet. A piece that
// cannot be matched in full (a shell split() turned inside out) is resampled from the source.
class SplitPainter
{
public:
    // `source` (with the painting) must outlive the painter; `source_to_world` maps its mesh into
    // the frame paint() is given each piece's matrix in.
    SplitPainter(const ModelVolume &source, const Transform3d &source_to_world, unsigned layers = ALL_LAYERS);
    ~SplitPainter();
    // Call on the piece's mesh as split() made it, before anything moves it.
    std::vector<int> match(const indexed_triangle_set &piece) const;
    // Paints `piece` from the map match() gave for its mesh. `piece_to_world`: its mesh into the
    // source's frame (only used when the map is incomplete). Returns whether it was an exact copy.
    bool paint(ModelVolume &piece, const std::vector<int> &map, const Transform3d &piece_to_world);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace PaintTransfer
} // namespace Slic3r

#endif // slic3r_PaintTransfer_hpp_
