#ifndef slic3r_Showcase_ShowcaseRenderCli_hpp_
#define slic3r_Showcase_ShowcaseRenderCli_hpp_

// Showcase S5: the command line that renders a Showcase video or still without a window
//
//   EdgeSlicer --showcase-render <project.3mf> --out <file> [--path <path.json>] [--plate N]
//              [--size 1080] [--aspect 16:9] [--fps 30] [--format mp4|webm|webm_alpha|gif|webp|png]
//              [--quality draft|high|best] [--bitrate 0-100] [--still <seconds>] [--report <report.json>]
//              [--slice] [--dry-run] [--datadir <dir>]
//
// This half parses and checks the arguments (unit tested) and writes the report; the GUI half
// (slic3r/GUI/Showcase/ShowcaseRenderJob) loads the project in a hidden instance and renders through
// the same renderer as the Showcase tab. See docs/showcase_paths.md.

#include "ShowcaseScene.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
namespace Showcase {

// The process exit codes, for scripts and agents.
enum RenderExit : int {
    RENDER_OK            = 0,
    RENDER_USAGE         = 2,   // bad arguments
    RENDER_INVALID_PATH  = 3,   // the path file does not parse or does not compile for this project
    RENDER_PROJECT_ERROR = 4,   // the project does not load, or has no such plate
    RENDER_FAILED        = 5,   // OpenGL or a frame failed
    RENDER_ENCODER_ERROR = 6,   // the encoder (ffmpeg, PNG writing) failed
};

struct RenderRequest
{
    std::string project;          // the .3mf
    std::string path;             // the path file; empty = the scene's own video settings
    std::string out;              // the file (or, for a PNG sequence, the folder) to write
    std::string report;           // optional: where the JSON report goes
    std::string datadir;          // optional: the data folder (as --datadir everywhere)
    int         plate = 1;        // 1-based
    int         size  = 0;        // the shorter side in pixels; 0 = the scene's video resolution
    int         aspect_w = 0, aspect_h = 0;   // 0 = the scene's export aspect
    int         fps   = 0;        // 0 = the path's, else the scene's
    std::optional<Quality> quality;           // supersampling; empty = the scene's
    int         bitrate = -1;     // 0..100, -1 = the scene's
    // A video (format) or, with --still, one image (still_format).
    bool        still      = false;
    double      still_time = 0.;
    VideoFormat format       = VideoFormat::MP4;
    ImageFormat still_format = ImageFormat::PNG;
    bool        slice   = false;  // slice the plate first: the build-up lays its real toolpaths
    bool        dry_run = false;  // load and check everything, render nothing
    bool        help    = false;
};

// Whether the command line asks for a Showcase render (then nothing else runs).
bool is_showcase_render_cli(int argc, char **argv);
// `args` without the program name. The format comes from --format, else from --out's extension
// (.mp4 .webm .gif .webp; .png .jpg .jpeg .webp with --still; no extension = a PNG sequence folder).
bool parse_render_args(const std::vector<std::string> &args, RenderRequest &out, std::string &error);
std::string render_usage();

// What the render did, for --report (and the last lines on stdout).
struct RenderReport
{
    bool                     ok        = false;
    int                      exit_code = RENDER_OK;
    std::string              project;
    std::string              path;
    std::string              path_name;
    std::string              out;
    int                      plate      = 1;
    bool                     still      = false;
    double                   still_time = 0.;
    std::string              format;      // "mp4", "png_sequence", "png" ...
    std::string              encoder;     // what wrote it: "ffmpeg h264_mf", "AVFoundation", "png" ...
    int                      width = 0, height = 0;
    int                      fps      = 0;
    int                      frames   = 0;   // frames written
    double                   duration = 0.;  // seconds of video
    int                      keyframes = 0;
    int                      events    = 0;
    std::string              source;         // "model", "toolpaths"
    std::string              build;          // "", "toolpaths", "clip"
    bool                     sliced = false; // --slice: the plate was sliced
    double                   seconds = 0.;   // wall time of the render
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};
std::string render_report_json(const RenderReport &report);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseRenderCli_hpp_
