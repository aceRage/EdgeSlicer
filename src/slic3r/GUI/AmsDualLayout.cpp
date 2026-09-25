#include "AmsDualLayout.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>

namespace Slic3r { namespace GUI { namespace AmsDual {

uint32_t flag_bits_no_border(const std::string &str, int start, int count)
{
    if (start < 0 || count <= 0)
        return 0;

    std::string hex;
    hex.reserve(str.size());
    size_t i = 0;
    while (i < str.size() && std::isspace(static_cast<unsigned char>(str[i])))
        ++i;
    if (i + 1 < str.size() && str[i] == '0' && (str[i + 1] == 'x' || str[i + 1] == 'X'))
        i += 2;
    for (; i < str.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(str[i]);
        if (std::isxdigit(c))
            hex.push_back(char(c));
    }
    if (hex.empty())
        return 0;

    auto nibble = [](char c) -> uint32_t {
        if (c >= '0' && c <= '9') return uint32_t(c - '0');
        if (c >= 'a' && c <= 'f') return uint32_t(c - 'a' + 10);
        return uint32_t(c - 'A' + 10);
    };

    const size_t total_bits = hex.size() * 4;
    const int    need       = std::min(count, 32);
    uint32_t     value      = 0;
    for (int b = 0; b < need; ++b) {
        const size_t bit = size_t(start) + size_t(b);
        if (bit >= total_bits)
            break;
        const size_t digit_from_right = bit / 4;
        const char   digit            = hex[hex.size() - 1 - digit_from_right];
        if ((nibble(digit) >> (bit % 4)) & 1u)
            value |= (1u << b);
    }
    return value;
}

UnitInfoBits parse_unit_info(const std::string &info_hex)
{
    UnitInfoBits bits;
    bits.type           = int(flag_bits_no_border(info_hex, 0, 4));
    bits.dry_status     = int(flag_bits_no_border(info_hex, 4, 4));
    bits.extruder_id    = int(flag_bits_no_border(info_hex, 8, 4));
    bits.dry_fan1       = int(flag_bits_no_border(info_hex, 18, 2));
    bits.dry_fan2       = int(flag_bits_no_border(info_hex, 20, 2));
    bits.dry_sub_status = int(flag_bits_no_border(info_hex, 22, 2));
    return bits;
}

bool fun2_supports_remote_dry(const std::string &fun2_hex) { return flag_bits_no_border(fun2_hex, 5) == 1; }

bool is_virtual_slot(const std::string &ams_id)
{
    return ams_id == std::to_string(VIRTUAL_SLOT_MAIN) || ams_id == std::to_string(VIRTUAL_SLOT_DEPUTY);
}

int extruder_of_virtual_slot(int virtual_slot_id, int extruder_count)
{
    if (extruder_count >= 2 && virtual_slot_id == VIRTUAL_SLOT_DEPUTY)
        return DEPUTY_EXTRUDER;
    return MAIN_EXTRUDER;
}

static long numeric_id(const std::string &id)
{
    char *end = nullptr;
    const long v = std::strtol(id.c_str(), &end, 10);
    return (end == id.c_str()) ? std::numeric_limits<long>::max() : v;
}

std::vector<Side> group_units(const std::vector<UnitRef> &units, int extruder_count)
{
    std::vector<Side> sides;
    if (extruder_count >= 2) {
        sides.resize(2);
        sides[0].extruder_id = DEPUTY_EXTRUDER;
        sides[1].extruder_id = MAIN_EXTRUDER;
    } else {
        sides.resize(1);
        sides[0].extruder_id = MAIN_EXTRUDER;
    }

    auto side_index = [&](int extruder_id) -> int {
        for (size_t i = 0; i < sides.size(); ++i)
            if (sides[i].extruder_id == extruder_id)
                return int(i);
        // A unit that names an extruder this machine does not have: show it with the main one.
        return int(sides.size()) - 1;
    };

    for (const UnitRef &u : units) {
        UnitRef unit = u;
        if (is_virtual_slot(unit.ams_id)) {
            unit.type        = UNIT_EXT_SPOOL;
            unit.slot_count  = 1;
            unit.extruder_id = extruder_of_virtual_slot(std::atoi(unit.ams_id.c_str()), extruder_count);
        }
        if (unit.slot_count < 1)
            unit.slot_count = 1;
        sides[side_index(unit.extruder_id)].units.push_back(unit);
    }

    for (Side &side : sides) {
        // Bambu keeps AMS units in numeric id order (NumericStrCompare); the spool holder comes last.
        std::stable_sort(side.units.begin(), side.units.end(), [](const UnitRef &a, const UnitRef &b) {
            const bool a_ext = a.type == UNIT_EXT_SPOOL, b_ext = b.type == UNIT_EXT_SPOOL;
            if (a_ext != b_ext)
                return !a_ext;
            return numeric_id(a.ams_id) < numeric_id(b.ams_id);
        });

        std::vector<int> pending_single;
        for (int i = 0; i < int(side.units.size()); ++i) {
            if (side.units[i].slot_count > 1) {
                if (!pending_single.empty()) {
                    side.pages.push_back(pending_single);
                    pending_single.clear();
                }
                side.pages.push_back({i});
            } else {
                pending_single.push_back(i);
                if (pending_single.size() == 2) {
                    side.pages.push_back(pending_single);
                    pending_single.clear();
                }
            }
        }
        if (!pending_single.empty())
            side.pages.push_back(pending_single);
    }
    return sides;
}

int page_of_unit(const Side &side, const std::string &ams_id)
{
    for (size_t p = 0; p < side.pages.size(); ++p)
        for (int idx : side.pages[p])
            if (idx >= 0 && idx < int(side.units.size()) && side.units[idx].ams_id == ams_id)
                return int(p);
    return -1;
}

int side_of_unit(const std::vector<Side> &sides, const std::string &ams_id)
{
    for (size_t s = 0; s < sides.size(); ++s)
        for (const UnitRef &u : sides[s].units)
            if (u.ams_id == ams_id)
                return int(s);
    return -1;
}

std::string slot_label(const UnitRef &unit, int slot_index)
{
    if (unit.type == UNIT_EXT_SPOOL || is_virtual_slot(unit.ams_id))
        return "Ext";
    const long id = numeric_id(unit.ams_id);
    if (id == std::numeric_limits<long>::max() || id < 0)
        return "?";
    if (id >= 128) {
        // AMS HT: "A".."H", counted from 128 (GUI_App::transition_tridid, DevFilaSystem).
        const long n = id - 128;
        return n < 26 ? std::string(1, char('A' + n)) : "?";
    }
    if (id >= 26)
        return "?";
    std::string label(1, char('A' + id));
    if (unit.slot_count > 1)
        label += std::to_string(slot_index + 1);
    return label;
}

bool slot_is_loaded(const std::string &ams_id, const std::string &slot_id)
{
    if (ams_id.empty() || slot_id.empty())
        return false;
    // 255/255 is "nothing"; an external spool is 254/0 or 255/0.
    return slot_id != "255";
}

}}} // namespace Slic3r::GUI::AmsDual
