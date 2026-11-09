#pragma once

// EdgeSlicer: the rules the GUI applies to the support interface settings, kept free of wx so the
// tests can reach them (tests/libslic3r/test_support_interface_settings.cpp).

#include <algorithm>
#include <set>
#include <string>
#include <utility>

namespace Slic3r {

// The support interface interlock weaves `woven_layers` interface layers under the top contact
// layer, which itself is left alone; so it needs that many top interface layers plus one.
// The woven count is clamped to 2-4 exactly as SupportParameters does.
inline int interlock_needed_interface_layers(int woven_layers)
{
    return 1 + std::clamp(woven_layers, 2, 4);
}

// What the interlock prompt would change: the top interface layer count is raised to
// interlock_needed_interface_layers(), and a bottom interface layer count that is set to its own
// number (not -1 "Same as top", which follows the top anyway, and not 0 "no bottom interface")
// and lower than that is raised along with it.
struct InterlockInterfaceRaise
{
    bool needed       { false };
    int  top_from     { 0 };
    int  top_to       { 0 };
    bool raise_bottom { false };
    int  bottom_from  { 0 };
    int  bottom_to    { 0 };
};

inline InterlockInterfaceRaise interlock_interface_raise(bool interlock_on, int woven_layers, int top_layers, int bottom_layers)
{
    InterlockInterfaceRaise out;
    out.top_from = out.top_to = top_layers;
    out.bottom_from = out.bottom_to = bottom_layers;
    if (! interlock_on)
        return out;
    const int need = interlock_needed_interface_layers(woven_layers);
    if (top_layers >= need)
        return out;
    out.needed = true;
    out.top_to = need;
    if (bottom_layers > 0 && bottom_layers < need) {
        out.raise_bottom = true;
        out.bottom_to    = need;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// The "top Z distance 0" offer for an interface printed in another filament.
//
// Bambu Studio offers the soluble-interface recipe whenever the interface filament changes to a
// support material, and asks again on every such change however often the user says No. Here it
// is offered for any interface filament that differs from the print's, and a No is remembered for
// that (interface filament, print filament) pair until the project changes. It is never offered
// while the top Z distance itself is being edited, while a preset or project loads (only
// Tab::on_value_change of a filament selector calls it), when the distance already is 0, when no
// support or raft is printed, or when "Support Filament Matching" picks the interface filament.
// ---------------------------------------------------------------------------------------------

// The filament the print's walls use: wall_filament when it is set, otherwise the object's own
// filament (extruder), otherwise filament 1. All are 1-based, 0 meaning "not set".
inline int support_print_filament(int wall_filament, int object_extruder)
{
    return wall_filament > 0 ? wall_filament : (object_extruder > 0 ? object_extruder : 1);
}

// True when the support (and raft) interface is printed in a filament of its own that is not the
// print's. 0 ("Default") prints the interface with the current filament.
inline bool support_interface_filament_differs(int interface_filament, int print_filament, bool filament_matching)
{
    return ! filament_matching && interface_filament > 0 && interface_filament != print_filament;
}

// The settings whose interactive change may bring up the offer: the filament selectors.
inline bool is_top_z_zero_prompt_trigger(const std::string &changed_key)
{
    return changed_key == "support_interface_filament" || changed_key == "wall_filament";
}

class TopZZeroPromptMemory
{
public:
    struct Inputs
    {
        std::string changed_key;
        bool        supports_used      { false }; // enable_support, or raft layers > 0
        bool        filament_matching  { false }; // support_filament_matching (Chameleon)
        int         interface_filament { 0 };
        int         print_filament     { 1 };
        double      top_z_distance     { 0. };
    };

    bool should_offer(const Inputs &in) const
    {
        if (! is_top_z_zero_prompt_trigger(in.changed_key))
            return false; // never while support_top_z_distance (or anything else) is edited
        if (! in.supports_used || in.top_z_distance <= 0.)
            return false;
        if (! support_interface_filament_differs(in.interface_filament, in.print_filament, in.filament_matching))
            return false;
        return ! this->declined(in.interface_filament, in.print_filament);
    }
    void remember_no(int interface_filament, int print_filament) { m_declined.emplace(interface_filament, print_filament); }
    bool declined(int interface_filament, int print_filament) const { return m_declined.count({ interface_filament, print_filament }) != 0; }
    void clear() { m_declined.clear(); }

private:
    std::set<std::pair<int, int>> m_declined;
};

} // namespace Slic3r
