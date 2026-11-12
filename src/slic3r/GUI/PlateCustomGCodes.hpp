#pragma once

#include <map>
#include <string>
#include <vector>

#include "libslic3r/CustomGCode.hpp"

namespace Slic3r { namespace GUI {

// Model::plates_custom_gcodes holds each plate's layer items (pauses, filament changes, custom
// G-code) under the plate's index. An index belongs to whichever plate sits there, so a plate that
// is deleted or moved, or that undo/redo brings back, has to take its items along: they are filed
// by the plate's own key (PartPlate::m_history_key) while the plate list changes, and filed by
// index again afterwards. `keys` lists the plates' keys in plate order.

// The items by plate key. `removed` holds the items of plates no longer in the list; they are kept
// for an undo or redo that brings such a plate back.
inline std::map<std::string, CustomGCode::Info> plate_custom_gcodes_by_key(const std::map<int, CustomGCode::Info> &by_index,
                                                                           const std::vector<std::string>          &keys,
                                                                           std::map<std::string, CustomGCode::Info> removed)
{
    for (size_t i = 0; i < keys.size(); ++i)
        if (auto it = by_index.find(int(i)); it != by_index.end())
            removed[keys[i]] = it->second;
    return removed;
}

// The items by plate index. What is left in `by_key` belongs to plates no longer in the list.
inline std::map<int, CustomGCode::Info> plate_custom_gcodes_by_index(std::map<std::string, CustomGCode::Info> &by_key,
                                                                     const std::vector<std::string>           &keys)
{
    std::map<int, CustomGCode::Info> by_index;
    for (size_t i = 0; i < keys.size(); ++i)
        if (auto it = by_key.find(keys[i]); it != by_key.end()) {
            by_index[int(i)] = std::move(it->second);
            by_key.erase(it);
        }
    return by_index;
}

}} // namespace Slic3r::GUI
