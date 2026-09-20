#ifndef slic3r_GUI_QuadRemeshJob_hpp_
#define slic3r_GUI_QuadRemeshJob_hpp_

// "Quad remesh": the background half of the action.
//
// It used to run synchronously on the UI thread under a wxBusyCursor, with no cancel and no time
// limit. QuadriFlow can spin for a very long time (or, on input its precheck lets through,
// effectively forever), and with the remesh on the UI thread that is indistinguishable from a
// crash: the window stops painting and the only way out is to force-close the app. The owner hit
// exactly that on an assembled 2-part object.
//
// The precheck in libslic3r (quad_remesh_accepts) is the real protection and now catches that
// input, but it cannot promise that no legitimate mesh is ever slow. So the work moves off the UI
// thread as well: the app stays responsive, the progress notification carries a Cancel, and each
// part gets a wall-clock cap after which it is abandoned rather than waited on.
//
// Everything the worker touches is a COPY taken on the main thread; finalize() re-finds each
// volume by ObjectID, so a model edited while the remesh ran cannot be written through a stale
// pointer.

#include "Job.hpp"

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/QuadRemesh.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

class Plater;

// How long one part may take before it is given up on. QuadriFlow has no cancellation callback of
// its own - the pipeline is a straight run of Initialize / optimize_* / ComputeIndexMap with no
// hook to poll - so the cap cannot interrupt the call itself. What it does is bound the WAIT: the
// remesh runs on a detached helper, the job waits up to the cap, and past that it reports the part
// as timed out and moves on, leaving the helper to finish into a result nobody reads. That leaks a
// thread for the lifetime of the run in the pathological case, which is the price of upstream
// having no cancel; it does NOT leak the app's responsiveness or the user's session.
static constexpr int QUAD_REMESH_TIMEOUT_SECONDS = 60;

class QuadRemeshJob : public Job
{
public:
    struct Target
    {
        ObjectID    object_id;
        ObjectID    volume_id;
        std::string name;
        // A cheap "the mesh was replaced while we ran" guard, checked again in finalize().
        size_t      mesh_vertices = 0;
        size_t      mesh_indices  = 0;

        indexed_triangle_set mesh;      // input, taken on the main thread
        QuadRemeshOptions    opts;      // already resolved to this part's own target

        indexed_triangle_set result;    // filled by process()
        QuadRemeshReport     report;
        bool                 ok        = false;
        bool                 timed_out = false;
    };

    QuadRemeshJob(Plater *plater, std::vector<Target> targets);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    Plater             *m_plater = nullptr;
    std::vector<Target> m_targets;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_QuadRemeshJob_hpp_
