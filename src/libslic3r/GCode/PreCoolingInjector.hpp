#ifndef slic3r_GCode_PreCoolingInjector_hpp_
#define slic3r_GCode_PreCoolingInjector_hpp_

// Idle-nozzle pre-cooling / pre-heating for Bambu printers with two extruders (H2D, H2D Pro, H2C,
// X2D). Port of BambuStudio's GCodeProcessor::PreCoolingInjector and the usage-block builder of
// TimeProcessor::post_process (BambuStudio GCodeProcessor.cpp:945-1220 and 6490-6860, 02.08.02.61),
// as OrcaSlicer also ported it (237ef41b06).
//
// While one extruder prints, the other one is idle. Bambu Studio lets it cool down ("M104 T<hotend>
// S<t> N0 ;Multi extruder pre cooling", right after its last use) and heats it back just in time for
// its next use ("M632 S<filament> [N R] W / M104 T<hotend> S<t> N0 ;Multi extruder pre heating /
// M633"). The timing comes from the G-code processor's time estimate, so this runs as a pass of the
// G-code post-processor, once the times are known. T is always the PHYSICAL hotend
// (physical_extruder_map[logical extruder]).
//
// Only printers whose profile sets enable_pre_heating (and that print with two extruders from a
// filament -> nozzle grouping) are touched; see pre_cooling_active().

#include "libslic3r/MultiNozzleUtils.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class Print;

namespace PreCooling {

// The comment markers GCode.cpp writes for the usage-block builder (the text after ';'), as
// BambuStudio names them (GCodeProcessor.cpp:68-72).
constexpr const char *MachineStartGCodeEndMarker = " MACHINE_START_GCODE_END";
constexpr const char *MachineEndGCodeStartMarker = " MACHINE_END_GCODE_START";

// Input line id -> lines to write right after that line.
using InsertedLines = std::map<unsigned int, std::vector<std::string>>;

// True when the idle-nozzle pre-cooling applies to this print: a Bambu printer whose profile sets
// enable_pre_heating, with at least two extruders, and a filament -> nozzle grouping that agrees with
// the filament_map every printed filament is routed by. When it is false nothing about the G-code
// changes (no markers, no injected lines).
bool pre_cooling_active(const Print &print);

// Bambu's first-pass usage blocks (BambuStudio GCodeProcessor.hpp:379-441).
struct FilamentUsageBlock
{
    int          filament_id;
    int          extruder_id;
    int          nozzle_id;
    unsigned int lower_gcode_id;
    unsigned int upper_gcode_id; // [lower, upper) prints this filament; upper is set by the next block
    FilamentUsageBlock(int filament_id_, int extruder_id_, int nozzle_id_, unsigned int lower_gcode_id_, unsigned int upper_gcode_id_)
        : filament_id(filament_id_), extruder_id(extruder_id_), nozzle_id(nozzle_id_), lower_gcode_id(lower_gcode_id_), upper_gcode_id(upper_gcode_id_)
    {}
};

// The span an extruder is in use, delimited by two NOZZLE_CHANGE blocks. Post extrusion is the last
// extrusion before the switch to the other extruder (the nozzle-change block itself).
struct ExtruderUsageBlock
{
    int          extruder_id             = -1;
    unsigned int start_id                = (unsigned int) -1;
    unsigned int end_id                  = (unsigned int) -1;
    int          start_filament          = -1;
    int          end_filament            = -1;
    int          start_nozzle_id         = -1;
    int          end_nozzle_id           = -1;
    unsigned int post_extrusion_start_id = (unsigned int) -1;
    unsigned int post_extrusion_end_id   = (unsigned int) -1;
    bool         ignore_cooling_before_tower = false;

    void initialize_step_1(int extruder_id_, unsigned int start_id_, int start_filament_, int start_nozzle_id_)
    {
        extruder_id     = extruder_id_;
        start_id        = start_id_;
        start_filament  = start_filament_;
        start_nozzle_id = start_nozzle_id_;
    }
    void initialize_step_2(unsigned int post_extrusion_start_id_) { post_extrusion_start_id = post_extrusion_start_id_; }
    void initialize_step_3(unsigned int end_id_, int end_filament_, unsigned int post_extrusion_end_id_, int end_nozzle_id_)
    {
        end_id                = end_id_;
        end_filament          = end_filament_;
        post_extrusion_end_id = post_extrusion_end_id_;
        end_nozzle_id         = end_nozzle_id_;
    }
    void reset() { *this = ExtruderUsageBlock(); }
};

// Builds the usage blocks from the G-code, one line at a time (BambuStudio's first pass). Line ids
// are the post-processor's input line ids.
class UsageBlockBuilder
{
public:
    UsageBlockBuilder(const MultiNozzleUtils::LayeredNozzleGroupResult &group, std::string layer_change_tag);

    // line: one G-code line, with or without its EOL.
    void on_line(const std::string &line, unsigned int line_id);
    // Closes the open blocks after the last line.
    void finish();

    std::vector<FilamentUsageBlock>                    filament_blocks;
    std::vector<ExtruderUsageBlock>                    extruder_blocks;
    std::vector<std::pair<unsigned int, unsigned int>> skippable_blocks;
    unsigned int                                       machine_start_gcode_end_id = (unsigned int) -1;
    unsigned int                                       machine_end_gcode_start_id = (unsigned int) -1;

private:
    void handle_filament_change(int filament_id, unsigned int line_id, int nozzle_id);

    const MultiNozzleUtils::LayeredNozzleGroupResult &m_group;
    const std::string                                 m_layer_change_tag;
    ExtruderUsageBlock                                m_temp_block;
    int                                               m_layer_id = 0;
};

// One move of the G-code with the estimated print time at its end (cumulative, seconds).
struct TimedMove
{
    unsigned int gcode_id;
    float        time;
};

struct InjectorParams
{
    std::vector<int>    filament_nozzle_temps;         // per filament
    std::vector<int>    filament_pre_cooling_temps;    // per filament
    std::vector<double> filament_preheat_temperature_delta; // per filament
    std::vector<double> cooling_rate;                  // per logical extruder, degrees per second
    std::vector<double> heating_rate;                  // per logical extruder, degrees per second
    std::vector<int>    physical_extruder_map;         // logical -> physical extruder
    std::vector<int>    extruder_max_nozzle_count;     // per logical extruder
    std::vector<int>    extruder_types;                // per logical extruder (ExtruderType)
    std::vector<double> nozzle_diameter;
    // Bambu Studio's filament change time model (seconds), used for the moves' time at a T command.
    float               extruder_change_time      = 0.f; // machine_switch_extruder_time
    float               filament_load_time        = 0.f; // machine_load_filament_time
    float               filament_unload_time      = 0.f; // machine_unload_filament_time
    float               inject_time_threshold     = 0.f;
    bool                handle_hotend_as_extruder = false;
    bool                has_filament_switcher     = false;
};

class PreCoolingInjector
{
public:
    struct ExtruderFreeBlock
    {
        unsigned int free_lower_gcode_id;
        unsigned int free_upper_gcode_id;
        unsigned int partial_free_lower_id; // the extrusion on the tower (post extrusion); equal to free_lower_gcode_id without one
        unsigned int partial_free_upper_id;
        int          last_filament_id;
        int          next_filament_id;
        int          last_nozzle_id;
        int          next_nozzle_id;
        int          extruder_id;
        bool         ignore_cooling_before_tower = false;
    };

    PreCoolingInjector(const std::vector<TimedMove>                           &moves,
                       const MultiNozzleUtils::LayeredNozzleGroupResult        &group,
                       const InjectorParams                                   &params,
                       const std::vector<std::pair<unsigned int, unsigned int>> &skippable_blocks,
                       unsigned int                                            machine_start_gcode_end_id,
                       unsigned int                                            machine_end_gcode_start_id)
        : m_moves(moves)
        , m_group(group)
        , m_params(params)
        , m_skippable_blocks(skippable_blocks)
        , m_machine_start_gcode_end_id(machine_start_gcode_end_id)
        , m_machine_end_gcode_start_id(machine_end_gcode_start_id)
    {}

    void build_extruder_free_blocks(const std::vector<FilamentUsageBlock> &filament_blocks, const std::vector<ExtruderUsageBlock> &extruder_blocks);
    void process_pre_cooling_and_heating(InsertedLines &inserted_lines) const;

    const std::vector<ExtruderFreeBlock> &free_blocks() const { return m_free_blocks; }

private:
    void build_by_filament_blocks(const std::vector<FilamentUsageBlock> &filament_blocks);
    void build_by_extruder_blocks(const std::vector<ExtruderUsageBlock> &extruder_blocks);
    void inject_cooling_heating_command(InsertedLines &inserted_lines, const ExtruderFreeBlock &block, float curr_temp, float target_temp,
                                        bool pre_cooling, bool pre_heating) const;

    std::vector<ExtruderFreeBlock>                            m_free_blocks;
    const std::vector<TimedMove>                             &m_moves;
    const MultiNozzleUtils::LayeredNozzleGroupResult          &m_group;
    const InjectorParams                                     &m_params;
    const std::vector<std::pair<unsigned int, unsigned int>> &m_skippable_blocks;
    const unsigned int                                        m_machine_start_gcode_end_id;
    const unsigned int                                        m_machine_end_gcode_start_id;
};

// Everything the post-processor needs, derived from the print; built once per export.
struct Plan
{
    std::shared_ptr<MultiNozzleUtils::LayeredNozzleGroupResult> group;
    InjectorParams                                              params;
};

// Returns false (and leaves plan alone) when pre_cooling_active(print) is false.
bool make_plan(const Print &print, Plan &plan);

} // namespace PreCooling
} // namespace Slic3r

#endif // slic3r_GCode_PreCoolingInjector_hpp_
