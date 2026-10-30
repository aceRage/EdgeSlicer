// Showcase tab, phase S5: programmatic sequencing and the printed look
// (src/libslic3r/Showcase/ShowcasePath.{hpp,cpp}, ShowcaseRenderCli.{hpp,cpp}, the S5 parts of
// ShowcaseVideo and ShowcaseMaterial) - the path file's parsing and validation (messages that say
// where), the high-level moves compiled to keyframes deterministically, glide runs, the event
// timeline, the --showcase-render arguments and report, the layer build-up's schedule, the toolpath
// Smoothness mapping (D13), the export of a scene's video as a path, the shipped example paths, and
// the S5 video settings in the scene's JSON.

#include <catch2/catch.hpp>

#include "libslic3r/Showcase/ShowcaseMaterial.hpp"
#include "libslic3r/Showcase/ShowcasePath.hpp"
#include "libslic3r/Showcase/ShowcaseRenderCli.hpp"
#include "libslic3r/Showcase/ShowcaseScene.hpp"
#include "libslic3r/Showcase/ShowcaseVideo.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Showcase;
using Catch::Matchers::WithinAbs;
using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

constexpr double TEST_PI = 3.14159265358979323846;

// A plate with two objects (a cube and a tall part) and two filaments, framed from the front right.
PathContext sample_context()
{
    PathContext ctx;
    ctx.frame_height_px = 900.;
    ctx.projection      = Projection::Perspective;
    CameraView start;
    start.valid      = true;
    start.eye        = { 200., -200., 150. };
    start.target     = { 0., 0., 20. };
    start.up         = { 0., 0., 1. };
    start.zoom       = 3.;
    ctx.start        = start;
    ctx.default_view = start;
    ctx.plate.min    = { -40., -20., 0. };
    ctx.plate.max    = { 40., 20., 60. };
    PathContext::Object cube;
    cube.index   = 0;
    cube.name    = "Cube";
    cube.box.min = { -40., -20., 0. };
    cube.box.max = { 0., 20., 40. };
    PathContext::Object tower;
    tower.index   = 1;
    tower.name    = "Tower";
    tower.box.min = { 10., -10., 0. };
    tower.box.max = { 40., 10., 60. };
    ctx.objects   = { cube, tower };
    ctx.filaments = { { 1, cube.box }, { 2, tower.box } };
    CameraView hero;
    hero.valid  = true;
    hero.name   = "Hero";
    hero.eye    = { 0., -300., 80. };
    hero.target = { 0., 0., 30. };
    hero.zoom   = 4.;
    ctx.views   = { hero };
    ctx.callouts = { { "f1", 1, "" }, { "f2", 2, "Tower top" } };
    return ctx;
}

std::string doc_with(const std::string &body)
{
    return std::string("{\"format\": \"edgeslicer-showcase-path\", \"version\": 1") + (body.empty() ? "" : ", " + body) + "}";
}

bool has_issue(const PathIssues &issues, const std::string &where, bool error)
{
    for (const PathIssue &i : issues.items())
        if (i.where == where && i.error == error)
            return true;
    return false;
}

double dist(const std::array<double, 3> &a, const std::array<double, 3> &b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

double azimuth_deg(const std::array<double, 3> &eye, const std::array<double, 3> &target)
{
    return std::atan2(eye[1] - target[1], eye[0] - target[0]) * 180. / TEST_PI;
}

CompiledPath compile_text(const std::string &text, const PathContext &ctx, PathIssues &issues, bool expect_ok = true)
{
    PathDocument doc;
    const bool   parsed = parse_path(text, doc, issues);
    CompiledPath out;
    const bool   ok = parsed && compile_path(doc, ctx, out, issues);
    INFO(issues.text());
    CHECK(ok == expect_ok);
    return out;
}

Scene sample_scene()
{
    Scene s = make_scene(PresetId::StudioWhite, 0);
    Callout a;
    a.key      = "f1";
    a.filament = 1;
    Callout b;
    b.key      = "f2";
    b.filament = 2;
    b.title    = "Tower top";
    Callout c;
    c.key      = "f2@12.40";
    c.filament = 2;
    c.z_from   = 12.4;
    s.callouts.enabled = true;
    s.callouts.items   = { a, b, c };
    return s;
}

} // namespace

TEST_CASE("Path files: parsing and validation", "[Showcase][Path]")
{
    SECTION("a minimal file parses")
    {
        PathDocument doc;
        PathIssues   issues;
        REQUIRE(parse_path(doc_with("\"name\": \"Hello\""), doc, issues));
        CHECK(doc.name == "Hello");
        CHECK(issues.items().empty());
    }
    SECTION("the format and version are required and checked")
    {
        PathDocument doc;
        PathIssues   a;
        CHECK_FALSE(parse_path("{\"version\": 1}", doc, a));
        CHECK(has_issue(a, "format", true));
        PathIssues b;
        CHECK_FALSE(parse_path("{\"format\": \"something-else\", \"version\": 1}", doc, b));
        CHECK(has_issue(b, "format", true));
        PathIssues c;
        CHECK_FALSE(parse_path("{\"format\": \"edgeslicer-showcase-path\", \"version\": 2}", doc, c));
        CHECK(has_issue(c, "version", true));
        CHECK(c.text().find("newer EdgeSlicer") != std::string::npos);
    }
    SECTION("broken JSON says where")
    {
        PathDocument doc;
        PathIssues   issues;
        CHECK_FALSE(parse_path("{\"format\": \"edgeslicer-showcase-path\",\n \"version\": 1,,}", doc, issues));
        REQUIRE(issues.errors() == 1);
        CHECK(issues.items()[0].message.find("not valid JSON") != std::string::npos);
        CHECK(issues.items()[0].message.find("line 2") != std::string::npos);
    }
    SECTION("unknown keys warn with the key meant; comments are fine")
    {
        PathDocument doc;
        PathIssues   issues;
        REQUIRE(parse_path(doc_with("\"_comment\": \"x\", \"moves\": [{\"op\": \"orbit\", \"deg\": 90, \"durration\": 2}]"), doc, issues));
        CHECK(issues.errors() == 0);
        REQUIRE(issues.warnings() == 1);
        CHECK(issues.items()[0].where == "moves[0].durration");
        CHECK(issues.items()[0].message.find("did you mean \"duration\"") != std::string::npos);
    }
    SECTION("type and range errors name the field")
    {
        PathDocument doc;
        PathIssues   issues;
        CHECK_FALSE(parse_path(doc_with("\"moves\": [{\"op\": \"orbit\", \"deg\": \"ninety\"}, {\"op\": \"teleport\"},"
                                        " {\"op\": \"dolly_in\", \"factor\": 0.5}, {\"op\": \"wait\", \"duration\": 99}]"),
                               doc, issues));
        CHECK(has_issue(issues, "moves[0].deg", true));
        CHECK(has_issue(issues, "moves[1].op", true));
        CHECK(issues.text().find("teleport") != std::string::npos);
        CHECK(has_issue(issues, "moves[2].factor", true));
        CHECK(has_issue(issues, "moves[3].duration", true));
    }
    SECTION("poses: one framing only, eye and target apart")
    {
        PathDocument doc;
        PathIssues   issues;
        CHECK_FALSE(parse_path(doc_with("\"keyframes\": [{\"t\": 0, \"eye\": [0,-100,50], \"target\": [0,0,0], \"fov\": 30, \"zoom\": 2},"
                                        " {\"t\": 1, \"eye\": [1,1,1], \"target\": [1,1,1]}, {\"eye\": [0,1,0], \"target\": [0,0,0]}]"),
                               doc, issues));
        CHECK(has_issue(issues, "keyframes[0].fov", true));
        CHECK(has_issue(issues, "keyframes[1].eye", true));
        CHECK(has_issue(issues, "keyframes[2].t", true));
    }
    SECTION("events: timing forms and per-type checks")
    {
        PathDocument doc;
        PathIssues   issues;
        CHECK_FALSE(parse_path(doc_with("\"events\": ["
                                        "{\"type\": \"lighting\", \"at\": 1, \"from\": 2, \"to\": 3},"
                                        "{\"type\": \"lighting\", \"from\": 3, \"to\": 2},"
                                        "{\"type\": \"build\", \"at\": 1},"
                                        "{\"type\": \"background\", \"at\": 0, \"color1\": \"red\"},"
                                        "{\"type\": \"callouts\", \"at\": 0, \"action\": \"wiggle\"},"
                                        "{\"type\": \"look\", \"at\": 0, \"preset\": \"moon\"},"
                                        "{\"type\": \"lighting\", \"from\": 0, \"to\": 1, \"fade\": 0.8}]"),
                               doc, issues));
        CHECK(has_issue(issues, "events[0].at", true));
        CHECK(has_issue(issues, "events[1].to", true));
        CHECK(has_issue(issues, "events[2].at", true));
        CHECK(has_issue(issues, "events[3].color1", true));
        CHECK(has_issue(issues, "events[4].action", true));
        CHECK(has_issue(issues, "events[5].preset", true));
        CHECK(has_issue(issues, "events[6].fade", true));
    }
    SECTION("a valid event of every type")
    {
        PathDocument doc;
        PathIssues   issues;
        REQUIRE(parse_path(doc_with("\"events\": ["
                                    "{\"type\": \"lighting\", \"at\": 1, \"transition\": 0.5, \"rig\": \"dramatic\", \"azimuth\": 200, \"fill\": 0},"
                                    "{\"type\": \"background\", \"at\": 2, \"kind\": \"solid\", \"color1\": \"#101820\"},"
                                    "{\"type\": \"floor\", \"at\": 2, \"kind\": \"solid\", \"color\": \"#202020\", \"reflection\": 0.3},"
                                    "{\"type\": \"look\", \"at\": 3, \"preset\": \"dark_gradient\"},"
                                    "{\"type\": \"callouts\", \"from\": 1, \"to\": 4, \"action\": \"highlight\", \"filaments\": [2], \"labels\": \"f1\"},"
                                    "{\"type\": \"watermark\", \"from\": 0, \"to\": 2, \"text\": \"@maker\", \"anchor\": \"top_left\"},"
                                    "{\"type\": \"build\", \"from\": 0, \"to\": 5, \"ease\": \"ease_out\", \"source\": \"clip\"}]"),
                           doc, issues));
        CHECK(issues.items().empty());
        REQUIRE(doc.events.size() == 7);
        CHECK(doc.events[0].rig == LightRig::Dramatic);
        CHECK(*doc.events[0].fill == 0.);
        CHECK(doc.events[4].filaments == std::vector<int>{ 2 });
        CHECK(doc.events[4].labels == std::vector<std::string>{ "f1" });
        CHECK(*doc.events[5].enabled);   // a range shows the mark
        CHECK(doc.events[6].build_source == BuildSource::Clip);
        // The canonical events read back the same.
        std::vector<PathEvent> back;
        PathIssues             again;
        REQUIRE(parse_path_events(path_events_to_json(doc.events), back, again));
        CHECK(path_events_to_json(back) == path_events_to_json(doc.events));
    }
}

TEST_CASE("Path moves compile to keyframes", "[Showcase][Path]")
{
    const PathContext ctx = sample_context();
    const double      r0  = dist(ctx.start.eye, ctx.start.target);

    SECTION("orbit: a glide run every 15 degrees, at the same distance and height")
    {
        PathIssues         issues;
        const CompiledPath p = compile_text(doc_with("\"moves\": [{\"op\": \"orbit\", \"deg\": 90, \"duration\": 3, \"ease\": \"linear\"}]"), ctx, issues);
        REQUIRE(p.keys.size() == 7);   // the start, then 6 keys of 15 degrees
        CHECK_FALSE(p.keys.front().glide);
        CHECK_FALSE(p.keys.back().glide);
        for (size_t i = 1; i + 1 < p.keys.size(); ++i)
            CHECK(p.keys[i].glide);
        CHECK_THAT(p.keys.back().time, WithinAbs(3., 1e-9));
        CHECK_THAT(p.duration, WithinAbs(3., 1e-9));
        for (const Keyframe &k : p.keys) {
            CHECK_THAT(dist(k.eye, k.target), WithinAbs(r0, 1e-6));
            CHECK_THAT(k.eye[2], WithinAbs(ctx.start.eye[2], 1e-6));
        }
        const double a0 = azimuth_deg(ctx.start.eye, ctx.start.target);
        CHECK_THAT(azimuth_deg(p.keys.back().eye, p.keys.back().target), WithinAbs(a0 + 90., 1e-6));
        // Glide keys sit where the run passes them, in order.
        for (size_t i = 1; i < p.keys.size(); ++i)
            CHECK(p.keys[i].time > p.keys[i - 1].time);
        // Evaluated: half the time is half the turn (linear), always on the circle.
        const CameraPose mid = evaluate_keyframes(p.keys, 1.5, ctx.start);
        CHECK_THAT(azimuth_deg(mid.eye, mid.target), WithinAbs(a0 + 45., 0.5));
        for (double t = 0.; t <= 3.; t += 0.1) {
            const CameraPose q = evaluate_keyframes(p.keys, t, ctx.start);
            CHECK_THAT(dist(q.eye, q.target), WithinAbs(r0, 0.01 * r0));
        }
    }
    SECTION("an eased orbit eases once over the whole run, not per key")
    {
        PathIssues         issues;
        const CompiledPath p  = compile_text(doc_with("\"moves\": [{\"op\": \"orbit\", \"deg\": 180, \"duration\": 4}]"), ctx, issues);
        const double       a0 = azimuth_deg(ctx.start.eye, ctx.start.target);
        auto turned = [&](double t) {
            const CameraPose q = evaluate_keyframes(p.keys, t, ctx.start);
            double           d = azimuth_deg(q.eye, q.target) - a0;
            while (d < -1.)
                d += 360.;
            return d;
        };
        CHECK_THAT(turned(2.), WithinAbs(90., 1.));   // smoothstep is symmetric
        CHECK(turned(0.4) < 0.1 * 180.);              // slow start
        // Even speed in the middle: equal steps turn about equally (no stop at the 15-degree keys).
        const double s1 = turned(1.9) - turned(1.8), s2 = turned(2.1) - turned(2.0);
        CHECK_THAT(s1, WithinAbs(s2, 0.15 * s2));
        double prev = -1.;
        for (double t = 0.; t <= 4.0001; t += 0.05) {
            const double now = turned(t);
            CHECK(now >= prev - 1e-6);   // never back
            prev = now;
        }
    }
    SECTION("compiling is deterministic")
    {
        const std::string text = doc_with("\"moves\": [{\"op\": \"orbit\", \"deg\": -120, \"elevation\": 10},"
                                          " {\"op\": \"focus_object\", \"object\": \"Tower\"}, {\"op\": \"dolly_in\", \"factor\": 1.4},"
                                          " {\"op\": \"wait\", \"duration\": 0.5}, {\"op\": \"fly_to\", \"view\": \"iso\"}],"
                                          " \"events\": [{\"type\": \"lighting\", \"at\": 1, \"rig\": \"sun\"}]");
        PathIssues         i1, i2;
        const CompiledPath a = compile_text(text, ctx, i1);
        const CompiledPath b = compile_text(text, ctx, i2);
        REQUIRE(a.keys.size() == b.keys.size());
        CHECK(a.keys == b.keys);
        CHECK(a.duration == b.duration);
        CHECK(path_events_to_json(a.events) == path_events_to_json(b.events));
    }
    SECTION("turntable: the camera goes the other way and the model's spin is kept")
    {
        PathIssues         issues;
        const CompiledPath p  = compile_text(doc_with("\"moves\": [{\"op\": \"turntable\", \"deg\": 360, \"duration\": 8, \"ease\": \"linear\"}]"), ctx, issues);
        REQUIRE(p.keys.size() == 25);
        CHECK_THAT(p.keys.back().spin, WithinAbs(360., 1e-9));
        const CameraPose q  = evaluate_keyframes(p.keys, 2., ctx.start);
        CHECK_THAT(q.spin_deg, WithinAbs(90., 2.));
        const double a0 = azimuth_deg(ctx.start.eye, ctx.start.target);
        double       d  = azimuth_deg(q.eye, q.target) - a0;
        while (d > 180.)
            d -= 360.;
        while (d < -180.)
            d += 360.;
        CHECK_THAT(d, WithinAbs(-90., 2.));
    }
    SECTION("dolly, zoom and pan")
    {
        PathIssues         issues;
        const CompiledPath p = compile_text(doc_with("\"moves\": [{\"op\": \"dolly_in\", \"factor\": 2, \"duration\": 1},"
                                                     " {\"op\": \"dolly_out\", \"distance\": 50, \"duration\": 1},"
                                                     " {\"op\": \"zoom\", \"factor\": 2, \"duration\": 1},"
                                                     " {\"op\": \"pan\", \"right\": 10, \"up\": 5, \"duration\": 1}]"),
                                             ctx, issues);
        REQUIRE(p.keys.size() == 5);
        CHECK_THAT(dist(p.keys[1].eye, p.keys[1].target), WithinAbs(r0 / 2., 1e-6));
        // The field of view stays through a dolly: the zoom (pixels per mm at the target) doubles.
        CHECK_THAT(p.keys[1].zoom, WithinAbs(2. * ctx.start.zoom, 1e-6));
        CHECK_THAT(dist(p.keys[2].eye, p.keys[2].target), WithinAbs(r0 / 2. + 50., 1e-6));
        CHECK_THAT(p.keys[3].zoom, WithinAbs(2. * p.keys[2].zoom, 1e-9));
        // A pan moves the eye and the target together, 10 mm right and 5 mm up on the frame.
        CHECK_THAT(dist(p.keys[4].eye, p.keys[3].eye), WithinAbs(std::sqrt(125.), 1e-6));
        CHECK_THAT(dist(p.keys[4].target, p.keys[3].target), WithinAbs(std::sqrt(125.), 1e-6));
        CHECK(p.keys[4].target[2] > p.keys[3].target[2]);
        CHECK_THAT(p.duration, WithinAbs(4., 1e-9));
    }
    SECTION("focus on an object or a filament frames its box from the same side")
    {
        PathIssues         issues;
        const CompiledPath p = compile_text(doc_with("\"moves\": [{\"op\": \"focus_object\", \"object\": \"tower\", \"margin\": 1.0},"
                                                     " {\"op\": \"focus_filament\", \"filament\": 1}, {\"op\": \"focus_object\", \"object\": 1}]"),
                                             ctx, issues);
        REQUIRE(p.keys.size() == 4);
        CHECK(p.keys[1].target == std::array<double, 3>{ { 25., 0., 30. } });
        CHECK(p.keys[2].target == std::array<double, 3>{ { -20., 0., 20. } });
        CHECK(p.keys[3].target == p.keys[1].target);
        // The same direction of view as before.
        auto dir = [](const Keyframe &k) {
            const double d = dist(k.eye, k.target);
            return std::array<double, 3>{ { (k.eye[0] - k.target[0]) / d, (k.eye[1] - k.target[1]) / d, (k.eye[2] - k.target[2]) / d } };
        };
        CHECK(dist(dir(p.keys[0]), dir(p.keys[1])) < 1e-9);
        // Margin 1: the frame shows the bounding sphere's diameter.
        const double radius = 0.5 * std::sqrt(30. * 30. + 20. * 20. + 60. * 60.);
        CHECK_THAT(path_view_height(p.keys[1].zoom, ctx.frame_height_px), WithinAbs(2. * radius, 1e-6));
    }
    SECTION("fly to a built-in or a saved view, move to a pose")
    {
        PathIssues         issues;
        const CompiledPath p = compile_text(doc_with("\"moves\": [{\"op\": \"fly_to\", \"view\": \"top\"}, {\"op\": \"fly_to\", \"view\": \"hero\"},"
                                                     " {\"op\": \"move_to\", \"eye\": [0, -150, 40], \"target\": [0, 0, 30], \"fov\": 40}]"),
                                             ctx, issues);
        REQUIRE(p.keys.size() == 4);
        CHECK_THAT(p.keys[1].eye[0], WithinAbs(0., 1e-9));
        CHECK(p.keys[1].eye[2] > 60.);   // looking down at the plate's centre
        CHECK(p.keys[1].target == std::array<double, 3>{ { 0., 0., 30. } });
        CHECK(p.keys[2].eye == ctx.views[0].eye);
        CHECK_THAT(p.keys[2].zoom, WithinAbs(4., 1e-9));
        const double vh = path_view_height(p.keys[3].zoom, ctx.frame_height_px);
        CHECK_THAT(path_fov_deg(vh, dist(p.keys[3].eye, p.keys[3].target)), WithinAbs(40., 1e-6));
    }
    SECTION("keyframes with holds, then moves after them")
    {
        PathIssues         issues;
        const CompiledPath p = compile_text(doc_with("\"keyframes\": [{\"t\": 0, \"eye\": [0, -200, 60], \"target\": [0, 0, 20], \"fov\": 30, \"hold\": 1},"
                                                     " {\"t\": 3, \"eye\": [200, 0, 60], \"target\": [0, 0, 20], \"ease\": \"ease_out\"}],"
                                                     " \"moves\": [{\"op\": \"wait\", \"duration\": 2}]"),
                                             ctx, issues);
        REQUIRE(p.keys.size() == 4);
        CHECK(p.keys[0].time == 0.);
        CHECK(p.keys[1].time == 1.);
        CHECK(p.keys[1].eye == p.keys[0].eye);
        CHECK(p.keys[2].ease == Easing::EaseOut);
        CHECK_THAT(p.keys[3].time, WithinAbs(5., 1e-12));
        // The hold: nothing moves in the first second.
        const CameraPose q = evaluate_keyframes(p.keys, 0.6, ctx.start);
        CHECK(dist(q.eye, p.keys[0].eye) < 1e-3);
        // Without a framing the second key keeps the first's 30 degrees.
        const double vh = path_view_height(p.keys[2].zoom, ctx.frame_height_px);
        CHECK_THAT(path_fov_deg(vh, dist(p.keys[2].eye, p.keys[2].target)), WithinAbs(30., 1e-6));
    }
    SECTION("compile errors say what and where")
    {
        PathIssues issues;
        compile_text(doc_with("\"moves\": [{\"op\": \"focus_object\", \"object\": \"Benchy\"}, {\"op\": \"dolly_in\", \"distance\": 5000},"
                              " {\"op\": \"fly_to\", \"view\": \"somewhere\"}, {\"op\": \"focus_filament\", \"filament\": 7}]"),
                     ctx, issues, false);
        CHECK(has_issue(issues, "moves[0].object", true));
        CHECK(issues.text().find("1 \"Tower\"") != std::string::npos);   // the objects there are
        CHECK(has_issue(issues, "moves[1].distance", true));
        CHECK(has_issue(issues, "moves[2].view", true));
        CHECK(issues.text().find("\"Hero\"") != std::string::npos);
        CHECK(has_issue(issues, "moves[3].filament", true));
    }
    SECTION("keyframes out of order, a too long path, an ortho mismatch")
    {
        PathIssues a;
        compile_text(doc_with("\"keyframes\": [{\"t\": 2, \"eye\": [0,-100,0], \"target\": [0,0,0]}, {\"t\": 1, \"eye\": [0,-90,0], \"target\": [0,0,0]}]"), ctx, a,
                     false);
        CHECK(has_issue(a, "keyframes[1].t", true));
        PathIssues b;
        compile_text(doc_with("\"moves\": [{\"op\": \"wait\", \"duration\": 40}, {\"op\": \"orbit\", \"deg\": 90, \"duration\": 30}]"), ctx, b, false);
        CHECK(b.text().find("at most 60") != std::string::npos);
        PathIssues c;
        compile_text(doc_with("\"projection\": \"ortho\", \"moves\": [{\"op\": \"move_to\", \"eye\": [0,-100,0], \"target\": [0,0,0], \"fov\": 30}]"), ctx, c,
                     false);
        CHECK(has_issue(c, "moves[0].fov", true));
    }
    SECTION("warnings: unknown callouts, a cut path, a watermark without content")
    {
        PathIssues         issues;
        const CompiledPath p = compile_text(doc_with("\"duration\": 2, \"moves\": [{\"op\": \"orbit\", \"deg\": 90, \"duration\": 3}],"
                                                     " \"events\": [{\"type\": \"callouts\", \"at\": 0, \"action\": \"show\", \"filaments\": [9], \"labels\": [\"nope\"]},"
                                                     " {\"type\": \"watermark\", \"at\": 0, \"enabled\": true}]"),
                                             ctx, issues);
        CHECK(issues.errors() == 0);
        CHECK(has_issue(issues, "duration", false));
        CHECK(has_issue(issues, "events[0].filaments", false));
        CHECK(has_issue(issues, "events[0].labels", false));
        CHECK(has_issue(issues, "events[1]", false));
        CHECK_THAT(p.duration, WithinAbs(2., 1e-12));
    }
    SECTION("the compiled path becomes the scene's keyframe video")
    {
        PathIssues         issues;
        const CompiledPath p = compile_text(doc_with("\"name\": \"Spin\", \"fps\": 24, \"moves\": [{\"op\": \"orbit\", \"deg\": 45, \"duration\": 2}],"
                                                     " \"events\": [{\"type\": \"build\", \"from\": 0, \"to\": 2}]"),
                                             ctx, issues);
        VideoSettings v;
        v.build.enabled = true;
        v.format        = VideoFormat::GIF;
        apply_compiled_path(p, v);
        CHECK(v.path == VideoPath::Keyframes);
        CHECK(v.keyframes == p.keys);
        CHECK(v.fps == 24);
        CHECK(v.duration == 2.);
        CHECK(v.path_name == "Spin");
        CHECK(v.format == VideoFormat::GIF);   // output settings stay
        CHECK_FALSE(v.build.enabled);          // the file's build event says when
        CHECK_FALSE(v.events.empty());
        // Every frame of the video is on the path.
        VideoSettings s = v;
        sanitize(s);
        REQUIRE(s.keyframes.size() == v.keyframes.size());
        for (size_t i = 0; i < s.keyframes.size(); ++i) {
            CHECK(s.keyframes[i].time == v.keyframes[i].time);
            CHECK(s.keyframes[i].eye == v.keyframes[i].eye);
        }
        const CameraPose end = evaluate_path(s, ctx.start, total_seconds(s));
        CHECK(dist(end.eye, p.keys.back().eye) < 1e-6);
    }
}

TEST_CASE("Path timeline: events at a time", "[Showcase][Path]")
{
    const Scene base = sample_scene();
    auto events_of = [](const std::string &arr) {
        std::vector<PathEvent> ev;
        PathIssues             issues;
        const bool             ok = parse_path_events(arr, ev, issues);
        INFO(issues.text());
        REQUIRE(ok);
        return ev;
    };
    SECTION("no events: the base scene, callouts as they are")
    {
        const TimelineState st = evaluate_timeline({}, base, 3.);
        CHECK(st.scene == base);
        CHECK(st.build < 0.);
        CHECK(st.callouts.at("f1").alpha == 1.);
    }
    SECTION("a lighting change blends over its transition")
    {
        Scene s               = base;
        s.lighting.azimuth_deg = 225.f;
        const auto ev = events_of("[{\"type\": \"lighting\", \"at\": 2, \"transition\": 2, \"key_intensity\": 2.0, \"azimuth\": 315}]");
        CHECK(evaluate_timeline(ev, s, 1.).scene.lighting == s.lighting);
        const TimelineState half = evaluate_timeline(ev, s, 3.);
        CHECK_THAT(half.scene.lighting.key_intensity, WithinAbs(0.5 * (s.lighting.key_intensity + 2.f), 1e-5));
        CHECK_THAT(half.scene.lighting.azimuth_deg, WithinAbs(270., 1e-3));
        CHECK_THAT(evaluate_timeline(ev, s, 9.).scene.lighting.key_intensity, WithinAbs(2., 1e-6));
    }
    SECTION("the short way round for angles")
    {
        Scene s                 = base;
        s.lighting.azimuth_deg  = 350.f;
        const auto          ev  = events_of("[{\"type\": \"lighting\", \"at\": 0, \"transition\": 1, \"azimuth\": 10}]");
        const TimelineState mid = evaluate_timeline(ev, s, 0.5);
        CHECK(std::abs(std::fmod(double(mid.scene.lighting.azimuth_deg) + 360., 360.)) < 1e-3);
    }
    SECTION("a range comes and goes with its fade")
    {
        const auto ev = events_of("[{\"type\": \"background\", \"from\": 1, \"to\": 3, \"fade\": 0.5, \"color1\": \"#000000\"}]");
        CHECK(evaluate_timeline(ev, base, 0.5).scene.background.color1 == base.background.color1);
        CHECK(evaluate_timeline(ev, base, 2.).scene.background.color1 == "#000000");
        CHECK(evaluate_timeline(ev, base, 1.25).scene.background.color1 != "#000000");
        CHECK(evaluate_timeline(ev, base, 3.5).scene.background.color1 == base.background.color1);
    }
    SECTION("callouts: show, hide and highlight")
    {
        const auto ev = events_of("[{\"type\": \"callouts\", \"at\": 0, \"action\": \"hide\"},"
                                  " {\"type\": \"callouts\", \"at\": 1, \"action\": \"show\", \"filaments\": [2]},"
                                  " {\"type\": \"callouts\", \"from\": 2, \"to\": 4, \"action\": \"highlight\", \"labels\": [\"tower top\"]}]");
        const TimelineState t0 = evaluate_timeline(ev, base, 0.5);
        CHECK(t0.callouts.at("f1").alpha == 0.);
        CHECK_FALSE(t0.scene.callouts.enabled);
        const TimelineState t1 = evaluate_timeline(ev, base, 1.5);
        CHECK(t1.callouts.at("f1").alpha == 0.);
        CHECK(t1.callouts.at("f2").alpha == 1.);
        CHECK(t1.callouts.at("f2@12.40").alpha == 1.);   // filament 2 after a colour change too
        CHECK(t1.scene.callouts.enabled);
        CHECK(t1.scene.callouts.find("f1")->hidden);
        CHECK_FALSE(t1.scene.callouts.find("f2")->hidden);
        const TimelineState t2 = evaluate_timeline(ev, base, 3.);
        CHECK(t2.callouts.at("f2").highlight == 1.);   // by its title, any case
        CHECK(t2.callouts.at("f2@12.40").highlight == 0.);
        CHECK(t2.callouts.at("f2@12.40").alpha < 0.5);   // the others step back
        CHECK(evaluate_timeline(ev, base, 4.5).callouts.at("f2").highlight == 0.);
    }
    SECTION("a look, the floor and the watermark")
    {
        const auto ev = events_of("[{\"type\": \"look\", \"at\": 1, \"preset\": \"dark_gradient\"},"
                                  " {\"type\": \"floor\", \"at\": 2, \"reflection\": 0.5},"
                                  " {\"type\": \"watermark\", \"from\": 3, \"to\": 5, \"text\": \"@maker\", \"opacity\": 0.8}]");
        Scene dark = base;
        apply_preset(dark, PresetId::DarkGradient);
        const TimelineState t1 = evaluate_timeline(ev, base, 1.5);
        CHECK(t1.scene.background == dark.background);
        CHECK(t1.scene.lighting == dark.lighting);
        const TimelineState t2 = evaluate_timeline(ev, base, 2.5);
        CHECK_THAT(t2.scene.floor.reflection, WithinAbs(0.5, 1e-6));
        CHECK_FALSE(t2.scene.export_settings.watermark.enabled);
        const TimelineState t3 = evaluate_timeline(ev, base, 4.);
        CHECK(t3.scene.export_settings.watermark.enabled);
        CHECK(t3.scene.export_settings.watermark.text == "@maker");
        CHECK_THAT(t3.scene.export_settings.watermark.opacity, WithinAbs(0.8, 1e-6));
        CHECK_FALSE(evaluate_timeline(ev, base, 5.5).scene.export_settings.watermark.enabled);
    }
    SECTION("the build-up: before, during and after, and the scene's own")
    {
        const auto ev = events_of("[{\"type\": \"build\", \"from\": 1, \"to\": 3, \"start\": 0.2, \"end\": 1, \"source\": \"toolpaths\"}]");
        CHECK_THAT(evaluate_timeline(ev, base, 0.).build, WithinAbs(0.2, 1e-12));
        CHECK_THAT(evaluate_timeline(ev, base, 2.).build, WithinAbs(0.6, 1e-12));
        CHECK_THAT(evaluate_timeline(ev, base, 5.).build, WithinAbs(1., 1e-12));
        CHECK(evaluate_timeline(ev, base, 2.).build_source == BuildSource::Toolpaths);
        CHECK_THAT(evaluate_timeline({}, base, 2., 0.25).build, WithinAbs(0.25, 1e-12));
        CHECK_THAT(timeline_end(ev), WithinAbs(3., 1e-12));
    }
    SECTION("events apply in time order, file order for ties")
    {
        const auto ev = events_of("[{\"type\": \"lighting\", \"at\": 2, \"key_intensity\": 0.5},"
                                  " {\"type\": \"lighting\", \"at\": 1, \"key_intensity\": 2.5},"
                                  " {\"type\": \"lighting\", \"at\": 2, \"key_intensity\": 1.5}]");
        CHECK_THAT(evaluate_timeline(ev, base, 1.5).scene.lighting.key_intensity, WithinAbs(2.5, 1e-6));
        CHECK_THAT(evaluate_timeline(ev, base, 3.).scene.lighting.key_intensity, WithinAbs(1.5, 1e-6));
    }
}

TEST_CASE("--showcase-render arguments", "[Showcase][Path][CLI]")
{
    auto parse = [](std::vector<std::string> args, RenderRequest &r) {
        std::string error;
        const bool  ok = parse_render_args(args, r, error);
        INFO(error);
        return ok;
    };
    SECTION("a full command line")
    {
        RenderRequest r;
        REQUIRE(parse({ "--showcase-render", "C:/p/benchy.3mf", "--path", "orbit.json", "--out", "C:/o/v.webm", "--plate", "2", "--size", "720",
                        "--aspect", "1:1", "--fps", "24", "--quality", "draft", "--bitrate", "70", "--report", "r.json", "--datadir", "D:/d" },
                      r));
        CHECK(r.project == "C:/p/benchy.3mf");
        CHECK(r.path == "orbit.json");
        CHECK(r.format == VideoFormat::WebM);
        CHECK(r.plate == 2);
        CHECK(r.size == 720);
        CHECK(r.aspect_w == 1);
        CHECK(r.aspect_h == 1);
        CHECK(r.fps == 24);
        CHECK(r.quality == Quality::Draft);
        CHECK(r.bitrate == 70);
        CHECK(r.report == "r.json");
        CHECK(r.datadir == "D:/d");
        CHECK_FALSE(r.still);
    }
    SECTION("the format from --format or the extension")
    {
        RenderRequest r;
        REQUIRE(parse({ "--showcase-render", "a.3mf", "--out", "x.MP4" }, r));
        CHECK(r.format == VideoFormat::MP4);
        REQUIRE(parse({ "--showcase-render", "a.3mf", "--out", "frames" }, r));
        CHECK(r.format == VideoFormat::PNGSequence);
        REQUIRE(parse({ "--showcase-render", "a.3mf", "--out", "frames", "--format", "png" }, r));
        CHECK(r.format == VideoFormat::PNGSequence);
        REQUIRE(parse({ "--showcase-render", "a.3mf", "--out", "x.gif" }, r));
        CHECK(r.format == VideoFormat::GIF);
        REQUIRE(parse({ "--showcase-render", "a.3mf", "--out", "x.webm", "--format", "webm_alpha" }, r));
        CHECK(r.format == VideoFormat::WebMAlpha);
        REQUIRE(parse({ "--showcase-render", "a.3mf", "--out", "shot.jpg", "--still", "1.5" }, r));
        CHECK(r.still);
        CHECK(r.still_time == 1.5);
        CHECK(r.still_format == ImageFormat::JPEG);
        REQUIRE(parse({ "--showcase-render", "a.3mf", "--out", "shot.webp", "--still", "0" }, r));
        CHECK(r.still_format == ImageFormat::WebP);
        REQUIRE(parse({ "--showcase-render", "a.3mf", "--dry-run", "--path", "p.json" }, r));
        CHECK(r.dry_run);
        CHECK_FALSE(r.slice);
        REQUIRE(parse({ "--showcase-render", "a.3mf", "--out", "x.mp4", "--slice" }, r));
        CHECK(r.slice);
    }
    SECTION("mistakes are refused with a reason")
    {
        RenderRequest r;
        std::string   error;
        for (const std::vector<std::string> &bad : std::vector<std::vector<std::string>>{
                 { "--showcase-render" },
                 { "--showcase-render", "a.stl", "--out", "x.mp4" },
                 { "--showcase-render", "a.3mf" },
                 { "--showcase-render", "a.3mf", "--out", "x.avi" },
                 { "--showcase-render", "a.3mf", "--out", "x.png" },
                 { "--showcase-render", "a.3mf", "--out", "x.jpg" },
                 { "--showcase-render", "a.3mf", "--out", "x.mp4", "--still", "1" },
                 { "--showcase-render", "a.3mf", "--out", "x.mp4", "--plate", "0" },
                 { "--showcase-render", "a.3mf", "--out", "x.mp4", "--size", "99999" },
                 { "--showcase-render", "a.3mf", "--out", "x.mp4", "--fps", "7" },
                 { "--showcase-render", "a.3mf", "--out", "x.mp4", "--aspect", "wide" },
                 { "--showcase-render", "a.3mf", "--out", "x.mp4", "--quality", "ultra" },
                 { "--showcase-render", "a.3mf", "--out", "x.mp4", "--still", "-1" },
                 { "--showcase-render", "a.3mf", "--out", "x.mp4", "--path" },
                 { "--showcase-render", "a.3mf", "--out", "x.mp4", "--frobnicate" } }) {
            INFO(bad.back());
            CHECK_FALSE(parse_render_args(bad, r, error));
            CHECK_FALSE(error.empty());
        }
    }
    SECTION("detecting the mode and the help")
    {
        const char *argv1[] = { "EdgeSlicer.exe", "--datadir", "x", "--showcase-render", "a.3mf" };
        const char *argv2[] = { "EdgeSlicer.exe", "--export-3mf", "a.3mf" };
        CHECK(is_showcase_render_cli(5, const_cast<char **>(argv1)));
        CHECK_FALSE(is_showcase_render_cli(3, const_cast<char **>(argv2)));
        RenderRequest r;
        REQUIRE(parse({ "--showcase-render", "--help" }, r));
        CHECK(r.help);
        CHECK(render_usage().find("--still") != std::string::npos);
    }
    SECTION("the report")
    {
        RenderReport rep;
        rep.ok       = true;
        rep.project  = "a.3mf";
        rep.out      = "v.mp4";
        rep.format   = "mp4";
        rep.encoder  = "ffmpeg h264_mf";
        rep.width    = 1920;
        rep.height   = 1080;
        rep.fps      = 30;
        rep.frames   = 90;
        rep.duration = 3.;
        rep.warnings = { "moves[0].durration: unknown key" };
        const json j = json::parse(render_report_json(rep));
        CHECK(j["ok"] == true);
        CHECK(j["exit_code"] == 0);
        CHECK(j["kind"] == "video");
        CHECK(j["frames"] == 90);
        CHECK(j["encoder"] == "ffmpeg h264_mf");
        CHECK(j["warnings"].size() == 1);
        CHECK(j["errors"].empty());
        CHECK(j["sliced"] == false);
    }
}

TEST_CASE("Layer build-up schedule", "[Showcase][Path][Build]")
{
    SECTION("uniform layers: every layer the same time, the plane inside the layer")
    {
        std::vector<double> tops;
        for (int i = 1; i <= 50; ++i)
            tops.push_back(0.2 * i);
        const BuildSchedule s(tops, 10.);
        CHECK(s.layers() == 50);
        CHECK(s.at(0.).layer == -1);
        CHECK(s.at(0.).z == 0.);
        CHECK(s.at(1.).layer == 50);
        CHECK_THAT(s.at(1.).z, WithinAbs(10., 1e-12));
        const BuildSchedule::Position p = s.at(0.5);
        CHECK(p.layer == 25);
        CHECK_THAT(p.progress, WithinAbs(0., 1e-9));
        CHECK_THAT(s.at(0.51).z, WithinAbs(5.1, 1e-9));
        double prev = -1.;
        for (int i = 0; i <= 200; ++i) {
            const double z = s.at(i / 200.).z;
            CHECK(z >= prev);
            prev = z;
        }
    }
    SECTION("several objects: their layers merged, a thick first layer takes one step")
    {
        const BuildSchedule s({ 0.3, 0.5, 0.7, 0.9, 0.3, 0.6, 0.9, 0.9, -1., 0.6 }, 0.);
        CHECK(s.layers() == 5);   // 0.3 0.5 0.6 0.7 0.9, deduplicated, nothing below the plate
        CHECK_THAT(s.height(), WithinAbs(0.9, 1e-12));
        const BuildSchedule::Position p = s.at(0.1);   // half way through the first of five layers
        CHECK(p.layer == 0);
        CHECK_THAT(p.z, WithinAbs(0.15, 1e-9));
    }
    SECTION("no layers known: an even sweep to the top")
    {
        const BuildSchedule s({}, 40.);
        CHECK(s.layers() == 1);
        CHECK_THAT(s.at(0.25).z, WithinAbs(10., 1e-9));
        CHECK(BuildSchedule().at(0.5).layer == -1);
    }
    SECTION("the video's own build-up over the motion")
    {
        VideoSettings v;
        v.duration      = 4.;
        v.hold_start    = 1.;
        v.build.enabled = true;
        CHECK(build_fraction(VideoSettings(), 1.) < 0.);
        CHECK_THAT(build_fraction(v, 0.5), WithinAbs(0., 1e-12));
        CHECK_THAT(build_fraction(v, 3.), WithinAbs(0.5, 1e-12));
        CHECK_THAT(build_fraction(v, 5.), WithinAbs(1., 1e-12));
        v.loop = true;
        v.hold_start = 0.;
        CHECK_FALSE(seamless_loop(v));   // a growing print never loops back
    }
}

TEST_CASE("Toolpath smoothness mapping (D13)", "[Showcase][Path][Toolpaths]")
{
    SECTION("0 is Preview's look: everything off")
    {
        for (Finish f : all_finishes()) {
            const ToolpathLook l = toolpath_look(f, 0.f);
            CHECK(l.smoothness == 0.f);
            CHECK(l.skin_overlap == 1.f);
            CHECK(l.skin_flatten == 0.f);
            CHECK(l.groove == 1.f);
            CHECK(l.ssao_skin == 1.f);
            CHECK(l.anisotropy == 0.f);
        }
    }
    SECTION("the finish defaults of design 2.6.3")
    {
        CHECK(default_toolpath_smoothness(Finish::Matte) == 0.5f);
        CHECK(default_toolpath_smoothness(Finish::Standard) == 0.6f);
        CHECK(default_toolpath_smoothness(Finish::Silk) == 0.8f);
        CHECK(default_toolpath_smoothness(Finish::Gloss) == 0.75f);
        CHECK(default_toolpath_smoothness(Finish::Transparent) == 0.7f);
        CHECK_THAT(toolpath_look(Finish::Matte, -1.f).skin_overlap, WithinAbs(1.10, 1e-5));
        CHECK_THAT(toolpath_look(Finish::Standard, -1.f).skin_overlap, WithinAbs(1.12, 1e-5));
        CHECK_THAT(toolpath_look(Finish::Silk, -1.f).skin_overlap, WithinAbs(1.18, 1e-5));
        CHECK_THAT(toolpath_look(Finish::Gloss, -1.f).skin_overlap, WithinAbs(1.15, 1e-5));
        CHECK(toolpath_look(Finish::Silk, -1.f).anisotropy > 0.5f);
        CHECK(toolpath_look(Finish::Standard, -1.f).anisotropy == 0.f);
        // Matte keeps more of its grooves than gloss.
        CHECK(toolpath_look(Finish::Matte, -1.f).groove > toolpath_look(Finish::Gloss, -1.f).groove);
    }
    SECTION("more smoothness, smoother: monotonic and in range")
    {
        for (Finish f : all_finishes()) {
            ToolpathLook prev = toolpath_look(f, 0.f);
            for (int i = 1; i <= 20; ++i) {
                const ToolpathLook l = toolpath_look(f, i / 20.f);
                CHECK(l.skin_overlap >= prev.skin_overlap);
                CHECK(l.skin_flatten >= prev.skin_flatten);
                CHECK(l.groove <= prev.groove);
                CHECK(l.ssao_skin <= prev.ssao_skin);
                CHECK(l.bulge >= 2.f);
                CHECK(l.skin_overlap <= 1.3f);
                CHECK(l.ssao_skin >= 0.f);
                prev = l;
            }
        }
        CHECK(toolpath_look(Finish::Standard, 7.f).smoothness == 1.f);
    }
    SECTION("the scene keeps the slider; an S4 scene reads as Auto")
    {
        Project p;
        Scene  &s                       = p.ensure_scene_for_plate(0);
        s.source                        = "toolpaths";
        s.materials.toolpath_smoothness = 0.35f;
        Project back;
        REQUIRE(scenes_from_json(scenes_to_json(p), back));
        CHECK(back.scenes[0].source == "toolpaths");
        CHECK(back.scenes[0].materials.toolpath_smoothness == 0.35f);
        Project plain;
        plain.ensure_scene_for_plate(0);
        const json j = json::parse(scenes_to_json(plain));
        CHECK_FALSE(j["scenes"][0]["materials"].contains("toolpath_smoothness"));
        Project back2;
        REQUIRE(scenes_from_json(j.dump(), back2));
        CHECK(back2.scenes[0].materials.toolpath_smoothness < 0.f);
    }
    SECTION("the coextruded look is on by default and written only when off (multi-colour P2)")
    {
        Project plain;
        plain.ensure_scene_for_plate(0);
        CHECK(plain.scenes[0].materials.coextruded_look);
        const json j = json::parse(scenes_to_json(plain));
        CHECK_FALSE(j["scenes"][0]["materials"].contains("coextruded_look"));
        Project off;
        off.ensure_scene_for_plate(0).materials.coextruded_look = false;
        Project back;
        REQUIRE(scenes_from_json(scenes_to_json(off), back));
        CHECK_FALSE(back.scenes[0].materials.coextruded_look);
    }
}

TEST_CASE("S5 video settings in the scene; a video exported as a path", "[Showcase][Path]")
{
    const PathContext ctx = sample_context();
    SECTION("glide, spin, the build-up and events round-trip; an S4 video writes no new keys")
    {
        Project p;
        Scene  &s = p.ensure_scene_for_plate(0);
        Keyframe a, b;
        a.time  = 0.;
        b.time  = 2.;
        b.eye   = { 10., -100., 40. };
        b.spin  = 90.;
        b.glide = true;
        Keyframe c = b;
        c.time  = 3.;
        c.glide = false;
        s.video.path          = VideoPath::Keyframes;
        s.video.keyframes     = { a, b, c };
        s.video.build.enabled = true;
        s.video.build.to      = 0.8;
        s.video.build.source  = BuildSource::Clip;
        s.video.events        = "[{\"at\":1.0,\"rig\":\"sun\",\"type\":\"lighting\"}]";
        s.video.path_name     = "Mine";
        s.view_frame_height   = 812;
        Project back;
        REQUIRE(scenes_from_json(scenes_to_json(p), back));
        CHECK(back.scenes[0].video == s.video);
        CHECK(back.scenes[0].view_frame_height == 812);
        CHECK(back.scenes[0] == s);

        Project plain;
        plain.ensure_scene_for_plate(0).video.keyframes = { a };
        const json j = json::parse(scenes_to_json(plain));
        const json &v = j["scenes"][0]["video"];
        CHECK_FALSE(v.contains("build"));
        CHECK_FALSE(v.contains("events"));
        CHECK_FALSE(v.contains("path_name"));
        CHECK_FALSE(v["keyframes"][0].contains("glide"));
        CHECK_FALSE(v["keyframes"][0].contains("spin"));
        CHECK_FALSE(j["scenes"][0].contains("view_frame_height"));
    }
    SECTION("a keyframe video exports and compiles back to the same keys")
    {
        PathIssues         issues;
        const CompiledPath p = compile_text(doc_with("\"moves\": [{\"op\": \"orbit\", \"deg\": 60, \"duration\": 2},"
                                                     " {\"op\": \"dolly_in\", \"factor\": 1.5, \"duration\": 1.5}]"),
                                             ctx, issues);
        VideoSettings v;
        apply_compiled_path(p, v);
        const PathDocument doc  = export_path(v, ctx, "Round trip");
        const std::string  text = path_to_json(doc);
        PathDocument       again;
        PathIssues         issues2;
        REQUIRE(parse_path(text, again, issues2));
        CHECK(issues2.items().empty());
        CompiledPath p2;
        REQUIRE(compile_path(again, ctx, p2, issues2));
        REQUIRE(p2.keys.size() == p.keys.size());
        for (size_t i = 0; i < p.keys.size(); ++i) {
            CHECK(dist(p2.keys[i].eye, p.keys[i].eye) < 1e-3);
            CHECK(dist(p2.keys[i].target, p.keys[i].target) < 1e-3);
            CHECK_THAT(p2.keys[i].zoom, WithinAbs(p.keys[i].zoom, 1e-4 * p.keys[i].zoom));
            CHECK(p2.keys[i].glide == p.keys[i].glide);
            CHECK(p2.keys[i].ease == p.keys[i].ease);
        }
        CHECK_THAT(p2.duration, WithinAbs(p.duration, 1e-6));
    }
    SECTION("an orbit video exports as an orbit move from its start")
    {
        VideoSettings v;
        v.path      = VideoPath::Turntable;
        v.sweep_deg = 180.;
        v.clockwise = true;
        v.duration  = 6.;
        v.hold_end  = 1.;
        v.easing    = Easing::EaseInOut;
        v.build.enabled = true;
        const PathDocument doc = export_path(v, ctx, "TT");
        REQUIRE(doc.moves.size() == 2);
        CHECK(doc.moves[0].op == PathMoveOp::Turntable);
        CHECK(doc.moves[0].deg == -180.);
        CHECK(doc.moves[1].op == PathMoveOp::Wait);
        CHECK(doc.start == PathStart::Pose);
        REQUIRE(doc.events.size() == 1);
        CHECK(doc.events[0].type == PathEventType::Build);
        CHECK(doc.events[0].to == 6.);
        // It compiles to the same motion as the scene's own turntable.
        PathIssues   issues;
        CompiledPath p;
        REQUIRE(compile_path(doc, ctx, p, issues));
        CHECK_THAT(p.duration, WithinAbs(7., 1e-9));
        for (double t : { 0., 1.5, 3., 4.5, 6. }) {
            const CameraPose want = evaluate_path(v, ctx.start, t);
            const CameraPose got  = evaluate_keyframes(p.keys, t, ctx.start);
            CHECK(dist(want.eye, got.eye) < 0.02 * dist(ctx.start.eye, ctx.start.target));
            CHECK_THAT(got.spin_deg, WithinAbs(want.spin_deg, 1.5));
        }
    }
}

TEST_CASE("The shipped example paths parse and compile", "[Showcase][Path]")
{
    // TEST_DATA_DIR is <source>/tests/data.
    const fs::path dir = fs::path(TEST_DATA_DIR).parent_path().parent_path() / "resources" / "showcase" / "paths";
    if (!fs::is_directory(dir)) {
        WARN("no resources/showcase/paths next to the tests: " << dir.string());
        return;
    }
    int count = 0;
    for (const fs::directory_entry &e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".json")
            continue;
        ++count;
        boost::nowide::ifstream f(e.path().string());
        std::stringstream       ss;
        ss << f.rdbuf();
        PathDocument doc;
        PathIssues   issues;
        INFO(e.path().filename().string() << "\n" << issues.text());
        REQUIRE(parse_path(ss.str(), doc, issues));
        CHECK(issues.items().empty());
        CompiledPath p;
        PathContext  ctx = sample_context();
        REQUIRE(compile_path(doc, ctx, p, issues));
        CHECK(p.duration <= MAX_VIDEO_SECONDS);
        CHECK(int(p.keys.size()) <= MAX_KEYFRAMES);
    }
    CHECK(count >= 2);
}

TEST_CASE("S5b: the build-up may leave out infill and supports", "[Showcase][Path][Build]")
{
    SECTION("both on by default, as S5 laid every extrusion; an S5 scene writes no new keys")
    {
        const BuildUp b;
        CHECK(b.infill);
        CHECK(b.supports);
        Project p;
        p.ensure_scene_for_plate(0).video.build.enabled = true;
        const json j = json::parse(scenes_to_json(p));
        REQUIRE(j["scenes"][0]["video"].contains("build"));
        CHECK_FALSE(j["scenes"][0]["video"]["build"].contains("infill"));
        CHECK_FALSE(j["scenes"][0]["video"]["build"].contains("supports"));
    }
    SECTION("the scene keeps them")
    {
        Project p;
        Scene  &s             = p.ensure_scene_for_plate(0);
        s.video.build.enabled  = true;
        s.video.build.infill   = false;
        s.video.build.supports = false;
        Project back;
        REQUIRE(scenes_from_json(scenes_to_json(p), back));
        CHECK(back.scenes[0].video.build == s.video.build);
        // ... and the timeline follows the scene.
        const TimelineState st = evaluate_timeline({}, back.scenes[0], 1., 0.5);
        CHECK_FALSE(st.build_infill);
        CHECK_FALSE(st.build_supports);
        // A path exported from it says so.
        s.video.duration = 4.;
        const PathDocument doc = export_path(s.video, sample_context(), "B");
        REQUIRE(doc.events.size() == 1);
        REQUIRE(doc.events[0].build_infill.has_value());
        CHECK_FALSE(*doc.events[0].build_infill);
        CHECK_FALSE(*doc.events[0].build_supports);
    }
    SECTION("a path's build event sets them; unset keeps the scene's")
    {
        std::vector<PathEvent> ev;
        PathIssues             issues;
        REQUIRE(parse_path_events("[{\"type\": \"build\", \"from\": 0, \"to\": 2, \"infill\": false}]", ev, issues));
        REQUIRE(ev.size() == 1);
        REQUIRE(ev[0].build_infill.has_value());
        CHECK_FALSE(*ev[0].build_infill);
        CHECK_FALSE(ev[0].build_supports.has_value());
        const Scene         base = sample_scene();
        const TimelineState st   = evaluate_timeline(ev, base, 1.);
        CHECK_FALSE(st.build_infill);
        CHECK(st.build_supports);
        // Written back as read.
        std::vector<PathEvent> again;
        REQUIRE(parse_path_events(path_events_to_json(ev), again, issues));
        REQUIRE(again.size() == 1);
        CHECK(again[0].build_infill == ev[0].build_infill);
        CHECK_FALSE(again[0].build_supports.has_value());
        // Not a flag: an error.
        std::vector<PathEvent> bad;
        PathIssues             bad_issues;
        parse_path_events("[{\"type\": \"build\", \"from\": 0, \"to\": 2, \"supports\": \"some\"}]", bad, bad_issues);
        CHECK(bad_issues.has_errors());
    }
}

TEST_CASE("S5c: static camera paths and the video's callout options", "[Showcase][Path]")
{
    const PathContext ctx = sample_context();
    SECTION("Static and Static with zoom out hold the scene's camera") {
        for (VideoPath path : { VideoPath::Static, VideoPath::StaticZoomOut }) {
            VideoSettings v;
            v.path = path;
            CHECK(is_static_path(v.path));
            CHECK_FALSE(seamless_loop(v));
            for (double t : { 0., 2.5, 8. }) {
                const CameraPose p = evaluate_path(v, ctx.start, t);
                CHECK(dist(p.eye, ctx.start.eye) < 1e-9);
                CHECK(dist(p.target, ctx.start.target) < 1e-9);
                CHECK_THAT(p.zoom, WithinAbs(ctx.start.zoom, 1e-12));
            }
            VideoPath back;
            REQUIRE(from_string(to_string(path), back));
            CHECK(back == path);
        }
    }
    SECTION("zooming out on the build-up keeps the angle and ends on the whole model") {
        CameraPose base;
        base.eye    = { 100., -150., 120. };
        base.target = { 0., 0., 20. };
        base.zoom   = 2.;
        const std::array<double, 3> lo{ { -20., -20., 0. } }, hi{ { 20., 20., 40. } };
        const CameraPose done = frame_built_part(base, lo, hi, 40.);
        CHECK(dist(done.eye, base.eye) < 1e-9);
        CHECK_THAT(done.zoom, WithinAbs(2., 1e-12));
        const CameraPose part = frame_built_part(base, lo, hi, 10.);
        CHECK(part.zoom > base.zoom);
        CHECK(part.zoom <= 4. * base.zoom + 1e-9);
        CHECK(part.target[2] < base.target[2]);
        // The same angle: the view direction does not change.
        for (int i = 0; i < 3; ++i)
            CHECK_THAT(part.eye[i] - part.target[i], WithinAbs(base.eye[i] - base.target[i], 1e-9));
        // The zoom grows as less is built, never past the cap.
        CHECK(frame_built_part(base, lo, hi, 5.).zoom >= part.zoom);
        CHECK(frame_built_part(base, lo, hi, 0.).zoom <= 4. * base.zoom + 1e-9);
    }
    SECTION("the options round-trip; an S5 video writes no new keys; a static zoom-out exports a following build") {
        Project p;
        Scene  &s                   = p.ensure_scene_for_plate(0);
        s.video.path                = VideoPath::StaticZoomOut;
        s.video.callout_motion      = CalloutMotion::Static;
        s.video.callout_appearance  = CalloutAppearance::AsUsed;
        s.video.build.enabled       = true;
        Project back;
        REQUIRE(scenes_from_json(scenes_to_json(p), back));
        CHECK(back.scenes[0].video == s.video);

        Project plain;
        plain.ensure_scene_for_plate(0);
        const json j = json::parse(scenes_to_json(plain));
        CHECK_FALSE(j["scenes"][0]["video"].contains("callout_motion"));
        CHECK_FALSE(j["scenes"][0]["video"].contains("callout_appearance"));

        const PathDocument doc = export_path(s.video, ctx, "Z");
        REQUIRE(doc.moves.size() == 1);
        CHECK(doc.moves[0].op == PathMoveOp::Wait);
        REQUIRE(doc.events.size() == 1);
        CHECK(doc.events[0].build_follow);
        std::vector<PathEvent> ev;
        PathIssues             issues;
        REQUIRE(parse_path_events(path_events_to_json(doc.events), ev, issues));
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].build_follow);
        CHECK(evaluate_timeline(ev, s, 1.).build_follow);
    }
}

TEST_CASE("Path timeline: a floor event turns and mirrors the picture", "[Showcase][Path]")
{
    const Scene base = sample_scene();
    auto events_of = [](const std::string &arr) {
        std::vector<PathEvent> ev;
        PathIssues             issues;
        const bool             ok = parse_path_events(arr, ev, issues);
        INFO(issues.text());
        REQUIRE(ok);
        return ev;
    };
    Scene s       = base;
    s.floor.type  = FloorType::Image;
    const auto ev = events_of("[{\"type\": \"floor\", \"at\": 1, \"rotation\": 200, \"flip\": true}]");
    CHECK(evaluate_timeline(ev, s, 0.).scene.floor.rotation == 0);
    const Floor f = evaluate_timeline(ev, s, 2.).scene.floor;
    CHECK(f.rotation == 180);
    CHECK(f.flip);
    CHECK(path_events_to_json(ev).find("\"rotation\"") != std::string::npos);
}
