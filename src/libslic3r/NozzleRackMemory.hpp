#ifndef slic3r_NozzleRackMemory_hpp_
#define slic3r_NozzleRackMemory_hpp_

// The project's nozzles remembered per printer between sessions (owner bugs 2026-11-06: an H2C opened with its whole rack on the
// variant's size, an H3C with every slot 0.4 Standard, on every start, whatever was set last).
//
// What the nozzle strip, the Extruder settings dialog and "Sync from printer" set is project state (P7 nozzle_diameter_override,
// nozzle_volume_type, P9 extruder_rack_nozzle_override). The project config is not kept between sessions, so the GUI keeps a
// copy in the app config: section "nozzle_rack", key = the printer preset's name (and section "nozzle_rack_model", key =
// printer_model, the last rack of any of the model's presets), written on every edit and sync and put back when the printer
// model is selected again (startup included), when it still fits the printer.
//
// Only the text form and the fit check live here, so they can be tested without the GUI.

#include "PrintConfig.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;

struct NozzleRackMemory
{
    std::vector<double>      diameters;    // the size each extruder carries (the preset's size with the project override applied)
    std::vector<int>         volume_types; // the project's nozzle_volume_type per extruder (NozzleVolumeType values)
    std::vector<std::string> rack_texts;   // extruder_rack_nozzle_override per extruder ("" = the printer preset's rack)

    bool empty() const { return diameters.empty(); }
    bool operator==(const NozzleRackMemory &rhs) const
    {
        return diameters == rhs.diameters && volume_types == rhs.volume_types && rack_texts == rhs.rack_texts;
    }
    bool operator!=(const NozzleRackMemory &rhs) const { return !(*this == rhs); }
};

// App config text: {"diameters":[0.4,0.6],"types":[0,1],"racks":["","0.4,0.6:high_flow"]}. Empty for an empty memory.
std::string serialize_nozzle_rack_memory(const NozzleRackMemory &memory);
// False (and `out` empty) when the text is empty or not a rack memory.
bool parse_nozzle_rack_memory(const std::string &text, NozzleRackMemory &out);

// "Direct Drive TPU285" -> nvtTPU285 (the longest flow name an extruder_variant_list entry ends with); false when none.
bool nozzle_type_of_extruder_variant(const std::string &variant, NozzleVolumeType &out);

// What a printer model takes: per extruder its rack capacity and its nozzle types, and the model's nozzle sizes.
struct NozzleRackLimits
{
    std::vector<int>                           capacity; // extruder_max_nozzle_count per extruder (<= 1: no rack)
    std::vector<double>                        sizes;    // the sizes the model's nozzles come in (empty = any)
    std::vector<std::vector<NozzleVolumeType>> types;    // per extruder (Standard always fits)
};
// One printer preset's limits: its extruders, extruder_max_nozzle_count, the types its extruder_variant_list and
// default_nozzle_volume_type name (a default list with one entry per rack slot is read per extruder), and its printer_variant
// sizes (nozzle_size_choices: the standard four sizes too).
NozzleRackLimits nozzle_rack_limits(const ConfigBase &printer);
// Adds another variant of the same model (same extruder count): its sizes and types. A variant with another extruder count is
// another machine layout and is skipped.
void merge_nozzle_rack_limits(NozzleRackLimits &into, const ConfigBase &variant);
// The types extruder `extruder` takes, Standard first, then in enum order.
std::vector<NozzleVolumeType> nozzle_rack_types(const NozzleRackLimits &limits, size_t extruder);

// Why `memory` does not fit `limits` (another extruder count, a rack on an extruder without one or with more nozzles than slots,
// a size or type the model has no nozzle of, an unreadable rack text), or "" when it fits.
std::string validate_nozzle_rack_memory(const NozzleRackMemory &memory, const NozzleRackLimits &limits);

} // namespace Slic3r

#endif // slic3r_NozzleRackMemory_hpp_
