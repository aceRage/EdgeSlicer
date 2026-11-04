#include "RemotePaintV1.hpp"

#include "libslic3r/ImageFill.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>

namespace Slic3r {
namespace GUI {
namespace PaintV1 {

const std::vector<std::string>& pair_features()
{
    // paint_v1: H1, the read side - objects, sessions, a volume's mesh and the PC's painting;
    // paint_v1_apply: H2, the tablet's paint applied with the per-triangle merge (one undo step).
    static const std::vector<std::string> f = { "paint_v1", "paint_v1_apply" };
    return f;
}

bool is_session_id(const std::string& id)
{
    return id.size() == 16 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string new_session_id()
{
    static std::mutex                            mutex;
    static std::mt19937_64                       rng { std::random_device {}() };
    std::lock_guard<std::mutex>                  lock(mutex);
    char                                         buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long) rng());
    return buf;
}

std::string object_key(const ModelObject& object) { return "o" + std::to_string(object.id().id); }

std::string volume_type_name(ModelVolumeType type)
{
    if (is_precise_seam(type))
        return "seam_modifier";
    switch (type) {
    case ModelVolumeType::MODEL_PART: return "model_part";
    case ModelVolumeType::NEGATIVE_VOLUME: return "negative_volume";
    case ModelVolumeType::PARAMETER_MODIFIER: return "modifier";
    case ModelVolumeType::SUPPORT_BLOCKER: return "support_blocker";
    case ModelVolumeType::SUPPORT_ENFORCER: return "support_enforcer";
    default: return "other";
    }
}

size_t VolumeSnapshot::triangles() const { return mesh ? mesh->its.indices.size() : 0; }
size_t VolumeSnapshot::vertices() const { return mesh ? mesh->its.vertices.size() : 0; }
bool   VolumeSnapshot::paintable() const { return type == ModelVolumeType::MODEL_PART; }

size_t ObjectSnapshot::paint_triangles() const
{
    size_t n = 0;
    for (const VolumeSnapshot& v : volumes)
        if (v.paintable())
            n += v.triangles();
    return n;
}

ObjectSnapshot snapshot(const ModelObject& object, int index, int plate, bool with_paint)
{
    ObjectSnapshot s;
    s.key   = object_key(object);
    s.index = index;
    s.name  = object.name;
    s.plate = plate;
    // 0 = "not set": the PC prints it with filament 1, so the app is told that rather than 0.
    if (const ConfigOption* e = object.config.option("extruder"))
        s.extruder = e->getInt();
    if (s.extruder <= 0)
        s.extruder = 1;
    for (size_t i = 0; i < object.volumes.size(); ++i) {
        const ModelVolume* mv = object.volumes[i];
        VolumeSnapshot     v;
        v.index      = i;
        v.id         = mv->id().id;
        v.name       = mv->name;
        v.type       = mv->type();
        v.mesh       = mv->mesh_ptr();
        v.painted    = mv->is_mm_painted();
        v.airbrushed = mv->airbrush_field != nullptr;
        ImageFillParams params;
        v.image_fill = image_fill_params_of(*mv, params);
        v.extruder   = mv->extruder_id() > 0 ? mv->extruder_id() : s.extruder;
        v.matrix     = mv->get_matrix();
        if (with_paint)
            v.paint = mv->mmu_segmentation_facets.get_data();
        s.volumes.push_back(std::move(v));
    }
    for (const ModelInstance* mi : object.instances)
        s.instances.push_back(mi->get_matrix());
    return s;
}

// ---- hashing ----

namespace {

class Sha256
{
public:
    Sha256() { SHA256_Init(&m_ctx); }
    void update(const void* data, size_t len) { SHA256_Update(&m_ctx, data, len); }
    void update(const std::string& s) { update(s.data(), s.size()); }
    std::string hex()
    {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256_Final(digest, &m_ctx);
        char out[SHA256_DIGEST_LENGTH * 2 + 1];
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
            std::snprintf(out + 2 * i, 3, "%02x", digest[i]);
        return std::string(out, SHA256_DIGEST_LENGTH * 2);
    }

private:
    SHA256_CTX m_ctx;
};

void put_u32(std::string& out, uint32_t v)
{
    const char b[4] = { char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF), char((v >> 24) & 0xFF) };
    out.append(b, 4);
}

void put_f32(std::string& out, float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    put_u32(out, u);
}

} // namespace

std::string encode_mesh(const indexed_triangle_set& its)
{
    std::string out;
    out.reserve(12 + its.vertices.size() * 12 + its.indices.size() * 12);
    out.append("PSM1", 4);
    put_u32(out, uint32_t(its.vertices.size()));
    put_u32(out, uint32_t(its.indices.size()));
    for (const Vec3f& v : its.vertices)
        for (int i = 0; i < 3; ++i)
            put_f32(out, v[i]);
    for (const stl_triangle_vertex_indices& f : its.indices)
        for (int i = 0; i < 3; ++i)
            put_u32(out, uint32_t(f[i]));
    return out;
}

std::string mesh_hash(const indexed_triangle_set& its)
{
    // The body after the header, in slices, so a 2M-triangle mesh is not encoded twice in memory.
    Sha256      sha;
    std::string chunk;
    chunk.reserve(1 << 16);
    auto flush = [&](bool force) {
        if (force || chunk.size() >= (1 << 16) - 12) {
            sha.update(chunk);
            chunk.clear();
        }
    };
    for (const Vec3f& v : its.vertices) {
        for (int i = 0; i < 3; ++i)
            put_f32(chunk, v[i]);
        flush(false);
    }
    for (const stl_triangle_vertex_indices& f : its.indices) {
        for (int i = 0; i < 3; ++i)
            put_u32(chunk, uint32_t(f[i]));
        flush(false);
    }
    flush(true);
    return sha.hex();
}

std::string mesh_hash(const std::shared_ptr<const TriangleMesh>& mesh)
{
    if (!mesh)
        return mesh_hash(indexed_triangle_set());
    // A model's meshes are immutable and shared: an edit replaces the pointer. The weak_ptr tells a
    // freed mesh from a new one that happens to get the same address.
    struct Entry
    {
        std::weak_ptr<const TriangleMesh> mesh;
        std::string                       hash;
    };
    static std::mutex                                   mutex;
    static std::map<const TriangleMesh*, Entry>         cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto                        it = cache.find(mesh.get());
        if (it != cache.end() && !it->second.mesh.expired() && it->second.mesh.lock() == mesh)
            return it->second.hash;
    }
    std::string h = mesh_hash(mesh->its);
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = cache.begin(); it != cache.end();)
        it = it->second.mesh.expired() ? cache.erase(it) : std::next(it);
    cache[mesh.get()] = Entry { mesh, h };
    return h;
}

std::string encode_paint(const TriangleMesh& mesh, const TriangleSelector::TriangleSplittingData& paint)
{
    std::string out("PSP1", 4);
    put_u32(out, 0); // the state count, patched below
    uint32_t states = 0;
    if (!paint.triangles_to_split.empty()) {
        TriangleSelector selector(mesh);
        selector.deserialize(paint);
        for (size_t s = 1; s < paint.used_states.size(); ++s) {
            if (!paint.used_states[s])
                continue;
            std::vector<int>           source;
            const indexed_triangle_set its = selector.get_facets_strict(EnforcerBlockerType(s), &source);
            if (its.indices.empty())
                continue;
            put_u32(out, uint32_t(s));
            put_u32(out, uint32_t(its.indices.size()));
            for (const stl_triangle_vertex_indices& f : its.indices)
                for (int c = 0; c < 3; ++c)
                    for (int i = 0; i < 3; ++i)
                        put_f32(out, its.vertices[size_t(f[c])][i]);
            for (size_t t = 0; t < its.indices.size(); ++t)
                put_u32(out, uint32_t(t < source.size() ? source[t] : -1));
            ++states;
        }
    }
    std::string count;
    put_u32(count, states);
    out.replace(4, 4, count);
    return out;
}

std::string paint_hash(const TriangleSelector::TriangleSplittingData& paint)
{
    Sha256      sha;
    std::string buf;
    put_u32(buf, uint32_t(paint.triangles_to_split.size()));
    for (const TriangleSelector::TriangleBitStreamMapping& m : paint.triangles_to_split) {
        put_u32(buf, uint32_t(m.triangle_idx));
        put_u32(buf, uint32_t(m.bitstream_start_idx));
    }
    put_u32(buf, uint32_t(paint.bitstream.size()));
    uint8_t byte = 0;
    for (size_t i = 0; i < paint.bitstream.size(); ++i) {
        if (paint.bitstream[i])
            byte |= uint8_t(1u << (i % 8));
        if (i % 8 == 7) {
            buf.push_back(char(byte));
            byte = 0;
        }
    }
    if (paint.bitstream.size() % 8 != 0)
        buf.push_back(char(byte));
    sha.update(buf);
    return sha.hex();
}

std::string palette_hash(const std::vector<PaletteSlot>& palette)
{
    Sha256 sha;
    for (const PaletteSlot& p : palette)
        sha.update(p.color + "\t" + p.type + "\t" + p.preset + "\n");
    return sha.hex();
}

std::string revision(const ObjectSnapshot& object, const std::vector<PaletteSlot>& palette)
{
    Sha256 sha;
    // The session is bound to one object already: its volumes by index and type, not their ObjectIDs,
    // so the same project gives the same revision on every run (and in the fixtures).
    sha.update("paint_v1\n");
    for (const VolumeSnapshot& v : object.volumes)
        sha.update("v " + std::to_string(v.index) + " " + volume_type_name(v.type) + " " + mesh_hash(v.mesh) + " " +
                   paint_hash(v.paint) + "\n");
    sha.update("p " + palette_hash(palette) + "\n");
    return sha.hex().substr(0, 16);
}

// ---- apply (H2) ----

namespace {

struct Reader
{
    const std::string& b;
    size_t             at { 0 };
    bool               ok { true };

    uint32_t u32()
    {
        if (at + 4 > b.size()) { ok = false; return 0; }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= uint32_t(uint8_t(b[at + i])) << (8 * i);
        at += 4;
        return v;
    }

    std::string text(size_t n)
    {
        if (at + n > b.size()) { ok = false; return {}; }
        std::string out = b.substr(at, n);
        at += n;
        return out;
    }
};

bool is_hex(const std::string& s) { return s.find_first_not_of("0123456789abcdef") == std::string::npos; }

} // namespace

bool decode_apply(const std::string& body, ApplyRequest& out, std::string& error)
{
    out = ApplyRequest();
    Reader r { body };
    if (r.text(4) != "PSA1") { error = "not a paint apply body"; return false; }
    out.revision = r.text(16);
    const uint32_t n_volumes = r.u32();
    if (!r.ok || !is_hex(out.revision)) { error = "the body is truncated or its revision is not hex"; return false; }
    // Every volume and tree costs at least 8 bytes: a count past that is a lie, refused before allocating.
    if (n_volumes > body.size() / 8) { error = "the body is truncated"; return false; }
    std::set<int> seen;
    for (uint32_t v = 0; v < n_volumes; ++v) {
        ApplyVolume vol;
        vol.index     = int(r.u32());
        vol.triangles = r.u32();
        vol.mesh_hash = r.text(64);
        const uint32_t n_trees = r.u32();
        if (!r.ok || n_trees > body.size() / 8 || !is_hex(vol.mesh_hash)) { error = "the body is truncated"; return false; }
        if (!seen.insert(vol.index).second) { error = "volume " + std::to_string(vol.index) + " is listed twice"; return false; }
        vol.trees.reserve(n_trees);
        for (uint32_t t = 0; t < n_trees; ++t) {
            const uint32_t tri = r.u32(), n = r.u32();
            if (!r.ok || n == 0 || r.at + n > body.size()) { error = "the body is truncated"; return false; }
            vol.trees.emplace_back(int(tri), std::vector<uint8_t>(body.begin() + r.at, body.begin() + r.at + n));
            r.at += n;
        }
        out.volumes.push_back(std::move(vol));
    }
    if (r.at != body.size()) { error = "the body has trailing bytes"; return false; }
    return true;
}

std::string encode_apply(const ApplyRequest& request)
{
    std::string out("PSA1", 4);
    std::string rev = request.revision;
    rev.resize(16, '0');
    out += rev;
    put_u32(out, uint32_t(request.volumes.size()));
    for (const ApplyVolume& v : request.volumes) {
        put_u32(out, uint32_t(v.index));
        put_u32(out, v.triangles);
        std::string hash = v.mesh_hash;
        hash.resize(64, '0');
        out += hash;
        put_u32(out, uint32_t(v.trees.size()));
        for (const PaintWireTree& t : v.trees) {
            put_u32(out, uint32_t(t.first));
            put_u32(out, uint32_t(t.second.size()));
            out.append(reinterpret_cast<const char*>(t.second.data()), t.second.size());
        }
    }
    return out;
}

PaintByVolume paint_by_volume(const ObjectSnapshot& object)
{
    PaintByVolume out;
    for (const VolumeSnapshot& v : object.volumes)
        if (v.paintable())
            out[int(v.index)] = v.paint;
    return out;
}

ApplyPlan plan_apply(const ApplyRequest& request, const ObjectSnapshot& current, const std::string& current_revision,
                     const PaintByVolume* base, size_t filaments)
{
    ApplyPlan plan;
    auto refuse = [&plan](int status, nlohmann::json answer) {
        plan.status = status;
        plan.answer = std::move(answer);
        plan.paint.clear();
        return plan;
    };
    if (request.volumes.empty())
        return refuse(400, { { "error", "nothing to apply" } });
    const bool    moved = request.revision != current_revision;
    nlohmann::json conflicts = nlohmann::json::array();
    for (const ApplyVolume& want : request.volumes) {
        const VolumeSnapshot* vol = nullptr;
        for (const VolumeSnapshot& v : current.volumes)
            if (int(v.index) == want.index && v.paintable())
                vol = &v;
        if (vol == nullptr || !vol->mesh)
            return refuse(400, { { "error", "no such volume (" + std::to_string(want.index) + " is not a model part)" } });
        if (want.triangles != vol->triangles() || want.mesh_hash != mesh_hash(vol->mesh))
            return refuse(409, { { "error", "the mesh changed on the PC" }, { "revision", current_revision }, { "volume", want.index } });
        std::string error;
        for (const PaintWireTree& t : want.trees)
            if (!paint_wire_check_tree(t.second, error))
                return refuse(400, { { "error", "volume " + std::to_string(want.index) + ", triangle " + std::to_string(t.first) + ": " + error } });
        const int top = paint_wire_max_state(want.trees, error);
        if (top < 0)
            return refuse(400, { { "error", error } });
        if (size_t(top) > filaments)
            return refuse(400, { { "error", "filament " + std::to_string(top) + " is not in the project (it has " +
                                            std::to_string(filaments) + ")" } });
        if (moved) {
            if (base == nullptr || base->count(want.index) == 0)
                return refuse(409, { { "error", "the PC's paint changed; read the object again" }, { "revision", current_revision } });
            const TriangleSelector::TriangleSplittingData& then = base->at(want.index);
            nlohmann::json clash = nlohmann::json::array();
            for (const PaintWireTree& t : want.trees)
                if (paint_wire_triangle_bits(then, t.first) != paint_wire_triangle_bits(vol->paint, t.first))
                    clash.push_back(t.first);
            if (!clash.empty())
                conflicts.push_back({ { "volume", want.index }, { "triangles", clash } });
        }
        TriangleSelector::TriangleSplittingData merged;
        if (!paint_wire_apply(vol->triangles(), want.trees, vol->paint, merged, error))
            return refuse(400, { { "error", "volume " + std::to_string(want.index) + ": " + error } });
        plan.paint.emplace_back(want.index, std::move(merged));
        plan.triangles += want.trees.size();
    }
    if (!conflicts.empty())
        return refuse(409, { { "error", "the PC painted some of the same triangles" }, { "revision", current_revision },
                             { "conflicts", conflicts } });
    return plan;
}

// ---- JSON ----

static nlohmann::json matrix_json(const Transform3d& t)
{
    // Column-major, 16 numbers, as Metal / simd_double4x4 take them.
    nlohmann::json m = nlohmann::json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            m.push_back(t.matrix()(r, c));
    return m;
}

static size_t mesh_bytes(const VolumeSnapshot& v) { return 12 + v.vertices() * 12 + v.triangles() * 12; }

nlohmann::json objects_json(const std::vector<ObjectSnapshot>& objects)
{
    nlohmann::json j;
    j["max_triangles"] = MAX_TRIANGLES;
    j["objects"]       = nlohmann::json::array();
    for (const ObjectSnapshot& o : objects) {
        nlohmann::json jo;
        jo["key"]      = o.key;
        jo["obj"]      = o.index;
        jo["name"]     = o.name;
        jo["plate"]    = o.plate;
        jo["extruder"] = o.extruder;
        jo["volumes"]  = nlohmann::json::array();
        size_t bytes = 0;
        bool   painted = false, airbrushed = false;
        for (const VolumeSnapshot& v : o.volumes) {
            nlohmann::json jv;
            jv["index"]      = v.index;
            jv["name"]       = v.name;
            jv["type"]       = volume_type_name(v.type);
            jv["triangles"]  = v.triangles();
            jv["painted"]    = v.painted;
            jv["airbrushed"] = v.airbrushed;
            jv["image_fill"] = v.image_fill;
            jv["extruder"]   = v.extruder;
            jo["volumes"].push_back(jv);
            if (v.paintable()) {
                bytes += mesh_bytes(v);
                painted    = painted || v.painted;
                airbrushed = airbrushed || v.airbrushed;
            }
        }
        const size_t triangles = o.paint_triangles();
        jo["triangles"]  = triangles;
        jo["painted"]    = painted;
        jo["airbrushed"] = airbrushed;
        // The meshes the app downloads; the painting comes on top (about 40 bytes per painted piece).
        jo["size_estimate"] = bytes;
        jo["paintable"]     = triangles > 0 && triangles <= MAX_TRIANGLES;
        if (triangles == 0)
            jo["reason"] = "no model parts";
        else if (triangles > MAX_TRIANGLES)
            jo["reason"] = "too many triangles";
        j["objects"].push_back(jo);
    }
    return j;
}

// The options that decide how deep paint goes into the print (PrintConfig, Multimaterial page).
static const char* const PAINT_DEPTH_KEYS[] = { "paint_depth_mode", "paint_depth_walls", "paint_depth_mm", "paint_infill_override",
                                                "paint_depth_solid_interfaces", "mmu_segmented_region_interlocking_depth" };

static const char* option_type_name(ConfigOptionType t)
{
    switch (t) {
    case coFloat: return "float";
    case coInt: return "int";
    case coBool: return "bool";
    case coEnum: return "enum";
    case coString: return "string";
    case coPercent: return "percent";
    default: return "other";
    }
}

static nlohmann::json option_value_json(const ConfigOption* opt)
{
    switch (opt->type()) {
    case coFloat:
    case coPercent: return opt->getFloat();
    case coInt: return opt->getInt();
    case coBool: return opt->getBool();
    default: return opt->serialize(); // enums by name
    }
}

nlohmann::json paint_depth_json(const DynamicPrintConfig& print, const DynamicPrintConfig& object)
{
    nlohmann::json j;
    j["definitions"] = nlohmann::json::array();
    j["values"]      = nlohmann::json::object();
    for (const char* key : PAINT_DEPTH_KEYS) {
        const ConfigOptionDef* def = print_config_def.get(key);
        if (def == nullptr)
            continue;
        nlohmann::json d;
        d["key"]     = key;
        d["type"]    = option_type_name(def->type);
        d["label"]   = def->label;
        d["tooltip"] = def->tooltip;
        if (!def->sidetext.empty())
            d["unit"] = def->sidetext;
        if (def->min > -FLT_MAX)
            d["min"] = def->min;
        if (def->max < FLT_MAX)
            d["max"] = def->max;
        if (def->type == coEnum) {
            d["enum_values"] = def->enum_values;
            d["enum_labels"] = def->enum_labels;
        }
        j["definitions"].push_back(d);
        const ConfigOption* opt = object.option(key);
        if (opt == nullptr)
            opt = print.option(key);
        if (opt == nullptr && def->default_value)
            opt = def->default_value.get();
        if (opt != nullptr)
            j["values"][key] = option_value_json(opt);
    }
    return j;
}

nlohmann::json session_json(const std::string& id, const std::string& rev, const ObjectSnapshot& object,
                            const std::vector<PaletteSlot>& palette, const nlohmann::json& paint_depth)
{
    nlohmann::json j;
    j["session"]  = id;
    j["revision"] = rev;
    j["object"]   = { { "key", object.key }, { "obj", object.index }, { "name", object.name }, { "plate", object.plate },
                      { "extruder", object.extruder } };
    j["volumes"]  = nlohmann::json::array();
    for (const VolumeSnapshot& v : object.volumes) {
        if (!v.paintable())
            continue;
        nlohmann::json jv;
        jv["index"]      = v.index;
        jv["name"]       = v.name;
        jv["triangles"]  = v.triangles();
        jv["vertices"]   = v.vertices();
        jv["mesh_hash"]  = mesh_hash(v.mesh);
        jv["mesh_bytes"] = mesh_bytes(v);
        jv["painted"]    = v.painted;
        jv["airbrushed"] = v.airbrushed;
        jv["image_fill"] = v.image_fill;
        jv["extruder"]   = v.extruder;
        jv["matrix"]     = matrix_json(v.matrix);
        j["volumes"].push_back(jv);
    }
    j["instances"] = nlohmann::json::array();
    for (const Transform3d& t : object.instances)
        j["instances"].push_back(matrix_json(t));
    j["palette"] = nlohmann::json::array();
    for (size_t i = 0; i < palette.size(); ++i)
        j["palette"].push_back({ { "id", i + 1 }, { "color", palette[i].color }, { "type", palette[i].type }, { "preset", palette[i].preset } });
    j["paint_depth"] = paint_depth;
    j["caps"]        = { { "triangles", MAX_TRIANGLES }, { "leaves", MAX_LEAVES } };
    j["formats"]     = { { "mesh", "psmesh1" }, { "paint", "pspaint1" } };
    return j;
}

} // namespace PaintV1
} // namespace GUI
} // namespace Slic3r
