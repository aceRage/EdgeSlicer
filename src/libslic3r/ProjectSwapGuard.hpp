#ifndef slic3r_ProjectSwapGuard_hpp_
#define slic3r_ProjectSwapGuard_hpp_

// Project strip (tests/design_multi_project_sidebar.md, P1): one Plater shows the active project; the others are parked as
// a temporary 3MF each. A switch parks the outgoing project and loads the incoming one as a 3MF is opened, which selects
// printers, rewrites the project config and refreshes the nozzle UI. None of that is a user edit of the incoming project:
// the per-printer nozzle memory (#914, PresetBundle::remember_nozzle_rack / restore_nozzle_rack) must not take the
// project's rack as the printer's, and the #911 rule (the printer variant follows one nozzle size) must not switch
// presets or ask anything. Both check this guard (design risk 1).
//
// The GUI holds it from the park until after the deferred calls the load queued (wxApp::CallAfter, which runs them in
// order): hold() before the switch, release() from a CallAfter queued at its end.

#include <atomic>

#include "Format/bbs_3mf.hpp"

namespace Slic3r {

class ProjectSwapGuard
{
public:
    ProjectSwapGuard() { hold(); }
    ~ProjectSwapGuard() { release(); }
    ProjectSwapGuard(const ProjectSwapGuard &) = delete;
    ProjectSwapGuard &operator=(const ProjectSwapGuard &) = delete;

    // True while a project switch (or what it deferred) runs.
    static bool active() { return counter().load() > 0; }
    static void hold() { ++counter(); }
    // Never below zero, even for an unmatched release.
    static void release()
    {
        int v = counter().load();
        while (v > 0 && !counter().compare_exchange_weak(v, v - 1)) {}
    }

private:
    static std::atomic<int> &counter()
    {
        static std::atomic<int> c{ 0 };
        return c;
    }
};

// The parked project: a self-contained project save (as File > Save project writes it: model, per-object and per-part
// settings, paint and seam / support / fuzzy-skin facets, plates, project config, embedded project presets, plate
// thumbnails), Silence so neither the backup folder's origin.txt nor the window's project file name change.
// Not SaveStrategy::Backup: a backup 3MF leaves the meshes to separate files the backup manager writes next to it and the
// G-code to the plates' temp files (SkipStatic), so it is not a snapshot on its own. Valid plates' G-code is copied next
// to the snapshot by the GUI instead (WithGcode would run the user's post-processing scripts on every switch).
inline SaveStrategy project_park_save_strategy()
{
    return SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::ShareMesh | SaveStrategy::FullPathSources;
}

// The parked project comes back as an opened project 3MF does (presets travel with it), quietly. Not LoadStrategy::Restore:
// that one re-keys objects by backup id for the backup writer's layout.
inline LoadStrategy project_park_load_strategy()
{
    return LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::LoadAuxiliary | LoadStrategy::Silence;
}

} // namespace Slic3r

#endif // slic3r_ProjectSwapGuard_hpp_
