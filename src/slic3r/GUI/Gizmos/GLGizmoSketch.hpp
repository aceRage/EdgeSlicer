// Ported from OrcaSlicer (PR #15762 / #15238, AGPL-3.0); original authors as credited upstream.
// See src/slic3r/GUI/CAD/NOTICE.md.
#ifndef slic3r_GLGizmoSketch_hpp_
#define slic3r_GLGizmoSketch_hpp_

#ifdef SLIC3R_CAD

#include "GLGizmoBase.hpp"

namespace Slic3r {
namespace GUI {

// Prepare-toolbar Sketch gizmo. Orca's original ImGui/OCCT canvas gizmo (GeometryEngine
// PrimitiveParams / make_primitive) was removed upstream before 3610d8bb; the Design tab
// is the CAD UI at that pin. Opening this gizmo switches to the Design tab and starts a
// sketch. Registered last before Undefined and only when enable_cad_feature is on (#15762).
class GLGizmoSketch : public GLGizmoBase
{
public:
    GLGizmoSketch(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    bool        on_is_selectable() const override;
    void        on_render() override;
    void        on_set_state() override;
    CommonGizmosDataID on_get_requirements() const override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;

private:
    void launch_design_sketch();
};

} // namespace GUI
} // namespace Slic3r

#endif // SLIC3R_CAD

#endif // slic3r_GLGizmoSketch_hpp_
