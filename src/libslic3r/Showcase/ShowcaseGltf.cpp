#include "ShowcaseGltf.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace Slic3r {
namespace Showcase {

namespace {

using gltf_json = nlohmann::json;

constexpr int GLTF_FLOAT          = 5126;
constexpr int GLTF_UNSIGNED_INT   = 5125;
constexpr int GLTF_UNSIGNED_SHORT = 5123;
constexpr int GLTF_UNSIGNED_BYTE  = 5121;
constexpr int GLTF_ARRAY_BUFFER         = 34962;
constexpr int GLTF_ELEMENT_ARRAY_BUFFER = 34963;

// A float as the shortest text that reads back as the same float, as a JSON number: 0.1f is written
// "0.1", not "0.10000000149011612". The same on every platform (printf is correctly rounded).
double gltf_num(double v)
{
    if (!std::isfinite(v))
        return 0.;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", float(v));
    for (int p = 1; p <= 9; ++p) {
        char shorter[64];
        std::snprintf(shorter, sizeof(shorter), "%.*g", p, float(v));
        if (float(std::strtod(shorter, nullptr)) == float(v))
            return std::strtod(shorter, nullptr);
    }
    return std::strtod(buf, nullptr);
}

gltf_json gltf_vec(const float *v, int n)
{
    gltf_json a = gltf_json::array();
    for (int i = 0; i < n; ++i)
        a.push_back(gltf_num(v[i]));
    return a;
}

gltf_json gltf_extras(const std::string &text)
{
    if (text.empty())
        return gltf_json();
    gltf_json j = gltf_json::parse(text, nullptr, false);
    return j.is_object() ? j : gltf_json();
}

const char *gltf_type_name(int components)
{
    switch (components) {
    case 1: return "SCALAR";
    case 2: return "VEC2";
    case 3: return "VEC3";
    case 4: return "VEC4";
    default: return nullptr;
    }
}

int gltf_type_components(const std::string &type)
{
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    return 0;
}

int gltf_component_size(int component_type)
{
    switch (component_type) {
    case 5120: case 5121: return 1;
    case 5122: case 5123: return 2;
    case 5125: case 5126: return 4;
    default: return 0;
    }
}

struct GltfBin
{
    std::vector<uint8_t> bytes;
    gltf_json            views     = gltf_json::array();
    gltf_json            accessors = gltf_json::array();

    void align4()
    {
        while (bytes.size() % 4 != 0)
            bytes.push_back(0);
    }
    int add_view(const void *data, size_t size, int target)
    {
        align4();
        gltf_json v;
        v["buffer"]     = 0;
        v["byteOffset"] = bytes.size();
        v["byteLength"] = size;
        if (target != 0)
            v["target"] = target;
        const uint8_t *p = static_cast<const uint8_t *>(data);
        bytes.insert(bytes.end(), p, p + size);
        views.push_back(v);
        return int(views.size()) - 1;
    }
    int add_floats(const std::vector<float> &data, int components, bool min_max)
    {
        const size_t count = data.size() / size_t(components);
        const int    view  = add_view(data.data(), count * size_t(components) * sizeof(float), GLTF_ARRAY_BUFFER);
        gltf_json a;
        a["bufferView"]    = view;
        a["componentType"] = GLTF_FLOAT;
        a["count"]         = count;
        a["type"]          = gltf_type_name(components);
        if (min_max && count > 0) {
            std::vector<float> lo(size_t(components), std::numeric_limits<float>::max());
            std::vector<float> hi(size_t(components), std::numeric_limits<float>::lowest());
            for (size_t i = 0; i < count; ++i)
                for (int c = 0; c < components; ++c) {
                    lo[size_t(c)] = std::min(lo[size_t(c)], data[i * size_t(components) + size_t(c)]);
                    hi[size_t(c)] = std::max(hi[size_t(c)], data[i * size_t(components) + size_t(c)]);
                }
            a["min"] = gltf_vec(lo.data(), components);
            a["max"] = gltf_vec(hi.data(), components);
        }
        accessors.push_back(a);
        return int(accessors.size()) - 1;
    }
    int add_indices(const std::vector<uint32_t> &idx)
    {
        const int view = add_view(idx.data(), idx.size() * sizeof(uint32_t), GLTF_ELEMENT_ARRAY_BUFFER);
        gltf_json a;
        a["bufferView"]    = view;
        a["componentType"] = GLTF_UNSIGNED_INT;
        a["count"]         = idx.size();
        a["type"]          = "SCALAR";
        accessors.push_back(a);
        return int(accessors.size()) - 1;
    }
};

void gltf_put_u32(std::vector<uint8_t> &out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}

uint32_t gltf_get_u32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

bool gltf_fail(std::string *error, const std::string &what)
{
    if (error != nullptr)
        *error = what;
    return false;
}

} // namespace

std::vector<uint8_t> write_glb(const GltfDocument &doc)
{
    GltfBin   bin;
    gltf_json root;
    root["asset"]["version"]   = "2.0";
    root["asset"]["generator"] = doc.generator;

    std::vector<std::string> used;
    auto use = [&used](const char *ext) {
        if (std::find(used.begin(), used.end(), ext) == used.end())
            used.emplace_back(ext);
    };

    gltf_json materials = gltf_json::array();
    for (const GltfMaterial &m : doc.materials) {
        gltf_json j;
        j["name"] = m.name;
        gltf_json &pbr = j["pbrMetallicRoughness"];
        pbr["baseColorFactor"] = gltf_vec(m.base_color.data(), 4);
        pbr["metallicFactor"]  = gltf_num(std::clamp(m.metallic, 0.f, 1.f));
        pbr["roughnessFactor"] = gltf_num(std::clamp(m.roughness, 0.f, 1.f));
        const float emax = std::max({ m.emissive[0], m.emissive[1], m.emissive[2] });
        if (emax > 0.f) {
            j["emissiveFactor"] = gltf_vec(m.emissive.data(), 3);
            if (m.emissive_strength > 1.f) {
                j["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] = gltf_num(m.emissive_strength);
                use("KHR_materials_emissive_strength");
            }
        }
        if (m.blend)
            j["alphaMode"] = "BLEND";
        if (m.double_sided)
            j["doubleSided"] = true;
        if (m.clearcoat > 0.f) {
            j["extensions"]["KHR_materials_clearcoat"]["clearcoatFactor"]          = gltf_num(m.clearcoat);
            j["extensions"]["KHR_materials_clearcoat"]["clearcoatRoughnessFactor"] = gltf_num(m.clearcoat_roughness);
            use("KHR_materials_clearcoat");
        }
        if (m.transmission > 0.f) {
            j["extensions"]["KHR_materials_transmission"]["transmissionFactor"] = gltf_num(m.transmission);
            use("KHR_materials_transmission");
            if (m.thickness > 0.f) {
                gltf_json &vol = j["extensions"]["KHR_materials_volume"];
                vol["thicknessFactor"]  = gltf_num(m.thickness);
                vol["attenuationColor"] = gltf_vec(m.attenuation_color.data(), 3);
                if (m.attenuation_distance > 0.f)
                    vol["attenuationDistance"] = gltf_num(m.attenuation_distance);
                use("KHR_materials_volume");
            }
        }
        if (std::abs(m.ior - 1.5f) > 1e-4f) {
            j["extensions"]["KHR_materials_ior"]["ior"] = gltf_num(m.ior);
            use("KHR_materials_ior");
        }
        if (std::max({ m.sheen_color[0], m.sheen_color[1], m.sheen_color[2] }) > 0.f) {
            j["extensions"]["KHR_materials_sheen"]["sheenColorFactor"]     = gltf_vec(m.sheen_color.data(), 3);
            j["extensions"]["KHR_materials_sheen"]["sheenRoughnessFactor"] = gltf_num(m.sheen_roughness);
            use("KHR_materials_sheen");
        }
        if (std::abs(m.specular - 1.f) > 1e-4f || m.specular_color != std::array<float, 3>{ { 1.f, 1.f, 1.f } }) {
            j["extensions"]["KHR_materials_specular"]["specularFactor"]      = gltf_num(m.specular);
            j["extensions"]["KHR_materials_specular"]["specularColorFactor"] = gltf_vec(m.specular_color.data(), 3);
            use("KHR_materials_specular");
        }
        if (m.anisotropy > 0.f) {
            j["extensions"]["KHR_materials_anisotropy"]["anisotropyStrength"] = gltf_num(m.anisotropy);
            j["extensions"]["KHR_materials_anisotropy"]["anisotropyRotation"] = gltf_num(m.anisotropy_rotation);
            use("KHR_materials_anisotropy");
        }
        gltf_json extras = gltf_extras(m.extras_json);
        if (!extras.is_null())
            j["extras"] = extras;
        materials.push_back(j);
    }

    gltf_json meshes = gltf_json::array();
    for (const GltfMesh &m : doc.meshes) {
        gltf_json prim;
        prim["mode"] = 4;
        const size_t nv = m.vertex_count();
        prim["attributes"]["POSITION"] = bin.add_floats(m.positions, 3, true);
        if (m.normals.size() == m.positions.size() && !m.normals.empty())
            prim["attributes"]["NORMAL"] = bin.add_floats(m.normals, 3, false);
        for (const GltfAttribute &a : m.attributes) {
            if (a.name.empty() || a.name[0] != '_' || gltf_type_name(a.components) == nullptr ||
                a.data.size() != nv * size_t(a.components))
                continue;
            prim["attributes"][a.name] = bin.add_floats(a.data, a.components, false);
        }
        std::vector<uint32_t> idx;
        idx.reserve(m.indices.size());
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
            if (m.indices[i] < nv && m.indices[i + 1] < nv && m.indices[i + 2] < nv)
                idx.insert(idx.end(), { m.indices[i], m.indices[i + 1], m.indices[i + 2] });
        prim["indices"] = bin.add_indices(idx);
        if (m.material >= 0 && size_t(m.material) < doc.materials.size())
            prim["material"] = m.material;
        gltf_json mesh;
        mesh["name"]       = m.name;
        mesh["primitives"] = gltf_json::array({ prim });
        meshes.push_back(mesh);
    }

    gltf_json nodes      = gltf_json::array();
    gltf_json scene_list = gltf_json::array();
    for (const GltfNode &n : doc.nodes) {
        gltf_json j;
        j["name"] = n.name;
        if (n.mesh >= 0 && size_t(n.mesh) < doc.meshes.size())
            j["mesh"] = n.mesh;
        if (n.translation != std::array<double, 3>{ { 0., 0., 0. } })
            j["translation"] = gltf_json::array({ gltf_num(n.translation[0]), gltf_num(n.translation[1]), gltf_num(n.translation[2]) });
        gltf_json extras = gltf_extras(n.extras_json);
        if (!extras.is_null())
            j["extras"] = extras;
        scene_list.push_back(nodes.size());
        nodes.push_back(j);
    }

    bin.align4();
    root["buffers"]     = gltf_json::array({ gltf_json{ { "byteLength", bin.bytes.size() } } });
    root["bufferViews"] = bin.views;
    root["accessors"]   = bin.accessors;
    root["materials"]   = materials;
    root["meshes"]      = meshes;
    root["nodes"]       = nodes;
    gltf_json scene;
    scene["name"]  = "EdgeSlicer Showcase";
    scene["nodes"] = scene_list;
    gltf_json scene_extras = gltf_extras(doc.scene_extras_json);
    if (!scene_extras.is_null())
        scene["extras"] = scene_extras;
    root["scenes"] = gltf_json::array({ scene });
    root["scene"]  = 0;
    if (!used.empty()) {
        std::sort(used.begin(), used.end());
        root["extensionsUsed"] = used;
    }
    if (bin.views.empty()) {
        root.erase("bufferViews");
        root.erase("accessors");
    }
    if (bin.bytes.empty())
        root.erase("buffers");
    if (materials.empty())
        root.erase("materials");
    if (meshes.empty())
        root.erase("meshes");

    std::string json_text = root.dump();
    while (json_text.size() % 4 != 0)
        json_text.push_back(' ');

    std::vector<uint8_t> out;
    const size_t total = 12 + 8 + json_text.size() + (bin.bytes.empty() ? 0 : 8 + bin.bytes.size());
    out.reserve(total);
    gltf_put_u32(out, GLB_MAGIC);
    gltf_put_u32(out, GLB_VERSION);
    gltf_put_u32(out, uint32_t(total));
    gltf_put_u32(out, uint32_t(json_text.size()));
    gltf_put_u32(out, GLB_CHUNK_JSON);
    out.insert(out.end(), json_text.begin(), json_text.end());
    if (!bin.bytes.empty()) {
        gltf_put_u32(out, uint32_t(bin.bytes.size()));
        gltf_put_u32(out, GLB_CHUNK_BIN);
        out.insert(out.end(), bin.bytes.begin(), bin.bytes.end());
    }
    return out;
}

bool read_glb(const uint8_t *data, size_t size, GlbChunks &out, std::string *error)
{
    out = GlbChunks();
    if (data == nullptr || size < 20)
        return gltf_fail(error, "too short for a GLB");
    if (gltf_get_u32(data) != GLB_MAGIC)
        return gltf_fail(error, "not a GLB (magic)");
    if (gltf_get_u32(data + 4) != GLB_VERSION)
        return gltf_fail(error, "not glTF 2.0");
    const uint32_t length = gltf_get_u32(data + 8);
    if (length != size)
        return gltf_fail(error, "the header's length is not the file's size");
    size_t pos   = 12;
    int    chunk = 0;
    while (pos < size) {
        if (pos + 8 > size)
            return gltf_fail(error, "a truncated chunk header");
        const uint32_t clen  = gltf_get_u32(data + pos);
        const uint32_t ctype = gltf_get_u32(data + pos + 4);
        pos += 8;
        if (clen % 4 != 0 || pos + clen > size)
            return gltf_fail(error, "a chunk is not 4-byte aligned or runs past the end");
        if (chunk == 0) {
            if (ctype != GLB_CHUNK_JSON)
                return gltf_fail(error, "the first chunk is not JSON");
            out.json.assign(reinterpret_cast<const char *>(data + pos), clen);
        } else if (chunk == 1) {
            if (ctype != GLB_CHUNK_BIN)
                return gltf_fail(error, "the second chunk is not BIN");
            out.bin.assign(data + pos, data + pos + clen);
        } else
            return gltf_fail(error, "more than two chunks");
        pos += clen;
        ++chunk;
    }
    if (chunk == 0)
        return gltf_fail(error, "no JSON chunk");
    return true;
}

static bool validate_glb_impl(const uint8_t *data, size_t size, GlbStats *stats, std::string *error)
{
    GlbChunks chunks;
    if (!read_glb(data, size, chunks, error))
        return false;
    gltf_json j = gltf_json::parse(chunks.json, nullptr, false);
    if (!j.is_object())
        return gltf_fail(error, "the JSON chunk is not an object");
    if (!j.contains("asset") || j["asset"].value("version", "") != "2.0")
        return gltf_fail(error, "asset.version is not 2.0");
    GlbStats st;
    const gltf_json empty = gltf_json::array();
    const gltf_json &buffers   = j.contains("buffers") ? j["buffers"] : empty;
    const gltf_json &views     = j.contains("bufferViews") ? j["bufferViews"] : empty;
    const gltf_json &accessors = j.contains("accessors") ? j["accessors"] : empty;
    const gltf_json &materials = j.contains("materials") ? j["materials"] : empty;
    const gltf_json &meshes    = j.contains("meshes") ? j["meshes"] : empty;
    const gltf_json &nodes     = j.contains("nodes") ? j["nodes"] : empty;
    if (buffers.size() > 1)
        return gltf_fail(error, "more than one buffer");
    if (buffers.size() == 1) {
        if (buffers[0].contains("uri"))
            return gltf_fail(error, "the GLB buffer has a uri");
        const size_t blen = buffers[0].value("byteLength", size_t(0));
        if (blen > chunks.bin.size() || chunks.bin.size() - blen > 3)
            return gltf_fail(error, "buffer 0's byteLength does not match the BIN chunk");
    }
    for (const gltf_json &v : views) {
        const size_t off = v.value("byteOffset", size_t(0)), len = v.value("byteLength", size_t(0));
        if (v.value("buffer", -1) != 0 || off + len > chunks.bin.size() || len == 0)
            return gltf_fail(error, "a buffer view lies outside the buffer");
    }
    std::vector<size_t> acc_count;
    for (const gltf_json &a : accessors) {
        const int view = a.value("bufferView", -1);
        if (view < 0 || size_t(view) >= views.size())
            return gltf_fail(error, "an accessor without a valid buffer view");
        const int    csize = gltf_component_size(a.value("componentType", 0));
        const int    comps = gltf_type_components(a.value("type", std::string()));
        const size_t count = a.value("count", size_t(0));
        if (csize == 0 || comps == 0 || count == 0)
            return gltf_fail(error, "an accessor with an unknown type or no elements");
        const size_t need = a.value("byteOffset", size_t(0)) + count * size_t(csize) * size_t(comps);
        if (need > views[size_t(view)].value("byteLength", size_t(0)))
            return gltf_fail(error, "an accessor runs past its buffer view");
        acc_count.push_back(count);
    }
    for (const gltf_json &m : meshes) {
        if (!m.contains("primitives") || !m["primitives"].is_array() || m["primitives"].empty())
            return gltf_fail(error, "a mesh without primitives");
        for (const gltf_json &p : m["primitives"]) {
            if (!p.contains("attributes") || !p["attributes"].contains("POSITION"))
                return gltf_fail(error, "a primitive without POSITION");
            const int pos = p["attributes"]["POSITION"].get<int>();
            if (pos < 0 || size_t(pos) >= accessors.size())
                return gltf_fail(error, "POSITION points at no accessor");
            const gltf_json &pa = accessors[size_t(pos)];
            if (pa.value("componentType", 0) != GLTF_FLOAT || pa.value("type", std::string()) != "VEC3" || !pa.contains("min") || !pa.contains("max"))
                return gltf_fail(error, "POSITION is not a float VEC3 with min and max");
            const size_t nv = acc_count[size_t(pos)];
            for (auto it = p["attributes"].begin(); it != p["attributes"].end(); ++it) {
                const int ai = it.value().get<int>();
                if (ai < 0 || size_t(ai) >= accessors.size() || acc_count[size_t(ai)] != nv)
                    return gltf_fail(error, "attribute " + it.key() + " does not match POSITION's count");
                if (it.key() != "POSITION" && it.key() != "NORMAL" && it.key() != "TANGENT" && it.key().rfind("TEXCOORD_", 0) != 0 &&
                    it.key().rfind("COLOR_", 0) != 0 && it.key()[0] != '_')
                    return gltf_fail(error, "custom attribute " + it.key() + " has no leading underscore");
            }
            if (p.contains("indices")) {
                const int ii = p["indices"].get<int>();
                if (ii < 0 || size_t(ii) >= accessors.size())
                    return gltf_fail(error, "indices point at no accessor");
                const gltf_json &ia = accessors[size_t(ii)];
                const int ct = ia.value("componentType", 0);
                if (ct != GLTF_UNSIGNED_INT && ct != GLTF_UNSIGNED_SHORT && ct != GLTF_UNSIGNED_BYTE)
                    return gltf_fail(error, "indices are not unsigned");
                if (acc_count[size_t(ii)] % 3 != 0)
                    return gltf_fail(error, "an index count that is not a multiple of 3");
                const gltf_json &view = views[size_t(ia.value("bufferView", 0))];
                const uint8_t   *base = chunks.bin.data() + view.value("byteOffset", size_t(0)) + ia.value("byteOffset", size_t(0));
                const int        cs   = gltf_component_size(ct);
                for (size_t k = 0; k < acc_count[size_t(ii)]; ++k) {
                    uint32_t v = 0;
                    std::memcpy(&v, base + k * size_t(cs), size_t(cs));
                    if (v >= nv)
                        return gltf_fail(error, "an index past the last vertex");
                }
                st.triangles += acc_count[size_t(ii)] / 3;
            }
            if (p.contains("material")) {
                const int mi = p["material"].get<int>();
                if (mi < 0 || size_t(mi) >= materials.size())
                    return gltf_fail(error, "a primitive's material does not exist");
            }
            st.vertices += nv;
        }
    }
    for (const gltf_json &n : nodes) {
        if (n.contains("mesh")) {
            const int mi = n["mesh"].get<int>();
            if (mi < 0 || size_t(mi) >= meshes.size())
                return gltf_fail(error, "a node's mesh does not exist");
        }
    }
    if (j.contains("scenes"))
        for (const gltf_json &s : j["scenes"])
            if (s.contains("nodes"))
                for (const gltf_json &n : s["nodes"])
                    if (n.get<int>() < 0 || size_t(n.get<int>()) >= nodes.size())
                        return gltf_fail(error, "a scene lists a node that does not exist");
    if (j.contains("extensionsUsed"))
        for (const gltf_json &e : j["extensionsUsed"])
            st.extensions_used.push_back(e.get<std::string>());
    st.meshes    = meshes.size();
    st.nodes     = nodes.size();
    st.materials = materials.size();
    st.accessors = accessors.size();
    if (stats != nullptr)
        *stats = st;
    return true;
}

bool validate_glb(const uint8_t *data, size_t size, GlbStats *stats, std::string *error)
{
    try {
        return validate_glb_impl(data, size, stats, error);
    } catch (const std::exception &e) {
        return gltf_fail(error, std::string("malformed glTF JSON: ") + e.what());
    }
}

} // namespace Showcase
} // namespace Slic3r
