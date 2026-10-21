#include "ShowcaseVideo.hpp"
#include "ShowcaseScene.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace Showcase {

using json = nlohmann::json;

namespace {

constexpr double PI = 3.14159265358979323846;

const char *const PATH_NAMES[]   = { "orbit", "turntable", "orbit_bob", "keyframes" };
const char *const EASING_NAMES[] = { "linear", "ease_in_out", "ease_in", "ease_out" };
const char *const FORMAT_NAMES[] = { "mp4", "webm", "webm_alpha", "gif", "webp", "png_sequence" };
const char *const BUILD_SOURCE_NAMES[] = { "auto", "toolpaths", "clip" };

template<class E, size_t N> const char *video_enum_name(E v, const char *const (&names)[N])
{
    const int i = int(v);
    return i >= 0 && size_t(i) < N ? names[i] : names[0];
}

template<class E, size_t N> bool video_enum_parse(const std::string &s, E &v, const char *const (&names)[N])
{
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) {
            v = E(int(i));
            return true;
        }
    return false;
}

using V3 = std::array<double, 3>;

V3     v_add(const V3 &a, const V3 &b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
V3     v_sub(const V3 &a, const V3 &b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
V3     v_mul(const V3 &a, double s) { return { a[0] * s, a[1] * s, a[2] * s }; }
double v_dot(const V3 &a, const V3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double v_len(const V3 &a) { return std::sqrt(v_dot(a, a)); }
V3     v_lerp(const V3 &a, const V3 &b, double s) { return v_add(v_mul(a, 1. - s), v_mul(b, s)); }
V3     v_norm(const V3 &a, const V3 &fallback = { 0., 0., 1. })
{
    const double l = v_len(a);
    return l > 1e-12 ? v_mul(a, 1. / l) : fallback;
}
bool v_finite(const V3 &a) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); }

// An up vector at right angles to the view, so the camera can always be built from it.
V3 safe_up(const V3 &eye, const V3 &target, const V3 &up)
{
    const V3 dir = v_norm(v_sub(target, eye), { 0., 1., 0. });
    for (const V3 &candidate : { up, V3{ 0., 0., 1. }, V3{ 0., 1., 0. }, V3{ 1., 0., 0. } }) {
        const V3 u = v_sub(candidate, v_mul(dir, v_dot(candidate, dir)));
        if (v_len(u) > 1e-4)
            return v_norm(u);
    }
    return { 0., 0., 1. };
}

double clamp_finite(double v, double lo, double hi, double def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; }

} // namespace

const char *to_string(VideoPath v) { return video_enum_name(v, PATH_NAMES); }
const char *to_string(Easing v) { return video_enum_name(v, EASING_NAMES); }
const char *to_string(VideoFormat v) { return video_enum_name(v, FORMAT_NAMES); }
bool from_string(const std::string &s, VideoPath &v) { return video_enum_parse(s, v, PATH_NAMES); }
bool from_string(const std::string &s, Easing &v) { return video_enum_parse(s, v, EASING_NAMES); }
bool from_string(const std::string &s, VideoFormat &v) { return video_enum_parse(s, v, FORMAT_NAMES); }
const char *to_string(BuildSource v) { return video_enum_name(v, BUILD_SOURCE_NAMES); }
bool from_string(const std::string &s, BuildSource &v) { return video_enum_parse(s, v, BUILD_SOURCE_NAMES); }

const char *video_extension(VideoFormat f)
{
    switch (f) {
    case VideoFormat::MP4: return "mp4";
    case VideoFormat::WebM:
    case VideoFormat::WebMAlpha: return "webm";
    case VideoFormat::GIF: return "gif";
    case VideoFormat::WebP: return "webp";
    default: return "";
    }
}

bool Keyframe::operator==(const Keyframe &o) const
{
    return time == o.time && eye == o.eye && target == o.target && up == o.up && zoom == o.zoom && ease == o.ease && spin == o.spin &&
           glide == o.glide;
}

bool VideoSettings::operator==(const VideoSettings &o) const
{
    return path == o.path && duration == o.duration && fps == o.fps && resolution == o.resolution && quality == o.quality &&
           bitrate == o.bitrate && format == o.format && easing == o.easing && sweep_deg == o.sweep_deg && clockwise == o.clockwise &&
           bob_deg == o.bob_deg && bob_cycles == o.bob_cycles && hold_start == o.hold_start && hold_end == o.hold_end && loop == o.loop &&
           callouts == o.callouts && keyframes == o.keyframes && build == o.build && events == o.events && path_name == o.path_name;
}

void sanitize(VideoSettings &v)
{
    v.duration   = clamp_finite(v.duration, MIN_VIDEO_SECONDS, MAX_VIDEO_SECONDS, 8.);
    v.fps        = std::clamp(v.fps, 10, 120);
    v.resolution = std::clamp(v.resolution, 144, 4320);
    v.bitrate    = std::clamp(v.bitrate, 0, 100);
    if (int(v.quality) < int(Quality::Draft) || int(v.quality) > int(Quality::Best))
        v.quality = Quality::High;
    v.sweep_deg  = clamp_finite(v.sweep_deg, 10., 1080., 360.);
    v.bob_deg    = clamp_finite(v.bob_deg, 0., 60., 10.);
    v.bob_cycles = std::clamp(v.bob_cycles, 1, 8);
    v.hold_start = clamp_finite(v.hold_start, 0., MAX_HOLD_SECONDS, 0.);
    v.hold_end   = clamp_finite(v.hold_end, 0., MAX_HOLD_SECONDS, 0.);
    v.build.from = clamp_finite(v.build.from, 0., 1., 0.);
    v.build.to   = clamp_finite(v.build.to, 0., 1., 1.);
    if (int(v.build.easing) < int(Easing::Linear) || int(v.build.easing) > int(Easing::EaseOut))
        v.build.easing = Easing::Linear;
    if (int(v.build.source) < int(BuildSource::Auto) || int(v.build.source) > int(BuildSource::Clip))
        v.build.source = BuildSource::Auto;
    std::vector<Keyframe> keys;
    for (Keyframe k : v.keyframes) {
        if (!std::isfinite(k.time) || !v_finite(k.eye) || !v_finite(k.target) || !v_finite(k.up))
            continue;
        if (v_len(v_sub(k.eye, k.target)) < 1e-9)
            continue;   // no direction to look in
        k.time = std::clamp(k.time, 0., MAX_VIDEO_SECONDS);
        k.zoom = std::isfinite(k.zoom) && k.zoom > 0. ? k.zoom : 1.;
        k.up   = v_norm(k.up);
        k.spin = std::isfinite(k.spin) ? k.spin : 0.;
        insert_keyframe(keys, k);
    }
    v.keyframes = std::move(keys);
}

// ------------------------------------------------------------------------------------------------
// Easing and timing
// ------------------------------------------------------------------------------------------------

double ease(Easing e, double u)
{
    u = std::isfinite(u) ? std::clamp(u, 0., 1.) : 0.;
    switch (e) {
    case Easing::EaseInOut: return u * u * (3. - 2. * u);
    case Easing::EaseIn: return u * u;
    case Easing::EaseOut: return 1. - (1. - u) * (1. - u);
    default: return u;
    }
}

bool seamless_loop(const VideoSettings &v)
{
    if (!v.loop || v.path == VideoPath::Keyframes || v.hold_start > 0. || v.hold_end > 0. || v.build.enabled)
        return false;
    // A whole number of turns comes back where it started.
    const double turns = v.sweep_deg / 360.;
    return std::abs(turns - std::round(turns)) < 1e-9 && std::round(turns) >= 1.;
}

double total_seconds(const VideoSettings &v) { return v.hold_start + v.duration + v.hold_end; }

int total_frames(const VideoSettings &v) { return std::max(1, int(std::lround(total_seconds(v) * double(std::max(1, v.fps))))); }

double frame_time(const VideoSettings &v, int frame)
{
    const int n = total_frames(v);
    frame       = std::clamp(frame, 0, n - 1);
    if (seamless_loop(v))
        return double(frame) / double(std::max(1, v.fps));
    return n <= 1 ? 0. : double(frame) * total_seconds(v) / double(n - 1);
}

double motion_fraction(const VideoSettings &v, double t)
{
    if (!(v.duration > 0.) || t <= v.hold_start)
        return 0.;
    return std::clamp((t - v.hold_start) / v.duration, 0., 1.);
}

double build_fraction(const VideoSettings &v, double t)
{
    if (!v.build.enabled)
        return -1.;
    return v.build.from + (v.build.to - v.build.from) * ease(v.build.easing, motion_fraction(v, t));
}

// ------------------------------------------------------------------------------------------------
// Paths
// ------------------------------------------------------------------------------------------------

std::array<double, 3> catmull_rom(const V3 &p0, const V3 &p1, const V3 &p2, const V3 &p3, double s)
{
    s = std::clamp(s, 0., 1.);
    // Centripetal knots (alpha 0.5): no cusps or self-intersections within a segment, even with
    // unevenly spaced keys. Coincident points get a tiny interval instead of a division by zero.
    auto knot = [](const V3 &a, const V3 &b) { return std::max(1e-6, std::sqrt(v_len(v_sub(b, a)))); };
    const double t0 = 0., t1 = t0 + knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
    const double t  = t1 + s * (t2 - t1);
    const V3 a1 = v_add(v_mul(p0, (t1 - t) / (t1 - t0)), v_mul(p1, (t - t0) / (t1 - t0)));
    const V3 a2 = v_add(v_mul(p1, (t2 - t) / (t2 - t1)), v_mul(p2, (t - t1) / (t2 - t1)));
    const V3 a3 = v_add(v_mul(p2, (t3 - t) / (t3 - t2)), v_mul(p3, (t - t2) / (t3 - t2)));
    const V3 b1 = v_add(v_mul(a1, (t2 - t) / (t2 - t0)), v_mul(a2, (t - t0) / (t2 - t0)));
    const V3 b2 = v_add(v_mul(a2, (t3 - t) / (t3 - t1)), v_mul(a3, (t - t1) / (t3 - t1)));
    return v_add(v_mul(b1, (t2 - t) / (t2 - t1)), v_mul(b2, (t - t1) / (t2 - t1)));
}

std::array<double, 3> slerp_dir(const V3 &a_in, const V3 &b_in, double s)
{
    const V3     a = v_norm(a_in), b = v_norm(b_in);
    const double d = std::clamp(v_dot(a, b), -1., 1.);
    if (d > 0.9995)
        return v_norm(v_lerp(a, b, s));
    if (d < -0.9995) {
        // Opposite: turn about any axis at right angles to them.
        V3 axis = std::abs(a[0]) < 0.9 ? V3{ 1., 0., 0. } : V3{ 0., 1., 0. };
        axis    = v_norm(v_sub(axis, v_mul(a, v_dot(axis, a))));
        const double ang = PI * s;
        return v_norm(v_add(v_mul(a, std::cos(ang)), v_mul(axis, std::sin(ang))));
    }
    const double theta = std::acos(d);
    const double sn    = std::sin(theta);
    return v_norm(v_add(v_mul(a, std::sin((1. - s) * theta) / sn), v_mul(b, std::sin(s * theta) / sn)));
}

namespace {

// Maps an eased share of a segment to the curve parameter that has covered that share of its
// length (arc-length re-parameterisation), so the camera moves at an even speed between keys.
double arc_length_param(const V3 &p0, const V3 &p1, const V3 &p2, const V3 &p3, double s)
{
    constexpr int N = 48;
    double        cum[N + 1];
    cum[0]  = 0.;
    V3 prev = p1;
    for (int k = 1; k <= N; ++k) {
        const V3 p = catmull_rom(p0, p1, p2, p3, double(k) / N);
        cum[k]     = cum[k - 1] + v_len(v_sub(p, prev));
        prev       = p;
    }
    const double total = cum[N];
    if (total < 1e-9)
        return -1.;   // no length to measure by
    const double want = std::clamp(s, 0., 1.) * total;
    const int    k    = int(std::upper_bound(cum, cum + N + 1, want) - cum) - 1;
    if (k >= N)
        return 1.;
    const double seg = cum[k + 1] - cum[k];
    const double f   = seg > 1e-12 ? (want - cum[k]) / seg : 0.;
    return (double(k) + f) / double(N);
}

CameraPose pose_of(const V3 &eye, const V3 &target, const V3 &up, double zoom)
{
    CameraPose p;
    p.eye    = eye;
    p.target = target;
    p.up     = safe_up(eye, target, up);
    p.zoom   = zoom > 0. && std::isfinite(zoom) ? zoom : 1.;
    return p;
}

CameraPose pose_of(const Keyframe &k) { return pose_of(k.eye, k.target, k.up, k.zoom); }

} // namespace

namespace {

// The neighbours of segment i (keys i and i + 1) of one channel; at the ends a mirrored phantom key
// keeps the curve's end straight.
void segment_neighbours(const std::vector<Keyframe> &keys, size_t i, const V3 Keyframe::*m, V3 &p0, V3 &p3)
{
    const Keyframe &k1 = keys[i], &k2 = keys[i + 1];
    p0 = i > 0 ? keys[i - 1].*m : v_sub(v_mul(k1.*m, 2.), k2.*m);
    p3 = i + 2 < keys.size() ? keys[i + 2].*m : v_sub(v_mul(k2.*m, 2.), k1.*m);
}

// The length of segment i of a channel (the polyline the arc length re-parameterisation measures).
double segment_length(const std::vector<Keyframe> &keys, size_t i, const V3 Keyframe::*m)
{
    V3 p0, p3;
    segment_neighbours(keys, i, m, p0, p3);
    constexpr int N = 48;
    double        len  = 0.;
    V3            prev = keys[i].*m;
    for (int k = 1; k <= N; ++k) {
        const V3 p = catmull_rom(p0, keys[i].*m, keys[i + 1].*m, p3, double(k) / N);
        len += v_len(v_sub(p, prev));
        prev = p;
    }
    return len;
}

// The pose on segment i at `s` (0..1, already eased: the share of the segment's length covered).
CameraPose pose_on_segment(const std::vector<Keyframe> &keys, size_t i, double s)
{
    const Keyframe &k1 = keys[i], &k2 = keys[i + 1];
    V3 e0, e3, t0, t3;
    segment_neighbours(keys, i, &Keyframe::eye, e0, e3);
    segment_neighbours(keys, i, &Keyframe::target, t0, t3);
    // Even speed along the eye's path; along the target's when the eye stands still (a pan).
    double param = arc_length_param(e0, k1.eye, k2.eye, e3, s);
    if (param < 0.)
        param = arc_length_param(t0, k1.target, k2.target, t3, s);
    if (param < 0.)
        param = s;
    const V3     eye    = catmull_rom(e0, k1.eye, k2.eye, e3, param);
    const V3     target = catmull_rom(t0, k1.target, k2.target, t3, param);
    const V3     up     = slerp_dir(k1.up, k2.up, param);
    const double z1 = std::max(1e-6, k1.zoom), z2 = std::max(1e-6, k2.zoom);
    const double zoom   = std::exp(std::log(z1) + (std::log(z2) - std::log(z1)) * param);
    CameraPose   pose   = pose_of(eye, target, up, zoom);
    pose.spin_deg       = k1.spin + (k2.spin - k1.spin) * param;
    return pose;
}

} // namespace

CameraPose evaluate_keyframes(const std::vector<Keyframe> &keys, double t, const CameraView &fallback)
{
    if (keys.empty())
        return pose_of(fallback.eye, fallback.target, fallback.up, fallback.zoom);
    auto with_spin = [](CameraPose p, const Keyframe &k) {
        p.spin_deg = k.spin;
        return p;
    };
    if (keys.size() == 1 || !(t > keys.front().time))
        return with_spin(pose_of(keys.front()), keys.front());
    if (t >= keys.back().time)
        return with_spin(pose_of(keys.back()), keys.back());
    size_t i = 0;
    while (i + 2 < keys.size() && keys[i + 1].time <= t)
        ++i;

    // S5: the run of glide keys this segment belongs to, from the last key before it that stops to
    // the first after it that does.
    size_t a = i, b = i + 1;
    while (a > 0 && keys[a].glide)
        --a;
    while (b + 1 < keys.size() && keys[b].glide)
        ++b;
    if (b == a + 1) {
        const Keyframe &k1 = keys[a], &k2 = keys[b];
        const double    dt = k2.time - k1.time;
        if (dt <= 1e-9)
            return with_spin(pose_of(k2), k2);
        return pose_on_segment(keys, a, ease(k2.ease, (t - k1.time) / dt));
    }
    const double dt = keys[b].time - keys[a].time;
    if (dt <= 1e-9)
        return with_spin(pose_of(keys[b]), keys[b]);
    const double s = ease(keys[b].ease, (t - keys[a].time) / dt);
    // Even speed along the whole run: by the eye's path, else the target's, else by the keys' times.
    std::vector<double> lengths;
    double              total = 0.;
    for (const V3 Keyframe::*m : { &Keyframe::eye, &Keyframe::target }) {
        lengths.clear();
        total = 0.;
        for (size_t j = a; j < b; ++j) {
            lengths.push_back(segment_length(keys, j, m));
            total += lengths.back();
        }
        if (total > 1e-9)
            break;
    }
    if (total <= 1e-9) {
        lengths.clear();
        total = 0.;
        for (size_t j = a; j < b; ++j) {
            lengths.push_back(std::max(0., keys[j + 1].time - keys[j].time));
            total += lengths.back();
        }
        if (total <= 1e-9)
            return with_spin(pose_of(keys[b]), keys[b]);
    }
    double want = s * total;
    for (size_t j = a; j < b; ++j) {
        const double len = lengths[j - a];
        if (want <= len || j + 1 == b)
            return pose_on_segment(keys, j, len > 1e-12 ? std::clamp(want / len, 0., 1.) : 1.);
        want -= len;
    }
    return with_spin(pose_of(keys[b]), keys[b]);
}

double keyframe_run_time(const std::vector<Keyframe> &keys, size_t index)
{
    if (index >= keys.size())
        return 0.;
    if (!keys[index].glide || index == 0 || index + 1 >= keys.size())
        return keys[index].time;
    size_t a = index, b = index;
    while (a > 0 && keys[a].glide)
        --a;
    while (b + 1 < keys.size() && keys[b].glide)
        ++b;
    double total = 0., upto = 0.;
    for (size_t j = a; j < b; ++j) {
        const double len = segment_length(keys, j, &Keyframe::eye);
        total += len;
        if (j < index)
            upto += len;
    }
    if (total <= 1e-9)
        return keys[index].time;
    // The time the eased run covers that share of its length: invert the ease by bisection.
    const double share = upto / total, t0 = keys[a].time, t1 = keys[b].time;
    double lo = 0., hi = 1.;
    for (int k = 0; k < 50; ++k) {
        const double mid = 0.5 * (lo + hi);
        (ease(keys[b].ease, mid) < share ? lo : hi) = mid;
    }
    return t0 + (t1 - t0) * 0.5 * (lo + hi);
}

CameraPose evaluate_path(const VideoSettings &v, const CameraView &base, double t)
{
    const double u = motion_fraction(v, t);
    if (v.path == VideoPath::Keyframes)
        return evaluate_keyframes(v.keyframes, u * v.duration, base);

    // Orbit, turntable and bob: round the base camera's target, at its distance and height.
    V3     target = base.target;
    V3     d      = v_sub(base.eye, base.target);
    double r      = v_len(d);
    if (r < 1e-9) {
        d = { 0., -200., 150. };
        r = v_len(d);
    }
    const double a0    = std::atan2(d[1], d[0]);
    const double e0    = std::atan2(d[2], std::hypot(d[0], d[1]));
    const double e     = ease(v.easing, u);
    const double angle = v.sweep_deg * e * (v.clockwise ? -1. : 1.);   // degrees
    double       az    = a0;
    double       elev  = e0;
    CameraPose   pose;
    switch (v.path) {
    case VideoPath::Turntable:
        // The model turns by `angle`: the camera (and, in the renderer, the lights) turn back.
        az            = a0 - angle * PI / 180.;
        pose.spin_deg = angle;
        break;
    case VideoPath::OrbitBob:
        az   = a0 + angle * PI / 180.;
        elev = e0 + v.bob_deg * PI / 180. * std::sin(2. * PI * double(v.bob_cycles) * e);
        break;
    default: az = a0 + angle * PI / 180.; break;
    }
    elev = std::clamp(elev, -89. * PI / 180., 89. * PI / 180.);
    const V3 eye = v_add(target, V3{ r * std::cos(elev) * std::cos(az), r * std::cos(elev) * std::sin(az), r * std::sin(elev) });
    const CameraPose p = pose_of(eye, target, { 0., 0., 1. }, base.zoom);
    pose.eye    = p.eye;
    pose.target = p.target;
    pose.up     = p.up;
    pose.zoom   = p.zoom;
    return pose;
}

Keyframe make_keyframe(const CameraView &camera, double time, Easing e)
{
    Keyframe k;
    k.time   = std::clamp(std::isfinite(time) ? time : 0., 0., MAX_VIDEO_SECONDS);
    k.eye    = camera.eye;
    k.target = camera.target;
    k.up     = v_norm(camera.up);
    k.zoom   = camera.zoom > 0. && std::isfinite(camera.zoom) ? camera.zoom : 1.;
    k.ease   = e;
    return k;
}

int insert_keyframe(std::vector<Keyframe> &keys, const Keyframe &key)
{
    for (size_t i = 0; i < keys.size(); ++i)
        if (std::abs(keys[i].time - key.time) < 1e-6) {
            keys[i] = key;
            return int(i);
        }
    if (int(keys.size()) >= MAX_KEYFRAMES)
        return -1;
    auto it = std::upper_bound(keys.begin(), keys.end(), key.time, [](double t, const Keyframe &k) { return t < k.time; });
    it      = keys.insert(it, key);
    return int(it - keys.begin());
}

// ------------------------------------------------------------------------------------------------
// Size and bitrate
// ------------------------------------------------------------------------------------------------

void video_size(int resolution, int aspect_w, int aspect_h, int &width, int &height)
{
    if (aspect_w <= 0 || aspect_h <= 0) {
        aspect_w = 16;
        aspect_h = 9;
    }
    const double shorter = double(std::clamp(resolution, 144, 4320));
    double       w, h;
    if (aspect_w >= aspect_h) {
        h = shorter;
        w = shorter * double(aspect_w) / double(aspect_h);
    } else {
        w = shorter;
        h = shorter * double(aspect_h) / double(aspect_w);
    }
    const double longer = std::max(w, h);
    if (longer > double(MAX_EXPORT_SIDE)) {
        w *= double(MAX_EXPORT_SIDE) / longer;
        h *= double(MAX_EXPORT_SIDE) / longer;
    }
    // Even sides, rounded down so the cap holds.
    width  = std::max(2, (int(std::lround(w)) / 2) * 2);
    height = std::max(2, (int(std::lround(h)) / 2) * 2);
    width  = std::min(width, MAX_EXPORT_SIDE);
    height = std::min(height, MAX_EXPORT_SIDE);
}

int video_bitrate_kbps(int width, int height, int fps, int quality)
{
    // 12 Mb/s at 1080p30 and 40 Mb/s at 4K30 (four times the pixels) at the default 60: the rate grows
    // as pixels per second to the power log(40/12)/log(4); every 20 steps of the slider double it.
    const double ref      = 1920. * 1080. * 30.;
    const double rate     = double(std::max(2, width)) * double(std::max(2, height)) * double(std::max(1, fps));
    const double exponent = std::log(40. / 12.) / std::log(4.);
    const double kbps     = 12000. * std::pow(rate / ref, exponent) * std::pow(2., (double(std::clamp(quality, 0, 100)) - 60.) / 20.);
    return int(std::clamp(std::lround(kbps), 300L, 200000L));
}

// ------------------------------------------------------------------------------------------------
// The export's progress
// ------------------------------------------------------------------------------------------------

VideoJob::VideoJob(int total_frames) : m_total(std::max(1, total_frames)) {}

int VideoJob::next_frame() const { return rendering_done() ? -1 : m_rendered; }

void VideoJob::frame_rendered(double seconds)
{
    if (rendering_done())
        return;
    ++m_rendered;
    if (std::isfinite(seconds) && seconds > 0.)
        m_render_seconds += seconds;
}

void VideoJob::frame_encoded()
{
    if (m_encoded < m_rendered)
        ++m_encoded;
}

double VideoJob::progress() const { return std::clamp(0.5 * double(m_rendered + m_encoded) / double(m_total), 0., 1.); }

double VideoJob::eta_seconds() const
{
    if (m_rendered <= 0 || m_render_seconds <= 0.)
        return -1.;
    return m_render_seconds / double(m_rendered) * double(m_total - m_rendered);
}

// ------------------------------------------------------------------------------------------------
// The callouts' fade
// ------------------------------------------------------------------------------------------------

double CalloutFade::update(const std::string &key, int anchor, bool occluded)
{
    State &st = m_states[{ key, anchor }];
    if (!st.seen) {
        // The first frame shows the anchor as it is, without a fade.
        st.seen  = true;
        st.shown = !occluded;
        st.alpha = st.shown ? 1. : 0.;
        return st.alpha;
    }
    if (st.shown == occluded) {
        // The other state: it counts once it has held for the hysteresis.
        if (++st.pending >= std::max(1, m_hysteresis)) {
            st.shown   = !occluded;
            st.pending = 0;
        }
    } else
        st.pending = 0;
    const double step = 1. / double(std::max(1, m_fade_frames));
    st.alpha          = st.shown ? std::min(1., st.alpha + step) : std::max(0., st.alpha - step);
    return st.alpha;
}

double CalloutFade::alpha(const std::string &key, int anchor) const
{
    auto it = m_states.find({ key, anchor });
    return it == m_states.end() ? 1. : it->second.alpha;
}

double CalloutFade::label_alpha(const std::string &key, int anchors) const
{
    if (anchors <= 0)
        return 1.;
    double a = 0.;
    for (int i = 0; i < anchors; ++i)
        a = std::max(a, alpha(key, i));
    return a;
}

// ------------------------------------------------------------------------------------------------
// JSON
// ------------------------------------------------------------------------------------------------

namespace {

template<class T> T vget(const json &j, const char *key, const T &def)
{
    if (!j.is_object())
        return def;
    auto it = j.find(key);
    if (it == j.end())
        return def;
    try {
        return it->get<T>();
    } catch (...) {
        return def;
    }
}

template<class E> E venum(const json &j, const char *key, E def)
{
    E v = def;
    const std::string s = vget<std::string>(j, key, std::string());
    return (!s.empty() && from_string(s, v)) ? v : def;
}

V3 vvec(const json &j, const char *key, const V3 &def)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_array() || j[key].size() != 3)
        return def;
    V3 v;
    for (size_t i = 0; i < 3; ++i) {
        if (!j[key][i].is_number())
            return def;
        v[i] = j[key][i].get<double>();
    }
    return v_finite(v) ? v : def;
}

} // namespace

std::string video_to_json(const VideoSettings &v)
{
    json j;
    j["path"]       = to_string(v.path);
    j["duration"]   = v.duration;
    j["fps"]        = v.fps;
    j["resolution"] = v.resolution;
    j["quality"]    = to_string(v.quality);
    j["bitrate"]    = v.bitrate;
    j["format"]     = to_string(v.format);
    j["easing"]     = to_string(v.easing);
    j["sweep"]      = v.sweep_deg;
    j["clockwise"]  = v.clockwise;
    j["bob"]        = v.bob_deg;
    j["bob_cycles"] = v.bob_cycles;
    j["hold_start"] = v.hold_start;
    j["hold_end"]   = v.hold_end;
    j["loop"]       = v.loop;
    j["callouts"]   = v.callouts;
    json keys       = json::array();
    for (const Keyframe &k : v.keyframes) {
        json kj = { { "time", k.time }, { "eye", k.eye }, { "target", k.target }, { "up", k.up }, { "zoom", k.zoom }, { "ease", to_string(k.ease) } };
        // S5 keys only where used, so an S4 scene writes back exactly as it was.
        if (k.spin != 0.)
            kj["spin"] = k.spin;
        if (k.glide)
            kj["glide"] = true;
        keys.push_back(std::move(kj));
    }
    j["keyframes"] = keys;
    if (v.build != BuildUp())
        j["build"] = { { "enabled", v.build.enabled }, { "from", v.build.from }, { "to", v.build.to },
                       { "easing", to_string(v.build.easing) }, { "source", to_string(v.build.source) } };
    if (!v.events.empty()) {
        const json ev = json::parse(v.events, nullptr, false);
        if (ev.is_array())
            j["events"] = ev;
    }
    if (!v.path_name.empty())
        j["path_name"] = v.path_name;
    return j.dump();
}

bool video_from_json(const std::string &text, VideoSettings &out)
{
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object())
        return false;
    VideoSettings v;
    v.path       = venum(j, "path", v.path);
    v.duration   = vget<double>(j, "duration", v.duration);
    v.fps        = vget<int>(j, "fps", v.fps);
    v.resolution = vget<int>(j, "resolution", v.resolution);
    v.quality    = venum(j, "quality", v.quality);
    v.bitrate    = vget<int>(j, "bitrate", v.bitrate);
    v.format     = venum(j, "format", v.format);
    v.easing     = venum(j, "easing", v.easing);
    v.sweep_deg  = vget<double>(j, "sweep", v.sweep_deg);
    v.clockwise  = vget<bool>(j, "clockwise", v.clockwise);
    v.bob_deg    = vget<double>(j, "bob", v.bob_deg);
    v.bob_cycles = vget<int>(j, "bob_cycles", v.bob_cycles);
    v.hold_start = vget<double>(j, "hold_start", v.hold_start);
    v.hold_end   = vget<double>(j, "hold_end", v.hold_end);
    v.loop       = vget<bool>(j, "loop", v.loop);
    v.callouts   = vget<bool>(j, "callouts", v.callouts);
    if (j.contains("keyframes") && j["keyframes"].is_array())
        for (const json &kj : j["keyframes"]) {
            if (!kj.is_object() || !kj.contains("eye") || !kj.contains("target"))
                continue;
            Keyframe k;
            k.time   = vget<double>(kj, "time", 0.);
            k.eye    = vvec(kj, "eye", k.eye);
            k.target = vvec(kj, "target", k.target);
            k.up     = vvec(kj, "up", k.up);
            k.zoom   = vget<double>(kj, "zoom", 1.);
            k.ease   = venum(kj, "ease", k.ease);
            k.spin   = vget<double>(kj, "spin", 0.);
            k.glide  = vget<bool>(kj, "glide", false);
            v.keyframes.push_back(k);
        }
    if (j.contains("build") && j["build"].is_object()) {
        const json &b    = j["build"];
        v.build.enabled  = vget<bool>(b, "enabled", false);
        v.build.from     = vget<double>(b, "from", 0.);
        v.build.to       = vget<double>(b, "to", 1.);
        v.build.easing   = venum(b, "easing", v.build.easing);
        v.build.source   = venum(b, "source", v.build.source);
    }
    if (j.contains("events") && j["events"].is_array() && !j["events"].empty())
        v.events = j["events"].dump();
    v.path_name = vget<std::string>(j, "path_name", std::string());
    sanitize(v);
    out = std::move(v);
    return true;
}

} // namespace Showcase
} // namespace Slic3r
