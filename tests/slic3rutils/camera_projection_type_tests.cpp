#include <catch2/catch.hpp>

#include "slic3r/GUI/Camera.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

// Orca #12230 (box selection picked the wrong object in orthogonal view): the rectangular selection
// picking pass renders with a second camera that only covers the selection rectangle. A fresh
// Camera is a perspective one, so the pass has to copy the main camera's type or the pick image is
// projected differently from the scene the user is looking at. GLCanvas3D calls
// framebuffer_camera.set_type(camera->get_type()) before apply_projection(); these tests pin the
// Camera behaviour that call relies on.

namespace {

Transform3d pick_projection(Camera::EType main_type, bool copy_type)
{
    Camera main_camera;
    main_camera.set_type(main_type);

    Camera framebuffer_camera;
    if (copy_type)
        framebuffer_camera.set_type(main_camera.get_type());
    framebuffer_camera.apply_projection(-10., 10., -5., 5., 1., 100.);
    return framebuffer_camera.get_projection_matrix();
}

} // namespace

TEST_CASE("A new camera is perspective and set_type switches it", "[CameraProjectionType]")
{
    Camera camera;
    REQUIRE(camera.get_type() == Camera::EType::Perspective);
    camera.set_type(Camera::EType::Ortho);
    REQUIRE(camera.get_type() == Camera::EType::Ortho);
    camera.set_type(Camera::EType::Perspective);
    REQUIRE(camera.get_type() == Camera::EType::Perspective);
}

TEST_CASE("The picking camera projection matches an orthographic main camera only once the type is copied", "[CameraProjectionType]")
{
    const Transform3d ortho_ref = pick_projection(Camera::EType::Ortho, true);
    // orthographic: affine projection, no perspective divide
    CHECK(ortho_ref.matrix()(3, 2) == Approx(0.));
    CHECK(ortho_ref.matrix()(3, 3) == Approx(1.));

    // without the copy the picking camera stays perspective and projects differently (the bug)
    const Transform3d uncopied = pick_projection(Camera::EType::Ortho, false);
    CHECK(uncopied.matrix()(3, 2) == Approx(-1.));
    CHECK_FALSE(uncopied.matrix().isApprox(ortho_ref.matrix()));
}

TEST_CASE("The picking camera projection matches a perspective main camera", "[CameraProjectionType]")
{
    const Transform3d copied   = pick_projection(Camera::EType::Perspective, true);
    const Transform3d uncopied = pick_projection(Camera::EType::Perspective, false);
    CHECK(copied.matrix()(3, 2) == Approx(-1.));
    CHECK(copied.matrix().isApprox(uncopied.matrix()));
}

// Orca #14768: 3D mouse rotation asks for a perspective camera. Without preferences (no GUI_App,
// as in tests and the CLI) auto_type() must be a no-op rather than dereference a missing app.
TEST_CASE("auto_type is a no-op without a GUI app", "[CameraProjectionType]")
{
    Camera camera;
    camera.set_type(Camera::EType::Ortho);
    camera.auto_type(Camera::EType::Perspective);
    CHECK(camera.get_type() == Camera::EType::Ortho);
}
