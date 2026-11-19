#include "MoonrakerProgress.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace MoonrakerProgress {

using nlohmann::json;

static bool num_at(const json& j, const char* key, double& out)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_number()) return false;
    out = j[key].get<double>();
    return std::isfinite(out);
}

static std::string str_at(const json& j, const char* key)
{
    return (j.is_object() && j.contains(key) && j[key].is_string()) ? j[key].get<std::string>() : std::string();
}

FileMeta parse_metadata(const json& answer)
{
    FileMeta m;
    // {"error": {"code": 404, ...}}: the printer has no metadata for that file.
    if (answer.is_object() && answer.contains("error") && !answer.contains("result")) return m;
    const json& r = (answer.is_object() && answer.contains("result") && answer["result"].is_object()) ? answer["result"] : answer;
    if (!r.is_object()) return m;
    m.valid    = true;
    m.filename = str_at(r, "filename");
    double d   = 0;
    if (num_at(r, "estimated_time", d) && d > 0) m.estimated_time = d;
    if (num_at(r, "gcode_start_byte", d) && d >= 0) m.gcode_start_byte = (long long) d;
    if (num_at(r, "gcode_end_byte", d) && d >= 0) m.gcode_end_byte = (long long) d;
    if (num_at(r, "size", d) && d >= 0) m.size = (long long) d;
    if (num_at(r, "modified", d)) m.modified = d;
    return m;
}

std::string Sample::metadata_filename() const
{
    if (!filename.empty()) return filename;
    const std::string key = "gcodes";
    const size_t      at  = file_path.rfind(key);
    if (at == std::string::npos) return std::string();
    std::string rest = file_path.substr(at + key.size());
    while (!rest.empty() && (rest.front() == '/' || rest.front() == '\\')) rest.erase(0, 1);
    return rest;
}

Sample parse_sample(const json& in)
{
    Sample      s;
    const json* st = &in;
    if (in.is_object() && in.contains("result") && in["result"].is_object() && in["result"].contains("status"))
        st = &in["result"]["status"];
    if (!st->is_object()) return s;
    double d = 0;
    if (st->contains("print_stats") && (*st)["print_stats"].is_object()) {
        const json& ps = (*st)["print_stats"];
        s.state        = str_at(ps, "state");
        s.filename     = str_at(ps, "filename");
        if (num_at(ps, "print_duration", d) && d > 0) s.print_duration = d;
    }
    if (st->contains("display_status") && num_at((*st)["display_status"], "progress", d))
        s.display_progress = std::clamp(d, 0.0, 1.0);
    if (st->contains("virtual_sdcard") && (*st)["virtual_sdcard"].is_object()) {
        const json& sd = (*st)["virtual_sdcard"];
        s.file_path    = str_at(sd, "file_path");
        if (num_at(sd, "progress", d)) s.sd_progress = std::clamp(d, 0.0, 1.0);
        if (num_at(sd, "file_position", d) && d >= 0) s.file_position = (long long) d;
        if (num_at(sd, "file_size", d) && d >= 0) s.file_size = (long long) d;
    }
    return s;
}

const char* basis_name(Basis b)
{
    switch (b) {
    case Basis::BodyBytes: return "body bytes";
    case Basis::DisplayStatus: return "display_status";
    case Basis::SdCard: return "virtual_sdcard";
    case Basis::SlicerEstimate: return "slicer estimate";
    case Basis::Linear: return "linear";
    default: return "none";
    }
}

Estimate estimate(const Sample& s, const FileMeta& meta)
{
    Estimate e;
    // Percent, on the Device page's basis when it can be had.
    if (meta.valid && meta.has_body_range() && s.file_position >= 0) {
        const long long pos = s.file_position;
        if (pos > meta.gcode_end_byte)
            e.fraction = 1.0;
        else if (pos < meta.gcode_start_byte)
            e.fraction = 0.0;
        else
            e.fraction = double(pos - meta.gcode_start_byte) / double(meta.gcode_end_byte - meta.gcode_start_byte);
        e.percent_basis = Basis::BodyBytes;
    } else if (s.display_progress >= 0) {
        e.fraction      = s.display_progress;
        e.percent_basis = Basis::DisplayStatus;
    } else if (s.sd_progress >= 0) {
        e.fraction      = s.sd_progress;
        e.percent_basis = Basis::SdCard;
    }
    e.fraction = std::clamp(e.fraction, 0.0, 1.0);
    e.percent  = (int) std::lround(e.fraction * 100.0);

    // Time left: only while a job is on (a finished, cancelled or idle printer has none to show).
    const bool on_job = s.state == "printing" || s.state == "paused";
    if (!on_job) return e;
    if (meta.valid && meta.estimated_time > 0) {
        e.left_s     = (int) std::lround(std::max(0.0, meta.estimated_time * (1.0 - e.fraction)));
        e.time_basis = Basis::SlicerEstimate;
    } else if (e.fraction >= MIN_LINEAR_FRACTION && e.fraction < 1.0 && s.print_duration > 0) {
        e.left_s     = (int) std::lround(s.print_duration / e.fraction - s.print_duration);
        e.time_basis = Basis::Linear;
    }
    return e;
}

} // namespace MoonrakerProgress
} // namespace Slic3r
