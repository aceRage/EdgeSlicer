#include "LayerTimeSpeedSmoothingFilter.hpp"

#include <cstdio>
#include <utility>

namespace Slic3r {

LayerTimeSpeedSmoothingFilter::LayerTimeSpeedSmoothingFilter(const PrintConfig &config)
    : m_mode(config.layer_time_speed_smoothing.value)
{}

std::string LayerTimeSpeedSmoothingFilter::format_comment(double factor, double t_raw, double t_out)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), "; LAYER_TIME_SPEED_SMOOTH factor=%.3f t_raw=%.2f t_out=%.2f\n", factor, t_raw, t_out);
    return std::string(buf);
}

std::string LayerTimeSpeedSmoothingFilter::process_layer(std::string &&gcode)
{
    if (m_mode == ltssmOff || gcode.empty())
        return std::move(gcode);

    // S3 identity stub: factor=1 so t_raw ≈ t_out. Times are not parsed until S4.
    std::string out = format_comment(1.0, 0.0, 0.0);
    out += gcode;
    return out;
}

} // namespace Slic3r
