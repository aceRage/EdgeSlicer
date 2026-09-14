#ifndef slic3r_GCode_LayerTimeSpeedSmoothingFilter_hpp_
#define slic3r_GCode_LayerTimeSpeedSmoothingFilter_hpp_

#include "../PrintConfig.hpp"

#include <string>
#include <vector>

namespace Slic3r {

// G-code pipeline stage after CoolingBuffer, before FanMover.
//
// Off: this object is not constructed; the TBB stage is identity (streaming, no extra buffer).
// spiral_mode: pass-through even when the option is on (vase layers are not retimed).
// Any other enabled mode: buffer every cooled layer, then on the last layer call the S2
// solvers and rewrite F. Fan commands from CoolingBuffer are left untouched (F-only v1).
//
// Plan: 09-concept-layer-time-speed-smoothing.md
class LayerTimeSpeedSmoothingFilter
{
public:
    explicit LayerTimeSpeedSmoothingFilter(const PrintConfig &config);

    bool enabled() const { return m_mode != ltssmOff; }

    // Buffer one cooled layer. When last_layer is false the G-code is held and the return
    // is empty. When last_layer is true, all buffered layers are solved and rewritten.
    // Empty gcode is not stored; a last_layer flush still drains the buffer.
    std::string process_layer(std::string &&gcode, size_t layer_id, bool last_layer);

    // Test helper: treat the snippet as a complete (single-layer) print.
    std::string process_layer(std::string &&gcode);

    void reset();

    static const char *mode_key(LayerTimeSpeedSmoothMode mode);
    static std::string format_comment(LayerTimeSpeedSmoothMode mode, double factor, double t_raw, double t_out);

private:
    struct BufferedLayer
    {
        std::string gcode;
        size_t      layer_id = 0;
    };

    std::string flush();

    const PrintConfig             &m_config;
    LayerTimeSpeedSmoothMode       m_mode;
    LayerTimeSlowdownScope         m_slowdown_scope;
    bool                           m_spiral_mode;
    bool                           m_relative_e;
    int                            m_slow_down_layers;
    std::vector<BufferedLayer>     m_layers;
};

} // namespace Slic3r

#endif // slic3r_GCode_LayerTimeSpeedSmoothingFilter_hpp_
