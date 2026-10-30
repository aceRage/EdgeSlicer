// Ported from OrcaSlicer (PR #15762 / #15238, AGPL-3.0); original authors as credited upstream.
// See src/slic3r/GUI/CAD/NOTICE.md.
#ifndef slic3r_GLGizmoPrimitive_hpp_
#define slic3r_GLGizmoPrimitive_hpp_

#ifdef SLIC3R_CAD

#include "GLGizmoBase.hpp"

namespace Slic3r {
namespace GUI {

// Prepare-toolbar Primitive gizmo. Same history as GLGizmoSketch: the old ImGui primitive
// builder depended on GeometryEngine APIs that are gone at 3610d8bb. Opening this gizmo
// switches to the Design tab and adds a 20 mm box (rectangle sketch + extrude), which is
// the Design-tab equivalent of Orca's one-click "Add Shape". Hidden when the CAD feature
// preference is off (#15762).
class GLGizmoPrimitive : public GLGizmoBase
{
public:
    GLGizmoPrimitive(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

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
    void launch_design_primitive();
};

} // namespace GUI
} // namespace Slic3r

#endif // SLIC3R_CAD

#endif // slic3r_GLGizmoPrimitive_hpp_
