#include "ZipCleanup.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/CoalescingWorker.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/ZipOpen.hpp"

#include <memory>
#include <set>
#include <vector>

#include <boost/log/trivial.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Slic3r {
namespace GUI {
namespace ZipCleanup {

namespace fs = boost::filesystem;

namespace {

struct Request
{
    Mode                    mode = Mode::UsageOnly;
    ZipWorkCleaner::Options options;
};

// UI thread only.
std::set<fs::path>     s_sessions;
Listener               s_listener;

int rank(Mode m) { return m == Mode::Now ? 2 : (m == Mode::Auto ? 1 : 0); }

void lower_thread_priority()
{
#ifdef _WIN32
    // Background mode lowers the thread's CPU and I/O priority, so a big clean never competes
    // with slicing or the UI.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#endif
}

// The worker thread and its queue. Defined after everything the passes use, so that it is destroyed
// first at process exit. Its destructor cancels and joins the thread: shutdown() normally ran long
// before (MainFrame::shutdown, GUI_App::OnExit), but an exit that skipped both must not end in
// std::terminate() on a still-joinable thread.
CoalescingWorker s_worker(lower_thread_priority);

void run_pass(const Request& req)
{
    std::shared_ptr<ZipWorkCleaner::Result> result;
    try {
        if (req.mode != Mode::UsageOnly)
            result = std::make_shared<ZipWorkCleaner::Result>(ZipWorkCleaner::clean(req.options));
        const ZipWorkCleaner::Usage usage = ZipWorkCleaner::usage(req.options.root, req.options.cancelled);
        // Handed to the UI thread only while the clean-up is not shut down: after shutdown() nothing
        // reaches the app (the listener's page, wxTheApp) from this thread any more.
        s_worker.run_unless_stopped([&usage, &result]() {
            if (wxTheApp != nullptr)
                wxTheApp->CallAfter([usage, result]() {
                    if (s_listener)
                        s_listener(usage, result.get());
                });
        });
    } catch (const std::exception& e) {
        if (!s_worker.cancelled())
            BOOST_LOG_TRIVIAL(error) << "ZIP work folder clean-up failed: " << e.what();
    } catch (...) {
        if (!s_worker.cancelled())
            BOOST_LOG_TRIVIAL(error) << "ZIP work folder clean-up failed";
    }
}

std::vector<fs::path> paths_in_use()
{
    std::vector<fs::path> out(s_sessions.begin(), s_sessions.end());
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return out;
    const wxString project = plater->get_project_filename(".3mf");
    if (!project.IsEmpty())
        out.push_back(into_path(project));
    const wxString gcode = plater->get_last_loaded_gcode();
    if (!gcode.IsEmpty())
        out.push_back(into_path(gcode));
    // Files the objects on the plate came from (Reload from disk reads them again).
    for (const ModelObject* obj : plater->model().objects) {
        if (obj == nullptr)
            continue;
        if (!obj->input_file.empty())
            out.push_back(fs::path(obj->input_file));
        for (const ModelVolume* vol : obj->volumes)
            if (vol != nullptr && !vol->source.input_file.empty())
                out.push_back(fs::path(vol->source.input_file));
    }
    return out;
}

} // namespace

fs::path work_root()
{
    const AppConfig* cfg = wxGetApp().app_config;
    return resolve_zip_work_root(cfg ? cfg->get(ZIP_WORK_DIR_KEY) : std::string(), fs::path(data_dir()));
}

void note_session(const fs::path& session)
{
    if (!session.empty())
        s_sessions.insert(session);
}

void start(Mode mode)
{
    if (s_worker.cancelled())
        return; // shut down: the app is closing, its Plater may be going away
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    if (mode == Mode::Auto && !cfg->get_bool(ZIP_WORK_CLEANUP_KEY))
        return;

    Request req;
    req.mode                    = mode;
    req.options.root            = work_root();
    req.options.install_id      = cfg->get(ZIP_WORK_INSTALL_ID_KEY);
    req.options.max_age_days    = mode == Mode::Now ? 0 : zip_cleanup_days_from_value(cfg->get(ZIP_WORK_CLEANUP_DAYS_KEY));
    req.options.now             = std::time(nullptr);
    req.options.in_use          = paths_in_use();
    req.options.cancelled       = []() { return s_worker.cancelled(); };
    if (req.options.root.empty())
        return;

    // A stronger request wins; equal or stronger replaces the pending one with fresh settings.
    auto request = std::make_shared<Request>(std::move(req));
    if (!s_worker.submit(rank(mode), [request]() { run_pass(*request); }) && !s_worker.cancelled())
        BOOST_LOG_TRIVIAL(error) << "Could not start the ZIP work folder clean-up";
}

void set_listener(Listener listener) { s_listener = std::move(listener); }

void shutdown()
{
    // Cancelled between files, so the join is quick. Idempotent: MainFrame::shutdown() and
    // GUI_App::OnExit() both call it.
    s_worker.shutdown();
    s_listener = {};
}

} // namespace ZipCleanup
} // namespace GUI
} // namespace Slic3r
