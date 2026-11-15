#ifndef slic3r_GLGizmoSplitToFit_hpp_
#define slic3r_GLGizmoSplitToFit_hpp_

// SPLIT TO FIT, phase 1 (tests/design_split_to_build_volume.md; owner decisions D1-D17, 2026-11-15).
//
// Splits the selected object into parts that each fit the current printer (D8), by an axis-aligned
// grid of plain planar cuts. The panel shows the printer's volume, the margins (D2), the axes, the
// layout (D5), the resulting parts with their sizes, and the warnings; the 3D view shows the cut
// planes on the model and outlines in red the parts that would not fit. Apply runs the cuts as a
// background job (SplitToFitJob), names the parts by grid cell (D7), arranges them over plates (D12)
// and is one undo step.
//
// Entry points (D1): its own toolbar icon, a "Split to fit..." button in the Cut panel and
// "Split to fit the printer..." in the object's right-click menu.
//
// The planner (libslic3r/SplitToVolume.hpp) does all the work; this class only gathers its inputs,
// draws what comes back and starts the job.

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/SplitToVolume.hpp"

#include <string>

namespace Slic3r {
class ModelObject;

namespace GUI {

// The panel's (and the job's) text for a planner warning.
wxString split_warning_text(const SplitWarning &warning, const SplitPlan &plan);

class GLGizmoSplitToFit : public GLGizmoBase
{
public:
    GLGizmoSplitToFit(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

    void data_changed(bool is_serializing) override;

protected:
    bool               on_init() override;
    std::string        on_get_name() const override;
    void               on_set_state() override;
    bool               on_is_activable() const override;
    CommonGizmosDataID on_get_requirements() const override;
    void               on_render() override;
    void               on_render_input_window(float x, float y, float bottom_limit) override;

private:
    // The object the gizmo works on (the selected one), and its selected instance.
    ModelObject *selected_object(int *instance = nullptr) const;
    // Re-reads the model's mesh when the object or its placement changed, and the printer's volume
    // when the printer changed; marks the plan dirty when either did.
    void refresh_inputs();
    void update_plan();
    void rebuild_preview();
    void apply();
    void reset_options();
    void load_settings();
    void save_settings() const;

    SplitOptions          m_options;
    SplitPrinterVolume    m_printer;
    SplitPlan             m_plan;
    bool                  m_plan_dirty{ true };

    // The mesh in the grid frame, and what it was taken from.
    indexed_triangle_set  m_mesh;
    ObjectID              m_mesh_object;
    Transform3d           m_mesh_trafo{ Transform3d::Identity() };
    size_t                m_mesh_volumes{ 0 };

    GLModel               m_planes_fill;
    GLModel               m_pinned_fill;
    GLModel               m_planes_outline;
    GLModel               m_bad_parts;
    bool                  m_preview_dirty{ true };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoSplitToFit_hpp_
