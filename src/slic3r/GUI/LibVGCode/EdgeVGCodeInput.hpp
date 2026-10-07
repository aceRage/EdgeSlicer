#ifndef slic3r_EdgeVGCodeInput_hpp_
#define slic3r_EdgeVGCodeInput_hpp_

// EdgeSlicer libvgcode spike (tests/spike_libvgcode.md).
//
// Feeds libvgcode (src/libvgcode, the PrusaSlicer 2.8 G-code viewer that OrcaSlicer took in #10735)
// from EdgeSlicer's own GCodeProcessorResult, WITHOUT the GCodeProcessor rework that #10735 ships.
// Upstream's LibVGCodeWrapper::convert() expects a result whose arcs are already discretised into
// internal G1 vertices and that carries actual_feedrate per move; ours keeps BBS arc interpolation
// points and has no actual speed. This adapter bridges exactly those two gaps, so the data path can
// be measured and unit tested before any viewer code is replaced.
//
// libvgcode is AGPL-3.0-or-later (Prusa Research); see src/libvgcode file headers.

#include <cstddef>
#include <string>
#include <vector>

#include "libvgcode/include/GCodeInputData.hpp"
#include "libvgcode/include/Types.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

namespace Slic3r {
namespace GUI {
namespace EdgeVGCode {

struct ConvertStats
{
    size_t moves{ 0 };            // GCodeProcessorResult::moves.size()
    size_t vertices{ 0 };         // libvgcode::PathVertex count produced
    size_t phantom_vertices{ 0 }; // path-start vertices libvgcode needs to split paths
    size_t arc_vertices{ 0 };     // extra vertices produced by expanding arc interpolation points
    size_t layers{ 0 };           // distinct layer ids seen
    double total_time_normal{ 0.0 }; // sum of per-move durations, normal mode (s)
};

libvgcode::EGCodeExtrusionRole to_vgcode(ExtrusionRole role);
libvgcode::EMoveType           to_vgcode(EMoveType type);

// Converts EdgeSlicer's processor result into libvgcode input. Tool/colour-print palettes are
// "#RRGGBB" strings as the legacy viewer receives them.
libvgcode::GCodeInputData convert(const GCodeProcessorResult&       result,
                                  const std::vector<std::string>& str_tool_colors,
                                  const std::vector<std::string>& str_color_print_colors,
                                  ConvertStats*                   stats = nullptr);

} // namespace EdgeVGCode
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_EdgeVGCodeInput_hpp_
