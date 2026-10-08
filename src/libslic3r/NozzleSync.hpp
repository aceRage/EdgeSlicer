#pragma once

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace Slic3r {
namespace NozzleSync {

// Pure logic behind "Synchronize nozzle information" (a connected tool changer reports the nozzle
// diameter of every head). No GUI, no preset access, so it can be tested with fake reports.

// The nozzle diameters a printer's system_info product_info reports ("nozzle_diameter": a number, a
// string, or an array of either), as the strings the sync uses ("0.2", "0.4", "0.6", "0.8"). Values that
// are not one of those four sizes are dropped. A single scalar value maps to ITSELF (it used to map
// 0.4, 0.6 and 0.8 to "0.2").
std::vector<std::string> parse_reported_nozzles(const nlohmann::json &reported);

// One reported entry ("0.4", "0.4 mm", "0.4mm") as a diameter in mm, or 0 when it is not a plain number in
// (0, 2]. Position is kept by the callers: a head whose report cannot be read must not shift the others.
double parse_reported_diameter(const std::string &text);

// "0.4" for 0.4: the printer_variant a preset for that diameter carries.
std::string variant_name(double diameter);

struct Plan
{
    // Every readable head reports the same diameter: select the machine for it (all heads get it).
    bool uniform = false;
    // The machine variant for a uniform report ("0.4"); empty otherwise.
    std::string variant;
    // Per head, in head order; 0 for a head whose report could not be read.
    std::vector<double> per_head;
};

Plan plan(const std::vector<std::string> &reported);

// Overwrite `current` (the selected preset's per-extruder diameters) with the readable reported ones, head
// by head from head 0. A head the printer did not report, or reported unreadably, keeps its value.
// `changed` (optional) tells whether anything differed.
std::vector<double> apply_per_head(const std::vector<double> &current, const std::vector<double> &reported_per_head, bool *changed = nullptr);

} // namespace NozzleSync
} // namespace Slic3r
