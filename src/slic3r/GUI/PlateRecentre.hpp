#ifndef slic3r_GUI_PlateRecentre_hpp_
#define slic3r_GUI_PlateRecentre_hpp_

#include <vector>

// The printer-switch plate recentre ("BBS: Model reset by plate center" in
// Plater::priv::on_select_preset), kept free of GUI types so tests/slic3rutils can drive it.
//
// When a printer switch moves the plate centre, every plate's objects are selected and centred on
// their plate. Each centring used to take its own "Move Object" undo snapshot (a full model
// snapshot, ~130 - 210 ms per plate in the owner's logs), empty plates included, and the
// deselect after the loop one more selection snapshot, so undoing a switch took one step per
// plate plus one. Now the whole pass is one snapshot and empty plates are not visited.
//
// Where objects end up does not change: the non-empty plates are centred in the same order by
// the same calls. Visiting an empty plate selected nothing and moved nothing; its do_move only
// re-synced every volume's transform into the model (a no-op once any plate has been synced) and
// re-applied the flying-instance fix (z only, and idempotent), which the non-empty plates' passes
// do as well.

namespace Slic3r { namespace GUI {

// The plates to visit, in order: each plate holding at least one object. When none does, plate 0
// alone (if there is a plate), so the one model/scene sync the old loop performed still happens.
inline std::vector<int> plates_to_recentre(const std::vector<std::vector<int>>& plate_objects)
{
    std::vector<int> out;
    for (size_t i = 0; i < plate_objects.size(); ++i)
        if (!plate_objects[i].empty())
            out.push_back(int(i));
    if (out.empty() && !plate_objects.empty())
        out.push_back(0);
    return out;
}

// Runs the recentre under a single undo snapshot. `begin_snapshot()` returns a scope guard that
// takes one snapshot and suppresses every nested one until it is destroyed (Plater::TakeSnapshot);
// `recentre_plate(i)` selects plate i's objects and centres them; `finish()` (the deselect) runs
// inside the guard too, so it adds no selection snapshot of its own. Returns the plates visited.
template<class BeginSnapshot, class RecentrePlate, class Finish>
int run_plate_recentre(const std::vector<std::vector<int>>& plate_objects,
                       BeginSnapshot&&                      begin_snapshot,
                       RecentrePlate&&                      recentre_plate,
                       Finish&&                             finish)
{
    const std::vector<int> plates = plates_to_recentre(plate_objects);
    auto                   guard  = begin_snapshot();
    (void) guard;
    for (int i : plates)
        recentre_plate(i);
    finish();
    return int(plates.size());
}

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PlateRecentre_hpp_
