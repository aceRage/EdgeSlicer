// Ported from OrcaSlicer (PR #15762 / #15238, AGPL-3.0); original authors as credited upstream.
// See src/slic3r/GUI/CAD/NOTICE.md.
#ifdef SLIC3R_CAD

#include "GLGizmoSketch.hpp"

#include "slic3r/GUI/CAD/DesignPanel.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MainFrame.hpp"

namespace Slic3r {
namespace GUI {

GLGizmoSketch::GLGizmoSketch(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoSketch::on_init()
{
    // K: unused as a Prepare gizmo letter (Ctrl+K is unused; K alone was free).
    m_shortcut_key = WXK_CONTROL_K;
    return true;
}

std::string GLGizmoSketch::on_get_name() const { return _u8L("Sketch"); }

bool GLGizmoSketch::on_is_activable() const
{
    return wxGetApp().is_enable_cad_feature();
}

bool GLGizmoSketch::on_is_selectable() const
{
    return wxGetApp().is_enable_cad_feature();
}

void GLGizmoSketch::on_render() {}

CommonGizmosDataID GLGizmoSketch::on_get_requirements() const
{
    return CommonGizmosDataID(0);
}

void GLGizmoSketch::launch_design_sketch()
{
    if (wxGetApp().mainframe == nullptr)
        return;
    wxGetApp().mainframe->select_tab(size_t(MainFrame::tpDesign));
    if (DesignPanel* design = DesignPanel::if_built())
        design->prepare_start_sketch();
}

void GLGizmoSketch::on_set_state()
{
    if (m_state != EState::On)
        return;
    wxGetApp().CallAfter([this] {
        launch_design_sketch();
        m_parent.reset_all_gizmos();
    });
}

void GLGizmoSketch::on_render_input_window(float x, float y, float bottom_limit)
{
    y = std::min(y, bottom_limit - ImGui::GetWindowHeight());
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin("Sketch", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove
                                  | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse
                                  | ImGuiWindowFlags_NoTitleBar);
    ImGui::TextUnformatted(_u8L("Opens the Design tab to start a sketch.").c_str());
    if (ImGui::Button(_u8L("Open Design").c_str(), {-1, 0}))
        launch_design_sketch();
    if (ImGui::Button(_u8L("Close").c_str(), {-1, 0}))
        m_parent.reset_all_gizmos();
    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace GUI
} // namespace Slic3r

#endif // SLIC3R_CAD
