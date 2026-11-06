#ifndef slic3r_NozzleStripModel_hpp_
#define slic3r_NozzleStripModel_hpp_

// The nozzle strip of the Prepare sidebar, the Extruder Settings dialog and the Process tab's chips
// (tests/design_bambu29_mixed_nozzle_compare.md section 4): what they show, worked out from the printer config with the
// project's sizes (PresetBundle::printer_config_with_project_sizes()) so it can be tested without a GUI.
//
// One tile per physical nozzle: U1 T1..T4, H2D / X2D L R, H2C L then R1..R6, an H3C-like single extruder with a rack 1..9.
// A rack extruder shows one tile per rack position (extruder_max_nozzle_count), positions the project's rack text does not
// fill are empty. Edits are expressed as the project keys P7 / P9 write (nozzle_diameter_override, nozzle_volume_type,
// extruder_rack_nozzle_override), so the strip is only another way to set them. Nothing here changes slicing.

#include "PrintConfig.hpp"
#include "ToolSlots.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;

enum class NozzleStripNaming {
    Sides,   // Bambu's two-extruder machines: L, R (rack: R1..R6)
    Tools,   // toolchangers and other multi-extruder printers: T1..Tn (rack: T2.1..)
    Numbers, // one extruder with a nozzle rack (H3C): 1..n
};

// What the strip is made from: per extruder its size, flow type, rack and rack capacity.
struct NozzleStripInput
{
    std::vector<double>                  diameters;    // the project's size per extruder
    std::vector<NozzleVolumeType>        volume_types; // per extruder (nozzle_volume_type)
    std::vector<std::vector<RackNozzle>> racks;        // the project's rack per extruder (empty: none written)
    std::vector<int>                     capacity;     // rack positions per extruder (extruder_max_nozzle_count), <= 1 = no rack
    NozzleStripNaming                    naming{ NozzleStripNaming::Tools };
};

// Reads the input from a printer config with the project's sizes applied. `sides`: Bambu's two-extruder naming.
NozzleStripInput nozzle_strip_input(const ConfigBase &sized_printer, bool sides);

// The project's flow types (they live in the project config, not the printer's): rack nozzles without a flow type of their own
// follow their extruder's.
void nozzle_strip_set_volume_types(NozzleStripInput &in, const std::vector<NozzleVolumeType> &types);
// Whether extruder `e` shows rack positions.
bool nozzle_strip_has_rack(const NozzleStripInput &in, size_t e);
// The rack of extruder `e` as the strip edits it: the project's rack when it lists a nozzle, else the extruder's own nozzle.
std::vector<RackNozzle> nozzle_strip_rack(const NozzleStripInput &in, size_t e);

struct NozzleStripTile
{
    size_t                    extruder{ 0 };
    int                       slot{ 0 };     // 1-based rack position, 0 = the extruder's own nozzle (no rack)
    std::string               label;         // "L", "R3", "T2", "7"
    double                    diameter{ 0. }; // 0 = an empty rack position
    NozzleVolumeType          volume_type{ nvtStandard };
    bool                      divider_before{ false }; // first tile of an extruder after another one (racks only)
    std::vector<unsigned int> filaments;     // 0-based filaments of the plate that print on this nozzle
    bool                      usable{ true };
    std::string               reason;        // why it is not usable
    bool                      empty() const { return !(diameter > 0.); }
    bool                      used() const { return !filaments.empty(); }
};

// The tiles in strip order. Empty rack positions are not usable ("Empty slot").
std::vector<NozzleStripTile> nozzle_strip_tiles(const NozzleStripInput &in);
// The strip replaces the per-nozzle notebook when there are two or more tiles.
bool nozzle_strip_applies(const NozzleStripInput &in);

// Where a filament prints, as the sidebar knows it: its extruder (-1 = not decided yet, e.g. Auto before slicing), a rack
// position it is pinned to (0 = none) and a rack size it is pinned to (0 = none).
struct NozzleStripPlace
{
    int    extruder{ -1 };
    int    slot_pin{ 0 };
    double size_pin{ 0. };
};
// Puts each used filament (0-based, indexes into `places`) on its tile: an extruder without a rack -> its tile; a rack ->
// the pinned position, else the first position with the pinned size (or the extruder's own size), else the first nozzle.
void place_nozzle_strip_filaments(std::vector<NozzleStripTile> &tiles, const NozzleStripInput &in, const std::vector<NozzleStripPlace> &places,
                                  const std::vector<unsigned int> &used_filaments);

// "Std", "HF", "TPU", "TPU285", "E3D".
std::string nozzle_flow_short(NozzleVolumeType type);
// Whether the printer declares nozzles of `type` (its default_nozzle_volume_type / nozzle_volume_type lists or its rack),
// so the Extruder Settings type list offers it (the H3C's TPU285 slot).
bool nozzle_strip_printer_has_type(const ConfigBase &printer, NozzleVolumeType type);
// "R1-R5", "T3-T4", "R1, R3", "1-3": labels that share a prefix and run in sequence are shortened.
std::string collapse_nozzle_strip_labels(const std::vector<std::string> &labels);

// One chip of the Process tab: a tool of the selector (an extruder, or a size of its rack) with the tiles it covers.
struct NozzleChip
{
    std::string size;  // "0.4"
    std::string flow;  // "Std", "Std/HF"
    std::string tiles; // "R1-R5"
};
// The chip of the selector tool (extruder, size): the non-empty tiles of `extruder` with that size.
NozzleChip nozzle_chip_for(const std::vector<NozzleStripTile> &tiles, size_t extruder, double diameter);

// A set of edits in the project keys. Starts as the input; nozzle_strip_edit_* change it.
struct NozzleStripEdit
{
    std::vector<double>           diameters;
    std::vector<NozzleVolumeType> volume_types;
    std::vector<std::string>      rack_texts; // per extruder; only extruders with a rack are written
    std::vector<bool>             rack_written;
};
NozzleStripEdit nozzle_strip_edit_start(const NozzleStripInput &in);
// "0.4,0.4,0.6:high_flow": entries whose flow type differs from `extruder_type` carry it.
std::string format_rack_nozzles(const std::vector<RackNozzle> &rack, NozzleVolumeType extruder_type);
// Sets extruder `e`'s whole rack (rack extruders) or its nozzle (others; `rack` has one entry). An empty rack is refused. The
// extruder's own size stays when the rack still holds it, else it becomes the first nozzle's (the mounted one after a sync);
// its flow type is that of the first nozzle of its size. Returns "" or what is wrong.
std::string nozzle_strip_edit_set_rack(NozzleStripEdit &edit, const NozzleStripInput &in, size_t e, std::vector<RackNozzle> rack);
// One tile: `diameter` 0 removes a rack nozzle (not the last one); an empty position is filled at the end of the rack.
std::string nozzle_strip_edit_tile(NozzleStripEdit &edit, const NozzleStripInput &in, const NozzleStripTile &tile, double diameter,
                                   NozzleVolumeType type);

// Filaments pinned to a rack size or position the edited racks no longer have (the dialog's confirm before applying):
// 0-based filament indexes. `size_pins` / `slot_pins` per filament, `filament_extruder` per filament (-1 = unknown).
std::vector<unsigned int> nozzle_strip_orphaned_pins(const NozzleStripEdit &edit, const NozzleStripInput &in,
                                                     const std::vector<int> &filament_extruder, const std::vector<double> &size_pins,
                                                     const std::vector<int> &slot_pins);

} // namespace Slic3r

#endif // slic3r_NozzleStripModel_hpp_
