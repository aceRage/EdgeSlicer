#include "WipeTower.hpp"

#include <cassert>
#include <iostream>
#include <map>
#include <vector>
#include <numeric>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <iomanip>

#include "GCodeProcessor.hpp"
#include "GCodeWriter.hpp"
#include "BoundingBox.hpp"
#include "Circle.hpp"
#include "ClipperUtils.hpp"
#include "LocalesUtils.hpp"
#include "PrintConfig.hpp"
#include "Triangulation.hpp"


namespace Slic3r
{

bool wipe_tower_sparse_layers_skipped(const PrintConfig &config)
{
    return config.wipe_tower_no_sparse_layers.value && config.timelapse_type.value != TimelapseType::tlSmooth;
}

bool wipe_tower_layer_is_sparse(const std::vector<WipeTower::ToolChangeResult> &layer_tool_changes)
{
    return layer_tool_changes.size() == 1 && layer_tool_changes.front().initial_tool == layer_tool_changes.front().new_tool;
}

std::vector<float> compute_compacted_wipe_tower_z(const std::vector<std::vector<WipeTower::ToolChangeResult>> &tool_changes,
                                                  float base_z)
{
    std::vector<float> tower_z(tool_changes.size(), base_z);
    float              last = base_z;
    for (size_t i = 0; i < tool_changes.size(); ++i) {
        if (! tool_changes[i].empty() && ! wipe_tower_layer_is_sparse(tool_changes[i]))
            last += tool_changes[i].front().layer_height;
        tower_z[i] = last;
    }
    return tower_z;
}

float last_emitted_wipe_tower_z_nonbbl(const std::vector<std::vector<WipeTower::ToolChangeResult>> &tool_changes,
                                       float                                                       base_z)
{
    float last = base_z;
    for (size_t i = 0; i < tool_changes.size(); ++i) {
        if (tool_changes[i].empty())
            continue;
        // Non-BBL always prints sparse layer 0 (ignore_sparse = sparse && m_layer_idx != 0).
        if (!(wipe_tower_layer_is_sparse(tool_changes[i]) && i != 0))
            last += tool_changes[i].front().layer_height;
    }
    return last;
}

bool wipe_tower_sparse_layers_combined(const PrintConfig &config)
{
    // Edge: no wrapping (clumping) detection option in this tree, so smooth timelapse is the only
    // other feature that needs a tower on every layer.
    return config.wipe_tower_sparse_layers_combination.value && ! wipe_tower_sparse_layers_skipped(config) &&
           config.timelapse_type.value != TimelapseType::tlSmooth;
}

bool wipe_tower_gcode_declares_height_first(const std::string &gcode, const std::string &tag)
{
    size_t start = 0;
    while (start < gcode.size()) {
        size_t end = gcode.find('\n', start);
        if (end == std::string::npos)
            end = gcode.size();
        size_t b = gcode.find_first_not_of(" \t", start);
        if (b < end) {
            if (gcode.compare(b, tag.size(), tag) == 0)
                return true;
            // An extruding move (G1/G2/G3 with a positive E) ends the search; travels and retracts do not.
            if (gcode[b] == 'G' && b + 1 < end && (gcode[b + 1] == '1' || gcode[b + 1] == '2' || gcode[b + 1] == '3') &&
                (b + 2 >= end || gcode[b + 2] == ' ')) {
                const size_t comment = gcode.find(';', b);
                const size_t code_end = std::min(end, comment);
                for (size_t e = gcode.find('E', b); e < code_end; e = gcode.find('E', e + 1))
                    if (e + 1 < code_end && gcode[e - 1] == ' ' && gcode[e + 1] != '-')
                        return false;
            }
        }
        start = end + 1;
    }
    return false;
}

bool wipe_tower_layer_is_combined_away(const std::vector<WipeTower::ToolChangeResult> &layer_tool_changes)
{
    return ! layer_tool_changes.empty() && layer_tool_changes.front().combined_away;
}

std::vector<char> combine_sparse_wipe_tower_layers(std::vector<float>       &layer_height,
                                                   const std::vector<char>  &layer_is_sparse,
                                                   const std::vector<float> &max_layer_height,
                                                   size_t                    first_layer_idx)
{
    assert(layer_is_sparse.size() == layer_height.size() && max_layer_height.size() == layer_height.size());
    std::vector<char> combined_away(layer_height.size(), 0);
    float             pending_height = 0.f; // what the layers folded away so far add up to
    for (size_t i = 0; i < layer_height.size(); ++i) {
        // A toolchange has to purge at its own z, so it neither folds away nor takes over the run
        // below it - and a run always flushes on its own last layer, so nothing is ever pending here.
        // Edge: first_layer_idx is size_t(-1) when no layer was marked first, and nothing combines then.
        if (! layer_is_sparse[i] || first_layer_idx == size_t(-1) || i <= first_layer_idx) {
            pending_height = 0.f;
            continue;
        }
        const float merged = pending_height + layer_height[i];
        // Hand the run on only if the next layer can swallow the whole thing; a layer already past
        // the cap is left alone rather than shrunk.
        const bool next_takes_it = i + 1 < layer_height.size() && layer_is_sparse[i + 1] &&
                                   merged + layer_height[i + 1] <= max_layer_height[i + 1] + float(EPSILON);
        if (next_takes_it) {
            combined_away[i] = 1;
            pending_height   = merged;
        } else {
            layer_height[i] = merged;
            pending_height  = 0.f;
        }
    }
    return combined_away;
}

static const double wipe_tower_wall_infill_overlap = 0.0;
// Orca #15035 (Bambu Studio): the flat spiral ironing at a wall gap (prime_tower_flat_ironing).
static constexpr float flat_iron_speed = 10.f * 60.f;
// Rib wall path resolution and fillet sampling, as in Bambu Studio's WipeTower.cpp.
static constexpr double WIPE_TOWER_RESOLUTION         = 0.1;
static const double     SCALED_WIPE_TOWER_RESOLUTION  = WIPE_TOWER_RESOLUTION / SCALING_FACTOR;
static const double     WT_SIMPLIFY_TOLERANCE_SCALED  = 0.001 / SCALING_FACTOR;
static constexpr int    arc_fit_size                  = 20;

// Bambu Studio WipeTower.cpp:282 generate_rectange(): the band of half-width `offset` around a line.
static Polygon generate_rectange(const Line &line, coord_t offset)
{
    Point p1 = line.a;
    Point p2 = line.b;

    double dx = p2.x() - p1.x();
    double dy = p2.y() - p1.y();

    double length = std::sqrt(dx * dx + dy * dy);

    double ux = dx / length;
    double uy = dy / length;

    double vx = -uy;
    double vy = ux;

    double ox = vx * offset;
    double oy = vy * offset;

    Points rect;
    rect.resize(4);
    rect[0] = {p1.x() + ox, p1.y() + oy};
    rect[1] = {p1.x() - ox, p1.y() - oy};
    rect[2] = {p2.x() - ox, p2.y() - oy};
    rect[3] = {p2.x() + ox, p2.y() + oy};
    Polygon poly(rect);
    return poly;
}

// Bambu Studio WipeTower.cpp:633 generate_rectange_polygon().
static Polygon generate_rectange_polygon(const Vec2f &wt_box_min, const Vec2f &wt_box_max)
{
    Polygon res;
    res.points.push_back(scaled(wt_box_min));
    res.points.push_back(scaled(Vec2f{wt_box_max[0], wt_box_min[1]}));
    res.points.push_back(scaled(wt_box_max));
    res.points.push_back(scaled(Vec2f{wt_box_min[0], wt_box_max[1]}));
    return res;
}

// One straight or arc piece of a wall path (Bambu Studio WipeTower.cpp:311 Segment).
struct WallSegment
{
    Vec2f      start;
    Vec2f      end;
    bool       is_arc = false;
    ArcSegment arcsegment;
    WallSegment(const Vec2f &s, const Vec2f &e) : start(s), end(e) {}
};

inline float align_round(float value, float base)
{
    return std::round(value / base) * base;
}

inline float align_ceil(float value, float base)
{
    return std::ceil(value / base) * base;
}

inline float align_floor(float value, float base)
{
    return std::floor((value) / base) * base;
}

static bool is_valid_gcode(const std::string &gcode)
{
    int  str_size    = gcode.size();
    int  start_index = 0;
    int  end_index   = 0;
    bool is_valid    = false;
    while (end_index < str_size) {
        if (gcode[end_index] != '\n') {
            end_index++;
            continue;
        }

        if (end_index > start_index) {
            std::string line_str = gcode.substr(start_index, end_index - start_index);
            line_str.erase(0, line_str.find_first_not_of(" "));
            line_str.erase(line_str.find_last_not_of(" ") + 1);
            if (!line_str.empty() && line_str[0] != ';') {
                is_valid = true;
                break;
            }
        }

        start_index = end_index + 1;
        end_index   = start_index;
    }

    return is_valid;
}

const char* flush_planner_queue_command(GCodeFlavor flavor)
{
    return flavor == gcfKlipper ? "M400\n" : "G4 S0\n";
}

std::string wait_command(GCodeFlavor flavor, float seconds)
{
    if (flavor == gcfKlipper)
        return "G4 P" + std::to_string(std::lround(seconds * 1000.f)) + "\n";
    return "G4 S" + Slic3r::float_to_string_decimal_point(seconds, 3) + "\n";
}

// HEIGHT/WIDTH tag values. std::to_string is sprintf("%f") and follows LC_NUMERIC, so a
// comma-decimal language emitted "0,300000" and GCodeProcessor (C locale) dropped the tag.
// Six decimals match %f, so the C-locale text is byte-identical to std::to_string.
static std::string analyzer_tag_value(double value)
{
    return Slic3r::float_to_string_decimal_point(value, 6);
}

class WipeTowerWriter
{
public:
	WipeTowerWriter(float layer_height, float line_width, GCodeFlavor flavor, const std::vector<WipeTower::FilamentParameters>& filament_parameters, bool has_nozzle_rack = false, bool bbl_gcode_tags = true) :
		m_current_pos(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()),
		m_current_z(0.f),
		m_current_feedrate(0.f),
		m_layer_height(layer_height),
		m_extrusion_flow(0.f),
		m_preview_suppressed(false),
		m_elapsed_time(0.f),
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_default_analyzer_line_width(line_width),
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_gcode_flavor(flavor),
        m_filpar(filament_parameters),
        m_has_nozzle_rack(has_nozzle_rack)
        {
            // ORCA: This class is only used by BBL printers, so set the parameter appropriately.
            // This fixes an issue where the wipe tower was using BBL tags resulting in statistics for purging in the purge tower not being displayed.
            // Edge: since Orca #12781 a non-Bambu printer can pick this tower too (wipe_tower_type = Type 1);
            // it gets the tags its G-code export reads (WipeTower::set_bbl_gcode_tags).
            GCodeProcessor::s_IsBBLPrinter = bbl_gcode_tags;
            // adds tag for analyzer:
            std::ostringstream str;
            str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) << analyzer_tag_value(m_layer_height) << "\n"; // don't rely on GCodeAnalyzer knowing the layer height - it knows nothing at priming
            str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) << ExtrusionEntity::role_to_string(erWipeTower) << "\n";
            m_gcode += str.str();
            change_analyzer_line_width(line_width);
    }

    WipeTowerWriter& change_analyzer_line_width(float line_width) {
        // adds tag for analyzer:
        std::stringstream str;
        str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) << analyzer_tag_value(line_width) << "\n";
        m_gcode += str.str();
        return *this;
    }

#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    WipeTowerWriter& change_analyzer_mm3_per_mm(float len, float e) {
        static const float area = float(M_PI) * 1.75f * 1.75f / 4.f;
        float mm3_per_mm = (len == 0.f ? 0.f : area * e / len);
        // adds tag for processor:
        std::stringstream str;
        str << ";" << GCodeProcessor::Mm3_Per_Mm_Tag << mm3_per_mm << "\n";
        m_gcode += str.str();
        return *this;
    }
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

	WipeTowerWriter& 			 set_initial_position(const Vec2f &pos, float width = 0.f, float depth = 0.f, float internal_angle = 0.f) {
        m_wipe_tower_width = width;
        m_wipe_tower_depth = depth;
        m_internal_angle = internal_angle;
		m_start_pos = this->rotate(pos);
		m_current_pos = pos;
		return *this;
	}

    WipeTowerWriter&				 set_initial_tool(size_t tool) { m_current_tool = tool; return *this; }

    // Rib wall: shift of the whole output (Bambu Studio's rib_offset), set before the initial position.
    WipeTowerWriter&            set_origin_offset(const Vec2f &offset) { m_origin_offset = offset; return *this; }
    WipeTowerWriter&            set_arc_fitting(bool enable) { m_enable_arc_fitting = enable; return *this; }
    // Bambu Studio 2.9.1 prime_tower_acceleration (WipeTower.cpp:1943-1950, 855-858, 934-937): an M204
    // before the first extrusion after a travel and before the first travel after an extrusion. 0 for
    // print_accel (the default) writes none, as before.
    WipeTowerWriter&            set_accelerations(unsigned int print_accel, unsigned int travel_accel)
    {
        m_print_accel  = print_accel;
        m_travel_accel = travel_accel;
        return *this;
    }

	WipeTowerWriter&				 set_z(float z)
		{ m_current_z = z; return *this; }

    // Bambu Studio 2.9.1 LimitFlow: the filament speed (mm/s) extrusions are capped at instead of the filament's
    // max_e_speed; 0 restores the normal cap.
    WipeTowerWriter&            set_e_speed_limit(float e_speed) { m_e_speed_limit = e_speed; return *this; }

	WipeTowerWriter& 			 set_extrusion_flow(float flow)
		{ m_extrusion_flow = flow; return *this; }

	WipeTowerWriter&				 set_y_shift(float shift) {
        m_current_pos.y() -= shift-m_y_shift;
        m_y_shift = shift;
        return (*this);
    }

    WipeTowerWriter&            disable_linear_advance() {
        if (m_gcode_flavor == gcfKlipper)
            m_gcode += "SET_PRESSURE_ADVANCE ADVANCE=0\n";
        else if (m_gcode_flavor == gcfRepRapFirmware)
            m_gcode += std::string("M572 D") + std::to_string(m_current_tool) + " S0\n";
        else
            m_gcode += "M900 K0\n";

        return *this;
    }

	// Suppress / resume G-code preview in Slic3r. Slic3r will have difficulty to differentiate the various
	// filament loading and cooling moves from normal extrusion moves. Therefore the writer
	// is asked to suppres output of some lines, which look like extrusions.
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    WipeTowerWriter& suppress_preview() { change_analyzer_line_width(0.f); m_preview_suppressed = true; return *this; }
    WipeTowerWriter& resume_preview() { change_analyzer_line_width(m_default_analyzer_line_width); m_preview_suppressed = false; return *this; }
#else
    WipeTowerWriter& 			 suppress_preview() { m_preview_suppressed = true; return *this; }
	WipeTowerWriter& 			 resume_preview()   { m_preview_suppressed = false; return *this; }
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

	WipeTowerWriter& 			 feedrate(float f)
	{
        if (f != m_current_feedrate) {
			m_gcode += "G1" + set_format_F(f) + "\n";
            m_current_feedrate = f;
        }
		return *this;
	}

	const std::string&   gcode() const { return m_gcode; }
	const std::vector<WipeTower::Extrusion>& extrusions() const { return m_extrusions; }
	float                x()     const { return m_current_pos.x(); }
	float                y()     const { return m_current_pos.y(); }
	const Vec2f& 		 pos()   const { return m_current_pos; }
	const Vec2f	 		 start_pos_rotated() const { return m_start_pos; }
	const Vec2f  		 pos_rotated() const { return this->rotate(m_current_pos); }
	float 				 elapsed_time() const { return m_elapsed_time; }
    float                get_and_reset_used_filament_length() { float temp = m_used_filament_length; m_used_filament_length = 0.f; return temp; }

	// Extrude with an explicitely provided amount of extrusion.
	WipeTowerWriter& extrude_explicit(float x, float y, float e, float f = 0.f, bool record_length = false, bool limit_volumetric_flow = true)
	{
		if (x == m_current_pos.x() && y == m_current_pos.y() && e == 0.f && (f == 0.f || f == m_current_feedrate))
			// Neither extrusion nor a travel move.
			return *this;

		float dx = x - m_current_pos.x();
		float dy = y - m_current_pos.y();
        float len = std::sqrt(dx*dx+dy*dy);
        if (record_length)
            m_used_filament_length += e;

		// Now do the "internal rotation" with respect to the wipe tower center
		Vec2f rotated_current_pos(this->pos_rotated());
		Vec2f rot(this->rotate(Vec2f(x,y)));                               // this is where we want to go

        if (! m_preview_suppressed && e > 0.f && len > 0.f) {
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
            change_analyzer_mm3_per_mm(len, e);
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
            // Width of a squished extrusion, corrected for the roundings of the squished extrusions.
			// This is left zero if it is a travel move.
            float width = e * m_filpar[0].filament_area / (len * m_layer_height);
			// Correct for the roundings of a squished extrusion.
			width += m_layer_height * float(1. - M_PI / 4.);
			if (m_extrusions.empty() || m_extrusions.back().pos != rotated_current_pos)
				m_extrusions.emplace_back(WipeTower::Extrusion(rotated_current_pos, 0, m_current_tool));
			m_extrusions.emplace_back(WipeTower::Extrusion(rot, width, m_current_tool));
		}

        m_gcode += acceleration_for(e);
		m_gcode += "G1";
        if (std::abs(rot.x() - rotated_current_pos.x()) > (float)EPSILON)
			m_gcode += set_format_X(rot.x());

        if (std::abs(rot.y() - rotated_current_pos.y()) > (float)EPSILON)
			m_gcode += set_format_Y(rot.y());


		if (e != 0.f)
			m_gcode += set_format_E(e);

		if (f != 0.f && f != m_current_feedrate) {
            if (limit_volumetric_flow) {
                float e_speed = e / (((len == 0.f) ? std::abs(e) : len) / f * 60.f);
                // Bambu Studio LimitFlow::LimitRammingFlow(NC): the ramming lines are capped by the ramming speed.
                const float limit = m_e_speed_limit > 0.f ? m_e_speed_limit : m_filpar[m_current_tool].max_e_speed;
                f /= std::max(1.f, e_speed / limit);
            }
			m_gcode += set_format_F(f);
        }

        m_current_pos.x() = x;
        m_current_pos.y() = y;

		// Update the elapsed time with a rough estimate.
        m_elapsed_time += ((len == 0.f) ? std::abs(e) : len) / m_current_feedrate * 60.f;
		m_gcode += "\n";
		return *this;
	}

	WipeTowerWriter& extrude_explicit(const Vec2f &dest, float e, float f = 0.f, bool record_length = false, bool limit_volumetric_flow = true)
		{ return extrude_explicit(dest.x(), dest.y(), e, f, record_length); }

    // Extrude along an arc with the extrusion amount given by m_extrusion_flow. Bambu Studio
    // WipeTower.cpp:821 extrude_arc_explicit(), minus its acceleration commands.
    WipeTowerWriter& extrude_arc(const ArcSegment &arc, float f = 0.f)
    {
        float x   = (float)unscale(arc.end_point).x();
        float y   = (float)unscale(arc.end_point).y();
        float len = unscaled<float>(arc.length);
        float e   = len * m_extrusion_flow;
        if (len < (float) EPSILON && e == 0.f && (f == 0.f || f == m_current_feedrate))
            // Neither extrusion nor a travel move.
            return *this;
        m_used_filament_length += e;

        // Now do the "internal rotation" with respect to the wipe tower center
        Vec2f rotated_current_pos(this->pos_rotated());
        Vec2f rot(this->rotate(Vec2f(x, y))); // this is where we want to go

        if (!m_preview_suppressed && e > 0.f && len > 0.f) {
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
            change_analyzer_mm3_per_mm(len, e);
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
            // Width of a squished extrusion, corrected for the roundings of the squished extrusions.
            float width = e * m_filpar[0].filament_area / (len * m_layer_height);
            // Correct for the roundings of a squished extrusion.
            width += m_layer_height * float(1. - M_PI / 4.);
            if (m_extrusions.empty() || m_extrusions.back().pos != rotated_current_pos)
                m_extrusions.emplace_back(WipeTower::Extrusion(rotated_current_pos, 0, m_current_tool));
            for (int j = 0; j < arc_fit_size; j++) {
                float cur_angle = arc.polar_start_theta + (float) j / arc_fit_size * arc.angle_radians;
                if (cur_angle > 2 * PI)
                    cur_angle -= 2 * PI;
                else if (cur_angle < 0)
                    cur_angle += 2 * PI;
                Point tmp = arc.center + Point{arc.radius * std::cos(cur_angle), arc.radius * std::sin(cur_angle)};
                m_extrusions.emplace_back(WipeTower::Extrusion(this->rotate(unscaled<float>(tmp)), width, m_current_tool));
            }
            m_extrusions.emplace_back(WipeTower::Extrusion(rot, width, m_current_tool));
        }

        m_gcode += acceleration_for(e);
        m_gcode += arc.direction == ArcDirection::Arc_Dir_CCW ? "G3" : "G2";
        const Vec2f center_offset = this->rotate(unscaled<float>(arc.center)) - rotated_current_pos;
        m_gcode += set_format_X(rot.x());
        m_gcode += set_format_Y(rot.y());
        m_gcode += " I" + Slic3r::float_to_string_decimal_point(center_offset.x(), 3);
        m_gcode += " J" + Slic3r::float_to_string_decimal_point(center_offset.y(), 3);
        if (e != 0.f)
            m_gcode += set_format_E(e);
        if (f != 0.f && f != m_current_feedrate) {
            float e_speed = e / (((len == 0.f) ? std::abs(e) : len) / f * 60.f);
            f /= std::max(1.f, e_speed / m_filpar[m_current_tool].max_e_speed);
            m_gcode += set_format_F(f);
        }

        m_current_pos.x() = x;
        m_current_pos.y() = y;

        // Update the elapsed time with a rough estimate.
        m_elapsed_time += ((len == 0.f) ? std::abs(e) : len) / m_current_feedrate * 60.f;
        m_gcode += "\n";
        return *this;
    }

    // Extrude a closed wall once around, starting from the vertex closest to the current position.
    // Bambu Studio WipeTower.cpp:1030 polygon() (brim loops, pre_simplify) and :1272 generate_path()
    // (the wall itself); a single closed polygon has no gaps, so generate_path()'s retracts between
    // disjoint pieces never fire and are left out.
    WipeTowerWriter& polygon(const Polygon &wall_polygon, const float f, bool pre_simplify)
    {
        Polyline pl = to_polyline(wall_polygon);
        if (pre_simplify)
            pl.simplify(WT_SIMPLIFY_TOLERANCE_SCALED);
        std::vector<WallSegment> segments;
        if (m_enable_arc_fitting) {
            pl.simplify_by_fitting_arc(SCALED_WIPE_TOWER_RESOLUTION);
            for (const PathFittingData &fit : pl.fitting_result) {
                if (fit.path_type == EMovePathType::Linear_move) {
                    for (size_t j = fit.start_point_index; j < fit.end_point_index; ++j)
                        segments.emplace_back(unscaled<float>(pl.points[j]), unscaled<float>(pl.points[j + 1]));
                } else if (fit.path_type == EMovePathType::Arc_move_ccw || fit.path_type == EMovePathType::Arc_move_cw) {
                    segments.emplace_back(unscaled<float>(pl.points[fit.start_point_index]), unscaled<float>(pl.points[fit.end_point_index]));
                    segments.back().is_arc     = true;
                    segments.back().arcsegment = fit.arc_data;
                }
            }
        } else {
            pl.simplify(SCALED_WIPE_TOWER_RESOLUTION);
            for (size_t j = 0; j + 1 < pl.points.size(); ++j)
                segments.emplace_back(unscaled<float>(pl.points[j]), unscaled<float>(pl.points[j + 1]));
        }
        if (segments.empty())
            return *this;

        int   index_of_closest = 0;
        float min_distance     = std::numeric_limits<float>::max();
        for (int i = 0; i < int(segments.size()); ++i) {
            const float distance = (segments[i].start - m_current_pos).squaredNorm();
            if (distance < min_distance) {
                min_distance     = distance;
                index_of_closest = i;
            }
        }
        int i = index_of_closest;
        travel(segments[i].start); // travel to the closest points
        do {
            segments[i].is_arc ? extrude_arc(segments[i].arcsegment, f) : extrude(segments[i].end, f);
            i = (i + 1) % int(segments.size());
        } while (i != index_of_closest);
        return *this;
    }

	// Travel to a new XY position. f=0 means use the current value.
	WipeTowerWriter& travel(float x, float y, float f = 0.f)
		{ return extrude_explicit(x, y, 0.f, f); }

	WipeTowerWriter& travel(const Vec2f &dest, float f = 0.f) 
		{ return extrude_explicit(dest.x(), dest.y(), 0.f, f); }

	// Extrude a line from current position to x, y with the extrusion amount given by m_extrusion_flow.
	WipeTowerWriter& extrude(float x, float y, float f = 0.f)
	{
		float dx = x - m_current_pos.x();
		float dy = y - m_current_pos.y();
        return extrude_explicit(x, y, std::sqrt(dx*dx+dy*dy) * m_extrusion_flow, f, true);
	}

	WipeTowerWriter& extrude(const Vec2f &dest, const float f = 0.f) 
		{ return extrude(dest.x(), dest.y(), f); }

    WipeTowerWriter& rectangle(const Vec2f& ld,float width,float height,const float f = 0.f)
    {
        Vec2f corners[4];
        corners[0] = ld;
        corners[1] = ld + Vec2f(width,0.f);
        corners[2] = ld + Vec2f(width,height);
        corners[3] = ld + Vec2f(0.f,height);
        int index_of_closest = 0;
        if (x()-ld.x() > ld.x()+width-x())    // closer to the right
            index_of_closest = 1;
        if (y()-ld.y() > ld.y()+height-y())   // closer to the top
            index_of_closest = (index_of_closest==0 ? 3 : 2);

        travel(corners[index_of_closest].x(), y());      // travel to the closest corner
        travel(x(),corners[index_of_closest].y());

        int i = index_of_closest;
        do {
            ++i;
            if (i==4) i=0;
            extrude(corners[i], f);
        } while (i != index_of_closest);
        return (*this);
    }

    WipeTowerWriter &rectangle_fill_box(const WipeTower* wipe_tower, const Vec2f &ld, float width, float height, const float f = 0.f)
    {
        bool need_change_flow = wipe_tower->need_thick_bridge_flow(ld.y());

        Vec2f corners[4];
        corners[0]           = ld;
        corners[1]           = ld + Vec2f(width, 0.f);
        corners[2]           = ld + Vec2f(width, height);
        corners[3]           = ld + Vec2f(0.f, height);
        int index_of_closest = 0;
        if (x() - ld.x() > ld.x() + width - x()) // closer to the right
            index_of_closest = 1;
        if (y() - ld.y() > ld.y() + height - y()) // closer to the top
            index_of_closest = (index_of_closest == 0 ? 3 : 2);

        travel(corners[index_of_closest].x(), y()); // travel to the closest corner
        travel(x(), corners[index_of_closest].y());

        int i = index_of_closest;
        bool flow_changed = false;
        do {
            ++i;
            if (i == 4) i = 0;
            if (need_change_flow) { 
                if (i == 1) {
                    // using bridge flow in bridge area, and add notes for gcode-check when flow changed
                    set_extrusion_flow(wipe_tower->extrusion_flow(0.2));
                    append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + analyzer_tag_value(0.2) + "\n");
                    flow_changed = true;
                } else if (i == 2 && flow_changed) {
                    set_extrusion_flow(wipe_tower->get_extrusion_flow());
                    append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + analyzer_tag_value(m_layer_height) + "\n");
                }
            }
            extrude(corners[i], f);
        } while (i != index_of_closest);
        return (*this);
    }

    // Orca #15035 (Bambu Studio): the inner perimeter of a fill box, or a single line when the box is thinner
    // than a line; finish_rect_wipe_path gets the wipe along the last side.
    WipeTowerWriter &rectangle_fill_box(const WipeTower *wipe_tower, const WipeTower::box_coordinates &fill_box,
                                        std::vector<Vec2f> &finish_rect_wipe_path, float perimeter_width, const float f = 0.f)
    {
        const float width  = fill_box.rd.x() - fill_box.ld.x();
        const float height = fill_box.ru.y() - fill_box.rd.y();
        if (height > perimeter_width - 1e-3f) {
            rectangle_fill_box(wipe_tower, fill_box.ld, width, height, f);
            Vec2f target = (pos() == fill_box.ld ? fill_box.rd : (pos() == fill_box.rd ? fill_box.ru : (pos() == fill_box.ru ? fill_box.lu : fill_box.ld)));
            finish_rect_wipe_path.emplace_back(pos());
            finish_rect_wipe_path.emplace_back(target);
        } else if (height > 1e-3f) {
            // Bambu Studio line(): from the nearer end.
            const bool from_left = (pos() - fill_box.ld).squaredNorm() <= (pos() - fill_box.rd).squaredNorm();
            travel(from_left ? fill_box.ld : fill_box.rd);
            extrude(from_left ? fill_box.rd : fill_box.ld, f);
            finish_rect_wipe_path.emplace_back(pos());
            finish_rect_wipe_path.emplace_back(from_left ? fill_box.ld : fill_box.rd);
        }
        return (*this);
    }

    // Orca #15035 (Bambu Studio): a flat spiral travel around center, ironing the blob at a wall gap.
    WipeTowerWriter &spiral_flat_ironing(const Vec2f &center, float area, float step_length, float feedrate)
    {
        if (step_length <= 0.f)
            return *this;
        const float edge_length = std::sqrt(area);
        Vec2f       box_max     = center + Vec2f{step_length, step_length};
        Vec2f       box_min     = center - Vec2f{step_length, step_length};
        int         n           = int(std::ceil(edge_length / step_length / 2.f));
        while (n-- > 0) {
            travel(box_max.x(), m_current_pos.y(), feedrate);
            travel(m_current_pos.x(), box_max.y(), feedrate);
            travel(box_min.x(), m_current_pos.y(), feedrate);
            travel(m_current_pos.x(), box_min.y(), feedrate);
            box_max += Vec2f{step_length, step_length};
            box_min -= Vec2f{step_length, step_length};
        }
        return *this;
    }

    // Orca #15035 (Bambu Studio generate_path()): a wall cut open at its gaps, from the piece nearest the
    // current position on, retracting over every gap.
    // allow_arcs: rib walls only; a rectangle wall stays straight lines (arc fitting rounds its corners).
    WipeTowerWriter &gap_wall(const Polylines &pieces, float f, float retract_length, float retract_speed, bool allow_arcs)
    {
        std::vector<WallSegment> segments;
        for (Polyline pl : pieces) {
            if (pl.points.size() < 2)
                continue;
            if (m_enable_arc_fitting && allow_arcs) {
                pl.simplify_by_fitting_arc(SCALED_WIPE_TOWER_RESOLUTION);
                for (const PathFittingData &fit : pl.fitting_result) {
                    if (fit.path_type == EMovePathType::Linear_move) {
                        for (size_t j = fit.start_point_index; j < fit.end_point_index; ++j)
                            segments.emplace_back(unscaled<float>(pl.points[j]), unscaled<float>(pl.points[j + 1]));
                    } else if (fit.path_type == EMovePathType::Arc_move_ccw || fit.path_type == EMovePathType::Arc_move_cw) {
                        segments.emplace_back(unscaled<float>(pl.points[fit.start_point_index]), unscaled<float>(pl.points[fit.end_point_index]));
                        segments.back().is_arc     = true;
                        segments.back().arcsegment = fit.arc_data;
                    }
                }
            } else {
                // A rectangle wall keeps its corners: simplifying would cut the corner next to a gap that
                // ends within the tolerance of it into a slanted wall line.
                if (allow_arcs)
                    pl.simplify(SCALED_WIPE_TOWER_RESOLUTION);
                for (size_t j = 0; j + 1 < pl.points.size(); ++j)
                    segments.emplace_back(unscaled<float>(pl.points[j]), unscaled<float>(pl.points[j + 1]));
            }
        }
        if (segments.empty())
            return *this;
        int   closest      = 0;
        float min_distance = std::numeric_limits<float>::max();
        for (int i = 0; i < int(segments.size()); ++i) {
            const float distance = (segments[i].start - m_current_pos).squaredNorm();
            if (distance < min_distance) {
                min_distance = distance;
                closest      = i;
            }
        }
        int i = closest;
        travel(segments[i].start);
        segments[i].is_arc ? extrude_arc(segments[i].arcsegment, f) : extrude(segments[i].end, f);
        for (i = (i + 1) % int(segments.size()); i != closest; i = (i + 1) % int(segments.size())) {
            if ((segments[i].start - m_current_pos).norm() > float(EPSILON)) {
                if (retract_length > 0.f)
                    retract(retract_length, retract_speed);
                travel(segments[i].start, 600.f);
                if (retract_length > 0.f)
                    retract(-retract_length, retract_speed);
            }
            segments[i].is_arc ? extrude_arc(segments[i].arcsegment, f) : extrude(segments[i].end, f);
        }
        return *this;
    }

    WipeTowerWriter& rectangle(const WipeTower::box_coordinates& box, const float f = 0.f)
    {
        rectangle(Vec2f(box.ld.x(), box.ld.y()),
                  box.ru.x() - box.lu.x(),
                  box.ru.y() - box.rd.y(), f);
        return (*this);
    }

	WipeTowerWriter& load(float e, float f = 0.f)
	{
		if (e == 0.f && (f == 0.f || f == m_current_feedrate))
			return *this;
		m_gcode += "G1";
		if (e != 0.f)
			m_gcode += set_format_E(e);
		if (f != 0.f && f != m_current_feedrate)
			m_gcode += set_format_F(f);
		m_gcode += "\n";
		return *this;
	}

	WipeTowerWriter& retract(float e, float f = 0.f)
		{ return load(-e, f); }

    // Push e mm of filament through the standing nozzle (tower interface extra prime), counted as used.
    WipeTowerWriter& prime(float e, float f)
    {
        if (e <= 0.f)
            return *this;
        m_used_filament_length += e;
        m_elapsed_time += e / f * 60.f;
        return load(e, f);
    }

    // A wall cut open by tower interface gaps: the pieces in wall order, starting with the one that
    // begins nearest to the current position, travelling over each gap.
    WipeTowerWriter& wall_pieces(const Polylines &pieces, float f)
    {
        std::vector<std::vector<Vec2f>> paths;
        for (Polyline pl : pieces) {
            pl.simplify(SCALED_WIPE_TOWER_RESOLUTION);
            if (pl.points.size() < 2)
                continue;
            std::vector<Vec2f> path;
            for (const Point &pt : pl.points)
                path.push_back(unscaled<float>(pt));
            paths.push_back(std::move(path));
        }
        if (paths.empty())
            return *this;
        size_t first = 0;
        for (size_t i = 1; i < paths.size(); ++i)
            if ((paths[i].front() - m_current_pos).squaredNorm() < (paths[first].front() - m_current_pos).squaredNorm())
                first = i;
        for (size_t k = 0; k < paths.size(); ++k) {
            const std::vector<Vec2f> &path = paths[(first + k) % paths.size()];
            travel(path.front());
            for (size_t i = 1; i < path.size(); ++i)
                extrude(path[i], f);
        }
        return *this;
    }

// Loads filament while also moving towards given points in x-axis (x feedrate is limited by cutting the distance short if necessary)
    WipeTowerWriter& load_move_x_advanced(float farthest_x, float loading_dist, float loading_speed, float max_x_speed = 50.f)
    {
        float time = std::abs(loading_dist / loading_speed); // time that the move must take
        float x_distance = std::abs(farthest_x - x());       // max x-distance that we can travel
        float x_speed = x_distance / time;                   // x-speed to do it in that time

        if (x_speed > max_x_speed) {
            // Necessary x_speed is too high - we must shorten the distance to achieve max_x_speed and still respect the time.
            x_distance = max_x_speed * time;
            x_speed = max_x_speed;
        }

        float end_point = x() + (farthest_x > x() ? 1.f : -1.f) * x_distance;
        return extrude_explicit(end_point, y(), loading_dist, x_speed * 60.f, false, false);
    }

	// Elevate the extruder head above the current print_z position.
	WipeTowerWriter& z_hop(float hop, float f = 0.f)
	{ 
		m_gcode += std::string("G1") + set_format_Z(m_current_z + hop);
		if (f != 0 && f != m_current_feedrate)
			m_gcode += set_format_F(f);
		m_gcode += "\n";
		return *this;
	}

	// Lower the extruder head back to the current print_z position.
	WipeTowerWriter& z_hop_reset(float f = 0.f) 
		{ return z_hop(0, f); }

	// Move to x1, +y_increment,
	// extrude quickly amount e to x2 with feed f.
	WipeTowerWriter& ram(float x1, float x2, float dy, float e0, float e, float f)
	{
		extrude_explicit(x1, m_current_pos.y() + dy, e0, f, true, false);
		extrude_explicit(x2, m_current_pos.y(), e, 0.f, true, false);
		return *this;
	}

	// Let the end of the pulled out filament cool down in the cooling tube
	// by moving up and down and moving the print head left / right
	// at the current Y position to spread the leaking material.
	WipeTowerWriter& cool(float x1, float x2, float e1, float e2, float f)
	{
		extrude_explicit(x1, m_current_pos.y(), e1, f, false, false);
		extrude_explicit(x2, m_current_pos.y(), e2, false, false);
		return *this;
	}

    WipeTowerWriter& set_tool(size_t tool)
	{
		m_current_tool = tool;
		return *this;
	}

	// Set extruder temperature, don't wait by default.
	WipeTowerWriter& set_extruder_temp(int temperature, bool wait = false)
	{
        m_gcode += "M" + std::to_string(wait ? 109 : 104) + " S" + std::to_string(temperature) + "\n";
        return *this;
    }

    // Wait for a period of time (seconds).
	WipeTowerWriter& wait(float time)
	{
        if (time==0.f)
            return *this;
        m_gcode += wait_command(m_gcode_flavor, time);
		return *this;
    }

	// Set speed factor override percentage.
	WipeTowerWriter& speed_override(int speed)
	{
        m_gcode += "M220 S" + std::to_string(speed) + "\n";
		return *this;
    }

	// Let the firmware back up the active speed override value.
	WipeTowerWriter& speed_override_backup()
    {
        // Ultra (H2C rack): BambuStudio WipeTower.cpp:1179-1185 emits this for gcfMarlinLegacy /
        // gcfMarlinFirmware; the gold H2C file carries one "M220 B" per toolchange. Orca had it
        // #if 0'd out ("BBL machine don't support speed backup"), which is wrong for the rack
        // machines. Gated on the rack so P1S/H2D output stays unchanged - see
        // docs/superpowers/specs/2026-09-07-h2c-rack-nozzle-change.md.
        if (m_has_nozzle_rack && (m_gcode_flavor == gcfMarlinLegacy || m_gcode_flavor == gcfMarlinFirmware))
            m_gcode += "M220 B\n";
		return *this;
    }

	// Let the firmware restore the active speed override value.
	WipeTowerWriter& speed_override_restore()
	{
	    // Ultra (H2C rack): see speed_override_backup above (BambuStudio WipeTower.cpp:1188-1194).
        if (m_has_nozzle_rack && (m_gcode_flavor == gcfMarlinLegacy || m_gcode_flavor == gcfMarlinFirmware))
            m_gcode += "M220 R\n";
		return *this;
    }

	// Set digital trimpot motor
	WipeTowerWriter& set_extruder_trimpot(int current)
	{
        // BBS: don't control trimpot
#if 0
        if (m_gcode_flavor == gcfRepRapSprinter || m_gcode_flavor == gcfRepRapFirmware)
            m_gcode += "M906 E";
        else
            m_gcode += "M907 E";
        m_gcode += std::to_string(current) + "\n";
#endif
		return *this;
    }

	WipeTowerWriter& flush_planner_queue()
	{
		// Only called at toolchange boundaries; format_line_M104 syncs temperature changes itself.
		// On Klipper M400 here only stalls the toolhead over the tower.
		if (m_gcode_flavor != gcfKlipper)
			m_gcode += flush_planner_queue_command(m_gcode_flavor);
		return *this;
	}

	// Reset internal extruder counter.
	WipeTowerWriter& reset_extruder()
	{ 
		m_gcode += "G92 E0\n";
		return *this;
	}

	WipeTowerWriter& comment_with_value(const char *comment, int value)
    {
        m_gcode += std::string(";") + comment + std::to_string(value) + "\n";
		return *this;
    }


    WipeTowerWriter& set_fan(unsigned speed)
	{
		if (speed == m_last_fan_speed)
			return *this;
		if (speed == 0)
			m_gcode += "M107\n";
        else
            m_gcode += "M106 S" + std::to_string(unsigned(255.0 * speed / 100.0)) + "\n";
		m_last_fan_speed = speed;
		return *this;
	}

	WipeTowerWriter& append(const std::string& text) { m_gcode += text; return *this; }

    const std::vector<Vec2f>& wipe_path() const
    {
        return m_wipe_path;
    }

    WipeTowerWriter& add_wipe_point(const Vec2f& pt)
    {
        m_wipe_path.push_back(rotate(pt));
        return *this;
    }

    WipeTowerWriter& add_wipe_point(float x, float y)
    {
        return add_wipe_point(Vec2f(x, y));
    }

    // Wipe back along a wall for wipe_dist (Bambu Studio WipeTower.cpp:1260 add_wipe_path()).
    WipeTowerWriter& add_wipe_path(const Polygon &polygon, double wipe_dist)
    {
        int      closest_idx = polygon.closest_point_index(scaled(m_current_pos));
        Polyline wipe_path   = polygon.split_at_index(closest_idx);
        wipe_path.reverse();
        for (int i = 0; i < int(wipe_path.size()); ++i) {
            if (wipe_dist < EPSILON) break;
            add_wipe_point(unscaled<float>(wipe_path[i]));
            if (i != 0) wipe_dist -= (unscaled(wipe_path[i]) - unscaled(wipe_path[i - 1])).norm();
        }
        return *this;
    }

private:
	Vec2f         m_start_pos;
	Vec2f         m_current_pos;
    std::vector<Vec2f>  m_wipe_path;
	float    	  m_current_z;
	float 	  	  m_current_feedrate;
    size_t        m_current_tool;
	float 		  m_layer_height;
	float 	  	  m_extrusion_flow;
	bool		  m_preview_suppressed;
	std::string   m_gcode;
	std::vector<WipeTower::Extrusion> m_extrusions;
	float         m_elapsed_time;
	float   	  m_internal_angle = 0.f;
	float		  m_y_shift = 0.f;
	float 		  m_wipe_tower_width = 0.f;
	float		  m_wipe_tower_depth = 0.f;
    unsigned      m_last_fan_speed = 0;
    int           current_temp = -1;
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    const float   m_default_analyzer_line_width;
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
    float         m_used_filament_length = 0.f;
    GCodeFlavor   m_gcode_flavor;
    bool          m_has_nozzle_rack = false;   // Ultra (H2C rack)
    bool          m_enable_arc_fitting = false;
    Vec2f         m_origin_offset = Vec2f::Zero();
    const std::vector<WipeTower::FilamentParameters>& m_filpar;
    unsigned int  m_print_accel       = 0;
    unsigned int  m_travel_accel      = 0;
    unsigned int  m_last_acceleration = 0;
    float         m_e_speed_limit     = 0.f;

    // Bambu Studio set_acceleration_impl() (WipeTower.cpp:1532), M204 S for the flavours the Type 1 tower
    // serves; nothing while the prime tower acceleration is off.
    std::string   acceleration_for(float e)
    {
        if (m_print_accel == 0)
            return std::string();
        const unsigned int acceleration = e == 0.f ? m_travel_accel : m_print_accel;
        if (acceleration == 0 || acceleration == m_last_acceleration)
            return std::string();
        m_last_acceleration = acceleration;
        return "M204 S" + std::to_string(acceleration) + "\n";
    }

	std::string   set_format_X(float x)
    {
        m_current_pos.x() = x;
        return " X" + Slic3r::float_to_string_decimal_point(x, 3);
	}

	std::string   set_format_Y(float y) {
        m_current_pos.y() = y;
        return " Y" + Slic3r::float_to_string_decimal_point(y, 3);
	}

	std::string   set_format_Z(float z) {
        return " Z" + Slic3r::float_to_string_decimal_point(z, 3);
	}

	std::string   set_format_E(float e) {
        return " E" + Slic3r::float_to_string_decimal_point(e, 4);
	}

	std::string   set_format_F(float f) {
        char buf[64];
        sprintf(buf, " F%d", int(floor(f + 0.5f)));
        m_current_feedrate = f;
        return buf;
	}

	WipeTowerWriter& operator=(const WipeTowerWriter &rhs);

	// Rotate the point around center of the wipe tower about given angle (in degrees)
	Vec2f rotate(Vec2f pt) const
	{
		pt.x() -= m_wipe_tower_width / 2.f;
		pt.y() += m_y_shift - m_wipe_tower_depth / 2.f;
	    double angle = m_internal_angle * float(M_PI/180.);
	    double c = cos(angle);
	    double s = sin(angle);
	    Vec2f out(float(pt.x() * c - pt.y() * s) + m_wipe_tower_width / 2.f, float(pt.x() * s + pt.y() * c) + m_wipe_tower_depth / 2.f);
	    // Only a rib wall moves the tower, so every other output stays bit-identical.
	    if (m_origin_offset != Vec2f::Zero())
	        out += m_origin_offset;
	    return out;
	}

}; // class WipeTowerWriter



WipeTower::ToolChangeResult WipeTower::construct_tcr(WipeTowerWriter& writer,
                                                     bool priming,
                                                     size_t old_tool,
                                                     bool is_finish,
                                                     bool is_tool_change,
                                                     float purge_volume,
                                                     bool is_contact) const
{
    ToolChangeResult result;
    result.priming      = priming;
    result.initial_tool = int(old_tool);
    result.new_tool     = int(m_current_tool);
    result.print_z      = m_z_pos;
    result.layer_height = m_layer_height;
    result.elapsed_time = writer.elapsed_time();
    result.start_pos    = writer.start_pos_rotated();
    result.end_pos      = priming ? writer.pos() : writer.pos_rotated();
    result.gcode        = std::move(writer.gcode());
    result.extrusions   = std::move(writer.extrusions());
    result.wipe_path    = std::move(writer.wipe_path());
    result.is_finish_first = is_finish;
    result.is_tool_change        = is_tool_change;
    result.tool_change_start_pos = is_tool_change ? result.start_pos : Vec2f::Zero();
    result.is_contact            = is_contact;
    // BBS
    result.purge_volume = purge_volume;
    return result;
}

WipeTower::ToolChangeResult WipeTower::construct_block_tcr(WipeTowerWriter &writer, size_t filament_id, bool is_finish) const
{
    ToolChangeResult result;
    result.priming         = false;
    result.initial_tool    = int(filament_id);
    result.new_tool        = int(filament_id);
    result.print_z         = m_z_pos;
    result.layer_height    = m_layer_height;
    result.elapsed_time    = writer.elapsed_time();
    result.start_pos       = writer.start_pos_rotated();
    result.end_pos         = writer.pos_rotated();
    result.gcode           = std::move(writer.gcode());
    result.extrusions      = std::move(writer.extrusions());
    result.wipe_path       = std::move(writer.wipe_path());
    result.is_finish_first = is_finish;
    return result;
}

// BBS
const std::map<float, float> WipeTower::min_depth_per_height = {
    {100.f, 20.f}, {250.f, 40.f}
};

float WipeTower::get_limit_depth_by_height(float max_height)
{
    float min_wipe_tower_depth = 0.f;
    auto  iter                 = WipeTower::min_depth_per_height.begin();
    while (iter != WipeTower::min_depth_per_height.end()) {
        auto curr_height_to_depth = *iter;

        // This is the case that wipe tower height is lower than the first min_depth_to_height member.
        if (curr_height_to_depth.first >= max_height) {
            min_wipe_tower_depth = curr_height_to_depth.second;
            break;
        }

        iter++;

        // If curr_height_to_depth is the last member, use its min_depth.
        if (iter == WipeTower::min_depth_per_height.end()) {
            min_wipe_tower_depth = curr_height_to_depth.second;
            break;
        }

        // If wipe tower height is between the current and next member, set the min_depth as linear interpolation between them
        auto next_height_to_depth = *iter;
        if (next_height_to_depth.first > max_height) {
            float height_base    = curr_height_to_depth.first;
            float height_diff    = next_height_to_depth.first - curr_height_to_depth.first;
            float min_depth_base = curr_height_to_depth.second;
            float depth_diff     = next_height_to_depth.second - curr_height_to_depth.second;

            min_wipe_tower_depth = min_depth_base + (max_height - curr_height_to_depth.first) / height_diff * depth_diff;
            break;
        }
    }
    return min_wipe_tower_depth;
}

float WipeTower::get_auto_brim_by_height(float max_height)
{
    if (max_height < 100)
        return max_height / 100.f * 8.f;
    return 8.f;
}

bool WipeTower::outline_fills_bounding_box(const Polygons &polygons)
{
    if (polygons.empty())
        return true;
    const BoundingBox bed = get_extents(polygons);
    // The tolerance is relative because an exact rectangle loses a few ulps once the areas are
    // squared world coordinates.
    double area = 0.;
    for (const Polygon &poly : polygons)
        area += std::abs(poly.area());
    const double bed_area = double(bed.size().x()) * double(bed.size().y());
    return area >= bed_area * (1. - EPSILON);
}

// Orca #15146 (move_box_inside_box -> move_box_inside_polygon).
Vec2f WipeTower::move_box_inside_polygon(const BoundingBox &box, const Polygons &polygons, coord_t offset)
{
    if (polygons.empty())
        return Vec2f{0.f, 0.f};

    const BoundingBox bed = get_extents(polygons);
    // No position fits the footprint.
    if (box.size().x() >= bed.size().x() - 2 * offset || box.size().y() >= bed.size().y() - 2 * offset)
        return Vec2f{0.f, 0.f};

    // Clamp against the bounding box first, moving only along the axis that is violated so a dragged
    // prime tower slides along the bed edge instead of jumping inwards.
    Point shift(0, 0);
    for (int axis = 0; axis < 2; ++axis) {
        if (box.max[axis] > bed.max[axis] - offset)
            shift[axis] = (bed.max[axis] - offset) - box.max[axis];
        else if (box.min[axis] < bed.min[axis] + offset)
            shift[axis] = (bed.min[axis] + offset) - box.min[axis];
    }

    // A bed that fills its own bounding box is fully clamped by that, so every rectangular bed - all
    // but the delta-style profiles - stops here, including when a negative margin lets the footprint
    // hang over the edge.
    if (outline_fills_bounding_box(polygons))
        return unscaled<float>(shift);

    // Clamp a negative margin (an auto brim width that has not been resolved yet) to zero: padding by
    // it would shrink the footprint and hand back a position the validation still rejects. The
    // epsilon lets the move's round trip through millimeters land on the outline without counting as
    // a violation.
    BoundingBox padded = box.inflated(std::max<coord_t>(offset, 0) - SCALED_EPSILON);
    padded.translate(shift);
    auto fits = [&padded, &polygons](const Point &move) {
        BoundingBox moved = padded;
        moved.translate(move);
        return diff(Polygons{moved.polygon()}, polygons).empty();
    };
    if (fits(Point(0, 0)))
        return unscaled<float>(shift);

    // Walk towards the middle of the bed. On every non-rectangular bed we ship, the fitting positions
    // form a convex region around it, so bisecting stops just inside the outline.
    Point lo(0, 0), hi = bed.center() - padded.center();
    if (!fits(hi))
        return unscaled<float>(shift);
    for (int i = 0; i < 12; ++i) {
        const Point mid = (lo + hi) / 2;
        if (fits(mid))
            hi = mid;
        else
            lo = mid;
    }
    return unscaled<float>(Point(shift + hi));
}

static const double wrapping_wipe_tower_depth = 10;
static const std::map<float, float> nozzle_diameter_to_nozzle_change_width{{0.2f, 0.5f}, {0.4f, 1.0f}, {0.6f, 1.2f}, {0.8f, 1.4f}};

float WipeTower::estimate_brim_real_width(float brim_width, float nozzle_diameter, float first_layer_height, bool type2, float line_width)
{
    if (brim_width <= 0.f)
        return brim_width;
    const float spacing = (line_width > 0.f ? line_width : nozzle_diameter * 1.25f) - first_layer_height * float(1. - M_PI_4); // Width_To_Nozzle_Ratio
    if (spacing <= EPSILON)
        return brim_width;
    const int loops_num = int((brim_width + spacing / 2.f) / spacing);
    return loops_num * spacing + (type2 ? 0.f : spacing / 2.f);
}

float WipeTower::filament_max_layer_height(const PrintConfig &config, size_t idx)
{
    // Edge: physical_extruder_for_filament() is the filament->nozzle map Slicing.cpp reads these
    // limits through (a toolchanger such as the U1 maps filament i to nozzle i without filament_map).
    const size_t nozzle           = physical_extruder_for_filament(config, (unsigned int) idx + 1);
    const float  max_layer_height = config.max_layer_height.values.empty() ? 0.f : float(config.max_layer_height.get_at(nozzle));
    const float  nozzle_diameter  = config.nozzle_diameter.values.empty() ? 0.4f : float(config.nozzle_diameter.get_at(nozzle));
    return max_layer_height > 0.f ? max_layer_height : 0.75f * nozzle_diameter;
}

float WipeTower::get_wrapping_detection_depth()
{
    return float(wrapping_wipe_tower_depth);
}

float WipeTower::nozzle_change_perimeter_width(float nozzle_diameter)
{
    auto it = nozzle_diameter_to_nozzle_change_width.find(nozzle_diameter);
    return it != nozzle_diameter_to_nozzle_change_width.end() ? it->second : 2.f * nozzle_diameter * 1.25f;
}

float WipeTower::bambu_prime_volume(const PrintConfig &config, unsigned int filament, bool hotend_change)
{
    if (config.prime_volume_mode == PrimeVolumeMode::pvmSaving)
        return 15.f;
    if (hotend_change && !config.filament_prime_volume_nc.values.empty())
        return float(config.filament_prime_volume_nc.get_at(filament));
    if (!config.filament_prime_volume.values.empty())
        return float(config.filament_prime_volume.get_at(filament));
    return float(config.prime_volume.value);
}

float WipeTower::estimate_tower_blocks_depth(const std::vector<PurgeEstimate> &purges, float width, float layer_height, float nozzle_diameter, float extra_spacing, float tower_line_width)
{
    if (purges.empty() || layer_height < EPSILON || nozzle_diameter < EPSILON)
        return 0.f;
    const float pw         = tower_line_width > 0.f ? tower_line_width : nozzle_diameter * 1.25f; // Width_To_Nozzle_Ratio
    const float ncpw       = nozzle_change_perimeter_width(nozzle_diameter);
    const float line_width = width - 2.f * pw;
    if (line_width <= EPSILON)
        return 0.f;
    // Line cross-section as volume_to_length() sees it; the infill gap stretches the perimeter
    // width by the configured ratio and nozzle-change lines keep their own width
    // (calc_block_infill_gap).
    auto        line_area   = [layer_height](float w) { return layer_height * (w - layer_height * float(1. - M_PI_4)); };
    const float extra_width = (extra_spacing - 1.f) * pw;
    const float gap         = pw + extra_width;
    const float nc_gap      = ncpw + extra_width;
    // A layer purges into at most (filaments - 1) targets, so a category holding every filament
    // never sees its smallest purge (the layer's first filament) in its worst layer.
    struct Block { float depth = 0.f; float min_purge = 0.f; size_t filaments = 0; };
    std::map<int, Block> blocks;
    for (const PurgeEstimate &purge : purges) {
        Block      &block       = blocks[purge.category];
        const float purge_depth = std::ceil(purge.prime_volume / line_area(pw) / line_width) * gap;
        block.min_purge         = block.filaments == 0 ? purge_depth : std::min(block.min_purge, purge_depth);
        block.depth += purge_depth;
        ++block.filaments;
        if (purge.filament_change_length > EPSILON) {
            // The leaving filament is rammed over the nozzle-change flow, again in whole lines.
            const float filament_area = float(M_PI) * purge.filament_diameter * purge.filament_diameter / 4.f;
            const float nc_length     = purge.filament_change_length * filament_area / line_area(ncpw);
            block.depth += std::ceil(nc_length / (width - ncpw - pw)) * nc_gap;
        }
    }
    float depth = pw; // plan_tower_new starts the first block one perimeter width in
    for (const auto &[category, block] : blocks)
        depth += block.filaments == purges.size() ? block.depth - block.min_purge : block.depth;
    return depth;
}

float WipeTower::rib_footprint_side(float width, float depth, float rib_width, float extra_rib_length, float max_height)
{
    if (width < EPSILON || depth < EPSILON)
        return 0.f;
    // Ribs run the diagonal; below the height-based minimum they are extended rather than the
    // body, then by the extra length, never ending up shorter than the diagonal.
    const float diagonal   = std::sqrt(width * width + depth * depth);
    float       rib_length = diagonal;
    if (depth + EPSILON < get_limit_depth_by_height(max_height))
        rib_length = std::max(rib_length, get_limit_depth_by_height(max_height) * float(std::sqrt(2.)));
    rib_length = std::max(diagonal, rib_length + extra_rib_length);
    // Half the extension at each end of the diagonal plus half the rib width, projected onto the axes.
    const float rib_w    = std::min(rib_width, std::min(width, depth) / 2.f);
    const float per_side = ((rib_length - diagonal) / 2.f + rib_w / 2.f) / float(std::sqrt(2.));
    return std::max(width, depth) + 2.f * per_side;
}

float WipeTower::estimate_rib_tower_bbox_side(const std::vector<PurgeEstimate> &purges, float width, float layer_height, float nozzle_diameter, float extra_spacing, float rib_width, float extra_rib_length, float max_height, float line_width)
{
    if (purges.empty() || width < EPSILON || layer_height < EPSILON || nozzle_diameter < EPSILON)
        return 0.f;
    const float pw     = line_width > 0.f ? line_width : nozzle_diameter * 1.25f; // Width_To_Nozzle_Ratio
    const float square = align_ceil(std::sqrt(estimate_tower_blocks_depth(purges, width, layer_height, nozzle_diameter, extra_spacing, line_width) * width), pw);
    const float depth  = estimate_tower_blocks_depth(purges, square, layer_height, nozzle_diameter, extra_spacing, line_width);
    return rib_footprint_side(square, depth, rib_width, extra_rib_length, max_height);
}

TriangleMesh WipeTower::its_make_rib_brim(const Polygon &brim, float layer_height)
{
    TriangleMesh res;
    if (brim.area() < scaled(EPSILON))
        return res;
    int offset = int(brim.size());
    res.its.vertices.reserve(brim.size() * 2);
    auto faces = Triangulation::triangulate(brim);
    res.its.indices.reserve(brim.size() * 2 + 2 * faces.size());
    for (auto &t : faces) res.its.indices.push_back({t[1], t[0], t[2]});
    for (auto &t : faces) res.its.indices.push_back({t[0] + offset, t[1] + offset, t[2] + offset});

    for (int i = 0; i < int(brim.size()); i++)
        res.its.vertices.push_back({unscaled<float>(brim[i][0]), unscaled<float>(brim[i][1]), 0});
    for (int i = 0; i < int(brim.size()); i++)
        res.its.vertices.push_back({unscaled<float>(brim[i][0]), unscaled<float>(brim[i][1]), layer_height});

    for (int i = 0; i < offset; i++) {
        int a = i;
        int b = (i + 1) % offset;
        int c = i + offset;
        int d = b + offset;
        res.its.indices.push_back({a, b, c});
        res.its.indices.push_back({d, c, b});
    }
    return res;
}

// Bambu Studio WipeTower.cpp:128 rounding_polygon(): replaces every corner sharper than angle_tol
// by a circular arc of arc_fit_size points, tangent to both sides `rounding` mm from the corner.
Polygon WipeTower::rounding_polygon(Polygon &polygon, double rounding /*= 2.*/, double angle_tol /* = 30. / 180. * PI*/)
{
    if (polygon.points.size() < 3) return polygon;
    Polygon res;
    res.points.reserve(polygon.points.size() * 2);
    int    mod           = polygon.points.size();
    double cos_angle_tol = std::abs(std::cos(angle_tol));

    for (int i = 0; i < int(polygon.points.size()); i++) {
        Vec2d  a      = unscaled(polygon.points[(i - 1 + mod) % mod]);
        Vec2d  b      = unscaled(polygon.points[i]);
        Vec2d  c      = unscaled(polygon.points[(i + 1) % mod]);
        double ab_len = (a - b).norm();
        double bc_len = (b - c).norm();
        Vec2d  ab     = (b - a) / ab_len;
        Vec2d  bc     = (c - b) / bc_len;
        assert(ab_len != 0);
        assert(bc_len != 0);
        float cosangle = ab.dot(bc);
        cosangle       = std::clamp(cosangle, -1.f, 1.f);
        bool  is_ccw   = cross2(ab, bc) > 0;
        if (std::abs(cosangle) < cos_angle_tol) {
            float real_rounding_dis = std::min({rounding, ab_len / 2.1, bc_len / 2.1}); // 2.1 to ensure the points do not coincide
            Vec2d left              = b - ab * real_rounding_dis;
            Vec2d right             = b + bc * real_rounding_dis;
            {
                float half_angle = std::acos(cosangle) / 2.f;
                Vec2d dir        = (right - left).normalized();
                dir              = Vec2d{-dir[1], dir[0]};
                dir              = is_ccw ? dir : -dir;
                double dis       = real_rounding_dis / sin(half_angle);

                Vec2d      center = b + dir * dis;
                double     radius = (left - center).norm();
                ArcSegment arc(scaled(center), scaled(radius), scaled(left), scaled(right), is_ccw ? ArcDirection::Arc_Dir_CCW : ArcDirection::Arc_Dir_CW);
                int        n = arc_fit_size;
                for (int j = 0; j < n; j++) {
                    float cur_angle = arc.polar_start_theta + (float) j / n * arc.angle_radians;
                    if (cur_angle > 2 * PI)
                        cur_angle -= 2 * PI;
                    else if (cur_angle < 0)
                        cur_angle += 2 * PI;
                    Point tmp = arc.center + Point{arc.radius * std::cos(cur_angle), arc.radius * std::sin(cur_angle)};
                    res.points.push_back(tmp);
                }
            }
            res.points.push_back(scaled(right));
        } else
            res.points.push_back(polygon.points[i]);
    }
    res.remove_duplicate_points();
    res.points.shrink_to_fit();
    return res;
}

// Bambu Studio WipeTower.cpp:5041 generate_rib_polygon(). The ribs run along the diagonals of the
// whole tower (m_wipe_tower_width x m_wipe_tower_depth), which sits m_y_shift below this layer's
// local origin, and are extended past the corners by a length that shrinks linearly from its
// first-layer value to nothing at the top of the tower.
Polygon WipeTower::generate_rib_polygon(const box_coordinates &wt_box) const
{
    auto    get_current_layer_rib_len = [](float cur_height, float max_height, float max_len) -> float { return std::abs(max_height - cur_height) / max_height * max_len; };
    coord_t diagonal_width            = scaled(m_rib_width) / 2;
    float   a = this->m_wipe_tower_width, b = this->m_wipe_tower_depth;
    Line    line_1(Point::new_scale(Vec2f{0, 0}), Point::new_scale(Vec2f{a, b}));
    Line    line_2(Point::new_scale(Vec2f{a, 0}), Point::new_scale(Vec2f{0, b}));
    float   diagonal_extra_length = std::max(0.f, m_rib_length - (float) unscaled(line_1.length())) / 2.f;
    diagonal_extra_length         = scaled(get_current_layer_rib_len(this->m_z_pos, this->m_wipe_tower_height, diagonal_extra_length));
    Point   y_shift{0, scaled(this->m_y_shift)};

    line_1.extend(double(diagonal_extra_length));
    line_2.extend(double(diagonal_extra_length));
    line_1.translate(-y_shift);
    line_2.translate(-y_shift);

    Polygon poly_1 = generate_rectange(line_1, diagonal_width);
    Polygon poly_2 = generate_rectange(line_2, diagonal_width);
    Polygon poly;
    poly.points.push_back(Point::new_scale(wt_box.ld));
    poly.points.push_back(Point::new_scale(wt_box.rd));
    poly.points.push_back(Point::new_scale(wt_box.ru));
    poly.points.push_back(Point::new_scale(wt_box.lu));

    Polygons p_1_2 = union_({poly_1, poly_2, poly});
    return p_1_2.front();
}

// The wall Bambu Studio's generate_support_wall_new() (WipeTower.cpp:5073) extrudes for a rib
// wall: the rib polygon, filleted and unioned with the box again when fillet_wall is on.
Polygon WipeTower::rib_wall_polygon(const box_coordinates &wt_box) const
{
    Polygon wall_polygon = generate_rib_polygon(wt_box);
    if (m_used_fillet) {
        wall_polygon           = rounding_polygon(wall_polygon);
        Polygon wt_box_polygon = generate_rectange_polygon(wt_box.ld, wt_box.ru);
        wall_polygon           = union_({wall_polygon, wt_box_polygon}).front();
    }
    return wall_polygon;
}

void WipeTower::record_outer_wall(const Polygon &wall)
{
    // Edge (OD16 review): a layer folded into a later one prints nothing, so the conflict checker and
    // the object brim must not see a wall at its z.
    if (m_layer_info != m_plan.end() && m_layer_info->combined_away)
        return;
    // To the frame the writer emits in: the layer's y shift, then the rib offset.
    Polyline pl = to_polyline(wall);
    pl.translate(scaled(m_rib_offset.x()), scaled(m_rib_offset.y() + m_y_shift));
    m_outer_wall[m_z_pos].push_back(std::move(pl));
}

float WipeTower::wall_feedrate(size_t tool) const
{
    // Bambu Studio WipeTower.cpp:3591 (finish_layer_new): the wall speed, capped by prime_tower_max_speed.
    return is_first_layer() ? std::min(m_first_layer_speed * 60.f, m_max_speed) :
                              std::min(60.0f * m_filpar[tool].max_e_speed / m_extrusion_flow, m_max_speed);
}

WipeTower::WipeTower(const PrintConfig& config, int plate_idx, Vec3d plate_origin, const float prime_volume, size_t initial_tool, const float wipe_tower_height) :
    m_semm(config.single_extruder_multi_material.value),
    m_wipe_tower_pos(config.wipe_tower_x.get_at(plate_idx), config.wipe_tower_y.get_at(plate_idx)),
    m_wipe_tower_width(float(config.prime_tower_width)),
    // BBS
    m_wipe_tower_height(wipe_tower_height),
    m_wipe_tower_rotation_angle(float(config.wipe_tower_rotation_angle)),
    m_wipe_tower_brim_width(float(config.prime_tower_brim_width)),
    m_y_shift(0.f),
    m_z_pos(0.f),
    //m_bridging(float(config.wipe_tower_bridging)),
    m_bridging(10.f),
    m_sparse_layers_skipped(wipe_tower_sparse_layers_skipped(config)),
    m_sparse_layers_combined(wipe_tower_sparse_layers_combined(config)),
    m_gcode_flavor(config.gcode_flavor),
    m_has_nozzle_rack(Slic3r::has_nozzle_rack(config)),
    m_travel_speed(float(get_value_at(config, config.travel_speed, ConfigFlowDomain::Process, initial_tool))),
    m_current_tool(initial_tool),
    //wipe_volumes(flush_matrix)
    m_wipe_volume(prime_volume),
    m_enable_timelapse_print(config.timelapse_type.value == TimelapseType::tlSmooth)
{
    // Read absolute value of first layer speed, if given as percentage,
    // it is taken over following default. Speeds from config are not
    // easily accessible here.
    const float default_speed = 60.f;
    m_first_layer_speed = float(get_value_at(config, config.initial_layer_speed, ConfigFlowDomain::Process, initial_tool));
    if (m_first_layer_speed == 0.f) // just to make sure autospeed doesn't break it.
        m_first_layer_speed = default_speed / 2.f;

    // prime_tower_brim_width -1 is Bambu Studio's "auto" brim, which the H2D/H2C/X2D/H2S/P2S/A2L
    // process presets ship; plan_tower_new() resolves it from the tower's real height, as Bambu
    // Studio and Orca do (estimate_wipe_tower_footprint() estimates it from the object height).

    // Orca #10780 / #15035 (Bambu Studio): the block planner's purge line gap, internal ribs and flat
    // ironing (the latter only behind a wall gap).
    m_extra_spacing   = float(config.prime_tower_infill_gap.value) / 100.f;
    m_tower_framework = config.prime_tower_enable_framework.value;
    m_flat_ironing    = config.prime_tower_flat_ironing.value && config.wipe_tower_wall_gap.value;
    // is_valid_last_layer(): Bambu Studio leaves extruder_printable_height empty on printers without a limit;
    // this tree's default is 0, which is read as no limit as well.
    m_printable_height = config.extruder_printable_height.values;
    m_last_layer_id.assign(std::max<size_t>(1, config.nozzle_diameter.values.size()), -1);

    // Bambu Studio's prime_tower_rib_wall / _rib_width / _extra_rib_length / _fillet_wall /
    // _max_speed (WipeTower.cpp:1773-1779), under the names this fork shares with Orca.
    m_use_rib_wall       = config.wipe_tower_wall_type.value == WipeTowerWallType::wtwRib;
    m_rib_width          = float(config.wipe_tower_rib_width.value);
    m_extra_rib_length   = float(config.wipe_tower_extra_rib_length.value);
    m_used_fillet        = config.wipe_tower_fillet_wall.value;
    m_max_speed          = float(config.wipe_tower_max_purge_speed.value) * 60.f;
    m_enable_arc_fitting = config.enable_arc_fitting.value && std::abs(m_wipe_tower_rotation_angle) < EPSILON;
    m_tower_line_width = float(std::max(0., config.prime_tower_line_width.value));
    // Bambu Studio 2.9.1 (WipeTower.cpp:1942-1960): prime tower extrusions at prime_tower_acceleration,
    // travels inside the tower at the travel acceleration.
    // The travel acceleration is an object setting here: Print hands it over (set_travel_acceleration).
    if (config.prime_tower_acceleration.value > 0.)
        m_tower_acceleration = (unsigned int) std::floor(config.prime_tower_acceleration.value + 0.5);

    // If this is a single extruder MM printer, we will use all the SE-specific config values.
    // Otherwise, the defaults will be used to turn off the SE stuff.
    // BBS: remove useless config
#if 0
    if (m_semm) {
        m_cooling_tube_retraction = float(config.cooling_tube_retraction);
        m_cooling_tube_length     = float(config.cooling_tube_length);
        m_parking_pos_retraction  = float(config.parking_pos_retraction);
        m_extra_loading_move      = float(config.extra_loading_move);
        m_set_extruder_trimpot    = config.high_current_on_filament_swap;
    }
#endif
    // Calculate where the priming lines should be - very naive test not detecting parallelograms etc.
    const std::vector<Vec2d>& bed_points = config.printable_area.values;
    BoundingBoxf bb(bed_points);
    m_bed_width = float(bb.size().x());
    m_bed_shape = (bed_points.size() == 4 ? RectangularBed : CircularBed);

    if (m_bed_shape == CircularBed) {
        // this may still be a custom bed, check that the points are roughly on a circle
        double r2 = std::pow(m_bed_width/2., 2.);
        double lim2 = std::pow(m_bed_width/10., 2.);
        Vec2d center = bb.center();
        for (const Vec2d& pt : bed_points)
            if (std::abs(std::pow(pt.x()-center.x(), 2.) + std::pow(pt.y()-center.y(), 2.) - r2) > lim2) {
                m_bed_shape = CustomBed;
                break;
            }
    }

    m_bed_bottom_left = m_bed_shape == RectangularBed
                  ? Vec2f(bed_points.front().x(), bed_points.front().y())
                  : Vec2f::Zero();

    m_interface    = TowerInterface::Settings::from_config(config);
    m_use_gap_wall = config.wipe_tower_wall_gap.value;
    m_shared_bed   = TowerInterface::shared_printable_box(config);

    // Bambu Studio 2.9.1 (WipeTower.cpp:1929-1935): the nozzle-change ramming mode and its cooling timing.
    m_handle_hotend_as_extruder = Slic3r::hotend_as_extruder(config);
    for (double rate : config.hotend_cooling_rate.values)
        m_hotend_cooling_rate.push_back(std::isfinite(rate) ? float(rate) : 0.f);
}



void WipeTower::set_extruder(size_t idx, const PrintConfig& config)
{
    //while (m_filpar.size() < idx+1)   // makes sure the required element is in the vector
    m_filpar.push_back(FilamentParameters());

    m_filpar[idx].material = config.filament_type.get_at(idx);
    // m_filpar[idx].is_soluble = config.filament_soluble.get_at(idx);
    m_filpar[idx].is_soluble = config.wipe_tower_filament == 0 ? config.filament_soluble.get_at(idx) : (idx != size_t(config.wipe_tower_filament - 1));
    // BBS
    m_filpar[idx].is_support = config.filament_is_support.get_at(idx);
    m_filpar[idx].nozzle_temperature = get_value_at(config, config.nozzle_temperature, ConfigFlowDomain::Filament, idx);
    m_filpar[idx].nozzle_temperature_initial_layer = get_value_at(config, config.nozzle_temperature_initial_layer, ConfigFlowDomain::Filament, idx);

    // If this is a single extruder MM printer, we will use all the SE-specific config values.
    // Otherwise, the defaults will be used to turn off the SE stuff.
    // BBS: remove useless config
#if 0
    if (m_semm) {
        m_filpar[idx].loading_speed           = float(config.filament_loading_speed.get_at(idx));
        m_filpar[idx].loading_speed_start     = float(config.filament_loading_speed_start.get_at(idx));
        m_filpar[idx].unloading_speed         = float(config.filament_unloading_speed.get_at(idx));
        m_filpar[idx].unloading_speed_start   = float(config.filament_unloading_speed_start.get_at(idx));
        m_filpar[idx].delay                   = float(config.filament_toolchange_delay.get_at(idx));
        m_filpar[idx].cooling_moves           = config.filament_cooling_moves.get_at(idx);
        m_filpar[idx].cooling_initial_speed   = float(config.filament_cooling_initial_speed.get_at(idx));
        m_filpar[idx].cooling_final_speed     = float(config.filament_cooling_final_speed.get_at(idx));
    }
#endif

    m_filpar[idx].filament_area = float((M_PI/4.f) * pow(config.filament_diameter.get_at(idx), 2)); // all extruders are assumed to have the same filament diameter at this point
    // Mixed nozzle diameters (per-nozzle P2): the nozzle this filament prints through (filament_map on an
    // H2D / H2C), not the extruder with the filament's index.
    float nozzle_diameter = float(nozzle_diameter_for_filament(config, unsigned(idx) + 1));
    m_filpar[idx].nozzle_diameter = nozzle_diameter;
    m_filpar[idx].max_layer_height = filament_max_layer_height(config, idx);

    float max_vol_speed = float(get_value_at(config, config.filament_max_volumetric_speed, ConfigFlowDomain::Filament, idx));
    if (max_vol_speed!= 0.f)
        m_filpar[idx].max_e_speed = (max_vol_speed / filament_area());
    m_filpar[idx].wipe_dist = float(config.wipe_distance.get_at(idx));
    // Orca #10780 / #15035: the filament's block, and the retraction and spiral of the wall gap ironing.
    m_filpar[idx].category       = config.filament_adhesiveness_category.values.empty() ? 0 : config.filament_adhesiveness_category.get_at(idx);
    m_filpar[idx].retract_length = config.retraction_length.values.empty() ? 0.f : float(config.retraction_length.get_at(idx));
    m_filpar[idx].retract_speed  = config.retraction_speed.values.empty() ? 0.f : float(config.retraction_speed.get_at(idx));
    m_filpar[idx].flat_iron_area = config.filament_tower_ironing_area.values.empty() ? 0.f : float(config.filament_tower_ironing_area.get_at(idx));

    m_filpar[idx].kind                  = TowerInterface::filament_kind(config, (unsigned int) idx);
    m_filpar[idx].interface_temperature = TowerInterface::interface_temperature(config.filament_tower_interface_print_temp.get_at(idx),
                                                                                config.nozzle_temperature_range_high.get_at(idx),
                                                                                m_filpar[idx].nozzle_temperature);
    m_filpar[idx].run_in_distance       = std::max(0.f, float(config.filament_tower_interface_pre_extrusion_dist.get_at(idx)));
    m_filpar[idx].extra_prime_length    = std::max(0.f, float(config.filament_tower_interface_pre_extrusion_length.get_at(idx)));
    // Bambu Studio names the nozzle of an interface M109 / M104 on multi-nozzle machines
    // (format_line_M109: T<physical extruder> ... N0).
    if (config.nozzle_diameter.values.size() > 1 && idx < config.filament_map.values.size()) {
        const int logical = config.filament_map.values[idx] - 1;
        if (logical >= 0)
            m_filpar[idx].physical_extruder = logical < int(config.physical_extruder_map.values.size()) ? config.physical_extruder_map.values[logical] : logical;
    }
    // BBS: Bambu Studio's tower reheats the nozzle before the wipe of a toolchange whose change_filament_gcode
    // cooled it (WipeTower.cpp toolchange_wipe_new: "Wipe tower reheat before wipe"). Its M104 always names the
    // physical extruder: the filament's on a multi-nozzle machine, extruder 0's otherwise.
    m_filpar[idx].cooling_before_tower = float(filament_cooling_before_tower_at(config, (unsigned int) idx, true));
    m_filpar[idx].reheat_extruder      = m_filpar[idx].physical_extruder >= 0 ? m_filpar[idx].physical_extruder :
                                         config.physical_extruder_map.values.empty() ? 0 : std::max(0, config.physical_extruder_map.values.front());

    // Bambu Studio 2.9.1 (WipeTower.cpp:2078-2135): the nozzle-change ramming of this filament when it leaves its
    // nozzle. .first is an extruder change, .second a hotend change on one extruder.
    {
        FilamentParameters &fp = m_filpar[idx];
        auto at = [idx](const ConfigOptionFloats &opt) { return opt.values.empty() ? 0.f : float(opt.get_at(idx)); };
        fp.change_length = { std::max(0.f, at(config.filament_change_length)), std::max(0.f, at(config.filament_change_length_nc)) };
        // Nullable, per flow variant; nil or -1 means the filament's maximum volumetric speed.
        auto variant = [&config, idx](const ConfigOptionFloatsNullable &opt, float nil_value) {
            if (opt.values.empty())
                return nil_value;
            size_t i = get_config_idx(config, ConfigFlowDomain::Filament, (unsigned int) idx);
            if (i >= opt.values.size())
                i = 0;
            return opt.is_nil(i) || std::isnan(opt.values[i]) ? nil_value : float(opt.values[i]);
        };
        const float area      = fp.filament_area;
        auto        ram_speed = [&](const ConfigOptionFloatsNullable &opt) {
            float vol = variant(opt, -1.f);
            if (is_approx(vol, -1.f) || vol <= 0.f)
                vol = max_vol_speed;
            return vol > 0.f && area > 0.f ? vol / area : fp.max_e_speed;
        };
        fp.max_e_ramming_speed = { ram_speed(config.filament_ramming_volumetric_speed), ram_speed(config.filament_ramming_volumetric_speed_nc) };
        fp.ramming_travel_time = { std::max(0.f, variant(config.filament_ramming_travel_time, 0.f)),
                                   std::max(0.f, variant(config.filament_ramming_travel_time_nc, 0.f)) };
        fp.idle_travel_cooling_enabled = config.filament_idle_travel_cooling_enabled.values.empty() ? true :
                                                                                                     config.filament_idle_travel_cooling_enabled.get_at(idx);

        // Pre-cooling during the ramming: only with enable_pre_heating and a target (0 = off).
        const size_t extruders = std::max<size_t>(1, config.nozzle_diameter.values.size());
        fp.precool_t             = { std::vector<float>(extruders, 0.f), std::vector<float>(extruders, 0.f) };
        fp.precool_t_first_layer = fp.precool_t;
        fp.precool_target_temp   = { 0, 0 };
        auto int_at = [idx](const ConfigOptionInts &opt) { return opt.values.empty() ? 0 : opt.get_at(idx); };
        const int targets[2] = { int_at(config.filament_pre_cooling_temperature), int_at(config.filament_pre_cooling_temperature_nc) };
        for (int nc = 0; nc < 2; ++nc) {
            if (!config.enable_pre_heating.value || targets[nc] == 0)
                continue;
            std::vector<float> &t       = nc == 0 ? fp.precool_t.first : fp.precool_t.second;
            std::vector<float> &t_first = nc == 0 ? fp.precool_t_first_layer.first : fp.precool_t_first_layer.second;
            for (size_t e = 0; e < extruders; ++e) {
                const float rate = e < m_hotend_cooling_rate.size() ? m_hotend_cooling_rate[e] : 0.f;
                if (rate <= 0.f)
                    continue;
                t[e]       = std::max(0.f, float(fp.nozzle_temperature - targets[nc])) / rate;
                t_first[e] = std::max(0.f, float(fp.nozzle_temperature_initial_layer - targets[nc])) / rate;
            }
            (nc == 0 ? fp.precool_target_temp.first : fp.precool_target_temp.second) = targets[nc];
        }
    }

    // Structure width (Edge, per-nozzle P2): one width for the footprint, the wall, the brim and the sparse
    // fill, so the tower stays one rectangle whatever tool prints it. The forced wipe tower filament's
    // nozzle when there is one, else the smallest nozzle registered (the wall is printed by whichever tool
    // changes in, and the smallest width is safe for every tool). Each tool's purge uses
    // tool_line_width(). With uniform nozzles this is the value every set_extruder() call wrote before.
    {
        const float tool_pw = m_tower_line_width > 0.f ? m_tower_line_width : nozzle_diameter * Width_To_Nozzle_Ratio;
        const bool  forced  = config.wipe_tower_filament > 0;
        const bool  is_wall = forced && idx == size_t(config.wipe_tower_filament - 1);
        if (idx == 0 || is_wall || (!forced && tool_pw < m_perimeter_width))
            m_perimeter_width = tool_pw;
    }
    // Orca's tower-wide ramming line width (one width with uniform nozzles): the widest, for the block gaps.
    m_nozzle_change_perimeter_width = std::max(idx == 0 ? 0.f : m_nozzle_change_perimeter_width, nozzle_change_width(idx));
    // BBS: remove useless config
#if 0
    if (m_semm) {
        std::istringstream stream{config.filament_ramming_parameters.get_at(idx)};
        float speed = 0.f;
        stream >> m_filpar[idx].ramming_line_width_multiplicator >> m_filpar[idx].ramming_step_multiplicator;
        m_filpar[idx].ramming_line_width_multiplicator /= 100;
        m_filpar[idx].ramming_step_multiplicator /= 100;
        while (stream >> speed)
            m_filpar[idx].ramming_speed.push_back(speed);
    }
#endif

    m_used_filament_length.resize(std::max(m_used_filament_length.size(), idx + 1)); // makes sure that the vector is big enough so we don't have to check later
}



// Returns gcode to prime the nozzles at the front edge of the print bed.
std::vector<WipeTower::ToolChangeResult> WipeTower::prime(
	// print_z of the first layer.
	float 						initial_layer_print_height,
	// Extruder indices, in the order to be primed. The last extruder will later print the wipe tower brim, print brim and the object.
	const std::vector<unsigned int> &tools,
	// If true, the last priming are will be the same as the other priming areas, and the rest of the wipe will be performed inside the wipe tower.
	// If false, the last priming are will be large enough to wipe the last extruder sufficiently.
    bool 						/*last_wipe_inside_wipe_tower*/)
{
    return std::vector<ToolChangeResult>();
}

// The final unload (Print::_make_wipe_tower's final purge) or a planned tool change (tool_change_new()).
WipeTower::ToolChangeResult WipeTower::tool_change(size_t tool, bool /*extrude_perimeter*/, bool /*first_toolchange_to_nonsoluble*/)
{
    if (tool != (unsigned int) (-1))
        return tool_change_new(tool);

    // Unload only: change_filament_gcode unloads on the printers this tower serves, so this is an empty change.
    const size_t    old_tool = m_current_tool;
    box_coordinates cleaning_box(Vec2f(m_perimeter_width, m_perimeter_width), m_wipe_tower_width - 2 * m_perimeter_width,
                                 m_wipe_tower_depth - m_perimeter_width);
    WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_has_nozzle_rack, m_bbl_gcode_tags);
    writer.set_extrusion_flow(m_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(m_current_tool)
        .set_origin_offset(m_rib_offset)
        .set_arc_fitting(m_enable_arc_fitting)
        .set_accelerations(writer_tower_acceleration(), m_tower_travel_acceleration)
        .set_y_shift(m_y_shift)
        .append(";--------------------\n"
                "; CP TOOLCHANGE START\n")
        .comment_with_value(" toolchange #", m_num_tool_changes + 1); // the number is zero-based
    writer.speed_override_backup();
    writer.speed_override(100);
    writer.set_initial_position(cleaning_box.ld, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);
    toolchange_Unload(writer, cleaning_box, m_filpar[m_current_tool].material, m_filpar[m_current_tool].nozzle_temperature);
    writer.speed_override_restore();
    writer.feedrate(m_travel_speed * 60.f)
          .flush_planner_queue()
          .reset_extruder()
          .append("; CP TOOLCHANGE END\n"
                  ";------------------\n"
                  "\n\n");
    if (m_current_tool < m_used_filament_length.size())
        m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();
    return construct_tcr(writer, false, old_tool, false, false, 0.f, false);
}

// Orca #10780 / #15035 (Bambu Studio tool_change_new()): the outgoing filament's nozzle change (ramming in its own
// block), then the new filament's purge in its own category's block, starting on the corner the layer number
// picks (get_next_pos()).
WipeTower::ToolChangeResult WipeTower::tool_change_new(size_t new_tool, bool solid_toolchange, bool solid_nozzlechange)
{
    const size_t                     old_tool = m_current_tool;
    const WipeTowerInfo::ToolChange *planned  = nullptr;
    for (const auto &b : m_layer_info->tool_changes)
        if (b.new_tool == new_tool) {
            planned = &b;
            break;
        }
    const float wipe_length  = planned != nullptr ? planned->wipe_length : 0.f;
    const float wipe_depth   = planned != nullptr ? planned->required_depth - planned->nozzle_change_depth : 0.f;
    const float purge_volume = planned != nullptr ? planned->purge_volume : 0.f;

    // Bambu Studio 2.9.1 tool_change_new(): a change onto another nozzle starts with the outgoing filament's nozzle
    // change, unless that filament's extruder is past its printable height on its last layer. On a
    // handle_hotend_as_extruder printer (H3C) a filament with no ramming length turns its heater off instead, or
    // cools on an idle travel over the purge area, except on the first layer, which rams (zero lines when the
    // length is zero).
    NozzleChangeResult nozzle_change_result;
    if (planned != nullptr && planned->nozzle_change.need_ramming && old_tool < m_filpar.size() &&
        is_valid_last_layer(old_tool, int(m_cur_layer_id), m_z_pos)) {
        const FilamentParameters &outgoing  = m_filpar[old_tool];
        const float               fc_length = planned->nozzle_change.extruder_change ? outgoing.change_length.first : outgoing.change_length.second;
        if (m_handle_hotend_as_extruder && fc_length <= EPSILON && !is_first_layer())
            nozzle_change_result = outgoing.idle_travel_cooling_enabled ? idle_travel_cooling(old_tool, new_tool, *planned) :
                                                                         heater_off_nozzle_change(old_tool, new_tool, *planned);
        else
            nozzle_change_result = ramming(old_tool, new_tool, *planned, solid_nozzlechange);
    }

    WipeTowerBlock *block = get_block_by_category(get_filament_category(new_tool), false);
    if (block == nullptr) {
        assert(false);
        return ToolChangeResult();
    }
    m_cur_block = block;
    box_coordinates cleaning_box(Vec2f(m_perimeter_width, block->cur_depth), m_wipe_tower_width - 2 * m_perimeter_width, wipe_depth);

    WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_has_nozzle_rack, m_bbl_gcode_tags);
    writer.set_extrusion_flow(m_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(m_current_tool)
        .set_origin_offset(m_rib_offset)
        .set_arc_fitting(m_enable_arc_fitting)
        .set_accelerations(writer_tower_acceleration(), m_tower_travel_acceleration)
        .set_y_shift(m_y_shift)
        .append(";--------------------\n"
                "; CP TOOLCHANGE START\n")
        .comment_with_value(" toolchange #", m_num_tool_changes + 1); // the number is zero-based
    writer.append(std::string("; material : " + (m_current_tool < m_filpar.size() ? m_filpar[m_current_tool].material : "(NONE)") + " -> " +
                              m_filpar[new_tool].material + "\n").c_str())
          .append(";--------------------\n");

    writer.speed_override_backup();
    writer.speed_override(100);

    const Vec2f initial_position = get_next_pos(cleaning_box, wipe_length, new_tool);
    writer.set_initial_position(initial_position, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");
    toolchange_Unload(writer, cleaning_box, m_filpar[m_current_tool].material,
                      is_first_layer() ? m_filpar[new_tool].nozzle_temperature_initial_layer : m_filpar[new_tool].nozzle_temperature);
    toolchange_Change(writer, new_tool, m_filpar[new_tool].material); // Change the tool, set a speed override for soluble and flex materials.
    toolchange_Load(writer, cleaning_box);

    // BBS: Bambu Studio's "Wipe tower reheat before wipe" (toolchange_wipe_new(), add_M104_by_requirement):
    // change_filament_gcode left the nozzle filament_cooling_before_tower below the print temperature (M620.15 C);
    // the purge heats back up. Not on the tower's first layer, an interface or a contact (solid) change: GCode
    // publishes no cooling for those (tcr.reheats_after_cooling).
    const bool at_interface          = planned != nullptr && planned->is_interface;
    const bool reheats_after_cooling = !is_first_layer() && !at_interface && !solid_toolchange;
    if (at_interface)
        interface_before_wipe(writer, *planned);
    toolchange_wipe_new(writer, cleaning_box, wipe_length, solid_toolchange, reheats_after_cooling,
                        !nozzle_change_result.gcode.empty());
    if (at_interface)
        interface_after_wipe(writer, *planned);
    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");
    ++m_num_tool_changes;

    block->cur_depth += wipe_depth;
    block->last_filament_change_id = int(new_tool);

    writer.speed_override_restore();
    writer.feedrate(m_travel_speed * 60.f)
          .flush_planner_queue()
          .reset_extruder()
          .append("; CP TOOLCHANGE END\n"
                  ";------------------\n"
                  "\n\n");

    // Ask our writer about how much material was consumed:
    if (m_current_tool < m_used_filament_length.size())
        m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    ToolChangeResult result      = construct_tcr(writer, false, old_tool, false, true, purge_volume, solid_toolchange);
    result.reheats_after_cooling = reheats_after_cooling;
    result.nozzle_change_result  = std::move(nozzle_change_result);
    return result;
}

// Orca #15035 (Bambu Studio get_next_pos()): the purge starts on a corner that turns with the layer number, at the
// bottom of its box on even layers and at the top on odd ones, so the start blobs do not stack.
Vec2f WipeTower::get_next_pos(const box_coordinates &cleaning_box, float wipe_length, size_t tool) const
{
    const float xl         = cleaning_box.ld.x();
    const float xr         = cleaning_box.rd.x();
    const int   line_count = xr - xl > EPSILON ? int(wipe_length / (xr - xl)) : 0;
    const float dy         = m_layer_info->extra_spacing * get_block_gap_width(tool, false);
    const float y_offset   = float(line_count) * dy;
    switch (m_cur_layer_id % 4) {
    case 0: return cleaning_box.ld;
    case 1: return cleaning_box.rd + Vec2f(0.f, y_offset);
    case 2: return cleaning_box.rd;
    default: return cleaning_box.ld + Vec2f(0.f, y_offset);
    }
}

std::string WipeTower::nozzle_change_marker(bool start, size_t old_tool, size_t new_tool, const NozzleChangePlan &plan) const
{
    // The same line GCode::format_nozzle_change_block() and Bambu Studio write; the pre-cooling injector and the
    // G-code processor read it.
    char buf[128];
    snprintf(buf, sizeof(buf), "; NOZZLE_CHANGE_%s OF%d NF%d ON%d NN%d\n", start ? "START" : "END", int(old_tool), int(new_tool),
             plan.old_nozzle, plan.new_nozzle);
    return buf;
}

std::string WipeTower::format_line_M104(int target_temp, size_t tool, const NozzleChangePlan &plan, bool is_heating, const std::string &comment) const
{
    // Bambu Studio 2.9.1 WipeTowerWriter::format_line_M104 (WipeTower.cpp:1459): cooling waits for the moves
    // (G1 is non-blocking); N0 marks a slicer line, C / W cooling or heating. The nozzle is the physical
    // extruder, or on hotend_as_extruder printers the filament (I). No physical extruder: no line at all.
    std::string out;
    if (!m_handle_hotend_as_extruder && plan.old_physical_extruder < 0)
        return out;
    if (!is_heating)
        out += "M400\n";
    out += "M104";
    out += m_handle_hotend_as_extruder ? " I" + std::to_string(tool) : " T" + std::to_string(plan.old_physical_extruder);
    out += " S" + std::to_string(target_temp) + " N0";
    out += is_heating ? " W" : " C";
    if (!comment.empty())
        out += " ;" + comment;
    out += '\n';
    return out;
}

// Bambu Studio 2.9.1 WipeTower::ramming() (WipeTower.cpp:3727-3920) in the block planner (Orca #10780): the
// outgoing filament rams its filament_change_length(_nc) on the tower before the swap, in its own category's
// block, ahead of whatever purges there next. A contact (solid) block layer rams solid and slower.
WipeTower::NozzleChangeResult WipeTower::ramming(size_t old_tool, size_t new_tool, const WipeTowerInfo::ToolChange &planned, bool solid_nozzlechange)
{
    const NozzleChangePlan   &plan            = planned.nozzle_change;
    const bool                extruder_change = plan.extruder_change;
    const FilamentParameters &fp              = m_filpar[old_tool];

    WipeTowerBlock *block = get_block_by_category(get_filament_category(old_tool), false);
    if (block == nullptr) {
        assert(false);
        return NozzleChangeResult();
    }
    m_cur_block = block;

    // The ramming lines follow the outgoing filament's nozzle.
    const float nc_width       = nozzle_change_width(old_tool);
    const float old_tool_width = tool_line_width(old_tool);
    const float x_offset       = old_tool_width + (nc_width - old_tool_width) / 2.f;
    const float box_width      = m_wipe_tower_width - 2.f * x_offset;
    int         line_count     = box_width > EPSILON ? int(std::ceil(planned.nozzle_change_length / box_width)) : 0;
    const float nc_depth       = planned.nozzle_change_depth;

    auto flow_for = [this, nc_width](float height) { return height * (nc_width - height * (1.f - float(M_PI) / 4.f)) / filament_area(); };
    const float nz_extrusion_flow = flow_for(m_layer_height);

    WipeTowerWriter writer(m_layer_height, nc_width, m_gcode_flavor, m_filpar, m_has_nozzle_rack, m_bbl_gcode_tags);
    writer.set_extrusion_flow(nz_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(old_tool)
        .set_origin_offset(m_rib_offset)
        .set_accelerations(writer_tower_acceleration(), m_tower_travel_acceleration)
        .set_y_shift(m_y_shift);

    const bool first_layer = is_first_layer();
    float      dy          = first_layer ? nc_width : m_layer_info->extra_spacing * get_block_gap_width(old_tool, true);
    // The block's next free row; rows start one perimeter width in, like the purge rows.
    box_coordinates cleaning_box(Vec2f(x_offset, block->cur_depth + (nc_width - old_tool_width) / 2.f), box_width, nc_depth);
    const Vec2f     initial_position = cleaning_box.ld;
    writer.set_initial_position(initial_position, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    // Nozzle change preamble: tell the firmware (a hotend change on one extruder) and pre-cool.
    writer.append(nozzle_change_marker(true, old_tool, new_tool, plan));
    const std::string m632 = "M632 S" + std::to_string(new_tool) +
                             (plan.dynamic_nozzle_map && plan.new_nozzle >= 0 ? " H" + std::to_string(plan.new_nozzle) : std::string()) + " M N\n";
    if (!extruder_change) {
        writer.append(m632);
        if (fp.precool_target_temp.second != 0) {
            const std::string m104 = format_line_M104(fp.precool_target_temp.second, old_tool, plan, false, "Wipe tower nozzle change pre cooling");
            if (!m104.empty())
                writer.append(m104).append("M106 S255\n");
        }
        writer.append("M633\n");
    }

    NozzleChangeResult result;
    if (line_count > 0) {
        const float max_e_ramming_speed = extruder_change ? fp.max_e_ramming_speed.first : fp.max_e_ramming_speed.second;
        float       nozzle_change_speed = 60.0f * max_e_ramming_speed / nz_extrusion_flow;
        // Orca #10780: a contact layer rams at no more than 40 mm/s.
        if (solid_nozzlechange)
            nozzle_change_speed = std::min(40.f * 60.f, nozzle_change_speed);
        float bridge_speed = std::min(60.0f * max_e_ramming_speed / flow_for(0.2f), nozzle_change_speed);

        const float xl = cleaning_box.ld.x();
        const float xr = cleaning_box.rd.x();
        // Orca #10780: a contact layer rams solid lines over the whole box.
        if (solid_nozzlechange) {
            dy         = nc_width;
            line_count = int(std::floor(EPSILON + (cleaning_box.ru.y() - cleaning_box.rd.y() + (nc_width - old_tool_width) / 2.f) / nc_width));
        }
        m_left_to_right       = true;
        bool  need_change_flow = false;
        // An extruder change stretches the ramming over the time the nozzle needs to reach its pre-cool target.
        const float ramming_length  = line_count * (xr - xl);
        const int   extruder        = plan.old_extruder;
        auto        seconds         = [extruder](const std::vector<float> &t) { return extruder >= 0 && extruder < int(t.size()) ? t[extruder] : 0.f; };
        const float precool_t       = extruder_change ? seconds(fp.precool_t.first) : seconds(fp.precool_t.second);
        const float precool_t_first = extruder_change ? seconds(fp.precool_t_first_layer.first) : seconds(fp.precool_t_first_layer.second);
        float       per_cooling_max_speed = nozzle_change_speed;
        // PVA on a hotend_as_extruder printer keeps its volumetric ramming speed (Bambu Studio: the long cut
        // retraction must not wait for the temperature).
        const bool command_only_active = m_handle_hotend_as_extruder && fp.material == "PVA";
        if (extruder_change && !command_only_active) {
            if (first_layer && precool_t_first > EPSILON)
                per_cooling_max_speed = ramming_length / precool_t_first * 60.f;
            else if (precool_t > EPSILON)
                per_cooling_max_speed = ramming_length / precool_t * 60.f;
        }
        nozzle_change_speed = std::min(nozzle_change_speed, per_cooling_max_speed);
        // Edge (review S2): on the first layer the ramming rows go straight onto the plate; keep them at the
        // first-layer speed (GCode::append_tcr sets the first-layer acceleration around them).
        if (first_layer)
            nozzle_change_speed = std::min(nozzle_change_speed, m_first_layer_speed * 60.f);
        bridge_speed        = std::min(bridge_speed, per_cooling_max_speed);
        bridge_speed        = std::min(bridge_speed, nozzle_change_speed);

        writer.set_e_speed_limit(max_e_ramming_speed);
        for (int i = 0; true; ++i) {
            if (need_thick_bridge_flow(writer.y())) {
                writer.set_extrusion_flow(flow_for(0.2f));
                writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + analyzer_tag_value(0.2) + "\n");
                need_change_flow = true;
            }
            if (m_left_to_right)
                writer.extrude(xr + wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), need_change_flow ? bridge_speed : nozzle_change_speed);
            else
                writer.extrude(xl - wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), need_change_flow ? bridge_speed : nozzle_change_speed);
            if (i == line_count - 1)
                break;
            if ((writer.y() + dy - cleaning_box.ru.y() + (nc_width + old_tool_width) / 2.f) > float(EPSILON))
                break;
            if (need_change_flow) {
                writer.set_extrusion_flow(nz_extrusion_flow);
                writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + analyzer_tag_value(m_layer_height) + "\n");
                need_change_flow = false;
            }
            writer.extrude(writer.x(), writer.y() + dy, nozzle_change_speed);
            m_left_to_right = !m_left_to_right;
        }
        if (need_change_flow)
            writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + analyzer_tag_value(m_layer_height) + "\n");
        writer.set_extrusion_flow(nz_extrusion_flow);
        writer.set_e_speed_limit(0.f);
        block->cur_depth += nc_depth;
        block->last_nozzle_change_id = int(old_tool);

        // Post-ramming: re-arm the hotend change for the travel phase.
        if (!extruder_change)
            writer.append(m632);

        const float travel_time = extruder_change ? fp.ramming_travel_time.first : fp.ramming_travel_time.second;
        const float fc_length   = extruder_change ? fp.change_length.first : fp.change_length.second;
        if (travel_time > EPSILON && fc_length > EPSILON) {
            // Reverse travel over the ramming lines for ramming_travel_time seconds (anti-ooze).
            bool  left_to_right           = !m_left_to_right;
            int   tpu_line_count          = line_count;
            nozzle_change_speed *= 2;
            float need_reverse_travel_dis = travel_time * nozzle_change_speed / 60.f;
            float real_travel_dis         = tpu_line_count * (xr - xl - 2 * old_tool_width);
            if (real_travel_dis < need_reverse_travel_dis)
                nozzle_change_speed *= real_travel_dis / need_reverse_travel_dis;
            writer.travel(writer.x(), writer.y() + dy / 2);
            for (int i = 0; true; ++i) {
                need_reverse_travel_dis -= (xr - xl - 2 * old_tool_width);
                float offset_dis = need_reverse_travel_dis < 0 ? -need_reverse_travel_dis : 0.f;
                if (left_to_right)
                    writer.travel(xr - old_tool_width - offset_dis, writer.y(), nozzle_change_speed);
                else
                    writer.travel(xl + old_tool_width + offset_dis, writer.y(), nozzle_change_speed);
                if (need_reverse_travel_dis < EPSILON)
                    break;
                if (i == tpu_line_count - 1)
                    break;
                writer.travel(writer.x(), writer.y() - dy);
                left_to_right = !left_to_right;
            }
        } else {
            // Wipe back along the last ramming line.
            writer.add_wipe_point(writer.x(), writer.y()).add_wipe_point(m_left_to_right ? 0.f : m_wipe_tower_width, writer.y());
        }
        if (!extruder_change)
            writer.append("M633\n");
    }

    writer.append(nozzle_change_marker(false, old_tool, new_tool, plan));

    if (old_tool < m_used_filament_length.size() && layer_is_printed(true))
        m_used_filament_length[old_tool] += writer.get_and_reset_used_filament_length();

    result.start_pos          = writer.start_pos_rotated();
    result.origin_start_pos   = initial_position;
    result.end_pos            = writer.pos_rotated();
    result.gcode              = writer.gcode();
    result.wipe_path          = writer.wipe_path();
    result.is_extruder_change = extruder_change;
    return result;
}

// Bambu Studio 2.9.1 heater_off_nozzle_change() (WipeTower.cpp:3922): markers only; GCode turns the fan to full
// and keeps the toolchange retraction at the model, and the pre-cooling pass turns the outgoing heater down.
WipeTower::NozzleChangeResult WipeTower::heater_off_nozzle_change(size_t old_tool, size_t new_tool, const WipeTowerInfo::ToolChange &planned) const
{
    NozzleChangeResult result;
    result.heater_off_only     = true;
    result.is_extruder_change  = true;
    result.precool_filament_id = int(old_tool);
    result.gcode               = nozzle_change_marker(true, old_tool, new_tool, planned.nozzle_change) +
                                 nozzle_change_marker(false, old_tool, new_tool, planned.nozzle_change);
    return result;
}

// Bambu Studio 2.9.1 idle_travel_cooling() (WipeTower.cpp:3944-4054): with no ramming length the outgoing hotend
// cools while the head travels (no extrusion) over the new filament's purge area, timed by hotend_cooling_rate.
WipeTower::NozzleChangeResult WipeTower::idle_travel_cooling(size_t old_tool, size_t new_tool, const WipeTowerInfo::ToolChange &planned)
{
    const NozzleChangePlan &plan = planned.nozzle_change;
    const float wipe_length = planned.wipe_length;
    const float wipe_depth  = planned.required_depth - planned.nozzle_change_depth;

    const float new_width = tool_line_width(new_tool);
    // Orca #10780: the new filament's purge rows in its category's block.
    const float dy        = m_layer_info->extra_spacing * get_block_gap_width(new_tool, false);
    const float xl        = new_width;
    const float xr        = m_wipe_tower_width - xl;
    const float line_len  = xr - xl;

    const int line_count_by_length = line_len > EPSILON ? std::max(1, int(std::ceil(wipe_length / line_len))) : 0;
    const int line_count_by_depth  = dy > EPSILON ? int(std::floor(wipe_depth / dy)) : 0;
    const int line_count           = std::min(line_count_by_length, std::max(1, line_count_by_depth));
    if (line_count <= 0)
        return NozzleChangeResult();

    const FilamentParameters &fp             = m_filpar[old_tool];
    const bool                extruder_change = plan.extruder_change;
    const int                 precool_target  = extruder_change ? fp.precool_target_temp.first : fp.precool_target_temp.second;
    const float               max_travel_speed = m_travel_speed * 60.f;
    float                     travel_speed     = max_travel_speed;
    if (precool_target > 0) {
        // Bambu Studio indexes the cooling rate by the outgoing nozzle.
        float cooling_rate = plan.old_nozzle >= 0 && plan.old_nozzle < int(m_hotend_cooling_rate.size()) ? m_hotend_cooling_rate[plan.old_nozzle] : 2.f;
        if (cooling_rate < EPSILON)
            cooling_rate = 2.f;
        const float current_temp = float(fp.nozzle_temperature);
        if (current_temp > precool_target) {
            const float cooling_time     = (current_temp - precool_target) / cooling_rate;
            const float total_travel_len = line_count * line_len;
            if (total_travel_len > EPSILON && cooling_time > EPSILON)
                travel_speed = std::min(max_travel_speed, total_travel_len / cooling_time * 60.f);
        }
    }

    WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_has_nozzle_rack, m_bbl_gcode_tags);
    writer.set_extrusion_flow(m_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(old_tool)
        .set_origin_offset(m_rib_offset)
        .set_accelerations(writer_tower_acceleration(), m_tower_travel_acceleration)
        .set_y_shift(m_y_shift)
        .append(nozzle_change_marker(true, old_tool, new_tool, plan));

    // Alternate the entry corner every layer, like the purge does.
    const bool  start_at_left   = (m_cur_layer_id % 4 == 0) || (m_cur_layer_id % 4 == 3);
    const bool  start_at_bottom = (m_cur_layer_id % 4 == 0) || (m_cur_layer_id % 4 == 2);
    const WipeTowerBlock *block = get_block_by_category(get_filament_category(new_tool), false);
    const float cur_depth       = block != nullptr ? block->cur_depth : m_perimeter_width;
    const float depth_span      = (line_count - 1) * dy;
    const Vec2f start_pos(start_at_left ? xl : xr, start_at_bottom ? cur_depth : cur_depth + depth_span);
    writer.set_initial_position(start_pos, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    writer.append(precool_target > 0 ? format_line_M104(precool_target, old_tool, plan, false, "Wipe tower idle travel pre cooling") :
                                       format_line_M104(0, old_tool, plan, false, "Wipe tower idle travel heater off"));
    writer.append("M632 S" + std::to_string(new_tool) + " D\n");

    bool        moving_right = start_at_left;
    const float row_step     = start_at_bottom ? dy : -dy;
    for (int i = 0; i < line_count; ++i) {
        writer.travel(moving_right ? xr : xl, writer.y(), travel_speed);
        if (i < line_count - 1)
            writer.travel(writer.x(), writer.y() + row_step);
        moving_right = !moving_right;
    }
    writer.append("M633\n");
    writer.append(nozzle_change_marker(false, old_tool, new_tool, plan));

    NozzleChangeResult result;
    result.start_pos               = writer.start_pos_rotated();
    result.origin_start_pos        = start_pos;
    result.end_pos                 = writer.pos_rotated();
    result.gcode                   = writer.gcode();
    result.is_extruder_change      = extruder_change;
    result.zero_flush_idle_cooling = true;
    result.precool_target_temp     = precool_target;
    result.precool_filament_id     = int(old_tool);
    return result;
}


// Ram the hot material out of the melt zone, retract the filament into the cooling tubes and let it cool.
void WipeTower::toolchange_Unload(
	WipeTowerWriter &writer,
	const box_coordinates 	&cleaning_box,
	const std::string&		 current_material,
	const int 				 new_temperature)
{
    // BBS: toolchange unload is done in change_filament_gcode
#if 0
	float xl = cleaning_box.ld.x() + 1.f * m_perimeter_width;
	float xr = cleaning_box.rd.x() - 1.f * m_perimeter_width;

	const float line_width = m_perimeter_width * m_filpar[m_current_tool].ramming_line_width_multiplicator;       // desired ramming line thickness
	const float y_step = line_width * m_filpar[m_current_tool].ramming_step_multiplicator * m_extra_spacing; // spacing between lines in mm

    writer.append("; CP TOOLCHANGE UNLOAD\n")
        .change_analyzer_line_width(line_width);

	unsigned i = 0;										// iterates through ramming_speed
	m_left_to_right = true;								// current direction of ramming
	float remaining = xr - xl ;							// keeps track of distance to the next turnaround
	float e_done = 0;									// measures E move done from each segment

	writer.travel(xl, cleaning_box.ld.y() + m_depth_traversed + y_step/2.f ); // move to starting position

    // if the ending point of the ram would end up in mid air, align it with the end of the wipe tower:
    if (m_layer_info > m_plan.begin() && m_layer_info < m_plan.end() && (m_layer_info-1!=m_plan.begin() || !m_adhesion )) {

        // this is y of the center of previous sparse infill border
        float sparse_beginning_y = 0.f;
        if (m_current_shape == SHAPE_REVERSED)
            sparse_beginning_y += ((m_layer_info-1)->depth - (m_layer_info-1)->toolchanges_depth())
                                      - ((m_layer_info)->depth-(m_layer_info)->toolchanges_depth()) ;
        else
            sparse_beginning_y += (m_layer_info-1)->toolchanges_depth() + m_perimeter_width;

        float sum_of_depths = 0.f;
        for (const auto& tch : m_layer_info->tool_changes) {  // let's find this toolchange
            if (tch.old_tool == m_current_tool) {
                sum_of_depths += tch.ramming_depth;
                float ramming_end_y = sum_of_depths;
                ramming_end_y -= (y_step/m_extra_spacing-m_perimeter_width) / 2.f;   // center of final ramming line

                if ( (m_current_shape == SHAPE_REVERSED   && ramming_end_y < sparse_beginning_y - 0.5f*m_perimeter_width  ) ||
                     (m_current_shape == SHAPE_NORMAL && ramming_end_y > sparse_beginning_y + 0.5f*m_perimeter_width  )  )
                {
                    writer.extrude(xl + tch.first_wipe_line-1.f*m_perimeter_width,writer.y());
                    remaining -= tch.first_wipe_line-1.f*m_perimeter_width;
                }
                break;
            }
            sum_of_depths += tch.required_depth;
        }
    }

    writer.disable_linear_advance();

    // now the ramming itself:
    while (i < m_filpar[m_current_tool].ramming_speed.size())
    {
        const float x = volume_to_length(m_filpar[m_current_tool].ramming_speed[i] * 0.25f, line_width, m_layer_height);
        const float e = m_filpar[m_current_tool].ramming_speed[i] * 0.25f / filament_area(); // transform volume per sec to E move;
        const float dist = std::min(x - e_done, remaining);		  // distance to travel for either the next 0.25s, or to the next turnaround
        const float actual_time = dist/x * 0.25f;
        writer.ram(writer.x(), writer.x() + (m_left_to_right ? 1.f : -1.f) * dist, 0.f, 0.f, e * (dist / x), dist / (actual_time / 60.f));
        remaining -= dist;

		if (remaining < WT_EPSILON)	{ // we reached a turning point
			writer.travel(writer.x(), writer.y() + y_step, 7200);
			m_left_to_right = !m_left_to_right;
			remaining = xr - xl;
		}
		e_done += dist; // subtract what was actually done
		if (e_done > x - WT_EPSILON) { // current segment finished
			++i;
			e_done = 0;
		}
	}
	Vec2f end_of_ramming(writer.x(),writer.y());
    writer.change_analyzer_line_width(m_perimeter_width);   // so the next lines are not affected by ramming_line_width_multiplier

    // Retraction:
    float old_x = writer.x();
    float turning_point = (!m_left_to_right ? xl : xr );
    if (m_semm && (m_cooling_tube_retraction != 0 || m_cooling_tube_length != 0)) {
        float total_retraction_distance = m_cooling_tube_retraction + m_cooling_tube_length/2.f - 15.f; // the 15mm is reserved for the first part after ramming
        writer.suppress_preview()
              .retract(15.f, m_filpar[m_current_tool].unloading_speed_start * 60.f) // feedrate 5000mm/min = 83mm/s
              .retract(0.70f * total_retraction_distance, 1.0f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .retract(0.20f * total_retraction_distance, 0.5f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .retract(0.10f * total_retraction_distance, 0.3f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .resume_preview();
    }
    // Wipe tower should only change temperature with single extruder MM. Otherwise, all temperatures should
    // be already set and there is no need to change anything. Also, the temperature could be changed
    // for wrong extruder.
    if (m_semm) {
        if (new_temperature != 0 && (new_temperature != m_old_temperature || is_first_layer()) ) { 	// Set the extruder temperature, but don't wait.
            // If the required temperature is the same as last time, don't emit the M104 again (if user adjusted the value, it would be reset)
            // However, always change temperatures on the first layer (this is to avoid issues with priming lines turned off).
            writer.set_extruder_temp(new_temperature, false);
            m_old_temperature = new_temperature;
        }
    }

    // Cooling:
    const int& number_of_moves = m_filpar[m_current_tool].cooling_moves;
    if (number_of_moves > 0) {
        const float& initial_speed = m_filpar[m_current_tool].cooling_initial_speed;
        const float& final_speed   = m_filpar[m_current_tool].cooling_final_speed;

        float speed_inc = (final_speed - initial_speed) / (2.f * number_of_moves - 1.f);

        writer.suppress_preview()
              .travel(writer.x(), writer.y() + y_step);
        old_x = writer.x();
        turning_point = xr-old_x > old_x-xl ? xr : xl;
        for (int i=0; i<number_of_moves; ++i) {
            float speed = initial_speed + speed_inc * 2*i;
            writer.load_move_x_advanced(turning_point, m_cooling_tube_length, speed);
            speed += speed_inc;
            writer.load_move_x_advanced(old_x, -m_cooling_tube_length, speed);
        }
    }

    // let's wait is necessary:
    writer.wait(m_filpar[m_current_tool].delay);
    // we should be at the beginning of the cooling tube again - let's move to parking position:
    writer.retract(-m_cooling_tube_length/2.f+m_parking_pos_retraction-m_cooling_tube_retraction, 2000);

	// this is to align ramming and future wiping extrusions, so the future y-steps can be uniform from the start:
    // the perimeter_width will later be subtracted, it is there to not load while moving over just extruded material
	writer.travel(end_of_ramming.x(), end_of_ramming.y() + (y_step/m_extra_spacing-m_perimeter_width) / 2.f + m_perimeter_width, 2400.f);

	writer.resume_preview()
		  .flush_planner_queue();
#endif
}

// Change the tool, set a speed override for soluble and flex materials.
void WipeTower::toolchange_Change(
	WipeTowerWriter &writer,
    const size_t 	new_tool,
    const std::string&  new_material)
{
    // Ask the writer about how much of the old filament we consumed:
    if (m_current_tool < m_used_filament_length.size())
    	m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    // This is where we want to place the custom gcodes. We will use placeholders for this.
    // These will be substituted by the actual gcodes when the gcode is generated.
    writer.append("[filament_end_gcode]\n");
    // Orca #15441: restore Z to the real topmost printed layer before running change_filament_gcode.
    // The wipe tower can be printing below that height (e.g. wipe_tower_no_sparse_layers), and
    // change_filament_gcode is free to travel anywhere on the bed, so it must not run while the
    // toolhead sits lower than already-printed parts. See append_tcr() in GCode.cpp.
    writer.append("[restore_layer_z_before_toolchange]\n");
    writer.append("[change_filament_gcode]\n");
    // Orca #15441: bring Z back down to the wipe tower layer once change_filament_gcode has finished,
    // so the rest of the tower (filament_start_gcode, wipe, ...) prints at the right height.
    writer.append("[deretraction_from_wipe_tower_generator]\n");

    // BBS: do travel in GCode::append_tcr() for lazy_lift
#if 0
    // Travel to where we assume we are. Custom toolchange or some special T code handling (parking extruder etc)
    // gcode could have left the extruder somewhere, we cannot just start extruding. We should also inform the
    // postprocessor that we absolutely want to have this in the gcode, even if it thought it is the same as before.
    Vec2f current_pos = writer.pos_rotated();
    writer.feedrate(m_travel_speed * 60.f)
          .append(std::string("G1 X") + Slic3r::float_to_string_decimal_point(current_pos.x())
                             +  " Y"  + Slic3r::float_to_string_decimal_point(current_pos.y())
                             + never_skip_tag() + "\n");
#endif

    // The toolchange Tn command will be inserted later, only in case that the user does
    // not provide a custom toolchange gcode.
	writer.set_tool(new_tool); // This outputs nothing, the writer just needs to know the tool has changed.
    writer.append("[filament_start_gcode]\n");

	writer.flush_planner_queue();
	m_current_tool = new_tool;
}

void WipeTower::toolchange_Load(
	WipeTowerWriter &writer,
	const box_coordinates  &cleaning_box)
{
    // BBS: tool load is done in change_filament_gcode
#if 0
    if (m_semm && (m_parking_pos_retraction != 0 || m_extra_loading_move != 0)) {
        float xl = cleaning_box.ld.x() + m_perimeter_width * 0.75f;
        float xr = cleaning_box.rd.x() - m_perimeter_width * 0.75f;
        float oldx = writer.x();	// the nozzle is in place to do the first wiping moves, we will remember the position

        // Load the filament while moving left / right, so the excess material will not create a blob at a single position.
        float turning_point = ( oldx-xl < xr-oldx ? xr : xl );
        float edist = m_parking_pos_retraction+m_extra_loading_move;

        writer.append("; CP TOOLCHANGE LOAD\n")
              .suppress_preview()
              .load(0.2f * edist, 60.f * m_filpar[m_current_tool].loading_speed_start)
              .load_move_x_advanced(turning_point, 0.7f * edist,        m_filpar[m_current_tool].loading_speed)  // Fast phase
              .load_move_x_advanced(oldx,          0.1f * edist, 0.1f * m_filpar[m_current_tool].loading_speed)  // Super slow*/

              .travel(oldx, writer.y()) // in case last move was shortened to limit x feedrate
              .resume_preview();

        // Reset the extruder current to the normal value.
        if (m_set_extruder_trimpot)
            writer.set_extruder_trimpot(550);
    }
#endif
}

// Orca #10780 / #15035 (Bambu Studio toolchange_wipe_new()): purge the newly loaded filament into its box, row by row
// at the block's gap, from the bottom on even layers and from the top on odd ones. With a wall gap the first row
// irons the start blob (and spirals it flat with prime_tower_flat_ironing); a contact change purges solid and slow.
void WipeTower::toolchange_wipe_new(WipeTowerWriter &writer, const box_coordinates &cleaning_box, float wipe_length, bool solid_toolchange,
                                    bool reheat, bool after_nozzle_change)
{
    // Mixed nozzle diameters (Edge, per-nozzle P2): the purge is laid by the tool just loaded (m_current_tool), at
    // its own width and flow. The box edges and the wall overlap stay on the structure width.
    const float tool_pw     = tool_line_width(m_current_tool);
    const float tool_flow   = tool_extrusion_flow(m_current_tool);
    const bool  tool_width  = tool_pw != m_perimeter_width;
    const bool  first_layer = is_first_layer();

    writer.set_extrusion_flow(tool_flow * (first_layer ? m_first_layer_flow_ratio : 1.f))
          .append("; CP TOOLCHANGE WIPE\n");
    // BBS: add the note for gcode-check, when the flow changed, the width should follow the change. After a
    // nozzle change the ramming's own width was the last one announced (its G-code lands in this change).
    if (first_layer)
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) + analyzer_tag_value(m_first_layer_flow_ratio * tool_pw) + "\n");
    else if (tool_width || after_nozzle_change)
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) + analyzer_tag_value(tool_pw) + "\n");

    const float retract_length      = m_filpar[m_current_tool].retract_length;
    const float retract_speed       = m_filpar[m_current_tool].retract_speed * 60.f;
    const float xl                  = cleaning_box.ld.x();
    const float xr                  = cleaning_box.rd.x();
    // A contact change neither irons nor stops early: it fills its box solid.
    const bool  should_flat_ironing = m_flat_ironing && !solid_toolchange;
    const bool  should_line_ironing = !solid_toolchange;
    float       x_to_wipe           = solid_toolchange ? std::numeric_limits<float>::max() : wipe_length;
    float       dy                  = first_layer ? tool_pw : m_layer_info->extra_spacing * get_block_gap_width(m_current_tool, false);
    if (solid_toolchange)
        dy = tool_pw;
    float target_speed = first_layer ? std::min(m_first_layer_speed * 60.f, m_max_speed) : m_max_speed;
    target_speed       = solid_toolchange ? m_contact_speed : target_speed;
    const std::vector<float> wipe_speed_map{ 0.33f * target_speed, 0.375f * target_speed, 0.458f * target_speed, 0.875f * target_speed,
                                             std::min(target_speed, 0.875f * target_speed + 50.f) };
    float wipe_speed = wipe_speed_map[0];

    m_left_to_right       = ((m_cur_layer_id + 3) % 4 >= 2);
    const bool is_from_up = (m_cur_layer_id % 2 == 1);

    auto reheat_before_wipe = [&writer, reheat, this]() {
        if (reheat && m_filpar[m_current_tool].cooling_before_tower > EPSILON)
            writer.append("M104 T" + std::to_string(m_filpar[m_current_tool].reheat_extruder) + " S" +
                          std::to_string(m_filpar[m_current_tool].nozzle_temperature) + " N0 ;Wipe tower reheat before wipe\n");
    };

    for (int i = 0; true; ++i) {
        if (i < int(wipe_speed_map.size()))
            wipe_speed = wipe_speed_map[i];

        // BBS: check the bridging area and use the bridge flow
        const bool need_change_flow = need_thick_bridge_flow(writer.y());
        if (need_change_flow) {
            writer.set_extrusion_flow(tool_extrusion_flow(m_current_tool, 0.2f));
            writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + analyzer_tag_value(0.2) + "\n");
        }

        // Edge: the reheat goes before the first extrusion of the purge, the start ironing included (Bambu Studio
        // writes it after the ironing, which then prints at the cooled temperature).
        if (i == 0)
            reheat_before_wipe();
        if (i == 0 && m_use_gap_wall) {
            // BBS: iron the start blob into the wall gap, then purge.
            float ironing_length = 3.f;
            const float edge     = m_left_to_right ? xr + wipe_tower_wall_infill_overlap * m_perimeter_width :
                                                     xl - wipe_tower_wall_infill_overlap * m_perimeter_width;
            const float sign     = m_left_to_right ? 1.f : -1.f;
            if (should_line_ironing) {
                const float dx = edge - writer.pos().x();
                if (std::abs(dx) < ironing_length)
                    ironing_length = std::abs(dx);
                writer.extrude(writer.x() + sign * ironing_length, writer.y(), wipe_speed);
                writer.retract(retract_length, retract_speed);
                writer.travel(writer.x() - sign * 1.5f * ironing_length, writer.y(), 600.f);
                if (should_flat_ironing) {
                    writer.travel(writer.x() + sign * 0.5f * ironing_length, writer.y(), 240.f);
                    const Vec2f pos{ writer.x() + sign * 1.f * ironing_length, writer.y() };
                    writer.spiral_flat_ironing(writer.pos(), m_filpar[m_current_tool].flat_iron_area, m_perimeter_width, flat_iron_speed);
                    writer.travel(pos, wipe_speed);
                } else
                    writer.travel(writer.x() + sign * 1.5f * ironing_length, writer.y(), 240.f);
                writer.retract(-retract_length, retract_speed);
            }
            writer.extrude(edge, writer.y(), wipe_speed);
        } else {
            if (m_left_to_right)
                writer.extrude(xr + wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), wipe_speed);
            else
                writer.extrude(xl - wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), wipe_speed);
        }

        // BBS: recover the flow in non-bridging area
        if (need_change_flow) {
            writer.set_extrusion_flow(tool_flow);
            writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + analyzer_tag_value(m_layer_height) + "\n");
        }

        if (!is_from_up && (writer.y() + dy - float(EPSILON) > cleaning_box.lu.y() - m_perimeter_width))
            break; // in case next line would not fit
        // The top of the box takes no row, its bottom does.
        if (is_from_up && (writer.y() - dy + float(EPSILON)) < cleaning_box.ld.y())
            break;

        x_to_wipe -= (xr - xl);
        if (x_to_wipe < WT_EPSILON)
            break;
        // stepping to the next line:
        writer.extrude(writer.x(), writer.y() + (is_from_up ? -dy : dy));
        m_left_to_right = !m_left_to_right;
    }

    // BBS: modify the wipe_path after toolchange
    writer.add_wipe_point(writer.x(), writer.y()).add_wipe_point(!m_left_to_right ? m_wipe_tower_width : 0.f, writer.y());

    if (m_layer_info != m_plan.end() && m_current_tool != m_layer_info->tool_changes.back().new_tool)
        m_left_to_right = !m_left_to_right;

    writer.set_extrusion_flow(m_extrusion_flow); // Reset the extrusion flow.
    // BBS: add the note for gcode-check when the flow changed
    if (first_layer || tool_width || after_nozzle_change)
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) + analyzer_tag_value(m_perimeter_width) + "\n");
}


// BBS
WipeTower::box_coordinates WipeTower::align_perimeter(const WipeTower::box_coordinates& perimeter_box)
{
    box_coordinates aligned_box = perimeter_box;

    float spacing = m_extra_spacing * m_perimeter_width;
    float up = perimeter_box.lu(1) - m_perimeter_width;
    up = align_ceil(up, spacing);
    up += m_perimeter_width;
    up = std::min(up, m_wipe_tower_depth);

    float down = perimeter_box.ld(1) - m_perimeter_width;
    down = align_floor(down, spacing);
    down += m_perimeter_width;
    down = std::max(down, -m_y_shift);

    aligned_box.lu(1) = aligned_box.ru(1) = up;
    aligned_box.ld(1) = aligned_box.rd(1) = down;

    return aligned_box;
}

// Orca #10780 / #15035 (Bambu Studio finish_layer_new()): the layer's outer wall and brim; on a layer without tool
// changes also the sparse fill of every block (extrude_fill_wall / extrude_fill). The blocks a layer's tool changes
// leave unfilled are finished by finish_block() / finish_block_solid().
WipeTower::ToolChangeResult WipeTower::finish_layer_new(bool extrude_perimeter, bool extrude_fill, bool extrude_fill_wall)
{
    assert(!this->layer_finished());
    m_current_layer_finished = true;

    const size_t old_tool = m_current_tool;

    WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_has_nozzle_rack, m_bbl_gcode_tags);
    writer.set_extrusion_flow(m_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(m_current_tool)
        .set_origin_offset(m_rib_offset)
        .set_arc_fitting(m_enable_arc_fitting)
        .set_accelerations(writer_tower_acceleration(), m_tower_travel_acceleration)
        .set_y_shift(m_y_shift);

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");

    // Slow down on the 1st layer.
    const bool  first_layer          = is_first_layer();
    // BBS: speed up perimeter speed to 90mm/s for non-first layer (prime_tower_max_speed, 90 mm/s by default)
    const float feedrate             = wall_feedrate(m_current_tool);
    const bool  toolchanges_on_layer = m_layer_info->toolchanges_depth() > WT_EPSILON;

    std::vector<Vec2f> finish_rect_wipe_path;
    // One fill box per block when there is more than one, else one for the whole tower.
    const bool                   multi_block_fill = m_wipe_tower_blocks.size() > 1 && (extrude_fill_wall || extrude_fill);
    std::vector<box_coordinates> fill_boxes;
    std::vector<int>             fill_box_categories; // multi_block_fill only
    std::vector<float>           fill_box_origins;    // the block start: its purge rows lie on start + k * row pitch
    if (multi_block_fill) {
        for (const WipeTowerBlock &block : m_wipe_tower_blocks) {
            float block_fill_height = block.depth - 2 * m_perimeter_width;
            if (m_cur_layer_id < block.layer_depths.size())
                block_fill_height = block.layer_depths[m_cur_layer_id] - 2 * m_perimeter_width;
            if (block_fill_height <= WT_EPSILON)
                continue;
            fill_boxes.emplace_back(Vec2f(m_perimeter_width, block.start_depth + m_perimeter_width), m_wipe_tower_width - 2 * m_perimeter_width,
                                    block_fill_height);
            fill_box_categories.push_back(block.filament_adhesiveness_category);
            fill_box_origins.push_back(block.start_depth);
        }
    }
    if (fill_boxes.empty()) {
        float fill_box_depth = m_wipe_tower_depth - 2 * m_perimeter_width;
        if (m_wipe_tower_blocks.size() == 1)
            fill_box_depth = m_layer_info->depth - 2 * m_perimeter_width;
        fill_boxes.emplace_back(Vec2f(m_perimeter_width, m_perimeter_width), m_wipe_tower_width - 2 * m_perimeter_width, fill_box_depth);
    }

    writer.set_initial_position((m_left_to_right ? fill_boxes.front().ru : fill_boxes.front().lu), // so there is never a diagonal travel
                                m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    // Is there a soluble filament wiped/rammed at the next layer? If so, the infill should not be sparse.
    bool solid_infill = (m_layer_info + 1 == m_plan.end()) ?
                            false :
                            std::any_of((m_layer_info + 1)->tool_changes.begin(), (m_layer_info + 1)->tool_changes.end(),
                                        [this](const WipeTowerInfo::ToolChange &tch) { return m_filpar[tch.new_tool].is_soluble || m_filpar[tch.old_tool].is_soluble; });
    solid_infill |= first_layer && m_adhesion;

    for (size_t box_idx = 0; box_idx < fill_boxes.size(); ++box_idx) {
        const box_coordinates &fill_box = fill_boxes[box_idx];
        // Edge (#1052 review S3): rows on the block's purge grid under the next layer's purge or ramming rows in it.
        const bool box_grid = !solid_infill && box_idx < fill_box_categories.size() && next_layer_purges_into(fill_box_categories[box_idx]);
        if (box_idx > 0)
            writer.travel(m_left_to_right ? fill_box.ru : fill_box.lu);

        // inner perimeter of the sparse section, if there is space for it:
        if (extrude_fill_wall && (fill_box.ru.y() - fill_box.rd.y() > WT_EPSILON))
            writer.rectangle_fill_box(this, fill_box, finish_rect_wipe_path, m_perimeter_width, feedrate);
        // Extrude infill to support the material to be printed above.
        const float dy    = (fill_box.lu.y() - fill_box.ld.y() - m_perimeter_width);
        float       left  = fill_box.lu.x() + 2 * m_perimeter_width;
        float       right = fill_box.ru.x() - 2 * m_perimeter_width;
        if (extrude_fill && dy > m_perimeter_width) {
            writer.travel(fill_box.ld + Vec2f(m_perimeter_width * 2, 0.f))
                  .append(";--------------------\n"
                          "; CP EMPTY GRID START\n")
                  .comment_with_value(" layer #", m_num_layer_changes + 1);
            if (box_grid) {
                grid_fill(writer, fill_box, fill_box_origins[box_idx], block_row_pitch(fill_box_categories[box_idx]), feedrate);
            } else if (solid_infill) {
                float sparse_factor = 1.5f; // 1=solid, 2=every other line, etc.
                if (first_layer) {          // the infill should touch perimeters
                    left -= m_perimeter_width;
                    right += m_perimeter_width;
                    sparse_factor = 1.f;
                }
                float y       = fill_box.ld.y() + m_perimeter_width;
                int   n       = dy / (m_perimeter_width * sparse_factor);
                float spacing = (dy - m_perimeter_width) / (n - 1);
                for (int i = 0; i < n; ++i) {
                    writer.extrude(writer.x(), y, feedrate).extrude(i % 2 ? left : right, y);
                    y = y + spacing;
                }
                writer.extrude(writer.x(), fill_box.lu.y());
            } else {
                // Extrude an inverse U at the left of the region and the sparse infill.
                writer.extrude(fill_box.lu + Vec2f(m_perimeter_width * 2, 0.f), feedrate);
                const int   n  = 1 + int((right - left) / m_bridging);
                const float dx = (right - left) / n;
                for (int i = 1; i <= n; ++i) {
                    float x = left + dx * i;
                    writer.travel(x, writer.y());
                    writer.extrude(x, i % 2 ? fill_box.rd.y() : fill_box.ru.y());
                }
                // BBS: add wipe_path for this case: only with finish rectangle
                finish_rect_wipe_path.clear();
                finish_rect_wipe_path.emplace_back(writer.pos());
                finish_rect_wipe_path.emplace_back(Vec2f(left + dx * n, n % 2 ? fill_box.ru.y() : fill_box.rd.y()));
            }
            writer.append("; CP EMPTY GRID END\n"
                          ";------------------\n\n\n\n\n\n\n");
        }
    }

    // outer perimeter (always): a single block narrows with the layer, several keep the tower's full depth.
    const float     wipe_tower_depth = m_wipe_tower_blocks.size() == 1 ? m_layer_info->depth + m_perimeter_width : m_wipe_tower_depth;
    box_coordinates wt_box(Vec2f(0.f, 0.f), m_wipe_tower_width, wipe_tower_depth);
    wt_box = align_perimeter(wt_box);
    // Bambu Studio finish_layer_new(): the rib wall is needed for the brim loops even when not printed here.
    Polygon outer_wall = print_outer_wall(writer, wt_box, feedrate, extrude_perimeter);

    // A tower interface run-in also crosses the brim loops (the chamfer above the first layer).
    const size_t layer_id = size_t(m_layer_info - m_plan.begin());
    const std::vector<TowerInterface::GapPoint>  no_gaps;
    const std::vector<TowerInterface::GapPoint> &loop_gaps = layer_id < m_interface_gaps.size() ? m_interface_gaps[layer_id] : no_gaps;
    auto print_loop = [&writer, &loop_gaps, feedrate, this](const Polygon &loop, bool pre_simplify) {
        if (loop_gaps.empty()) {
            writer.polygon(loop, feedrate, pre_simplify);
        } else {
            Polygon inserted;
            writer.wall_pieces(TowerInterface::cut_wall_gaps(loop, loop_gaps, 2.5f * m_perimeter_width, inserted), feedrate);
        }
    };

    // brim chamfer
    float spacing = m_perimeter_width - m_layer_height * float(1. - M_PI_4);
    // How many perimeters shall the brim have?
    int         loops_num         = (m_wipe_tower_brim_width + spacing / 2.f) / spacing;
    const float max_chamfer_width = 3.f;
    if (!first_layer) {
        // stop print chamfer if depth changes
        if (m_layer_info->depth != m_plan.front().depth) {
            loops_num = 0;
        } else {
            // limit max chamfer width to 3 mm
            int chamfer_loops_num = (int) (max_chamfer_width / spacing);
            // Edge (OD16 review): in printed layers, so a combined tower steps one loop per printed layer.
            int dist_to_1st = wipe_tower_printed_layer_distance(m_plan, m_first_layer_idx, layer_id);
            // A layer below the tower's first real layer gets no chamfer (no sparse layers: those layers are
            // never printed, but their walls still go to the conflict checker).
            loops_num = dist_to_1st < 0 ? 0 : std::min(loops_num, chamfer_loops_num) - dist_to_1st;
        }
    }

    if (loops_num > 0 && m_use_rib_wall) {
        // Bambu Studio finish_layer_new(): the brim follows the rib wall.
        for (int i = 0; i < loops_num; ++i) {
            Polygons grown = offset(outer_wall, scaled(spacing));
            if (grown.empty())
                break;
            outer_wall = std::move(grown.front());
            print_loop(outer_wall, true);
            record_outer_wall(outer_wall);
        }
        if (first_layer)
            m_wipe_tower_brim_width_real = loops_num * spacing + spacing / 2.f;
    } else if (loops_num > 0) {
        box_coordinates box = wt_box;
        for (int i = 0; i < loops_num; ++i) {
            box.expand(spacing);
            // Explicit feedrate: the last F in the writer may be a travel.
            if (loop_gaps.empty())
                writer.rectangle(box, feedrate);
            else
                print_loop(generate_rectange_polygon(box.ld, box.ru), false);
        }
        if (first_layer) {
            // Save actual brim width to be later passed to the Print object, which will use it
            // for skirt calculation and pass it to GLCanvas for precise preview box
            m_wipe_tower_brim_width_real = wt_box.ld.x() - box.ld.x() + spacing / 2.f;
        }
        wt_box = box;
    }

    if (m_use_rib_wall && (extrude_perimeter || loops_num > 0)) {
        // Bambu Studio: wipe back along the wall just printed.
        writer.add_wipe_path(outer_wall, m_filpar[m_current_tool].wipe_dist);
    } else {
        // Now prepare future wipe. box contains rectangle that was extruded last (ccw).
        Vec2f target = (writer.pos() == wt_box.ld ? wt_box.rd :
                       (writer.pos() == wt_box.rd ? wt_box.ru :
                       (writer.pos() == wt_box.ru ? wt_box.lu :
                        wt_box.ld)));
        // BBS: add wipe_path for this case: only with finish rectangle
        if (finish_rect_wipe_path.size() == 2 && finish_rect_wipe_path[0] == writer.pos())
            target = finish_rect_wipe_path[1];
        writer.add_wipe_point(writer.pos()).add_wipe_point(target);
    }

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");

    // Ask our writer about how much material was consumed.
    // Skip this in case the layer is sparse and config option to not print sparse layers is enabled.
    if (layer_is_printed(toolchanges_on_layer))
        if (m_current_tool < m_used_filament_length.size())
            m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    return construct_tcr(writer, false, old_tool, true, false, 0.f, false);
}

// Edge (#1052 review S3): whether the next tower layer lays purge or ramming rows into the block of this category.
// Always false on a single-block tower (unchanged from Orca there).
bool WipeTower::next_layer_purges_into(int category) const
{
    if (m_wipe_tower_blocks.size() < 2 || m_layer_info == m_plan.end() || m_layer_info + 1 == m_plan.end())
        return false;
    for (const WipeTowerInfo::ToolChange &tc : (m_layer_info + 1)->tool_changes) {
        if (get_filament_category(tc.new_tool) == category)
            return true;
        if (tc.nozzle_change.need_ramming && tc.nozzle_change_depth > EPSILON && get_filament_category(tc.old_tool) == category)
            return true;
    }
    return false;
}

// Edge (#1052 review S3): the pitch of a block's purge rows on the current layer.
float WipeTower::block_row_pitch(int category) const
{
    const auto it = m_block_infill_gap_width.find(category);
    return m_layer_info->extra_spacing * (it == m_block_infill_gap_width.end() ? m_perimeter_width : it->second.first);
}

// Edge (#1052 review S3): rows across fill_box on the lattice origin_y + k * row, strictly inside the box (its rectangle
// prints the edges), travelling between them.
void WipeTower::grid_fill(WipeTowerWriter &writer, const box_coordinates &fill_box, float origin_y, float row, float feedrate) const
{
    if (row < m_perimeter_width - WT_EPSILON)
        row = m_perimeter_width;
    float y = origin_y + std::ceil((fill_box.ld.y() - origin_y) / row - 1e-4f) * row;
    if (y < fill_box.ld.y() + WT_EPSILON)
        y += row;
    const float xl = fill_box.ld.x(), xr = fill_box.rd.x();
    bool        left_to_right = std::abs(writer.x() - xl) <= std::abs(writer.x() - xr);
    for (; y < fill_box.lu.y() - WT_EPSILON; y += row) {
        writer.travel(left_to_right ? xl : xr, y);
        writer.extrude(left_to_right ? xr : xl, y, feedrate);
        left_to_right = !left_to_right;
    }
}

// Orca #10780 (Bambu Studio finish_block()): the rest of a block this layer's tool changes did not purge into, as
// sparse fill printed with filament_id, a filament of the block's category where the layer has one.
WipeTower::ToolChangeResult WipeTower::finish_block(const WipeTowerBlock &block, size_t filament_id, bool extrude_fill)
{
    WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_has_nozzle_rack, m_bbl_gcode_tags);
    writer.set_extrusion_flow(m_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(filament_id)
        .set_origin_offset(m_rib_offset)
        .set_arc_fitting(m_enable_arc_fitting)
        .set_accelerations(writer_tower_acceleration(), m_tower_travel_acceleration)
        .set_y_shift(m_y_shift);

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");

    const bool  first_layer = is_first_layer();
    const float feedrate    = wall_feedrate(filament_id);

    box_coordinates fill_box(Vec2f(m_perimeter_width, block.cur_depth), m_wipe_tower_width - 2 * m_perimeter_width,
                             block.start_depth + block.layer_depths[m_cur_layer_id] - block.cur_depth - m_perimeter_width);

    writer.set_initial_position((m_left_to_right ? fill_box.ru : fill_box.lu), m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    const bool toolchanges_on_layer = m_layer_info->toolchanges_depth() > WT_EPSILON;

    std::vector<Vec2f> finish_rect_wipe_path;
    // inner perimeter of the sparse section, if there is space for it:
    if (fill_box.ru.y() - fill_box.rd.y() > WT_EPSILON)
        writer.rectangle_fill_box(this, fill_box, finish_rect_wipe_path, m_perimeter_width, feedrate);

    // Extrude infill to support the material to be printed above.
    const float dy    = (fill_box.lu.y() - fill_box.ld.y() - m_perimeter_width);
    float       left  = fill_box.lu.x() + 2 * m_perimeter_width;
    float       right = fill_box.ru.x() - 2 * m_perimeter_width;
    if (extrude_fill && dy > m_perimeter_width) {
        writer.travel(fill_box.ld + Vec2f(m_perimeter_width * 2, 0.f))
              .append(";--------------------\n"
                      "; CP EMPTY GRID START\n")
              .comment_with_value(" layer #", m_num_layer_changes + 1);

        // Is there a soluble filament wiped/rammed at the next layer? If so, the infill should not be sparse.
        bool solid_infill = m_layer_info + 1 == m_plan.end() ?
                                false :
                                std::any_of((m_layer_info + 1)->tool_changes.begin(), (m_layer_info + 1)->tool_changes.end(),
                                            [this](const WipeTowerInfo::ToolChange &tch) { return m_filpar[tch.new_tool].is_soluble || m_filpar[tch.old_tool].is_soluble; });
        solid_infill |= first_layer && m_adhesion;
        // Edge (#1052 review S3): a block whose category the next layer purges (or rams) into gets rows on its purge
        // grid instead of the sparse fill, so those rows are not bridges over it, and the rows are themselves on the
        // purge rows of the layer below (a solid fill at its own pitch is not, on a stretched tower). The soluble
        // rule above, for one more trigger. Only on a tower with more than one block: a single block is purged into
        // on every layer and stays as upstream.
        const bool grid = !solid_infill && next_layer_purges_into(block.filament_adhesiveness_category);

        if (grid) {
            grid_fill(writer, fill_box, fill_box.ld.y(), block_row_pitch(block.filament_adhesiveness_category), feedrate);
        } else if (solid_infill) {
            float sparse_factor = 1.5f; // 1=solid, 2=every other line, etc.
            if (first_layer) {          // the infill should touch perimeters
                left -= m_perimeter_width;
                right += m_perimeter_width;
                sparse_factor = 1.f;
            }
            float y       = fill_box.ld.y() + m_perimeter_width;
            int   n       = dy / (m_perimeter_width * sparse_factor);
            float spacing = (dy - m_perimeter_width) / (n - 1);
            for (int i = 0; i < n; ++i) {
                writer.extrude(writer.x(), y, feedrate).extrude(i % 2 ? left : right, y);
                y = y + spacing;
            }
            writer.extrude(writer.x(), fill_box.lu.y());
        } else {
            // Extrude an inverse U at the left of the region and the sparse infill.
            writer.extrude(fill_box.lu + Vec2f(m_perimeter_width * 2, 0.f), feedrate);
            const int   n  = 1 + int((right - left) / m_bridging);
            const float dx = (right - left) / n;
            for (int i = 1; i <= n; ++i) {
                float x = left + dx * i;
                writer.travel(x, writer.y());
                writer.extrude(x, i % 2 ? fill_box.rd.y() : fill_box.ru.y());
            }
            finish_rect_wipe_path.clear();
            // BBS: add wipe_path for this case: only with finish rectangle
            finish_rect_wipe_path.emplace_back(writer.pos());
            finish_rect_wipe_path.emplace_back(Vec2f(left + dx * n, n % 2 ? fill_box.ru.y() : fill_box.rd.y()));
        }

        writer.append("; CP EMPTY GRID END\n"
                      ";------------------\n\n\n\n\n\n\n");
    }

    // Now prepare future wipe.
    box_coordinates wt_box(Vec2f(0.f, 0.f), m_wipe_tower_width, m_layer_info->depth + m_perimeter_width);
    wt_box       = align_perimeter(wt_box);
    Vec2f target = (writer.pos() == wt_box.ld ? wt_box.rd : (writer.pos() == wt_box.rd ? wt_box.ru : (writer.pos() == wt_box.ru ? wt_box.lu : wt_box.ld)));
    // BBS: add wipe_path for this case: only with finish rectangle
    if (finish_rect_wipe_path.size() == 2 && finish_rect_wipe_path[0] == writer.pos())
        target = finish_rect_wipe_path[1];
    writer.add_wipe_point(writer.pos()).add_wipe_point(target);

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");

    // Ask our writer about how much material was consumed.
    // Skip this in case the layer is sparse and config option to not print sparse layers is enabled.
    if (layer_is_printed(toolchanges_on_layer))
        if (filament_id < m_used_filament_length.size())
            m_used_filament_length[filament_id] += writer.get_and_reset_used_filament_length();

    return construct_block_tcr(writer, filament_id, true);
}

// Orca #10780 (Bambu Studio finish_block_solid()): the rest of a block on a contact / solid layer, as a solid grid
// starting on the corner the layer number picks; a contact layer prints it at 20 mm/s, and a block over a sparse
// layer at the bridge flow of a 0.2 mm layer.
WipeTower::ToolChangeResult WipeTower::finish_block_solid(const WipeTowerBlock &block, size_t filament_id, WipeTowerLayerType layer_type)
{
    float layer_height = m_layer_height;
    float e_flow       = m_extrusion_flow;
    if (m_cur_layer_id > 1 && block.layers_type[m_cur_layer_id - 1] == WipeTowerLayerType::Normal && m_extrusion_flow < extrusion_flow(0.2f)) {
        layer_height = 0.2f;
        e_flow       = extrusion_flow(0.2f);
    }

    WipeTowerWriter writer(layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_has_nozzle_rack, m_bbl_gcode_tags);
    writer.set_extrusion_flow(e_flow)
        .set_z(m_z_pos)
        .set_initial_tool(filament_id)
        .set_origin_offset(m_rib_offset)
        .set_arc_fitting(m_enable_arc_fitting)
        .set_accelerations(writer_tower_acceleration(), m_tower_travel_acceleration)
        .set_y_shift(m_y_shift);

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");

    float feedrate = wall_feedrate(filament_id);
    if (layer_type == WipeTowerLayerType::Contact || layer_type == WipeTowerLayerType::Contact_UP)
        feedrate = m_contact_speed;
    box_coordinates fill_box(Vec2f(m_perimeter_width, block.cur_depth), m_wipe_tower_width - 2 * m_perimeter_width,
                             block.start_depth + block.layer_depths[m_cur_layer_id] - block.cur_depth - m_perimeter_width);
    const bool  toolchanges_on_layer = m_layer_info->toolchanges_depth() > WT_EPSILON;
    const float dy                   = (fill_box.lu.y() - fill_box.ld.y());
    const int   n                    = int((dy + 0.25f * m_perimeter_width) / m_perimeter_width + 1);
    const float spacing              = m_perimeter_width;
    Vec2f       initial_pos(0.f, 0.f);
    bool        up_to_down = false;
    {
        const float grid_depth = spacing * (n - 1);
        switch (m_cur_layer_id % 4) {
        case 0:
            initial_pos     = fill_box.ld;
            m_left_to_right = true;
            up_to_down      = false;
            break;
        case 1:
            initial_pos     = Vec2f(fill_box.rd.x(), fill_box.rd.y() + grid_depth);
            m_left_to_right = false;
            up_to_down      = true;
            break;
        case 2:
            initial_pos     = fill_box.rd;
            m_left_to_right = false;
            up_to_down      = false;
            break;
        default:
            initial_pos     = Vec2f(fill_box.ld.x(), grid_depth + fill_box.ld.y());
            m_left_to_right = true;
            up_to_down      = true;
            break;
        }
    }
    const float left  = fill_box.lu.x();
    const float right = fill_box.ru.x();
    writer.append(";--------------------\n"
                  "; CP EMPTY GRID START\n")
          .comment_with_value(" layer #", m_num_layer_changes + 1);
    writer.set_initial_position(initial_pos, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);
    for (int i = 0; i < n; ++i) {
        writer.extrude(m_left_to_right ? right : left, writer.y(), feedrate);
        if (i == n - 1) {
            writer.add_wipe_point(writer.pos()).add_wipe_point(Vec2f(m_left_to_right ? left : right, writer.y()));
            break;
        }
        m_left_to_right = !m_left_to_right;
        writer.extrude(writer.x(), writer.y() + spacing * (up_to_down ? -1.f : 1.f), feedrate);
    }
    writer.append("; CP EMPTY GRID END\n"
                  ";------------------\n\n\n\n\n\n\n");

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");

    // Ask our writer about how much material was consumed.
    // Skip this in case the layer is sparse and config option to not print sparse layers is enabled.
    if (layer_is_printed(toolchanges_on_layer))
        if (filament_id < m_used_filament_length.size())
            m_used_filament_length[filament_id] += writer.get_and_reset_used_filament_length();

    return construct_block_tcr(writer, filament_id, true);
}

// Appends a toolchange into m_plan and calculates neccessary depth of the corresponding box
float WipeTower::nozzle_change_width(size_t tool) const
{
    const float nozzle = tool < m_filpar.size() ? m_filpar[tool].nozzle_diameter : 0.f;
    return nozzle > 0.f ? nozzle_change_perimeter_width(nozzle) : 2.5f * m_perimeter_width / Width_To_Nozzle_Ratio;
}

float WipeTower::nozzle_change_length(size_t old_tool, bool extruder_change, float layer_height) const
{
    if (old_tool >= m_filpar.size() || layer_height <= 0.f)
        return 0.f;
    const FilamentParameters &fp     = m_filpar[old_tool];
    const float               length = extruder_change ? fp.change_length.first : fp.change_length.second;
    const float               width  = nozzle_change_width(old_tool);
    // Bambu Studio set_toolchange(): mm of filament per mm of a ramming line, at the ramming width.
    const double e_flow = layer_height * (width - layer_height * (1.f - float(M_PI) / 4.f)) / fp.filament_area;
    return e_flow > 0. ? float(length / e_flow) : 0.f;
}

float WipeTower::nozzle_change_depth(size_t old_tool, float length) const
{
    if (length <= 0.f)
        return 0.f;
    // The ramming box spans the tower between the outgoing filament's purge lines on both sides
    // (Bambu Studio ramming(): x_offset = width + (ramming width - width) / 2 on each side).
    const float nc_width  = nozzle_change_width(old_tool);
    const float box_width = m_wipe_tower_width - (nc_width + tool_line_width(old_tool));
    if (box_width <= EPSILON)
        return 0.f;
    return std::ceil(length / box_width) * nc_width;
}

void WipeTower::plan_toolchange(float z_par, float layer_height_par, unsigned int old_tool,
                                unsigned int new_tool, float wipe_volume, float purge_volume, const NozzleChangePlan &nozzle_change)
{
	assert(m_plan.empty() || m_plan.back().z <= z_par + WT_EPSILON);	// refuses to add a layer below the last one

	if (m_plan.empty() || m_plan.back().z + WT_EPSILON < z_par) // if we moved to a new layer, we'll add it to m_plan first
		m_plan.push_back(WipeTowerInfo(z_par, layer_height_par));

    if (m_first_layer_idx == size_t(-1) && (! m_sparse_layers_skipped || old_tool != new_tool))
        m_first_layer_idx = m_plan.size() - 1;

    if (old_tool == new_tool)	// new layer without toolchanges - we are done
        return;

    // BBS: if the wipe tower width is too small, the depth will be infinity
    if (m_wipe_tower_width - 2 * m_perimeter_width <= EPSILON)
        return;

    // Bambu Studio 2.9.1 (set_toolchange): the new filament's purge, and on a change onto another nozzle the
    // outgoing filament's ramming lines. generate() re-plans both at the block gaps (calc_block_infill_gap()).
    WipeTowerInfo::ToolChange tool_change(old_tool, new_tool, 0.f, 0.f, 0.f, wipe_volume, 0.f, purge_volume);
    tool_change.nozzle_change = nozzle_change;
    replan_toolchange(tool_change, layer_height_par);
    m_plan.back().tool_changes.push_back(tool_change);
}

// Bambu Studio set_toolchange() (Orca #10780 / #14685): whole purge lines at the new filament's block gap, plus
// whole ramming lines of the outgoing filament at its block's ramming gap. Edge (per-nozzle P2): each filament's
// own line width; uniform nozzles give Orca's numbers.
void WipeTower::replan_toolchange(WipeTowerInfo::ToolChange &tool_change, float layer_height) const
{
    const float width = m_wipe_tower_width - 2 * m_perimeter_width;
    if (width <= EPSILON)
        return;
    tool_change.wipe_length = volume_to_length(tool_change.wipe_volume, tool_line_width(tool_change.new_tool), layer_height);
    float depth             = std::ceil(tool_change.wipe_length / width) * get_block_gap_width(tool_change.new_tool, false);
    tool_change.nozzle_change_length = 0.f;
    tool_change.nozzle_change_depth  = 0.f;
    if (tool_change.nozzle_change.need_ramming) {
        tool_change.nozzle_change_length = nozzle_change_length(tool_change.old_tool, tool_change.nozzle_change.extruder_change, layer_height);
        // The ramming box spans the tower between the outgoing filament's purge lines on both sides.
        const float box_width = m_wipe_tower_width - (nozzle_change_width(tool_change.old_tool) + tool_line_width(tool_change.old_tool));
        if (tool_change.nozzle_change_length > 0.f && box_width > EPSILON)
            tool_change.nozzle_change_depth = std::ceil(tool_change.nozzle_change_length / box_width) * get_block_gap_width(tool_change.old_tool, true);
        depth += tool_change.nozzle_change_depth;
    }
    tool_change.required_depth = depth;
}

float WipeTower::get_block_gap_width(size_t tool, bool nozzle_change) const
{
    // Orca: a block that does not exist (yet) has no gap of its own; its lines are laid at their own width.
    // Edge (per-nozzle P2): the gap is the category's, from the structure / widest ramming width, moved by the
    // tool's own width; with uniform nozzles that is Orca's gap.
    const auto it = m_block_infill_gap_width.find(get_filament_category(tool));
    if (nozzle_change) {
        const float nc_width = nozzle_change_width(tool);
        return it == m_block_infill_gap_width.end() ? nc_width : it->second.second + (nc_width - m_nozzle_change_perimeter_width);
    }
    const float pw = tool_line_width(tool);
    return it == m_block_infill_gap_width.end() ? pw : it->second.first + (pw - m_perimeter_width);
}

WipeTower::WipeTowerBlock *WipeTower::get_block_by_category(int category, bool create)
{
    auto iter = std::find_if(m_wipe_tower_blocks.begin(), m_wipe_tower_blocks.end(),
                             [category](const WipeTowerBlock &item) { return item.filament_adhesiveness_category == category; });
    if (iter != m_wipe_tower_blocks.end())
        return &(*iter);
    if (create) {
        WipeTowerBlock new_block;
        new_block.block_id                       = int(m_wipe_tower_blocks.size());
        new_block.filament_adhesiveness_category = category;
        m_wipe_tower_blocks.emplace_back(new_block);
        return &m_wipe_tower_blocks.back();
    }
    return nullptr;
}

void WipeTower::add_depth_to_block(int category, float depth, bool is_nozzle_change)
{
    std::vector<BlockDepthInfo> &layer_depth = m_all_layers_depth[m_cur_layer_id];
    auto iter = std::find_if(layer_depth.begin(), layer_depth.end(), [category](const BlockDepthInfo &item) { return item.category == category; });
    if (iter == layer_depth.end()) {
        layer_depth.emplace_back();
        iter           = layer_depth.end() - 1;
        iter->category = category;
    }
    iter->depth += depth;
    if (is_nozzle_change)
        iter->nozzle_change_depth += depth;
}

void WipeTower::reset_block_status()
{
    for (WipeTowerBlock &block : m_wipe_tower_blocks) {
        block.cur_depth               = block.start_depth;
        block.last_filament_change_id = -1;
        block.last_nozzle_change_id   = -1;
    }
}

size_t WipeTower::layer_start_tool(const WipeTowerInfo &layer, size_t carried)
{
    if (layer.local_z_start_tool >= 0)
        return size_t(layer.local_z_start_tool);
    return layer.tool_changes.empty() ? carried : layer.tool_changes.front().old_tool;
}

size_t WipeTower::layer_end_tool(const WipeTowerInfo &layer, size_t carried)
{
    return layer.tool_changes.empty() ? layer_start_tool(layer, carried) : layer.tool_changes.back().new_tool;
}

bool WipeTower::is_valid_last_layer(size_t tool, int layer_id, double layer_z) const
{
    if (!m_nozzle_group)
        return true;
    const int extruder = m_nozzle_group->get_extruder_id(int(tool), layer_id);
    if (extruder < 0 || extruder >= int(m_printable_height.size()) || extruder >= int(m_last_layer_id.size()))
        return true;
    // Edge: extruder_printable_height defaults to 0 here (Bambu Studio leaves it empty): no limit.
    const double height = m_printable_height[extruder];
    if (!(height > 0.))
        return true;
    return !(m_last_layer_id[extruder] == layer_id && layer_z > height);
}

void WipeTower::set_nozzle_last_layer_id()
{
    std::fill(m_last_layer_id.begin(), m_last_layer_id.end(), -1);
    if (!m_nozzle_group)
        return;
    for (size_t idx = 0; idx < m_plan.size(); ++idx)
        for (const WipeTowerInfo::ToolChange &tc : m_plan[idx].tool_changes)
            for (size_t tool : { tc.old_tool, tc.new_tool })
                if (int ex = m_nozzle_group->get_extruder_id(int(tool), int(idx)); ex >= 0 && ex < int(m_last_layer_id.size()))
                    m_last_layer_id[ex] = int(idx);
}

// Orca #10780 / #14685 (Bambu Studio calc_block_infill_gap()): the purge line gap of each block (prime_tower_infill_gap),
// and that of its ramming lines. A block that rams uses one gap for both once the ramming gap gets wide, so its
// purge and ramming rows stay on one grid from layer to layer; a reverse travel after the ramming needs room
// (m_tpu_fixed_spacing). Then every change is re-planned at its block's gaps.
void WipeTower::calc_block_infill_gap()
{
    struct BlockInfo
    {
        bool  has_ramming        = false;
        bool  has_reverse_travel = false;
        float depth              = 0.f;
    };
    auto reverse_travel = [this](size_t tool, bool extruder_change) {
        const FilamentParameters &fp = m_filpar[tool];
        return extruder_change ? fp.ramming_travel_time.first > EPSILON && fp.change_length.first > EPSILON :
                                 fp.ramming_travel_time.second > EPSILON && fp.change_length.second > EPSILON;
    };
    std::map<int, BlockInfo> block_info;
    std::map<int, BlockInfo> high_block_info;
    for (int i = int(m_plan.size()) - 1; i >= 0; --i) {
        for (const WipeTowerInfo::ToolChange &tc : m_plan[i].tool_changes) {
            if (tc.nozzle_change.need_ramming) {
                BlockInfo &old_info  = block_info[get_filament_category(tc.old_tool)];
                old_info.has_ramming = true;
                if (reverse_travel(tc.old_tool, tc.nozzle_change.extruder_change))
                    old_info.has_reverse_travel = true;
                old_info.depth += tc.nozzle_change_depth;
            }
            block_info[get_filament_category(tc.new_tool)].depth += tc.required_depth - tc.nozzle_change_depth;
        }
        for (auto &block : block_info) {
            auto high = high_block_info.find(block.first);
            if (high != high_block_info.end() && high->second.depth > block.second.depth)
                block.second.depth = high->second.depth;
        }
        high_block_info = block_info;
        for (auto &block : block_info)
            block.second.depth = 0.f;
        if (i == 0)
            block_info = high_block_info;
    }
    const float max_depth         = std::accumulate(block_info.begin(), block_info.end(), 0.f,
                                                    [](float value, const std::pair<const int, BlockInfo> &block) { return value + block.second.depth; });
    const float height_to_depth   = get_limit_depth_by_height(m_wipe_tower_height);
    const float height_to_spacing = (max_depth > height_to_depth || max_depth < EPSILON) ? 1.f : height_to_depth / max_depth;

    const float ncpw         = m_nozzle_change_perimeter_width;
    const float extra_width  = (m_extra_spacing - 1.f) * m_perimeter_width;
    const float line_gap_tol = 2.f * ncpw; // a ramming gap past this aligns the block's purge rows to it
    m_block_infill_gap_width.clear();
    for (const auto &info : block_info) {
        std::pair<float, float> &gap = m_block_infill_gap_width[info.first];
        if (!info.second.has_ramming) {
            // No ramming: it always aligns.
            gap.first = gap.second = extra_width + m_perimeter_width;
        } else if (!info.second.has_reverse_travel) {
            float line_gap = ncpw + extra_width;
            if (!m_use_rib_wall)
                line_gap *= height_to_spacing;
            if (line_gap < line_gap_tol) {
                gap.first  = m_perimeter_width + extra_width;
                gap.second = ncpw + extra_width;
            } else
                gap.first = gap.second = ncpw + extra_width;
        } else {
            const float reverse_gap = ncpw + std::max((m_tpu_fixed_spacing - 1.f) * m_perimeter_width, extra_width);
            float       line_gap    = reverse_gap;
            if (!m_use_rib_wall)
                line_gap *= height_to_spacing;
            if (line_gap < line_gap_tol) {
                gap.first  = m_perimeter_width + extra_width;
                gap.second = reverse_gap;
            } else
                gap.first = gap.second = reverse_gap;
        }
    }

    for (WipeTowerInfo &info : m_plan)
        for (WipeTowerInfo::ToolChange &tc : info.tool_changes)
            replan_toolchange(tc, info.height);
    m_extra_spacing = 1.f;
}

// Orca #10780 (Bambu Studio generate_wipe_tower_blocks()): one block per adhesiveness category, as deep as its
// deepest layer from there up; a filament's purge goes to its own block, the outgoing filament's ramming to its.
// With add_solid_flag each block's layers get their type: Contact where the category starts or stops being purged
// on the tower, Contact_UP on the layer above, Solid on the layers below a change.
void WipeTower::generate_wipe_tower_blocks(bool add_solid_flag)
{
    // 1. every layer's depth per category
    m_all_layers_depth.clear();
    m_all_layers_depth.resize(m_plan.size());
    m_cur_layer_id = 0;
    for (const WipeTowerInfo &info : m_plan) {
        for (const WipeTowerInfo::ToolChange &tool_change : info.tool_changes) {
            if (!tool_change.nozzle_change.need_ramming) {
                add_depth_to_block(get_filament_category(tool_change.new_tool), tool_change.required_depth);
            } else {
                add_depth_to_block(get_filament_category(tool_change.old_tool), tool_change.nozzle_change_depth, true);
                add_depth_to_block(get_filament_category(tool_change.new_tool), tool_change.required_depth - tool_change.nozzle_change_depth);
            }
        }
        ++m_cur_layer_id;
    }
    auto depth_of = [this](size_t layer_id, int category) {
        for (const BlockDepthInfo &info : m_all_layers_depth[layer_id])
            if (info.category == category)
                return info.depth;
        return 0.f;
    };

    // 2. the blocks, in the order their categories first appear (Edge: Orca walks an unordered_map here,
    // whose order differs between compilers).
    m_wipe_tower_blocks.clear();
    m_cur_block = nullptr;
    for (size_t layer_id = 0; layer_id < m_all_layers_depth.size(); ++layer_id)
        for (const BlockDepthInfo &info : m_all_layers_depth[layer_id]) {
            WipeTowerBlock *block = get_block_by_category(info.category, true);
            if (block->layer_depths.empty()) {
                block->layer_depths.resize(m_plan.size(), 0.f);
                block->finish_depth.resize(m_plan.size(), 0.f);
                block->layers_type.resize(m_plan.size(), WipeTowerLayerType::Normal);
            }
            block->depth                   = std::max(block->depth, info.depth);
            block->layer_depths[layer_id]  = info.depth;
        }

    // 3. a block is as deep on a layer as on any layer above it
    for (int layer_id = int(m_plan.size()) - 1; layer_id >= 0; --layer_id) {
        m_plan[layer_id].depth = 0.f;
        for (WipeTowerBlock &block : m_wipe_tower_blocks) {
            if (layer_id < int(m_plan.size()) - 1)
                block.layer_depths[layer_id] = std::max(block.layer_depths[layer_id], block.layer_depths[layer_id + 1]);
            m_plan[layer_id].depth += block.layer_depths[layer_id];
        }
    }

    // prime_tower_enable_framework: every layer keeps the first layer's blocks (internal ribs).
    if (m_tower_framework) {
        for (size_t layer_id = 1; layer_id < m_plan.size(); ++layer_id) {
            m_plan[layer_id].depth = 0.f;
            for (WipeTowerBlock &block : m_wipe_tower_blocks) {
                block.layer_depths[layer_id] = block.layer_depths[0];
                m_plan[layer_id].depth += block.layer_depths[layer_id];
            }
        }
    }

    if (!add_solid_flag)
        return;

    // 4. layer types. The categories each layer purges on the tower: a layer without tool changes is printed by
    // the filament loaded on it (Edge, #937: the one its Local-Z passes left loaded).
    const int                            solid_infill_layer_low = 4;
    std::vector<std::unordered_set<int>> layers_used_categories;
    size_t                               loaded = m_current_tool;
    for (const WipeTowerInfo &layer : m_plan)
        if (!layer.tool_changes.empty() || layer.local_z_start_tool >= 0) {
            loaded = layer.local_z_start_tool >= 0 ? size_t(layer.local_z_prev_tool) : layer.tool_changes.front().old_tool;
            break;
        }
    for (const WipeTowerInfo &info : m_plan) {
        std::unordered_set<int> used;
        loaded = layer_start_tool(info, loaded);
        if (info.tool_changes.empty())
            used.insert(get_filament_category(loaded));
        for (const WipeTowerInfo::ToolChange &tool_change : info.tool_changes) {
            used.insert(get_filament_category(tool_change.old_tool));
            used.insert(get_filament_category(tool_change.new_tool));
        }
        loaded = layer_end_tool(info, loaded);
        layers_used_categories.push_back(std::move(used));
    }

    for (WipeTowerBlock &block : m_wipe_tower_blocks) {
        const int category = block.filament_adhesiveness_category;
        for (size_t layer_id = 0; layer_id < m_plan.size(); ++layer_id) {
            const bool cur_has_category = layers_used_categories[layer_id].count(category) > 0;
            // Solid: the block is not filled by purges here and the category changes within the next layers.
            if (depth_of(layer_id, category) < block.layer_depths[layer_id] - m_perimeter_width) {
                for (int layer_count = solid_infill_layer_low; layer_count > 0; --layer_count) {
                    if (layer_id + layer_count < m_plan.size() &&
                        cur_has_category != (layers_used_categories[layer_id + layer_count].count(category) > 0)) {
                        block.layers_type[layer_id] = WipeTowerLayerType::Solid;
                        break;
                    }
                }
            }
            if (layer_id > 0) {
                if (cur_has_category != (layers_used_categories[layer_id - 1].count(category) > 0))
                    block.layers_type[layer_id] = WipeTowerLayerType::Contact;
                if (block.layers_type[layer_id - 1] == WipeTowerLayerType::Contact && block.layers_type[layer_id] != WipeTowerLayerType::Contact)
                    block.layers_type[layer_id] = WipeTowerLayerType::Contact_UP;
            }
        }
    }
}

// Orca #10780 (Bambu Studio update_all_layer_depth()): stretch the blocks for the stability minimum and stack them
// from one perimeter width in; a smooth timelapse tower keeps its full depth on every layer.
void WipeTower::update_all_layer_depth(float idle_depth)
{
    m_wipe_tower_depth       = 0.f;
    const float start_offset = m_perimeter_width;
    float       start_depth  = start_offset;
    for (WipeTowerBlock &block : m_wipe_tower_blocks) {
        block.depth *= m_extra_spacing;
        block.start_depth = start_depth;
        start_depth += block.depth;
        m_wipe_tower_depth += block.depth;
        for (float &layer_depth : block.layer_depths)
            layer_depth *= m_extra_spacing;
    }
    // Edge: Orca stretches every layer's depth once per block; the layer depth is the sum of its blocks', stretched once.
    for (WipeTowerInfo &plan_info : m_plan)
        plan_info.depth *= m_extra_spacing;
    if (m_wipe_tower_depth > 0.f)
        m_wipe_tower_depth += start_offset;

    if (m_enable_timelapse_print) {
        if (is_approx(m_wipe_tower_depth, 0.f))
            m_wipe_tower_depth = idle_depth;
        for (WipeTowerInfo &plan_info : m_plan)
            plan_info.depth = m_wipe_tower_depth;
    }
}

// Orca #10780 / #15035 (Bambu Studio plan_tower_new()): the block gaps, a rib tower squared, the blocks, the stretch to
// the stability minimum (a rib tower lengthens its ribs instead), the first layer printed solid, the wall gaps.
void WipeTower::plan_tower_new()
{
    // Bambu Studio's "auto" brim (prime_tower_brim_width -1), from the tower's real height.
    if (m_wipe_tower_brim_width < 0.f)
        m_wipe_tower_brim_width = get_auto_brim_by_height(m_wipe_tower_height);

    calc_block_infill_gap();
    if (m_use_rib_wall) {
        // A rib tower is square, with the purge area it would have had at the configured width.
        generate_wipe_tower_blocks(false);
        float max_depth = m_perimeter_width;
        for (const WipeTowerBlock &block : m_wipe_tower_blocks)
            max_depth += block.depth;
        const float square_width = align_ceil(std::sqrt(max_depth * m_wipe_tower_width * m_extra_spacing), m_perimeter_width);
        if (! m_wipe_tower_blocks.empty() && square_width - 2 * m_perimeter_width > EPSILON) {
            m_wipe_tower_width = square_width;
            for (WipeTowerInfo &info : m_plan)
                for (WipeTowerInfo::ToolChange &tc : info.tool_changes)
                    replan_toolchange(tc, info.height);
        }
    }

    generate_wipe_tower_blocks(true);

    float max_depth = 0.f;
    for (const WipeTowerBlock &block : m_wipe_tower_blocks)
        max_depth += block.depth;
    const float min_wipe_tower_depth = get_limit_depth_by_height(m_wipe_tower_height);

    if (m_enable_timelapse_print && max_depth < EPSILON) {
        max_depth = min_wipe_tower_depth;
        // Bambu Studio: an idle rib tower is a square of the minimum depth.
        if (m_use_rib_wall)
            m_wipe_tower_width = max_depth;
    }
    if (max_depth > EPSILON && max_depth + EPSILON < min_wipe_tower_depth) {
        // A rib wall reaches the stability minimum by lengthening the ribs, not by spreading the purge lines.
        if (m_use_rib_wall)
            m_rib_length = std::max(m_rib_length, min_wipe_tower_depth * float(std::sqrt(2.)));
        else
            m_extra_spacing = std::max(min_wipe_tower_depth / max_depth, m_extra_spacing);
    }

    for (size_t idx = 0; idx < m_plan.size(); ++idx) {
        WipeTowerInfo &info = m_plan[idx];
        if (idx == 0) {
            // The first layer is printed solid: its purge and ramming lines fill the depth reserved for them.
            info.extra_spacing = 1.f;
            for (WipeTowerInfo::ToolChange &tc : info.tool_changes) {
                const float new_pw          = tool_line_width(tc.new_tool);
                const float nc_width        = nozzle_change_width(tc.old_tool);
                const float line_len        = m_wipe_tower_width - 2 * m_perimeter_width;
                const float wipe_depth      = (tc.required_depth - tc.nozzle_change_depth) * m_extra_spacing;
                const float wipe_line_count = wipe_depth / new_pw;
                const float nc_depth        = tc.nozzle_change_depth * m_extra_spacing;
                const int   nc_line_count   = int((nc_depth + WT_EPSILON) / nc_width);
                tc.required_depth       = wipe_depth + nc_depth;
                tc.wipe_length          = wipe_line_count * line_len;
                tc.wipe_volume          = length_to_volume(tc.wipe_length, new_pw, info.height);
                tc.nozzle_change_length = nc_line_count * (m_wipe_tower_width - (nc_width + tool_line_width(tc.old_tool)));
                tc.nozzle_change_depth  = nc_depth;
            }
        } else {
            info.extra_spacing = m_extra_spacing;
            for (WipeTowerInfo::ToolChange &tc : info.tool_changes) {
                tc.required_depth *= m_extra_spacing;
                tc.nozzle_change_depth *= m_extra_spacing;
                tc.wipe_length = volume_to_length(tc.wipe_volume, tool_line_width(tc.new_tool), info.height);
            }
        }
    }

    update_all_layer_depth(max_depth);
    set_nozzle_last_layer_id();
    get_all_wall_skip_points();

    if (m_use_rib_wall) {
        const float diagonal = std::sqrt(m_wipe_tower_depth * m_wipe_tower_depth + m_wipe_tower_width * m_wipe_tower_width);
        m_rib_length         = std::max({ m_rib_length, diagonal });
        m_rib_length += m_extra_rib_length;
        m_rib_length = std::max(diagonal, m_rib_length);
        m_rib_width  = std::min(m_rib_width, std::min(m_wipe_tower_depth, m_wipe_tower_width) / 2.f); // Ensure that the rib wall of the wipetower are attached to the infill.
    }
}

std::vector<float> WipeTower::layer_y_shifts() const
{
    std::vector<float> shifts(m_plan.size(), 0.f);
    float              shift = 0.f;
    for (size_t i = 0; i < m_plan.size(); ++i) {
        const float depth = m_plan[i].depth;
        // A single block narrows with the layers above; centre it on the tower (aligned to the line spacing).
        if (m_wipe_tower_blocks.size() == 1 && depth >= m_perimeter_width && depth < m_wipe_tower_depth - m_perimeter_width)
            shift = align_round((m_wipe_tower_depth - depth) / 2.f, m_extra_spacing * m_perimeter_width);
        shifts[i] = shift;
    }
    return shifts;
}

// Orca #15035 (Bambu Studio get_wall_skip_points()): every tool change's purge starts on the outer wall, where the
// wall gets a gap (prime_tower_skip_points, here wipe_tower_wall_gap). Edge: also the tower interface run-ins'
// gaps, on their layer and the GAP_LAYERS - 1 below (m_interface_gaps), at the same bed position.
void WipeTower::get_all_wall_skip_points()
{
    m_wall_skip_points.assign(m_plan.size(), {});
    m_interface_gaps.assign(m_plan.size(), {});
    if (!m_use_gap_wall)
        return;
    const std::vector<float> y_shifts = layer_y_shifts();
    for (size_t i = 0; i < m_plan.size(); ++i)
        get_wall_skip_points(m_plan[i], int(i), y_shifts);
}

void WipeTower::get_wall_skip_points(WipeTowerInfo &layer, int layer_id, const std::vector<float> &y_shifts)
{
    std::unordered_map<int, float> cur_block_depth;
    for (WipeTowerInfo::ToolChange &tc : layer.tool_changes) {
        const size_t old_filament        = tc.old_tool;
        const size_t new_filament        = tc.new_tool;
        float        nozzle_change_depth = tc.nozzle_change_depth;
        const float  wipe_depth          = tc.required_depth - nozzle_change_depth;
        if (!is_valid_last_layer(old_filament, layer_id, layer.z))
            nozzle_change_depth = 0.f;
        const int       new_category = get_filament_category(new_filament);
        const int       old_category = get_filament_category(old_filament);
        WipeTowerBlock *block        = get_block_by_category(new_category, false);
        if (block == nullptr)
            continue;
        if (!cur_block_depth.count(new_category))
            cur_block_depth[new_category] = block->start_depth;
        float process_depth = cur_block_depth[new_category];
        if (tc.nozzle_change.need_ramming) {
            if (new_category == old_category)
                process_depth += nozzle_change_depth;
            else {
                if (!cur_block_depth.count(old_category)) {
                    const WipeTowerBlock *old_block = get_block_by_category(old_category, false);
                    if (old_block == nullptr)
                        continue;
                    cur_block_depth[old_category] = old_block->start_depth;
                }
                cur_block_depth[old_category] += nozzle_change_depth;
            }
        }

        const float infill_gap_width = get_block_gap_width(new_filament, false);
        Vec2f       res;
        switch (layer_id % 4) {
        case 0: res = Vec2f(0.f, process_depth); break;
        case 1: res = Vec2f(m_wipe_tower_width, process_depth + wipe_depth - layer.extra_spacing * infill_gap_width); break;
        case 2: res = Vec2f(m_wipe_tower_width, process_depth); break;
        default: res = Vec2f(0.f, process_depth + wipe_depth - layer.extra_spacing * infill_gap_width); break;
        }
        tc.start_point = res;
        m_wall_skip_points[layer_id].emplace_back(res);
        cur_block_depth[new_category] = process_depth + wipe_depth;

        if (tc.run_in) {
            const bool is_left = res.x() < m_wipe_tower_width / 2.f;
            for (int below = 0; below < TowerInterface::GAP_LAYERS && below <= layer_id; ++below) {
                const int lower = layer_id - below;
                m_interface_gaps[lower].push_back({ Vec2f(res.x(), res.y() + y_shifts[layer_id] - y_shifts[lower]), is_left });
            }
        }
    }
}

void WipeTower::set_rib_offset()
{
    // Bambu Studio measures the first layer's wall as it extrudes it (generate_support_wall_new()) and GCode.cpp
    // shifts every tower move by the result. Here it is measured up front so the writer can emit the shifted
    // coordinates directly: same state as generate() sets for the first printed layer.
    m_rib_offset    = Vec2f::Zero();
    m_rib_footprint = Vec2f::Zero();
    m_first_layer_wall.clear();
    auto first = std::find_if(m_plan.begin(), m_plan.end(), [this](const WipeTowerInfo &info) { return info.depth >= m_perimeter_width; });
    if (first == m_plan.end())
        return;
    const float saved_z = m_z_pos, saved_y_shift = m_y_shift;
    m_z_pos   = first->z;
    m_y_shift = layer_y_shifts()[size_t(first - m_plan.begin())];
    const float     wall_depth = m_wipe_tower_blocks.size() == 1 ? first->depth + m_perimeter_width : m_wipe_tower_depth;
    box_coordinates wt_box(Vec2f(0.f, 0.f), m_wipe_tower_width, wall_depth);
    wt_box       = align_perimeter(wt_box);
    Polygon wall = rib_wall_polygon(wt_box);
    wall.translate(0, scaled(m_y_shift));
    const BoundingBox bbox = get_extents(wall);
    m_rib_offset    = Vec2f(-unscaled<float>(bbox.min.x()), -unscaled<float>(bbox.min.y()));
    m_rib_footprint = unscaled<float>(bbox.size());
    wall.translate(-bbox.min.x(), -bbox.min.y());
    m_first_layer_wall = std::move(wall);
    m_z_pos   = saved_z;
    m_y_shift = saved_y_shift;
}

// Orca #10780 / #16717 (Bambu Studio merge_tcr()): one tower visit made of two parts; the travel between them, unless
// the second is a tool change GCode travels to itself (tool_change_start_pos).
WipeTower::ToolChangeResult WipeTower::merge_tcr(ToolChangeResult &first, ToolChangeResult &second) const
{
    assert(first.new_tool == second.initial_tool);
    ToolChangeResult out = first;
    if ((first.end_pos - second.start_pos).norm() > float(EPSILON)) {
        const bool travel_in_gcode = second.is_tool_change && is_approx(second.start_pos.x(), second.tool_change_start_pos.x()) &&
                                     is_approx(second.start_pos.y(), second.tool_change_start_pos.y());
        if (!travel_in_gcode)
            out.gcode += "G1 X" + Slic3r::float_to_string_decimal_point(second.start_pos.x(), 3) + " Y" +
                         Slic3r::float_to_string_decimal_point(second.start_pos.y(), 3) + " F" +
                         Slic3r::float_to_string_decimal_point(m_max_speed, 6) + "\n";
    }
    out.gcode += second.gcode;
    out.extrusions.insert(out.extrusions.end(), second.extrusions.begin(), second.extrusions.end());
    out.end_pos      = second.end_pos;
    out.wipe_path    = second.wipe_path;
    out.initial_tool = first.initial_tool;
    out.new_tool     = second.new_tool;
    out.is_contact   = first.is_contact || second.is_contact;
    // Bambu Studio merge_tcr: the nozzle change travels with whichever part carries one.
    if (first.nozzle_change_result.has_action())
        out.nozzle_change_result = first.nozzle_change_result;
    else if (second.nozzle_change_result.has_action())
        out.nozzle_change_result = second.nozzle_change_result;
    // The tool change part decides where its purge starts and whether it reheats.
    if (first.is_tool_change) {
        out.is_tool_change        = true;
        out.tool_change_start_pos = first.tool_change_start_pos;
    } else if (second.is_tool_change) {
        out.is_tool_change        = true;
        out.tool_change_start_pos = second.tool_change_start_pos;
        out.reheats_after_cooling = second.reheats_after_cooling;
    } else
        out.is_tool_change = false;

    // BBS
    out.purge_volume += second.purge_volume;
    return out;
}

// Orca #10780 (Bambu Studio get_wall_filament_for_all_layer()): the filament that prints the wall where it can, one of
// the category used on the most layers.
int WipeTower::get_wall_filament_for_all_layer() const
{
    std::map<int, int> category_counts;
    std::map<int, int> filament_counts;
    size_t             current_tool = m_current_tool;
    for (const WipeTowerInfo &layer : m_plan) {
        current_tool = layer_start_tool(layer, current_tool);
        if (layer.tool_changes.empty()) {
            filament_counts[int(current_tool)]++;
            category_counts[get_filament_category(current_tool)]++;
            continue;
        }
        std::unordered_set<int> used_tools;
        std::unordered_set<int> used_category;
        for (size_t i = 0; i < layer.tool_changes.size(); ++i) {
            const WipeTowerInfo::ToolChange &tc = layer.tool_changes[i];
            if (i == 0) {
                filament_counts[int(tc.old_tool)]++;
                category_counts[get_filament_category(tc.old_tool)]++;
                used_tools.insert(int(tc.old_tool));
                used_category.insert(get_filament_category(tc.old_tool));
            }
            if (!used_category.count(get_filament_category(tc.new_tool)))
                category_counts[get_filament_category(tc.new_tool)]++;
            if (!used_tools.count(int(tc.new_tool)))
                filament_counts[int(tc.new_tool)]++;
            used_tools.insert(int(tc.new_tool));
            used_category.insert(get_filament_category(tc.new_tool));
        }
        current_tool = layer.tool_changes.back().new_tool;
    }

    int selected_category = -1;
    int selected_count    = 0;
    for (const auto &[category, count] : category_counts)
        if (count > selected_count) {
            selected_category = category;
            selected_count    = count;
        }
    int filament_id    = -1;
    int filament_count = 0;
    for (const auto &[filament, count] : filament_counts)
        if (get_filament_category(size_t(filament)) == selected_category && count > filament_count) {
            filament_id    = filament;
            filament_count = count;
        }
    return filament_id;
}

// Orca #10780 / #15035 (Bambu Studio generate_new()): the block planner.
// Processes vector m_plan and calls respective functions to generate G-code for the wipe tower
// Resulting ToolChangeResults are appended into vector "result"
void WipeTower::generate(std::vector<std::vector<WipeTower::ToolChangeResult>> &result)
{
	if (m_plan.empty())
        return;

    // Orca #15841: before planning, since the layer heights this rewrites feed the extrusion flow of
    // every later pass. A layer with no toolchange prints with the filament the layer below left loaded.
    if (m_sparse_layers_combined) {
        size_t start_tool = m_current_tool;
        for (const WipeTowerInfo &layer : m_plan) {
            if (layer.local_z_start_tool >= 0) {
                start_tool = size_t(layer.local_z_prev_tool);
                break;
            }
            if (! layer.tool_changes.empty()) {
                start_tool = layer.tool_changes.front().old_tool;
                break;
            }
        }
        combine_sparse_wipe_tower_plan(
            m_plan, m_filpar, m_first_layer_idx, start_tool, [](const WipeTowerInfo &layer) { return layer.tool_changes.empty(); },
            [](const WipeTowerInfo &layer, size_t tool) {
                if (! layer.tool_changes.empty())
                    return layer.tool_changes.back().new_tool;
                return layer.local_z_start_tool >= 0 ? size_t(layer.local_z_start_tool) : tool;
            });
    }

    // we don't know which extruder to start with - we'll set it according to the first toolchange
    for (const auto& layer : m_plan) {
        // A layer whose Local-Z passes changed filament first started with the one loaded before them.
        if (layer.local_z_start_tool >= 0) {
            m_current_tool = size_t(layer.local_z_prev_tool);
            break;
        }
        if (!layer.tool_changes.empty()) {
            m_current_tool = layer.tool_changes.front().old_tool;
            break;
        }
    }

    // Orca #10780: the real tower height (stability minimum, auto brim, rib taper).
    m_wipe_tower_height = m_plan.back().z;
    mark_interfaces();
	plan_tower_new();
    m_outer_wall.clear();
    if (m_use_rib_wall)
        set_rib_offset();

    m_layer_info = m_plan.begin();

    for (auto& used : m_used_filament_length) // reset used filament stats
        used = 0.f;

    m_old_temperature = -1; // reset last temperature written in the gcode

    const int                wall_filament      = get_wall_filament_for_all_layer();
    const std::vector<float> y_shifts           = layer_y_shifts();
    const bool               only_generate_wall = m_enable_timelapse_print;

    int index = 0;
	for (auto layer : m_plan)
	{
        std::vector<WipeTower::ToolChangeResult> layer_result;
        reset_block_status();
        m_cur_layer_id = index++;
        // The layer's Local-Z passes left another filament loaded than the tower's last change: its
        // first change starts from that one (initial_tool, unload and material of the old filament).
        if (layer.local_z_start_tool >= 0)
            m_current_tool = size_t(layer.local_z_start_tool);
        set_layer(layer.z, layer.height, 0, false/*layer.z == m_plan.front().z*/, layer.z == m_plan.back().z);

        if (m_layer_info->depth < m_perimeter_width)
            continue;
        m_y_shift = y_shifts[m_cur_layer_id];

        // The filament that prints this layer's wall: the tower's wall filament when the layer has it, else one of
        // its category, else the first change's new filament. -1: no tool change on the layer.
        auto get_wall_filament_for_this_layer = [this, &layer, wall_filament]() -> int {
            if (layer.tool_changes.empty())
                return -1;
            if (wall_filament < 0)
                return int(layer.tool_changes[0].new_tool);
            int candidate_id = -1;
            for (size_t idx = 0; idx < layer.tool_changes.size(); ++idx) {
                const WipeTowerInfo::ToolChange &tc = layer.tool_changes[idx];
                if (idx == 0) {
                    if (int(tc.old_tool) == wall_filament && is_valid_last_layer(tc.old_tool, int(m_cur_layer_id), layer.z))
                        return wall_filament;
                    else if (get_filament_category(tc.old_tool) == get_filament_category(size_t(wall_filament)) &&
                             is_valid_last_layer(tc.old_tool, int(m_cur_layer_id), layer.z))
                        candidate_id = int(tc.old_tool);
                }
                if (int(tc.new_tool) == wall_filament)
                    return wall_filament;
                if (candidate_id == -1 && get_filament_category(tc.new_tool) == get_filament_category(size_t(wall_filament)))
                    candidate_id = int(tc.new_tool);
            }
            return candidate_id == -1 ? int(layer.tool_changes[0].new_tool) : candidate_id;
        };
        int wall_idx = get_wall_filament_for_this_layer();

        ToolChangeResult finish_layer_tcr;
        ToolChangeResult timelapse_wall;
        if (wall_idx == -1) {
            // No tool change: one filament prints the whole layer, solid where a block needs it.
            const bool need_insert_solid_infill = std::any_of(m_wipe_tower_blocks.begin(), m_wipe_tower_blocks.end(),
                                                              [this](const WipeTowerBlock &block) { return block.layers_type[m_cur_layer_id] != WipeTowerLayerType::Normal; });
            if (need_insert_solid_infill) {
                wall_idx = int(m_current_tool);
            } else {
                if (only_generate_wall)
                    timelapse_wall = only_generate_out_wall();
                finish_layer_tcr = finish_layer_new(!only_generate_wall, layer.extruder_fill);
                for (WipeTowerBlock &block : m_wipe_tower_blocks)
                    block.finish_depth[m_cur_layer_id] = block.start_depth;
            }
        }

        // The tool changes, the wall printed by the wall filament.
        int insert_finish_layer_idx = -1;
        if (wall_idx != -1 && only_generate_wall)
            timelapse_wall = only_generate_out_wall();
        for (int i = 0; i < int(layer.tool_changes.size()); ++i) {
            const WipeTowerInfo::ToolChange &tc            = layer.tool_changes[i];
            const bool                       wall_by_first = i == 0 && int(tc.old_tool) == wall_idx;
            if (wall_by_first)
                finish_layer_tcr = finish_layer_new(!only_generate_wall, false, false);
            bool solid_toolchange = false, solid_nozzlechange = false;
            if (const WipeTowerBlock *block = get_block_by_category(get_filament_category(tc.new_tool), false))
                solid_toolchange = block->layers_type[m_cur_layer_id] == WipeTowerLayerType::Contact;
            if (const WipeTowerBlock *block = get_block_by_category(get_filament_category(tc.old_tool), false))
                solid_nozzlechange = block->layers_type[m_cur_layer_id] == WipeTowerLayerType::Contact;
            layer_result.emplace_back(tool_change_new(tc.new_tool, solid_toolchange, solid_nozzlechange));
            if (!wall_by_first && int(tc.new_tool) == wall_idx) {
                finish_layer_tcr        = finish_layer_new(!only_generate_wall, false, false);
                insert_finish_layer_idx = i;
            }
        }

        // The blocks the tool changes left unfilled, each with a filament of its own category where the layer has
        // one: printed right after that filament's purge, or before its nozzle change.
        if (wall_idx != -1) {
            if (layer.tool_changes.empty())
                finish_layer_tcr = finish_layer_new(!only_generate_wall, false, false);
            for (WipeTowerBlock &block : m_wipe_tower_blocks) {
                block.finish_depth[m_cur_layer_id] = block.start_depth + block.depth;
                if (block.cur_depth + EPSILON >= block.start_depth + block.layer_depths[m_cur_layer_id] - m_perimeter_width)
                    continue;
                const WipeTowerLayerType layer_type  = block.layers_type[m_cur_layer_id];
                const bool               block_solid = layer_type != WipeTowerLayerType::Normal;
                int                      finish_layer_filament = -1;
                if (block.last_filament_change_id != -1)
                    finish_layer_filament = block.last_filament_change_id;
                else if (block.last_nozzle_change_id != -1)
                    finish_layer_filament = block.last_nozzle_change_id;
                if (!layer.tool_changes.empty()) {
                    const WipeTowerBlock *start_block = get_block_by_category(get_filament_category(layer.tool_changes.front().old_tool), false);
                    if (start_block != nullptr && start_block->block_id == block.block_id && finish_layer_filament == -1)
                        finish_layer_filament = int(layer.tool_changes.front().old_tool);
                }
                if (finish_layer_filament == -1)
                    finish_layer_filament = wall_idx;
                // Cancel the block on an extruder's last layer above its printable height.
                if (!is_valid_last_layer(size_t(finish_layer_filament), int(m_cur_layer_id), layer.z))
                    continue;
                ToolChangeResult finish_block_tcr;
                if (block_solid) {
                    finish_block_tcr                   = finish_block_solid(block, size_t(finish_layer_filament), layer_type);
                    block.finish_depth[m_cur_layer_id] = block.start_depth + block.depth;
                } else {
                    finish_block_tcr                   = finish_block(block, size_t(finish_layer_filament), layer.extruder_fill);
                    block.finish_depth[m_cur_layer_id] = block.cur_depth;
                }

                bool has_inserted = false;
                auto fc_iter      = std::find_if(layer_result.begin(), layer_result.end(),
                                                 [finish_layer_filament](const ToolChangeResult &item) { return item.new_tool == finish_layer_filament; });
                if (fc_iter != layer_result.end()) {
                    *fc_iter     = merge_tcr(*fc_iter, finish_block_tcr);
                    has_inserted = true;
                }
                if (block.last_filament_change_id == -1 && !has_inserted) {
                    auto nc_iter = std::find_if(layer_result.begin(), layer_result.end(),
                                                [finish_layer_filament](const ToolChangeResult &item) { return item.initial_tool == finish_layer_filament; });
                    if (nc_iter != layer_result.end()) {
                        *nc_iter     = merge_tcr(finish_block_tcr, *nc_iter);
                        has_inserted = true;
                    }
                }
                if (!has_inserted && !finish_block_tcr.gcode.empty())
                    finish_layer_tcr = merge_tcr(finish_layer_tcr, finish_block_tcr);
            }
        }

        if (layer_result.empty()) {
            // there is nothing to merge finish_layer with
            layer_result.emplace_back(std::move(finish_layer_tcr));
        } else if (is_valid_gcode(finish_layer_tcr.gcode)) {
            if (insert_finish_layer_idx == -1)
                layer_result[0] = merge_tcr(finish_layer_tcr, layer_result[0]);
            else
                layer_result[insert_finish_layer_idx] = merge_tcr(layer_result[insert_finish_layer_idx], finish_layer_tcr);
        }

        if (only_generate_wall && !timelapse_wall.gcode.empty())
            layer_result.insert(layer_result.begin(), std::move(timelapse_wall));

        if (layer.combined_away)
            for (WipeTower::ToolChangeResult &tcr : layer_result)
                tcr.combined_away = true;

		result.emplace_back(std::move(layer_result));
	}
}

WipeTower::ToolChangeResult WipeTower::only_generate_out_wall()
{
    size_t old_tool = m_current_tool;

    WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_has_nozzle_rack, m_bbl_gcode_tags);
    writer.set_extrusion_flow(m_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(m_current_tool)
        .set_origin_offset(m_rib_offset)
        .set_arc_fitting(m_enable_arc_fitting)
        .set_accelerations(writer_tower_acceleration(), m_tower_travel_acceleration)
        .set_y_shift(m_y_shift);

    // BBS: speed up perimeter speed to 90mm/s for non-first layer (prime_tower_max_speed, 90 mm/s by default)
    float           feedrate   = wall_feedrate(m_current_tool);
    float           fill_box_y = m_layer_info->toolchanges_depth() + m_perimeter_width;
    box_coordinates fill_box(Vec2f(m_perimeter_width, fill_box_y), m_wipe_tower_width - 2 * m_perimeter_width, m_layer_info->depth - fill_box_y);

    writer.set_initial_position((m_left_to_right ? fill_box.ru : fill_box.lu), // so there is never a diagonal travel
                                m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    bool toolchanges_on_layer = m_layer_info->toolchanges_depth() > WT_EPSILON;

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");
    // outer perimeter (always): a smooth timelapse tower keeps its full depth (Orca #15035).
    const float     wipe_tower_depth = m_enable_timelapse_print ? m_wipe_tower_depth : m_layer_info->depth + m_perimeter_width;
    box_coordinates wt_box(Vec2f(0.f, 0.f), m_wipe_tower_width, wipe_tower_depth);
    wt_box = align_perimeter(wt_box);
    const Polygon wall = print_outer_wall(writer, wt_box, feedrate, true);
    if (m_use_rib_wall) {
        writer.add_wipe_path(wall, m_filpar[m_current_tool].wipe_dist);
    } else {
        // Now prepare future wipe. box contains rectangle that was extruded last (ccw).
        Vec2f target = (writer.pos() == wt_box.ld ? wt_box.rd : (writer.pos() == wt_box.rd ? wt_box.ru : (writer.pos() == wt_box.ru ? wt_box.lu : wt_box.ld)));
        writer.add_wipe_point(writer.pos()).add_wipe_point(target);
    }

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");

    // Ask our writer about how much material was consumed.
    // Skip this in case the layer is sparse and config option to not print sparse layers is enabled.
    if (layer_is_printed(toolchanges_on_layer))
        if (m_current_tool < m_used_filament_length.size()) m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    return construct_tcr(writer, false, old_tool, true, false, 0.f, false);
}

void WipeTower::mark_interfaces()
{
    m_run_in_reserve = 0.f;
    if (! m_interface.any())
        return;
    const bool run_in = m_interface.run_in && m_use_gap_wall;

    std::vector<unsigned int> filaments;
    for (size_t layer_id = 0; layer_id < m_plan.size(); ++layer_id) {
        for (WipeTowerInfo::ToolChange &tc : m_plan[layer_id].tool_changes) {
            for (size_t f : { tc.old_tool, tc.new_tool })
                if (std::find(filaments.begin(), filaments.end(), (unsigned int) f) == filaments.end())
                    filaments.push_back((unsigned int) f);
            // The tower's first layer is never an interface (Bambu Studio: "first layer never be contact").
            tc.is_interface = layer_id != m_first_layer_idx &&
                              TowerInterface::triggers(m_interface.trigger, m_filpar[tc.old_tool].kind, m_filpar[tc.new_tool].kind);
            tc.run_in       = tc.is_interface && run_in && m_filpar[tc.new_tool].run_in_distance > EPSILON;
        }
    }
    if (run_in) {
        std::vector<TowerInterface::FilamentKind> kinds;
        std::vector<float>                        distances;
        for (const FilamentParameters &fp : m_filpar) {
            kinds.push_back(fp.kind);
            distances.push_back(fp.run_in_distance);
        }
        m_run_in_reserve = float(TowerInterface::run_in_reserve(m_interface, kinds, distances, filaments, m_perimeter_width));
    }
}

Polygon WipeTower::print_outer_wall(WipeTowerWriter &writer, const box_coordinates &wt_box, float feedrate, bool extrude)
{
    const Polygon wall = m_use_rib_wall ? rib_wall_polygon(wt_box) : generate_rectange_polygon(wt_box.ld, wt_box.ru);
    if (!extrude)
        return wall;
    // The wall gaps: the tool changes' start points (Orca #15035 skip points) and the interface run-ins.
    const size_t                          layer_id = size_t(m_layer_info - m_plan.begin());
    std::vector<TowerInterface::GapPoint> gaps;
    if (layer_id < m_wall_skip_points.size())
        for (const Vec2f &p : m_wall_skip_points[layer_id])
            gaps.push_back({ p, p.x() < m_wipe_tower_width / 2.f });
    if (layer_id < m_interface_gaps.size())
        for (const TowerInterface::GapPoint &g : m_interface_gaps[layer_id])
            if (std::none_of(gaps.begin(), gaps.end(), [&g](const TowerInterface::GapPoint &p) { return (p.pos - g.pos).squaredNorm() < 1e-6f; }))
                gaps.push_back(g);
    if (gaps.empty()) {
        if (m_use_rib_wall) {
            writer.polygon(wall, feedrate, false);
            record_outer_wall(wall);
        } else
            writer.rectangle(wt_box, feedrate);
    } else {
        Polygon inserted;
        writer.gap_wall(TowerInterface::cut_wall_gaps(wall, gaps, 2.5f * m_perimeter_width, inserted), feedrate,
                        m_filpar[m_current_tool].retract_length, m_filpar[m_current_tool].retract_speed * 60.f, m_use_rib_wall);
        if (m_use_rib_wall)
            record_outer_wall(wall);
    }
    return wall;
}

void WipeTower::interface_before_wipe(WipeTowerWriter &writer, const WipeTowerInfo::ToolChange &tool_change)
{
    const FilamentParameters &fp = m_filpar[tool_change.new_tool];
    writer.append("; tower interface\n");
    if (m_interface.temp && fp.interface_temperature != fp.nozzle_temperature)
        writer.append(GCodeWriter::set_temperature(fp.interface_temperature, m_gcode_flavor, true, fp.physical_extruder, "tower interface temperature"));

    const Vec2f start  = writer.pos();
    bool        primed = false;
    if (tool_change.run_in) {
        // Out through the wall gap at the purge's start, then run in along the purge's first line: nothing is ever
        // dragged over the wall. The purge starts on the left or the right side, as the layer number picks.
        const Vec2f cross  = tool_change.start_point;
        const bool  left   = cross.x() < m_wipe_tower_width / 2.f;
        const Vec2f dir(left ? -1.f : 1.f, 0.f);
        const float alpha  = m_wipe_tower_rotation_angle * float(M_PI / 180.);
        const Vec2f shift  = Vec2f(0.f, m_y_shift) + m_rib_offset; // what the writer adds (no internal rotation here)
        auto        to_bed = [alpha, shift, this](const Vec2f &p) -> Vec2f {
            return Vec2f(Eigen::Rotation2Df(alpha) * (p + shift)) + m_wipe_tower_pos;
        };
        const float dist = TowerInterface::clamp_to_bed(cross, dir, fp.run_in_distance, to_bed, m_shared_bed);
        if (dist > EPSILON) {
            const Vec2f inside(left ? m_perimeter_width : m_wipe_tower_width - m_perimeter_width, cross.y());
            const Vec2f outside = cross + dir * dist;
            // The purge starts its first line at a third of the purge speed.
            const float speed = 0.33f * m_max_speed;
            writer.append("; tower interface run-in\n").travel(inside, m_travel_speed * 60.f).travel(outside);
            if (m_interface.extra_prime) {
                writer.prime(fp.extra_prime_length + TowerInterface::EXTRA_PRIME_BASE, TowerInterface::PRIME_FEEDRATE);
                primed = true;
            }
            writer.extrude(inside, speed).extrude(start, speed);
        }
    }
    if (m_interface.extra_prime && ! primed)
        writer.prime(fp.extra_prime_length + TowerInterface::EXTRA_PRIME_BASE, TowerInterface::PRIME_FEEDRATE);
}

// Orca #15841: a folded layer is generated like any other but thrown away by the emitter, so its
// extrusions must not be charged to the filament used.
bool WipeTower::layer_is_printed(bool toolchanges_on_layer) const
{
    if (m_layer_info != m_plan.end() && m_layer_info->combined_away)
        return false;
    return ! m_sparse_layers_skipped || toolchanges_on_layer;
}


void WipeTower::interface_after_wipe(WipeTowerWriter &writer, const WipeTowerInfo::ToolChange &tool_change)
{
    const FilamentParameters &fp = m_filpar[tool_change.new_tool];
    // Back to the normal temperature; wait only if that means heating up.
    if (m_interface.temp && fp.interface_temperature != fp.nozzle_temperature)
        writer.append(GCodeWriter::set_temperature(fp.nozzle_temperature, m_gcode_flavor, fp.nozzle_temperature > fp.interface_temperature,
                                                   fp.physical_extruder, "tower interface done, normal temperature"));
}

// Orca #10780 (Bambu Studio get_floating_area()): the part of the current block the previous layer left sparse, which
// this layer bridges.
bool WipeTower::get_floating_area(float &start_pos_y, float &end_pos_y) const {
    if (m_layer_info == m_plan.begin() || (m_layer_info - 1) == m_plan.begin())
        return false;
    if (m_cur_block == nullptr || m_cur_layer_id < 1 || m_cur_layer_id - 1 >= m_cur_block->finish_depth.size())
        return false;
    end_pos_y   = m_cur_block->start_depth + m_cur_block->depth - m_perimeter_width;
    start_pos_y = m_cur_block->finish_depth[m_cur_layer_id - 1];
    return true;
}

bool WipeTower::need_thick_bridge_flow(float pos_y) const {
    if (m_layer_height >= 0.2f)
        return false;

    float y_min = 0., y_max = 0.;
    if (get_floating_area(y_min, y_max)) {
        return pos_y > y_min && pos_y < y_max;
    }
    return false;
}

} // namespace Slic3r
