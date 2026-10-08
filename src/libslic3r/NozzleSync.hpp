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

// A machine variant of one printer model: its printer_variant ("0.4", "0.4+0.6") and the per-head nozzle
// diameters of its preset.
struct MachineVariant
{
    std::string         variant;
    std::vector<double> nozzles;
};

// The machine variant whose per-head diameters are exactly what the printer reported ("0.4" x4 -> "0.4",
// 0.4/0.4/0.6/0.6 -> "0.4+0.6"). A head whose report could not be read (0) matches anything; the machine must
// have as many heads as were reported. Empty when no machine matches (a mix we have no machine for: the caller
// picks a base machine and writes the heads over it with apply_per_head).
std::string match_machine_variant(const std::vector<double> &reported_per_head, const std::vector<MachineVariant> &machines);

// One filament preset a machine offers: its name, the family ("alias") the variants of one filament share,
// and the nozzle diameter it was cut for (0 = fits any).
struct FilamentChoice
{
    std::string name;
    std::string family;
    double      nozzle = 0.;
};

// The families in the order they first appear: the entries of an "apply to all" list, each filament once.
std::vector<std::string> families(const std::vector<FilamentChoice> &choices);

struct SlotAssignment
{
    std::string preset; // empty when skipped
    bool        skipped = false;
};

// Apply a family to every filament slot: the variant cut for the slot's nozzle (`slot_nozzles[i]`, 0 =
// unknown, takes the first variant), else one that fits any nozzle; a slot with no matching variant is
// skipped (never given the wrong nozzle size).
std::vector<SlotAssignment> assign_family_to_slots(const std::vector<FilamentChoice> &choices, const std::string &family,
                                                   const std::vector<double> &slot_nozzles);

} // namespace NozzleSync
} // namespace Slic3r
