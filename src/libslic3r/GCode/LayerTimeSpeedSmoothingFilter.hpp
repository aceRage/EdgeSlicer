#ifndef slic3r_GCode_LayerTimeSpeedSmoothingFilter_hpp_
#define slic3r_GCode_LayerTimeSpeedSmoothingFilter_hpp_

#include "../PrintConfig.hpp"

#include <string>

namespace Slic3r {

// S3 G-code pipeline stage: after CoolingBuffer, before FanMover.
//
// When layer_time_speed_smoothing is Off this object is not constructed and the
// TBB stage is an identity (no extra layer buffer). When enabled, S3 only prepends
// a diagnostic comment with factor=1 (t_raw ≈ t_out). No F rewrite — that is S4.
//
// Plan: 09-concept-layer-time-speed-smoothing.md
// Solvers live in LayerTimeSpeedSmoothing.{hpp,cpp}; this filter does not call them yet.
class LayerTimeSpeedSmoothingFilter
{
public:
    explicit LayerTimeSpeedSmoothingFilter(const PrintConfig &config);

    bool enabled() const { return m_mode != ltssmOff; }

    // Identity when Off or when gcode is empty. Otherwise prepends the diagnostic comment.
    std::string process_layer(std::string &&gcode);

    // S3 is stateless. S4 will clear the collected-layer buffer here.
    void reset() {}

    static std::string format_comment(double factor, double t_raw, double t_out);

private:
    LayerTimeSpeedSmoothMode m_mode;
};

} // namespace Slic3r

#endif // slic3r_GCode_LayerTimeSpeedSmoothingFilter_hpp_
