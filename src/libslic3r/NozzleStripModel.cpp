#include "NozzleStripModel.hpp"

#include "Config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace Slic3r {

namespace {

int strip_capacity_of(const ConfigBase &config, size_t e)
{
    const ConfigOption *opt = config.option("extruder_max_nozzle_count");
    if (const auto *counts = dynamic_cast<const ConfigOptionIntsNullable *>(opt))
        return e < counts->values.size() && !counts->is_nil(e) ? counts->values[e] : 1;
    if (const auto *counts = dynamic_cast<const ConfigOptionInts *>(opt))
        return e < counts->values.size() ? counts->values[e] : 1;
    return -1; // not declared
}

std::string strip_label_prefix(const std::string &label, int &number)
{
    size_t i = label.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(label[i - 1])))
        --i;
    if (i == label.size()) {
        number = -1;
        return label;
    }
    number = std::atoi(label.c_str() + i);
    return label.substr(0, i);
}

bool strip_rack_has_size(const std::vector<RackNozzle> &rack, double d)
{
    return std::any_of(rack.begin(), rack.end(), [d](const RackNozzle &n) { return same_nozzle_diameter(n.diameter, d); });
}

} // namespace

NozzleStripInput nozzle_strip_input(const ConfigBase &sized_printer, bool sides)
{
    NozzleStripInput in;
    if (const auto *nd = sized_printer.option<ConfigOptionFloats>("nozzle_diameter"))
        in.diameters = nd->values;
    const size_t n = in.diameters.size();
    in.volume_types.assign(n, nvtStandard);
    if (const auto *opt = dynamic_cast<const ConfigOptionEnumsGeneric *>(sized_printer.option("nozzle_volume_type")))
        for (size_t e = 0; e < n && e < opt->values.size(); ++e)
            in.volume_types[e] = opt->values[e] == nvtHybrid ? nvtStandard : NozzleVolumeType(opt->values[e]);
    in.racks = extruder_rack_nozzles(sized_printer);
    in.racks.resize(n);
    in.capacity.assign(n, 1);
    for (size_t e = 0; e < n; ++e) {
        const int declared = strip_capacity_of(sized_printer, e);
        // A config without the key (tests, custom printers) declares a rack through its rack text only.
        in.capacity[e] = declared >= 0 ? declared : (in.racks[e].size() >= 2 ? int(in.racks[e].size()) : 1);
        // A rack of another printer left in the project (a U1 after an H2C project) is not this printer's.
        if (in.capacity[e] <= 1)
            in.racks[e].clear();
    }
    if (n == 2 && sides)
        in.naming = NozzleStripNaming::Sides;
    else if (n == 1)
        in.naming = NozzleStripNaming::Numbers;
    else
        in.naming = NozzleStripNaming::Tools;
    return in;
}

void nozzle_strip_set_volume_types(NozzleStripInput &in, const std::vector<NozzleVolumeType> &types)
{
    for (size_t e = 0; e < in.volume_types.size() && e < types.size(); ++e) {
        in.volume_types[e] = types[e] == nvtHybrid ? nvtStandard : types[e];
        if (e < in.racks.size())
            for (RackNozzle &n : in.racks[e])
                if (!n.volume_given)
                    n.volume_type = in.volume_types[e];
    }
}

bool nozzle_strip_has_rack(const NozzleStripInput &in, size_t e) { return e < in.capacity.size() && in.capacity[e] > 1; }

std::vector<RackNozzle> nozzle_strip_rack(const NozzleStripInput &in, size_t e)
{
    if (e < in.racks.size() && !in.racks[e].empty())
        return in.racks[e];
    RackNozzle own;
    own.diameter    = e < in.diameters.size() ? in.diameters[e] : 0.;
    own.volume_type = e < in.volume_types.size() ? in.volume_types[e] : nvtStandard;
    return { own };
}

static std::string strip_extruder_prefix(const NozzleStripInput &in, size_t e)
{
    switch (in.naming) {
    case NozzleStripNaming::Sides: return e == 0 ? "L" : "R";
    case NozzleStripNaming::Numbers: return std::string();
    default: return "T" + std::to_string(e + 1);
    }
}

std::vector<NozzleStripTile> nozzle_strip_tiles(const NozzleStripInput &in)
{
    std::vector<NozzleStripTile> tiles;
    bool any_rack = false;
    for (size_t e = 0; e < in.diameters.size(); ++e)
        any_rack |= nozzle_strip_has_rack(in, e);
    for (size_t e = 0; e < in.diameters.size(); ++e) {
        const std::string prefix = strip_extruder_prefix(in, e);
        if (!nozzle_strip_has_rack(in, e)) {
            NozzleStripTile t;
            t.extruder       = e;
            t.label          = prefix.empty() ? std::to_string(e + 1) : prefix;
            t.diameter       = in.diameters[e];
            t.volume_type    = e < in.volume_types.size() ? in.volume_types[e] : nvtStandard;
            t.divider_before = any_rack && e > 0;
            tiles.push_back(std::move(t));
            continue;
        }
        const std::vector<RackNozzle> rack  = nozzle_strip_rack(in, e);
        const size_t                  count = std::max(size_t(in.capacity[e]), rack.size());
        for (size_t k = 0; k < count; ++k) {
            NozzleStripTile t;
            t.extruder = e;
            t.slot     = int(k) + 1;
            // "T2.3" keeps the tool and the position apart; L / R and plain numbers need no separator.
            t.label          = prefix + (in.naming == NozzleStripNaming::Tools ? "." : "") + std::to_string(k + 1);
            t.divider_before = k == 0 && e > 0;
            if (k < rack.size()) {
                t.diameter    = rack[k].diameter;
                t.volume_type = rack[k].volume_type;
            } else {
                t.usable = false;
                t.reason = "Empty slot";
            }
            tiles.push_back(std::move(t));
        }
    }
    return tiles;
}

bool nozzle_strip_applies(const NozzleStripInput &in) { return nozzle_strip_tiles(in).size() >= 2; }

void place_nozzle_strip_filaments(std::vector<NozzleStripTile> &tiles, const NozzleStripInput &in, const std::vector<NozzleStripPlace> &places,
                                  const std::vector<unsigned int> &used_filaments)
{
    for (NozzleStripTile &t : tiles)
        t.filaments.clear();
    for (unsigned int f : used_filaments) {
        if (f >= places.size() || places[f].extruder < 0)
            continue;
        const size_t           e = size_t(places[f].extruder);
        std::vector<size_t>    own;
        for (size_t i = 0; i < tiles.size(); ++i)
            if (tiles[i].extruder == e && !tiles[i].empty())
                own.push_back(i);
        if (own.empty())
            continue;
        size_t pick = own.front();
        if (own.size() > 1) {
            const auto with_slot = std::find_if(own.begin(), own.end(), [&](size_t i) { return tiles[i].slot == places[f].slot_pin; });
            if (places[f].slot_pin > 0 && with_slot != own.end()) {
                pick = *with_slot;
            } else {
                const double size = places[f].size_pin > 0. ? places[f].size_pin : (e < in.diameters.size() ? in.diameters[e] : 0.);
                const auto   it   = std::find_if(own.begin(), own.end(), [&](size_t i) { return same_nozzle_diameter(tiles[i].diameter, size); });
                if (it != own.end())
                    pick = *it;
            }
        }
        tiles[pick].filaments.push_back(f);
    }
}

std::string nozzle_flow_short(NozzleVolumeType type)
{
    switch (type) {
    case nvtHighFlow: return "HF";
    case nvtTPUHighFlow: return "TPU";
    case nvtE3DHighFlow: return "E3D";
    default: return "Std";
    }
}

std::string collapse_nozzle_strip_labels(const std::vector<std::string> &labels)
{
    std::string out;
    for (size_t i = 0; i < labels.size();) {
        int               first = 0;
        const std::string prefix = strip_label_prefix(labels[i], first);
        size_t            j      = i;
        if (first >= 0)
            while (j + 1 < labels.size()) {
                int next = 0;
                if (strip_label_prefix(labels[j + 1], next) != prefix || next != first + int(j + 1 - i))
                    break;
                ++j;
            }
        if (!out.empty())
            out += ", ";
        out += j > i ? labels[i] + "-" + labels[j] : labels[i];
        i = j + 1;
    }
    return out;
}

NozzleChip nozzle_chip_for(const std::vector<NozzleStripTile> &tiles, size_t extruder, double diameter)
{
    NozzleChip               chip;
    std::vector<std::string> labels, flows;
    for (const NozzleStripTile &t : tiles)
        if (t.extruder == extruder && !t.empty() && same_nozzle_diameter(t.diameter, diameter)) {
            labels.push_back(t.label);
            const std::string flow = nozzle_flow_short(t.volume_type);
            if (std::find(flows.begin(), flows.end(), flow) == flows.end())
                flows.push_back(flow);
        }
    chip.size  = format_nozzle_diameter_word(diameter);
    chip.tiles = collapse_nozzle_strip_labels(labels);
    for (const std::string &f : flows)
        chip.flow += (chip.flow.empty() ? "" : "/") + f;
    if (chip.flow.empty())
        chip.flow = "Std";
    return chip;
}

NozzleStripEdit nozzle_strip_edit_start(const NozzleStripInput &in)
{
    NozzleStripEdit edit;
    edit.diameters    = in.diameters;
    edit.volume_types = in.volume_types;
    edit.volume_types.resize(in.diameters.size(), nvtStandard);
    edit.rack_texts.assign(in.diameters.size(), std::string());
    edit.rack_written.assign(in.diameters.size(), false);
    return edit;
}

static const char *strip_volume_word(NozzleVolumeType type)
{
    switch (type) {
    case nvtHighFlow: return "high_flow";
    case nvtTPUHighFlow: return "tpu_high_flow";
    case nvtE3DHighFlow: return "e3d_high_flow";
    default: return "standard";
    }
}

std::string format_rack_nozzles(const std::vector<RackNozzle> &rack, NozzleVolumeType extruder_type)
{
    std::string out;
    for (const RackNozzle &n : rack) {
        if (!out.empty())
            out += ",";
        out += format_nozzle_diameter_word(n.diameter);
        if (n.volume_type != extruder_type)
            out += std::string(":") + strip_volume_word(n.volume_type);
    }
    return out;
}

std::string nozzle_strip_edit_set_rack(NozzleStripEdit &edit, const NozzleStripInput &in, size_t e, std::vector<RackNozzle> rack)
{
    if (e >= edit.diameters.size())
        return "no such extruder";
    rack.erase(std::remove_if(rack.begin(), rack.end(), [](const RackNozzle &n) { return !(n.diameter > 0.); }), rack.end());
    if (rack.empty())
        return "an extruder needs a nozzle";
    if (!nozzle_strip_has_rack(in, e)) {
        edit.diameters[e]    = rack.front().diameter;
        edit.volume_types[e] = rack.front().volume_type;
        return std::string();
    }
    if (int(rack.size()) > in.capacity[e])
        return "the rack holds " + std::to_string(in.capacity[e]) + " nozzles";
    if (!strip_rack_has_size(rack, edit.diameters[e]))
        edit.diameters[e] = rack.front().diameter;
    for (const RackNozzle &n : rack)
        if (same_nozzle_diameter(n.diameter, edit.diameters[e])) {
            edit.volume_types[e] = n.volume_type;
            break;
        }
    for (RackNozzle &n : rack)
        n.volume_given = n.volume_type != edit.volume_types[e];
    const std::string text  = format_rack_nozzles(rack, edit.volume_types[e]);
    const std::string error = validate_rack_nozzle_text(text, edit.diameters[e]);
    if (!error.empty())
        return error;
    edit.rack_texts[e]   = text;
    edit.rack_written[e] = true;
    return std::string();
}

std::string nozzle_strip_edit_tile(NozzleStripEdit &edit, const NozzleStripInput &in, const NozzleStripTile &tile, double diameter,
                                   NozzleVolumeType type)
{
    const size_t e = tile.extruder;
    if (e >= edit.diameters.size())
        return "no such extruder";
    if (tile.slot == 0 || !nozzle_strip_has_rack(in, e)) {
        if (!(diameter > 0.))
            return "an extruder needs a nozzle";
        RackNozzle n;
        n.diameter    = diameter;
        n.volume_type = type;
        return nozzle_strip_edit_set_rack(edit, in, e, { n });
    }
    // The rack as already edited, else as the project has it.
    std::vector<RackNozzle> rack = edit.rack_written[e] ? parse_rack_nozzles(edit.rack_texts[e]) : nozzle_strip_rack(in, e);
    if (edit.rack_written[e])
        for (RackNozzle &n : rack)
            if (!n.volume_given)
                n.volume_type = edit.volume_types[e];
    const size_t k = size_t(tile.slot - 1);
    if (!(diameter > 0.)) {
        if (k >= rack.size())
            return std::string(); // already empty
        if (rack.size() == 1)
            return "an extruder needs a nozzle";
        rack.erase(rack.begin() + k);
    } else {
        RackNozzle n;
        n.diameter    = diameter;
        n.volume_type = type;
        if (k < rack.size())
            rack[k] = n;
        else
            rack.push_back(n); // positions are filled in order
    }
    return nozzle_strip_edit_set_rack(edit, in, e, rack);
}

std::vector<unsigned int> nozzle_strip_orphaned_pins(const NozzleStripEdit &edit, const NozzleStripInput &in,
                                                     const std::vector<int> &filament_extruder, const std::vector<double> &size_pins,
                                                     const std::vector<int> &slot_pins)
{
    std::vector<unsigned int> out;
    const size_t              n = std::max(size_pins.size(), slot_pins.size());
    for (size_t f = 0; f < n; ++f) {
        const double size = f < size_pins.size() ? size_pins[f] : 0.;
        const int    slot = f < slot_pins.size() ? slot_pins[f] : 0;
        if (!(size > 0.) && slot <= 0)
            continue;
        const int e = f < filament_extruder.size() ? filament_extruder[f] : -1;
        if (e < 0 || size_t(e) >= edit.diameters.size() || !nozzle_strip_has_rack(in, size_t(e)))
            continue;
        std::vector<RackNozzle> rack = edit.rack_written[size_t(e)] ? parse_rack_nozzles(edit.rack_texts[size_t(e)]) : nozzle_strip_rack(in, size_t(e));
        if ((size > 0. && !strip_rack_has_size(rack, size)) || (slot > 0 && size_t(slot) > rack.size()))
            out.push_back(unsigned(f));
    }
    return out;
}

} // namespace Slic3r
