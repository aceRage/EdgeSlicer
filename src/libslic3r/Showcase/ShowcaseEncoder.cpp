#include "ShowcaseEncoder.hpp"
#include "ShowcaseScene.hpp"

#include "../Utils.hpp"   // resources_dir

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/process.hpp>
#ifdef _WIN32
#include <boost/process/windows.hpp>
#else
#include <csignal>
#endif

#include <png.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <map>
#include <sstream>

namespace Slic3r {
namespace Showcase {

namespace fs = boost::filesystem;
namespace bp = boost::process;

// ------------------------------------------------------------------------------------------------
// The queue and the encoder thread
// ------------------------------------------------------------------------------------------------

bool FrameQueue::try_push(VideoFrame &frame)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed || m_cancelled || m_frames.size() >= m_capacity)
            return false;
        m_frames.push_back(std::move(frame));
    }
    m_cv.notify_all();
    return true;
}

bool FrameQueue::pop(VideoFrame &out)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this]() { return m_cancelled || !m_frames.empty() || m_closed; });
    if (m_cancelled || m_frames.empty())
        return false;
    out = std::move(m_frames.front());
    m_frames.pop_front();
    return true;
}

void FrameQueue::close()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
    }
    m_cv.notify_all();
}

void FrameQueue::cancel()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancelled = true;
        m_frames.clear();
    }
    m_cv.notify_all();
}

size_t FrameQueue::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_frames.size();
}

bool FrameQueue::full() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_frames.size() >= m_capacity;
}

bool FrameQueue::cancelled() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cancelled;
}

EncoderWorker::EncoderWorker(std::unique_ptr<FrameSink> sink, size_t capacity) : m_sink(std::move(sink)), m_queue(capacity) {}

EncoderWorker::~EncoderWorker()
{
    if (m_thread.joinable())
        cancel();
}

bool EncoderWorker::start(std::string &error)
{
    if (!m_sink) {
        error = "no output";
        return false;
    }
    if (!m_sink->open(error)) {
        m_sink->abort();
        m_failed = true;
        m_done   = true;
        std::lock_guard<std::mutex> lock(m_error_mutex);
        m_error = error;
        return false;
    }
    m_thread = std::thread([this]() { run(); });
    return true;
}

bool EncoderWorker::try_submit(VideoFrame &frame)
{
    if (m_failed || m_cancelled || m_done)
        return false;
    return m_queue.try_push(frame);
}

void EncoderWorker::finish() { m_queue.close(); }

void EncoderWorker::cancel()
{
    m_cancelled = true;
    m_queue.cancel();
    if (m_sink)
        m_sink->interrupt();
    if (m_thread.joinable())
        m_thread.join();
    if (m_sink)
        m_sink->abort();
    m_done = true;
    m_join_cv.notify_all();
}

bool EncoderWorker::wait(int timeout_ms)
{
    {
        std::unique_lock<std::mutex> lock(m_join_mutex);
        m_join_cv.wait_for(lock, std::chrono::milliseconds(std::max(0, timeout_ms)), [this]() { return m_done.load(); });
    }
    if (m_done && m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id())
        m_thread.join();
    return m_done;
}

std::string EncoderWorker::error() const
{
    std::lock_guard<std::mutex> lock(m_error_mutex);
    return m_error;
}

void EncoderWorker::run()
{
    auto fail = [this](const std::string &why) {
        {
            std::lock_guard<std::mutex> lock(m_error_mutex);
            m_error = why;
        }
        m_failed = true;
        m_queue.cancel();
        m_sink->abort();
    };
    VideoFrame frame;
    while (m_queue.pop(frame)) {
        if (m_cancelled)
            break;
        std::string err;
        if (!m_sink->write(frame, err)) {
            if (!m_cancelled)
                fail(err.empty() ? "the encoder stopped" : err);
            break;
        }
        ++m_written;
    }
    if (!m_cancelled && !m_failed) {
        std::string err;
        if (!m_sink->finish(err))
            fail(err.empty() ? "the video could not be completed" : err);
    }
    {
        std::lock_guard<std::mutex> lock(m_join_mutex);
        m_done = true;
    }
    m_join_cv.notify_all();
}

// ------------------------------------------------------------------------------------------------
// ffmpeg: finding it, its encoders, its command line
// ------------------------------------------------------------------------------------------------

EncoderPlatform current_encoder_platform()
{
#if defined(_WIN32)
    return EncoderPlatform::Windows;
#elif defined(__APPLE__)
    return EncoderPlatform::Mac;
#else
    return EncoderPlatform::Linux;
#endif
}

std::string find_ffmpeg()
{
#ifdef _WIN32
    const char *name = "ffmpeg.exe";
#else
    const char *name = "ffmpeg";
#endif
    boost::system::error_code ec;
    if (const char *env = boost::nowide::getenv("EDGESLICER_FFMPEG"); env != nullptr && *env != 0 && fs::exists(env, ec))
        return env;
    // The bundled build (Windows, and Linux packages that carry one), next to go2rtc as the hub uses it.
    if (!resources_dir().empty()) {
        fs::path bundled = fs::path(resources_dir()) / "tools" / "go2rtc" / name;
        if (fs::exists(bundled, ec))
            return bundled.make_preferred().string();
    }
    const fs::path in_path = bp::search_path(name);
    if (!in_path.empty())
        return in_path.string();
#ifdef __APPLE__
    for (const char *dir : { "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg" })
        if (fs::exists(dir, ec))
            return dir;
#endif
    return std::string();
}

std::set<std::string> parse_ffmpeg_encoders(const std::string &text)
{
    // " V....D libx264              libx264 H.264 / AVC ..." after the "------" line.
    std::set<std::string> out;
    std::istringstream    in(text);
    std::string           line;
    bool                  table = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!table) {
            if (line.find("------") != std::string::npos)
                table = true;
            continue;
        }
        std::istringstream ls(line);
        std::string        flags, name;
        if (!(ls >> flags >> name) || flags.size() < 6 || name.empty())
            continue;
        out.insert(name);
    }
    return out;
}

std::vector<std::string> h264_encoder_order(const std::set<std::string> &available, EncoderPlatform platform)
{
    std::vector<std::string> order;
    switch (platform) {
    case EncoderPlatform::Windows: order = { "h264_mf", "libopenh264", "h264_nvenc", "h264_qsv", "h264_amf" }; break;
    case EncoderPlatform::Mac: order = { "h264_videotoolbox", "libx264", "libopenh264" }; break;
    default: order = { "libx264", "libopenh264", "h264_nvenc", "h264_qsv", "h264_amf" }; break;
    }
    std::vector<std::string> out;
    for (const std::string &e : order)
        if (available.count(e))
            out.push_back(e);
    return out;
}

std::vector<std::string> h264_codec_args(const std::string &encoder, int bitrate_kbps, int fps, int quality)
{
    const bool hw = encoder == "h264_mf" || encoder == "h264_nvenc" || encoder == "h264_qsv" || encoder == "h264_amf";
    const std::string rate = std::to_string(std::max(100, bitrate_kbps)) + "k";
    // Rec. 709 for HD video, tagged so players do not guess.
    std::vector<std::string> a = { "-vf", "scale=out_color_matrix=bt709:out_range=tv", "-pix_fmt", hw ? "nv12" : "yuv420p",
                                   "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-c:v", encoder };
    if (encoder == "libx264") {
        const int crf = int(std::lround(30. - 0.14 * double(std::clamp(quality, 0, 100))));
        a.insert(a.end(), { "-preset", "medium", "-crf", std::to_string(crf) });
    } else if (encoder == "h264_nvenc")
        a.insert(a.end(), { "-preset", "p5", "-b:v", rate });
    else if (encoder == "h264_amf")
        a.insert(a.end(), { "-quality", "quality", "-b:v", rate });
    else
        a.insert(a.end(), { "-b:v", rate });   // h264_mf, libopenh264 (no CRF), h264_qsv, VideoToolbox
    // A key frame every two seconds, so the video seeks well.
    a.insert(a.end(), { "-g", std::to_string(std::max(1, 2 * fps)) });
    return a;
}

std::vector<std::string> ffmpeg_args(const FfmpegRequest &r)
{
    if (r.format == VideoFormat::PNGSequence)
        return {};
    const int w = std::max(2, r.width), h = std::max(2, r.height), fps = std::max(1, r.fps);
    std::vector<std::string> a = { "-hide_banner", "-loglevel", "error", "-nostats", "-y",
                                   "-f", "rawvideo", "-pix_fmt", "rgba", "-s", std::to_string(w) + "x" + std::to_string(h),
                                   "-framerate", std::to_string(fps), "-i", "-", "-an" };
    const int q = std::clamp(r.quality, 0, 100);
    switch (r.format) {
    case VideoFormat::MP4: {
        const std::vector<std::string> c = h264_codec_args(r.encoder.empty() ? std::string("libopenh264") : r.encoder,
                                                           video_bitrate_kbps(w, h, fps, q), fps, q);
        a.insert(a.end(), c.begin(), c.end());
        a.insert(a.end(), { "-movflags", "+faststart", "-f", "mp4" });
        break;
    }
    case VideoFormat::WebM:
    case VideoFormat::WebMAlpha: {
        const bool alpha = r.format == VideoFormat::WebMAlpha;
        const int  crf   = int(std::lround(50. - 0.35 * double(q)));
        a.insert(a.end(), { "-c:v", "libvpx-vp9", "-pix_fmt", alpha ? "yuva420p" : "yuv420p", "-b:v", "0", "-crf", std::to_string(crf),
                            "-row-mt", "1", "-deadline", "good", "-cpu-used", "2" });
        if (alpha)
            a.insert(a.end(), { "-auto-alt-ref", "0" });
        a.insert(a.end(), { "-f", "webm" });
        break;
    }
    case VideoFormat::GIF: {
        // A palette per frame (palettegen + paletteuse in one run). One palette for the whole clip
        // would make ffmpeg hold every frame until the input ends - gigabytes for a long clip - so
        // each frame gets its own, which streams. Forums and chat apps want it small: at most 800 px
        // wide and 20 frames a second.
        const int gw = std::min(w, GIF_MAX_WIDTH) / 2 * 2, gf = std::min(fps, GIF_MAX_FPS);
        a.insert(a.end(), { "-filter_complex",
                            "[0:v]fps=" + std::to_string(gf) + ",scale=" + std::to_string(gw) +
                                ":-1:flags=lanczos,split[a][b];[a]palettegen=stats_mode=single[p];[b][p]paletteuse=new=1:dither=bayer:bayer_scale=4",
                            "-loop", "0", "-f", "gif" });
        break;
    }
    case VideoFormat::WebP: {
        const int wq = std::clamp(q + 20, 30, 100);
        a.insert(a.end(), { "-c:v", "libwebp_anim", "-pix_fmt", r.alpha ? "yuva420p" : "yuv420p", "-lossless", "0", "-q:v", std::to_string(wq),
                            "-compression_level", "4", "-loop", "0", "-f", "webp" });
        break;
    }
    default: break;
    }
    a.push_back(r.output);
    return a;
}

std::string ffmpeg_command_line(const std::string &ffmpeg, const std::vector<std::string> &args)
{
    auto quote = [](const std::string &s) { return s.find_first_of(" \t;[]") != std::string::npos ? "\"" + s + "\"" : s; };
    std::string out = quote(ffmpeg);
    for (const std::string &a : args)
        out += " " + quote(a);
    return out;
}

namespace {

// Waits for the process to end, up to `timeout`. Polled: boost.process 1.84's wait_for() on POSIX can
// block for good once the child is gone (its helper thread never wakes).
bool wait_exit(bp::child &child, std::chrono::milliseconds timeout)
{
    const auto      end = std::chrono::steady_clock::now() + timeout;
    std::error_code ec;
    for (;;) {
        if (!child.running(ec) || ec)
            return true;
        if (std::chrono::steady_clock::now() >= end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Writes all of it to the pipe; false once the reader has gone. (Not through bp::opstream: its
// buffer overruns when a write meets a broken pipe, boost 1.84.)
bool pipe_write_all(bp::pipe &p, const char *data, size_t n)
{
    try {
        while (n > 0) {
            const int w = p.write(data, int(std::min<size_t>(n, size_t(1) << 20)));
            if (w <= 0)
                return false;
            data += w;
            n -= size_t(w);
        }
        return true;
    } catch (...) {
        return false;
    }
}

void pipe_close(bp::pipe &p)
{
    try {
        p.close();
    } catch (...) {}
}

void ignore_sigpipe()
{
#ifndef _WIN32
    // A write to an ffmpeg that has quit must fail, not end the program.
    static const bool once = []() {
        std::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    (void) once;
#endif
}

// Runs ffmpeg with `args`, feeding it `frames` blank frames of w x h on stdin (`frames` 0: reads its
// output instead); true when it exits 0 within the time. The frames go from a thread of their own:
// an encoder that hangs (VideoToolbox outside a login session, a GPU driver) stops reading, and the
// timeout must still be able to end it.
bool ffmpeg_runs(const std::string &ffmpeg, const std::vector<std::string> &args, int frames, int w, int h, int timeout_ms, std::string &output)
{
    ignore_sigpipe();
    try {
        bp::pipe     in;
        bp::ipstream out;
        bp::child    child;
        if (frames <= 0)
            child = bp::child(bp::exe = ffmpeg, bp::args = args, bp::std_in < in, bp::std_out > out, bp::std_err > bp::null
#ifdef _WIN32
                              , bp::windows::create_no_window
#endif
            );
        else
            child = bp::child(bp::exe = ffmpeg, bp::args = args, bp::std_in < in, bp::std_out > bp::null, bp::std_err > bp::null
#ifdef _WIN32
                              , bp::windows::create_no_window
#endif
            );
        std::thread feeder([&in, frames, w, h]() {
            if (frames > 0) {
                std::vector<char> blank(size_t(w) * size_t(h) * 4, char(96));
                for (int i = 0; i < frames; ++i)
                    if (!pipe_write_all(in, blank.data(), blank.size()))
                        break;
            }
            pipe_close(in);
        });
        if (frames <= 0) {
            std::string        line;
            std::ostringstream all;
            while (std::getline(out, line))
                all << line << '\n';
            output = all.str();
        }
        std::error_code ec;
        const bool      finished = wait_exit(child, std::chrono::milliseconds(timeout_ms));
        if (!finished) {
            child.terminate(ec);
            child.wait(ec);
        }
        feeder.join();
        return finished && child.exit_code() == 0;
    } catch (const std::exception &e) {
        output = e.what();
        return false;
    }
}

} // namespace

EncoderCaps probe_ffmpeg(const std::string &ffmpeg, EncoderPlatform platform)
{
    static std::mutex                         mutex;
    static std::map<std::string, EncoderCaps> cache;
    std::lock_guard<std::mutex>               lock(mutex);
    const std::string                         key = ffmpeg + "|" + std::to_string(int(platform));
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    EncoderCaps caps;
    caps.ffmpeg = ffmpeg;
    if (ffmpeg.empty()) {
        cache[key] = caps;
        return caps;
    }
    std::string text;
    if (!ffmpeg_runs(ffmpeg, { "-hide_banner", "-encoders" }, 0, 0, 0, 15000, text)) {
        caps.log = "`ffmpeg -encoders` failed";
        BOOST_LOG_TRIVIAL(warning) << "Showcase video: " << ffmpeg << " did not list its encoders";
        cache[key] = caps;
        return caps;
    }
    caps.listed = parse_ffmpeg_encoders(text);
    caps.vp9    = caps.listed.count("libvpx-vp9") > 0;
    caps.gif    = caps.listed.count("gif") > 0;
    caps.webp   = caps.listed.count("libwebp_anim") > 0;
    // Listed is not working: the bundled build lists NVENC, QSV and AMF on every PC, and Media
    // Foundation can be missing (Windows N). A few frames each tell.
    const int w = 256, h = 144;
    for (const std::string &enc : h264_encoder_order(caps.listed, platform)) {
        std::vector<std::string> args = { "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgba", "-s",
                                          std::to_string(w) + "x" + std::to_string(h), "-framerate", "30", "-i", "-", "-frames:v", "6", "-an" };
        const std::vector<std::string> c = h264_codec_args(enc, 2000, 30, 60);
        args.insert(args.end(), c.begin(), c.end());
        args.insert(args.end(), { "-f", "null", "-" });
        std::string out;
        const auto  t0 = std::chrono::steady_clock::now();
        const bool  ok = ffmpeg_runs(ffmpeg, args, 6, w, h, 20000, out);
        const auto  ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        caps.log += enc + (ok ? ": works" : ": fails") + " (" + std::to_string(ms) + " ms)\n";
        if (ok)
            caps.h264.push_back(enc);
    }
    BOOST_LOG_TRIVIAL(info) << "Showcase video: encoders of " << ffmpeg << ":\n" << caps.log;
    cache[key] = caps;
    return caps;
}

std::vector<VideoFormat> available_formats(const EncoderCaps &caps, bool native_mp4)
{
    std::vector<VideoFormat> out;
    if (native_mp4 || !caps.h264.empty())
        out.push_back(VideoFormat::MP4);
    if (caps.vp9) {
        out.push_back(VideoFormat::WebM);
        out.push_back(VideoFormat::WebMAlpha);
    }
    if (caps.gif)
        out.push_back(VideoFormat::GIF);
    if (caps.webp)
        out.push_back(VideoFormat::WebP);
    out.push_back(VideoFormat::PNGSequence);
    return out;
}

// ------------------------------------------------------------------------------------------------
// The ffmpeg pipe
// ------------------------------------------------------------------------------------------------

struct FfmpegSink::Process
{
    bp::pipe     in;
    bp::child    child;
    std::mutex   mutex;
    bool         interrupted = false;
};

FfmpegSink::FfmpegSink(FfmpegRequest request) : m_request(std::move(request)) {}

FfmpegSink::~FfmpegSink()
{
    if (m_process) {
        std::error_code ec;
        if (m_process->child.valid() && m_process->child.running(ec))
            m_process->child.terminate(ec);
    }
    boost::system::error_code ec;
    if (!m_log_path.empty())
        fs::remove(m_log_path, ec);
}

std::string FfmpegSink::description() const
{
    switch (m_request.format) {
    case VideoFormat::MP4: return "ffmpeg " + m_request.encoder;
    case VideoFormat::WebM: return "ffmpeg libvpx-vp9";
    case VideoFormat::WebMAlpha: return "ffmpeg libvpx-vp9 (alpha)";
    case VideoFormat::GIF: return "ffmpeg gif";
    case VideoFormat::WebP: return "ffmpeg libwebp_anim";
    default: return "ffmpeg";
    }
}

std::string FfmpegSink::stderr_tail() const
{
    if (m_log_path.empty())
        return std::string();
    boost::nowide::ifstream in(m_log_path, std::ios::binary);
    if (!in)
        return std::string();
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() > 1500)
        text = "..." + text.substr(text.size() - 1500);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}

bool FfmpegSink::open(std::string &error)
{
    ignore_sigpipe();
    if (m_request.ffmpeg.empty()) {
        error = "no ffmpeg was found";
        return false;
    }
    boost::system::error_code ec;
    m_log_path = (fs::temp_directory_path(ec) / fs::unique_path("edgeslicer_showcase_ffmpeg_%%%%%%%%.log", ec)).string();
    const std::vector<std::string> args = ffmpeg_args(m_request);
    BOOST_LOG_TRIVIAL(info) << "Showcase video: " << ffmpeg_command_line(m_request.ffmpeg, args);
    try {
        m_process        = std::make_unique<Process>();
        m_process->child = bp::child(bp::exe = m_request.ffmpeg, bp::args = args, bp::std_in < m_process->in, bp::std_out > bp::null,
                                     bp::std_err > m_log_path
#ifdef _WIN32
                                     , bp::windows::create_no_window
#endif
        );
    } catch (const std::exception &e) {
        error = std::string("ffmpeg could not be started: ") + e.what();
        m_process.reset();
        return false;
    }
    return true;
}

bool FfmpegSink::write(const VideoFrame &frame, std::string &error)
{
    if (!m_process) {
        error = "ffmpeg is not running";
        return false;
    }
    if (frame.width != m_request.width || frame.height != m_request.height ||
        frame.rgba.size() != size_t(frame.width) * size_t(frame.height) * 4) {
        error = "a frame of the wrong size";
        return false;
    }
    if (!pipe_write_all(m_process->in, reinterpret_cast<const char *>(frame.rgba.data()), frame.rgba.size())) {
        std::error_code ec;
        if (m_process->child.running(ec))
            wait_exit(m_process->child, std::chrono::milliseconds(2000));
        const std::string tail = stderr_tail();
        error = "ffmpeg stopped taking frames" + (tail.empty() ? std::string() : ":\n" + tail);
        BOOST_LOG_TRIVIAL(error) << "Showcase video: " << error;
        return false;
    }
    return true;
}

bool FfmpegSink::finish(std::string &error)
{
    if (!m_process) {
        error = "ffmpeg is not running";
        return false;
    }
    pipe_close(m_process->in);
    std::error_code ec;
    // GIF and WebP do their work at the end; ten minutes is far more than any clip here needs.
    if (!wait_exit(m_process->child, std::chrono::minutes(10))) {
        m_process->child.terminate(ec);
        error = "ffmpeg did not finish";
        return false;
    }
    const int code = m_process->child.exit_code();
    boost::system::error_code fec;
    const bool written = fs::exists(m_request.output, fec) && fs::file_size(m_request.output, fec) > 0;
    if (code != 0 || !written) {
        const std::string tail = stderr_tail();
        error = "ffmpeg ended with code " + std::to_string(code) + (tail.empty() ? std::string() : ":\n" + tail);
        BOOST_LOG_TRIVIAL(error) << "Showcase video: " << error;
        return false;
    }
    fs::remove(m_log_path, fec);
    m_log_path.clear();
    return true;
}

void FfmpegSink::interrupt()
{
    if (!m_process)
        return;
    std::lock_guard<std::mutex> lock(m_process->mutex);
    m_process->interrupted = true;
    std::error_code ec;
    if (m_process->child.valid() && m_process->child.running(ec))
        m_process->child.terminate(ec);
}

void FfmpegSink::abort()
{
    if (m_process) {
        std::error_code ec;
        if (m_process->child.valid() && m_process->child.running(ec)) {
            m_process->child.terminate(ec);
            m_process->child.wait(ec);
        }
        pipe_close(m_process->in);
    }
    // The partial file goes (the process has let go of it by now).
    boost::system::error_code ec;
    if (!m_request.output.empty())
        fs::remove(m_request.output, ec);
    if (!m_log_path.empty())
        fs::remove(m_log_path, ec);
}

// ------------------------------------------------------------------------------------------------
// The PNG sequence
// ------------------------------------------------------------------------------------------------

bool write_png_rgba(const std::string &path, int width, int height, const std::vector<uint8_t> &rgba, bool alpha)
{
    if (width <= 0 || height <= 0 || rgba.size() < size_t(width) * size_t(height) * 4)
        return false;
    // Rows ready before setjmp: nothing below it owns memory a longjmp could skip.
    std::vector<uint8_t> rgb;
    const uint8_t       *data   = rgba.data();
    size_t               stride = size_t(width) * 4;
    if (!alpha) {
        rgb.resize(size_t(width) * size_t(height) * 3);
        for (size_t i = 0, n = size_t(width) * size_t(height); i < n; ++i)
            std::memcpy(&rgb[i * 3], &rgba[i * 4], 3);
        data   = rgb.data();
        stride = size_t(width) * 3;
    }
    std::vector<png_bytep> rows(static_cast<size_t>(height));
    for (int y = 0; y < height; ++y)
        rows[size_t(y)] = const_cast<png_bytep>(data + size_t(y) * stride);

    FILE *fp = boost::nowide::fopen(path.c_str(), "wb");
    if (fp == nullptr)
        return false;
    png_structp png  = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop   info = png != nullptr ? png_create_info_struct(png) : nullptr;
    volatile bool ok = false;
    if (png != nullptr && info != nullptr) {
        if (setjmp(png_jmpbuf(png)) == 0) {
            png_init_io(png, fp);
            png_set_compression_level(png, 3);   // fast: a sequence has hundreds of frames
            png_set_IHDR(png, info, png_uint_32(width), png_uint_32(height), 8, alpha ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB,
                         PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
            png_set_rows(png, info, rows.data());
            png_write_png(png, info, PNG_TRANSFORM_IDENTITY, nullptr);
            ok = true;
        }
    }
    png_destroy_write_struct(&png, info != nullptr ? &info : nullptr);
    const bool written = ok;
    return std::fclose(fp) == 0 && written;
}

std::string PngSequenceSink::frame_name(int index)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "frame_%05d.png", index + 1);
    return buf;
}

bool PngSequenceSink::open(std::string &error)
{
    boost::system::error_code ec;
    const fs::path dir(m_folder);
    if (!fs::exists(dir, ec)) {
        fs::create_directories(dir, ec);
        if (ec) {
            error = "the folder could not be created: " + ec.message();
            return false;
        }
        m_created_folder = true;
    } else if (!fs::is_directory(dir, ec)) {
        error = m_folder + " is not a folder";
        return false;
    }
    return true;
}

bool PngSequenceSink::write(const VideoFrame &frame, std::string &error)
{
    const std::string file = (fs::path(m_folder) / frame_name(frame.index)).string();
    if (!write_png_rgba(file, frame.width, frame.height, frame.rgba, m_alpha)) {
        error = "could not write " + file;
        return false;
    }
    m_files.push_back(file);
    return true;
}

bool PngSequenceSink::finish(std::string &error)
{
    if (m_files.empty()) {
        error = "no frames were written";
        return false;
    }
    return true;
}

void PngSequenceSink::abort()
{
    boost::system::error_code ec;
    for (const std::string &f : m_files)
        fs::remove(f, ec);
    m_files.clear();
    if (m_created_folder && fs::is_empty(m_folder, ec))
        fs::remove(m_folder, ec);
    m_created_folder = false;
}

// ------------------------------------------------------------------------------------------------
// The headless test
// ------------------------------------------------------------------------------------------------

std::vector<uint8_t> render_test_frame(const CameraPose &pose, int width, int height)
{
    std::vector<uint8_t> img(size_t(std::max(0, width)) * size_t(std::max(0, height)) * 4, 255);
    if (width <= 0 || height <= 0)
        return img;
    // A soft vertical gradient behind.
    for (int y = 0; y < height; ++y) {
        const double  t = double(y) / double(std::max(1, height - 1));
        const uint8_t v = uint8_t(std::lround(250. - 40. * t));
        for (int x = 0; x < width; ++x) {
            uint8_t *p = &img[(size_t(y) * size_t(width) + size_t(x)) * 4];
            p[0] = v;
            p[1] = v;
            p[2] = uint8_t(std::min(255, int(v) + 4));
        }
    }
    using V3 = std::array<double, 3>;
    auto sub   = [](const V3 &a, const V3 &b) { return V3{ a[0] - b[0], a[1] - b[1], a[2] - b[2] }; };
    auto dot   = [](const V3 &a, const V3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto cross = [](const V3 &a, const V3 &b) { return V3{ a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; };
    auto norm  = [&dot](const V3 &a) {
        const double l = std::sqrt(dot(a, a));
        return l > 1e-12 ? V3{ a[0] / l, a[1] / l, a[2] / l } : V3{ 0., 0., 1. };
    };
    // The camera's frame (look_at), a 35 degree vertical field of view narrowed by the zoom.
    const V3     fwd   = norm(sub(pose.target, pose.eye));
    const V3     right = norm(cross(fwd, pose.up));
    const V3     up    = cross(right, fwd);
    const double f     = 1. / std::tan(0.5 * 35. * 3.14159265358979323846 / 180.) * std::max(0.05, pose.zoom);
    const double aspect = double(width) / double(height);
    auto project = [&](const V3 &p, double &sx, double &sy) {
        const V3     d = sub(p, pose.eye);
        const double z = dot(d, fwd);
        if (z < 1e-3)
            return false;
        sx = (0.5 + 0.5 * dot(d, right) * f / (z * aspect)) * width;
        sy = (0.5 - 0.5 * dot(d, up) * f / z) * height;
        return true;
    };
    // A 30 mm cube standing on the plate, its faces in two filament colours, lit from a fixed light
    // in the world (D14): as the camera orbits, the bright faces stay the same faces.
    const double s = 15.;
    const V3     c[8] = { { -s, -s, 0. }, { s, -s, 0. }, { s, s, 0. }, { -s, s, 0. }, { -s, -s, 2 * s }, { s, -s, 2 * s }, { s, s, 2 * s }, { -s, s, 2 * s } };
    struct Face { int v[4]; V3 n; uint8_t rgb[3]; };
    const Face faces[6] = { { { 0, 1, 5, 4 }, { 0, -1, 0 }, { 235, 120, 40 } }, { { 1, 2, 6, 5 }, { 1, 0, 0 }, { 235, 120, 40 } },
                            { { 2, 3, 7, 6 }, { 0, 1, 0 }, { 235, 120, 40 } },  { { 3, 0, 4, 7 }, { -1, 0, 0 }, { 235, 120, 40 } },
                            { { 4, 5, 6, 7 }, { 0, 0, 1 }, { 60, 70, 80 } },    { { 3, 2, 1, 0 }, { 0, 0, -1 }, { 60, 70, 80 } } };
    const V3 light = norm(V3{ -0.5, -0.6, 0.8 });
    for (const Face &face : faces) {
        const V3 center = { 0.25 * (c[face.v[0]][0] + c[face.v[1]][0] + c[face.v[2]][0] + c[face.v[3]][0]),
                            0.25 * (c[face.v[0]][1] + c[face.v[1]][1] + c[face.v[2]][1] + c[face.v[3]][1]),
                            0.25 * (c[face.v[0]][2] + c[face.v[1]][2] + c[face.v[2]][2] + c[face.v[3]][2]) };
        if (dot(face.n, sub(pose.eye, center)) <= 0.)
            continue;   // facing away: a convex solid needs no depth buffer
        double px[4], py[4];
        bool   visible = true;
        for (int k = 0; k < 4; ++k)
            visible = visible && project(c[face.v[k]], px[k], py[k]);
        if (!visible)
            continue;
        const double shade = 0.35 + 0.65 * std::max(0., dot(face.n, light));
        const uint8_t col[3] = { uint8_t(std::lround(face.rgb[0] * shade)), uint8_t(std::lround(face.rgb[1] * shade)),
                                 uint8_t(std::lround(face.rgb[2] * shade)) };
        for (int tri = 0; tri < 2; ++tri) {
            const int    i0 = 0, i1 = tri == 0 ? 1 : 2, i2 = tri == 0 ? 2 : 3;
            const double x0 = px[i0], y0 = py[i0], x1 = px[i1], y1 = py[i1], x2 = px[i2], y2 = py[i2];
            const double area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (std::abs(area) < 1e-9)
                continue;
            const int minx = std::max(0, int(std::floor(std::min({ x0, x1, x2 })))), maxx = std::min(width - 1, int(std::ceil(std::max({ x0, x1, x2 }))));
            const int miny = std::max(0, int(std::floor(std::min({ y0, y1, y2 })))), maxy = std::min(height - 1, int(std::ceil(std::max({ y0, y1, y2 }))));
            for (int y = miny; y <= maxy; ++y)
                for (int x = minx; x <= maxx; ++x) {
                    const double qx = x + 0.5, qy = y + 0.5;
                    const double w0 = ((x1 - qx) * (y2 - qy) - (x2 - qx) * (y1 - qy)) / area;
                    const double w1 = ((x2 - qx) * (y0 - qy) - (x0 - qx) * (y2 - qy)) / area;
                    const double w2 = 1. - w0 - w1;
                    if (w0 < 0. || w1 < 0. || w2 < 0.)
                        continue;
                    uint8_t *p = &img[(size_t(y) * size_t(width) + size_t(x)) * 4];
                    p[0] = col[0];
                    p[1] = col[1];
                    p[2] = col[2];
                    p[3] = 255;
                }
        }
    }
    return img;
}

int run_video_selftest(const std::string &output, std::ostream &out)
{
    const std::string ext = [&output]() {
        std::string e = fs::path(output).extension().string();
        for (char &ch : e)
            ch = char(std::tolower((unsigned char) ch));
        return e;
    }();
    VideoSettings v;
    v.path       = VideoPath::Orbit;
    v.duration   = 2.;
    v.fps        = 30;
    v.resolution = 720;
    v.format     = ext == ".mp4" ? VideoFormat::MP4 : ext == ".webm" ? VideoFormat::WebM : ext == ".gif" ? VideoFormat::GIF :
                   ext == ".webp" ? VideoFormat::WebP : VideoFormat::PNGSequence;
    int w = 0, h = 0;
    video_size(v.resolution, 16, 9, w, h);
    CameraView base;
    base.eye    = { 90., -120., 70. };
    base.target = { 0., 0., 15. };
    base.valid  = true;
    const int frames = total_frames(v);
    out << "SHOWCASE_FORMAT=" << to_string(v.format) << "\n"
        << "SHOWCASE_SIZE=" << w << "x" << h << "\n"
        << "SHOWCASE_FRAMES=" << frames << "\n"
        << "SHOWCASE_SEAMLESS=" << (seamless_loop(v) ? 1 : 0) << "\n";

    std::unique_ptr<FrameSink> sink;
    if (v.format == VideoFormat::PNGSequence)
        sink = std::make_unique<PngSequenceSink>(output, false);
    else {
        const std::string ffmpeg = find_ffmpeg();
        out << "SHOWCASE_FFMPEG=" << ffmpeg << "\n";
        if (ffmpeg.empty()) {
            out << "SHOWCASE_ERROR=no ffmpeg\nSHOWCASE_RESULT=failed" << std::endl;
            return 1;
        }
        const EncoderCaps caps = probe_ffmpeg(ffmpeg);
        std::string listed, working;
        for (const std::string &e : h264_encoder_order(caps.listed, current_encoder_platform()))
            listed += (listed.empty() ? "" : ",") + e;
        for (const std::string &e : caps.h264)
            working += (working.empty() ? "" : ",") + e;
        out << "SHOWCASE_H264_LISTED=" << listed << "\n"
            << "SHOWCASE_H264_WORKING=" << working << "\n"
            << "SHOWCASE_VP9=" << caps.vp9 << "\nSHOWCASE_GIF=" << caps.gif << "\nSHOWCASE_WEBP=" << caps.webp << "\n";
        std::istringstream probe_log(caps.log);
        for (std::string line; std::getline(probe_log, line);)
            if (!line.empty())
                out << "SHOWCASE_PROBE=" << line << "\n";
        FfmpegRequest r;
        r.ffmpeg = ffmpeg;
        r.output = output;
        r.width  = w;
        r.height = h;
        r.fps    = v.fps;
        r.format = v.format;
        if (v.format == VideoFormat::MP4) {
            // $EDGESLICER_SHOWCASE_ENCODER picks a later one in the fallback order, to test it.
            const char *forced = boost::nowide::getenv("EDGESLICER_SHOWCASE_ENCODER");
            if (forced != nullptr && *forced != 0 && std::find(caps.h264.begin(), caps.h264.end(), forced) != caps.h264.end())
                r.encoder = forced;
            else if (!caps.h264.empty())
                r.encoder = caps.h264.front();
            else {
                out << "SHOWCASE_ERROR=no working H.264 encoder\nSHOWCASE_RESULT=failed" << std::endl;
                return 1;
            }
        }
        out << "SHOWCASE_COMMAND=" << ffmpeg_command_line(ffmpeg, ffmpeg_args(r)) << "\n";
        sink = std::make_unique<FfmpegSink>(r);
    }

    const auto    t0 = std::chrono::steady_clock::now();
    EncoderWorker worker(std::move(sink));
    std::string   error;
    if (!worker.start(error)) {
        out << "SHOWCASE_ERROR=" << error << "\nSHOWCASE_RESULT=failed" << std::endl;
        return 1;
    }
    out << "SHOWCASE_ENCODER=" << worker.description() << "\n";
    for (int i = 0; i < frames && !worker.failed(); ++i) {
        VideoFrame frame;
        frame.index  = i;
        frame.width  = w;
        frame.height = h;
        frame.rgba   = render_test_frame(pose_at_frame(v, base, i), w, h);
        while (!worker.try_submit(frame) && !worker.failed())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    worker.finish();
    worker.wait(15 * 60 * 1000);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    boost::system::error_code ec;
    uintmax_t bytes = 0;
    if (fs::is_directory(output, ec)) {
        for (fs::directory_iterator it(output, ec), end; !ec && it != end; it.increment(ec))
            bytes += fs::file_size(it->path(), ec);
    } else if (fs::exists(output, ec))
        bytes = fs::file_size(output, ec);
    const bool ok = worker.done() && !worker.failed() && worker.written() == frames && bytes > 0;
    out << "SHOWCASE_WRITTEN=" << worker.written() << "\n"
        << "SHOWCASE_SECONDS=" << seconds << "\n"
        << "SHOWCASE_OUTPUT=" << output << "\n"
        << "SHOWCASE_BYTES=" << bytes << "\n";
    if (!worker.error().empty())
        out << "SHOWCASE_ERROR=" << worker.error() << "\n";
    out << "SHOWCASE_RESULT=" << (ok ? "ok" : "failed") << std::endl;
    return ok ? 0 : 1;
}

} // namespace Showcase
} // namespace Slic3r
