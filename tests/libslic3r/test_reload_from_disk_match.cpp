#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <string>

using namespace Slic3r;

namespace {

// One object per name, each with a single volume of that name, as a 3MF project reads back.
Model project_model(const std::vector<std::string> &names, const std::string &file)
{
    Model model;
    int   index = 0;
    for (const std::string &name : names) {
        ModelObject *object = model.add_object();
        object->name        = name;
        ModelVolume *volume = object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
        volume->name        = name;
        volume->source.input_file = file;
        volume->source.object_idx = index;
        volume->source.volume_idx = 0;
        ++index;
    }
    return model;
}

} // namespace

TEST_CASE("Reload from disk finds the object by name when the remembered indices went stale", "[Model][ReloadFromDisk]")
{
    // The project was saved with three objects whose source indices were 0, 1, 2 ...
    Model saved = project_model({ "Cube", "Sphere", "Cone" }, "project.3mf");
    // ... and the volume of "Cone" later moved to the front, as when the first two objects were deleted
    // and a new one was added: the file read back has its objects in another order.
    Model reread = project_model({ "Cone", "Cube", "Sphere" }, "project.3mf");

    const ModelVolume &cone = *saved.objects[2]->volumes[0];
    REQUIRE(cone.source.object_idx == 2);

    const auto [object_idx, volume_idx] = find_reload_from_disk_match(reread, cone, true, false);
    CHECK(object_idx == 0);
    CHECK(volume_idx == 0);

    const auto [cube_object, cube_volume] = find_reload_from_disk_match(reread, *saved.objects[0]->volumes[0], true, false);
    CHECK(cube_object == 1);
    CHECK(cube_volume == 0);
}

TEST_CASE("Reload from disk keeps the remembered indices while they still name the volume", "[Model][ReloadFromDisk]")
{
    Model saved  = project_model({ "Cube", "Sphere", "Cone" }, "project.3mf");
    Model reread = project_model({ "Cube", "Sphere", "Cone" }, "project.3mf");
    for (int i = 0; i < 3; ++i) {
        const auto [object_idx, volume_idx] = find_reload_from_disk_match(reread, *saved.objects[i]->volumes[0], true, false);
        CHECK(object_idx == i);
        CHECK(volume_idx == 0);
    }
}

TEST_CASE("Reload from disk falls back to the remembered indices for a renamed volume", "[Model][ReloadFromDisk]")
{
    Model saved  = project_model({ "part.stl" }, "part.stl");
    saved.objects[0]->volumes[0]->name = "My renamed part";
    Model reread = project_model({ "part.stl" }, "part.stl");

    const auto [object_idx, volume_idx] = find_reload_from_disk_match(reread, *saved.objects[0]->volumes[0], true, false);
    CHECK(object_idx == 0);
    CHECK(volume_idx == 0);
}

TEST_CASE("Reload from disk matches by name alone without a usable source", "[Model][ReloadFromDisk]")
{
    Model saved  = project_model({ "a.stl" }, "elsewhere/a.stl");
    saved.objects[0]->volumes[0]->source.input_file.clear();
    Model reread = project_model({ "b.stl", "a.stl" }, "a.stl");

    const auto [object_idx, volume_idx] = find_reload_from_disk_match(reread, *saved.objects[0]->volumes[0], false, true);
    CHECK(object_idx == 1);
    CHECK(volume_idx == 0);

    // Neither the file nor the volume name fits: no match.
    const auto [none_object, none_volume] = find_reload_from_disk_match(reread, *saved.objects[0]->volumes[0], false, false);
    CHECK(none_object == -1);
    CHECK(none_volume == -1);
}
