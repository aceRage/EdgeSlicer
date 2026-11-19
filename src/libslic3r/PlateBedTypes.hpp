#pragma once

#include <string>
#include <vector>

#include "PrintConfig.hpp"

namespace Slic3r {
namespace PlateBedTypes {

// Orca #15916 (per-plate bed type handling), adapted for EdgeSlicer: which per-plate bed type
// overrides the selected printer accepts. Pure logic, so it can be tested without the GUI.
//
// - Bambu printers and printers with support_multi_bed_types accept every bed type.
// - The Snapmaker U1 without support_multi_bed_types has its own bed type selector (the curr_bed_type
//   enum_values_u1 list): Textured PEI, Smooth PEI / High Temp, Graphic Effect (Snapmaker #759/#920,
//   PR #502) and Cool Plate (Edge, owner 2026-10-15), so it accepts those four. Upstream Orca enables
//   support_multi_bed_types on the U1 instead (#15174), which would bring back plates the U1 does not have.
// - Every other printer has a single bed type: per-plate overrides are not allowed.
struct Allowed
{
    bool                 any = false;
    std::vector<BedType> types; // used when !any; empty = single bed type
};

// printer_model names a Snapmaker U1 ("Snapmaker U1", case-insensitive).
bool is_snapmaker_u1_model(const std::string &printer_model);

Allowed allowed_bed_types(const DynamicPrintConfig &printer_config, bool is_bbl_vendor);

// The printer offers a bed type choice at all (global selector, plate settings).
inline bool supports_bed_type_choice(const Allowed &allowed) { return allowed.any || !allowed.types.empty(); }

// A per-plate override `type` is acceptable. btDefault ("same as global") always is.
bool is_allowed(const Allowed &allowed, BedType type);

// Resets every override in `plate_bed_types` that is not allowed to btDefault. True when one was reset.
bool normalize(std::vector<BedType> &plate_bed_types, const Allowed &allowed);

// The global bed type for a printer whose bed type selector lists `listed_keys` (the keys of "curr_bed_type":
// ConfigOptionDef::enum_values, or its _u1 / _ex variant). The selector can only show a plate it lists, so one
// it does not list (Graphic Effect Plate on a printer that is not a U1, a value that is no plate at all) must
// not stay in the project: the sidebar then showed its first row and the slice used the other plate.
// `wanted` when it is listed, else `printer_default` (Preset::get_default_bed_type) when that is listed, else
// the first plate listed. `wanted` when the list holds no plate.
BedType listed_bed_type(BedType wanted, const std::vector<std::string> &listed_keys, BedType printer_default);

} // namespace PlateBedTypes
} // namespace Slic3r
