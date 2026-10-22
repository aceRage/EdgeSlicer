#ifndef slic3r_Showcase_ShowcaseVideo_hpp_
#define slic3r_Showcase_ShowcaseVideo_hpp_

// Showcase tab, phase S4: the videographer (design tests/design_showcase_tab.md, section 4). This is
// the GL- and wxWidgets-free half, so the unit tests, the 3MF code and the CLI can use it:
//   - what a scene stores about its video (VideoSettings: the camera path, its timing and the output),
//   - the camera paths: orbit, turntable, orbit with a bob, and spline keyframes (centripetal
//     Catmull-Rom, re-parameterised by arc length, eased per key) - evaluate_path(),
//   - which frames a video has and at which times (seamless loops included),
//   - the frame size a resolution gives for the safe frame's aspect,
//   - the export's progress: which frame is next, how far it is, the ETA and cancelling (VideoJob).
// The encoders (ffmpeg, the PNG sequence) are in ShowcaseEncoder.hpp; the GUI renders the frames.

#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace Showcase {

// From ShowcaseScene.hpp, which includes this file (a scene holds its video settings).
enum class Quality : int;
struct CameraView;

// New values go at the end: the JSON names and the panel's choices follow this order.
// The layer build-up (S5) goes with any of them: VideoSettings::build.
enum class VideoPath   : int { Orbit = 0, Turntable, OrbitBob, Keyframes };
enum class Easing      : int { Linear = 0, EaseInOut, EaseIn, EaseOut };
// MP4 is H.264. WebMAlpha is VP9 with a transparent background. PNGSequence is always available.
enum class VideoFormat : int { MP4 = 0, WebM, WebMAlpha, GIF, WebP, PNGSequence };

const char *to_string(VideoPath v);
const char *to_string(Easing v);
const char *to_string(VideoFormat v);
bool from_string(const std::string &s, VideoPath &v);
bool from_string(const std::string &s, Easing &v);
bool from_string(const std::string &s, VideoFormat &v);

// The file name extension of a format ("" for the PNG sequence, a folder).
const char *video_extension(VideoFormat f);

// One camera position on the spline path, at a time from the start of the motion.
struct Keyframe
{
    double                time = 0.;   // seconds
    std::array<double, 3> eye{ { 0., -200., 150. } };
    std::array<double, 3> target{ { 0., 0., 0. } };
    std::array<double, 3> up{ { 0., 0., 1. } };   // the roll
    double                zoom = 1.;              // the focal length, as the camera's zoom
    Easing                ease = Easing::EaseInOut;   // how the segment arriving at this key speeds up and slows down
    // S5 (compiled paths, ShowcasePath.hpp): how far the model has turned at this key (a turntable
    // in a path; the lights and the environment turn the other way, as for VideoPath::Turntable).
    double                spin = 0.;
    // S5: the camera passes through this key without stopping. A run of glide keys is eased as one
    // segment, from the key before it to the first key after it that does not glide (with that key's
    // ease), at an even speed along the whole run; the glide keys' own times are then only where the
    // run passes them. An orbit in a path is such a run, so it eases in and out once, not per key.
    bool                  glide = false;

    bool operator==(const Keyframe &o) const;
    bool operator!=(const Keyframe &o) const { return !(*this == o); }
};

constexpr double MIN_VIDEO_SECONDS = 1.;
constexpr double MAX_VIDEO_SECONDS = 60.;
constexpr double MAX_HOLD_SECONDS  = 10.;
// S5: a compiled path (an orbit is a key every 15 degrees) needs more than the 64 S4 allowed.
constexpr int    MAX_KEYFRAMES     = 512;

// S5: the layer build-up (design 4.1): the print grows from the plate while the camera moves. With
// toolpaths (a sliced plate) the layers of the slice are shown up to a height, otherwise the model is
// cut by a horizontal plane (the "clip" fallback).
enum class BuildSource : int { Auto = 0, Toolpaths, Clip };
const char *to_string(BuildSource v);
bool        from_string(const std::string &s, BuildSource &v);

struct BuildUp
{
    bool        enabled = false;
    // Over the motion (between the holds): 0 = the empty plate, 1 = the finished print.
    double      from    = 0.;
    double      to      = 1.;
    Easing      easing  = Easing::Linear;
    BuildSource source  = BuildSource::Auto;
    // S5b: what of the slice the build-up lays (toolpaths only; the clip of the model has neither).
    // Both on, as S5 showed every extrusion.
    bool        infill   = true;   // sparse and internal solid infill, internal bridges
    bool        supports = true;   // supports, their interfaces and transitions

    bool operator==(const BuildUp &o) const
    {
        return enabled == o.enabled && from == o.from && to == o.to && easing == o.easing && source == o.source && infill == o.infill &&
               supports == o.supports;
    }
    bool operator!=(const BuildUp &o) const { return !(*this == o); }
};

struct VideoSettings
{
    VideoPath   path       = VideoPath::Orbit;
    double      duration   = 8.;      // seconds of motion (design 4.1: 2-60 s)
    int         fps        = 30;      // 24, 25, 30 or 60
    // The shorter side of the video in pixels; the longer follows the safe frame's aspect.
    int         resolution = 1080;
    Quality     quality    = Quality(1);      // supersampling, as for stills (High)
    int         bitrate    = 60;      // 0..100, the "Quality" slider of the encoder (see video_bitrate_kbps)
    VideoFormat format     = VideoFormat::MP4;
    Easing      easing     = Easing::Linear;  // orbit, turntable and bob
    double      sweep_deg  = 360.;    // orbit and turntable: how far round
    bool        clockwise  = false;   // seen from above
    double      bob_deg    = 10.;     // orbit with bob: up and down by this much
    int         bob_cycles = 1;       // ... this many times
    double      hold_start = 0.;      // seconds standing still before the motion
    double      hold_end   = 0.;      // ... and after it
    bool        loop       = true;    // a full turn: the last frame leads into the first
    bool        callouts   = true;    // draw the scene's callouts (when they are on) in the video
    std::vector<Keyframe> keyframes;  // sorted by time
    // S5: the layer build-up over the motion (the timeline's "build" events override it).
    BuildUp     build;
    // S5: what an imported path file timed besides the camera (lights, backdrop, floor, callouts,
    // the watermark, the build-up): its "events" array, as JSON text (ShowcasePath.hpp). Empty = none.
    std::string events;
    std::string path_name;            // the imported path's name, informational

    bool operator==(const VideoSettings &o) const;
    bool operator!=(const VideoSettings &o) const { return !(*this == o); }
};

// Clamps every field into its range and sorts the keyframes (a loaded file may say anything).
void sanitize(VideoSettings &v);

// --- easing --------------------------------------------------------------------------------------

// 0 -> 0, 1 -> 1, monotonic. EaseInOut is smoothstep, EaseIn / EaseOut quadratic.
double ease(Easing e, double u);

// --- timing --------------------------------------------------------------------------------------

// A full-turn orbit or turntable without holds that loops: frame N would equal frame 0, so the
// frames sample [0, duration) and the video repeats without a stutter (design 4.1).
bool   seamless_loop(const VideoSettings &v);
double total_seconds(const VideoSettings &v);
int    total_frames(const VideoSettings &v);
// Seconds from the start of the video (holds included). The last frame is the end of the path,
// except in a seamless loop.
double frame_time(const VideoSettings &v, int frame);
// How far along the motion a time is, 0..1 (before easing). 0 in the start hold, 1 in the end hold;
// in a seamless loop it never reaches 1.
double motion_fraction(const VideoSettings &v, double t);
// S5: how much of the print the build-up shows at `t` (0 the empty plate .. 1 finished), from
// VideoSettings::build; < 0 when the video has no build-up.
double build_fraction(const VideoSettings &v, double t);

// --- the camera path -----------------------------------------------------------------------------

struct CameraPose
{
    std::array<double, 3> eye{ { 0., -200., 150. } };
    std::array<double, 3> target{ { 0., 0., 0. } };
    std::array<double, 3> up{ { 0., 0., 1. } };   // unit, never along the view
    double                zoom = 1.;
    // Turntable: how far the model has turned (degrees, counter-clockwise from above). The renderer
    // turns the camera, the lights and the environment the other way instead, which looks the same.
    double                spin_deg = 0.;
};

// The pose at `t` seconds from the start of the video. `base` is the scene's camera: where an orbit
// starts and what it circles, and the pose of a keyframe path without keys.
CameraPose evaluate_path(const VideoSettings &v, const CameraView &base, double t);
inline CameraPose pose_at_frame(const VideoSettings &v, const CameraView &base, int frame)
{
    return evaluate_path(v, base, frame_time(v, frame));
}

// The keyframe spline on its own (time in seconds of motion). Keys must be sorted by time.
CameraPose evaluate_keyframes(const std::vector<Keyframe> &keys, double t, const CameraView &fallback);
// S5: when the camera actually passes key `index`: its own time, or for a glide key the moment its
// eased run reaches it (by length along the run).
double     keyframe_run_time(const std::vector<Keyframe> &keys, size_t index);

// Centripetal Catmull-Rom (alpha 0.5) between p1 and p2 at s in [0, 1], p0 and p3 the neighbours.
std::array<double, 3> catmull_rom(const std::array<double, 3> &p0, const std::array<double, 3> &p1, const std::array<double, 3> &p2,
                                  const std::array<double, 3> &p3, double s);
// Spherical interpolation of two directions (unit vectors out).
std::array<double, 3> slerp_dir(const std::array<double, 3> &a, const std::array<double, 3> &b, double s);

// A keyframe from a camera, and inserting one in time order (a key at the same time is replaced).
Keyframe make_keyframe(const CameraView &camera, double time, Easing ease = Easing::EaseInOut);
// Returns the index of the inserted key.
int      insert_keyframe(std::vector<Keyframe> &keys, const Keyframe &key);

// --- size and bitrate ----------------------------------------------------------------------------

// The video's size for a safe frame of aspect_w : aspect_h: the shorter side `resolution` pixels,
// both sides even (H.264 4:2:0 needs it) and at most MAX_EXPORT_SIDE.
void video_size(int resolution, int aspect_w, int aspect_h, int &width, int &height);
// The target bitrate for the encoders that take one: 12 Mb/s at 1080p30 and 40 Mb/s at 4K30 for the
// default quality (design 4.2), scaled by pixels per second and the 0..100 slider.
int  video_bitrate_kbps(int width, int height, int fps, int quality);

// --- the export's progress ------------------------------------------------------------------------

// Which frame the renderer does next, how far the export is and its ETA. The renderer runs a frame
// at a time on the GUI thread; the encoder thread reports the frames it has written.
class VideoJob
{
public:
    explicit VideoJob(int total_frames);

    int    total() const { return m_total; }
    // The frame to render next, or -1 when every frame was rendered or the job was cancelled.
    int    next_frame() const;
    void   frame_rendered(double seconds);
    void   frame_encoded();
    int    rendered() const { return m_rendered; }
    int    encoded() const { return m_encoded; }

    void   cancel() { m_cancelled = true; }
    bool   cancelled() const { return m_cancelled; }
    bool   rendering_done() const { return m_cancelled || m_rendered >= m_total; }
    bool   finished() const { return !m_cancelled && m_encoded >= m_total; }

    // 0..1, the frames written (rendering counts half, so the bar moves before the encoder starts).
    double progress() const;
    // Seconds left, from the average time per frame so far; < 0 while unknown.
    double eta_seconds() const;

private:
    int    m_total     = 0;
    int    m_rendered  = 0;
    int    m_encoded   = 0;
    bool   m_cancelled = false;
    double m_render_seconds = 0.;
};

// --- the callouts' occlusion fade (design 3.3 item 6, R13) -----------------------------------------

// In a video an anchor that goes behind the model fades its leader and dot out, and in again when it
// shows; a label fades with the last of its anchors. The occlusion test of one frame is noisy at the
// model's edges, so a change only counts once it has held for `hysteresis` frames in a row; then the
// fade takes `fade_frames`.
class CalloutFade
{
public:
    explicit CalloutFade(int hysteresis = 10, int fade_frames = 8) : m_hysteresis(hysteresis), m_fade_frames(fade_frames) {}

    // Feeds one frame's occlusion of one anchor; returns its opacity for this frame (0..1).
    double update(const std::string &key, int anchor, bool occluded);
    double alpha(const std::string &key, int anchor) const;
    // A label's opacity: that of its most visible anchor (1 without anchors).
    double label_alpha(const std::string &key, int anchors) const;
    void   reset() { m_states.clear(); }

private:
    struct State
    {
        bool   shown   = true;
        int    pending = 0;    // frames the other state has held
        double alpha   = 1.;
        bool   seen    = false;
    };
    int                                m_hysteresis;
    int                                m_fade_frames;
    std::map<std::pair<std::string, int>, State> m_states;
};

// --- the scene's JSON ------------------------------------------------------------------------------

std::string video_to_json(const VideoSettings &v);
bool        video_from_json(const std::string &text, VideoSettings &out);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseVideo_hpp_
