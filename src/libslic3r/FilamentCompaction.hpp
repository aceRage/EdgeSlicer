#ifndef slic3r_FilamentCompaction_hpp_
#define slic3r_FilamentCompaction_hpp_

#include <vector>

namespace Slic3r {

class Model;
class DynamicPrintConfig;

// Dense logical tool numbering for printers whose firmware only accepts T0..T(n-1).
//
// A project may hold more filaments than the printer has tools (see
// device_resolves_filament_mapping), so a plate can legitimately print with project slots 3 and
// 6 while the machine has four heads. The Snapmaker U1 takes those slot numbers as-is -- its
// 32-entry extruder_map_table is indexed by them -- but firmware with no macro past
// T(tool_count-1) needs slots 3 and 6 to reach it as T0 and T1.
//
// Rather than translating tool numbers at every emit site, the filament index space is
// renumbered once at the top of Print::apply. Downstream consumers index that same space.
//
// Gated on the namespace (build_filament_compaction): printers that do not resolve the map never
// build a compaction.
//
// Mixed-colour filaments are virtual slots (MixedFilamentManager). ToolOrdering resolves each
// to its physical components; only those are commanded as tools. A used mix is numbered AFTER
// the physical tools so T0..T(n-1) stay exactly the filaments the printer loads.
struct FilamentCompaction
{
    // The 0-based project filament slot each dense tool number prints: the used physical slots
    // in ascending order, then any used mixed slots. Empty means "no renumbering".
    std::vector<int> slot_of_tool;

    bool is_identity() const;
    // The dense tool number that prints a project slot, or -1 when the plate doesn't use it.
    int  tool_of_slot(int slot_0based) const;
};

// The 0-based PHYSICAL filament slots the printable objects of the model's current plate use,
// sorted and deduplicated, with every mixed slot replaced by its components.
std::vector<int> used_filament_slots(const Model& model, const DynamicPrintConfig& config);

// is_identity() unless the plate's highest used slot reaches past the printer's T namespace
// (filament_namespace_size): then the used slots are packed to 0..n-1.
FilamentCompaction build_filament_compaction(const Model& model, const DynamicPrintConfig& config, size_t namespace_size);

// Renumber every filament reference in the model. `source` is the model `model` was copied from
// (same objects, same order). Mutated configs/painting get timestamps mirrored from the source
// so Print::apply change detection does not loop. Passing the same object as both arguments is
// allowed (in-place, used by tests).
void apply_filament_compaction(Model& model, const Model& source, const FilamentCompaction& compaction);

// Gather every per-filament config vector down to the used physical slots, in dense order, and
// renumber scalar 1-based filament references plus MixedFilamentManager component lists.
void apply_filament_compaction(DynamicPrintConfig& config, const FilamentCompaction& compaction);

} // namespace Slic3r

#endif // slic3r_FilamentCompaction_hpp_
