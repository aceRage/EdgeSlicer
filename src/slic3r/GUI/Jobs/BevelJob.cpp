#include "BevelJob.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

BevelJob::BevelJob(indexed_triangle_set         its,
                   std::vector<int>             edges,
                   const MeshEdit::BevelParams &params,
                   Done                         on_done)
    : m_its(std::move(its))
    , m_edges(std::move(edges))
    , m_params(params)
    , m_on_done(std::move(on_done))
{}

void BevelJob::process(Ctl &ctl)
{
    if (m_its.indices.empty() || m_edges.empty())
        return;

    const std::string status = _u8L("Bevelling the selected edges");
    ctl.update_status(0, status);

    // The two hooks MeshEdit::bevel_edges polls. Cancel is what makes the
    // operation escapable; progress is what makes the wait legible.
    m_params.cancelled = [&ctl]() { return ctl.was_canceled(); };
    m_params.progress  = [&ctl, &status](int percent) { ctl.update_status(percent, status); };

    // The topology is built here rather than handed in: it is O(facets) and
    // belongs on this side of the thread boundary.
    const MeshEdit::MeshTopology topo = MeshEdit::build_topology(m_its);
    if (ctl.was_canceled())
        return;

    m_result = MeshEdit::bevel_edges(m_its, topo, m_edges, m_params);

    ctl.update_status(100, status);
}

void BevelJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (eptr)
        return;                     // the worker rethrows on the UI thread
    if (canceled) {
        // A cancel leaves the model exactly as it was; the gizmo still wants to
        // know, so it can drop its "working" state and re-enable the panel.
        m_result        = MeshEdit::BevelResult{};
        m_result.status = MeshEdit::BevelStatus::Cancelled;
    }

    BOOST_LOG_TRIVIAL(info) << "BevelJob: status " << int(m_result.status)
                            << " edges " << m_result.bevelled_edges
                            << " corners " << m_result.corner_patches
                            << " triangles " << m_result.mesh.indices.size();

    if (m_on_done)
        m_on_done(m_result);
}

}} // namespace Slic3r::GUI
