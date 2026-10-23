#ifndef slic3r_Showcase_ShowcaseEncoder_hpp_
#define slic3r_Showcase_ShowcaseEncoder_hpp_

// Showcase tab, phase S4: writing the video's frames (design 4.2, D5). GL- and wxWidgets-free:
//   - FrameSink: one output. FfmpegSink pipes raw RGBA frames into an ffmpeg process (Windows: the
//     bundled LGPL ffmpeg.exe; Linux: ffmpeg from PATH; a Mac with a Homebrew ffmpeg for WebM, GIF and
//     WebP), PngSequenceSink writes numbered PNGs (always available). The Mac's MP4 goes through
//     AVFoundation in the GUI (ShowcaseVideoMac.mm), behind the same interface.
//   - EncoderWorker: the encoder thread. The GUI thread renders a frame at a time and hands it over
//     through a bounded queue (three frames), so rendering and encoding overlap without either running
//     ahead of the other.
//   - which encoders an ffmpeg has and which of them actually work here (probe_ffmpeg, once per run),
//     the H.264 fallback order, and the command line for each format (ffmpeg_args).
//   - a CPU-drawn test scene (a lit cube) for the CLI's --showcase-video-test, which proves the pipe
//     headless.

#include "ShowcaseVideo.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {
namespace Showcase {

// One frame: RGBA8, top row first, straight alpha.
struct VideoFrame
{
    int                  index  = 0;
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgba;
};

class FrameSink
{
public:
    virtual ~FrameSink() = default;
    virtual bool        open(std::string &error) = 0;
    virtual bool        write(const VideoFrame &frame, std::string &error) = 0;
    // Completes the output; false (with the reason) when it could not be completed.
    virtual bool        finish(std::string &error) = 0;
    // Unblocks a write() or finish() running on another thread (kills the encoder process). Safe to
    // call from any thread, at any time.
    virtual void        interrupt() {}
    // Removes the partial output. Called when no write() or finish() runs any more; also twice.
    virtual void        abort() = 0;
    // For the log and the status line: "ffmpeg h264_mf", "PNG sequence".
    virtual std::string description() const = 0;
};

// The frames between the GUI thread and the encoder thread.
class FrameQueue
{
public:
    explicit FrameQueue(size_t capacity = 3) : m_capacity(std::max<size_t>(1, capacity)) {}

    // Moves the frame in unless the queue is full, closed or cancelled (the frame is then untouched).
    bool   try_push(VideoFrame &frame);
    // Waits until a frame is there (true) or the queue is closed and empty, or cancelled (false).
    bool   pop(VideoFrame &out);
    // No more frames will come: pop() drains what is there, then returns false.
    void   close();
    // Drops everything and wakes everyone.
    void   cancel();
    size_t size() const;
    bool   full() const;
    bool   cancelled() const;

private:
    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::deque<VideoFrame>  m_frames;
    size_t                  m_capacity;
    bool                    m_closed    = false;
    bool                    m_cancelled = false;
};

// The encoder thread: takes frames from the queue and writes them to the sink, in order.
class EncoderWorker
{
public:
    explicit EncoderWorker(std::unique_ptr<FrameSink> sink, size_t capacity = 3);
    ~EncoderWorker();
    EncoderWorker(const EncoderWorker &) = delete;
    EncoderWorker &operator=(const EncoderWorker &) = delete;

    // Opens the sink and starts the thread.
    bool        start(std::string &error);
    // Hands a frame over; false while the queue is full (try again on the next tick) or after a
    // failure or a cancel. The frame is moved only on success.
    bool        try_submit(VideoFrame &frame);
    // All frames were handed over: the thread writes what is queued and completes the output.
    void        finish();
    // Stops: drops the queued frames, aborts the sink (removing the partial output) and joins.
    void        cancel();
    // Waits up to timeout_ms for the thread to end (finish() or a failure); true when it has.
    bool        wait(int timeout_ms);

    bool        done() const { return m_done; }
    bool        failed() const { return m_failed; }
    int         written() const { return m_written; }
    std::string error() const;
    std::string description() const { return m_sink ? m_sink->description() : std::string(); }

private:
    void run();

    std::unique_ptr<FrameSink> m_sink;
    FrameQueue                 m_queue;
    std::thread                m_thread;
    std::atomic<bool>          m_done{ false };
    std::atomic<bool>          m_failed{ false };
    std::atomic<bool>          m_cancelled{ false };
    std::atomic<int>           m_written{ 0 };
    mutable std::mutex         m_error_mutex;
    std::string                m_error;
    std::mutex                 m_join_mutex;
    std::condition_variable    m_join_cv;
};

// --- ffmpeg ---------------------------------------------------------------------------------------

enum class EncoderPlatform : int { Windows = 0, Mac, Linux };
EncoderPlatform current_encoder_platform();

// The ffmpeg to use: $EDGESLICER_FFMPEG, then the bundled one (<resources>/tools/go2rtc), then PATH
// (and, on a Mac, Homebrew's folders, which an app started from the Dock does not have in PATH).
// Empty when there is none.
std::string find_ffmpeg();

// The encoder names in the output of `ffmpeg -encoders`.
std::set<std::string> parse_ffmpeg_encoders(const std::string &text);
// The H.264 encoders to try, best first, of those the ffmpeg has (D5). Windows: Media Foundation, then
// OpenH264, then NVENC, Quick Sync and AMF. Linux: x264, OpenH264, then the hardware ones. Mac (a
// Homebrew ffmpeg; MP4 itself goes through AVFoundation): VideoToolbox, x264, OpenH264.
std::vector<std::string> h264_encoder_order(const std::set<std::string> &available, EncoderPlatform platform);

struct EncoderCaps
{
    std::string              ffmpeg;        // empty: no ffmpeg
    std::set<std::string>    listed;        // what `-encoders` lists
    std::vector<std::string> h264;          // the H.264 encoders that encoded a test clip, in order
    bool                     vp9  = false;
    bool                     gif  = false;
    bool                     webp = false;
    std::string              log;           // one line per encoder tried
};

// Lists the encoders and test-encodes a few frames with each H.264 candidate (the bundled build lists
// NVENC, QSV and AMF everywhere, but each needs its GPU and driver). Slow the first time (seconds);
// the answer is kept for the rest of the run, per ffmpeg path.
EncoderCaps probe_ffmpeg(const std::string &ffmpeg, EncoderPlatform platform = current_encoder_platform());

// The formats an export can offer: ffmpeg's, plus MP4 through the platform's own encoder
// (`native_mp4`, AVFoundation on a Mac), plus the PNG sequence always.
std::vector<VideoFormat> available_formats(const EncoderCaps &caps, bool native_mp4);

struct FfmpegRequest
{
    std::string ffmpeg;
    std::string output;
    int         width   = 0;
    int         height  = 0;
    int         fps     = 30;
    VideoFormat format  = VideoFormat::MP4;
    std::string encoder;        // MP4: the H.264 encoder (h264_mf, libopenh264, ...)
    int         quality = 60;   // 0..100
    bool        alpha   = false;   // WebP: keep the transparent background (WebMAlpha always does)
};

// The codec options of one H.264 encoder (pixel format, colour tags, rate) for ffmpeg_args and the probe.
std::vector<std::string> h264_codec_args(const std::string &encoder, int bitrate_kbps, int fps, int quality);

constexpr int GIF_MAX_WIDTH = 800;
constexpr int GIF_MAX_FPS   = 20;

// The arguments after the executable: raw RGBA frames on stdin, the format's encoder and options, the
// output file last (overwritten).
std::vector<std::string> ffmpeg_args(const FfmpegRequest &request);
// The command line for the log.
std::string ffmpeg_command_line(const std::string &ffmpeg, const std::vector<std::string> &args);

class FfmpegSink : public FrameSink
{
public:
    explicit FfmpegSink(FfmpegRequest request);
    ~FfmpegSink() override;
    bool        open(std::string &error) override;
    bool        write(const VideoFrame &frame, std::string &error) override;
    bool        finish(std::string &error) override;
    void        interrupt() override;
    void        abort() override;
    std::string description() const override;

private:
    struct Process;
    std::string stderr_tail() const;

    FfmpegRequest            m_request;
    std::unique_ptr<Process> m_process;
    std::string              m_log_path;
};

// Numbered PNGs in a folder: frame_00001.png ... `alpha`: keep the alpha channel (a transparent
// background), else RGB.
class PngSequenceSink : public FrameSink
{
public:
    PngSequenceSink(std::string folder, bool alpha) : m_folder(std::move(folder)), m_alpha(alpha) {}
    bool        open(std::string &error) override;
    bool        write(const VideoFrame &frame, std::string &error) override;
    bool        finish(std::string &error) override;
    void        abort() override;
    std::string description() const override { return "PNG sequence"; }

    static std::string frame_name(int index);   // 0-based index -> "frame_00001.png"

private:
    std::string              m_folder;
    bool                     m_alpha = false;
    bool                     m_created_folder = false;
    std::vector<std::string> m_files;
};

// Writes an RGBA image as PNG (RGB when `alpha` is false). Compression is kept fast.
bool write_png_rgba(const std::string &path, int width, int height, const std::vector<uint8_t> &rgba, bool alpha);

// --- the headless test ----------------------------------------------------------------------------

// A lit cube on a gradient, seen from `pose`: a stand-in for the GL renderer where there is none.
std::vector<uint8_t> render_test_frame(const CameraPose &pose, int width, int height);

// --showcase-video-test <output>: a 2 s orbit at 720p30 of the test cube through the real encoder
// path (the format from the extension: .mp4, .webm, .gif, .webp, anything else a PNG folder), with
// KEY=value lines on `out`. Returns 0 when the output was written.
int run_video_selftest(const std::string &output, std::ostream &out);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseEncoder_hpp_
