// Coverage for the --inspect-paint CLI dump (PR 46, Orca #14608).
//
// Drives Slic3r::PaintCLI::inspect_to_json directly rather than shelling out to
// the built exe: the handler in Snapmaker_Orca.cpp only loads the files and
// forwards them here, so everything worth asserting about the dump's shape is
// reachable through this entry point.
//
// The two behaviours the review called out:
//  - two inputs must produce ONE parseable JSON document (an earlier revision
//    dumped once per leftover Model, concatenating documents on stdout, which
//    is not parseable JSON),
//  - an unpainted / empty mesh must report {"empty": true} per paint layer
//    rather than an empty states array or a missing key.

#include <catch2/catch.hpp>

#include "slic3r/Utils/PaintCLI.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "nlohmann/json.hpp"

#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using json = nlohmann::json;

namespace {

// A minimal single-triangle mesh. Enough to give a volume a non-zero facet
// count without depending on any file under TEST_DATA_DIR.
TriangleMesh make_triangle_mesh()
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(10.f, 0.f, 0.f), Vec3f(0.f, 10.f, 0.f)};
    its.indices  = {stl_triangle_vertex_indices(0, 1, 2)};
    return TriangleMesh(std::move(its));
}

// Build a Model carrying one object with one unpainted volume.
Model make_model(const std::string &object_name, TriangleMesh mesh)
{
    Model         model;
    ModelObject  *obj = model.add_object();
    obj->name         = object_name;
    ModelVolume *vol  = obj->add_volume(std::move(mesh));
    vol->name         = object_name + "_volume";
    return model;
}

json dump_to_json(const std::vector<Model> &models, const std::vector<std::string> &sources)
{
    std::ostringstream out;
    PaintCLI::inspect_to_json(models, sources, out);
    // Throws if the stream is not exactly one JSON document, which is the
    // regression this guards against.
    return json::parse(out.str());
}

} // namespace

TEST_CASE("inspect_to_json emits one document for multiple inputs", "[InspectPaint]")
{
    std::vector<Model> models;
    models.push_back(make_model("first", make_triangle_mesh()));
    models.push_back(make_model("second", make_triangle_mesh()));

    const std::vector<std::string> sources{"first.3mf", "second.3mf"};

    std::ostringstream out;
    PaintCLI::inspect_to_json(models, sources, out);
    const std::string text = out.str();

    // Two concatenated documents would parse only up to the first one, so
    // accept_but_not_parse is not enough - require a clean full parse.
    REQUIRE(json::accept(text));
    json root;
    REQUIRE_NOTHROW(root = json::parse(text));

    // A single root object, not an array of two and not two roots.
    REQUIRE(root.is_object());

    // Both inputs are recorded, and both objects are folded into the one
    // objects array with a continuous index across models.
    REQUIRE(root["sources"].size() == 2);
    REQUIRE(root["objects"].size() == 2);
    REQUIRE(root["objects"][0]["index"].get<size_t>() == 0);
    REQUIRE(root["objects"][1]["index"].get<size_t>() == 1);
    REQUIRE(root["objects"][0]["name"].get<std::string>() == "first");
    REQUIRE(root["objects"][1]["name"].get<std::string>() == "second");

    // The summary covers both models rather than just the last one.
    REQUIRE(root["summary"]["objects"].get<size_t>() == 2);
    REQUIRE(root["summary"]["volumes"].get<size_t>() == 2);

    // Nothing was painted, so no volume counts as painted.
    REQUIRE(root["summary"]["volumes_with_paint"].get<size_t>() == 0);
    REQUIRE(root["summary"]["painted_facets_total"].get<size_t>() == 0);
}

TEST_CASE("inspect_to_json reports empty for a mesh with no paint", "[InspectPaint]")
{
    // An empty mesh: no vertices, no facets, and therefore no paint of any kind.
    std::vector<Model> models;
    models.push_back(make_model("empty", TriangleMesh()));

    const json root = dump_to_json(models, {"empty.3mf"});

    REQUIRE(root["objects"].size() == 1);
    const json &vol = root["objects"][0]["volumes"][0];

    REQUIRE(vol["n_facets"].get<size_t>() == 0);
    REQUIRE(vol["painted_facets_total"].get<size_t>() == 0);

    // Every paint layer reports {"empty": true}. FacetsAnnotation::empty() is
    // true for all four, so each layer short-circuits to the empty marker and
    // carries no "states" array at all.
    const json &paints = vol["paints"];
    for (const char *layer : {"supports", "seam", "mmu_segmentation", "fuzzy_skin"}) {
        INFO("paint layer: " << layer);
        REQUIRE(paints.contains(layer));
        REQUIRE(paints[layer].contains("empty"));
        REQUIRE(paints[layer]["empty"].get<bool>() == true);
    }

    REQUIRE(root["summary"]["volumes_with_paint"].get<size_t>() == 0);
    REQUIRE(root["summary"]["painted_facets_total"].get<size_t>() == 0);
}

TEST_CASE("inspect_to_json reports empty paint layers on an unpainted solid mesh", "[InspectPaint]")
{
    // Distinct from the empty-mesh case: the volume has real geometry, so
    // n_facets is non-zero, but no paint has been applied to it.
    std::vector<Model> models;
    models.push_back(make_model("solid", make_triangle_mesh()));

    const json root = dump_to_json(models, {"solid.3mf"});

    const json &vol = root["objects"][0]["volumes"][0];
    REQUIRE(vol["n_facets"].get<size_t>() == 1);
    REQUIRE(vol["painted_facets_total"].get<size_t>() == 0);

    const json &paints = vol["paints"];
    for (const char *layer : {"supports", "seam", "mmu_segmentation", "fuzzy_skin"}) {
        INFO("paint layer: " << layer);
        REQUIRE(paints[layer]["empty"].get<bool>() == true);
    }

    // The dump always carries its frame marker so consumers know the
    // coordinates are mesh-local rather than world.
    REQUIRE(root["frame"].get<std::string>() == "mesh_local");
}
