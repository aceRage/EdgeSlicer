#ifndef slic3r_Showcase_ShowcasePath_hpp_
#define slic3r_Showcase_ShowcasePath_hpp_

// Showcase tab, phase S5: programmatic sequencing. A path file is a versioned JSON document an agent
// (or a person) writes to drive a Showcase video:
//   - keyframes: absolute camera poses at times, with the framing as a field of view or an
//     orthographic scale, a per-key ease and holds;
//   - moves: high-level camera moves chained one after the other (orbit, turntable, dolly, pan,
//     zoom, focus on an object or a filament, fly to a named view, move to a pose, wait);
//   - events: timed changes besides the camera (lights, backdrop, floor, a whole look, callouts shown,
//     hidden or highlighted, the watermark, the layer build-up).
// Moves compile to keyframes deterministically (the same file and project give the same keys), and
// the events are evaluated per frame on top of the scene. The format is documented in
// docs/showcase_paths.md; examples ship in resources/showcase/paths/.
//
// GL- and wxWidgets-free: the panel's import and export, the --showcase-render CLI and the unit tests
// share it.

#include "ShowcaseScene.hpp"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
namespace Showcase {

constexpr const char *PATH_FORMAT_ID      = "edgeslicer-showcase-path";
constexpr int         PATH_FORMAT_VERSION = 1;
// An orbit or a turntable compiles to a key at least every this many degrees.
constexpr double      PATH_ORBIT_STEP_DEG = 15.;
// The shortest move (a camera cut is two keys this far apart).
constexpr double      PATH_MIN_MOVE_SECONDS = 0.05;

using PathVec = std::array<double, 3>;

// --- problems found in a file ----------------------------------------------------------------------

struct PathIssue
{
    std::string where;     // a JSON path into the file, like "moves[2].deg"; empty = the file
    std::string message;
    bool        error = true;   // false = a warning: the path still renders
};

class PathIssues
{
public:
    void error(const std::string &where, const std::string &message) { m_items.push_back({ where, message, true }); }
    void warn(const std::string &where, const std::string &message) { m_items.push_back({ where, message, false }); }

    const std::vector<PathIssue> &items() const { return m_items; }
    bool                          has_errors() const;
    int                           errors() const;
    int                           warnings() const;
    // One line per issue: "error: moves[2].deg: expected a number".
    std::string                   text() const;

private:
    std::vector<PathIssue> m_items;
};

// --- the document --------------------------------------------------------------------------------

// How a pose says what the frame shows. Keep = as the pose before it (the same field of view).
enum class PathFraming : int { Keep = 0, Fov, OrthoScale, Zoom };

struct PathPoseSpec
{
    PathVec     eye{ { 0., -200., 150. } };
    PathVec     target{ { 0., 0., 0. } };
    bool        has_up = false;
    PathVec     up{ { 0., 0., 1. } };
    PathFraming framing = PathFraming::Keep;
    // Fov: the vertical angle across the frame, degrees. OrthoScale: the height the frame shows, mm.
    // Zoom: the camera's own zoom (pixels per mm of the live view; not portable, discouraged).
    double      framing_value = 0.;
};

struct PathKeyframe
{
    std::string  where;
    double       t = 0.;
    PathPoseSpec pose;
    Easing       ease = Easing::EaseInOut;
    double       hold = 0.;    // seconds the camera stays here before going on
    double       spin = 0.;    // how far the model has turned (a turntable), degrees
    bool         glide = false; // pass through without stopping (Keyframe::glide)
};

enum class PathMoveOp : int { Orbit = 0, Turntable, DollyIn, DollyOut, Pan, Zoom, FocusObject, FocusFilament, FlyTo, MoveTo, Wait };
const char *to_string(PathMoveOp v);
bool        from_string(const std::string &s, PathMoveOp &v);

struct PathMove
{
    std::string where;
    PathMoveOp  op       = PathMoveOp::Wait;
    double      duration = 1.;
    Easing      ease     = Easing::EaseInOut;
    // orbit, turntable: degrees round the target (counter-clockwise from above when positive) and,
    // orbit only, degrees up (positive) or down along the way.
    double      deg       = 0.;
    double      elevation = 0.;
    // dolly_in / dolly_out / zoom: by a factor (> 1), or dolly by a distance in mm.
    double      factor   = 0.;
    double      distance = 0.;
    // pan: mm along the frame's right and up, or a world offset.
    double      right = 0., up = 0.;
    bool        has_by = false;
    PathVec     by{ { 0., 0., 0. } };
    // focus_object: the object by its 0-based index or by name; focus_filament: the 1-based slot.
    int         object_index = -1;
    std::string object_name;
    int         filament = 0;
    double      margin   = 1.15;   // focus, fly_to: room round what is framed (1 = touching)
    // fly_to: a built-in view (front, back, left, right, top, bottom, iso) or a saved view's name.
    std::string view;
    // move_to
    PathPoseSpec pose;
};

enum class PathEventType : int { Lighting = 0, Background, Floor, Look, Callouts, Watermark, Build };
const char *to_string(PathEventType v);
bool        from_string(const std::string &s, PathEventType &v);

enum class CalloutAction : int { Show = 0, Hide, Highlight };
const char *to_string(CalloutAction v);
bool        from_string(const std::string &s, CalloutAction &v);

// The light rigs by name (ShowcaseScene.hpp LightRig): soft, classic, dramatic, key_only, sun.
const char *to_string(LightRig v);
bool        from_string(const std::string &s, LightRig &v);

struct PathEvent
{
    std::string   where;
    PathEventType type = PathEventType::Lighting;
    // "at": from then on, blending in over `transition` seconds. "from" / "to": only within that
    // range, blending in and out over `fade` seconds (kept in `transition`).
    bool          range      = false;
    double        at         = 0.;
    double        from       = 0.;
    double        to         = 0.;
    double        transition = 0.;

    // lighting
    std::optional<LightRig>    rig;
    std::optional<double>      azimuth, elevation, key_intensity, temperature, fill, rim, env_intensity, env_rotation,
                               exposure, shadow_intensity, shadow_softness;
    std::optional<std::string> environment;
    std::optional<bool>        shadows, ssao;
    // background and floor
    std::optional<BackgroundType> background;
    std::optional<FloorType>      floor;
    std::optional<std::string>    color1, color2, color;
    std::optional<double>         shadow_strength, reflection, contact_shadow;
    // look: a built-in preset's backdrop, floor and lights
    std::optional<PresetId>       look;
    // callouts: which ones (none named = all of them)
    CalloutAction                 action = CalloutAction::Show;
    std::vector<int>              filaments;
    std::vector<std::string>      labels;   // callout keys ("f2", "f2@12.40") or titles
    // watermark
    std::optional<bool>           enabled;
    std::optional<std::string>    text;
    std::optional<Anchor>         anchor;
    std::optional<double>         opacity, size;
    // build: from `start` to `end` of the print (0..1) over the range, eased
    double                        build_start  = 0.;
    double                        build_end    = 1.;
    Easing                        build_easing = Easing::Linear;
    BuildSource                   build_source = BuildSource::Auto;
    // S5b: lay the infill / the supports (toolpaths only); unset = the scene's choice.
    std::optional<bool>           build_infill, build_supports;
    // S5c: the camera zooms so what is built so far stays framed ("static with zoom out").
    bool                          build_follow = false;
};

enum class PathStart : int { Scene = 0, Default, View, Pose };

struct PathDocument
{
    int                       version = PATH_FORMAT_VERSION;
    std::string               name;
    std::string               description;
    int                       fps = 0;              // 0 = not given (the caller's)
    double                    duration = 0.;        // 0 = as long as the path is
    std::optional<Projection> projection;           // not given = the scene's
    PathStart                 start = PathStart::Scene;
    std::string               start_view;           // PathStart::View
    PathPoseSpec              start_pose;           // PathStart::Pose
    std::vector<PathKeyframe> keyframes;
    std::vector<PathMove>     moves;
    std::vector<PathEvent>    events;
};

// Reads a path file. Errors (a wrong type, a missing field, a value out of range, an unknown
// operation) make it return false; unknown keys are warnings (with the likely meant key). Never
// throws.
bool parse_path(const std::string &text, PathDocument &out, PathIssues &issues);
// The events alone (VideoSettings::events holds them as a JSON array).
bool parse_path_events(const std::string &json_array, std::vector<PathEvent> &out, PathIssues &issues);
// The canonical JSON of events (what parse_path_events reads back to the same events).
std::string path_events_to_json(const std::vector<PathEvent> &events);
std::string path_to_json(const PathDocument &doc);

// --- compiling ------------------------------------------------------------------------------------

// What the moves need to know about the project and the view.
struct PathContext
{
    // Where the path starts: the scene's camera (valid) or, when it has none, the default view.
    CameraView   start;
    CameraView   default_view;
    Projection   projection = Projection::Perspective;
    // The live view's safe frame height in pixels: a camera's zoom is pixels per mm of it, so a field
    // of view or a scale in the file becomes a zoom through it.
    double       frame_height_px = 1000.;

    struct Box
    {
        PathVec min{ { 0., 0., 0. } };
        PathVec max{ { 0., 0., 0. } };
        bool    valid() const { return max[0] >= min[0] && max[1] >= min[1] && max[2] >= min[2] && !(max == min); }
    };
    Box          plate;   // everything the scene shows
    struct Object
    {
        int         index = -1;   // 0-based, model order
        std::string name;
        Box         box;
    };
    std::vector<Object> objects;   // the plate's objects only
    struct Filament
    {
        int filament = 1;
        Box box;
    };
    std::vector<Filament>   filaments;   // where each filament is on the plate
    std::vector<CameraView> views;       // the scene's saved views
    struct CalloutRef
    {
        std::string key;
        int         filament = 1;
        std::string title;
    };
    std::vector<CalloutRef> callouts;    // the scene's callouts (for the warnings only)
    bool                    has_watermark_content = false;
};

struct CompiledPath
{
    std::string           name;
    std::vector<Keyframe> keys;          // sorted, from t = 0 (zoom through frame_height_px)
    double                duration = 0.; // the video's length, seconds
    int                   fps = 0;       // the file's, 0 = not given
    std::optional<Projection> projection;
    std::vector<PathEvent> events;
};

// Moves become keyframes, holds become pairs of equal keys; every pose is checked (a camera inside
// what it looks at, a view that does not exist, an object index out of range ...). False on errors.
bool compile_path(const PathDocument &doc, const PathContext &ctx, CompiledPath &out, PathIssues &issues);

// The compiled path as the scene's video: a keyframe path, its length, the events (and the fps when
// the file gave one). Output settings (size, format, quality) stay as they were.
void apply_compiled_path(const CompiledPath &path, VideoSettings &video);

// The scene's video as a path file: keyframes stay keyframes (their zoom as a field of view or a
// scale), an orbit or a turntable becomes a move from the scene camera, a bob is sampled to keys.
PathDocument export_path(const VideoSettings &video, const PathContext &ctx, const std::string &name);

// The built-in view names fly_to and "start" accept.
const std::vector<std::string> &builtin_view_names();
// A built-in view of a box: from that side at a distance that frames it with `margin` at the field
// of view `fov_deg`. False for an unknown name.
bool builtin_view(const std::string &name, const PathContext::Box &box, double fov_deg, double margin, double frame_height_px,
                  Projection projection, CameraView &out);

// Field of view / scale <-> zoom (zoom = frame_height_px / the height the frame shows at the target).
double path_view_height(double zoom, double frame_height_px);
double path_zoom(double view_height, double frame_height_px);
double path_fov_deg(double view_height, double distance);
double path_view_height_for_fov(double fov_deg, double distance);

// --- the timeline (events at a time) ---------------------------------------------------------------

struct CalloutVisual
{
    double alpha     = 1.;   // 0 hidden .. 1 shown
    double highlight = 0.;   // 0 .. 1
};

struct TimelineState
{
    Scene                                scene;      // the scene at that moment
    std::map<std::string, CalloutVisual> callouts;   // by callout key
    // The layer build-up: < 0 = none (the whole print), else 0..1 of it.
    double                               build = -1.;
    BuildSource                          build_source = BuildSource::Auto;
    // S5b: what of the slice the build-up lays.
    bool                                 build_infill   = true;
    bool                                 build_supports = true;
    // S5c: frame the camera on what is built (a build event's "follow").
    bool                                 build_follow   = false;
};

// The scene at `t` seconds: the base scene with every event up to then applied in order (sorted by
// start, file order for equal starts), blended over their transitions. `video_build` is the scene's
// own build-up at t (build_fraction()), which "build" events replace. Deterministic.
TimelineState evaluate_timeline(const std::vector<PathEvent> &events, const Scene &base, double t, double video_build = -1.);
// The end of the last event (0 without events).
double        timeline_end(const std::vector<PathEvent> &events);
// Whether a callout matches an event's selection (no filaments and no labels = every callout).
bool          callout_selected(const PathEvent &event, const Callout &callout);

// --- the layer build-up --------------------------------------------------------------------------

// The layers a build-up steps through: the tops of the layers (mm, sorted), the plate's or the
// slice's. Empty = unknown (then the height is shared evenly up to `height`).
class BuildSchedule
{
public:
    BuildSchedule() = default;
    // `tops` in any order, duplicates allowed (several objects); `height` the print's top.
    BuildSchedule(std::vector<double> tops, double height);

    size_t layers() const { return m_tops.size(); }
    double height() const { return m_height; }
    // How far the build is at `fraction` (0..1): `layer` is the layer being laid (0-based; = layers()
    // when finished, -1 before the first), `progress` how much of it (0..1); the clip height is where
    // a plane cuts the model (the bottom of that layer plus its progress).
    struct Position
    {
        int    layer    = -1;
        double progress = 0.;
        double z        = 0.;
    };
    Position at(double fraction) const;

private:
    std::vector<double> m_tops;
    double              m_height = 0.;
};

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcasePath_hpp_
