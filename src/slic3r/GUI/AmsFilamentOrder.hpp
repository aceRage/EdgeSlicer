#pragma once

// The order of the filament list in the AMS slot settings dialog (AMSMaterialsSetting::Popup).
//
// GUI-free so the ordering rules can be unit-tested without a window: Popup() gathers a name, a
// vendor and a type for each filament the printer can use, hands them to sort(), and shows the
// result. Ported from OrcaSlicer #11293 (the filaments in use first) and #11306 (Generic above the
// other vendors) on top of Bambu Studio's sort ("FIX: sort the filament names").

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

// Case-insensitive for ASCII, which is what vendor, type and alias names are made of; bytes above
// 127 (UTF-8 in a user preset's name) compare as they are.
inline int compare_no_case(const std::string &a, const std::string &b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z')
            ca = static_cast<unsigned char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = static_cast<unsigned char>(cb - 'A' + 'a');
        if (ca != cb)
            return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size())
        return 0;
    return a.size() < b.size() ? -1 : 1;
}

// The position of value in the priority list (compared without case); values not listed come
// after every listed one.
inline int priority_rank(const std::vector<std::string> &priorities, const std::string &value)
{
    const auto it = std::find_if(priorities.begin(), priorities.end(),
                                 [&value](const std::string &p) { return compare_no_case(p, value) == 0; });
    return int(it - priorities.begin());
}

inline int selected_rank(const std::string &name, const std::map<std::string, int> &selected)
{
    const auto it = selected.find(name);
    return it != selected.end() ? it->second : INT_MAX;
}

} // namespace detail

inline bool less(const Entry &left, const Entry &right, const std::map<std::string, int> &selected)
{
    {
        const int l = detail::selected_rank(left.name, selected);
        const int r = detail::selected_rank(right.name, selected);
        if (l != r)
            return l < r;
    }
    { // Generic first, then the vendors alphabetically
        static const std::vector<std::string> vendors{"Generic"};
        const int l = detail::priority_rank(vendors, left.vendor);
        const int r = detail::priority_rank(vendors, right.vendor);
        if (l != r)
            return l < r;
        const int c = detail::compare_no_case(left.vendor, right.vendor);
        if (c != 0)
            return c < 0;
    }
    { // the common types first, then the types alphabetically
        static const std::vector<std::string> types{"PLA", "PETG", "ABS", "TPU"};
        const int l = detail::priority_rank(types, left.type);
        const int r = detail::priority_rank(types, right.type);
        if (l != r)
            return l < r;
        const int c = detail::compare_no_case(left.type, right.type);
        if (c != 0)
            return c < 0;
    }
    const int c = detail::compare_no_case(left.name, right.name);
    return c != 0 ? c < 0 : left.name < right.name;
}

inline void sort(std::vector<Entry> &entries, const std::map<std::string, int> &selected)
{
    std::sort(entries.begin(), entries.end(), [&selected](const Entry &a, const Entry &b) { return less(a, b, selected); });
}

}}} // namespace Slic3r::GUI::AmsFilamentOrder
