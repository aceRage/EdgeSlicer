#include "ShowcasePath.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <numeric>
#include <set>
#include <sstream>

namespace Slic3r {
namespace Showcase {

using json = nlohmann::json;

namespace {

constexpr double PATH_PI = 3.14159265358979323846;

// ------------------------------------------------------------------------------------------------
// Small vector maths (named apart from ShowcaseVideo.cpp's for unity builds)
// ------------------------------------------------------------------------------------------------

PathVec pv_add(const PathVec &a, const PathVec &b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
PathVec pv_sub(const PathVec &a, const PathVec &b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
PathVec pv_mul(const PathVec &a, double s) { return { a[0] * s, a[1] * s, a[2] * s }; }
double  pv_dot(const PathVec &a, const PathVec &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double  pv_len(const PathVec &a) { return std::sqrt(pv_dot(a, a)); }
PathVec pv_cross(const PathVec &a, const PathVec &b)
{
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}
PathVec pv_norm(const PathVec &a, const PathVec &fallback = { 0., 0., 1. })
{
    const double l = pv_len(a);
    return l > 1e-12 ? pv_mul(a, 1. / l) : fallback;
}
bool pv_finite(const PathVec &a) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); }
// Turns `v` about +Z by `deg`.
PathVec pv_rotz(const PathVec &v, double deg)
{
    const double r = deg * PATH_PI / 180., c = std::cos(r), s = std::sin(r);
    return { v[0] * c - v[1] * s, v[0] * s + v[1] * c, v[2] };
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower((unsigned char) c));
    return s;
}

// ------------------------------------------------------------------------------------------------
// Names
// ------------------------------------------------------------------------------------------------

const char *const MOVE_NAMES[]   = { "orbit", "turntable", "dolly_in", "dolly_out", "pan", "zoom", "focus_object",
                                     "focus_filament", "fly_to", "move_to", "wait" };
const char *const EVENT_NAMES[]  = { "lighting", "background", "floor", "look", "callouts", "watermark", "build" };
const char *const ACTION_NAMES[] = { "show", "hide", "highlight" };
const char *const RIG_NAMES[]    = { "soft", "classic", "dramatic", "key_only", "sun" };
const char *const VIEW_NAMES[]   = { "front", "back", "left", "right", "top", "bottom", "iso" };

template<class E, size_t N> const char *path_enum_name(E v, const char *const (&names)[N])
{
    const int i = int(v);
    return i >= 0 && size_t(i) < N ? names[i] : names[0];
}
template<class E, size_t N> bool path_enum_parse(const std::string &s, E &v, const char *const (&names)[N])
{
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) {
            v = E(int(i));
            return true;
        }
    return false;
}
template<class E, size_t N> std::string path_enum_list(const char *const (&names)[N])
{
    std::string out;
    for (size_t i = 0; i < N; ++i)
        out += (i ? ", " : "") + std::string(names[i]);
    return out;
}

} // namespace

const char *to_string(PathMoveOp v) { return path_enum_name(v, MOVE_NAMES); }
bool        from_string(const std::string &s, PathMoveOp &v) { return path_enum_parse(s, v, MOVE_NAMES); }
const char *to_string(PathEventType v) { return path_enum_name(v, EVENT_NAMES); }
bool        from_string(const std::string &s, PathEventType &v) { return path_enum_parse(s, v, EVENT_NAMES); }
const char *to_string(CalloutAction v) { return path_enum_name(v, ACTION_NAMES); }
bool        from_string(const std::string &s, CalloutAction &v) { return path_enum_parse(s, v, ACTION_NAMES); }
const char *to_string(LightRig v) { return path_enum_name(v, RIG_NAMES); }
bool        from_string(const std::string &s, LightRig &v) { return path_enum_parse(s, v, RIG_NAMES); }

const std::vector<std::string> &builtin_view_names()
{
    static const std::vector<std::string> names(std::begin(VIEW_NAMES), std::end(VIEW_NAMES));
    return names;
}

// ------------------------------------------------------------------------------------------------
// Issues
// ------------------------------------------------------------------------------------------------

bool PathIssues::has_errors() const { return errors() > 0; }
int  PathIssues::errors() const { return int(std::count_if(m_items.begin(), m_items.end(), [](const PathIssue &i) { return i.error; })); }
int  PathIssues::warnings() const { return int(m_items.size()) - errors(); }

std::string PathIssues::text() const
{
    std::string out;
    for (const PathIssue &i : m_items) {
        out += i.error ? "error: " : "warning: ";
        if (!i.where.empty())
            out += i.where + ": ";
        out += i.message + "\n";
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// Framing
// ------------------------------------------------------------------------------------------------

double path_view_height(double zoom, double frame_height_px)
{
    return zoom > 0. && std::isfinite(zoom) ? std::max(1., frame_height_px) / zoom : std::max(1., frame_height_px);
}
double path_zoom(double view_height, double frame_height_px)
{
    return view_height > 1e-9 && std::isfinite(view_height) ? std::max(1., frame_height_px) / view_height : 1.;
}
double path_fov_deg(double view_height, double distance)
{
    if (!(distance > 1e-9))
        return 30.;
    return 2. * std::atan(0.5 * view_height / distance) * 180. / PATH_PI;
}
double path_view_height_for_fov(double fov_deg, double distance) { return 2. * distance * std::tan(0.5 * fov_deg * PATH_PI / 180.); }

namespace {

PathVec box_center(const PathContext::Box &b) { return pv_mul(pv_add(b.min, b.max), 0.5); }
double  box_radius(const PathContext::Box &b) { return std::max(1., 0.5 * pv_len(pv_sub(b.max, b.min))); }

PathVec builtin_dir(const std::string &name, PathVec &up)
{
    up = { 0., 0., 1. };
    if (name == "front") return { 0., -1., 0. };
    if (name == "back") return { 0., 1., 0. };
    if (name == "left") return { -1., 0., 0. };
    if (name == "right") return { 1., 0., 0. };
    if (name == "top") { up = { 0., 1., 0. }; return { 0., 0., 1. }; }
    if (name == "bottom") { up = { 0., 1., 0. }; return { 0., 0., -1. }; }
    return pv_norm({ 1., -1., 0.8 });   // iso: front-right, from above
}

} // namespace

bool builtin_view(const std::string &name_in, const PathContext::Box &box, double fov_deg, double margin, double frame_height_px,
                  Projection projection, CameraView &out)
{
    const std::string name = lower(name_in);
    if (std::find(builtin_view_names().begin(), builtin_view_names().end(), name) == builtin_view_names().end())
        return false;
    PathVec      up;
    const PathVec dir    = builtin_dir(name, up);
    const PathVec center = box_center(box);
    const double  r      = box_radius(box);
    const double  vh     = 2. * r * std::max(0.5, margin);
    double        d      = projection == Projection::Ortho ? 4. * r : vh / (2. * std::tan(0.5 * std::clamp(fov_deg, 5., 120.) * PATH_PI / 180.));
    d                    = std::max(d, 1.2 * r);
    out            = CameraView();
    out.name       = name;
    out.projection = projection;
    out.target     = center;
    out.eye        = pv_add(center, pv_mul(dir, d));
    out.up         = up;
    out.zoom       = path_zoom(vh, frame_height_px);
    out.valid      = true;
    return true;
}

// ------------------------------------------------------------------------------------------------
// Parsing
// ------------------------------------------------------------------------------------------------

namespace {

size_t edit_distance(const std::string &a, const std::string &b)
{
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    std::iota(prev.begin(), prev.end(), size_t(0));
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1) });
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// One JSON object being read, with where it is in the file for the messages.
class ObjReader
{
public:
    ObjReader(const json &j, std::string where, PathIssues &issues) : m_j(j), m_where(std::move(where)), m_issues(issues) {}

    const json        &raw() const { return m_j; }
    const std::string &where() const { return m_where; }
    std::string        at(const std::string &key) const { return m_where.empty() ? key : m_where + "." + key; }
    bool               has(const char *key) const { return m_j.is_object() && m_j.contains(key) && !m_j[key].is_null(); }
    void               error(const std::string &key, const std::string &msg) { m_issues.error(at(key), msg); m_ok = false; }
    bool               ok() const { return m_ok; }

    // Keys this object may have; others are warned about (with the nearest allowed key). Keys that
    // start with '_' or '$' are comments.
    void allow(std::initializer_list<const char *> keys)
    {
        for (const char *k : keys)
            m_allowed.insert(k);
    }
    void check_unknown()
    {
        if (!m_j.is_object())
            return;
        for (auto it = m_j.begin(); it != m_j.end(); ++it) {
            const std::string &k = it.key();
            if (k.empty() || k[0] == '_' || k[0] == '$' || m_allowed.count(k))
                continue;
            std::string best;
            size_t      best_d = 3;
            for (const std::string &a : m_allowed) {
                const size_t d = edit_distance(lower(k), a);
                if (d < best_d) {
                    best_d = d;
                    best   = a;
                }
            }
            m_issues.warn(at(k), "unknown key, ignored" + (best.empty() ? std::string() : " (did you mean \"" + best + "\"?)"));
        }
    }

    bool number(const char *key, double &out, bool required, double lo, double hi)
    {
        if (!has(key)) {
            if (required)
                error(key, "missing (a number is required)");
            return false;
        }
        const json &v = m_j[key];
        if (!v.is_number()) {
            error(key, "expected a number");
            return false;
        }
        const double d = v.get<double>();
        if (!std::isfinite(d) || d < lo || d > hi) {
            std::ostringstream ss;
            ss << "out of range (" << d << "; allowed " << lo << " to " << hi << ")";
            error(key, ss.str());
            return false;
        }
        out = d;
        return true;
    }
    bool number(const char *key, std::optional<double> &out, double lo, double hi)
    {
        double d = 0.;
        if (!number(key, d, false, lo, hi))
            return false;
        out = d;
        return true;
    }
    bool integer(const char *key, int &out, bool required, int lo, int hi)
    {
        if (!has(key)) {
            if (required)
                error(key, "missing (a whole number is required)");
            return false;
        }
        const json &v = m_j[key];
        if (!v.is_number_integer() && !(v.is_number_float() && std::floor(v.get<double>()) == v.get<double>())) {
            error(key, "expected a whole number");
            return false;
        }
        const double d = v.get<double>();
        if (d < lo || d > hi) {
            error(key, "out of range (" + std::to_string((long long) d) + "; allowed " + std::to_string(lo) + " to " + std::to_string(hi) + ")");
            return false;
        }
        out = int(d);
        return true;
    }
    bool boolean(const char *key, bool &out)
    {
        if (!has(key))
            return false;
        if (!m_j[key].is_boolean()) {
            error(key, "expected true or false");
            return false;
        }
        out = m_j[key].get<bool>();
        return true;
    }
    bool boolean(const char *key, std::optional<bool> &out)
    {
        bool b = false;
        if (!boolean(key, b))
            return false;
        out = b;
        return true;
    }
    bool string(const char *key, std::string &out, bool required = false)
    {
        if (!has(key)) {
            if (required)
                error(key, "missing (text is required)");
            return false;
        }
        if (!m_j[key].is_string()) {
            error(key, "expected text");
            return false;
        }
        out = m_j[key].get<std::string>();
        return true;
    }
    bool string(const char *key, std::optional<std::string> &out)
    {
        std::string s;
        if (!string(key, s))
            return false;
        out = s;
        return true;
    }
    bool color(const char *key, std::optional<std::string> &out)
    {
        std::string s;
        if (!string(key, s))
            return false;
        std::array<float, 4> rgba;
        if (!parse_hex_color(s, rgba)) {
            error(key, "expected a colour like \"#RRGGBB\"");
            return false;
        }
        out = normalize_hex_color(s, "#FFFFFF");
        return true;
    }
    bool vec3(const char *key, PathVec &out, bool required)
    {
        if (!has(key)) {
            if (required)
                error(key, "missing (three numbers [x, y, z] in mm are required)");
            return false;
        }
        const json &v = m_j[key];
        if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number()) {
            error(key, "expected three numbers [x, y, z]");
            return false;
        }
        PathVec p{ { v[0].get<double>(), v[1].get<double>(), v[2].get<double>() } };
        if (!pv_finite(p) || std::abs(p[0]) > 1e6 || std::abs(p[1]) > 1e6 || std::abs(p[2]) > 1e6) {
            error(key, "the numbers must be finite and within a kilometre");
            return false;
        }
        out = p;
        return true;
    }
    template<class E, size_t N> bool enumeration(const char *key, E &out, bool required, const char *const (&names)[N])
    {
        std::string s;
        if (!string(key, s, required))
            return false;
        if (!path_enum_parse(s, out, names)) {
            error(key, "unknown value \"" + s + "\" (one of: " + path_enum_list<E>(names) + ")");
            return false;
        }
        return true;
    }
    // The enums the scene names (to_string / from_string pairs).
    template<class E> bool scene_enum(const char *key, std::optional<E> &out, const std::vector<E> &values)
    {
        std::string s;
        if (!string(key, s))
            return false;
        for (const E &v : values)
            if (s == to_string(v)) {
                out = v;
                return true;
            }
        std::string list;
        for (const E &v : values)
            list += (list.empty() ? "" : ", ") + std::string(to_string(v));
        error(key, "unknown value \"" + s + "\" (one of: " + list + ")");
        return false;
    }
    bool easing(const char *key, Easing &out)
    {
        std::string s;
        if (!string(key, s))
            return false;
        if (!from_string(s, out)) {
            error(key, "unknown easing \"" + s + "\" (one of: linear, ease_in_out, ease_in, ease_out)");
            return false;
        }
        return true;
    }

private:
    const json           &m_j;
    std::string           m_where;
    PathIssues           &m_issues;
    std::set<std::string> m_allowed;
    bool                  m_ok = true;
};

constexpr const char *POSE_KEYS[] = { "eye", "target", "up", "fov", "ortho_scale", "zoom" };

void read_pose(ObjReader &r, PathPoseSpec &pose, bool required)
{
    r.vec3("eye", pose.eye, required);
    r.vec3("target", pose.target, required);
    pose.has_up = r.vec3("up", pose.up, false);
    if (pose.has_up && pv_len(pose.up) < 1e-9)
        r.error("up", "must not be [0, 0, 0]");
    int framings = 0;
    double v     = 0.;
    if (r.number("fov", v, false, 1., 150.)) {
        pose.framing       = PathFraming::Fov;
        pose.framing_value = v;
        ++framings;
    }
    if (r.number("ortho_scale", v, false, 0.01, 1e6)) {
        pose.framing       = PathFraming::OrthoScale;
        pose.framing_value = v;
        ++framings;
    }
    if (r.number("zoom", v, false, 1e-6, 1e6)) {
        pose.framing       = PathFraming::Zoom;
        pose.framing_value = v;
        ++framings;
    }
    if (framings > 1)
        r.error("fov", "give only one of fov, ortho_scale and zoom");
    if (r.has("eye") && r.has("target") && pv_len(pv_sub(pose.eye, pose.target)) < 1e-3)
        r.error("eye", "the eye and the target are the same point");
}

void allow_pose(ObjReader &r)
{
    for (const char *k : POSE_KEYS)
        r.allow({ k });
}

void read_keyframe(const json &kj, const std::string &where, PathKeyframe &k, PathIssues &issues)
{
    ObjReader r(kj, where, issues);
    if (!kj.is_object()) {
        issues.error(where, "expected an object");
        return;
    }
    r.allow({ "t", "time", "ease", "hold", "spin", "glide" });
    allow_pose(r);
    k.where = where;
    if (r.has("t"))
        r.number("t", k.t, true, 0., MAX_VIDEO_SECONDS);
    else if (r.has("time"))
        r.number("time", k.t, true, 0., MAX_VIDEO_SECONDS);
    else
        r.error("t", "missing (the time in seconds is required)");
    read_pose(r, k.pose, true);
    r.easing("ease", k.ease);
    r.number("hold", k.hold, false, 0., MAX_VIDEO_SECONDS);
    r.number("spin", k.spin, false, -36000., 36000.);
    r.boolean("glide", k.glide);
    r.check_unknown();
}

void read_move(const json &mj, const std::string &where, PathMove &m, PathIssues &issues)
{
    ObjReader r(mj, where, issues);
    m.where = where;
    if (!mj.is_object()) {
        issues.error(where, "expected an object");
        return;
    }
    if (!r.enumeration("op", m.op, true, MOVE_NAMES))
        return;
    r.allow({ "op", "duration", "ease" });
    // Durations by default: 1 s per 45 degrees of an orbit, 1 s for a wait, 2 s for everything else.
    double default_duration = 2.;
    switch (m.op) {
    case PathMoveOp::Orbit:
    case PathMoveOp::Turntable:
        r.allow({ "deg" });
        if (m.op == PathMoveOp::Orbit)
            r.allow({ "elevation" });
        r.number("deg", m.deg, true, -3600., 3600.);
        if (m.op == PathMoveOp::Orbit)
            r.number("elevation", m.elevation, false, -178., 178.);
        if (m.deg == 0. && m.elevation == 0. && r.ok())
            r.error("deg", "an orbit needs a turn (or, for an orbit, an elevation)");
        default_duration = std::max(0.5, std::max(std::abs(m.deg), std::abs(m.elevation)) / 45.);
        break;
    case PathMoveOp::DollyIn:
    case PathMoveOp::DollyOut:
        r.allow({ "factor", "distance" });
        if (r.has("factor") && r.has("distance"))
            r.error("factor", "give a factor or a distance, not both");
        else if (r.has("distance"))
            r.number("distance", m.distance, true, 0.001, 1e6);
        else if (r.has("factor"))
            r.number("factor", m.factor, true, 1.0001, 100.);
        else
            m.factor = 1.5;
        break;
    case PathMoveOp::Zoom:
        r.allow({ "factor" });
        r.number("factor", m.factor, true, 0.01, 100.);
        break;
    case PathMoveOp::Pan:
        r.allow({ "right", "up", "by" });
        m.has_by = r.vec3("by", m.by, false);
        r.number("right", m.right, false, -1e5, 1e5);
        r.number("up", m.up, false, -1e5, 1e5);
        if (m.has_by && (r.has("right") || r.has("up")))
            r.error("by", "give \"by\" or \"right\"/\"up\", not both");
        if (!m.has_by && !r.has("right") && !r.has("up"))
            r.error("right", "a pan needs \"right\" and/or \"up\" (mm), or \"by\" [x, y, z]");
        break;
    case PathMoveOp::FocusObject:
        r.allow({ "object", "margin" });
        if (!r.has("object"))
            r.error("object", "missing (the object's 0-based index or its name)");
        else if (mj["object"].is_number_integer())
            m.object_index = mj["object"].get<int>();
        else if (mj["object"].is_string())
            m.object_name = mj["object"].get<std::string>();
        else
            r.error("object", "expected the object's 0-based index or its name");
        r.number("margin", m.margin, false, 0.5, 20.);
        break;
    case PathMoveOp::FocusFilament:
        r.allow({ "filament", "margin" });
        r.integer("filament", m.filament, true, 1, 64);
        r.number("margin", m.margin, false, 0.5, 20.);
        break;
    case PathMoveOp::FlyTo:
        r.allow({ "view", "margin" });
        r.string("view", m.view, true);
        r.number("margin", m.margin, false, 0.5, 20.);
        break;
    case PathMoveOp::MoveTo:
        allow_pose(r);
        read_pose(r, m.pose, true);
        break;
    case PathMoveOp::Wait: default_duration = 1.; break;
    }
    m.duration = default_duration;
    r.number("duration", m.duration, false, m.op == PathMoveOp::Wait ? 0.001 : PATH_MIN_MOVE_SECONDS, MAX_VIDEO_SECONDS);
    if (m.op != PathMoveOp::Wait)
        r.easing("ease", m.ease);
    r.check_unknown();
}

std::vector<Anchor> all_anchors()
{
    return { Anchor::TopLeft, Anchor::Top, Anchor::TopRight, Anchor::Left, Anchor::Center, Anchor::Right, Anchor::BottomLeft, Anchor::Bottom,
             Anchor::BottomRight };
}

void read_event(const json &ej, const std::string &where, PathEvent &e, PathIssues &issues)
{
    ObjReader r(ej, where, issues);
    e.where = where;
    if (!ej.is_object()) {
        issues.error(where, "expected an object");
        return;
    }
    if (!r.enumeration("type", e.type, true, EVENT_NAMES))
        return;
    r.allow({ "type", "at", "from", "to", "transition", "fade" });
    const bool has_at = r.has("at"), has_range = r.has("from") || r.has("to");
    if (has_at && has_range)
        r.error("at", "give \"at\" or \"from\"/\"to\", not both");
    else if (has_range) {
        e.range = true;
        r.number("from", e.from, true, 0., MAX_VIDEO_SECONDS);
        r.number("to", e.to, true, 0., MAX_VIDEO_SECONDS);
        if (r.ok() && !(e.to > e.from))
            r.error("to", "must be later than \"from\"");
        if (r.has("transition"))
            r.error("transition", "a range fades with \"fade\"");
        r.number("fade", e.transition, false, 0., MAX_VIDEO_SECONDS);
        if (r.ok() && 2. * e.transition > e.to - e.from + 1e-9)
            r.error("fade", "longer than half the range");
    } else if (has_at) {
        r.number("at", e.at, true, 0., MAX_VIDEO_SECONDS);
        if (r.has("fade"))
            r.error("fade", "\"at\" blends in with \"transition\"");
        r.number("transition", e.transition, false, 0., MAX_VIDEO_SECONDS);
    } else
        r.error("at", "missing (\"at\" a time, or \"from\" and \"to\")");

    switch (e.type) {
    case PathEventType::Lighting:
        r.allow({ "rig", "azimuth", "elevation", "key_intensity", "temperature", "fill", "rim", "environment", "env_intensity",
                  "env_rotation", "exposure", "shadows", "shadow_intensity", "shadow_softness", "ssao" });
        if (r.has("rig")) {
            LightRig rig = LightRig::SoftThreePoint;
            if (r.enumeration("rig", rig, true, RIG_NAMES))
                e.rig = rig;
        }
        r.number("azimuth", e.azimuth, -3600., 3600.);
        r.number("elevation", e.elevation, -90., 90.);
        r.number("key_intensity", e.key_intensity, 0., 3.);
        r.number("temperature", e.temperature, 2000., 10000.);
        r.number("fill", e.fill, 0., 3.);
        r.number("rim", e.rim, 0., 3.);
        r.string("environment", e.environment);
        r.number("env_intensity", e.env_intensity, 0., 3.);
        r.number("env_rotation", e.env_rotation, -3600., 3600.);
        r.number("exposure", e.exposure, -3., 3.);
        r.boolean("shadows", e.shadows);
        r.number("shadow_intensity", e.shadow_intensity, 0., 1.);
        r.number("shadow_softness", e.shadow_softness, 0.5, 4.);
        r.boolean("ssao", e.ssao);
        break;
    case PathEventType::Background:
        r.allow({ "kind", "color1", "color2" });
        r.scene_enum("kind", e.background,
                     std::vector<BackgroundType>{ BackgroundType::Solid, BackgroundType::VerticalGradient, BackgroundType::RadialGradient,
                                                  BackgroundType::Image, BackgroundType::Transparent, BackgroundType::Environment });
        r.color("color1", e.color1);
        r.color("color2", e.color2);
        break;
    case PathEventType::Floor:
        r.allow({ "kind", "color", "shadow_strength", "reflection", "contact_shadow", "rotation", "flip" });
        r.scene_enum("kind", e.floor,
                     std::vector<FloorType>{ FloorType::None, FloorType::ShadowCatcher, FloorType::Solid, FloorType::Grid, FloorType::Texture,
                                             FloorType::Image });
        r.color("color", e.color);
        r.number("shadow_strength", e.shadow_strength, 0., 1.);
        r.number("reflection", e.reflection, 0., 1.);
        r.number("contact_shadow", e.contact_shadow, 0., 1.);
        r.number("rotation", e.floor_rotation, -3600., 3600.);
        r.boolean("flip", e.floor_flip);
        break;
    case PathEventType::Look:
        r.allow({ "preset" });
        if (!r.has("preset"))
            r.error("preset", "missing (a built-in look)");
        r.scene_enum("preset", e.look, builtin_presets());
        break;
    case PathEventType::Callouts: {
        r.allow({ "action", "filaments", "labels" });
        r.enumeration("action", e.action, true, ACTION_NAMES);
        if (r.has("filaments")) {
            const json &f = ej["filaments"];
            if (f.is_number_integer())
                e.filaments.push_back(f.get<int>());
            else if (f.is_array()) {
                for (const json &x : f)
                    if (x.is_number_integer() && x.get<int>() >= 1)
                        e.filaments.push_back(x.get<int>());
                    else
                        r.error("filaments", "expected filament numbers (1-based)");
            } else
                r.error("filaments", "expected a filament number or a list of them");
        }
        if (r.has("labels")) {
            const json &l = ej["labels"];
            if (l.is_string())
                e.labels.push_back(l.get<std::string>());
            else if (l.is_array()) {
                for (const json &x : l)
                    if (x.is_string())
                        e.labels.push_back(x.get<std::string>());
                    else
                        r.error("labels", "expected callout keys or titles (text)");
            } else
                r.error("labels", "expected a callout key or title, or a list of them");
        }
        break;
    }
    case PathEventType::Watermark:
        r.allow({ "enabled", "text", "anchor", "opacity", "size" });
        r.boolean("enabled", e.enabled);
        r.string("text", e.text);
        r.scene_enum("anchor", e.anchor, all_anchors());
        r.number("opacity", e.opacity, 0., 1.);
        r.number("size", e.size, 1., 30.);
        if (!e.enabled && e.range)
            e.enabled = true;   // a range shows the mark within it
        break;
    case PathEventType::Build:
        r.allow({ "start", "end", "ease", "source", "infill", "supports", "follow" });
        if (!e.range)
            r.error("at", "a build-up needs \"from\" and \"to\"");
        r.number("start", e.build_start, false, 0., 1.);
        r.number("end", e.build_end, false, 0., 1.);
        r.easing("ease", e.build_easing);
        {
            std::string s;
            if (r.string("source", s) && !from_string(s, e.build_source))
                r.error("source", "unknown value \"" + s + "\" (one of: auto, toolpaths, clip)");
        }
        r.boolean("infill", e.build_infill);
        r.boolean("supports", e.build_supports);
        r.boolean("follow", e.build_follow);
        break;
    }
    r.check_unknown();
}

} // namespace

bool parse_path_events(const std::string &json_array, std::vector<PathEvent> &out, PathIssues &issues)
{
    out.clear();
    const json j = json::parse(json_array, nullptr, false);
    if (j.is_discarded() || !j.is_array()) {
        issues.error("events", "expected a list of events");
        return false;
    }
    const int before = issues.errors();
    for (size_t i = 0; i < j.size(); ++i) {
        PathEvent e;
        read_event(j[i], "events[" + std::to_string(i) + "]", e, issues);
        out.push_back(std::move(e));
    }
    if (issues.errors() > before) {
        out.clear();
        return false;
    }
    return true;
}

bool parse_path(const std::string &text, PathDocument &out, PathIssues &issues)
{
    out = PathDocument();
    json j;
    try {
        j = json::parse(text);
    } catch (const json::parse_error &ex) {
        // The parser says where: "[json.exception.parse_error.101] parse error at line 3, column 5: ..."
        std::string msg = ex.what();
        const size_t p  = msg.find("] ");
        issues.error("", "not valid JSON: " + (p == std::string::npos ? msg : msg.substr(p + 2)));
        return false;
    }
    if (!j.is_object()) {
        issues.error("", "a path file is a JSON object");
        return false;
    }
    const int before = issues.errors();
    ObjReader r(j, "", issues);
    r.allow({ "format", "version", "name", "description", "fps", "duration", "projection", "start", "keyframes", "moves", "events" });
    std::string format;
    if (!r.string("format", format, true) || format != PATH_FORMAT_ID) {
        if (r.has("format"))
            r.error("format", std::string("expected \"") + PATH_FORMAT_ID + "\"");
    }
    if (r.integer("version", out.version, true, 1, 1000) && out.version > PATH_FORMAT_VERSION)
        r.error("version", "this path needs a newer EdgeSlicer (it reads version " + std::to_string(PATH_FORMAT_VERSION) + ")");
    r.string("name", out.name);
    r.string("description", out.description);
    r.integer("fps", out.fps, false, 10, 120);
    r.number("duration", out.duration, false, MIN_VIDEO_SECONDS, MAX_VIDEO_SECONDS);
    {
        std::optional<Projection> pr;
        r.scene_enum("projection", pr, std::vector<Projection>{ Projection::Perspective, Projection::Ortho });
        out.projection = pr;
    }
    if (r.has("start")) {
        const json &s = j["start"];
        if (s.is_string()) {
            const std::string v = s.get<std::string>();
            if (v == "scene")
                out.start = PathStart::Scene;
            else if (v == "default")
                out.start = PathStart::Default;
            else {
                out.start      = PathStart::View;
                out.start_view = v;
            }
        } else if (s.is_object()) {
            out.start = PathStart::Pose;
            ObjReader sr(s, "start", issues);
            allow_pose(sr);
            read_pose(sr, out.start_pose, true);
            sr.check_unknown();
        } else
            r.error("start", "expected \"scene\", \"default\", a view name or a camera {eye, target, ...}");
    }
    for (const char *list : { "keyframes", "moves", "events" }) {
        if (!r.has(list))
            continue;
        if (!j[list].is_array()) {
            r.error(list, "expected a list");
            continue;
        }
        const json &arr = j[list];
        for (size_t i = 0; i < arr.size(); ++i) {
            const std::string where = std::string(list) + "[" + std::to_string(i) + "]";
            if (std::string(list) == "keyframes") {
                PathKeyframe k;
                read_keyframe(arr[i], where, k, issues);
                out.keyframes.push_back(std::move(k));
            } else if (std::string(list) == "moves") {
                PathMove m;
                read_move(arr[i], where, m, issues);
                out.moves.push_back(std::move(m));
            } else {
                PathEvent e;
                read_event(arr[i], where, e, issues);
                out.events.push_back(std::move(e));
            }
        }
    }
    if (int(out.keyframes.size()) > MAX_KEYFRAMES)
        r.error("keyframes", "at most " + std::to_string(MAX_KEYFRAMES) + " keyframes");
    r.check_unknown();
    return issues.errors() == before;
}

// ------------------------------------------------------------------------------------------------
// Writing
// ------------------------------------------------------------------------------------------------

namespace {

double round_to(double v, double step) { return std::round(v / step) * step; }
template<class J = json> J vec_json(const PathVec &v) { return J::array({ round_to(v[0], 1e-4), round_to(v[1], 1e-4), round_to(v[2], 1e-4) }); }

template<class J> void write_pose(J &j, const PathPoseSpec &p)
{
    j["eye"]    = vec_json<J>(p.eye);
    j["target"] = vec_json<J>(p.target);
    if (p.has_up)
        j["up"] = vec_json<J>(p.up);
    switch (p.framing) {
    case PathFraming::Fov: j["fov"] = round_to(p.framing_value, 1e-4); break;
    case PathFraming::OrthoScale: j["ortho_scale"] = round_to(p.framing_value, 1e-4); break;
    case PathFraming::Zoom: j["zoom"] = p.framing_value; break;
    default: break;
    }
}

json event_json(const PathEvent &e)
{
    json j;
    j["type"] = to_string(e.type);
    if (e.range) {
        j["from"] = e.from;
        j["to"]   = e.to;
        if (e.transition > 0.)
            j["fade"] = e.transition;
    } else {
        j["at"] = e.at;
        if (e.transition > 0.)
            j["transition"] = e.transition;
    }
    auto opt = [&j](const char *k, const auto &v) {
        if (v)
            j[k] = *v;
    };
    switch (e.type) {
    case PathEventType::Lighting:
        if (e.rig)
            j["rig"] = to_string(*e.rig);
        opt("azimuth", e.azimuth);
        opt("elevation", e.elevation);
        opt("key_intensity", e.key_intensity);
        opt("temperature", e.temperature);
        opt("fill", e.fill);
        opt("rim", e.rim);
        opt("environment", e.environment);
        opt("env_intensity", e.env_intensity);
        opt("env_rotation", e.env_rotation);
        opt("exposure", e.exposure);
        opt("shadows", e.shadows);
        opt("shadow_intensity", e.shadow_intensity);
        opt("shadow_softness", e.shadow_softness);
        opt("ssao", e.ssao);
        break;
    case PathEventType::Background:
        if (e.background)
            j["kind"] = to_string(*e.background);
        opt("color1", e.color1);
        opt("color2", e.color2);
        break;
    case PathEventType::Floor:
        if (e.floor)
            j["kind"] = to_string(*e.floor);
        opt("color", e.color);
        opt("shadow_strength", e.shadow_strength);
        opt("reflection", e.reflection);
        opt("contact_shadow", e.contact_shadow);
        opt("rotation", e.floor_rotation);
        opt("flip", e.floor_flip);
        break;
    case PathEventType::Look:
        if (e.look)
            j["preset"] = to_string(*e.look);
        break;
    case PathEventType::Callouts:
        j["action"] = to_string(e.action);
        if (!e.filaments.empty())
            j["filaments"] = e.filaments;
        if (!e.labels.empty())
            j["labels"] = e.labels;
        break;
    case PathEventType::Watermark:
        opt("enabled", e.enabled);
        opt("text", e.text);
        if (e.anchor)
            j["anchor"] = to_string(*e.anchor);
        opt("opacity", e.opacity);
        opt("size", e.size);
        break;
    case PathEventType::Build:
        j["start"] = e.build_start;
        j["end"]   = e.build_end;
        j["ease"]  = to_string(e.build_easing);
        if (e.build_source != BuildSource::Auto)
            j["source"] = to_string(e.build_source);
        opt("infill", e.build_infill);
        opt("supports", e.build_supports);
        if (e.build_follow)
            j["follow"] = true;
        break;
    }
    return j;
}

} // namespace

std::string path_events_to_json(const std::vector<PathEvent> &events)
{
    json arr = json::array();
    for (const PathEvent &e : events)
        arr.push_back(event_json(e));
    return arr.dump();
}

std::string path_to_json(const PathDocument &doc)
{
    // Keys in a fixed, readable order (nlohmann's ordered_json keeps insertion order).
    using oj = nlohmann::ordered_json;
    oj j;
    j["format"]  = PATH_FORMAT_ID;
    j["version"] = PATH_FORMAT_VERSION;
    if (!doc.name.empty())
        j["name"] = doc.name;
    if (!doc.description.empty())
        j["description"] = doc.description;
    if (doc.fps > 0)
        j["fps"] = doc.fps;
    if (doc.duration > 0.)
        j["duration"] = doc.duration;
    if (doc.projection)
        j["projection"] = to_string(*doc.projection);
    switch (doc.start) {
    case PathStart::Default: j["start"] = "default"; break;
    case PathStart::View: j["start"] = doc.start_view; break;
    case PathStart::Pose: {
        oj p;
        write_pose(p, doc.start_pose);
        j["start"] = p;
        break;
    }
    default: break;
    }
    if (!doc.keyframes.empty()) {
        oj arr = oj::array();
        for (const PathKeyframe &k : doc.keyframes) {
            oj kj;
            kj["t"] = round_to(k.t, 1e-6);
            write_pose(kj, k.pose);
            kj["ease"] = to_string(k.ease);
            if (k.hold > 0.)
                kj["hold"] = k.hold;
            if (k.spin != 0.)
                kj["spin"] = round_to(k.spin, 1e-6);
            if (k.glide)
                kj["glide"] = true;
            arr.push_back(kj);
        }
        j["keyframes"] = arr;
    }
    if (!doc.moves.empty()) {
        oj arr = oj::array();
        for (const PathMove &m : doc.moves) {
            oj mj;
            mj["op"]       = to_string(m.op);
            mj["duration"] = round_to(m.duration, 1e-6);
            if (m.op != PathMoveOp::Wait)
                mj["ease"] = to_string(m.ease);
            switch (m.op) {
            case PathMoveOp::Orbit:
                mj["deg"] = m.deg;
                if (m.elevation != 0.)
                    mj["elevation"] = m.elevation;
                break;
            case PathMoveOp::Turntable: mj["deg"] = m.deg; break;
            case PathMoveOp::DollyIn:
            case PathMoveOp::DollyOut:
                if (m.distance > 0.)
                    mj["distance"] = m.distance;
                else
                    mj["factor"] = m.factor;
                break;
            case PathMoveOp::Zoom: mj["factor"] = m.factor; break;
            case PathMoveOp::Pan:
                if (m.has_by)
                    mj["by"] = vec_json<oj>(m.by);
                else {
                    mj["right"] = m.right;
                    mj["up"]    = m.up;
                }
                break;
            case PathMoveOp::FocusObject:
                if (!m.object_name.empty())
                    mj["object"] = m.object_name;
                else
                    mj["object"] = m.object_index;
                mj["margin"] = m.margin;
                break;
            case PathMoveOp::FocusFilament:
                mj["filament"] = m.filament;
                mj["margin"]   = m.margin;
                break;
            case PathMoveOp::FlyTo:
                mj["view"]   = m.view;
                mj["margin"] = m.margin;
                break;
            case PathMoveOp::MoveTo: write_pose(mj, m.pose); break;
            default: break;
            }
            arr.push_back(mj);
        }
        j["moves"] = arr;
    }
    if (!doc.events.empty())
        j["events"] = oj::parse(path_events_to_json(doc.events));
    return j.dump(2);
}

// ------------------------------------------------------------------------------------------------
// Compiling
// ------------------------------------------------------------------------------------------------

namespace {

// The camera while the moves are worked through: a pose and the height the frame shows at the target.
struct CamState
{
    PathVec eye{ { 0., -200., 150. } };
    PathVec target{ { 0., 0., 0. } };
    PathVec up{ { 0., 0., 1. } };
    double  view_height = 100.;
    double  spin        = 0.;

    double distance() const { return pv_len(pv_sub(eye, target)); }
};

CamState state_of(const CameraView &v, double frame_h)
{
    CamState s;
    s.eye         = v.eye;
    s.target      = v.target;
    s.up          = pv_norm(v.up);
    s.view_height = path_view_height(v.zoom, frame_h);
    return s;
}

Keyframe key_of(const CamState &s, double t, Easing ease, bool glide, double frame_h)
{
    Keyframe k;
    k.time   = t;
    k.eye    = s.eye;
    k.target = s.target;
    k.up     = pv_norm(s.up);
    k.zoom   = path_zoom(s.view_height, frame_h);
    k.ease   = ease;
    k.spin   = s.spin;
    k.glide  = glide;
    return k;
}

class Compiler
{
public:
    Compiler(const PathDocument &doc, const PathContext &ctx, PathIssues &issues)
        : m_doc(doc), m_ctx(ctx), m_issues(issues), m_projection(doc.projection ? *doc.projection : ctx.projection)
    {}

    bool run(CompiledPath &out)
    {
        const int before = m_issues.errors();
        m_frame_h        = std::max(1., m_ctx.frame_height_px);
        if (!start_state(m_state))
            return false;
        m_have_key = false;

        // Absolute keyframes first.
        double prev_end = -1.;
        for (const PathKeyframe &k : m_doc.keyframes) {
            if (k.t < prev_end + 1e-3 && prev_end >= 0.) {
                std::ostringstream ss;
                ss << "keyframes must be in time order: " << k.t << " s is not after " << prev_end << " s (the key before, with its hold)";
                m_issues.error(k.where + ".t", ss.str());
                continue;
            }
            CamState s;
            if (!resolve_pose(k.pose, m_state, k.where, s))
                continue;
            s.spin = k.spin;
            push(key_of(s, k.t, k.ease, k.glide, m_frame_h));
            m_state = s;
            m_t     = k.t;
            if (k.hold > 0.) {
                push(key_of(s, k.t + k.hold, Easing::Linear, false, m_frame_h));
                m_t = k.t + k.hold;
            }
            prev_end = m_t;
        }
        // Then the moves, from where the keyframes left the camera (or the start, at 0 s).
        for (const PathMove &m : m_doc.moves) {
            if (!m_have_key)
                push(key_of(m_state, 0., Easing::Linear, false, m_frame_h));
            move(m);
        }
        if (!m_have_key)
            push(key_of(m_state, 0., Easing::Linear, false, m_frame_h));

        if (m_issues.errors() > before)
            return false;

        // The glide keys' times: where their run passes them, for the panel's key list.
        for (size_t i = 0; i < m_keys.size(); ++i)
            if (m_keys[i].glide)
                m_keys[i].time = keyframe_run_time(m_keys, i);

        const double camera_end = m_keys.back().time;
        const double events_end = timeline_end(m_doc.events);
        double       duration   = std::max(camera_end, events_end);
        if (m_doc.duration > 0.) {
            if (m_doc.duration + 1e-9 < duration) {
                std::ostringstream ss;
                ss << "the path runs " << duration << " s; it is cut at " << m_doc.duration << " s";
                m_issues.warn("duration", ss.str());
            }
            duration = m_doc.duration;
        }
        if (duration > MAX_VIDEO_SECONDS + 1e-9) {
            std::ostringstream ss;
            ss << "the path runs " << duration << " s; a video is at most " << MAX_VIDEO_SECONDS << " s";
            m_issues.error("", ss.str());
            return false;
        }
        if (duration < MIN_VIDEO_SECONDS) {
            if (duration <= 0.)
                m_issues.warn("", "the path does nothing: a still second");
            duration = MIN_VIDEO_SECONDS;
        }
        if (int(m_keys.size()) > MAX_KEYFRAMES) {
            m_issues.error("moves", "the path compiles to " + std::to_string(m_keys.size()) + " keyframes; at most " +
                                        std::to_string(MAX_KEYFRAMES));
            return false;
        }
        check_events();

        out            = CompiledPath();
        out.name       = m_doc.name;
        out.keys       = std::move(m_keys);
        out.duration   = duration;
        out.fps        = m_doc.fps;
        out.projection = m_doc.projection;
        out.events     = m_doc.events;
        return m_issues.errors() == before;
    }

private:
    bool start_state(CamState &s)
    {
        CameraView start = m_ctx.start.valid ? m_ctx.start : m_ctx.default_view;
        // A built-in view keeps the scene camera's field of view.
        m_state = state_of(start, m_frame_h);
        switch (m_doc.start) {
        case PathStart::Default: start = m_ctx.default_view; break;
        case PathStart::View:
            if (!find_view(m_doc.start_view, 1.15, "start", start))
                return false;
            break;
        default: break;
        }
        s = state_of(start, m_frame_h);
        if (m_doc.start == PathStart::Pose) {
            CamState p;
            if (!resolve_pose(m_doc.start_pose, s, "start", p))
                return false;
            s = p;
        }
        if (s.distance() < 1e-3) {
            m_issues.error("start", "the starting camera has no direction to look in");
            return false;
        }
        return true;
    }

    // A built-in view of the plate or a saved view of the scene by name.
    bool find_view(const std::string &name, double margin, const std::string &where, CameraView &out)
    {
        const std::string want = lower(name);
        for (const CameraView &v : m_ctx.views)
            if (lower(v.name) == want && v.valid) {
                out = v;
                return true;
            }
        if (std::find(builtin_view_names().begin(), builtin_view_names().end(), want) != builtin_view_names().end()) {
            if (!m_ctx.plate.valid()) {
                m_issues.error(where, "nothing on the plate to frame");
                return false;
            }
            builtin_view(want, m_ctx.plate, current_fov(m_state), margin, m_frame_h, m_projection, out);
            return true;
        }
        std::string list;
        for (const std::string &n : builtin_view_names())
            list += (list.empty() ? "" : ", ") + n;
        for (const CameraView &v : m_ctx.views)
            if (v.valid && !v.name.empty())
                list += ", \"" + v.name + "\"";
        m_issues.error(where, "no view called \"" + name + "\" (views: " + list + ")");
        return false;
    }

    double current_fov(const CamState &s) const
    {
        return m_projection == Projection::Ortho ? 30. : std::clamp(path_fov_deg(s.view_height, s.distance()), 1., 150.);
    }

    bool resolve_pose(const PathPoseSpec &spec, const CamState &prev, const std::string &where, CamState &out)
    {
        out        = prev;
        out.eye    = spec.eye;
        out.target = spec.target;
        if (spec.has_up)
            out.up = pv_norm(spec.up);
        const double d = out.distance();
        if (d < 1e-3) {
            m_issues.error(where + ".eye", "the eye and the target are the same point");
            return false;
        }
        switch (spec.framing) {
        case PathFraming::Fov:
            if (m_projection == Projection::Ortho) {
                m_issues.error(where + ".fov", "an orthographic path frames with \"ortho_scale\", not \"fov\"");
                return false;
            }
            out.view_height = path_view_height_for_fov(spec.framing_value, d);
            break;
        case PathFraming::OrthoScale:
            if (m_projection != Projection::Ortho) {
                m_issues.error(where + ".ortho_scale", "\"ortho_scale\" needs \"projection\": \"ortho\" (or a scene in orthographic view)");
                return false;
            }
            out.view_height = spec.framing_value;
            break;
        case PathFraming::Zoom: out.view_height = path_view_height(spec.framing_value, m_frame_h); break;
        default:
            // The same field of view as before (the same scale in orthographic).
            if (m_projection != Projection::Ortho)
                out.view_height = path_view_height_for_fov(current_fov(prev), d);
            break;
        }
        return true;
    }

    void push(const Keyframe &k)
    {
        m_keys.push_back(k);
        m_have_key = true;
    }

    // A key at the end of a move.
    void end_move(const PathMove &m, const CamState &s)
    {
        m_t += m.duration;
        push(key_of(s, m_t, m.op == PathMoveOp::Wait ? Easing::Linear : m.ease, false, m_frame_h));
        m_state = s;
    }

    void orbit(const PathMove &m, double deg, double elevation, double spin)
    {
        const PathVec offset = pv_sub(m_state.eye, m_state.target);
        const double  r      = pv_len(offset);
        const double  az0    = std::atan2(offset[1], offset[0]) * 180. / PATH_PI;
        const double  el0    = std::atan2(offset[2], std::hypot(offset[0], offset[1])) * 180. / PATH_PI;
        const int     n      = std::max(1, int(std::ceil(std::max(std::abs(deg), std::abs(elevation)) / PATH_ORBIT_STEP_DEG - 1e-9)));
        const double  t0     = m_t;
        CamState      s      = m_state;
        for (int i = 1; i <= n; ++i) {
            const double f  = double(i) / double(n);
            const double az = (az0 + deg * f) * PATH_PI / 180.;
            const double el = std::clamp(el0 + elevation * f, -89., 89.) * PATH_PI / 180.;
            s.eye  = pv_add(m_state.target, PathVec{ r * std::cos(el) * std::cos(az), r * std::cos(el) * std::sin(az), r * std::sin(el) });
            s.up   = pv_rotz(m_state.up, deg * f);
            s.spin = m_state.spin + spin * f;
            const bool last = i == n;
            // Provisional, increasing times; a glide key's real time is where its run passes it.
            push(key_of(s, t0 + m.duration * f, last ? m.ease : Easing::Linear, !last, m_frame_h));
        }
        m_t     = t0 + m.duration;
        m_state = s;
    }

    bool focus_box(const PathMove &m, const PathContext::Box &box)
    {
        CamState     s      = m_state;
        const double r      = box_radius(box);
        const double vh     = 2. * r * m.margin;
        PathVec      dir    = pv_norm(pv_sub(m_state.eye, m_state.target), { 0., -1., 0. });
        s.target            = box_center(box);
        double d            = m_state.distance();
        if (m_projection == Projection::Ortho)
            d = std::max(d, 3. * r);
        else {
            const double fov = current_fov(m_state);
            d                = std::max(vh / (2. * std::tan(0.5 * fov * PATH_PI / 180.)), 1.2 * r);
        }
        s.eye         = pv_add(s.target, pv_mul(dir, d));
        s.view_height = m_projection == Projection::Ortho ? vh : path_view_height_for_fov(current_fov(m_state), d);
        end_move(m, s);
        return true;
    }

    void move(const PathMove &m)
    {
        CamState s = m_state;
        switch (m.op) {
        case PathMoveOp::Orbit: orbit(m, m.deg, m.elevation, 0.); return;
        case PathMoveOp::Turntable:
            // The model turns by deg: the camera goes the other way round (and the renderer turns the
            // lights and the environment with it), which looks the same.
            orbit(m, -m.deg, 0., m.deg);
            return;
        case PathMoveOp::DollyIn:
        case PathMoveOp::DollyOut: {
            const double d   = m_state.distance();
            const bool   in  = m.op == PathMoveOp::DollyIn;
            double       nd  = m.distance > 0. ? (in ? d - m.distance : d + m.distance) : (in ? d / m.factor : d * m.factor);
            if (!(nd > 1e-3)) {
                std::ostringstream ss;
                ss << "would pass the target (the camera is " << d << " mm from it)";
                m_issues.error(m.where + ".distance", ss.str());
                return;
            }
            const PathVec dir = pv_norm(pv_sub(m_state.eye, m_state.target));
            s.eye             = pv_add(m_state.target, pv_mul(dir, nd));
            // Perspective: the field of view stays, so the frame shows less the closer it gets.
            // Orthographic: moving the eye changes nothing, so the scale does it.
            s.view_height = m_state.view_height * nd / d;
            end_move(m, s);
            return;
        }
        case PathMoveOp::Zoom:
            s.view_height = m_state.view_height / m.factor;
            end_move(m, s);
            return;
        case PathMoveOp::Pan: {
            PathVec off = m.by;
            if (!m.has_by) {
                const PathVec fwd   = pv_norm(pv_sub(m_state.target, m_state.eye));
                PathVec       right = pv_cross(fwd, pv_norm(m_state.up));
                if (pv_len(right) < 1e-6)
                    right = pv_cross(fwd, { 0., 1., 0. });
                right            = pv_norm(right, { 1., 0., 0. });
                const PathVec up = pv_norm(pv_cross(right, fwd));
                off              = pv_add(pv_mul(right, m.right), pv_mul(up, m.up));
            }
            s.eye    = pv_add(m_state.eye, off);
            s.target = pv_add(m_state.target, off);
            end_move(m, s);
            return;
        }
        case PathMoveOp::FocusObject: {
            const PathContext::Object *found = nullptr;
            if (m.object_index >= 0) {
                for (const PathContext::Object &o : m_ctx.objects)
                    if (o.index == m.object_index)
                        found = &o;
            } else {
                int count = 0;
                for (const PathContext::Object &o : m_ctx.objects)
                    if (lower(o.name) == lower(m.object_name)) {
                        if (found == nullptr)
                            found = &o;
                        ++count;
                    }
                if (count > 1)
                    m_issues.warn(m.where + ".object", std::to_string(count) + " objects are called \"" + m.object_name + "\"; the first is used");
            }
            if (found == nullptr || !found->box.valid()) {
                std::string list;
                for (const PathContext::Object &o : m_ctx.objects)
                    list += (list.empty() ? "" : ", ") + std::to_string(o.index) + " \"" + o.name + "\"";
                m_issues.error(m.where + ".object", "no such object on this plate (objects: " + (list.empty() ? std::string("none") : list) + ")");
                return;
            }
            focus_box(m, found->box);
            return;
        }
        case PathMoveOp::FocusFilament: {
            for (const PathContext::Filament &f : m_ctx.filaments)
                if (f.filament == m.filament && f.box.valid()) {
                    focus_box(m, f.box);
                    return;
                }
            std::string list;
            for (const PathContext::Filament &f : m_ctx.filaments)
                list += (list.empty() ? "" : ", ") + std::to_string(f.filament);
            m_issues.error(m.where + ".filament", "filament " + std::to_string(m.filament) + " is not on this plate (filaments here: " +
                                                      (list.empty() ? std::string("none") : list) + ")");
            return;
        }
        case PathMoveOp::FlyTo: {
            CameraView v;
            if (!find_view(m.view, m.margin, m.where + ".view", v))
                return;
            s.eye         = v.eye;
            s.target      = v.target;
            s.up          = pv_norm(v.up);
            s.view_height = path_view_height(v.zoom, m_frame_h);
            end_move(m, s);
            return;
        }
        case PathMoveOp::MoveTo:
            if (!resolve_pose(m.pose, m_state, m.where, s))
                return;
            end_move(m, s);
            return;
        case PathMoveOp::Wait: end_move(m, s); return;
        }
    }

    // Warnings about events the scene cannot show as asked.
    void check_events()
    {
        for (const PathEvent &e : m_doc.events) {
            if (e.type == PathEventType::Callouts) {
                for (int f : e.filaments) {
                    bool any = false;
                    for (const PathContext::CalloutRef &c : m_ctx.callouts)
                        any |= c.filament == f;
                    bool on_plate = false;
                    for (const PathContext::Filament &pf : m_ctx.filaments)
                        on_plate |= pf.filament == f;
                    if (!any && !on_plate)
                        m_issues.warn(e.where + ".filaments", "filament " + std::to_string(f) + " is not on this plate: no callout to show");
                }
                for (const std::string &l : e.labels) {
                    bool any = false;
                    for (const PathContext::CalloutRef &c : m_ctx.callouts)
                        any |= c.key == l || (!c.title.empty() && lower(c.title) == lower(l));
                    if (!any)
                        m_issues.warn(e.where + ".labels", "no callout \"" + l + "\" in the scene yet (keys are like \"f2\")");
                }
            }
            if (e.type == PathEventType::Watermark && e.enabled && *e.enabled && !e.text && !m_ctx.has_watermark_content)
                m_issues.warn(e.where, "the scene's watermark has no text or logo: give \"text\"");
        }
    }

    const PathDocument   &m_doc;
    const PathContext    &m_ctx;
    PathIssues           &m_issues;
    Projection            m_projection;
    double                m_frame_h = 1000.;
    CamState              m_state;
    double                m_t = 0.;
    bool                  m_have_key = false;
    std::vector<Keyframe> m_keys;
};

} // namespace

bool compile_path(const PathDocument &doc, const PathContext &ctx, CompiledPath &out, PathIssues &issues)
{
    Compiler c(doc, ctx, issues);
    return c.run(out);
}

void apply_compiled_path(const CompiledPath &path, VideoSettings &video)
{
    video.path       = VideoPath::Keyframes;
    video.keyframes  = path.keys;
    video.duration   = std::clamp(path.duration, MIN_VIDEO_SECONDS, MAX_VIDEO_SECONDS);
    video.hold_start = 0.;
    video.hold_end   = 0.;
    video.loop       = false;
    if (path.fps > 0)
        video.fps = path.fps;
    video.events    = path.events.empty() ? std::string() : path_events_to_json(path.events);
    video.path_name = path.name;
    // The file's build-up events say when; the scene's own switch would be a second one.
    for (const PathEvent &e : path.events)
        if (e.type == PathEventType::Build)
            video.build.enabled = false;
}

PathDocument export_path(const VideoSettings &v_in, const PathContext &ctx, const std::string &name)
{
    VideoSettings v = v_in;
    sanitize(v);
    PathDocument doc;
    doc.name       = name.empty() ? v.path_name : name;
    doc.fps        = v.fps;
    doc.projection = ctx.projection;
    const double frame_h = std::max(1., ctx.frame_height_px);
    auto pose_spec = [&](const PathVec &eye, const PathVec &target, const PathVec &up, double zoom) {
        PathPoseSpec p;
        p.eye           = eye;
        p.target        = target;
        p.has_up        = true;
        p.up            = up;
        const double vh = path_view_height(zoom, frame_h);
        if (ctx.projection == Projection::Ortho) {
            p.framing       = PathFraming::OrthoScale;
            p.framing_value = vh;
        } else {
            p.framing       = PathFraming::Fov;
            p.framing_value = path_fov_deg(vh, pv_len(pv_sub(eye, target)));
        }
        return p;
    };
    const CameraView base = ctx.start.valid ? ctx.start : ctx.default_view;
    if (v.path == VideoPath::Keyframes) {
        for (size_t i = 0; i < v.keyframes.size(); ++i) {
            const Keyframe &k = v.keyframes[i];
            PathKeyframe    pk;
            pk.t     = v.hold_start + k.time;
            pk.pose  = pose_spec(k.eye, k.target, k.up, k.zoom);
            pk.ease  = k.ease;
            pk.spin  = k.spin;
            pk.glide = k.glide;
            if (i + 1 == v.keyframes.size() && v.hold_end > 0.)
                pk.hold = v.hold_end + std::max(0., v.duration - k.time);
            doc.keyframes.push_back(pk);
        }
        if (doc.keyframes.empty())
            doc.duration = total_seconds(v);
    } else {
        doc.start      = PathStart::Pose;
        doc.start_pose = pose_spec(base.eye, base.target, base.up, base.zoom);
        if (v.hold_start > 0.) {
            PathMove w;
            w.op       = PathMoveOp::Wait;
            w.duration = v.hold_start;
            doc.moves.push_back(w);
        }
        if (is_static_path(v.path)) {
            // S5c: the camera holds; "static with zoom out" is its build event's "follow".
            PathMove w;
            w.op       = PathMoveOp::Wait;
            w.duration = v.duration + v.hold_end;
            doc.moves.push_back(w);
        } else if (v.path == VideoPath::OrbitBob) {
            // No move for a bob: its keys, a glide run eased as the orbit was.
            const int n = std::max(8 * v.bob_cycles, int(std::ceil(v.sweep_deg / PATH_ORBIT_STEP_DEG)));
            for (int i = 0; i <= n; ++i) {
                const double     tm = v.hold_start + v.duration * double(i) / double(n);
                const CameraPose p  = evaluate_path(v, base, tm);
                PathKeyframe     pk;
                pk.t     = tm;
                pk.pose  = pose_spec(p.eye, p.target, p.up, p.zoom);
                pk.ease  = i == n ? v.easing : Easing::Linear;
                pk.glide = i > 0 && i < n;
                if (i == n && v.hold_end > 0.)
                    pk.hold = v.hold_end;
                doc.keyframes.push_back(pk);
            }
            doc.moves.clear();
            doc.start = PathStart::Pose;
        } else {
            PathMove m;
            m.op       = v.path == VideoPath::Turntable ? PathMoveOp::Turntable : PathMoveOp::Orbit;
            m.deg      = v.sweep_deg * (v.clockwise ? -1. : 1.);
            m.duration = v.duration;
            m.ease     = v.easing;
            doc.moves.push_back(m);
            if (v.hold_end > 0.) {
                PathMove w;
                w.op       = PathMoveOp::Wait;
                w.duration = v.hold_end;
                doc.moves.push_back(w);
            }
        }
    }
    if (!v.events.empty()) {
        PathIssues ignored;
        parse_path_events(v.events, doc.events, ignored);
    }
    if (v.build.enabled) {
        PathEvent b;
        b.type         = PathEventType::Build;
        b.range        = true;
        b.from         = v.hold_start;
        b.to           = v.hold_start + v.duration;
        b.build_start  = v.build.from;
        b.build_end    = v.build.to;
        b.build_easing = v.build.easing;
        b.build_source = v.build.source;
        if (!v.build.infill)
            b.build_infill = false;
        if (!v.build.supports)
            b.build_supports = false;
        b.build_follow = v.path == VideoPath::StaticZoomOut;
        doc.events.push_back(b);
    }
    return doc;
}

// ------------------------------------------------------------------------------------------------
// The timeline
// ------------------------------------------------------------------------------------------------

namespace {

double lerp(double a, double b, double w) { return a + (b - a) * w; }
float  lerpf(float a, float b, double w) { return float(double(a) + (double(b) - double(a)) * w); }
// The short way round.
float lerp_angle(float a, float b, double w)
{
    double d = std::fmod(double(b) - double(a), 360.);
    if (d > 180.)
        d -= 360.;
    if (d < -180.)
        d += 360.;
    return float(double(a) + d * w);
}
std::string lerp_color(const std::string &a, const std::string &b, double w)
{
    if (w <= 0.)
        return a;
    if (w >= 1.)
        return b;
    std::array<float, 4> ca, cb;
    if (!parse_hex_color(a, ca) || !parse_hex_color(b, cb))
        return w < 0.5 ? a : b;
    std::array<float, 4> c;
    for (int i = 0; i < 4; ++i)
        c[i] = lerpf(ca[i], cb[i], w);
    return to_hex_color(c, a.size() > 7 || b.size() > 7);
}
template<class T> T pick(const T &a, const T &b, double w) { return w < 0.5 ? a : b; }

void blend(Lighting &cur, const Lighting &to, double w)
{
    if (w >= 1.) {
        cur = to;
        return;
    }
    cur.world_fixed      = pick(cur.world_fixed, to.world_fixed, w);
    cur.azimuth_deg      = lerp_angle(cur.azimuth_deg, to.azimuth_deg, w);
    cur.elevation_deg    = lerpf(cur.elevation_deg, to.elevation_deg, w);
    cur.shadows          = pick(cur.shadows, to.shadows, w);
    cur.shadow_intensity = lerpf(cur.shadow_intensity, to.shadow_intensity, w);
    cur.ssao             = pick(cur.ssao, to.ssao, w);
    cur.key_intensity    = lerpf(cur.key_intensity, to.key_intensity, w);
    cur.key_temperature  = lerpf(cur.key_temperature, to.key_temperature, w);
    // A light switched on or off fades through zero.
    const float fill_a = cur.fill ? cur.fill_intensity : 0.f, fill_b = to.fill ? to.fill_intensity : 0.f;
    const float rim_a = cur.rim ? cur.rim_intensity : 0.f, rim_b = to.rim ? to.rim_intensity : 0.f;
    cur.fill_intensity = lerpf(fill_a, fill_b, w);
    cur.fill           = cur.fill_intensity > 1e-4f;
    cur.rim_intensity  = lerpf(rim_a, rim_b, w);
    cur.rim            = cur.rim_intensity > 1e-4f;
    if (!cur.fill)
        cur.fill_intensity = to.fill_intensity;
    if (!cur.rim)
        cur.rim_intensity = to.rim_intensity;
    cur.environment      = pick(cur.environment, to.environment, w);
    cur.env_intensity    = lerpf(cur.env_intensity, to.env_intensity, w);
    cur.env_rotation_deg = lerp_angle(cur.env_rotation_deg, to.env_rotation_deg, w);
    cur.exposure         = lerpf(cur.exposure, to.exposure, w);
    cur.shadow_softness  = lerpf(cur.shadow_softness, to.shadow_softness, w);
}

void blend(Background &cur, const Background &to, double w)
{
    if (w >= 1.) {
        cur = to;
        return;
    }
    if (cur.type == to.type) {
        cur.color1 = lerp_color(cur.color1, to.color1, w);
        cur.color2 = lerp_color(cur.color2, to.color2, w);
        cur.blur   = lerpf(cur.blur, to.blur, w);
    } else if (w >= 0.5)
        cur = to;
}

void blend(Floor &cur, const Floor &to, double w)
{
    if (w >= 1.) {
        cur = to;
        return;
    }
    if (cur.type != to.type) {
        if (w >= 0.5)
            cur = to;
        return;
    }
    cur.color           = lerp_color(cur.color, to.color, w);
    cur.shadow_strength = lerpf(cur.shadow_strength, to.shadow_strength, w);
    cur.reflection      = lerpf(cur.reflection, to.reflection, w);
    cur.contact_shadow  = lerpf(cur.contact_shadow, to.contact_shadow, w);
    if (w >= 0.5) {   // a turn or a mirror switches half way
        cur.rotation = to.rotation;
        cur.flip     = to.flip;
    }
}

double event_start(const PathEvent &e) { return e.range ? e.from : e.at; }

// How much of the event is in effect at t (0..1).
double event_weight(const PathEvent &e, double t)
{
    if (e.range) {
        if (t < e.from || t >= e.to)
            return 0.;
        if (e.transition <= 0.)
            return 1.;
        return std::clamp(std::min((t - e.from) / e.transition, (e.to - t) / e.transition), 0., 1.);
    }
    if (t < e.at)
        return 0.;
    if (e.transition <= 0.)
        return 1.;
    return std::clamp((t - e.at) / e.transition, 0., 1.);
}

void apply_lighting(Lighting &li, const PathEvent &e)
{
    if (e.rig)
        apply_light_rig(li, *e.rig);
    if (e.azimuth)
        li.azimuth_deg = float(std::fmod(*e.azimuth, 360.));
    if (e.elevation)
        li.elevation_deg = float(*e.elevation);
    if (e.key_intensity)
        li.key_intensity = float(*e.key_intensity);
    if (e.temperature)
        li.key_temperature = float(*e.temperature);
    if (e.fill) {
        li.fill = *e.fill > 0.;
        if (li.fill)
            li.fill_intensity = float(*e.fill);
    }
    if (e.rim) {
        li.rim = *e.rim > 0.;
        if (li.rim)
            li.rim_intensity = float(*e.rim);
    }
    if (e.environment)
        li.environment = *e.environment;
    if (e.env_intensity)
        li.env_intensity = float(*e.env_intensity);
    if (e.env_rotation)
        li.env_rotation_deg = float(std::fmod(*e.env_rotation, 360.));
    if (e.exposure)
        li.exposure = float(*e.exposure);
    if (e.shadows)
        li.shadows = *e.shadows;
    if (e.shadow_intensity)
        li.shadow_intensity = float(*e.shadow_intensity);
    if (e.shadow_softness)
        li.shadow_softness = float(*e.shadow_softness);
    if (e.ssao)
        li.ssao = *e.ssao;
}

} // namespace

bool callout_selected(const PathEvent &e, const Callout &c)
{
    if (e.filaments.empty() && e.labels.empty())
        return true;
    if (std::find(e.filaments.begin(), e.filaments.end(), c.filament) != e.filaments.end())
        return true;
    for (const std::string &l : e.labels)
        if (l == c.key || (!c.title.empty() && lower(l) == lower(c.title)))
            return true;
    return false;
}

double timeline_end(const std::vector<PathEvent> &events)
{
    double end = 0.;
    for (const PathEvent &e : events)
        end = std::max(end, e.range ? e.to : e.at + e.transition);
    return end;
}

TimelineState evaluate_timeline(const std::vector<PathEvent> &events, const Scene &base, double t, double video_build)
{
    TimelineState st;
    st.scene = base;
    st.build = video_build;
    st.build_source = base.video.build.source;
    st.build_infill   = base.video.build.infill;
    st.build_supports = base.video.build.supports;
    for (const Callout &c : base.callouts.items)
        st.callouts[c.key] = CalloutVisual{ base.callouts.enabled && !c.hidden ? 1. : 0., 0. };
    if (events.empty())
        return st;

    std::vector<size_t> order(events.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&events](size_t a, size_t b) { return event_start(events[a]) < event_start(events[b]); });

    const PathEvent *build_event = nullptr;
    const PathEvent *first_build = nullptr;
    for (size_t idx : order) {
        const PathEvent &e = events[idx];
        if (e.type == PathEventType::Build) {
            if (first_build == nullptr)
                first_build = &e;
            if (t >= e.from)
                build_event = &e;
            continue;
        }
        const double w = event_weight(e, t);
        if (w <= 0.)
            continue;
        Scene &s = st.scene;
        switch (e.type) {
        case PathEventType::Lighting: {
            Lighting to = s.lighting;
            apply_lighting(to, e);
            blend(s.lighting, to, w);
            break;
        }
        case PathEventType::Background: {
            Background to = s.background;
            if (e.background)
                to.type = *e.background;
            if (e.color1)
                to.color1 = *e.color1;
            if (e.color2)
                to.color2 = *e.color2;
            blend(s.background, to, w);
            break;
        }
        case PathEventType::Floor: {
            Floor to = s.floor;
            if (e.floor)
                to.type = *e.floor;
            if (e.color)
                to.color = *e.color;
            if (e.shadow_strength)
                to.shadow_strength = float(*e.shadow_strength);
            if (e.reflection)
                to.reflection = float(*e.reflection);
            if (e.contact_shadow)
                to.contact_shadow = float(*e.contact_shadow);
            if (e.floor_rotation)
                to.rotation = floor_rotation_snap(*e.floor_rotation);
            if (e.floor_flip)
                to.flip = *e.floor_flip;
            blend(s.floor, to, w);
            break;
        }
        case PathEventType::Look: {
            Scene to = s;
            apply_preset(to, *e.look);
            blend(s.background, to.background, w);
            blend(s.floor, to.floor, w);
            blend(s.lighting, to.lighting, w);
            break;
        }
        case PathEventType::Callouts:
            for (const Callout &c : s.callouts.items) {
                CalloutVisual &v   = st.callouts[c.key];
                const bool     sel = callout_selected(e, c);
                switch (e.action) {
                case CalloutAction::Show:
                    if (sel)
                        v.alpha = lerp(v.alpha, 1., w);
                    break;
                case CalloutAction::Hide:
                    if (sel)
                        v.alpha = lerp(v.alpha, 0., w);
                    break;
                case CalloutAction::Highlight:
                    if (sel)
                        v.highlight = std::max(v.highlight, w);
                    else
                        v.alpha *= 1. - 0.65 * w;   // the others step back
                    break;
                }
            }
            break;
        case PathEventType::Watermark: {
            Watermark &wm = s.export_settings.watermark;
            if (e.text && w >= 0.5)
                wm.text = *e.text;
            if (e.anchor && w >= 0.5)
                wm.anchor = *e.anchor;
            if (e.size)
                wm.size_pct = lerpf(wm.size_pct, float(*e.size), w);
            const float opacity = e.opacity ? lerpf(wm.opacity, float(*e.opacity), w) : wm.opacity;
            if (e.enabled) {
                if (*e.enabled && !wm.enabled) {
                    wm.enabled = true;
                    wm.opacity = float(double(e.opacity ? *e.opacity : wm.opacity) * w);
                } else if (!*e.enabled && wm.enabled) {
                    wm.opacity = float(double(opacity) * (1. - w));
                    wm.enabled = w < 1.;
                } else
                    wm.opacity = opacity;
            } else
                wm.opacity = opacity;
            break;
        }
        default: break;
        }
    }

    if (first_build != nullptr) {
        const PathEvent &e = build_event != nullptr ? *build_event : *first_build;
        double           f = 0.;
        if (build_event == nullptr)
            f = e.build_start;
        else if (t >= e.to)
            f = e.build_end;
        else
            f = e.build_start + (e.build_end - e.build_start) * ease(e.build_easing, (t - e.from) / (e.to - e.from));
        st.build        = std::clamp(f, 0., 1.);
        st.build_source = e.build_source;
        if (e.build_infill)
            st.build_infill = *e.build_infill;
        if (e.build_supports)
            st.build_supports = *e.build_supports;
        st.build_follow = e.build_follow;
    }

    bool any = false;
    for (Callout &c : st.scene.callouts.items) {
        const CalloutVisual &v = st.callouts[c.key];
        c.hidden               = v.alpha <= 1e-3;
        any |= !c.hidden;
    }
    st.scene.callouts.enabled = any;
    return st;
}

// ------------------------------------------------------------------------------------------------
// The build-up
// ------------------------------------------------------------------------------------------------

BuildSchedule::BuildSchedule(std::vector<double> tops, double height)
{
    tops.erase(std::remove_if(tops.begin(), tops.end(), [](double z) { return !std::isfinite(z) || z <= 1e-6; }), tops.end());
    std::sort(tops.begin(), tops.end());
    for (double z : tops)
        if (m_tops.empty() || z - m_tops.back() > 1e-4)
            m_tops.push_back(z);
    m_height = std::max(std::isfinite(height) ? height : 0., m_tops.empty() ? 0. : m_tops.back());
    if (m_tops.empty() && m_height > 0.)
        m_tops.push_back(m_height);   // one layer: an even sweep
}

BuildSchedule::Position BuildSchedule::at(double fraction) const
{
    Position p;
    if (m_tops.empty())
        return p;
    fraction = std::isfinite(fraction) ? std::clamp(fraction, 0., 1.) : 1.;
    if (fraction <= 0.)
        return p;   // layer -1, nothing yet
    const size_t n = m_tops.size();
    if (fraction >= 1.) {
        p.layer    = int(n);
        p.progress = 1.;
        p.z        = m_height;
        return p;
    }
    // Every layer takes the same time, as on the printer: thin layers go by as fast as thick ones.
    const double pos    = fraction * double(n);
    const size_t k      = std::min(n - 1, size_t(pos));
    p.layer             = int(k);
    p.progress          = std::clamp(pos - double(k), 0., 1.);
    const double bottom = k == 0 ? 0. : m_tops[k - 1];
    p.z                 = bottom + p.progress * (m_tops[k] - bottom);
    return p;
}

} // namespace Showcase
} // namespace Slic3r
