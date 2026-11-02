#pragma once

// Tablet paint sessions, read side (`paint_v1`, pen colouring phase H1): the JSON and the binary
// bodies of GET /api/paint/objects, POST /api/paint/sessions and GET /api/paint/sessions/{id}[/mesh,
// /paint]. wx-free on purpose, like RemoteSliceV1: the GUI thread only takes an ObjectSnapshot (the
// volumes' shared meshes and a copy of their paint), and everything below - hashing, encoding, the
// JSON - runs off it, so tests/slic3rutils/paint_v1_tests.cpp drives it with a plain Model and the
// app's fixtures (EdgeSlicerApp tools/paint_v1/fixtures/hub) are exactly what the hub sends.
//
// The app paints with its own model (EdgeSlicerApp docs/paint_wire_v0.md): it needs one volume's
// mesh in the hub's own triangle order, the PC's current painting to draw, the palette and the
// transforms. The formats are the ones spike S1 measured (memo 2026-11-01):
//
//   mesh  (application/x-edgeslicer-psmesh), little-endian:
//     "PSM1"  uint32 vertex count  uint32 triangle count
//     vertex count x 3 float32 (x, y, z in the volume's own mesh space, mm)
//     triangle count x 3 uint32 (vertex indices)
//   The mesh hash is SHA-256 (lowercase hex) of everything after the 12-byte header.
//
//   paint (application/x-edgeslicer-pspaint), little-endian: the PC's painting as the app draws it,
//   per filament state the painted sub-triangles and the original triangle each came from
//   (TriangleSelector::get_facets_strict). State 0 (unpainted) is not listed.
//     "PSP1"  uint32 state count
//     per state: uint32 state  uint32 sub-triangle count n
//                n x 9 float32 (three corners, mesh space)   n x uint32 (original triangle)

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class ModelObject;
class TriangleMesh;
enum class ModelVolumeType : int;

namespace GUI {
namespace PaintV1 {

// What /pair `features` gains: "paint_v1" (the read side; the apply side will add its own).
const std::vector<std::string>& pair_features();

// The caps the owner accepted after spike S1: an object above MAX_TRIANGLES (all its model parts)
// is listed but cannot be opened; MAX_LEAVES is the app's split-tree cap, handed out with the session.
constexpr size_t MAX_TRIANGLES = 2000000;
constexpr size_t MAX_LEAVES    = 1000000;
// Open sessions the instance keeps; the oldest is dropped when another one opens.
constexpr size_t MAX_SESSIONS = 16;

// A session id: 16 lowercase hex characters.
bool is_session_id(const std::string& id);
std::string new_session_id();

// The object's key: "o" and its ObjectID, which stays the same through undo / redo and edits for as long as
// the project is open. Indices shift when an object above is deleted; keys do not.
std::string object_key(const ModelObject& object);

// "model_part", "negative_volume", "modifier", "support_blocker", "support_enforcer" or
// "seam_modifier" (the app shows only model parts; the others are listed so it can say why).
std::string volume_type_name(ModelVolumeType type);

struct VolumeSnapshot
{
    size_t                              index { 0 };    // in ModelObject::volumes
    size_t                              id { 0 };       // ObjectID
    std::string                         name;
    ModelVolumeType                     type {};
    std::shared_ptr<const TriangleMesh> mesh;           // shared with the model: read-only, kept alive
    TriangleSelector::TriangleSplittingData paint;      // copied (empty without with_paint)
    bool                                painted { false };
    bool                                airbrushed { false }; // an airbrush field is stored (its bake is in the paint)
    bool                                image_fill { false }; // Image Fill settings are stored
    int                                 extruder { 1 };       // 1-based: the volume's own, else the object's, else 1
    Transform3d                         matrix { Transform3d::Identity() }; // volume in object space
    size_t                              triangles() const;
    size_t                              vertices() const;
    bool                                paintable() const;   // a model part
};

struct ObjectSnapshot
{
    std::string                 key;
    int                         index { -1 }; // in Model::objects now
    std::string                 name;
    int                         plate { -1 }; // the plate of its first instance, -1 on none
    int                         extruder { 1 };  // 1-based; an object without one prints with filament 1
    std::vector<VolumeSnapshot> volumes;
    std::vector<Transform3d>    instances;    // instance matrices (world)
    size_t                      paint_triangles() const; // all model parts
};

// GUI thread (the caller's): cheap - meshes are shared, the paint is copied only with with_paint.
ObjectSnapshot snapshot(const ModelObject& object, int index, int plate, bool with_paint);

struct PaletteSlot
{
    std::string color;  // #RRGGBB, filament_colour
    std::string type;   // filament_type, "" when unknown
    std::string preset; // the slot's filament preset
};

// ---- bodies and hashes (any thread) ----
std::string encode_mesh(const indexed_triangle_set& its);
std::string mesh_hash(const indexed_triangle_set& its);
// The same, remembered per mesh while the model keeps it (a 2M-triangle hash takes ~100 ms).
std::string mesh_hash(const std::shared_ptr<const TriangleMesh>& mesh);
std::string encode_paint(const TriangleMesh& mesh, const TriangleSelector::TriangleSplittingData& paint);
std::string paint_hash(const TriangleSelector::TriangleSplittingData& paint);
std::string palette_hash(const std::vector<PaletteSlot>& palette);
// 16 hex characters over each volume's index, type, mesh hash and paint hash, and the palette
// hash. Changes when anything the app shows changes; transforms are not part of it.
std::string revision(const ObjectSnapshot& object, const std::vector<PaletteSlot>& palette);

// ---- JSON ----
// GET /api/paint/objects: every object with its key, plate, volumes and what opening it costs.
nlohmann::json objects_json(const std::vector<ObjectSnapshot>& objects);
// The paint depth options the PC slices with for this object: definitions (type, label, tooltip,
// unit, limits, enum values) and values (the object's own, else the print preset's).
nlohmann::json paint_depth_json(const DynamicPrintConfig& print, const DynamicPrintConfig& object);
// GET /api/paint/sessions/{id} (and POST /api/paint/sessions).
nlohmann::json session_json(const std::string& id, const std::string& rev, const ObjectSnapshot& object,
                            const std::vector<PaletteSlot>& palette, const nlohmann::json& paint_depth);

} // namespace PaintV1
} // namespace GUI
} // namespace Slic3r
