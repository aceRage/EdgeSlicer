#include "ThirdPartyFilamentMap.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace Slic3r {
namespace GUI {

namespace {

char hex_digit(unsigned v)
{
    return v < 10 ? static_cast<char>('0' + v) : static_cast<char>('A' + (v - 10));
}

bool hex_nibble(char c, int& out)
{
    if (c >= '0' && c <= '9') {
        out = c - '0';
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        out = c - 'a' + 10;
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        out = c - 'A' + 10;
        return true;
    }
    return false;
}

bool rgb_of(const std::string& colour, int& r, int& g, int& b)
{
    const std::string hex = normalize_map_colour(colour);
    if (hex.size() != 7)
        return false;
    int v[6];
    for (int i = 0; i < 6; ++i) {
        if (!hex_nibble(hex[i + 1], v[i]))
            return false;
    }
    r = v[0] * 16 + v[1];
    g = v[2] * 16 + v[3];
    b = v[4] * 16 + v[5];
    return true;
}

std::string lower_copy(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::string normalize_map_colour(const std::string& colour)
{
    std::string s = colour;
    if (!s.empty() && s[0] == '#')
        s = s.substr(1);
    if (s.size() < 6)
        return {};
    int dummy;
    for (int i = 0; i < 6; ++i) {
        if (!hex_nibble(s[i], dummy))
            return {};
    }
    std::string out = "#";
    out.resize(7);
    out[0] = '#';
    for (int i = 0; i < 6; ++i) {
        int n = 0;
        hex_nibble(s[i], n);
        out[i + 1] = hex_digit(static_cast<unsigned>(n));
    }
    return out;
}

std::string bambu_style_slot_label(int unit_id, int slot_id)
{
    const int slot = slot_id < 0 ? 0 : slot_id;
    const int letter_i = slot % 4;
    const char letter  = static_cast<char>('A' + letter_i);
    if (unit_id < 0)
        return std::string(1, letter);
    return std::to_string(unit_id + 1) + letter;
}

std::string slot_display_label(const ThirdPartyDeviceSlot& slot)
{
    if (!slot.label.empty())
        return slot.label;
    return bambu_style_slot_label(slot.unit_id, slot.slot_id >= 0 ? slot.slot_id : slot.index);
}

bool types_match(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty())
        return true;
    return lower_copy(a) == lower_copy(b);
}

double map_colour_distance(const std::string& a, const std::string& b)
{
    int ar, ag, ab, br, bg, bb;
    if (!rgb_of(a, ar, ag, ab) || !rgb_of(b, br, bg, bb))
        return 0;
    const double rm = (ar + br) / 2.0;
    const double dr = ar - br, dg = ag - bg, db = ab - bb;
    return (2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db;
}

bool colours_mismatch(const std::string& a, const std::string& b)
{
    if (normalize_map_colour(a).empty() || normalize_map_colour(b).empty())
        return false;
    // Red vs a nearby red is a few thousand; red vs blue is tens of thousands. 8000 is
    // "noticeably different" without flagging two spools of the same shade.
    return map_colour_distance(a, b) > 8000.0;
}

ThirdPartyMapMismatch mismatch_for(const ThirdPartySlicerFilament& filament, const ThirdPartyDeviceSlot& slot)
{
    const bool type_bad   = !slot.empty && !types_match(filament.type, slot.type);
    const bool colour_bad = !slot.empty && colours_mismatch(filament.colour, slot.colour);
    if (type_bad && colour_bad)
        return ThirdPartyMapMismatch::TypeAndColour;
    if (type_bad)
        return ThirdPartyMapMismatch::Type;
    if (colour_bad)
        return ThirdPartyMapMismatch::Colour;
    return ThirdPartyMapMismatch::None;
}

ThirdPartyMapMismatch mismatch_at(const ThirdPartyFilamentMapModel& model, size_t filament_row)
{
    if (filament_row >= model.filaments.size() || filament_row >= model.mapping.size())
        return ThirdPartyMapMismatch::None;
    const int pos = model.mapping[filament_row];
    if (pos < 0 || pos >= (int) model.slots.size())
        return ThirdPartyMapMismatch::None;
    return mismatch_for(model.filaments[filament_row], model.slots[pos]);
}

std::vector<int> distinct_nozzle_ids(const ThirdPartyFilamentMapModel& model)
{
    std::set<int> ids;
    for (const ThirdPartySlicerFilament& f : model.filaments)
        if (f.nozzle_id >= 0)
            ids.insert(f.nozzle_id);
    for (const ThirdPartyDeviceSlot& s : model.slots)
        if (s.nozzle_id >= 0)
            ids.insert(s.nozzle_id);
    return std::vector<int>(ids.begin(), ids.end());
}

bool has_multiple_nozzles(const ThirdPartyFilamentMapModel& model)
{
    return distinct_nozzle_ids(model).size() > 1;
}

std::string filament_badge_text(const ThirdPartySlicerFilament& filament, const ThirdPartyFilamentMapModel& model)
{
    const std::vector<int> nozzles = distinct_nozzle_ids(model);
    if (filament.nozzle_id >= 0 && nozzles.size() > 1) {
        if (nozzles.size() == 2 && nozzles[0] == 0 && nozzles[1] == 1) {
            // Bambu H2D: physical 0 is the right nozzle, 1 is the left.
            if (filament.nozzle_id == 0)
                return "R";
            if (filament.nozzle_id == 1)
                return "L";
        }
        return std::to_string(filament.nozzle_id + 1);
    }
    return std::to_string(filament.index + 1);
}

std::vector<ThirdPartySlotGroup> group_slots(const std::vector<ThirdPartyDeviceSlot>& slots)
{
    std::vector<ThirdPartySlotGroup> groups;
    for (size_t i = 0; i < slots.size(); ++i) {
        const ThirdPartyDeviceSlot& s = slots[i];
        auto it = std::find_if(groups.begin(), groups.end(), [&](const ThirdPartySlotGroup& g) {
            return g.unit_id == s.unit_id && g.nozzle_id == s.nozzle_id;
        });
        if (it == groups.end()) {
            ThirdPartySlotGroup g;
            g.unit_id   = s.unit_id;
            g.nozzle_id = s.nozzle_id;
            g.slot_positions.push_back((int) i);
            groups.push_back(std::move(g));
        } else {
            it->slot_positions.push_back((int) i);
        }
    }
    return groups;
}

std::string encode_filament_mapping(const ThirdPartyFilamentMapModel& model)
{
    std::string out;
    for (size_t i = 0; i < model.filaments.size() && i < model.mapping.size(); ++i) {
        const int pos = model.mapping[i];
        if (pos < 0 || pos >= (int) model.slots.size())
            continue;
        if (!out.empty())
            out += ",";
        out += std::to_string(model.filaments[i].index) + ":" + std::to_string(model.slots[pos].index);
    }
    return out;
}

} // namespace GUI
} // namespace Slic3r
