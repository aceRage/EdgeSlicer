// EdgeSlicer libvgcode spike: see EdgeVGCodeInput.hpp and tests/spike_libvgcode.md.

#include "EdgeVGCodeInput.hpp"

#include <algorithm>
#include <set>

#include "libslic3r/Color.hpp"
#include "libvgcode/include/PathVertex.hpp"

namespace Slic3r {
namespace GUI {
namespace EdgeVGCode {

libvgcode::EGCodeExtrusionRole to_vgcode(ExtrusionRole role)
{
    using R = libvgcode::EGCodeExtrusionRole;
    switch (role) {
    case erNone:                     return R::None;
    case erPerimeter:                return R::Perimeter;
    case erExternalPerimeter:        return R::ExternalPerimeter;
    case erOverhangPerimeter:        return R::OverhangPerimeter;
    case erOverSupportPerimeter:     return R::OverSupportPerimeter;     // EDGE
    case erInternalInfill:           return R::InternalInfill;
    case erSolidInfill:              return R::SolidInfill;
    case erTopSolidInfill:           return R::TopSolidInfill;
    case erBottomSurface:            return R::BottomSurface;
    case erBottomSurfaceOverSupport: return R::BottomSurfaceOverSupport; // EDGE
    case erIroning:                  return R::Ironing;
    case erBridgeInfill:             return R::BridgeInfill;
    case erInternalBridgeInfill:     return R::InternalBridgeInfill;
    case erGapFill:                  return R::GapFill;
    case erSkirt:                    return R::Skirt;
    case erBrim:                     return R::Brim;
    case erSupportMaterial:          return R::SupportMaterial;
    case erSupportMaterialInterface: return R::SupportMaterialInterface;
    case erSupportTransition:        return R::SupportTransition;
    case erWipeTower:                return R::WipeTower;
    case erCustom:                   return R::Custom;
    case erMixed:                    return R::Mixed;
    default:                         return R::None;
    }
}

libvgcode::EMoveType to_vgcode(EMoveType type)
{
    using T = libvgcode::EMoveType;
    switch (type) {
    case EMoveType::Noop:         return T::Noop;
    case EMoveType::Retract:      return T::Retract;
    case EMoveType::Unretract:    return T::Unretract;
    case EMoveType::Seam:         return T::Seam;
    case EMoveType::Tool_change:  return T::ToolChange;
    case EMoveType::Color_change: return T::ColorChange;
    case EMoveType::Pause_Print:  return T::PausePrint;
    case EMoveType::Custom_GCode: return T::CustomGCode;
    case EMoveType::Travel:       return T::Travel;
    case EMoveType::Wipe:         return T::Wipe;
    case EMoveType::Extrude:      return T::Extrude;
    default:                      return T::COUNT;
    }
}

static libvgcode::Vec3 to_vgcode(const Vec3f& v) { return { v.x(), v.y(), v.z() }; }

static libvgcode::Color to_vgcode_color(const std::string& str)
{
    ColorRGBA c;
    if (!decode_color(str, c))
        return libvgcode::DUMMY_COLOR;
    // Same floor as upstream's convert(): libvgcode shades by multiplying, so pure black vanishes.
    auto safe = [](float v) { return std::max<uint8_t>(uint8_t(v * 255.0f), uint8_t(48)); };
    return { safe(c.r()), safe(c.g()), safe(c.b()) };
}

static libvgcode::PathVertex make_vertex(const GCodeProcessorResult::MoveVertex& m, const Vec3f& position,
                                         libvgcode::EMoveType type, const std::array<float, 2>& times)
{
    libvgcode::PathVertex v;
    v.position        = to_vgcode(position);
    v.height          = m.height;
    v.width           = m.width;
    v.feedrate        = m.feedrate;
    // GAP: our processor has no actual (planner) speed per move; upstream #10735 adds it in
    // TimeMachine::calculate_time. Using the commanded speed keeps the "Actual speed" views usable
    // but identical to "Speed".
    v.actual_feedrate = m.feedrate;
    v.mm3_per_mm      = m.mm3_per_mm;
    v.fan_speed       = m.fan_speed;
    v.temperature     = m.temperature;
    v.role            = to_vgcode(m.extrusion_role);
    v.type            = type;
    v.gcode_id        = static_cast<uint32_t>(m.gcode_id);
    v.layer_id        = static_cast<uint32_t>(m.layer_id);
    v.extruder_id     = static_cast<uint8_t>(m.extruder_id);
    v.color_id        = static_cast<uint8_t>(m.cp_color_id);
    v.times           = times;
    return v;
}

libvgcode::GCodeInputData convert(const GCodeProcessorResult& result, const std::vector<std::string>& str_tool_colors,
                                  const std::vector<std::string>& str_color_print_colors, ConvertStats* stats)
{
    libvgcode::GCodeInputData ret;
    ConvertStats              st;

    ret.tools_colors.reserve(str_tool_colors.size());
    for (const std::string& c : str_tool_colors)
        ret.tools_colors.emplace_back(to_vgcode_color(c));
    const std::vector<std::string>& cp = str_color_print_colors.empty() ? str_tool_colors : str_color_print_colors;
    ret.color_print_colors.reserve(cp.size());
    for (const std::string& c : cp)
        ret.color_print_colors.emplace_back(to_vgcode_color(c));

    const std::vector<GCodeProcessorResult::MoveVertex>& moves = result.moves;
    st.moves = moves.size();
    ret.vertices.reserve(2 * moves.size());
    std::set<unsigned int> layers;

    // Upstream's convert() starts at 1: moves[0] is the dummy vertex its processor always stores
    // first. Ours stores real moves from index 0, so the first move only provides the start point.
    for (size_t i = 1; i < moves.size(); ++i) {
        const GCodeProcessorResult::MoveVertex& curr = moves[i];
        const GCodeProcessorResult::MoveVertex& prev = moves[i - 1];
        const libvgcode::EMoveType   type   = to_vgcode(curr.type);
        if (type == libvgcode::EMoveType::COUNT)
            continue;
        const libvgcode::EOptionType option = libvgcode::move_type_to_option(type);
        layers.insert(curr.layer_id);
        st.total_time_normal += curr.times[0];

        if (option == libvgcode::EOptionType::COUNT || option == libvgcode::EOptionType::Travels ||
            option == libvgcode::EOptionType::Wipes) {
            if (ret.vertices.empty() || prev.type != curr.type || prev.extrusion_role != curr.extrusion_role ||
                prev.mm3_per_mm != curr.mm3_per_mm) {
                // Path start: a 'phantom' vertex at the previous position with zero time, exactly as
                // upstream's LibVGCodeWrapper::convert() emits it.
                ret.vertices.emplace_back(make_vertex(curr, prev.position, type, { 0.0f, 0.0f }));
                ++st.phantom_vertices;
            }
        }

        if (curr.is_arc_move_with_interpolation_points()) {
            // GAP: upstream #10735 discretises G2/G3 in the processor (internal_only G1 vertices).
            // Ours keeps BBS interpolation points on one move; expand them here and split the move's
            // time by segment length so layer and total times still add up.
            const size_t       n     = curr.interpolation_points.size();
            std::vector<float> lens(n + 1, 0.0f);
            float              total = 0.0f;
            for (size_t k = 0; k <= n; ++k) {
                const Vec3f& a = (k == 0) ? prev.position : curr.interpolation_points[k - 1];
                const Vec3f& b = (k == n) ? curr.position : curr.interpolation_points[k];
                lens[k]        = (b - a).norm();
                total += lens[k];
            }
            for (size_t k = 0; k <= n; ++k) {
                const float          f = (total > 0.0f) ? lens[k] / total : 1.0f / float(n + 1);
                std::array<float, 2> t{ curr.times[0] * f, curr.times[1] * f };
                const Vec3f&         p = (k == n) ? curr.position : curr.interpolation_points[k];
                ret.vertices.emplace_back(make_vertex(curr, p, type, t));
            }
            st.arc_vertices += n;
        } else {
            ret.vertices.emplace_back(make_vertex(curr, curr.position, type, curr.times));
        }
    }
    ret.vertices.shrink_to_fit();

    // GAP: our result keeps spiral_vase_layers (BBS); libvgcode only needs the flag.
    ret.spiral_vase_mode = !result.spiral_vase_layers.empty();

    st.vertices = ret.vertices.size();
    st.layers   = layers.size();
    if (stats != nullptr)
        *stats = st;
    return ret;
}

} // namespace EdgeVGCode
} // namespace GUI
} // namespace Slic3r
