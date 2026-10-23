// Showcase tab, phase S4: the videographer (src/libslic3r/Showcase/ShowcaseVideo.{hpp,cpp} and
// ShowcaseEncoder.{hpp,cpp}) - the camera paths (orbit, turntable, bob, keyframe splines), easing,
// timing and seamless loops, the video size and bitrate, the ffmpeg command lines and the H.264
// fallback order, the frame queue, the encoder thread and cancelling, the PNG sequence, the callouts'
// occlusion fade, S4's callout layout guarantees (bands, side hysteresis, pinned labels and
// multileader legs that never cross), the saved callout styles, and the video in the scene's JSON and
// the project 3MF (S1-S3 scenes included, nothing in a Bambu export).

#include <catch2/catch.hpp>

#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Showcase/ShowcaseEncoder.hpp"
#include "libslic3r/Showcase/ShowcaseScene.hpp"
#include "libslic3r/Showcase/ShowcaseVideo.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <numeric>
#include <random>
#include <thread>

using namespace Slic3r;
using namespace Slic3r::Showcase;
using Catch::Matchers::WithinAbs;
using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

constexpr double DEG = 3.14159265358979323846 / 180.;

CameraView base_camera()
{
    CameraView c;
    c.eye    = { 100., 0., 50. };
    c.target = { 0., 0., 0. };
    c.up     = { 0., 0., 1. };
    c.zoom   = 1.5;
    c.valid  = true;
    return c;
}

double dist(const std::array<double, 3> &a, const std::array<double, 3> &b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

double azimuth_deg(const CameraPose &p)
{
    double a = std::atan2(p.eye[1] - p.target[1], p.eye[0] - p.target[0]) / DEG;
    return a < 0. ? a + 360. : a;
}

double elevation_deg(const CameraPose &p)
{
    const double dx = p.eye[0] - p.target[0], dy = p.eye[1] - p.target[1], dz = p.eye[2] - p.target[2];
    return std::atan2(dz, std::hypot(dx, dy)) / DEG;
}

Keyframe key(double t, std::array<double, 3> eye, std::array<double, 3> target = { 0., 0., 0. }, Easing e = Easing::Linear, double zoom = 1.)
{
    Keyframe k;
    k.time   = t;
    k.eye    = eye;
    k.target = target;
    k.ease   = e;
    k.zoom   = zoom;
    return k;
}

fs::path scratch_dir(const std::string &name)
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("snorca_video_" + name + "_%%%%-%%%%");
    fs::create_directories(dir);
    return dir;
}

bool has_arg(const std::vector<std::string> &args, const std::string &a, const std::string &value = std::string())
{
    for (size_t i = 0; i < args.size(); ++i)
        if (args[i] == a && (value.empty() || (i + 1 < args.size() && args[i + 1] == value)))
            return true;
    return false;
}

std::string arg_after(const std::vector<std::string> &args, const std::string &a)
{
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == a)
            return args[i + 1];
    return {};
}

// A sink that records what it is given and can be told to be slow or to fail.
struct FakeSink : public FrameSink
{
    struct Log
    {
        std::atomic<int>  opened{ 0 }, finished{ 0 }, aborted{ 0 }, interrupted{ 0 };
        std::vector<int>  indices;
        std::mutex        mutex;
    };
    std::shared_ptr<Log> log;
    int                  delay_ms = 0;
    int                  fail_at  = -1;

    explicit FakeSink(std::shared_ptr<Log> l) : log(std::move(l)) {}
    bool open(std::string &) override { ++log->opened; return true; }
    bool write(const VideoFrame &f, std::string &error) override
    {
        if (delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        if (f.index == fail_at) {
            error = "fake failure";
            return false;
        }
        std::lock_guard<std::mutex> lock(log->mutex);
        log->indices.push_back(f.index);
        return true;
    }
    bool        finish(std::string &) override { ++log->finished; return true; }
    void        interrupt() override { ++log->interrupted; }
    void        abort() override { ++log->aborted; }
    std::string description() const override { return "fake"; }
};

VideoFrame make_frame(int index, int w = 4, int h = 2)
{
    VideoFrame f;
    f.index  = index;
    f.width  = w;
    f.height = h;
    f.rgba.assign(size_t(w) * size_t(h) * 4, uint8_t(index));
    return f;
}

void submit_all(EncoderWorker &worker, int n)
{
    for (int i = 0; i < n; ++i) {
        VideoFrame f = make_frame(i);
        while (!worker.try_submit(f)) {
            if (worker.failed() || worker.done())
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

std::string zip_entry(const std::string &zip_path, const std::string &entry_name)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return {};
    size_t      size = 0;
    void       *data = mz_zip_reader_extract_file_to_heap(&archive, entry_name.c_str(), &size, 0);
    std::string result;
    if (data != nullptr) {
        result.assign(static_cast<const char *>(data), size);
        mz_free(data);
    }
    close_zip_reader(&archive);
    return result;
}

DynamicPrintConfig project_config()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string &k : cfg.keys())
        if (const ConfigOption *opt = cfg.option(k); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(k);
            cfg.option(k, true);
        }
    cfg.set_num_extruders(1);
    cfg.set_num_filaments(1);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values  = { 0.4 };
    cfg.option<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab P1S"));
    return cfg;
}

VideoSettings sample_video()
{
    VideoSettings v;
    v.path       = VideoPath::Keyframes;
    v.duration   = 6.5;
    v.fps        = 60;
    v.resolution = 1440;
    v.quality    = Quality::Best;
    v.bitrate    = 85;
    v.format     = VideoFormat::WebMAlpha;
    v.easing     = Easing::EaseOut;
    v.sweep_deg  = 180.;
    v.clockwise  = true;
    v.bob_deg    = 22.;
    v.bob_cycles = 3;
    v.hold_start = 0.5;
    v.hold_end   = 1.5;
    v.loop       = false;
    v.callouts   = false;
    v.keyframes  = { key(0., { 100., 0., 40. }, { 0., 0., 10. }, Easing::EaseIn, 1.2), key(3.25, { 0., 120., 60. }, { 5., 0., 10. }, Easing::EaseInOut, 2.) };
    v.keyframes[1].up = { 0., 1., 0. };
    return v;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Easing and timing
// ------------------------------------------------------------------------------------------------

TEST_CASE("Video easing curves", "[Showcase][Video]")
{
    for (Easing e : { Easing::Linear, Easing::EaseInOut, Easing::EaseIn, Easing::EaseOut }) {
        DYNAMIC_SECTION("easing " << to_string(e))
        {
            CHECK_THAT(ease(e, 0.), WithinAbs(0., 1e-12));
            CHECK_THAT(ease(e, 1.), WithinAbs(1., 1e-12));
            CHECK_THAT(ease(e, -3.), WithinAbs(0., 1e-12));   // clamped
            CHECK_THAT(ease(e, 7.), WithinAbs(1., 1e-12));
            double prev = 0.;
            for (int i = 1; i <= 100; ++i) {
                const double v = ease(e, i / 100.);
                CHECK(v >= prev - 1e-12);
                prev = v;
            }
        }
    }
    // In-out is symmetric and slow at both ends; in is below the line, out above.
    for (double u : { 0.1, 0.25, 0.4 }) {
        CHECK_THAT(ease(Easing::EaseInOut, u) + ease(Easing::EaseInOut, 1. - u), WithinAbs(1., 1e-12));
        CHECK(ease(Easing::EaseIn, u) < u);
        CHECK(ease(Easing::EaseOut, u) > u);
    }
    CHECK(ease(Easing::EaseInOut, 0.01) < 0.01);
    CHECK_THAT(ease(Easing::EaseInOut, 0.5), WithinAbs(0.5, 1e-12));
    CHECK_THAT(ease(Easing::Linear, 0.3), WithinAbs(0.3, 1e-12));
}

TEST_CASE("Video timing: frames, holds and seamless loops", "[Showcase][Video]")
{
    VideoSettings v;
    v.duration = 8.;
    v.fps      = 30;
    SECTION("a full-turn orbit loops: frame N would be frame 0")
    {
        REQUIRE(seamless_loop(v));
        REQUIRE(total_frames(v) == 240);
        CHECK_THAT(frame_time(v, 0), WithinAbs(0., 1e-12));
        CHECK_THAT(frame_time(v, 239), WithinAbs(239. / 30., 1e-9));
        const CameraPose first = pose_at_frame(v, base_camera(), 0);
        const CameraPose last  = pose_at_frame(v, base_camera(), 239);
        const CameraPose wrap  = evaluate_path(v, base_camera(), 8.);   // where frame 240 would be
        CHECK(dist(first.eye, last.eye) > 1.);                         // not a duplicate of the first
        CHECK_THAT(dist(first.eye, wrap.eye), WithinAbs(0., 1e-6));
        // Even steps all the way round, the seam included.
        const double step = dist(pose_at_frame(v, base_camera(), 0).eye, pose_at_frame(v, base_camera(), 1).eye);
        CHECK_THAT(dist(last.eye, wrap.eye), WithinAbs(step, 1e-6));
    }
    SECTION("two whole turns loop too, half a turn does not")
    {
        v.sweep_deg = 720.;
        CHECK(seamless_loop(v));
        v.sweep_deg = 180.;
        CHECK_FALSE(seamless_loop(v));
        CHECK_THAT(frame_time(v, total_frames(v) - 1), WithinAbs(8., 1e-9));   // the end is reached exactly
        CHECK_THAT(azimuth_deg(pose_at_frame(v, base_camera(), total_frames(v) - 1)), WithinAbs(180., 1e-6));
    }
    SECTION("loop off, keyframes or holds: the last frame is the end")
    {
        v.loop = false;
        CHECK_FALSE(seamless_loop(v));
        v.loop       = true;
        v.hold_start = 1.;
        v.hold_end   = 0.5;
        CHECK_FALSE(seamless_loop(v));
        CHECK_THAT(total_seconds(v), WithinAbs(9.5, 1e-12));
        CHECK(total_frames(v) == 285);
        CHECK_THAT(frame_time(v, 284), WithinAbs(9.5, 1e-9));
        // Standing still in the holds.
        CHECK_THAT(motion_fraction(v, 0.5), WithinAbs(0., 1e-12));
        CHECK_THAT(motion_fraction(v, 1.), WithinAbs(0., 1e-12));
        CHECK_THAT(motion_fraction(v, 5.), WithinAbs(0.5, 1e-12));
        CHECK_THAT(motion_fraction(v, 9.2), WithinAbs(1., 1e-12));
        v.hold_start = v.hold_end = 0.;
        v.path       = VideoPath::Keyframes;
        CHECK_FALSE(seamless_loop(v));
    }
    SECTION("frame numbers outside the video clamp")
    {
        CHECK_THAT(frame_time(v, -5), WithinAbs(0., 1e-12));
        CHECK_THAT(frame_time(v, 100000), WithinAbs(frame_time(v, total_frames(v) - 1), 1e-12));
    }
}

// ------------------------------------------------------------------------------------------------
// Paths
// ------------------------------------------------------------------------------------------------

TEST_CASE("Video paths: orbit, turntable and bob", "[Showcase][Video]")
{
    const CameraView base = base_camera();
    VideoSettings    v;
    v.duration = 4.;
    v.fps      = 25;
    const double r      = dist(base.eye, base.target);
    const double elev0  = std::atan2(50., 100.) / DEG;

    SECTION("orbit: round the target at the camera's distance and height, counter-clockwise")
    {
        v.path = VideoPath::Orbit;
        const CameraPose start = evaluate_path(v, base, 0.);
        CHECK_THAT(dist(start.eye, base.eye), WithinAbs(0., 1e-9));
        CHECK_THAT(start.zoom, WithinAbs(1.5, 1e-12));
        const CameraPose quarter = evaluate_path(v, base, 1.);
        CHECK_THAT(azimuth_deg(quarter), WithinAbs(90., 1e-6));
        CHECK_THAT(dist(quarter.eye, quarter.target), WithinAbs(r, 1e-9));
        CHECK_THAT(elevation_deg(quarter), WithinAbs(elev0, 1e-9));
        CHECK_THAT(dist(quarter.target, base.target), WithinAbs(0., 1e-12));
        CHECK_THAT(quarter.spin_deg, WithinAbs(0., 1e-12));
        CHECK_THAT(quarter.up[2], WithinAbs(std::cos(elev0 * DEG), 1e-6));   // level horizon: up is Z made square to the view
        v.clockwise = true;
        CHECK_THAT(azimuth_deg(evaluate_path(v, base, 1.)), WithinAbs(270., 1e-6));
    }
    SECTION("orbit with easing: slow at the ends, the same end")
    {
        v.path      = VideoPath::Orbit;
        v.sweep_deg = 90.;
        v.loop      = false;
        v.easing    = Easing::EaseInOut;
        CHECK(azimuth_deg(evaluate_path(v, base, 0.4)) < 0.1 * 90.);
        CHECK_THAT(azimuth_deg(evaluate_path(v, base, 2.)), WithinAbs(45., 1e-6));
        CHECK_THAT(azimuth_deg(evaluate_path(v, base, 4.)), WithinAbs(90., 1e-6));
    }
    SECTION("turntable: the model spins, the camera goes the other way round")
    {
        v.path = VideoPath::Turntable;
        const CameraPose p = evaluate_path(v, base, 1.);
        CHECK_THAT(p.spin_deg, WithinAbs(90., 1e-9));
        CHECK_THAT(azimuth_deg(p), WithinAbs(270., 1e-6));
        CHECK_THAT(dist(p.eye, p.target), WithinAbs(r, 1e-9));
        CHECK_THAT(elevation_deg(p), WithinAbs(elev0, 1e-9));
        v.clockwise = true;
        CHECK_THAT(evaluate_path(v, base, 1.).spin_deg, WithinAbs(-90., 1e-9));
    }
    SECTION("bob: up and down by the amplitude, as many times as asked")
    {
        v.path       = VideoPath::OrbitBob;
        v.bob_deg    = 15.;
        v.bob_cycles = 2;
        CHECK_THAT(elevation_deg(evaluate_path(v, base, 0.)), WithinAbs(elev0, 1e-9));
        CHECK_THAT(elevation_deg(evaluate_path(v, base, 0.5)), WithinAbs(elev0 + 15., 1e-6));   // a quarter of the first bob
        CHECK_THAT(elevation_deg(evaluate_path(v, base, 1.)), WithinAbs(elev0, 1e-6));
        CHECK_THAT(elevation_deg(evaluate_path(v, base, 1.5)), WithinAbs(elev0 - 15., 1e-6));
        CHECK_THAT(dist(evaluate_path(v, base, 0.5).eye, base.target), WithinAbs(r, 1e-9));
    }
    SECTION("never over the pole: the elevation stops short of straight down")
    {
        CameraView top = base;
        top.eye        = { 0.001, 0., 300. };
        v.path         = VideoPath::OrbitBob;
        v.bob_deg      = 60.;
        for (int f = 0; f < total_frames(v); f += 7) {
            const CameraPose p = pose_at_frame(v, top, f);
            CHECK(std::abs(elevation_deg(p)) <= 89. + 1e-9);
            CHECK(std::isfinite(p.up[0]));
        }
    }
}

TEST_CASE("Video paths: keyframe splines", "[Showcase][Video]")
{
    const CameraView fallback = base_camera();
    SECTION("no key: the scene's camera; one key: that key")
    {
        CHECK_THAT(dist(evaluate_keyframes({}, 1., fallback).eye, fallback.eye), WithinAbs(0., 1e-12));
        const std::vector<Keyframe> one = { key(2., { 10., 20., 30. }) };
        CHECK_THAT(dist(evaluate_keyframes(one, 0., fallback).eye, one[0].eye), WithinAbs(0., 1e-12));
        CHECK_THAT(dist(evaluate_keyframes(one, 9., fallback).eye, one[0].eye), WithinAbs(0., 1e-12));
    }
    SECTION("through every key at its time, clamped outside")
    {
        const std::vector<Keyframe> keys = { key(0., { 100., 0., 30. }), key(2., { 0., 100., 50. }, { 0., 0., 10. }), key(5., { -100., 0., 20. }),
                                             key(6., { 0., -80., 40. }) };
        for (const Keyframe &k : keys) {
            const CameraPose p = evaluate_keyframes(keys, k.time, fallback);
            CHECK_THAT(dist(p.eye, k.eye), WithinAbs(0., 1e-6));
            CHECK_THAT(dist(p.target, k.target), WithinAbs(0., 1e-6));
        }
        CHECK_THAT(dist(evaluate_keyframes(keys, -1., fallback).eye, keys.front().eye), WithinAbs(0., 1e-12));
        CHECK_THAT(dist(evaluate_keyframes(keys, 99., fallback).eye, keys.back().eye), WithinAbs(0., 1e-12));
        // Continuous: small steps in time are small steps in space, across the keys too.
        std::array<double, 3> prev = evaluate_keyframes(keys, 0., fallback).eye;
        for (int i = 1; i <= 600; ++i) {
            const std::array<double, 3> e = evaluate_keyframes(keys, i * 0.01, fallback).eye;
            CHECK(dist(prev, e) < 5.);
            prev = e;
        }
    }
    SECTION("even speed within a segment, whatever the key spacing (arc length)")
    {
        // Keys along a line at uneven spacing: a plain Catmull-Rom parameter would speed up and slow
        // down; re-parameterised, every step of time covers the same distance.
        const std::vector<Keyframe> keys = { key(0., { 0., -200., 50. }), key(1., { 10., -200., 50. }), key(2., { 200., -200., 50. }),
                                             key(3., { 210., -200., 50. }) };
        std::vector<double> steps;
        for (int i = 0; i < 20; ++i)
            steps.push_back(dist(evaluate_keyframes(keys, 1. + i * 0.05, fallback).eye, evaluate_keyframes(keys, 1. + (i + 1) * 0.05, fallback).eye));
        const double mean = std::accumulate(steps.begin(), steps.end(), 0.) / steps.size();
        for (double s : steps)
            CHECK_THAT(s, WithinAbs(mean, 0.05 * mean));
    }
    SECTION("the arriving key's ease shapes its segment")
    {
        const std::vector<Keyframe> linear = { key(0., { 0., -200., 50. }), key(2., { 100., -200., 50. }, { 0., 0., 0. }, Easing::Linear) };
        const std::vector<Keyframe> eased  = { key(0., { 0., -200., 50. }), key(2., { 100., -200., 50. }, { 0., 0., 0. }, Easing::EaseIn) };
        CHECK_THAT(evaluate_keyframes(linear, 1., fallback).eye[0], WithinAbs(50., 0.5));
        CHECK_THAT(evaluate_keyframes(eased, 1., fallback).eye[0], WithinAbs(25., 0.5));
    }
    SECTION("zoom is interpolated geometrically, the roll by slerp")
    {
        std::vector<Keyframe> keys = { key(0., { 100., 0., 0.1 }, { 0., 0., 0. }, Easing::Linear, 1.), key(1., { 100., 0., 0.1 }, { 0., 0., 0. }, Easing::Linear, 4.) };
        keys[1].up = { 0., 1., 0. };
        const CameraPose mid = evaluate_keyframes(keys, 0.5, fallback);
        CHECK_THAT(mid.zoom, WithinAbs(2., 1e-6));
        // Halfway between Z and Y: 45 degrees of roll (still square to the view along -X).
        CHECK_THAT(mid.up[1], WithinAbs(std::sqrt(0.5), 1e-3));
        CHECK_THAT(mid.up[2], WithinAbs(std::sqrt(0.5), 1e-3));
    }
    SECTION("Catmull-Rom and slerp ends")
    {
        const std::array<double, 3> p0{ 0., 0., 0. }, p1{ 1., 0., 0. }, p2{ 2., 0., 0. }, p3{ 3., 0., 0. };
        CHECK_THAT(dist(catmull_rom(p0, p1, p2, p3, 0.), p1), WithinAbs(0., 1e-12));
        CHECK_THAT(dist(catmull_rom(p0, p1, p2, p3, 1.), p2), WithinAbs(0., 1e-12));
        CHECK_THAT(catmull_rom(p0, p1, p2, p3, 0.5)[0], WithinAbs(1.5, 1e-9));
        // Coincident neighbours (the phantom ends) do not divide by zero.
        const std::array<double, 3> c = catmull_rom(p1, p1, p2, p2, 0.5);
        CHECK(std::isfinite(c[0]));
        const std::array<double, 3> s = slerp_dir({ 1., 0., 0. }, { -1., 0., 0. }, 0.5);   // opposite directions
        CHECK_THAT(std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]), WithinAbs(1., 1e-9));
    }
    SECTION("keys are inserted in time order; the same time replaces")
    {
        std::vector<Keyframe> keys;
        CHECK(insert_keyframe(keys, key(2., { 1., 0., 0. })) == 0);
        CHECK(insert_keyframe(keys, key(0.5, { 2., 0., 0. })) == 0);
        CHECK(insert_keyframe(keys, key(4., { 3., 0., 0. })) == 2);
        CHECK(insert_keyframe(keys, key(2., { 9., 0., 0. })) == 1);
        REQUIRE(keys.size() == 3);
        CHECK(keys[1].eye[0] == 9.);
        for (int i = 0; int(keys.size()) < MAX_KEYFRAMES; ++i)
            insert_keyframe(keys, key(10. + i, { 1., 1., 1. }));
        CHECK(insert_keyframe(keys, key(59.5, { 1., 1., 1. })) == -1);
        CHECK(keys.size() == size_t(MAX_KEYFRAMES));
    }
}

// ------------------------------------------------------------------------------------------------
// Size, bitrate, ffmpeg
// ------------------------------------------------------------------------------------------------

TEST_CASE("Video size and bitrate", "[Showcase][Video]")
{
    int w = 0, h = 0;
    video_size(1080, 1920, 1080, w, h);
    CHECK(w == 1920);
    CHECK(h == 1080);
    video_size(1080, 1080, 1920, w, h);
    CHECK(w == 1080);
    CHECK(h == 1920);
    video_size(1080, 1080, 1350, w, h);
    CHECK(w == 1080);
    CHECK(h == 1350);
    video_size(2160, 16, 9, w, h);
    CHECK(w == 3840);
    CHECK(h == 2160);
    video_size(1080, 1000, 333, w, h);   // an odd side rounds to even
    CHECK(w % 2 == 0);
    CHECK(h % 2 == 0);
    video_size(2160, 5000, 1000, w, h);   // capped at the export limit
    CHECK(w <= MAX_EXPORT_SIDE);
    CHECK(w % 2 == 0);
    video_size(720, 0, 0, w, h);          // no aspect: 16:9
    CHECK(w == 1280);
    CHECK(h == 720);

    // Design 4.2: 12 Mb/s at 1080p30, 40 Mb/s at 4K30, more for a higher quality.
    CHECK(std::abs(video_bitrate_kbps(1920, 1080, 30, 60) - 12000) <= 1);
    CHECK(std::abs(video_bitrate_kbps(3840, 2160, 30, 60) - 40000) <= 1);
    CHECK(video_bitrate_kbps(1920, 1080, 30, 80) > video_bitrate_kbps(1920, 1080, 30, 60));
    CHECK(video_bitrate_kbps(1920, 1080, 60, 60) > video_bitrate_kbps(1920, 1080, 30, 60));
    CHECK(video_bitrate_kbps(16, 16, 1, 0) >= 300);
}

TEST_CASE("ffmpeg encoders: parsing and the fallback order (D5)", "[Showcase][Video]")
{
    const std::string listing = "Encoders:\n V..... = Video\n ------\n"
                                " V....D libopenh264          OpenH264 H.264 / AVC\n"
                                " V....D h264_amf             AMD AMF H.264 Encoder (codec h264)\r\n"
                                " V....D h264_mf              H264 via MediaFoundation (codec h264)\n"
                                " V....D h264_nvenc           NVIDIA NVENC H.264 encoder (codec h264)\n"
                                " V..... h264_qsv             H.264 (Intel Quick Sync Video acceleration)\n"
                                " V....D libvpx-vp9           libvpx VP9 (codec vp9)\n"
                                " V....D gif                  GIF (Graphics Interchange Format)\n"
                                " V....D libwebp_anim         libwebp WebP image (codec webp)\n"
                                " A....D aac                  AAC (Advanced Audio Coding)\n";
    const std::set<std::string> enc = parse_ffmpeg_encoders(listing);
    CHECK(enc.count("h264_mf") == 1);
    CHECK(enc.count("h264_amf") == 1);
    CHECK(enc.count("libvpx-vp9") == 1);
    CHECK(enc.count("aac") == 1);
    CHECK(enc.count("Video") == 0);   // the legend above the line is not an encoder
    CHECK(enc.count("libx264") == 0);

    // Windows: Media Foundation, then OpenH264, then the GPUs' encoders.
    CHECK(h264_encoder_order(enc, EncoderPlatform::Windows) == std::vector<std::string>{ "h264_mf", "libopenh264", "h264_nvenc", "h264_qsv", "h264_amf" });
    std::set<std::string> no_mf = enc;
    no_mf.erase("h264_mf");
    CHECK(h264_encoder_order(no_mf, EncoderPlatform::Windows).front() == "libopenh264");
    std::set<std::string> linux_enc = enc;
    linux_enc.insert("libx264");
    CHECK(h264_encoder_order(linux_enc, EncoderPlatform::Linux).front() == "libx264");
    std::set<std::string> mac_enc = { "libx264", "h264_videotoolbox" };
    CHECK(h264_encoder_order(mac_enc, EncoderPlatform::Mac) == std::vector<std::string>{ "h264_videotoolbox", "libx264" });
    CHECK(h264_encoder_order({}, EncoderPlatform::Windows).empty());

    // What an export can offer.
    EncoderCaps none;
    CHECK(available_formats(none, false) == std::vector<VideoFormat>{ VideoFormat::PNGSequence });
    CHECK(available_formats(none, true) == std::vector<VideoFormat>{ VideoFormat::MP4, VideoFormat::PNGSequence });
    EncoderCaps caps;
    caps.h264 = { "libopenh264" };
    caps.vp9  = true;
    caps.gif  = true;
    caps.webp = true;
    CHECK(available_formats(caps, false) == std::vector<VideoFormat>{ VideoFormat::MP4, VideoFormat::WebM, VideoFormat::WebMAlpha, VideoFormat::GIF,
                                                                      VideoFormat::WebP, VideoFormat::PNGSequence });
    caps.h264.clear();   // listed but none worked: no MP4 without a native encoder
    CHECK(available_formats(caps, false).front() == VideoFormat::WebM);
}

TEST_CASE("ffmpeg command lines", "[Showcase][Video]")
{
    FfmpegRequest r;
    r.ffmpeg  = "ffmpeg";
    r.output  = "C:/out dir/clip.mp4";
    r.width   = 1920;
    r.height  = 1080;
    r.fps     = 30;
    r.quality = 60;
    r.format  = VideoFormat::MP4;
    r.encoder = "h264_mf";

    SECTION("raw RGBA frames on stdin, the output last")
    {
        const std::vector<std::string> a = ffmpeg_args(r);
        CHECK(has_arg(a, "-f", "rawvideo"));
        CHECK(has_arg(a, "-pix_fmt", "rgba"));
        CHECK(has_arg(a, "-s", "1920x1080"));
        CHECK(has_arg(a, "-framerate", "30"));
        CHECK(has_arg(a, "-i", "-"));
        CHECK(has_arg(a, "-y"));
        CHECK(has_arg(a, "-an"));
        CHECK(a.back() == r.output);
        // The input options come before the input.
        const auto i_in = std::find(a.begin(), a.end(), "-i") - a.begin();
        const auto i_fmt = std::find(a.begin(), a.end(), "rawvideo") - a.begin();
        CHECK(i_fmt < i_in);
    }
    SECTION("MP4 through Media Foundation: NV12, a bitrate, faststart")
    {
        const std::vector<std::string> a = ffmpeg_args(r);
        CHECK(has_arg(a, "-c:v", "h264_mf"));
        CHECK(has_arg(a, "-b:v", "12000k"));
        CHECK(arg_after(a, "-pix_fmt") == "rgba");   // the input's
        CHECK(std::count(a.begin(), a.end(), "nv12") == 1);
        CHECK(has_arg(a, "-movflags", "+faststart"));
        CHECK(has_arg(a, "-f", "mp4"));
        CHECK(has_arg(a, "-colorspace", "bt709"));
        CHECK(has_arg(a, "-g", "60"));
    }
    SECTION("OpenH264 has no CRF: a bitrate and YUV 4:2:0")
    {
        r.encoder = "libopenh264";
        const std::vector<std::string> a = ffmpeg_args(r);
        CHECK(has_arg(a, "-c:v", "libopenh264"));
        CHECK(has_arg(a, "-b:v"));
        CHECK_FALSE(has_arg(a, "-crf"));
        CHECK(std::count(a.begin(), a.end(), "yuv420p") == 1);
    }
    SECTION("x264 by quality, NVENC and AMF with their presets")
    {
        r.encoder = "libx264";
        CHECK(has_arg(ffmpeg_args(r), "-crf"));
        r.encoder = "h264_nvenc";
        CHECK(has_arg(ffmpeg_args(r), "-preset", "p5"));
        r.encoder = "h264_amf";
        CHECK(has_arg(ffmpeg_args(r), "-quality", "quality"));
        r.encoder = "h264_qsv";
        const std::vector<std::string> qsv = ffmpeg_args(r);
        CHECK(std::count(qsv.begin(), qsv.end(), "nv12") == 1);
    }
    SECTION("WebM, with and without transparency")
    {
        r.format = VideoFormat::WebM;
        std::vector<std::string> a = ffmpeg_args(r);
        CHECK(has_arg(a, "-c:v", "libvpx-vp9"));
        CHECK(std::count(a.begin(), a.end(), "yuv420p") == 1);
        CHECK(has_arg(a, "-b:v", "0"));
        CHECK(has_arg(a, "-crf"));
        CHECK(has_arg(a, "-f", "webm"));
        CHECK_FALSE(has_arg(a, "-auto-alt-ref"));
        r.format = VideoFormat::WebMAlpha;
        a        = ffmpeg_args(r);
        CHECK(std::count(a.begin(), a.end(), "yuva420p") == 1);
        CHECK(has_arg(a, "-auto-alt-ref", "0"));
    }
    SECTION("GIF: at most 800 px and 20 fps, a palette that streams")
    {
        r.format = VideoFormat::GIF;
        r.fps    = 60;
        const std::vector<std::string> a = ffmpeg_args(r);
        const std::string f = arg_after(a, "-filter_complex");
        CHECK(f.find("fps=20") != std::string::npos);
        CHECK(f.find("scale=800:") != std::string::npos);
        CHECK(f.find("palettegen=stats_mode=single") != std::string::npos);
        CHECK(f.find("paletteuse=new=1") != std::string::npos);
        CHECK(has_arg(a, "-loop", "0"));
        r.width = 640;
        r.fps   = 12;
        const std::string small = arg_after(ffmpeg_args(r), "-filter_complex");
        CHECK(small.find("fps=12") != std::string::npos);
        CHECK(small.find("scale=640:") != std::string::npos);
    }
    SECTION("animated WebP")
    {
        r.format = VideoFormat::WebP;
        std::vector<std::string> a = ffmpeg_args(r);
        CHECK(has_arg(a, "-c:v", "libwebp_anim"));
        CHECK(has_arg(a, "-loop", "0"));
        CHECK(has_arg(a, "-q:v", "80"));
        CHECK(std::count(a.begin(), a.end(), "yuv420p") == 1);
        r.alpha = true;
        a       = ffmpeg_args(r);
        CHECK(std::count(a.begin(), a.end(), "yuva420p") == 1);
    }
    SECTION("the PNG sequence does not use ffmpeg")
    {
        r.format = VideoFormat::PNGSequence;
        CHECK(ffmpeg_args(r).empty());
    }
    SECTION("the command line for the log quotes what needs it")
    {
        const std::string cmd = ffmpeg_command_line("C:/Program Files/ffmpeg.exe", ffmpeg_args(r));
        CHECK(cmd.rfind("\"C:/Program Files/ffmpeg.exe\"", 0) == 0);
        CHECK(cmd.find("\"C:/out dir/clip.mp4\"") != std::string::npos);
    }
}

// ------------------------------------------------------------------------------------------------
// Frame scheduling, the encoder thread, cancelling
// ------------------------------------------------------------------------------------------------

TEST_CASE("Video job: frame schedule, progress, ETA and cancel", "[Showcase][Video]")
{
    VideoJob job(4);
    CHECK(job.total() == 4);
    CHECK(job.eta_seconds() < 0.);
    for (int i = 0; i < 4; ++i) {
        CHECK(job.next_frame() == i);
        job.frame_rendered(0.5);
    }
    CHECK(job.next_frame() == -1);
    CHECK(job.rendering_done());
    CHECK_FALSE(job.finished());
    CHECK_THAT(job.progress(), WithinAbs(0.5, 1e-12));
    for (int i = 0; i < 4; ++i)
        job.frame_encoded();
    job.frame_encoded();   // never more than rendered
    CHECK(job.encoded() == 4);
    CHECK(job.finished());
    CHECK_THAT(job.progress(), WithinAbs(1., 1e-12));

    VideoJob half(10);
    half.frame_rendered(2.);
    half.frame_rendered(2.);
    CHECK_THAT(half.eta_seconds(), WithinAbs(16., 1e-9));
    half.cancel();
    CHECK(half.cancelled());
    CHECK(half.next_frame() == -1);
    CHECK(half.rendering_done());
    CHECK_FALSE(half.finished());
    half.frame_rendered(1.);
    CHECK(half.rendered() == 2);   // nothing after a cancel

    VideoJob empty(0);   // at least one frame
    CHECK(empty.total() == 1);
}

TEST_CASE("Frame queue: bounded, in order, closing and cancelling", "[Showcase][Video]")
{
    FrameQueue q(3);
    for (int i = 0; i < 3; ++i) {
        VideoFrame f = make_frame(i);
        CHECK(q.try_push(f));
        CHECK(f.rgba.empty());   // moved in
    }
    VideoFrame extra = make_frame(9);
    CHECK(q.full());
    CHECK_FALSE(q.try_push(extra));
    CHECK(extra.rgba.size() == 32);   // untouched when refused
    VideoFrame out;
    REQUIRE(q.pop(out));
    CHECK(out.index == 0);
    CHECK(q.try_push(extra));
    q.close();
    VideoFrame late = make_frame(10);
    CHECK_FALSE(q.try_push(late));
    std::vector<int> drained;
    while (q.pop(out))
        drained.push_back(out.index);
    CHECK(drained == std::vector<int>{ 1, 2, 9 });

    // A cancel wakes a thread waiting for a frame.
    FrameQueue        waiting(2);
    std::atomic<bool> returned{ false };
    std::atomic<bool> got{ true };
    std::thread       t([&]() {
        VideoFrame f;
        got      = waiting.pop(f);
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(returned.load());
    waiting.cancel();
    t.join();
    CHECK(returned.load());
    CHECK_FALSE(got.load());
    CHECK(waiting.cancelled());
}

TEST_CASE("Encoder thread: every frame in order, then the file is completed", "[Showcase][Video]")
{
    auto log  = std::make_shared<FakeSink::Log>();
    auto sink = std::make_unique<FakeSink>(log);
    sink->delay_ms = 1;
    EncoderWorker worker(std::move(sink), 3);
    std::string   error;
    REQUIRE(worker.start(error));
    submit_all(worker, 40);
    worker.finish();
    REQUIRE(worker.wait(20000));
    CHECK(worker.done());
    CHECK_FALSE(worker.failed());
    CHECK(worker.written() == 40);
    CHECK(log->opened == 1);
    CHECK(log->finished == 1);
    CHECK(log->aborted == 0);
    std::vector<int> expected(40);
    std::iota(expected.begin(), expected.end(), 0);
    CHECK(log->indices == expected);
}

TEST_CASE("Encoder thread: cancel mid-way stops, aborts and joins", "[Showcase][Video]")
{
    auto log  = std::make_shared<FakeSink::Log>();
    auto sink = std::make_unique<FakeSink>(log);
    sink->delay_ms = 20;   // slower than the frames come, so the queue fills
    auto        worker = std::make_unique<EncoderWorker>(std::move(sink), 3);
    std::string error;
    REQUIRE(worker->start(error));
    int submitted = 0;
    for (int i = 0; i < 10; ++i) {
        VideoFrame f = make_frame(i);
        if (worker->try_submit(f))
            ++submitted;
    }
    CHECK(submitted <= 4);   // three queued, perhaps one taken: the queue is bounded
    const auto t0 = std::chrono::steady_clock::now();
    worker->cancel();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 2000.);
    CHECK(worker->done());
    CHECK(log->interrupted == 1);
    CHECK(log->aborted == 1);
    CHECK(log->finished == 0);
    CHECK(worker->written() < 10);
    VideoFrame after = make_frame(99);
    CHECK_FALSE(worker->try_submit(after));
    worker.reset();   // the destructor after a cancel is quiet
    CHECK(log->aborted == 1);
}

TEST_CASE("Encoder thread: a failing encoder stops the export", "[Showcase][Video]")
{
    auto log  = std::make_shared<FakeSink::Log>();
    auto sink = std::make_unique<FakeSink>(log);
    sink->fail_at = 5;
    EncoderWorker worker(std::move(sink), 3);
    std::string   error;
    REQUIRE(worker.start(error));
    submit_all(worker, 20);
    worker.finish();
    REQUIRE(worker.wait(20000));
    CHECK(worker.failed());
    CHECK(worker.error() == "fake failure");
    CHECK(worker.written() == 5);
    CHECK(log->aborted == 1);
    CHECK(log->finished == 0);
}

TEST_CASE("Encoder thread: destroying a running worker cancels it", "[Showcase][Video]")
{
    auto log = std::make_shared<FakeSink::Log>();
    {
        auto sink = std::make_unique<FakeSink>(log);
        sink->delay_ms = 10;
        EncoderWorker worker(std::move(sink), 3);
        std::string   error;
        REQUIRE(worker.start(error));
        VideoFrame f = make_frame(0);
        worker.try_submit(f);
    }
    CHECK(log->aborted == 1);
}

TEST_CASE("PNG sequence: numbered frames, removed again on abort", "[Showcase][Video]")
{
    const fs::path dir = scratch_dir("png");
    const fs::path out = dir / "frames";
    {
        PngSequenceSink sink(out.string(), true);
        std::string     error;
        REQUIRE(sink.open(error));
        for (int i = 0; i < 3; ++i) {
            VideoFrame f = make_frame(i, 8, 4);
            REQUIRE(sink.write(f, error));
        }
        REQUIRE(sink.finish(error));
        CHECK(PngSequenceSink::frame_name(0) == "frame_00001.png");
        const fs::path first = out / "frame_00001.png";
        REQUIRE(fs::exists(first));
        boost::nowide::ifstream in(first.string(), std::ios::binary);
        char sig[8] = {};
        in.read(sig, 8);
        CHECK(std::string(sig + 1, 3) == "PNG");
        CHECK(fs::exists(out / "frame_00003.png"));
    }
    {
        const fs::path  other = dir / "cancelled";
        PngSequenceSink sink(other.string(), false);
        std::string     error;
        REQUIRE(sink.open(error));
        VideoFrame f = make_frame(0, 8, 4);
        REQUIRE(sink.write(f, error));
        CHECK(fs::exists(other / "frame_00001.png"));
        sink.abort();
        CHECK_FALSE(fs::exists(other));   // the folder it made goes too
    }
    {
        PngSequenceSink sink((dir / "nothing").string(), false);
        std::string     error;
        REQUIRE(sink.open(error));
        CHECK_FALSE(sink.finish(error));   // no frames is no video
    }
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("The test scene for the headless check", "[Showcase][Video]")
{
    VideoSettings v;
    v.duration = 1.;
    v.fps      = 10;
    CameraView base;
    base.eye    = { 90., -120., 70. };
    base.target = { 0., 0., 15. };
    const std::vector<uint8_t> a = render_test_frame(pose_at_frame(v, base, 0), 160, 90);
    const std::vector<uint8_t> b = render_test_frame(pose_at_frame(v, base, 3), 160, 90);
    REQUIRE(a.size() == size_t(160) * 90 * 4);
    CHECK(a != b);   // the camera moved
    // The cube is in the middle, the backdrop at the corners.
    const uint8_t *centre = &a[(size_t(45) * 160 + 80) * 4];
    const uint8_t *corner = &a[0];
    CHECK(int(centre[0]) != int(corner[0]));
}

TEST_CASE("ffmpeg pipe end to end, when an ffmpeg is there", "[Showcase][Video][ffmpeg]")
{
    // Runs where $EDGESLICER_FFMPEG or PATH gives an ffmpeg (the bundled one is next to the app, which
    // the test does not know); elsewhere there is nothing to check.
    const std::string ffmpeg = find_ffmpeg();
    if (ffmpeg.empty()) {
        SUCCEED("no ffmpeg here");
        return;
    }
    const EncoderCaps caps = probe_ffmpeg(ffmpeg);
    INFO(caps.log);
    if (caps.h264.empty()) {
        WARN("this ffmpeg has no working H.264 encoder");
        return;
    }
    const fs::path dir = scratch_dir("ffmpeg");
    FfmpegRequest  r;
    r.ffmpeg  = ffmpeg;
    r.output  = (dir / "clip.mp4").string();
    r.width   = 320;
    r.height  = 180;
    r.fps     = 30;
    r.format  = VideoFormat::MP4;
    r.encoder = caps.h264.front();
    EncoderWorker worker(std::make_unique<FfmpegSink>(r), 3);
    std::string   error;
    REQUIRE(worker.start(error));
    VideoSettings v;
    v.duration = 1.;
    v.fps      = 30;
    for (int i = 0; i < total_frames(v); ++i) {
        VideoFrame f;
        f.index  = i;
        f.width  = 320;
        f.height = 180;
        f.rgba   = render_test_frame(pose_at_frame(v, base_camera(), i), 320, 180);
        while (!worker.try_submit(f) && !worker.failed())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.finish();
    REQUIRE(worker.wait(120000));
    INFO(worker.error());
    CHECK_FALSE(worker.failed());
    CHECK(fs::file_size(r.output) > 1000);

    // Cancelled: the partial file goes.
    r.output = (dir / "cancelled.mp4").string();
    EncoderWorker cancelled(std::make_unique<FfmpegSink>(r), 3);
    REQUIRE(cancelled.start(error));
    VideoFrame f = make_frame(0, 320, 180);
    cancelled.try_submit(f);
    cancelled.cancel();
    CHECK_FALSE(fs::exists(r.output));
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

// ------------------------------------------------------------------------------------------------
// The callouts in a video
// ------------------------------------------------------------------------------------------------

TEST_CASE("Callout fade: hysteresis against flicker, then a smooth fade", "[Showcase][Video][Callouts]")
{
    SECTION("the first frame shows the anchor as it is")
    {
        CalloutFade fade(10, 8);
        CHECK_THAT(fade.update("f1", 0, true), WithinAbs(0., 1e-12));
        CHECK_THAT(fade.update("f2", 0, false), WithinAbs(1., 1e-12));
        CHECK_THAT(fade.alpha("unknown", 0), WithinAbs(1., 1e-12));
    }
    SECTION("a flickering occlusion test changes nothing")
    {
        CalloutFade fade(10, 8);
        fade.update("f1", 0, false);
        for (int i = 0; i < 200; ++i) {
            const bool occluded = (i % 2) == 0 || (i % 7) == 0;   // never ten in a row
            CHECK_THAT(fade.update("f1", 0, occluded), WithinAbs(1., 1e-12));
        }
    }
    SECTION("a real occlusion fades out after the hysteresis, and back in")
    {
        CalloutFade fade(10, 8);
        fade.update("f1", 0, false);
        for (int i = 1; i < 10; ++i)
            CHECK_THAT(fade.update("f1", 0, true), WithinAbs(1., 1e-12));   // nine frames: not yet
        CHECK_THAT(fade.update("f1", 0, true), WithinAbs(1. - 1. / 8., 1e-12));   // the tenth: it starts
        double prev = 1.;
        for (int i = 0; i < 7; ++i) {
            const double a = fade.update("f1", 0, true);
            CHECK(a < prev);
            prev = a;
        }
        CHECK_THAT(prev, WithinAbs(0., 1e-12));
        CHECK_THAT(fade.update("f1", 0, true), WithinAbs(0., 1e-12));
        // Showing again: the same hysteresis, then up.
        for (int i = 1; i < 10; ++i)
            CHECK_THAT(fade.update("f1", 0, false), WithinAbs(0., 1e-12));
        CHECK(fade.update("f1", 0, false) > 0.);
        for (int i = 0; i < 10; ++i)
            fade.update("f1", 0, false);
        CHECK_THAT(fade.alpha("f1", 0), WithinAbs(1., 1e-12));
    }
    SECTION("a short dip that comes back before the hysteresis restarts the count")
    {
        CalloutFade fade(4, 2);
        fade.update("k", 0, false);
        fade.update("k", 0, true);
        fade.update("k", 0, true);
        fade.update("k", 0, true);
        fade.update("k", 0, false);   // back: the count starts again
        fade.update("k", 0, true);
        fade.update("k", 0, true);
        CHECK_THAT(fade.update("k", 0, true), WithinAbs(1., 1e-12));
        CHECK_THAT(fade.update("k", 0, true), WithinAbs(0.5, 1e-12));
    }
    SECTION("a label fades with its last visible anchor")
    {
        CalloutFade fade(1, 1);
        fade.update("m", 0, false);
        fade.update("m", 1, false);
        fade.update("m", 0, true);
        CHECK_THAT(fade.label_alpha("m", 2), WithinAbs(1., 1e-12));
        fade.update("m", 1, true);
        CHECK_THAT(fade.label_alpha("m", 2), WithinAbs(0., 1e-12));
        CHECK_THAT(fade.label_alpha("m", 0), WithinAbs(1., 1e-12));
        fade.reset();
        CHECK_THAT(fade.alpha("m", 0), WithinAbs(1., 1e-12));
    }
}

TEST_CASE("Callout layout S4: pinned labels and multileader legs never cross", "[Showcase][Video][Callouts]")
{
    std::mt19937 rng(20261021);
    std::uniform_real_distribution<double> ux(0.2, 0.8), uy(0.1, 0.9), uw(60., 160.), u01(0., 1.);
    int dropped = 0, legs = 0, pinned = 0;
    for (int trial = 0; trial < 400; ++trial) {
        LayoutParams p;
        p.frame_w = 1280.;
        p.frame_h = 720.;
        p.margin  = 24.;
        p.gap     = 8.;
        p.landing = 18.;
        p.leader  = trial % 2 ? LeaderStyle::Elbow : LeaderStyle::Straight;
        p.mode    = CalloutLayout::Columns;
        std::vector<LayoutLabel> in(size_t(2 + trial % 6));
        for (LayoutLabel &l : in) {
            l.anchor = { ux(rng) * p.frame_w, uy(rng) * p.frame_h };
            l.w      = uw(rng);
            l.h      = 36.;
            if (u01(rng) < 0.3) {
                l.pinned     = true;
                l.pinned_pos = { u01(rng) * (p.frame_w - l.w), u01(rng) * (p.frame_h - l.h) };
                ++pinned;
            }
            const int extra = int(u01(rng) * 3.);
            for (int e = 0; e < extra; ++e)
                l.extra_anchors.push_back({ ux(rng) * p.frame_w, uy(rng) * p.frame_h });
            legs += extra;
        }
        const CalloutLayoutResult r = layout_callouts(in, p);
        INFO("trial " << trial);
        REQUIRE(r.labels.size() == in.size());
        CHECK_FALSE(layout_has_crossings(r));
        // Also checked directly for different labels.
        for (size_t i = 0; i < r.labels.size(); ++i)
            for (size_t j = i + 1; j < r.labels.size(); ++j) {
                std::vector<std::vector<PointD>> a = r.labels[i].legs, b = r.labels[j].legs;
                a.push_back(r.labels[i].leader);
                b.push_back(r.labels[j].leader);
                for (const auto &x : a)
                    for (const auto &y : b)
                        CHECK_FALSE(polylines_cross(x, y));
            }
        for (size_t i = 0; i < r.labels.size(); ++i) {
            const PlacedLabel &pl = r.labels[i];
            CHECK(pl.legs.size() == in[i].extra_anchors.size());
            // A label with something left out carries a number, and only then.
            bool missing = pl.leader_dropped;
            for (const auto &leg : pl.legs)
                missing = missing || leg.empty();
            CHECK((pl.badge > 0) == missing);
            if (!pl.leader.empty()) {
                CHECK(pl.leader.front().x == in[i].anchor.x);
                CHECK(pl.leader.front().y == in[i].anchor.y);
            }
            for (size_t a = 0; a < pl.legs.size(); ++a)
                if (!pl.legs[a].empty()) {
                    CHECK(pl.legs[a].front().x == in[i].extra_anchors[a].x);
                    CHECK(pl.legs[a].front().y == in[i].extra_anchors[a].y);
                }
        }
        dropped += r.dropped;
    }
    CHECK(pinned > 50);
    CHECK(legs > 200);
    // Left out only now and then: the alternatives place most of what crosses at first.
    WARN("left out " << dropped << " of " << legs << " legs and the leaders of " << pinned << " pinned labels");
    CHECK(dropped < (legs + pinned) / 2);
}

TEST_CASE("Callout layout S4: a pinned label's leader picks a free side", "[Showcase][Video][Callouts]")
{
    LayoutParams p;
    p.frame_w = 1000.;
    p.frame_h = 600.;
    p.margin  = 20.;
    p.gap     = 8.;
    p.leader  = LeaderStyle::Straight;
    p.mode    = CalloutLayout::Columns;
    p.model_center_x = 600.;
    // A free label in the left column, its leader level at y = 300 from x = 450 to the column. The
    // pinned label's nearer side (left) would take its leader across that; its top does not.
    std::vector<LayoutLabel> in(2);
    in[0].anchor = { 450., 300. };
    in[0].w      = 120.;
    in[0].h      = 30.;
    in[1].anchor = { 300., 250. };
    in[1].w      = 120.;
    in[1].h      = 30.;
    in[1].pinned = true;
    in[1].pinned_pos = { 480., 320. };
    const CalloutLayoutResult r = layout_callouts(in, p);
    CHECK(r.labels[0].side == -1);
    CHECK_FALSE(layout_has_crossings(r));
    REQUIRE(r.labels[1].leader.size() == 2);
    CHECK(r.dropped == 0);
    CHECK(r.labels[1].badge == 0);
    CHECK_THAT(r.labels[1].leader.back().x, WithinAbs(540., 1e-9));   // the middle of the top side
    CHECK_THAT(r.labels[1].leader.back().y, WithinAbs(320., 1e-9));
}

TEST_CASE("Callout layout S4: rows above and below a wide, flat model", "[Showcase][Video][Callouts]")
{
    LayoutParams p;
    p.frame_w   = 1280.;
    p.frame_h   = 720.;
    p.margin    = 24.;
    p.gap       = 8.;
    p.landing   = 16.;
    p.mode      = CalloutLayout::Auto;
    p.model_box = { 40., 300., 1200., 120. };   // nearly the frame's width, a fifth of its height
    std::vector<LayoutLabel> in;
    for (int i = 0; i < 6; ++i) {
        LayoutLabel l;
        l.anchor = { 100. + i * 200., i % 2 ? 330. : 400. };
        l.w      = 150.;
        l.h      = 40.;
        in.push_back(l);
    }
    const CalloutLayoutResult r = layout_callouts(in, p);
    REQUIRE_FALSE(r.legend);
    CHECK(r.bands);
    CHECK_FALSE(layout_has_crossings(r));
    for (size_t i = 0; i < in.size(); ++i) {
        const RectD &b = r.labels[i].box;
        INFO("label " << i);
        CHECK(r.labels[i].band != 0);
        CHECK(r.labels[i].side == 0);
        // Above or below the model, never on it, and inside the frame.
        CHECK((b.bottom() <= p.model_box.y + 1e-6 || b.y >= p.model_box.bottom() - 1e-6));
        CHECK(b.x >= -1e-6);
        CHECK(b.right() <= p.frame_w + 1e-6);
        CHECK(b.y >= -1e-6);
        CHECK(b.bottom() <= p.frame_h + 1e-6);
        if (r.labels[i].band < 0)
            CHECK(b.bottom() <= p.model_box.y + 1e-6);
        // The leader starts at the anchor.
        REQUIRE_FALSE(r.labels[i].leader.empty());
        CHECK(r.labels[i].leader.front().x == in[i].anchor.x);
    }
    // Labels of one band do not overlap.
    for (size_t i = 0; i < in.size(); ++i)
        for (size_t j = i + 1; j < in.size(); ++j)
            if (r.labels[i].band == r.labels[j].band) {
                const RectD &a = r.labels[i].box, &b = r.labels[j].box;
                CHECK((a.right() <= b.x + 1e-6 || b.right() <= a.x + 1e-6));
            }

    SECTION("a tall model keeps the columns")
    {
        LayoutParams q = p;
        q.model_box    = { 500., 100., 280., 500. };
        CHECK_FALSE(layout_callouts(in, q).bands);
    }
    SECTION("rows can be asked for")
    {
        LayoutParams q = p;
        q.mode         = CalloutLayout::Bands;
        q.model_box    = RectD();
        const CalloutLayoutResult b = layout_callouts(in, q);
        CHECK(b.bands);
        CHECK_FALSE(layout_has_crossings(b));
    }
    SECTION("the layout name round-trips")
    {
        CalloutLayout l = CalloutLayout::Auto;
        CHECK(std::string(to_string(CalloutLayout::Bands)) == "bands");
        CHECK(from_string("bands", l));
        CHECK(l == CalloutLayout::Bands);
    }
}

TEST_CASE("Callout layout S4: labels keep their side while the camera moves", "[Showcase][Video][Callouts]")
{
    LayoutParams p;
    p.frame_w        = 1000.;
    p.frame_h        = 600.;
    p.margin         = 20.;
    p.gap            = 8.;
    p.mode           = CalloutLayout::Columns;
    p.model_center_x = 500.;
    std::vector<LayoutLabel> in(3);
    in[0].anchor = { 300., 200. };
    in[1].anchor = { 510., 300. };   // just right of the middle
    in[2].anchor = { 700., 400. };
    for (LayoutLabel &l : in) {
        l.w = 100.;
        l.h = 30.;
    }
    CHECK(layout_callouts(in, p).labels[1].side == 1);
    // It was on the left a frame ago and is still within the hysteresis: it stays left.
    p.previous_side   = { -1, -1, 1 };
    p.side_hysteresis = 30.;
    CalloutLayoutResult r = layout_callouts(in, p);
    CHECK(r.labels[1].side == -1);
    CHECK_FALSE(layout_has_crossings(r));
    // Clearly across: it goes.
    in[1].anchor.x = 560.;
    r              = layout_callouts(in, p);
    CHECK(r.labels[1].side == 1);
    // Without the previous frame the plain split.
    p.previous_side.clear();
    in[1].anchor.x = 510.;
    CHECK(layout_callouts(in, p).labels[1].side == 1);
}

TEST_CASE("Saved callout styles", "[Showcase][Video][Callouts]")
{
    CalloutStyleLibrary lib;
    CalloutStyle        dark;
    apply_callout_style_preset(dark, CalloutStylePreset::CardDark);
    dark.font_face = "Georgia";
    dark.size_pct  = 3.1f;
    CalloutStyle tech;
    apply_callout_style_preset(tech, CalloutStylePreset::Technical);
    CHECK(lib.put("", dark) == -1);
    CHECK(lib.put("Shop dark", dark) == 0);
    CHECK(lib.put("Tech", tech) == 1);
    CHECK(lib.put("Shop dark", tech) == 0);   // replaced, not added
    CHECK(lib.styles().size() == 2);
    CHECK(lib.find("Shop dark")->style == tech);
    lib.put("Shop dark", dark);
    CHECK(lib.match(dark)->name == "Shop dark");
    CalloutStyle other = dark;
    other.bold         = !other.bold;
    CHECK(lib.match(other) == nullptr);

    CalloutStyleLibrary back;
    REQUIRE(back.from_json(lib.to_json()));
    REQUIRE(back.styles().size() == 2);
    CHECK(back.styles() == lib.styles());
    CHECK(back.remove("Tech"));
    CHECK_FALSE(back.remove("Tech"));
    CHECK(back.styles().size() == 1);

    // A bad file: no styles, never a crash; a doubled name keeps the first.
    CHECK_FALSE(back.from_json("not json"));
    CHECK(back.styles().empty());
    REQUIRE(back.from_json(R"({"styles":[{"name":"a","style":{"shape":"pill","size_pct":99}},{"name":"a","style":{}},{"style":{}}]})"));
    REQUIRE(back.styles().size() == 1);
    CHECK(back.styles()[0].style.shape == CalloutShape::Pill);
    CHECK(back.styles()[0].style.size_pct <= 10.f);

    // One style on its own.
    CalloutStyle single;
    REQUIRE(callout_style_from_json(callout_style_to_json(dark), single));
    CHECK(single == dark);
}

// ------------------------------------------------------------------------------------------------
// Saving
// ------------------------------------------------------------------------------------------------

TEST_CASE("The video survives the scene's JSON", "[Showcase][Video]")
{
    SECTION("settings and keyframes round-trip")
    {
        const VideoSettings v = sample_video();
        VideoSettings       back;
        REQUIRE(video_from_json(video_to_json(v), back));
        CHECK(back == v);
        CHECK(back.keyframes[1].up == v.keyframes[1].up);
    }
    SECTION("in a scene, in a project")
    {
        Project p;
        Scene  &s = p.ensure_scene_for_plate(0);
        s.video   = sample_video();
        Project back;
        REQUIRE(scenes_from_json(scenes_to_json(p), back));
        REQUIRE(back.scenes.size() == 1);
        CHECK(back.scenes[0].video == s.video);
        CHECK(back.scenes[0] == s);
        CHECK(back.schema == SCHEMA_VERSION);
    }
    SECTION("an S1 to S3 scene has the default orbit")
    {
        Project p;
        p.ensure_scene_for_plate(0);
        json j = json::parse(scenes_to_json(p));
        j["scenes"][0].erase("video");
        Project back;
        REQUIRE(scenes_from_json(j.dump(), back));
        CHECK(back.scenes[0].video == VideoSettings());
        CHECK(back.scenes[0].video.path == VideoPath::Orbit);
    }
    SECTION("keys a later build adds are kept")
    {
        Project p;
        p.ensure_scene_for_plate(0).video = sample_video();
        json j = json::parse(scenes_to_json(p));
        j["scenes"][0]["video"]["future_option"] = 42;
        j["scenes"][0]["video"]["path"]          = "layer_build_up";   // S5's path, unknown here
        Project back;
        REQUIRE(scenes_from_json(j.dump(), back));
        CHECK(back.scenes[0].video.path == VideoPath::Orbit);   // unknown: the default
        const json again = json::parse(scenes_to_json(back));
        CHECK(again["scenes"][0]["video"]["future_option"] == 42);
    }
    SECTION("bad values are clamped, keys sorted, broken keys dropped")
    {
        json j = json::parse(video_to_json(sample_video()));
        j["fps"]        = 1000;
        j["duration"]   = -5;
        j["resolution"] = 99999;
        j["bitrate"]    = 400;
        j["bob_cycles"] = 0;
        j["hold_start"] = 1e9;
        j["format"]     = "avi";
        j["keyframes"]  = json::array({ { { "time", 5 }, { "eye", { 1, 2, 3 } }, { "target", { 0, 0, 0 } } },
                                        { { "time", 1 }, { "eye", { 4, 5, 6 } }, { "target", { 0, 0, 0 } }, { "zoom", -2 } },
                                        { { "time", 2 }, { "eye", { 0, 0, 0 } }, { "target", { 0, 0, 0 } } },   // no direction
                                        { { "time", 3 }, { "eye", "nonsense" }, { "target", { 0, 0, 0 } } },
                                        { { "eye", { 1, 1, 1 } } } });
        VideoSettings v;
        REQUIRE(video_from_json(j.dump(), v));
        CHECK(v.fps == 120);
        CHECK_THAT(v.duration, WithinAbs(MIN_VIDEO_SECONDS, 1e-12));
        CHECK(v.resolution == 4320);
        CHECK(v.bitrate == 100);
        CHECK(v.bob_cycles == 1);
        CHECK_THAT(v.hold_start, WithinAbs(MAX_HOLD_SECONDS, 1e-12));
        CHECK(v.format == VideoFormat::MP4);   // unknown: the default
        REQUIRE(v.keyframes.size() == 3);
        CHECK(v.keyframes[0].time == 1.);
        CHECK(v.keyframes[0].zoom == 1.);
        CHECK(v.keyframes[1].time == 3.);   // the default eye of a key whose eye was nonsense
        CHECK(v.keyframes[2].time == 5.);
        CHECK_FALSE(video_from_json("[1,2]", v));
    }
    SECTION("names round-trip")
    {
        for (VideoPath e : { VideoPath::Orbit, VideoPath::Turntable, VideoPath::OrbitBob, VideoPath::Keyframes }) {
            VideoPath back = VideoPath::Orbit;
            CHECK(from_string(to_string(e), back));
            CHECK(back == e);
        }
        for (VideoFormat e : { VideoFormat::MP4, VideoFormat::WebM, VideoFormat::WebMAlpha, VideoFormat::GIF, VideoFormat::WebP, VideoFormat::PNGSequence }) {
            VideoFormat back = VideoFormat::MP4;
            CHECK(from_string(to_string(e), back));
            CHECK(back == e);
        }
        CHECK(std::string(video_extension(VideoFormat::WebMAlpha)) == "webm");
        CHECK(std::string(video_extension(VideoFormat::PNGSequence)).empty());
    }
}

TEST_CASE("The video survives the project 3MF and stays out of a Bambu export", "[Showcase][Video][3MF]")
{
    const fs::path dir = scratch_dir("3mf");
    Model          model;
    {
        ModelObject *o = model.add_object();
        o->name        = "cube";
        o->add_volume(make_cube(20., 20., 20.));
        o->add_instance();
        o->ensure_on_bed();
    }
    model.showcase.ensure_scene_for_plate(0).video = sample_video();
    DynamicPrintConfig cfg = project_config();

    const SaveStrategy strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    const std::string  project  = (dir / "video.3mf").string();
    {
        StoreParams sp;
        sp.path     = project.c_str();
        sp.model    = &model;
        sp.config   = &cfg;
        sp.strategy = strategy;
        REQUIRE(store_bbs_3mf(sp));
    }
    CHECK(zip_entry(project, ARCHIVE_SCENES).find("\"keyframes\"") != std::string::npos);
    {
        Model                     loaded;
        DynamicPrintConfig        config;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
        PlateDataPtrs             plates;
        std::vector<Preset *>     presets;
        bool                      is_bbl = false;
        Semver                    version;
        REQUIRE(load_bbs_3mf(project.c_str(), &config, &ctxt, &loaded, &plates, &presets, &is_bbl, &version, nullptr,
                             LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
        release_PlateData_list(plates);
        for (Preset *preset : presets)
            delete preset;
        REQUIRE(loaded.showcase.scenes.size() == 1);
        CHECK(loaded.showcase.scenes[0].video == model.showcase.scenes[0].video);
    }
    {
        const std::string   bambu = (dir / "bambu.3mf").string();
        BambuExport::Report report;
        StoreParams         sp;
        sp.path         = bambu.c_str();
        sp.model        = &model;
        sp.config       = &cfg;
        sp.strategy     = strategy;
        sp.bambu_compat = true;
        sp.bambu_report = &report;
        REQUIRE(store_bbs_3mf(sp));
        CHECK(zip_entry(bambu, ARCHIVE_SCENES).empty());
    }
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}
