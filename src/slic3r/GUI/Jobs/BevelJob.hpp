#ifndef slic3r_GUI_BevelJob_hpp_
#define slic3r_GUI_BevelJob_hpp_

// The Edit gizmo's bevel/chamfer, run off the UI thread.
//
// Spec: docs/superpowers/specs/2026-09-12-edit-round-and-bevel.md
//
// WHY THIS EXISTS: the bevel used to run inline in GLGizmoEdit::apply_bevel(),
// on the UI thread. A selection whose solve is expensive - the 3DBenchy's bottom
// rim, hundreds of edges around a curved hull - therefore froze the whole
// application with no progress and no way out, and the owner had to kill it.
// The complexity that made that particular case slow is fixed in MeshEdit.cpp,
// but a mesh operation on an arbitrary user model has no upper bound worth
// betting the UI thread on, so it belongs in a worker with a Cancel button.
//
// The job reads a COPY of the mesh and produces a new indexed_triangle_set;
// nothing it touches is shared with the main thread while it runs. Everything
// that reaches the Model happens in finalize(), which the worker calls on the UI
// thread, and which re-finds the volume by id rather than holding a pointer
// across the wait.

#include "Job.hpp"

#include "libslic3r/MeshEdit.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <functional>
#include <vector>

namespace Slic3r { namespace GUI {

class BevelJob : public Job
{
public:
    // What finalize() hands back to the gizmo, on the UI thread.
    using Done = std::function<void(const MeshEdit::BevelResult &)>;

    // `its` is copied: the worker must not read a mesh the UI thread can edit.
    BevelJob(indexed_triangle_set        its,
             std::vector<int>            edges,
             const MeshEdit::BevelParams &params,
             Done                        on_done);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    indexed_triangle_set    m_its;
    std::vector<int>        m_edges;
    MeshEdit::BevelParams   m_params;
    Done                    m_on_done;

    MeshEdit::BevelResult   m_result;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_BevelJob_hpp_
