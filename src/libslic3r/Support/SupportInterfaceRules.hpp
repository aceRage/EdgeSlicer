#pragma once

// EdgeSlicer: the rules the GUI applies to the support interface settings, kept free of wx so the
// tests can reach them (tests/libslic3r/test_support_interface_settings.cpp).

#include <algorithm>

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

} // namespace Slic3r
