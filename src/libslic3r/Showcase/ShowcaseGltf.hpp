#ifndef slic3r_Showcase_ShowcaseGltf_hpp_
#define slic3r_Showcase_ShowcaseGltf_hpp_

// Showcase realism phase 4 (tests/design_showcase_realism.md 5.2): a small binary glTF 2.0 (GLB)
// writer and reader, enough for the Blender package's scene.glb. No new dependency: a GLB is a
// 12-byte header, one JSON chunk and one binary chunk.
//
//  * Positions are whatever the caller gives (the package writes metres in glTF's Y-up frame); the
//    writer adds the accessors' min / max that glTF requires for POSITION.
//  * Custom vertex attributes must be named with a leading underscore (glTF 3.7.2.1); Blender's
//    importer keeps them as mesh attributes.
//  * Materials: core PBR plus the KHR_materials_* extensions the Blender importer reads (clearcoat,
//    transmission, volume, ior, sheen, specular, anisotropy, emissive_strength); our own parameters
//    go in the material's "extras".
//  * Deterministic: the same document gives the same bytes (sorted JSON keys, no time stamps).
//
// read_glb() / validate_glb() are for the tests and for reading a package back: they check the
// container, the JSON and that every accessor fits its buffer view and the buffer.
//
// GL- and wxWidgets-free.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
namespace Showcase {

constexpr uint32_t GLB_MAGIC        = 0x46546C67;   // "glTF"
constexpr uint32_t GLB_VERSION      = 2;
constexpr uint32_t GLB_CHUNK_JSON   = 0x4E4F534A;   // "JSON"
constexpr uint32_t GLB_CHUNK_BIN    = 0x004E4942;   // "BIN\0"

struct GltfAttribute
{
    std::string        name;          // "_SILK_DIR", "_BUILD", ... (leading underscore)
    int                components = 1;   // 1 (SCALAR), 2, 3 or 4 (VECn)
    std::vector<float> data;          // components floats per vertex
};

struct GltfMesh
{
    std::string                name;
    std::vector<float>         positions;   // x, y, z per vertex
    std::vector<float>         normals;     // empty, or x, y, z per vertex (unit)
    std::vector<uint32_t>      indices;     // triangles
    std::vector<GltfAttribute> attributes;
    int                        material = -1;

    size_t vertex_count() const { return positions.size() / 3; }
};

struct GltfMaterial
{
    std::string          name;
    std::array<float, 4> base_color{ { 1.f, 1.f, 1.f, 1.f } };   // linear
    float                metallic  = 0.f;
    float                roughness = 0.5f;
    std::array<float, 3> emissive{ { 0.f, 0.f, 0.f } };          // linear
    float                emissive_strength = 1.f;                // KHR_materials_emissive_strength when > 1
    bool                 blend        = false;                   // alphaMode BLEND (else OPAQUE)
    bool                 double_sided = false;
    // KHR_materials_* (written when non-zero / non-default).
    float                clearcoat           = 0.f;
    float                clearcoat_roughness = 0.f;
    float                transmission        = 0.f;
    float                ior                 = 1.5f;
    float                thickness           = 0.f;              // volume thickness (scene units)
    std::array<float, 3> attenuation_color{ { 1.f, 1.f, 1.f } }; // linear
    float                attenuation_distance = 0.f;             // 0 = infinite (not written)
    std::array<float, 3> sheen_color{ { 0.f, 0.f, 0.f } };
    float                sheen_roughness     = 0.f;
    float                specular            = 1.f;              // KHR_materials_specular factor
    std::array<float, 3> specular_color{ { 1.f, 1.f, 1.f } };
    float                anisotropy          = 0.f;
    float                anisotropy_rotation = 0.f;
    // A JSON object text for "extras" (our parameters), or empty.
    std::string          extras_json;
};

struct GltfNode
{
    std::string           name;
    int                   mesh = -1;
    std::array<double, 3> translation{ { 0., 0., 0. } };
    std::string           extras_json;   // a JSON object text, or empty
};

struct GltfDocument
{
    std::string               generator = "EdgeSlicer";
    std::vector<GltfMesh>     meshes;
    std::vector<GltfMaterial> materials;
    std::vector<GltfNode>     nodes;     // all roots of the one scene, in this order
    std::string               scene_extras_json;
};

// The GLB bytes. Never throws; an index out of range (a mesh's material, a node's mesh) is dropped.
std::vector<uint8_t> write_glb(const GltfDocument &doc);

// A GLB split into its chunks. False with `error` for anything that is not a GLB 2.0 of a JSON chunk
// and at most one binary chunk.
struct GlbChunks
{
    std::string          json;
    std::vector<uint8_t> bin;
};
bool read_glb(const uint8_t *data, size_t size, GlbChunks &out, std::string *error = nullptr);

// Structural checks a strict loader makes: the asset version, one buffer the size of the binary chunk,
// buffer views inside it, accessors inside their views with a known component type and type, POSITION
// with min / max, index accessors of unsigned ints that index existing vertices, materials, meshes and
// nodes that point at existing ones. Fills the counts it saw. False with `error` at the first problem.
struct GlbStats
{
    size_t meshes = 0, nodes = 0, materials = 0, accessors = 0;
    size_t vertices = 0, triangles = 0;
    std::vector<std::string> extensions_used;
};
bool validate_glb(const uint8_t *data, size_t size, GlbStats *stats = nullptr, std::string *error = nullptr);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseGltf_hpp_
