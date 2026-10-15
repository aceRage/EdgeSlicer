#pragma once

// The order of the filament list in the AMS slot settings dialog (AMSMaterialsSetting::Popup).
//
// GUI-free so the ordering rules can be unit-tested without a window: Popup() gathers a name, a
// vendor and a type for each filament the printer can use, hands them to sort(), and shows the
// result. Ported from OrcaSlicer #11293 (current filaments first) on top of Bambu Studio's name
// sort ("FIX: sort the filament names").

#include <algorithm>
#include <climits>
#include <map>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace AmsFilamentOrder {

struct Entry
{
    std::string name;   // what the dropdown shows (a system preset's alias)
    std::string vendor; // the preset's filament_vendor
    std::string type;   // the preset's filament_type
};

// The rank of every filament preset in use on the plate, keyed by its base (system) alias. The
// aliases are in extruder order; the first extruder gets the most negative rank, so the filaments
// the user is slicing with come first, in extruder order. A filament used by several extruders
// keeps the rank of the first one. An empty alias (a preset whose base could not be found) is
// skipped but still counts as a position.
inline std::map<std::string, int> selected_ranks(const std::vector<std::string> &aliases)
{
    std::map<std::string, int> ranks;
    const int                  n = int(aliases.size());
    for (int i = n - 1; i >= 0; --i) {
        if (aliases[i].empty())
            continue;
        ranks[aliases[i]] = -(n - i); // later (lower i) assignments win, as in upstream's reverse loop
    }
    return ranks;
}

namespace detail {

// Bambu's own favourites, shown first when nothing in use outranks them.
inline const std::map<std::string, int> &bambu_name_order()
{
    static const std::map<std::string, int> table = {{"Bambu PLA Basic", 0},
                                                     {"Bambu PLA Matte", 1},
                                                     {"Bambu PETG HF", 2},
                                                     {"Bambu ABS", 3},
                                                     {"Bambu PLA Silk", 4},
                                                     {"Bambu PLA-CF", 5},
                                                     {"Bambu PLA Galaxy", 6},
                                                     {"Bambu PLA Metal", 7},
                                                     {"Bambu PLA Marble", 8},
                                                     {"Bambu PETG-CF", 9},
                                                     {"Bambu PETG Translucent", 10},
                                                     {"Bambu ABS-GF", 11}};
    return table;
}

inline int name_rank(const std::string &name, const std::map<std::string, int> &selected)
{
    const auto sel = selected.find(name);
    if (sel != selected.end())
        return sel->second;
    const auto &table = bambu_name_order();
    const auto  it    = table.find(name);
    return it != table.end() ? it->second : INT_MAX;
}

inline int position_in(const std::vector<std::string> &order, const std::string &value)
{
    return int(std::find(order.begin(), order.end(), value) - order.begin());
}

} // namespace detail

inline bool less(const Entry &left, const Entry &right, const std::map<std::string, int> &selected)
{
    {
        const int l = detail::name_rank(left.name, selected);
        const int r = detail::name_rank(right.name, selected);
        if (l != r)
            return l < r;
    }
    {
        static const std::vector<std::string> vendors{"Bambu Lab", "Generic"};
        const int                             l = detail::position_in(vendors, left.vendor);
        const int                             r = detail::position_in(vendors, right.vendor);
        if (l != r)
            return l < r;
    }
    {
        static const std::vector<std::string> types{"PLA", "PETG", "ABS", "TPU"};
        const int                             l = detail::position_in(types, left.type);
        const int                             r = detail::position_in(types, right.type);
        if (l != r)
            return l < r;
    }
    return left.name < right.name;
}

inline void sort(std::vector<Entry> &entries, const std::map<std::string, int> &selected)
{
    std::sort(entries.begin(), entries.end(), [&selected](const Entry &a, const Entry &b) { return less(a, b, selected); });
}

}}} // namespace Slic3r::GUI::AmsFilamentOrder
