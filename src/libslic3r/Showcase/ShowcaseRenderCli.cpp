#include "ShowcaseRenderCli.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Slic3r {
namespace Showcase {

namespace {

std::string cli_lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower((unsigned char) c));
    return s;
}

// The extension of a file name, lower case, without the dot ("" for none).
std::string cli_extension(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    const size_t dot   = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot + 1 >= path.size())
        return std::string();
    return cli_lower(path.substr(dot + 1));
}

bool parse_number(const std::string &s, double &out)
{
    if (s.empty())
        return false;
    char        *end = nullptr;
    const double v   = std::strtod(s.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(v))
        return false;
    out = v;
    return true;
}

bool parse_int(const std::string &s, int &out)
{
    double v = 0.;
    if (!parse_number(s, v) || std::floor(v) != v || std::abs(v) > 1e9)
        return false;
    out = int(v);
    return true;
}

} // namespace

bool is_showcase_render_cli(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
        if (argv[i] != nullptr && std::strcmp(argv[i], "--showcase-render") == 0)
            return true;
    return false;
}

std::string render_usage()
{
    return "Usage: EdgeSlicer --showcase-render <project.3mf> --out <file> [options]\n"
           "  --path <path.json>      the camera path and timed events (default: the scene's own video)\n"
           "  --plate <N>             the plate, 1-based (default 1)\n"
           "  --size <px>             the shorter side (default: the scene's video resolution)\n"
           "  --aspect <W:H>          the frame's shape (default: the scene's export size)\n"
           "  --fps <n>               frames per second, 10-120 (default: the path's, else the scene's)\n"
           "  --format <f>            mp4, webm, webm_alpha, gif, webp or png (a PNG sequence folder);\n"
           "                          with --still: png, jpg or webp (default: from --out's extension)\n"
           "  --quality <q>           draft, high or best supersampling (default: the scene's)\n"
           "  --bitrate <0-100>       the encoder's quality slider (default: the scene's)\n"
           "  --still <seconds>       render the single frame at that time instead of a video\n"
           "  --report <report.json>  write what was done (frames, encoder, warnings, errors)\n"
           "  --slice                 slice the plate first: a build-up then lays the real toolpaths, and a\n"
           "                          scene showing toolpaths shows them (otherwise the model is shown)\n"
           "  --dry-run               load and check the project and the path, render nothing\n"
           "  --datadir <dir>         the data folder to run with\n"
           "Exit codes: 0 done, 2 bad arguments, 3 invalid path, 4 project or plate problem, 5 render failed, 6 encoder failed.\n"
           "The path format: docs/showcase_paths.md; examples in resources/showcase/paths.\n";
}

bool parse_render_args(const std::vector<std::string> &args, RenderRequest &out, std::string &error)
{
    out = RenderRequest();
    error.clear();
    std::string format_arg;
    bool        have_size = false;
    for (const std::string &a : args)
        if (a == "--help" || a == "-h") {
            out.help = true;
            return true;
        }
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        auto value = [&](std::string &v) {
            if (i + 1 >= args.size() || args[i + 1].empty() || (args[i + 1].size() > 1 && args[i + 1][0] == '-' && args[i + 1][1] == '-')) {
                error = a + " needs a value";
                return false;
            }
            v = args[++i];
            return true;
        };
        std::string v;
        if (a == "--help" || a == "-h") {
            out.help = true;
            return true;
        } else if (a == "--showcase-render") {
            if (!value(out.project))
                return false;
        } else if (a == "--path") {
            if (!value(out.path))
                return false;
        } else if (a == "--out" || a == "-o") {
            if (!value(out.out))
                return false;
        } else if (a == "--report") {
            if (!value(out.report))
                return false;
        } else if (a == "--datadir") {
            if (!value(out.datadir))
                return false;
        } else if (a == "--plate") {
            if (!value(v))
                return false;
            if (!parse_int(v, out.plate) || out.plate < 1 || out.plate > 1000) {
                error = "--plate must be a plate number from 1";
                return false;
            }
        } else if (a == "--size") {
            if (!value(v))
                return false;
            if (!parse_int(v, out.size) || out.size < 144 || out.size > 4320) {
                error = "--size must be the shorter side in pixels, 144 to 4320";
                return false;
            }
            have_size = true;
        } else if (a == "--aspect") {
            if (!value(v))
                return false;
            const size_t colon = v.find_first_of(":x/");
            if (colon == std::string::npos || !parse_int(v.substr(0, colon), out.aspect_w) || !parse_int(v.substr(colon + 1), out.aspect_h) ||
                out.aspect_w < 1 || out.aspect_h < 1 || out.aspect_w > 100 * out.aspect_h || out.aspect_h > 100 * out.aspect_w) {
                error = "--aspect must look like 16:9";
                return false;
            }
        } else if (a == "--fps") {
            if (!value(v))
                return false;
            if (!parse_int(v, out.fps) || out.fps < 10 || out.fps > 120) {
                error = "--fps must be a whole number from 10 to 120";
                return false;
            }
        } else if (a == "--format") {
            if (!value(format_arg))
                return false;
            format_arg = cli_lower(format_arg);
        } else if (a == "--quality") {
            if (!value(v))
                return false;
            Quality q;
            if (!from_string(cli_lower(v), q)) {
                error = "--quality must be draft, high or best";
                return false;
            }
            out.quality = q;
        } else if (a == "--bitrate") {
            if (!value(v))
                return false;
            if (!parse_int(v, out.bitrate) || out.bitrate < 0 || out.bitrate > 100) {
                error = "--bitrate must be from 0 to 100";
                return false;
            }
        } else if (a == "--still") {
            if (!value(v))
                return false;
            if (!parse_number(v, out.still_time) || out.still_time < 0. || out.still_time > MAX_VIDEO_SECONDS) {
                error = "--still must be a time in seconds from 0 to 60";
                return false;
            }
            out.still = true;
        } else if (a == "--dry-run") {
            out.dry_run = true;
        } else if (a == "--slice") {
            out.slice = true;
        } else {
            error = "unknown option " + a + " (see --showcase-render --help)";
            return false;
        }
    }
    (void) have_size;
    if (out.project.empty()) {
        error = "--showcase-render needs the project (.3mf)";
        return false;
    }
    if (cli_extension(out.project) != "3mf") {
        error = "the project must be a .3mf file";
        return false;
    }
    if (out.out.empty() && !out.dry_run) {
        error = "--out is required (the video or image file to write)";
        return false;
    }
    const std::string ext = cli_extension(out.out);
    if (out.still) {
        const std::string f = format_arg.empty() ? ext : format_arg;
        if (f.empty() || f == "png")
            out.still_format = ImageFormat::PNG;
        else if (f == "jpg" || f == "jpeg")
            out.still_format = ImageFormat::JPEG;
        else if (f == "webp")
            out.still_format = ImageFormat::WebP;
        else {
            error = "a still is png, jpg or webp (not \"" + f + "\")";
            return false;
        }
    } else {
        const std::string f = format_arg.empty() ? ext : format_arg;
        if (f == "mp4")
            out.format = VideoFormat::MP4;
        else if (f == "webm")
            out.format = VideoFormat::WebM;
        else if (f == "webm_alpha")
            out.format = VideoFormat::WebMAlpha;
        else if (f == "gif")
            out.format = VideoFormat::GIF;
        else if (f == "webp")
            out.format = VideoFormat::WebP;
        else if (f == "png" || f == "png_sequence" || (f.empty() && !out.out.empty()))
            out.format = VideoFormat::PNGSequence;
        else if (f == "jpg" || f == "jpeg") {
            error = "a JPEG is a still: add --still <seconds>";
            return false;
        } else if (!out.dry_run || !f.empty()) {
            error = "unknown video format \"" + f + "\" (mp4, webm, webm_alpha, gif, webp or png)";
            return false;
        }
        if (out.format == VideoFormat::PNGSequence && ext == "png" && format_arg.empty()) {
            error = "a .png file is a still: add --still <seconds>, or give a folder for a PNG sequence";
            return false;
        }
    }
    return true;
}

std::string render_report_json(const RenderReport &r)
{
    nlohmann::ordered_json j;
    j["ok"]        = r.ok;
    j["exit_code"] = r.exit_code;
    j["project"]   = r.project;
    if (!r.path.empty())
        j["path"] = r.path;
    if (!r.path_name.empty())
        j["path_name"] = r.path_name;
    j["out"]   = r.out;
    j["plate"] = r.plate;
    j["kind"]  = r.still ? "still" : "video";
    if (r.still)
        j["still_time"] = r.still_time;
    j["format"]    = r.format;
    j["encoder"]   = r.encoder;
    j["width"]     = r.width;
    j["height"]    = r.height;
    j["fps"]       = r.fps;
    j["frames"]    = r.frames;
    j["duration"]  = r.duration;
    j["keyframes"] = r.keyframes;
    j["events"]    = r.events;
    j["source"]    = r.source;
    if (!r.build.empty())
        j["build"] = r.build;
    j["sliced"]   = r.sliced;
    j["seconds"]  = std::round(r.seconds * 100.) / 100.;
    j["warnings"] = r.warnings;
    j["errors"]   = r.errors;
    return j.dump(2);
}

} // namespace Showcase
} // namespace Slic3r
