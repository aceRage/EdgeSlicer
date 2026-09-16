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
// Modes A/B never speed up overhang/bridge, ironing, top solid, or support (incl. interface).
//
// Why the whole print is buffered, not a window of layers: the solvers are global. The
// neighbour band chains, t[i] <= t[i-1] / (1 - v) in both directions, so one short layer
// bounds every layer within ln(t_long / t_short) / ln(1 / (1 - v)) of it - 24 layers for
// v = 25% and a 1000:1 spread, 135 layers for v = 5% - and the layer that ends a chain can
// be arbitrarily far ahead and arbitrarily short. Mode C also relaxes v against a time
// budget summed over the whole print. Any fixed lookahead therefore changes the result
// whenever a shorter layer sits beyond it, so the stage keeps every layer until the last
// one and accepts the cost: nothing reaches the file until the last layer, and the peak
// footprint is about two copies of the print's G-code text (flush() parses in two passes
// so the per-line records exist for one layer at a time; see there) - a 200 MB print holds
// roughly 400 MB here, plus the one copy each downstream stage (FanMover, PA processor)
// makes of the string it returns. Off costs nothing.
//
// It never fights CoolingBuffer, which runs just upstream and owns the thermal floor:
//  - a line at or below slow_down_min_speed is never touched, in any mode, and a slowdown
//    is floored at that speed (the same floor CoolingBuffer observes);
//  - speed-up never takes a layer's time below slow_down_layer_time;
//  - a layer CoolingBuffer already stretched to slow_down_layer_time (cooling_slowed_down)
//    is frozen for Mode C: its speeds are what CoolingBuffer set, and it only bounds its
//    neighbours.
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
    // cooling_slowed_down: CoolingBuffer rewrote F on this layer to reach slow_down_layer_time
    // (LayerResult::cooling_slowed_down); Mode C then leaves the layer at those speeds.
    std::string process_layer(std::string &&gcode, size_t layer_id, bool last_layer, bool cooling_slowed_down = false);

    // Test helper: treat the snippet as a complete (single-layer) print.
    std::string process_layer(std::string &&gcode);

    void reset();

    static const char *mode_key(LayerTimeSpeedSmoothMode mode);
    static std::string format_comment(LayerTimeSpeedSmoothMode mode, double factor, double t_raw, double t_out);

private:
    struct BufferedLayer
    {
        std::string gcode;
        size_t      layer_id            = 0;
        bool        cooling_slowed_down = false;
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
