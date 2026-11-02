#ifndef slic3r_Showcase_ShowcaseLens_hpp_
#define slic3r_Showcase_ShowcaseLens_hpp_

// Showcase realism 1h-2 (tests/design_showcase_realism.md 2.8 and 2.7): the CPU side of
//
//   * where the environment backdrop sits: tilted up or down (an infinite panorama), or projected
//     onto a dome with a flat floor the model stands on (ground projection);
//   * the floor that is drawn with a ground-projected environment or a photo backplate (a shadow
//     catcher over the picture);
//   * matching the camera to a photo backplate: its horizon and focal length;
//   * depth of field: the circle of confusion from the lens of the frame, the f-number and the focus.
//
// resources/shaders/140/showcase_background.fs and showcase_dof.fs mirror the GLSL parts; the unit
// tests (tests/libslic3r/test_showcase_lens.cpp) check these. GL- and wxWidgets-free.
//
// Conventions: world Z up, millimetres. A camera "looks down" by a positive pitch. The 35 mm
// equivalent focal length is measured on the diagonal (a 36 x 24 mm frame, half-diagonal 21.633 mm),
// so a frame of any aspect ratio gets the angle of view a full-frame camera would have.

#include "ShowcaseScene.hpp"

#include <array>

namespace Slic3r {
namespace Showcase {

using LensVec = std::array<double, 3>;

// ---- The floor ----------------------------------------------------------------------------------

// The environment is projected onto a ground the model stands on: an Environment backdrop with an
// environment chosen and Ground projection on.
bool      ground_projection_active(const Scene &s);
// The floor as it is drawn. With a ground-projected environment or a photo backplate the picture is
// the floor, so any floor but None is drawn as a shadow catcher over it (its shadow, contact shadow
// and reflection); the scene keeps the type the user chose.
FloorType effective_floor_type(const Scene &s);

// ---- The environment backdrop -------------------------------------------------------------------

// The dome a ground-projected panorama is projected onto: the panorama's camera at `center`
// (`height` above the floor Z = 0, over the middle of the model), a sphere of `radius` around it, cut
// by the floor. The floor disc is where the sphere meets Z = 0.
struct GroundDome
{
    LensVec center{ { 0., 0., 0. } };
    double  height = 0.;
    double  radius = 0.;
    double  floor_radius() const;   // sqrt(radius^2 - height^2)
};

// The automatic height of the panorama's camera: three times the model's extent (its height or half
// its footprint's diagonal, whichever is larger), kept to 60 .. 5000 mm.
double     ground_auto_height(double size_x, double size_y, double size_z);
// The scene's dome. `hdri_height_mm` / `hdri_radius_mm`: the HDRI's own camera height and room
// radius (HdriInfo); only their ratio gives an automatic radius. `model_min` / `model_max`: the plate's
// objects. The radius is kept at least 1.25 x the height so the floor disc is never empty.
GroundDome ground_dome(const Background &bg, double hdri_height_mm, double hdri_radius_mm, const LensVec &model_min, const LensVec &model_max);

// What a ray from `origin` along `dir` (unit) sees of a ground-projected panorama: the direction to
// look the panorama up in (not normalised), and the distance to the floor or the dome along the ray
// (infinity when the ray leaves without hitting either: it sees the panorama as if it were infinitely
// far). Mirrors ground_lookup() in showcase_background.fs and showcase_dof.fs.
LensVec ground_lookup(const GroundDome &dome, const LensVec &origin, const LensVec &dir, double *distance = nullptr);

// The rotation (row-major 3 x 3) that tilts an infinite backdrop by `pitch_deg` about the camera's
// horizontal right axis (from `forward`): + turns the view direction up, so the horizon moves down
// in the frame and more sky shows. Identity for 0 or a camera looking straight up or down.
std::array<double, 9> backdrop_pitch_matrix(double pitch_deg, const LensVec &forward);
LensVec               apply_matrix(const std::array<double, 9> &m, const LensVec &v);

// ---- Camera match (photo backplate) -------------------------------------------------------------

constexpr double FULL_FRAME_HALF_DIAGONAL_MM = 21.633307652783937;   // sqrt(36^2 + 24^2) / 2

// Half the height of a full-frame-equivalent sensor of the frame's aspect ratio (width / height).
double  half_sensor_height_mm(double aspect);
// The full vertical angle of view (radians) of a 35 mm equivalent focal length, and back.
double  fov_y_from_focal(double focal_mm, double aspect);
double  focal_from_fov_y(double fov_y, double aspect);
// The pitch (radians, + = looking down) that puts the horizon at `horizon` of the frame (0 bottom,
// 0.5 middle, 1 top) with a vertical angle of view `fov_y`; and the horizon for a pitch.
double  pitch_for_horizon(double horizon, double fov_y);
double  horizon_for_pitch(double pitch, double fov_y);
// The camera distance that gives `fov_y` across a frame `frame_h_px` pixels high, with the camera's
// zoom (pixels per millimetre at the target) unchanged: the model keeps its size in the frame.
double  distance_for_fov(double frame_h_px, double zoom_px_per_mm, double fov_y);
// Where the camera goes to look at `target` pitched by `pitch` (radians, + = down) from `distance`,
// keeping the compass direction of `forward` (its horizontal part; +Y when it has none).
LensVec matched_eye(const LensVec &target, const LensVec &forward, double pitch, double distance);

// ---- Depth of field -----------------------------------------------------------------------------

// The circle of confusion is capped at this share of the frame height (its diameter).
constexpr double DOF_MAX_COC = 0.05;

// Gather taps per pixel for a bokeh quality.
int dof_taps(BokehQuality q);

// What the depth-of-field pass needs, worked out once per frame.
struct DofSetup
{
    bool   active = false;
    double focus  = 0.;   // focus distance along the view axis, mm
    double k_px   = 0.;   // circle of confusion (diameter, px) of something infinitely far
    double max_px = 0.;   // the cap
    int    taps   = 0;
    double focal_mm = 0.; // the frame's 35 mm equivalent focal length (for the log)
};

// `p11_frame`: the frame's projection(1, 1) (1 / tan(fov_y / 2)); `frame_aspect`: width / height;
// `frame_h_px`: the frame's height in pixels of the render target; `focus_distance`: mm along the view
// axis. Thin lens: the blur on the plane of focus is D |z - focus| / z with the aperture
// D = focal / f_number, in pixels of that plane.
DofSetup dof_setup(const DepthOfField &dof, double p11_frame, double frame_aspect, double frame_h_px, double focus_distance);
// The circle of confusion (diameter, px) of something at view distance `z` (mm), capped.
double   coc_px(const DofSetup &setup, double z);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseLens_hpp_
