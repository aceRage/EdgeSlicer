#ifndef WipeTower_
#define WipeTower_

#include <cmath>
#include <map>
#include <string>
#include <sstream>
#include <utility>
#include <algorithm>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "WipeTowerInterface.hpp"

#include <memory>
#include <unordered_map>

namespace Slic3r
{

class WipeTowerWriter;
class PrintConfig;
enum GCodeFlavor : unsigned char;

// Klipper acts on commands the instant it parses them, and its G4 reads only P (milliseconds),
// so the zero-second and seconds-valued dwells every other flavor uses neither synchronize nor
// pause there. Both defined in WipeTower.cpp, shared by WipeTower and WipeTower2.
const char* flush_planner_queue_command(GCodeFlavor flavor); // finish queued moves, e.g. around M104/M109
std::string wait_command(GCodeFlavor flavor, float seconds);  // pause for `seconds`


class WipeTower
{
public:
    static const std::string never_skip_tag() { return "_GCODE_WIPE_TOWER_NEVER_SKIP_TAG"; }

	// WipeTower height to minimum depth map: Bambu Studio 2.9.1's (also Orca's) for this Type 1 tower, and the
	// older two-point table WipeTower2 (Type 2) keeps so its towers stay as they were.
	static const std::map<float, float> min_depth_per_height;
	static const std::map<float, float> min_depth_per_height_type2;
    static float get_limit_depth_by_height(float max_height, bool type2 = false);
    static float get_auto_brim_by_height(float max_height);
    // Both generators lay the brim in whole loops one line spacing apart, so the printed width
    // differs from the configured one. WipeTower reports it with half a spacing of line width
    // added, WipeTower2 reports the loops alone; an estimate has to round like the generator
    // whose G-code it stands in for.
    // line_width > 0: Bambu Studio's prime_tower_line_width instead of the nozzle-derived width (Type1).
    static float estimate_brim_real_width(float brim_width, float nozzle_diameter, float first_layer_height, bool type2, float line_width = 0.f);
    // Orca #15841: the tallest layer filament idx's nozzle can lay down, the cap of the sparse layer
    // combination. max_layer_height is per nozzle, so it is read through the filament->nozzle map
    // rather than by filament id; zero means three quarters of the nozzle diameter, as in Slicing.cpp.
    static float filament_max_layer_height(const PrintConfig &config, size_t idx);
    // Depth a Type1 tower reserves once nothing but wrapping detection asks for one.
    static float get_wrapping_detection_depth();
    // Line width of the nozzle-change purge lines at this nozzle diameter.
    static float nozzle_change_perimeter_width(float nozzle_diameter);
    // Bambu Studio 2.9.1 / Orca #14800: what a Bambu Lab Type 1 tower primes after a change to `filament`:
    // filament_prime_volume, filament_prime_volume_nc after a hotend change on one extruder (H2C rack), 15 mm3 in
    // prime_volume_mode Saving.
    static float bambu_prime_volume(const PrintConfig &config, unsigned int filament, bool hotend_change);
    // One filament's share of a Type1 tower layer, as plan_tower_new() reserves it.
    struct PurgeEstimate
    {
        float prime_volume           = 0.f;   // mm3 wiped after changing to this filament
        int   category               = 0;     // filament_adhesiveness_category; one purge block per category
        float filament_change_length = 0.f;   // mm of filament rammed when it leaves its nozzle; 0 when no nozzle change is planned
        float filament_diameter      = 1.75f;
    };
    // Depth of the Type1 purge stack at the given width (also the rectangle-wall depth): each
    // purge is whole lines at the block infill gap, one block per adhesiveness category sized by
    // its worst layer, stacked behind one perimeter width.
    static float estimate_tower_blocks_depth(const std::vector<PurgeEstimate> &purges, float width, float layer_height, float nozzle_diameter, float extra_spacing, float line_width = 0.f);
    // Side of the square bounding a rib-wall tower's first layer, brim excluded: the body plus the
    // rib bulge, with the ribs extended to the height-based minimum as both generators do (type2: WipeTower2's table).
    static float rib_footprint_side(float width, float depth, float rib_width, float extra_rib_length, float max_height, bool type2 = false);
    // Type1 rib tower: plan_tower_new() squares the tower from the depth at the configured width,
    // then re-plans the depth at the squared width.
    static float estimate_rib_tower_bbox_side(const std::vector<PurgeEstimate> &purges, float width, float layer_height, float nozzle_diameter, float extra_spacing, float rib_width, float extra_rib_length, float max_height, float line_width = 0.f);
    static TriangleMesh its_make_rib_brim(const Polygon &brim, float layer_height);
    // Fillets the corners of a rib wall (Bambu Studio WipeTower.cpp:128 rounding_polygon()).
    static Polygon rounding_polygon(Polygon &polygon, double rounding = 2., double angle_tol = 30. / 180. * PI);
    // Orca #15146: translation that brings a footprint inside the printable outline, padded by offset. The
    // prime tower is validated against the real outline, so clamping against the bounding box alone would
    // leave it off a delta or hexagonal bed. box and polygons must share one scaled coordinate frame; the
    // translation comes back in millimeters.
    static Vec2f move_box_inside_polygon(const BoundingBox &box, const Polygons &polygons, coord_t offset = 0);
    // True when the outline fills its own bounding box (every rectangular bed), so a bounding-box clamp
    // already keeps a footprint on it. Callers use it to leave rectangular beds on their historic clamp.
    static bool outline_fills_bounding_box(const Polygons &polygons);

    struct Extrusion
    {
		Extrusion(const Vec2f &pos, float width, unsigned int tool) : pos(pos), width(width), tool(tool) {}
		// End position of this extrusion.
		Vec2f				pos;
		// Width of a squished extrusion, corrected for the roundings of the squished extrusions.
		// This is left zero if it is a travel move.
		float 			width;
		// Current extruder index.
		unsigned int    tool;
	};

    // Bambu Studio 2.9.1 (WipeTower.hpp NozzleChangeResult): what the outgoing filament does on the tower
    // when the change moves to another nozzle - the ramming lines (ramming()), the heater-off markers or the
    // idle-travel cooling (H3C) - written by GCode::append_tcr into the [filament_end_gcode] slot, before the
    // change_filament template. Coordinates are tower-local like ToolChangeResult's.
    struct NozzleChangeResult
    {
        std::string        gcode;
        Vec2f              start_pos        = Vec2f::Zero();
        Vec2f              origin_start_pos = Vec2f::Zero();
        Vec2f              end_pos          = Vec2f::Zero();
        std::vector<Vec2f> wipe_path;
        bool               is_extruder_change      = false;
        bool               heater_off_only         = false; // markers only; the toolchange retract stays at the model
        bool               zero_flush_idle_cooling = false; // travel-only cooling; no unretract / retract around it
        int                precool_target_temp     = 0;
        int                precool_filament_id     = -1;
        bool               has_action() const { return !gcode.empty() || heater_off_only || zero_flush_idle_cooling; }
    };

    // What Print knows about a planned change's nozzles (the plate's nozzle grouping); WipeTower has no
    // grouping of its own. need_ramming: the two filaments sit on different nozzles of a Bambu printer
    // (Bambu Studio is_need_ramming()).
    struct NozzleChangePlan
    {
        bool need_ramming       = false;
        bool extruder_change    = false; // different extruders (or hotend_as_extruder): filament_change_length, else _nc
        bool dynamic_nozzle_map = false; // M632 names the nozzle (H<n>)
        int  old_nozzle         = -1;
        int  new_nozzle         = -1;
        int  old_extruder       = -1;    // logical extruder of the outgoing filament (hotend_cooling_rate index)
        int  old_physical_extruder = -1; // T of the pre-cool M104
    };

	struct ToolChangeResult
	{
		// Print heigh of this tool change.
		float					print_z;
		float 					layer_height;
		// G-code section to be directly included into the output G-code.
		std::string				gcode;
		// For path preview.
		std::vector<Extrusion> 	extrusions;
		// Initial position, at which the wipe tower starts its action.
		// At this position the extruder is loaded and there is no Z-hop applied.
		Vec2f						start_pos;
		// Last point, at which the normal G-code generator of Slic3r shall continue.
		// At this position the extruder is loaded and there is no Z-hop applied.
		Vec2f						end_pos;
		// Time elapsed over this tool change.
		// This is useful not only for the print time estimation, but also for the control of layer cooling.
		float  				    elapsed_time;

        // Is this a priming extrusion? (If so, the wipe tower rotation & translation will not be applied later)
        bool                    priming;

        // Pass a polyline so that normal G-code generator can do a wipe for us.
        // The wipe cannot be done by the wipe tower because it has to pass back
        // a loaded extruder, so it would have to either do a wipe with no retraction
        // (leading to https://github.com/prusa3d/PrusaSlicer/issues/2834) or do
        // an extra retraction-unretraction pair.
        std::vector<Vec2f> wipe_path;

		// BBS
        float purge_volume = 0.f;

        // Initial tool
        int initial_tool;

        // New tool
        int new_tool;

        // BBS: in bbl filament_change_gcode, toolhead will be moved to the wipe tower automatically.
        // But if finish_layer_tcr is before tool_change_tcr, we have to travel to the wipe tower before
        // executing the gcode finish_layer_tcr.
        bool is_finish_first = false;

        // Orca #15841: folded into a later, thicker layer, so the emitter drops it. Set by the tower, so
        // the two cannot disagree about which layers print.
        bool combined_away = false;

        // BBS: this toolchange may cool the nozzle before the tower (change_filament_gcode's
        // M620.15 C{new_filament_temp - filament_cooling_before_tower}): it is not on the tower's
        // first layer and not a tower interface (Bambu Studio's "contact") toolchange, and the tower
        // wrote "Wipe tower reheat before wipe" back to the print temperature when the new filament
        // cools at all. GCode publishes 0 cooling for every other toolchange.
        bool reheats_after_cooling = false;

        // Bambu Studio 2.9.1: the nozzle change before this toolchange (empty when the nozzle stays).
        NozzleChangeResult nozzle_change_result;

        // Orca #10780: this result holds a tool change, which starts at tool_change_start_pos. A block
        // filled with the old filament may be merged in front of it (start_pos is then that block's),
        // so GCode moves to tool_change_start_pos after change_filament_gcode, not to start_pos.
        bool  is_tool_change        = false;
        Vec2f tool_change_start_pos = Vec2f::Zero();
        // Orca #10780: a solid "contact" tool change, where a category starts or stops on the tower.
        bool  is_contact            = false;
        // Bambu Studio 2.9.1: the change scrapes the ooze beside the tower before its purge. Its G-code carries
        // [wipe_tower_deretract_gcode] (the unretract, after the scrape) and the Z placeholders
        // [wipe_tower_scrape_z_low] / [wipe_tower_scrape_z_top], which GCode::append_tcr fills in: the tower only
        // knows the Z it planned the layer at, wipe_tower_no_sparse_layers prints it lower.
        bool  side_scrape                       = false;
        float side_scrape_z_low                 = 0.f; // lowest scrape Z in the planned layer frame
        float side_scrape_depth                 = 0.f; // requested descent below the tower top
        float side_scrape_min_height_above_base = 0.f; // keep this far above the tower's first layer

		// Sum the total length of the extrusion.
		float total_extrusion_length_in_plane() {
			float e_length = 0.f;
			for (size_t i = 1; i < this->extrusions.size(); ++ i) {
				const Extrusion &e = this->extrusions[i];
				if (e.width > 0) {
					Vec2f v = e.pos - (&e - 1)->pos;
					e_length += v.norm();
				}
			}
			return e_length;
		}
		bool force_travel = false;

        // Edge: with wall gaps on (WipeTower2), a point in the tower level with the gap the wall leaves
        // for this tool change, on the gap's side; in the frame of start_pos. The gap is cut at the
        // staggered purge start (or the ramming end), not at start_pos, so the entry travel goes in
        // through the gap to here before it goes on to start_pos.
        bool  has_gap_entry = false;
        Vec2f gap_entry     = Vec2f::Zero();
	};

    struct box_coordinates
    {
        box_coordinates(float left, float bottom, float width, float height) :
            ld(left        , bottom         ),
            lu(left        , bottom + height),
            rd(left + width, bottom         ),
            ru(left + width, bottom + height) {}
        box_coordinates(const Vec2f &pos, float width, float height) : box_coordinates(pos(0), pos(1), width, height) {}
        void translate(const Vec2f &shift) {
            ld += shift; lu += shift;
            rd += shift; ru += shift;
        }
        void translate(const float dx, const float dy) { translate(Vec2f(dx, dy)); }
        void expand(const float offset) {
            ld += Vec2f(- offset, - offset);
            lu += Vec2f(- offset,   offset);
            rd += Vec2f(  offset, - offset);
            ru += Vec2f(  offset,   offset);
        }
        void expand(const float offset_x, const float offset_y) {
            ld += Vec2f(- offset_x, - offset_y);
            lu += Vec2f(- offset_x,   offset_y);
            rd += Vec2f(  offset_x, - offset_y);
            ru += Vec2f(  offset_x,   offset_y);
        }
        Vec2f ld;  // left down
        Vec2f lu;	// left upper
        Vec2f rd;	// right lower
        Vec2f ru;  // right upper
    };

    // Construct ToolChangeResult from current state of WipeTower and WipeTowerWriter.
    // WipeTowerWriter is moved from !
    ToolChangeResult construct_tcr(WipeTowerWriter& writer,
                                   bool priming,
                                   size_t old_tool,
                                   bool is_finish,
                                   bool is_tool_change,
                                   float purge_volume,
                                   bool is_contact) const;
    // Orca #10780: the fill of one block, printed with filament_id loaded (no tool change).
    ToolChangeResult construct_block_tcr(WipeTowerWriter& writer,
                                         size_t filament_id,
                                         bool is_finish) const;

	// x			-- x coordinates of wipe tower in mm ( left bottom corner )
	// y			-- y coordinates of wipe tower in mm ( left bottom corner )
	// width		-- width of wipe tower in mm ( default 60 mm - leave as it is )
	// wipe_area	-- space available for one toolchange in mm
	// BBS: add partplate logic
	WipeTower(const PrintConfig& config, int plate_idx, Vec3d plate_origin, const float wipe_volume, size_t initial_tool, const float wipe_tower_height);


	// Set the extruder properties.
    void set_extruder(size_t idx, const PrintConfig& config);

    // Edge: G-code tag dialect (GCodeProcessor::reserved_tag) of the tower's output. True (the
    // Bambu tags) for Bambu Lab printers; false for a non-Bambu printer set to wipe_tower_type
    // Type 1, whose export reads the compatible tags.
    void set_bbl_gcode_tags(bool bbl) { m_bbl_gcode_tags = bbl; }
    // Bambu Studio 2.9.1 (Print.cpp:4220, set_first_layer_flow_ratio): the process' initial_layer_flow_ratio, which
    // is bottom_solid_infill_flow_ratio here (Format/BambuKeyAliases.cpp), scales the tower's first-layer purge.
    void set_first_layer_flow_ratio(float flow_ratio) { m_first_layer_flow_ratio = flow_ratio; }

    // Orca #10780 / #14685: the plate's per-layer nozzle grouping. The tower reads the extruder a filament
    // prints from on a layer (is_valid_last_layer(): an extruder's last layer above its printable height).
    // Optional: printers without a grouping leave it unset.
    void set_nozzle_group_result(std::shared_ptr<const MultiNozzleUtils::LayeredNozzleGroupResult> group) { m_nozzle_group = std::move(group); }

	// Appends into internal structure m_plan containing info about the future wipe tower
	// to be used before building begins. The entries must be added ordered in z.
	void plan_toolchange(float z_par, float layer_height_par, unsigned int old_tool, unsigned int new_tool, float wipe_volume,
	                     float prime_volume, const NozzleChangePlan &nozzle_change);
	// Without a nozzle change. An overload rather than a default argument: GCC (and clang) reject a default argument
	// that value-initialises a nested struct with default member initializers inside its enclosing class.
	void plan_toolchange(float z_par, float layer_height_par, unsigned int old_tool, unsigned int new_tool, float wipe_volume = 0.f,
	                     float prime_volume = 0.f)
	{
	    plan_toolchange(z_par, layer_height_par, old_tool, new_tool, wipe_volume, prime_volume, NozzleChangePlan());
	}
    // Bambu Studio 2.9.1 (set_toolchange): path length of the ramming lines old_tool lays before a nozzle change
    // (filament_change_length(_nc) over a ramming line's extrusion per mm), and the depth whole lines of it take
    // in a tower of this width. Public for the tests.
    float nozzle_change_length(size_t old_tool, bool extruder_change, float layer_height) const;
    float nozzle_change_depth(size_t old_tool, float length) const;
    // The ramming line width of a filament's nozzle (Bambu Studio's nozzle_change_width_for_diameter table).
    float nozzle_change_width(size_t tool) const;
	// The last planned layer starts with `tool` loaded: its Local-Z passes changed filament directly
	// (GCode::process_layer phase b) from `prev_tool` before the tower's planned changes.
	// Only before the layer's first planned change: a later object layer joined to this tower layer
	// keeps the order already planned.
	void set_last_layer_start_tool(unsigned int prev_tool, unsigned int tool)
	{
		if (! m_plan.empty() && m_plan.back().tool_changes.empty() && m_plan.back().local_z_start_tool < 0) {
			m_plan.back().local_z_prev_tool  = int(prev_tool);
			m_plan.back().local_z_start_tool = int(tool);
		}
	}

	// Iterates through prepared m_plan, generates ToolChangeResults and appends them to "result"
	void generate(std::vector<std::vector<ToolChangeResult>> &result);

	WipeTower::ToolChangeResult only_generate_out_wall();

    float get_depth() const { return m_wipe_tower_depth; }
    // The brim, or the room tower interface run-ins need when that is wider: what the plate has to
    // keep clear around the tower (see TowerInterface::run_in_reserve()).
    float get_brim_width() const { return std::max(m_wipe_tower_brim_width_real, m_run_in_reserve); }
    float get_height() const { return m_wipe_tower_height; }
    float get_layer_height() const { return m_layer_height; }

    // Rib wall (wipe_tower_wall_type = rib). Valid after generate().
    bool  is_rib_wall() const { return m_use_rib_wall; }
    float get_rib_length() const { return m_rib_length; }
    float get_rib_width() const { return m_rib_width; }
    // Shift of the whole tower so that its first-layer wall starts at the tower position, as
    // Bambu Studio's rib_offset does. Already applied to all G-code, extrusions and wipe paths the
    // generator returns (unlike Bambu Studio, where GCode.cpp adds it), so callers only need it to
    // place the tower body: the body box spans rib_offset .. rib_offset + (width, depth).
    Vec2f get_rib_offset() const { return m_rib_offset; }
    // Size of the tower's first-layer wall, brim excluded, from the tower position: the rib
    // wall's bounding box, or width x depth for a rectangle.
    Vec2f get_footprint_size() const { return m_use_rib_wall ? m_rib_footprint : Vec2f(m_wipe_tower_width, m_wipe_tower_depth); }
    // First-layer wall outline in the tower frame (rib wall only; empty otherwise).
    const Polygon &get_first_layer_wall() const { return m_first_layer_wall; }
    // Extruded outer walls (and brim loops) per print_z, in the tower frame (rib wall only).
    const std::map<float, Polylines> &get_outer_wall() const { return m_outer_wall; }

	void set_last_layer_extruder_fill(bool extruder_fill) {
        if (!m_plan.empty()) {
			m_plan.back().extruder_fill = extruder_fill;
		}
	}

	void set_wipe_volume(std::vector<std::vector<float>>& wiping_matrix) {
		wipe_volumes = wiping_matrix;
	}

	// Switch to a next layer.
	void set_layer(
		// Print height of this layer.
		float print_z,
		// Layer height, used to calculate extrusion the rate.
		float layer_height,
		// Maximum number of tool changes on this layer or the layers below.
		size_t max_tool_changes,
		// Is this the first layer of the print? In that case print the brim first.
		bool is_first_layer,
		// Is this the last layer of the waste tower?
		bool is_last_layer)
	{
		m_z_pos 				= print_z;
		m_layer_height			= layer_height;
		m_depth_traversed  = 0.f;
        m_current_layer_finished = false;
		//m_current_shape = (! is_first_layer && m_current_shape == SHAPE_NORMAL) ? SHAPE_REVERSED : SHAPE_NORMAL;
		m_current_shape = SHAPE_NORMAL;
		if (is_first_layer) {
            m_num_layer_changes = 0;
            m_num_tool_changes 	= 0;
        } else
            ++ m_num_layer_changes;
		
		// Calculate extrusion flow from desired line width, nozzle diameter, filament diameter and layer_height:
		m_extrusion_flow = extrusion_flow(layer_height);

        // Advance m_layer_info iterator, making sure we got it right
		while (!m_plan.empty() && m_layer_info->z < print_z - WT_EPSILON && m_layer_info+1 != m_plan.end())
			++m_layer_info;
	}

	// Return the wipe tower position.
	const Vec2f& 		 position() const { return m_wipe_tower_pos; }
	// Return the wipe tower width.
	float     		 width()    const { return m_wipe_tower_width; }
	// The wipe tower is finished, there should be no more tool changes or wipe tower prints.
	bool 	  		 finished() const { return m_max_color_changes == 0; }

	// Returns gcode to prime the nozzles at the front edge of the print bed.
	std::vector<ToolChangeResult> prime(
		// print_z of the first layer.
		float 						initial_layer_print_height, 
		// Extruder indices, in the order to be primed. The last extruder will later print the wipe tower brim, print brim and the object.
		const std::vector<unsigned int> &tools,
		// If true, the last priming are will be the same as the other priming areas, and the rest of the wipe will be performed inside the wipe tower.
		// If false, the last priming are will be large enough to wipe the last extruder sufficiently.
		bool 						last_wipe_inside_wipe_tower);

	// Returns gcode for a toolchange and a final print head position.
	// On the first layer, extrude a brim around the future wipe tower first.
	// BBS
    ToolChangeResult tool_change(size_t new_tool, bool extrude_perimeter = false, bool first_toolchange_to_nonsoluble = false);

	// Fill the unfilled space with a sparse infill.
	// Call this method only if layer_finished() is false.
    ToolChangeResult finish_layer(bool extruder_perimeter = true, bool extruder_fill = true);

	// Calculates extrusion flow needed to produce required line width for given layer height
    float extrusion_flow(float layer_height = -1.f) const // negative layer_height - return current m_extrusion_flow
    {
        if (layer_height < 0) return m_extrusion_flow;
        return layer_height * (m_perimeter_width - layer_height * (1.f - float(M_PI) / 4.f)) / filament_area();
    }

    // Mixed nozzle diameters (per-nozzle P2, Edge; same design as WipeTower2's FOrca port): the line width
    // a tool lays its own purge at, its nozzle x Width_To_Nozzle_Ratio. m_perimeter_width stays the tower's
    // structure width (footprint, wall, brim, sparse fill). Uniform nozzles: the same float, so every
    // tower is unchanged.
    float tool_line_width(size_t tool) const
    {
        // Bambu Studio 2.9.1 prime_tower_line_width (WipeTower.cpp:2054-2063): one width for every tool.
        if (m_tower_line_width > 0.f)
            return m_tower_line_width;
        if (tool < m_filpar.size() && m_filpar[tool].nozzle_diameter > 0.f)
            return m_filpar[tool].nozzle_diameter * Width_To_Nozzle_Ratio;
        return m_perimeter_width;
    }
    // The extrusion flow of the tool's line width at layer_height (the current layer when negative);
    // exactly extrusion_flow(layer_height) when the tool's width is the structure width.
    float tool_extrusion_flow(size_t tool, float layer_height = -1.f) const
    {
        const float w = tool_line_width(tool);
        if (w == m_perimeter_width)
            return extrusion_flow(layer_height);
        const float h = layer_height < 0 ? m_layer_height : layer_height;
        return h * (w - h * (1.f - float(M_PI) / 4.f)) / filament_area();
    }

	bool get_floating_area(float& start_pos_y, float& end_pos_y) const;
	bool need_thick_bridge_flow(float pos_y) const;
    float get_extrusion_flow() const { return m_extrusion_flow; }

	// Is the current layer finished?
	bool 			 layer_finished() const {
        return m_current_layer_finished;
	}

    std::vector<float> get_used_filament() const { return m_used_filament_length; }
    int get_number_of_toolchanges() const { return m_num_tool_changes; }

    struct FilamentParameters {
        std::string 	    material = "PLA";
        bool                is_soluble = false;
        // BBS
        bool                is_support = false;
        int  			    nozzle_temperature = 0;
        int  			    nozzle_temperature_initial_layer = 0;
        float               loading_speed = 0.f;
        float               loading_speed_start = 0.f;
        float               unloading_speed = 0.f;
        float               unloading_speed_start = 0.f;
        float               delay = 0.f ;
        int                 cooling_moves = 0;
        float               cooling_initial_speed = 0.f;
        float               cooling_final_speed = 0.f;
        float               ramming_line_width_multiplicator = 1.f;
        float               ramming_step_multiplicator = 1.f;
        float               max_e_speed = std::numeric_limits<float>::max();
        std::vector<float>  ramming_speed;
        float               nozzle_diameter = 0.f;
        float               filament_area;
        float               wipe_dist = 0.f; // wipe_distance, for the wipe along a rib wall
        // Tower interface (TowerInterface::): the filament as the trigger sees it, and its values.
        TowerInterface::FilamentKind kind;
        int                 interface_temperature = 0;
        float               run_in_distance       = 0.f;
        float               extra_prime_length    = 0.f;
        int                 physical_extruder     = -1; // T of its M104/M109 on a multi-nozzle machine, -1 for none
        // BBS: filament_cooling_before_tower (with the filament-switcher extra) and the T of the
        // tower's "Wipe tower reheat before wipe" M104 (Bambu Studio always names the physical extruder).
        float               cooling_before_tower  = 0.f;
        int                 reheat_extruder       = 0;
        // Orca #15841: tallest layer this filament's nozzle can lay down; caps the sparse layer combination.
        float               max_layer_height      = 0.f;
        // Bambu Studio 2.9.1 nozzle-change ramming; .first an extruder change, .second a hotend change.
        std::pair<float, float> change_length        { 0.f, 0.f }; // filament_change_length(_nc), mm of filament
        std::pair<float, float> max_e_ramming_speed  { 0.f, 0.f }; // mm/s of filament
        std::pair<float, float> ramming_travel_time  { 0.f, 0.f }; // s
        std::pair<int, int>     precool_target_temp  { 0, 0 };     // 0 = no pre-cooling
        // Seconds the nozzle needs to cool to the pre-cool target, per logical extruder (hotend_cooling_rate).
        std::pair<std::vector<float>, std::vector<float>> precool_t;
        std::pair<std::vector<float>, std::vector<float>> precool_t_first_layer;
        bool                idle_travel_cooling_enabled = true;
        // Orca #10780: filament_adhesiveness_category, the block this filament purges into.
        int                 category              = 0;
        // Orca #15035: wall gap ironing at the start of a purge (retraction around it) and its flat spiral.
        float               retract_length        = 0.f;
        float               retract_speed         = 0.f;
        float               flat_iron_area        = 0.f;
        // Bambu Studio 2.9.1: filament_prime_tower_speed_factor_curve (empty = the built-in ramp) and the
        // prime_tower_feedrate_factor the H3C tower speed is scaled by.
        std::vector<float>  speed_factor_curve;
        float               feedrate_factor       = 1.f;
    };

    // Orca #10780 (Bambu Studio): a block holds the purges of one adhesiveness category, in a fixed band
    // of the tower (start_depth .. start_depth + depth) on every layer.
    enum class WipeTowerLayerType : unsigned char {
        Normal,
        Contact,    // the first layer where the category starts or stops being purged: printed solid and slow
        Solid,      // one of the layers below such a change: printed solid
        Contact_UP  // the layer above a contact layer: solid and slow
    };
    struct WipeTowerBlock
    {
        int                             block_id{0};
        int                             filament_adhesiveness_category{0};
        std::vector<float>              layer_depths;  // depth this block takes on each layer
        std::vector<float>              finish_depth;  // where the previous layer's sparse fill of this block starts
        std::vector<WipeTowerLayerType> layers_type;
        float                           depth{0};
        float                           start_depth{0};
        float                           cur_depth{0};
        int                             last_filament_change_id{-1};
        int                             last_nozzle_change_id{-1};
    };
    // The planned blocks, in tower order (valid after generate(); for the tests).
    const std::vector<WipeTowerBlock> &blocks() const { return m_wipe_tower_blocks; }

private:
	enum wipe_shape // A fill-in direction
	{
		SHAPE_NORMAL = 1,
		SHAPE_REVERSED = -1
	};

    const float Width_To_Nozzle_Ratio = 1.25f; // desired line width (oval) in multiples of nozzle diameter - may not be actually neccessary to adjust
    const float WT_EPSILON            = 1e-3f;
    float filament_area() const {
        return m_filpar[0].filament_area; // all extruders are assumed to have the same filament diameter at this point
    }

	bool   m_enable_timelapse_print = false;
	bool   m_semm               = true; // Are we using a single extruder multimaterial printer?
	bool   m_purge_in_prime_tower = false; // Do we purge in the prime tower?
    Vec2f  m_wipe_tower_pos; 			// Left front corner of the wipe tower in mm.
	float  m_wipe_tower_width; 			// Width of the wipe tower.
	float  m_wipe_tower_depth 	= 0.f; 	// Depth of the wipe tower
	// BBS
	float  m_wipe_tower_height = 0.f;
    float  m_wipe_tower_brim_width      = 0.f; 	// Width of brim (mm) from config
    float  m_wipe_tower_brim_width_real = 0.f; 	// Width of brim (mm) after generation
	float  m_wipe_tower_rotation_angle = 0.f; // Wipe tower rotation angle in degrees (with respect to x axis)
    float  m_internal_rotation  = 0.f;
	float  m_y_shift			= 0.f;  // y shift passed to writer
	float  m_z_pos 				= 0.f;  // Current Z position.
	float  m_layer_height 		= 0.f; 	// Current layer height.
	size_t m_max_color_changes 	= 0; 	// Maximum number of color changes per layer.
    int    m_old_temperature    = -1;   // To keep track of what was the last temp that we set (so we don't issue the command when not neccessary)
    float  m_travel_speed       = 0.f;
    // Bambu Studio 2.9.1 prime_tower_acceleration and the travel acceleration the tower returns to
    // between extrusions; 0 = no M204 from the tower (the default). Not used on the first layer.
    unsigned int m_tower_acceleration        = 0;
    // Bambu Studio 2.9.1 prime_tower_line_width; 0 = the nozzle diameter x Width_To_Nozzle_Ratio.
    float        m_tower_line_width          = 0.f;
    unsigned int m_tower_travel_acceleration = 0;
    unsigned int writer_tower_acceleration() const { return is_first_layer() ? 0 : m_tower_acceleration; }
public:
    // Travel acceleration between the tower's extrusions when prime_tower_acceleration is set.
    void set_travel_acceleration(double acceleration) { m_tower_travel_acceleration = acceleration > 0. ? (unsigned int) std::floor(acceleration + 0.5) : 0; }
private:
    float  m_first_layer_speed  = 0.f;
    size_t m_first_layer_idx    = size_t(-1);
    size_t m_cur_layer_id;
	// G-code generator parameters.
    float           m_cooling_tube_retraction   = 0.f;
    float           m_cooling_tube_length       = 0.f;
    float           m_parking_pos_retraction    = 0.f;
    float           m_extra_loading_move        = 0.f;
    float           m_bridging                  = 0.f;
    bool            m_sparse_layers_skipped     = false;
    bool            m_sparse_layers_combined    = false;
    bool            m_set_extruder_trimpot      = false;
    bool            m_adhesion                  = true;
    GCodeFlavor     m_gcode_flavor;
    // Ultra (H2C rack): machine has a swappable nozzle rack (extruder_max_nozzle_count > 1).
    bool            m_has_nozzle_rack           = false;
    bool            m_bbl_gcode_tags            = true;

    // Rib wall, ported from Bambu Studio (prime_tower_rib_wall and friends). A cone wall is a
    // WipeTower2 feature; this generator prints it as a rectangle.
    bool            m_use_rib_wall              = false;
    float           m_rib_width                 = 0.f; // clamped to half the tower's shorter side by plan_tower()
    float           m_extra_rib_length          = 0.f;
    float           m_rib_length                = 0.f; // full rib length at the first layer, set by plan_tower()
    bool            m_used_fillet               = false;
    // wipe_tower_max_purge_speed (Bambu Studio's prime_tower_max_speed), mm/min.
    float           m_max_speed                 = 5400.f;
    // Emit G2/G3 for the rib wall's fillets, like Bambu Studio with enable_arc_fitting. Off for a
    // rotated tower: the G-code post-processor rotates X/Y but not the I/J centre offsets.
    bool            m_enable_arc_fitting        = false;
    Vec2f           m_rib_offset                = Vec2f::Zero();
    Vec2f           m_rib_footprint             = Vec2f::Zero();
    Polygon         m_first_layer_wall;
    std::map<float, Polylines> m_outer_wall;

    // Tower interface options. A run-in crosses the wall through a gap on the interface layer and the
    // layers below it (m_interface_gaps, per plan layer, tower-local); the brim loops get those gaps too.
    TowerInterface::Settings m_interface;
    // wipe_tower_wall_gap (Bambu Studio prime_tower_skip_points): a gap at every purge start; the run-in needs it.
    bool            m_use_gap_wall              = false;
    BoundingBoxf    m_shared_bed;                        // printable area every nozzle reaches, plate coordinates
    float           m_run_in_reserve            = 0.f;
    std::vector<std::vector<TowerInterface::GapPoint>> m_interface_gaps;

    // Bed properties
    enum {
        RectangularBed,
        CircularBed,
        CustomBed
    } m_bed_shape;
    float m_bed_width; // width of the bed bounding box
    Vec2f m_bed_bottom_left; // bottom-left corner coordinates (for rectangular beds)

	float m_perimeter_width = 0.4f * Width_To_Nozzle_Ratio; // Width of an extrusion line, also a perimeter spacing for 100% infill.
	float m_extrusion_flow = 0.038f; //0.029f;// Extrusion flow is derived from m_perimeter_width, layer height and filament diameter.

	// Extruder specific parameters.
    std::vector<FilamentParameters> m_filpar;


	// State of the wipe tower generator.
	unsigned int m_num_layer_changes = 0; // Layer change counter for the output statistics.
	unsigned int m_num_tool_changes  = 0; // Tool change change counter for the output statistics.
	///unsigned int 	m_idx_tool_change_in_layer = 0; // Layer change counter in this layer. Counting up to m_max_color_changes.
	bool m_print_brim = true;
	// A fill-in direction (positive Y, negative Y) alternates with each layer.
	wipe_shape   	m_current_shape = SHAPE_NORMAL;
    size_t 	m_current_tool  = 0;
	// Orca: support mmu wipe tower
    std::vector<std::vector<float>> wipe_volumes;
	const float		m_wipe_volume;

	float           m_depth_traversed = 0.f; // Current y position at the wipe tower.
    bool            m_current_layer_finished = false;
	bool 			m_left_to_right   = true;
	// prime_tower_infill_gap until calc_block_infill_gap() has turned it into the block gaps, then the
	// stretch plan_tower_new() applies to reach the stability minimum (1 = none).
	float			m_extra_spacing   = 1.f;

    // Orca #10780 block planner (Bambu Studio's WipeTowerBlock planner).
    struct BlockDepthInfo
    {
        int   category{-1};
        float depth{0};
        float nozzle_change_depth{0};
    };
    std::vector<std::vector<BlockDepthInfo>> m_all_layers_depth;
    std::vector<WipeTowerBlock>              m_wipe_tower_blocks;
    WipeTowerBlock                          *m_cur_block = nullptr;
    // Per category: the gap between purge lines (.first) and between ramming lines (.second).
    std::unordered_map<int, std::pair<float, float>> m_block_infill_gap_width;
    // The widest ramming line of the filaments (Orca: one tower-wide width; equal with uniform nozzles).
    float           m_nozzle_change_perimeter_width = 0.f;
    float           m_tpu_fixed_spacing             = 2.f;      // ramming line gap with a reverse travel
    float           m_contact_speed                 = 20.f * 60.f;
    // The tower's first-layer purge flow: Bambu Studio's initial_layer_flow_ratio (bottom_solid_infill_flow_ratio
    // here, set_first_layer_flow_ratio), Orca's first_layer_flow_ratio; all default to 1.
    float           m_first_layer_flow_ratio        = 1.f;
    bool            m_tower_framework               = false;    // prime_tower_enable_framework
    bool            m_flat_ironing                  = false;    // prime_tower_flat_ironing (needs the wall gap)
    // Wall gaps of each layer at the tool changes' start points (prime_tower_skip_points / wipe_tower_wall_gap).
    std::vector<std::vector<Vec2f>> m_wall_skip_points;
    // is_valid_last_layer(): each extruder's last tower layer and printable height.
    std::vector<int>                m_last_layer_id;
    std::vector<double>             m_printable_height;
    std::shared_ptr<const MultiNozzleUtils::LayeredNozzleGroupResult> m_nozzle_group;

    bool is_first_layer() const { return size_t(m_layer_info - m_plan.begin()) == m_first_layer_idx; }

	// Calculates length of extrusion line to extrude given volume
	float volume_to_length(float volume, float line_width, float layer_height) const {
		return std::max(0.f, volume / (layer_height * (line_width - layer_height * (1.f - float(M_PI) / 4.f))));
	}
    // Calculates volume of extrusion line
    float length_to_volume(float length, float line_width, float layer_height) const
    {
        return std::max(0.f, length * (layer_height * (line_width - layer_height * (1.f - float(M_PI) / 4.f))));
    }

	// Orca #15841: whether the layer reaches the G-code, and so whether its extrusions count as filament used.
	bool layer_is_printed(bool toolchanges_on_layer) const;
    // Whether the tower lays this planned layer at all: with sparse layers skipped only a layer with a
    // tool change is printed (WipeTowerIntegration leaves the others out), the tower's first included.
    bool plan_layer_is_laid(size_t layer_idx) const;

	// BBS
	box_coordinates align_perimeter(const box_coordinates& perimeter_box);

    // Rib wall of the current layer around wt_box (tower-local coordinates, like wt_box): Bambu
    // Studio's generate_rib_polygon(), filleted and re-unioned with the box when fillet_wall is on,
    // as its generate_support_wall_new() does.
    Polygon generate_rib_polygon(const box_coordinates &wt_box) const;
    Polygon rib_wall_polygon(const box_coordinates &wt_box) const;
    // Records an extruded wall / brim loop (tower-local) for the fake tower the conflict checker and
    // the object brim use.
    void    record_outer_wall(const Polygon &wall);
    // Measures the first printed layer's wall and sets m_rib_offset / m_rib_footprint from it.
    void    set_rib_offset();
    // Bambu Studio 2.9.1 calc_tower_feedrate(): the tower speed of tool's extrusions at the tower flow. First layer: the
    // first-layer speed; on hotend_as_extruder printers (H3C) the volumetric speed times prime_tower_feedrate_factor,
    // capped by the machine's XY limit; otherwise the volumetric speed capped at prime_tower_max_speed.
    float   calc_tower_feedrate(size_t tool, bool first_layer) const;
    // Speed of the outer wall, brim and fills (calc_tower_feedrate() on the current layer).
    float   wall_feedrate(size_t tool) const { return calc_tower_feedrate(tool, is_first_layer()); }

    // Bambu Studio 2.9.1 tail slowdown (handle_hotend_as_extruder only): the last prime_tower_tail_slowdown_volume mm3 of
    // every tower visit at prime_tower_tail_speed_factor, never on the tower's first layer.
    bool    tail_slowdown_applies() const { return m_handle_hotend_as_extruder && m_tail_speed_factor < 1.f && !is_first_layer(); }
    void    apply_tail_slowdown(ToolChangeResult &tcr) const;

    // Bambu Studio 2.9.1 side scrape (WipeTower.cpp 4659-4995). The geometry works on one pair of opposite walls with
    // two abstract axes: "across" is the axis the nozzle approaches the wall on, "along" the wall tangent;
    // across_is_x picks the left/right pair, positive_side the wall with the larger coordinate. All of it is in the
    // tower footprint frame m_scrape_walls is stored in: the writer frame with the layer's y shift folded in.
    struct SideScrapeZ
    {
        float z_low                 = 0.f;
        float depth                 = 0.f;
        float min_height_above_base = 0.f;
    };
    // With the scrape on, every layer keeps the full tower depth, so no wall steps inward above the brim chamfer.
    bool    keeps_outer_walls_unshrunk() const { return m_enable_side_scrape; }
    bool    toolchange_side_scrape(WipeTowerWriter &writer, const Vec2f &initial_position, Vec2f *out_scrape_start, SideScrapeZ *out_z);
    bool    outer_wall_extreme_at(float z, bool across_is_x, bool positive_side, float along, float &out_across) const;
    bool    side_wall_straight_along_range(bool across_is_x, bool positive_side, float &along_begin, float &along_end) const;
    // Lowest Z the scrape descends to: above the highest layer that carries a brim chamfer loop (the tower's first
    // layer without a brim), with a clearance.
    // above_base: the same floor measured from the top of the tower's first layer in laid layers.
    float   side_scrape_z_floor(float *above_base = nullptr) const;
    // Records a wall or brim loop the current layer printed (tower-local polygon) for the scrape geometry.
    void    record_scrape_wall(const Polygon &wall);
    // The brim chamfer loops the tower prints on plan layer `layer` (finish_layer_new()'s rule).
    int     brim_chamfer_loops_num(size_t layer) const;
    // First layer with the scrape on (Bambu Studio finish_layer_new()): loops fill what the outer wall encloses beside
    // the purge bands, so the base the scrape pushes against is glued to the bed everywhere.
    void    fill_first_layer_voids(WipeTowerWriter &writer, const Polygon &outer_wall, const std::vector<std::pair<float, float>> &purge_bands, float feedrate);


    // to store information about tool changes for a given layer
	struct WipeTowerInfo{
		struct ToolChange {
            size_t old_tool;
            size_t new_tool;
			float required_depth;
            float ramming_depth;
            float first_wipe_line;
            float wipe_volume;
			float wipe_length;
			// BBS
			float purge_volume;
            // Tower interface: whether the trigger picks this change (mark_interfaces()), and where its
            // purge starts on the outer wall (tower-local, x = 0 or the tower width), where a wall gap and
            // the run-in cross it (get_wall_skip_points()).
            bool  is_interface = false;
            bool  run_in    = false;
            Vec2f start_point = Vec2f::Zero();
            // Bambu Studio 2.9.1: the ramming before a nozzle change, part of required_depth.
            NozzleChangePlan nozzle_change;
            float nozzle_change_length = 0.f; // mm of ramming path
            float nozzle_change_depth  = 0.f;
            ToolChange(size_t old, size_t newtool, float depth=0.f, float ramming_depth=0.f, float fwl=0.f, float wv=0.f, float wl = 0, float pv = 0)
				: old_tool{ old }, new_tool{ newtool }, required_depth{ depth }, ramming_depth{ ramming_depth }, first_wipe_line{ fwl }, wipe_volume{ wv }, wipe_length{ wl }, purge_volume{ pv } {}
		};
		float z;		// z position of the layer
		float height;	// layer height
		float depth;	// depth of the layer based on all layers above
		float extra_spacing;
        bool  extruder_fill{true};
		// Orca #15841: folded into a later, thicker layer, so this one prints nothing at all.
		bool  combined_away{false};
		float toolchanges_depth() const { float sum = 0.f; for (const auto &a : tool_changes) sum += a.required_depth; return sum; }

		std::vector<ToolChange> tool_changes;
		// Local-Z / mixed color sublayer passes changed filament directly before this layer's tower
		// changes (no tower block): the filament they left loaded, or -1, and the one loaded before them.
		int local_z_start_tool{-1};
		int local_z_prev_tool{-1};

		WipeTowerInfo(float z_par, float layer_height_par)
			: z{z_par}, height{layer_height_par}, depth{0}, extra_spacing{1.f} {}
	};

	std::vector<WipeTowerInfo> m_plan; 	// Stores information about all layers and toolchanges for the future wipe tower (filled by plan_toolchange(...))
	std::vector<WipeTowerInfo>::iterator m_layer_info = m_plan.end();

    // Tower interface: marks the interface tool changes and sizes the run-in reserve.
    void    mark_interfaces();
    // The current layer's outer wall (rectangle or rib) around wt_box, cut open at its wall gaps (the tool
    // changes' start points and interface run-ins) when it has any. Returns the wall polygon (the brim
    // grows from it); extrude false only builds it.
    Polygon print_outer_wall(WipeTowerWriter &writer, const box_coordinates &wt_box, float feedrate, bool extrude);
    // Tower interface steps around a purge; the writer stands at the start of the purge.
    void    interface_before_wipe(WipeTowerWriter &writer, const WipeTowerInfo::ToolChange &tool_change);
    void    interface_after_wipe(WipeTowerWriter &writer, const WipeTowerInfo::ToolChange &tool_change);

    // Orca #10780 / #15035 block planner (Bambu Studio's plan_tower_new() / generate_new()).
    void               plan_tower_new();
    void               generate_wipe_tower_blocks(bool add_solid_flag);
    void               calc_block_infill_gap();
    void               update_all_layer_depth(float idle_depth);
    void               set_nozzle_last_layer_id();
    // Bambu Studio set_toolchange(): a change's purge and ramming depth at the current width and block gaps.
    void               replan_toolchange(WipeTowerInfo::ToolChange &tool_change, float layer_height) const;
    WipeTowerBlock    *get_block_by_category(int category, bool create);
    void               add_depth_to_block(int category, float depth, bool is_nozzle_change = false);
    int                get_filament_category(size_t tool) const { return tool < m_filpar.size() ? m_filpar[tool].category : 0; }
    void               reset_block_status();
    int                get_wall_filament_for_all_layer() const;
    // The gap between tool's purge lines (nozzle_change: its ramming lines) in its category's block.
    float              get_block_gap_width(size_t tool, bool nozzle_change) const;
    // Whether tool's extruder may still print on layer_id at layer_z (its printable height).
    bool               is_valid_last_layer(size_t tool, int layer_id, double layer_z) const;
    // Edge (#937): the filament loaded when the tower starts printing a layer (its Local-Z passes may have
    // changed it), given the one the layer below left loaded.
    static size_t      layer_start_tool(const WipeTowerInfo &layer, size_t carried);
    static size_t      layer_end_tool(const WipeTowerInfo &layer, size_t carried);
    void               get_all_wall_skip_points();
    void               get_wall_skip_points(WipeTowerInfo &layer, int layer_id, const std::vector<float> &y_shifts);
    Vec2f              get_next_pos(const box_coordinates &cleaning_box, float wipe_length, size_t tool) const;
    ToolChangeResult   tool_change_new(size_t new_tool, bool solid_toolchange = false, bool solid_nozzlechange = false);
    ToolChangeResult   finish_layer_new(bool extrude_perimeter = true, bool extrude_fill = true, bool extrude_fill_wall = true);
    ToolChangeResult   finish_block(const WipeTowerBlock &block, size_t filament_id, bool extrude_fill = true);
    // Edge (#1052 review S3): the next layer lays purge or ramming rows in this category's block (multi-block towers).
    bool               next_layer_purges_into(int category) const;
    float              block_row_pitch(int category) const;
    void               grid_fill(WipeTowerWriter &writer, const box_coordinates &fill_box, float origin_y, float row, float feedrate) const;
    ToolChangeResult   finish_block_solid(const WipeTowerBlock &block, size_t filament_id, WipeTowerLayerType layer_type);
    // reheat: "Wipe tower reheat before wipe"; after_nozzle_change: the ramming announced its own line width.
    void               toolchange_wipe_new(WipeTowerWriter &writer, const box_coordinates &cleaning_box, float wipe_length,
                                           bool solid_toolchange, bool reheat, bool after_nozzle_change);
    ToolChangeResult   merge_tcr(ToolChangeResult &first, ToolChangeResult &second) const;
    // The y shift generate() gives each layer (a single-block tower narrows towards the top, centred).
    std::vector<float> layer_y_shifts() const;

    // Stores information about used filament length per extruder:
    std::vector<float> m_used_filament_length;

	void toolchange_Unload(
		WipeTowerWriter &writer,
		const box_coordinates  &cleaning_box, 
		const std::string&	 	current_material,
		const int 				new_temperature);

	void toolchange_Change(
		WipeTowerWriter &writer,
        const size_t		new_tool,
		const std::string& 		new_material);
	
	void toolchange_Load(
		WipeTowerWriter &writer,
		const box_coordinates  &cleaning_box);

    // Bambu Studio 2.9.1 nozzle-change modes (WipeTower.cpp 3727-4054), picked by tool_change_new(). ramming()
    // rams in the outgoing filament's block and advances it; idle_travel_cooling() travels over the new
    // filament's purge area.
    NozzleChangeResult ramming(size_t old_tool, size_t new_tool, const WipeTowerInfo::ToolChange &planned, bool solid_nozzlechange);
    NozzleChangeResult heater_off_nozzle_change(size_t old_tool, size_t new_tool, const WipeTowerInfo::ToolChange &planned) const;
    NozzleChangeResult idle_travel_cooling(size_t old_tool, size_t new_tool, const WipeTowerInfo::ToolChange &planned);
    std::string        nozzle_change_marker(bool start, size_t old_tool, size_t new_tool, const NozzleChangePlan &plan) const;
    // Bambu Studio WipeTowerWriter::format_line_M104: "[M400] M104 T<physical>|I<filament> S<t> N0 C|W ;comment".
    std::string        format_line_M104(int target_temp, size_t tool, const NozzleChangePlan &plan, bool is_heating, const std::string &comment) const;

    bool               m_handle_hotend_as_extruder = false;
    std::vector<float> m_hotend_cooling_rate;

    // Bambu Studio 2.9.1 (H3C): machine XY speed limit, mm/min (the H3C tower is capped by it, not by m_max_speed).
    float              m_machine_max_speed         = 500.f * 60.f;
    // prime_tower_tail_speed_factor (0.1 .. 1) and prime_tower_tail_slowdown_volume (mm3).
    float              m_tail_speed_factor         = 1.f;
    float              m_tail_slowdown_volume      = 10.f;
    // prime_tower_line_ironing and prime_tower_ironing_speed (mm/min).
    bool               m_line_ironing              = true;
    float              m_ironing_speed             = 10.f * 60.f;
    // prime_tower_side_scrape and its settings.
    bool               m_enable_side_scrape        = false;
    bool               m_side_scrape_on_left_right = false;
    float              m_side_scrape_length        = 2.f;
    float              m_side_scrape_height        = 1.5f;
    float              m_side_scrape_speed         = 1400.f * 60.f;
    float              m_side_scrape_wall_gap      = 0.2f;
    float              m_side_scrape_point_spacing = 1.f;
    float              m_side_scrape_along_length  = 2.f;
    // Every scrape attempt advances it, cycling the start points over both walls of the pair.
    unsigned int       m_side_scrape_seq           = 0;
    // The printed walls and brim loops per layer Z, tower footprint frame (only with the scrape on). A separate store
    // from m_outer_wall, which keeps rib walls only (with the rib offset) for the conflict checker.
    std::map<float, Polylines> m_scrape_walls;
};

// Bambu Studio 2.9.1: how far the side scrape takes the nozzle outside the tower wall (wall gap + scrape length),
// 0 with the scrape off. The tower collision checks grow the tower by it.
double prime_tower_side_scrape_margin(const PrintConfig &config);

// Bambu Studio 2.9.1 apply_wipe_tower_tail_slowdown(): the last tail_volume mm3 (<= 0: all) of a tower visit's G-code at
// speed_factor of its feedrate, splitting the move the tail starts in. Exposed for the tests.
void wipe_tower_tail_slowdown(std::string &gcode, float tail_volume, float speed_factor, float filament_area,
                              const Vec2f *initial_position, const Vec2f *tool_change_start_position);


// Compaction rule for wipe_tower_no_sparse_layers. Shared by the G-code emitter and by the
// clearance validator so that both agree on where the compacted tower actually sits; a drift
// between the two would either let a real nozzle collision through or reject a safe plate.

// Whether sparse layers are really skipped, i.e. whether the tower is compacted at all. Smooth
// timelapse puts a tower on every layer, so no layer is ever dropped and the tower keeps following
// the object even though the option is on. Tower planning, G-code emission and the clearance
// validator all ask this single question, so none of them can compact on its own.
bool wipe_tower_sparse_layers_skipped(const PrintConfig &config);

// A planned layer prints no tower at all when its only toolchange keeps the same filament.
bool wipe_tower_layer_is_sparse(const std::vector<WipeTower::ToolChangeResult> &layer_tool_changes);

// Print z the compacted tower reaches on every planned layer. Sparse layers carry over the
// previous value, so the tower falls one layer height behind the object for each of them. base_z is
// the z the tower starts from, which Orca offsets by z_offset.
std::vector<float> compute_compacted_wipe_tower_z(const std::vector<std::vector<WipeTower::ToolChangeResult>> &tool_changes,
                                                  float base_z = 0.f);

// Last Z the non-BBL emitter actually prints (it always prints sparse layer 0; later sparse
// layers are skipped). Matches GCode.cpp's m_last_wipe_tower_print_z after the last layer.
float last_emitted_wipe_tower_z_nonbbl(const std::vector<std::vector<WipeTower::ToolChangeResult>> &tool_changes,
                                       float                                                       base_z = 0.f);


// Orca #15841: combination rule for wipe_tower_sparse_layers_combination. Nothing is compacted - the
// tower keeps following the object - but a run of consecutive toolchange-free layers prints as one
// thicker layer, the way infill combination merges sparse infill. Shared so that neither tower
// generator nor the G-code emitter can combine on its own.

// Whether sparse layers are really combined. Skipping them outright is the stronger answer to the
// same problem and wins over this; smooth timelapse and wrapping detection need a tower on every
// layer, so they rule it out too.
bool wipe_tower_sparse_layers_combined(const PrintConfig &config);

// A planned layer folded into a later, thicker one prints nothing at all.
bool wipe_tower_layer_is_combined_away(const std::vector<WipeTower::ToolChangeResult> &layer_tool_changes);

// Folds runs of sparse layers into one. layer_height is raised in place on the layer that prints a
// run - always its last, so the merged extrusion lands on top of what it covers - and the returned
// mask marks the layers that now print nothing. A run stops growing once one more layer would pass
// max_layer_height of the nozzle that prints it. first_layer_idx and below never combine: the
// tower's first layer carries the brim.
std::vector<char> combine_sparse_wipe_tower_layers(std::vector<float>       &layer_height,
                                                   const std::vector<char>  &layer_is_sparse,
                                                   const std::vector<float> &max_layer_height,
                                                   size_t                    first_layer_idx);

// Applies the rule above to a planned tower. Either generator's plan fits: both carry height and
// combined_away per layer, and index their filament parameters by tool. is_sparse(layer) says
// whether a layer has nothing but the tower's own finish (no tool change of any kind) and
// tool_after(layer, tool) the filament loaded once the layer is done (Edge: WipeTower2 also counts
// its Local-Z tool changes and reserve slots, which must purge at their own z).
template<class PlanLayers, class FilamentParams, class IsSparse, class ToolAfter>
void combine_sparse_wipe_tower_plan(PlanLayers &plan, const FilamentParams &filpar, size_t first_layer_idx, size_t initial_tool,
                                    IsSparse is_sparse, ToolAfter tool_after)
{
    const size_t       n = plan.size();
    std::vector<float> heights(n);
    std::vector<char>  sparse(n);
    std::vector<float> caps(n);

    // A layer with no toolchange prints with the filament the layer below left loaded.
    size_t tool = initial_tool;
    for (size_t i = 0; i < n; ++i) {
        heights[i] = plan[i].height;
        sparse[i]  = is_sparse(plan[i]) ? 1 : 0;
        caps[i]    = tool < filpar.size() ? filpar[tool].max_layer_height : 0.f;
        tool       = tool_after(plan[i], tool);
    }

    const std::vector<char> combined_away = combine_sparse_wipe_tower_layers(heights, sparse, caps, first_layer_idx);
    for (size_t i = 0; i < n; ++i) {
        plan[i].height        = heights[i];
        plan[i].combined_away = combined_away[i] != 0;
    }
}

// Edge (OD16 review): how many printed tower layers `current` is above `first_layer_idx`, for the brim
// chamfer that shrinks one loop per layer. Plain index difference (negative below the first layer),
// less the layers folded away in between, so a combined tower still steps its chamfer down one loop
// per printed layer. It also leaves out the layers is_laid(index) says the tower does not lay at all:
// the sparse layers wipe_tower_no_sparse_layers skips, over which the next tower layer sits directly on
// the last one laid, so that one is the next step of the chamfer. With every layer laid and none
// folded this is exactly current - first_layer_idx.
template<class PlanLayers, class IsLaid>
int wipe_tower_printed_layer_distance(const PlanLayers &plan, size_t first_layer_idx, size_t current, IsLaid is_laid)
{
    int distance = int(current) - int(first_layer_idx);
    for (size_t i = first_layer_idx + 1; distance > 0 && i <= current && i < plan.size(); ++i)
        if (plan[i].combined_away || ! is_laid(i))
            --distance;
    return distance;
}

template<class PlanLayers>
int wipe_tower_printed_layer_distance(const PlanLayers &plan, size_t first_layer_idx, size_t current)
{
    return wipe_tower_printed_layer_distance(plan, first_layer_idx, current, [](size_t) { return true; });
}

// Edge (OD16 review): whether a wipe tower ToolChangeResult's G-code declares its layer height (a
// `tag` line) before its first extrusion. A tag after an extrusion (the Type1 writer's bridge-flow
// switch on a narrowing tower) does not cover the paths before it.
bool wipe_tower_gcode_declares_height_first(const std::string &gcode, const std::string &tag);




} // namespace Slic3r

#endif // WipeTowerPrusaMM_hpp_ 
