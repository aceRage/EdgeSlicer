#ifndef slic3r_ProjectSwapGuard_hpp_
#define slic3r_ProjectSwapGuard_hpp_

// Project strip (tests/design_multi_project_sidebar.md). P4: one live Plater per open project; the presets are app-global,
// so a switch puts the incoming project's presets back in the bundle and refreshes the preset tabs (and a session restore
// loads each project's 3MF). None of that is a user edit of the incoming project: the per-printer nozzle memory (#914,
// PresetBundle::remember_nozzle_rack / restore_nozzle_rack) must not take the project's rack as the printer's, and the #911
// rule (the printer variant follows one nozzle size) must not switch presets or ask anything. Both check this guard (design
// risk 1).
//
// The GUI holds it from the start of the switch until after the deferred calls the preset refresh queued
// (wxApp::CallAfter, which runs them in order): hold() before the switch, release() from a CallAfter queued at its end.

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

// A project written for session restore: a self-contained project save (as File > Save project writes it: model,
// per-object and per-part settings, paint and seam / support / fuzzy-skin facets, plates, project config, embedded project
// presets, plate thumbnails), Silence so neither the backup folder's origin.txt nor the window's project file name change.
// Not SaveStrategy::Backup: a backup 3MF leaves the meshes to separate files the backup manager writes next to it, so it is
// not a snapshot on its own. No G-code (WithGcode would run the user's post-processing scripts): plates slice again after
// a restart.
inline SaveStrategy project_park_save_strategy()
{
    return SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::ShareMesh | SaveStrategy::FullPathSources;
}

// The session snapshot comes back as an opened project 3MF does (presets travel with it), quietly. Not LoadStrategy::Restore:
// that one re-keys objects by backup id for the backup writer's layout.
inline LoadStrategy project_park_load_strategy()
{
    return LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::LoadAuxiliary | LoadStrategy::Silence;
}

} // namespace Slic3r

#endif // slic3r_ProjectSwapGuard_hpp_
