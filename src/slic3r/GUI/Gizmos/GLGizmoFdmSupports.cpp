#include "GLGizmoFdmSupports.hpp"

#include "libslic3r/Model.hpp"
//BBS
#include "libslic3r/Layer.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r/Support/SupportOverhangHighlight.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/PresetBundle.hpp"

//#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <glad/gl.h>

#include <imgui/imgui_internal.h>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <chrono>
#include <sstream>

namespace Slic3r::GUI {

// The colour of painted stabilizer points (EnforcerBlockerType::STABILIZER) in this gizmo.
static const ColorRGBA STABILIZER_POINT_COLOR = { 0.62f, 0.36f, 0.90f, 1.f };
// "By layer": red where the support holds the face up, a clear yellow where it does not. Both are
// see-through over the painted mesh; the yellow stays apart from the red on light and dark filament.
static const ColorRGBA LAYER_PREVIEW_SUPPORTED_COLOR   = { 0.92f, 0.10f, 0.10f, 0.60f };
static const ColorRGBA LAYER_PREVIEW_UNSUPPORTED_COLOR = { 1.00f, 0.85f, 0.00f, 0.70f };

GLGizmoFdmSupports::GLGizmoFdmSupports(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id), m_current_tool(ImGui::CircleButtonIcon)
{
    m_tool_type = ToolType::BRUSH;
    m_cursor_type = TriangleSelector::CursorType::CIRCLE;
}

GLGizmoFdmSupports::~GLGizmoFdmSupports()
{
    stop_layer_preview_job();
}

void GLGizmoFdmSupports::on_shutdown()
{
    //BBS
    //wait the thread
    if (m_thread.joinable()) {
        Print *print = m_print_instance.print_object->print();
        if (print) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "cancel the print";
            print->cancel();
        }
        //join the thread
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "try to join thread for 2000 ms";
        auto ret = m_thread.try_join_for(boost::chrono::milliseconds(2000));
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "join thread returns "<<ret;
    }

    m_print_instance.print_object = NULL;
    m_print_instance.model_instance = NULL;

    clear_layer_preview();

    m_highlight_by_angle_threshold_deg = 0.f;
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

//BBS: add on_open
void GLGizmoFdmSupports::on_opening()
{
    m_angle_threshold_deg = 40;
    m_parent.set_slope_normal_angle(90.f - m_angle_threshold_deg);
    if (! m_parent.is_using_slope()) {
        m_parent.use_slope(true);
        m_parent.set_as_dirty();
    }
    m_print_instance.print_object = NULL;
    m_print_instance.model_instance = NULL;
    m_edit_state = state_idle;

    m_volume_ready = false;
    m_volume_valid = false;
}

std::string GLGizmoFdmSupports::on_get_name() const
{
    return _u8L("Support Painting");
}

bool GLGizmoFdmSupports::on_init()
{
    // BBS
    m_shortcut_key = WXK_CONTROL_L;

    // FIXME: maybe should be using GUI::shortkey_ctrl_prefix() or equivalent?
    const wxString ctrl  = _L("Ctrl+");
    // FIXME: maybe should be using GUI::shortkey_alt_prefix() or equivalent?
    const wxString alt   = _L("Alt+");
    const wxString shift = _L("Shift+");

    m_desc["clipping_of_view_caption"] = alt + _L("Mouse wheel");
    m_desc["clipping_of_view"]      = _L("Section view");
    m_desc["reset_direction"]       = _L("Reset direction");
    m_desc["cursor_size_caption"]   = ctrl + _L("Mouse wheel");
    m_desc["cursor_size"]           = _L("Pen size");
    m_desc["enforce_caption"]       = _L("Left mouse button");
    m_desc["enforce"]               = _L("Enforce supports");
    m_desc["block_caption"]         = _L("Right mouse button");
    m_desc["block"]                 = _L("Block supports");
    m_desc["remove_caption"]        = shift + _L("Left mouse button");
    m_desc["remove"]                = _L("Erase");
    m_desc["remove_all"]            = _L("Erase all painting");
    m_desc["highlight_by_angle"]    = _L("Threshold Angle");
    m_desc["threshold_overlap"]     = _L("Threshold Overlap");
    m_desc["gap_fill"]              = _L("Gap fill");
    m_desc["perform"]               = _L("Perform");
    m_desc["gap_area_caption"]      = ctrl + _L("Mouse wheel");
    m_desc["gap_area"]              = _L("Gap area");
    m_desc["tool_type"]             = _L("Tool type");
    m_desc["smart_fill_angle_caption"] = ctrl + _L("Mouse wheel");
    m_desc["smart_fill_angle"]      = _L("Smart fill angle");
    m_desc["on_overhangs_only"] = _L("On overhangs only");
    m_desc["stabilizer_points"]     = _L("Paint stabilizer points");
    m_desc["stabilizer_caption"]    = _L("Left mouse button");
    m_desc["stabilizer"]            = _L("Paint stabilizer points");
    m_desc["unpaint_caption"]       = _L("Right mouse button");
    m_desc["unpaint"]               = _L("Erase");
    m_desc["highlight_like_auto"]   = _L("Simulate Auto Supports");
    m_desc["highlight_by_layer"]    = _L("By layer (before slicing)");
    m_desc["layer_preview_layer"]   = _L("Layer");
    m_desc["enforce_highlighted"]   = _L("Enforce highlighted");
    m_desc["block_highlighted"]     = _L("Block highlighted");

    memset(&m_print_instance, 0, sizeof(m_print_instance));
    return true;
}

void GLGizmoFdmSupports::render_painter_gizmo()
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);
    render_layer_preview();
    //BBS: draw support volumes
    if (m_volume_ready && m_support_volume && (m_edit_state != state_generating))
    {
        // TODO: FIXME
        m_support_volume->set_render_color({0.f, 0.7f, 0.f, 0.7f});
        m_support_volume->render();
    }

    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

// BBS
bool GLGizmoFdmSupports::on_key_down_select_tool_type(int keyCode) {
    switch (keyCode)
    {
    case 'F':
        m_current_tool = ImGui::FillButtonIcon;
        break;
    case 'S':
        m_current_tool = ImGui::SphereButtonIcon;
        break;
    case 'C':
        m_current_tool = ImGui::CircleButtonIcon;
        break;
    case 'G':
        m_current_tool = ImGui::GapFillIcon;
        break;
    default:
        return false;
        break;
    }
    return true;
}

void GLGizmoFdmSupports::on_set_state()
{
    GLGizmoPainterBase::on_set_state();

    if (get_state() == On) {
        m_highlight_reset_pending = true;
        m_simulate_auto           = wxGetApp().app_config->get("support_paint_highlight_mode") != "angle";
        m_highlight_by_layer      = wxGetApp().app_config->get_bool("support_paint_highlight_by_layer");
        m_layer_preview_layer     = 0;
    }
    else if (get_state() == Off) {
        ModelObject* mo = m_c->selection_info()->model_object();
        if (mo) Slic3r::save_object_mesh(*mo);
    }
}

void GLGizmoFdmSupports::on_render_input_window(float x, float y, float bottom_limit)
{
    init_print_instance();
    if (! m_c->selection_info()->model_object())
        return;

    // BBS
    wchar_t old_tool = m_current_tool;

    // When the gizmo opens the sliders start at the object's support settings; with "Simulate Auto
    // Supports" they follow them while the gizmo is open (threshold, overlap, layer height, support
    // type, wall width, object overrides).
    update_highlight_angle_from_rule();
    update_layer_preview();
    // A setting changed in the sidebar does not redraw this canvas: look again twice a second.
    if (m_simulate_auto && !m_highlight_by_layer)
        m_parent.schedule_extra_frame(500);

    dock_setup_next_window(x, y, bottom_limit, 0.f, /*size_to_content=*/true);

    //BBS
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), dock_window_flags(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar));
    if (!dock_render_titlebar(get_name())) {
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }

    // First calculate width of all the texts that are could possibly be shown. We will decide set the dialog width based on that:
    const float space_size = m_imgui->get_style_scaling() * 8;
    const float clipping_slider_left    = m_imgui->calc_text_size(m_desc.at("clipping_of_view")).x + m_imgui->scaled(1.5f);
    const float cursor_slider_left      = m_imgui->calc_text_size(m_desc.at("cursor_size")).x + m_imgui->scaled(1.5f);
    const float gap_fill_slider_left    = m_imgui->calc_text_size(m_desc.at("gap_fill")).x + m_imgui->scaled(1.5f);
    const float highlight_slider_left   = std::max(m_imgui->calc_text_size(m_desc.at("highlight_by_angle")).x,
                                                   m_imgui->calc_text_size(m_desc.at("threshold_overlap")).x) + m_imgui->scaled(1.5f);
    const float reset_button_slider_left = m_imgui->calc_text_size(m_desc.at("reset_direction")).x + m_imgui->scaled(1.5f) + ImGui::GetStyle().FramePadding.x * 2;
    const float on_overhangs_only_width  = m_imgui->calc_text_size(m_desc["on_overhangs_only"]).x + m_imgui->scaled(1.5f);
    const float remove_btn_width        = m_imgui->calc_text_size(m_desc.at("remove_all")).x + m_imgui->scaled(1.5f);
    const float filter_btn_width        = m_imgui->calc_text_size(m_desc.at("perform")).x + m_imgui->scaled(1.5f);
    const float gap_area_txt_width = m_imgui->calc_text_size(m_desc.at("gap_area")).x + m_imgui->scaled(1.5f);
    const float smart_fill_angle_txt_width = m_imgui->calc_text_size(m_desc.at("smart_fill_angle")).x + m_imgui->scaled(1.5f);
    const float buttons_width           = remove_btn_width + filter_btn_width + m_imgui->scaled(1.5f);
    const float empty_button_width      = m_imgui->calc_button_size("").x;

    const float tips_width           = m_imgui->calc_text_size(_L("Auto support threshold angle: ") + " 90 ").x + m_imgui->scaled(1.5f);
    const float minimal_slider_width = m_imgui->scaled(4.f);

    float caption_max    = 0.f;
    float total_text_max = 0.f;
    for (const auto &t : std::array<std::string, 5>{"enforce", "block", "remove", "cursor_size", "clipping_of_view"}) {
        caption_max    = std::max(caption_max, m_imgui->calc_text_size(m_desc[t + "_caption"]).x);
        total_text_max = std::max(total_text_max, m_imgui->calc_text_size(m_desc[t]).x);
    }
    total_text_max += caption_max + m_imgui->scaled(1.f);
    caption_max += m_imgui->scaled(1.f);

    const float sliders_left_width = std::max(gap_area_txt_width, std::max(smart_fill_angle_txt_width, std::max(reset_button_slider_left, std::max(std::max(cursor_slider_left, clipping_slider_left), std::max(highlight_slider_left, gap_fill_slider_left)))));
    const float slider_icon_width  = m_imgui->get_slider_icon_size().x;
    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;

    const float sliders_width = m_imgui->scaled(7.0f);
    const float drag_left_width = ImGui::GetStyle().WindowPadding.x + sliders_left_width + sliders_width - space_size;

    float drag_pos_times     = 0.7;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("tool_type"));
    std::array<wchar_t, 4> tool_ids = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::FillButtonIcon, ImGui::GapFillIcon };
    std::array<wchar_t, 4> icons;
    if (m_is_dark_mode)
        icons = { ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon, ImGui::FillButtonDarkIcon, ImGui::GapFillDarkIcon };
    else
        icons = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::FillButtonIcon, ImGui::GapFillIcon };

    std::array<wxString, 4> tool_tips = { _L("Circle"), _L("Sphere"), _L("Fill"), _L("Gap Fill") };
    for (int i = 0; i < tool_ids.size(); i++) {
        std::string  str_label = std::string("##");
        std::wstring btn_name = icons[i] + boost::nowide::widen(str_label);

        if (i != 0) ImGui::SameLine((empty_button_width + m_imgui->scaled(1.75f)) * i + m_imgui->scaled(1.3f));

        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));                     // ORCA Removes button background on dark mode
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));                       // ORCA: Fixes icon rendered without colors while using Light theme
        if (m_current_tool == tool_ids[i]) {
            ImGui::PushStyleColor(ImGuiCol_Button,          ImVec4(0.f, 0.59f, 0.53f, 0.25f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,   ImVec4(0.f, 0.59f, 0.53f, 0.25f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,    ImVec4(0.f, 0.59f, 0.53f, 0.30f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_Border,          ImGuiWrapper::COL_ORCA);            // ORCA use orca color for border on selected tool / brush
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0);
        }
        bool btn_clicked = ImGui::Button(into_u8(btn_name).c_str());
        if (m_current_tool == tool_ids[i])
        {
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(1);

        if (btn_clicked && m_current_tool != tool_ids[i]) {
            m_current_tool = tool_ids[i];
            for (auto& triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        }

        if (ImGui::IsItemHovered()) {
            m_imgui->tooltip(tool_tips[i], max_tooltip_width);
        }
    }

    m_imgui->bbl_checkbox(m_desc["on_overhangs_only"], m_paint_on_overhangs_only);
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(format_wxstr(_L("Allows painting only on facets selected by: \"%1%\""), m_desc["highlight_by_angle"]), max_tooltip_width);
    // Side stabilizers (Support/Stabilizers.hpp): the same brushes paint the spots where a stabilizer
    // strut must always touch the part. Left paints them, right erases.
    m_imgui->bbl_checkbox(m_desc["stabilizer_points"], m_paint_stabilizers);
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Paint spots where a side stabilizer must touch the part. Left mouse button paints, right mouse "
                            "button erases. Used when the object's side stabilizers are Auto (on top of the rings) or "
                            "Manual (only these)."),
                         max_tooltip_width);
    ImGui::Separator();

    if (m_current_tool != old_tool)
        this->tool_changed(old_tool, m_current_tool);

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    if (m_current_tool == ImGui::CircleButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::CIRCLE;
        m_tool_type = ToolType::BRUSH;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");
    } else if (m_current_tool == ImGui::SphereButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::SPHERE;
        m_tool_type = ToolType::BRUSH;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");
    } else if (m_current_tool == ImGui::FillButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type = ToolType::SMART_FILL;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("smart_fill_angle"));
        std::string format_str = std::string("%.f") + I18N::translate_utf8("", "Face angle threshold, placed after the number with no whitespace in between.");
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        if (m_imgui->bbl_slider_float_style("##smart_fill_angle", &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, format_str.data(), 1.0f, true))
            for (auto& triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##smart_fill_angle_input", &m_smart_fill_angle, 0.05f, 0.0f, 0.0f, "%.2f");
    } else if (m_current_tool == ImGui::GapFillIcon) {
        m_tool_type = ToolType::GAP_FILL;
        m_cursor_type = TriangleSelector::CursorType::POINTER;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["gap_area"]);
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        std::string format_str = std::string("%.2f") + I18N::translate_utf8("", "Triangle patch area threshold,""triangle patch will be merged to neighbor if its area is less than threshold");
        m_imgui->bbl_slider_float_style("##gap_area", &TriangleSelectorPatch::gap_area, TriangleSelectorPatch::GapAreaMin, TriangleSelectorPatch::GapAreaMax, format_str.data(), 1.0f, true);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##gap_area_input", &TriangleSelectorPatch::gap_area, 0.05f, 0.0f, 0.0f, "%.2f");
    }

    // "Threshold Angle" and "Threshold Overlap": the support settings the highlight uses. With
    // "Simulate Auto Supports" they mirror the object's effective settings and are read-only;
    // otherwise they are free preview values.
    const bool thresholds_read_only = m_simulate_auto;
    const ModelObject *highlight_mo = m_c->selection_info()->model_object();
    const SupportOverhangRule highlight_rule = current_rule(*highlight_mo);
    const wxString source_text = threshold_source_text(*highlight_mo);
    bool thresholds_changed = false;
    auto threshold_row = [&](const wxString &label, const char *id, float *value, float vmax, const char *fmt, const char *drag_fmt,
                             bool dimmed, const wxString &tip) {
        float position_before_text_y = ImGui::GetCursorPos().y;
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::AlignTextToFramePadding();
        float position_after_text_y = ImGui::GetCursorPos().y;
        ImGui::SameLine(sliders_left_width);
        // Makes slider to be aligned to bottom of the multi-line text.
        float slider_start_position_y = std::max(position_before_text_y, position_after_text_y - m_imgui->get_slider_float_height());
        ImGui::SetCursorPosY(slider_start_position_y);
        ImGui::PushItemWidth(sliders_width);
        const bool disabled = thresholds_read_only;
        if (disabled)
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        if (disabled || dimmed)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
        bool changed = m_imgui->bbl_slider_float_style(std::string("##") + id, value, 0.f, vmax, fmt, 1.0f, true, wxString());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            m_imgui->tooltip(tip + "\n\n" + source_text + "\n\n" + highlight_rule_description(), max_tooltip_width);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        changed |= ImGui::BBLDragFloat((std::string("##") + id + "_input").c_str(), value, 0.1f, 0.0f, vmax, drag_fmt);
        if (disabled || dimmed)
            ImGui::PopStyleVar();
        if (disabled)
            ImGui::PopItemFlag();
        if (changed && ! disabled) {
            *value = std::clamp(std::round(*value), 0.f, vmax);
            thresholds_changed = true;
        }
    };
    threshold_row(m_desc["highlight_by_angle"], "threshold_angle", &m_threshold_angle, 90.f, "%.0f", "%.0f", false,
                  _L("The support threshold angle (support_threshold_angle). 0 means: normal supports use the threshold overlap, "
                     "tree supports 30°. Faces get the tint the automatic supports' layer rule gives this value."));
    // The overlap only counts for normal supports with a threshold angle of 0.
    const bool overlap_applies = highlight_rule.source == SupportOverhangRule::Source::Overlap;
    threshold_row(m_desc["threshold_overlap"], "threshold_overlap", &m_threshold_overlap, 100.f, "%.0f%%", "%.0f", ! overlap_applies,
                  _L("The support threshold overlap (support_threshold_overlap), in percent of the outer wall width. Only used by "
                     "normal supports when the threshold angle is 0."));
    if (thresholds_changed) {
        update_highlight_angle_from_rule();
        if (!m_parent.is_using_slope() && !(m_highlight_by_layer && m_layer_preview))
            m_parent.use_slope(true);
        m_parent.set_as_dirty();
    }

    if (m_imgui->bbl_checkbox(m_desc["highlight_like_auto"], m_simulate_auto)) {
        wxGetApp().app_config->set("support_paint_highlight_mode", m_simulate_auto ? "auto" : "angle");
        update_highlight_angle_from_rule();
        if (!m_parent.is_using_slope() && !(m_highlight_by_layer && m_layer_preview))
            m_parent.use_slope(true);
        m_parent.set_as_dirty();
    }
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Use the support settings in effect for this object (its own override, else the process) and follow them "
                            "when they change. Off, the threshold angle and overlap above are free preview values.") +
                             "\n\n" + source_text + "\n\n" + highlight_rule_description(),
                         max_tooltip_width);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.7f);
    m_imgui->text_wrapped(source_text, max_tooltip_width);
    ImGui::PopStyleVar();

    // "By layer (before slicing)": the generator's own decision.
    if (m_imgui->bbl_checkbox(m_desc["highlight_by_layer"], m_highlight_by_layer)) {
        wxGetApp().app_config->set_bool("support_paint_highlight_by_layer", m_highlight_by_layer);
        if (m_highlight_by_layer) {
            m_layer_preview_key.clear(); // start a job on the next frame
        } else {
            clear_layer_preview();
            m_parent.use_slope(true);
        }
        m_parent.set_as_dirty();
    }
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Run the support generator's own overhang detection on this object in the background, with the support "
                            "type and settings in effect for it (its overrides, bridges, small overhang removal, build plate only, "
                            "painted enforcers and blockers), and mark the result on the faces: red is held up by the support, yellow is "
                            "an overhang the generator leaves (a bridge, a small overhang, above the object with \"On build plate only\"). "
                            "Tree and organic supports are then placed as well, and red becomes only what their branches and tips "
                            "reach. Supports switched off or manual are simulated as automatic."),
                         max_tooltip_width);
    if (m_highlight_by_layer) {
        // The colour key: a swatch the height of a text line (follows the DPI scaling), outlined in
        // the theme's text colour so it reads on the light and the dark theme.
        auto swatch = [this](const ColorRGBA &color, const wxString &label) {
            const float  size = ImGui::GetTextLineHeight();
            const ImVec2 pos  = ImGui::GetCursorScreenPos();
            const ImVec2 off(0.f, std::max(0.f, 0.5f * (ImGui::GetFrameHeight() - size)));
            ImDrawList  *draw = ImGui::GetWindowDrawList();
            const float  round = 2.f * m_imgui->get_style_scaling();
            draw->AddRectFilled(ImVec2(pos.x + off.x, pos.y + off.y), ImVec2(pos.x + off.x + size, pos.y + off.y + size),
                                ImGuiWrapper::to_ImU32(ColorRGBA(color.r(), color.g(), color.b(), 1.f)), round);
            draw->AddRect(ImVec2(pos.x + off.x, pos.y + off.y), ImVec2(pos.x + off.x + size, pos.y + off.y + size),
                          ImGui::GetColorU32(ImGuiCol_Text, 0.45f), round);
            ImGui::Dummy(ImVec2(size, ImGui::GetFrameHeight()));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            m_imgui->text(label);
        };
        swatch(LAYER_PREVIEW_SUPPORTED_COLOR, _L("Supported"));
        ImGui::SameLine(0.f, m_imgui->scaled(1.5f));
        swatch(LAYER_PREVIEW_UNSUPPORTED_COLOR, _L("Unsupported"));
        const bool layer_preview_running = m_layer_preview_job && !m_layer_preview_job->done;
        if (m_layer_preview_too_big) {
            m_imgui->text_wrapped(format_wxstr(_L("Too many faces for the layer preview (%1%); the angle highlight is shown."), m_layer_preview_facets),
                                  max_tooltip_width);
        } else if (layer_preview_running && !(m_layer_preview && !m_layer_preview->slice_z.empty())) {
            m_imgui->text(_L("Computing the layers..."));
        } else if (m_layer_preview && !m_layer_preview->slice_z.empty()) {
            const SupportOverhangPreview &p = *m_layer_preview;
            const int n = int(p.slice_z.size());
            if (layer_preview_running)
                m_imgui->text_wrapped(m_layer_preview_intermediate ? _L("Placing the tree supports... Until then red shows what the tree detects.") :
                                                                     _L("Updating..."), max_tooltip_width);
            else if (p.from_generated_support)
                m_imgui->text_wrapped(format_wxstr(_L("%1% of %2% layers overhang. Red is what the placed tree reaches (%3% ms)."),
                                                   p.layers_with_overhangs(), n, int(std::round(p.slice_ms + p.generate_ms + p.map_ms))),
                                      max_tooltip_width);
            else
                m_imgui->text(format_wxstr(_L("%1% of %2% layers overhang (%3% ms)"), p.layers_with_overhangs(), n,
                                           int(std::round(p.slice_ms + p.detect_ms + p.map_ms))));
            // The layer stepper: 0 shows the whole object, k cuts it at layer k and draws that
            // layer's overhang areas on the cut.
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc["layer_preview_layer"]);
            ImGui::SameLine(sliders_left_width);
            ImGui::PushItemWidth(sliders_width);
            float layer = float(m_layer_preview_layer);
            const std::string layer_fmt = m_layer_preview_layer == 0 ? into_u8(_L("All")) :
                (boost::format("%1% (%2$.2f mm)") % m_layer_preview_layer % p.print_z[size_t(m_layer_preview_layer - 1)]).str();
            bool layer_changed = m_imgui->bbl_slider_float_style("##layer_preview_layer", &layer, 0.f, float(n), layer_fmt.c_str(), 1.0f, true,
                                                                  _L("0 shows the whole object. A layer cuts the object there and draws that layer's overhang areas on the cut."));
            ImGui::SameLine(drag_left_width);
            ImGui::PushItemWidth(1.5 * slider_icon_width);
            layer_changed |= ImGui::BBLDragFloat("##layer_preview_layer_input", &layer, 1.f, 0.0f, float(n), "%.0f");
            if (layer_changed)
                set_layer_preview_layer(std::clamp(int(std::round(layer)), 0, n));
            if (m_imgui->button(m_desc["enforce_highlighted"]))
                paint_layer_preview_facets(false);
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_L("Paint every highlighted face as a support enforcer (one undo step)."), max_tooltip_width);
            ImGui::SameLine();
            if (m_imgui->button(m_desc["block_highlighted"]))
                paint_layer_preview_facets(true);
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_L("Paint every highlighted face as a support blocker (one undo step)."), max_tooltip_width);
        } else if (m_layer_preview_job == nullptr && m_layer_preview == nullptr && !m_layer_preview_key.empty()) {
            m_imgui->text(_L("No layers to show."));
        }
    }

    if (m_current_tool != ImGui::GapFillIcon) {
        ImGui::Separator();
        if (m_c->object_clipper()->get_position() == 0.f) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc.at("clipping_of_view"));
        }
        else {
            if (m_imgui->button(m_desc.at("reset_direction"))) {
                wxGetApp().CallAfter([this]() {
                        m_c->object_clipper()->set_position_by_ratio(-1., false);
                    });
            }
        }

        auto clp_dist = float(m_c->object_clipper()->get_position());
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        bool b_bbl_slider_float = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);

        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        bool b_drag_input = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");

        if (b_bbl_slider_float || b_drag_input) m_c->object_clipper()->set_position_by_ratio(clp_dist, true);
    }

    ImGui::Separator();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    show_tooltip_information(caption_max, x, get_cur_y);

    float f_scale =m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    ImGui::SameLine();

    // Perform button is for gap fill
    if (m_current_tool == ImGui::GapFillIcon) {
        if (m_imgui->button(m_desc.at("perform"))) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset selection", UndoRedo::SnapshotType::GizmoAction);

            for (int i = 0; i < m_triangle_selectors.size(); i++) {
                TriangleSelectorPatch* ts_mm = dynamic_cast<TriangleSelectorPatch*>(m_triangle_selectors[i].get());
                ts_mm->update_selector_triangles();
                ts_mm->request_update_render_data(true);
            }
            update_model_object();
            m_parent.set_as_dirty();
        }
    }

    ImGui::SameLine();

    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset selection", UndoRedo::SnapshotType::GizmoAction);
        ModelObject *        mo  = m_c->selection_info()->model_object();
        int                  idx = -1;
        for (ModelVolume *mv : mo->volumes)
            if (mv->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data(true);
            }

        update_model_object();
        m_parent.set_as_dirty();
    }
    ImGui::PopStyleVar(2);

    GizmoImguiEnd();

    // BBS
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoFdmSupports::tool_changed(wchar_t old_tool, wchar_t new_tool)
{
    if ((old_tool == ImGui::GapFillIcon && new_tool == ImGui::GapFillIcon) ||
        (old_tool != ImGui::GapFillIcon && new_tool != ImGui::GapFillIcon))
        return;

    for (auto& selector_ptr : m_triangle_selectors) {
        TriangleSelectorPatch* tsp = dynamic_cast<TriangleSelectorPatch*>(selector_ptr.get());
        tsp->set_filter_state(new_tool == ImGui::GapFillIcon);
    }
}

void GLGizmoFdmSupports::show_tooltip_information(float caption_max, float x, float y)
{
    ImTextureID normal_id = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += m_imgui->calc_text_size(std::string_view{": "}).x + 15.f;

    float  scale       = m_parent.get_scale();
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 0}); // ORCA: Dont add padding
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &caption_max](const wxString &caption, const wxString &text) {
            // BBS
            m_imgui->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        std::vector<std::string> tip_items;
        switch (m_tool_type) {
            case ToolType::BRUSH:
                tip_items = m_paint_stabilizers ? std::vector<std::string>{"stabilizer", "unpaint", "remove", "cursor_size", "clipping_of_view"} :
                                                  std::vector<std::string>{"enforce", "block", "remove", "cursor_size", "clipping_of_view"};
                break;
            case ToolType::BUCKET_FILL:
                break;
            case ToolType::SMART_FILL:
                tip_items = m_paint_stabilizers ? std::vector<std::string>{"stabilizer", "unpaint", "remove", "smart_fill_angle", "clipping_of_view"} :
                                                  std::vector<std::string>{"enforce", "block", "remove", "smart_fill_angle", "clipping_of_view"};
                break;
            case ToolType::GAP_FILL:
                tip_items = {"gap_area"};
                break;
            default:
                break;
        }
        for (const auto &t : tip_items) draw_text_with_caption(m_desc.at(t + "_caption") + ": ", m_desc.at(t));

        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

// The threshold angle and overlap the object's supports really use: its own override, else the process.
GLGizmoFdmSupports::ThresholdSource GLGizmoFdmSupports::effective_thresholds(const ModelObject &mo) const
{
    ThresholdSource out;
    const DynamicPrintConfig &process = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    const DynamicPrintConfig &object  = mo.config.get();
    if (const auto *opt = object.option<ConfigOptionInt>("support_threshold_angle")) {
        out.angle             = opt->value;
        out.angle_from_object = true;
    } else if (const auto *process_angle = process.option<ConfigOptionInt>("support_threshold_angle"))
        out.angle = process_angle->value;
    const ConfigOptionFloatOrPercent *overlap = object.option<ConfigOptionFloatOrPercent>("support_threshold_overlap");
    out.overlap_from_object = overlap != nullptr;
    if (overlap == nullptr)
        overlap = process.option<ConfigOptionFloatOrPercent>("support_threshold_overlap");
    if (overlap != nullptr) {
        if (overlap->percent)
            out.overlap_pct = overlap->value;
        else {
            // An absolute overlap, shown as the share of this object's outer wall.
            out.overlap_absolute = true;
            const double wall    = support_overhang_rule_for(&mo).outer_wall_width;
            out.overlap_pct      = wall > 0. ? 100. * overlap->value / wall : 50.;
        }
    }
    return out;
}

SupportOverhangRule GLGizmoFdmSupports::current_rule(const ModelObject &mo) const
{
    const PresetBundle &bundle = *wxGetApp().preset_bundle;
    DynamicPrintConfig cfg = support_overhang_rule_config(bundle.prints.get_edited_preset().config, bundle.printers.get_edited_preset().config,
                                                          bundle.project_config, bundle.filament_presets.size(), &mo.config.get());
    if (! m_simulate_auto) {
        // Free preview values in place of the settings.
        cfg.set_key_value("support_threshold_angle", new ConfigOptionInt(int(std::lround(m_threshold_angle))));
        cfg.set_key_value("support_threshold_overlap", new ConfigOptionFloatOrPercent(double(m_threshold_overlap), true));
    }
    return support_overhang_rule(cfg);
}

wxString GLGizmoFdmSupports::threshold_source_text(const ModelObject &mo) const
{
    if (! m_simulate_auto)
        return _L("Preview values, not saved to the settings");
    const ThresholdSource src = effective_thresholds(mo);
    if (src.angle_from_object && src.overlap_from_object)
        return _L("From this object's settings");
    if (src.angle_from_object)
        return _L("Angle from this object's settings, overlap from the process");
    if (src.overlap_from_object)
        return _L("Angle from the process, overlap from this object's settings");
    return _L("From the process settings");
}

void GLGizmoFdmSupports::update_highlight_angle_from_rule()
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;
    // Simulate Auto Supports mirrors the settings; otherwise the sliders start there once.
    if (m_simulate_auto || m_highlight_reset_pending) {
        const ThresholdSource src = effective_thresholds(*mo);
        m_threshold_angle   = float(src.angle);
        m_threshold_overlap = float(src.overlap_pct);
    }
    m_highlight_reset_pending = false;
    // The face tint: the angle a planar slope needs to get support under those settings. The
    // sliders show the settings themselves (no +1); the rule adds the generators' +1.
    const float angle = float(std::clamp(current_rule(*mo).effective_angle_deg(), 0., 90.));
    if (std::abs(angle - m_highlight_by_angle_threshold_deg) > 1e-3f) {
        m_highlight_by_angle_threshold_deg = angle;
        m_parent.set_slope_normal_angle(90.f - angle);
        m_parent.set_as_dirty();
    }
}

wxString GLGizmoFdmSupports::highlight_rule_description() const
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return {};
    const SupportOverhangRule rule = current_rule(*mo);
    const double h     = rule.layer_height;
    const double angle = rule.effective_angle_deg();
    auto deg = [](double v) { return wxString::Format("%.1f", v); };
    auto mm  = [](double v) { return wxString::Format("%.3g", v); };
    const wxString type = rule.tree ? _L("Tree supports") : _L("Normal supports");

    wxString out = format_wxstr(_L("Automatic supports hold up faces flatter than %1%° at %2% mm layers."), deg(angle), mm(h));
    out += "\n";
    switch (rule.source) {
    case SupportOverhangRule::Source::Angle:
        out += format_wxstr(_L("%1%, threshold angle %2%° (inclusive): each layer may stick out %3% mm past the layer below."), type,
                            rule.threshold_angle, mm(rule.lower_layer_offset(h)));
        break;
    case SupportOverhangRule::Source::Overlap:
        out += format_wxstr(_L("%1%, threshold overlap %2% mm of a %3% mm outer wall: each layer may stick out %4% mm past the layer "
                               "below, so the angle changes with the layer height."),
                            type, mm(rule.overlap_mm), mm(rule.outer_wall_width), mm(rule.lower_layer_offset(h)));
        break;
    case SupportOverhangRule::Source::TreeFallback:
        out += format_wxstr(_L("%1%, threshold angle 0: tree supports use 30°; the threshold overlap does not apply."), type);
        break;
    }
    if (rule.capped_by_min_band(h))
        out += "\n" + format_wxstr(_L("Normal supports skip overhangs narrower than %1% mm, so a steeper threshold has no effect here."),
                                   mm(rule.min_band_width()));

    // A variable layer height profile or layer ranges change the answer per layer; one angle can
    // only show the object's layer height. Report the spread.
    double h_min = h, h_max = h;
    const std::vector<coordf_t> profile = mo->layer_height_profile.get();
    for (size_t i = 1; i < profile.size(); i += 2) {
        h_min = std::min(h_min, double(profile[i]));
        h_max = std::max(h_max, double(profile[i]));
    }
    for (const auto &range : mo->layer_config_ranges)
        if (const auto *opt = range.second.get().option<ConfigOptionFloat>("layer_height"); opt != nullptr && opt->value > 0.) {
            h_min = std::min(h_min, opt->value);
            h_max = std::max(h_max, opt->value);
        }
    if (h_max - h_min > 1e-6) {
        const double a_min = rule.effective_angle_deg(h_min), a_max = rule.effective_angle_deg(h_max);
        if (std::abs(a_max - a_min) > 0.05)
            out += "\n" + format_wxstr(_L("Variable layer height %1%-%2% mm: %3%°-%4%°. The highlight uses %5% mm."), mm(h_min), mm(h_max),
                                       deg(std::min(a_min, a_max)), deg(std::max(a_min, a_max)), mm(h));
    }

    if (! rule.enable_support)
        out += "\n" + _L("Supports are off for this object: this shows what automatic supports would hold up.");
    else if (! rule.auto_support)
        out += "\n" + _L("The support type is manual: only painted areas get support. This shows what automatic supports would hold up.");
    out += "\n" + _L("Not shown by a face angle: bridges, sharp tails, small overhang removal, build plate only and convex "
                     "corners. \"By layer (before slicing)\" shows the generator's own decision.");
    return out;
}

// ---------------------------------------------------------------------------------------------
// "By layer (before slicing)" (phase 3; since phase 5 the generator's own detection)
// ---------------------------------------------------------------------------------------------

// Above this many faces the background job is skipped and the angle highlight stays.
static constexpr size_t LAYER_PREVIEW_MAX_FACETS = 3000000;

static long long layer_preview_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The geometry and the highlight's own inputs: the object's parts, their placement, the painted
// enforcers / blockers, the layer height profile and ranges, and the free threshold values.
std::string GLGizmoFdmSupports::layer_preview_key(const ModelObject &mo, int instance_idx) const
{
    std::ostringstream ss;
    ss.precision(9);
    auto matrix = [&ss](const Transform3d &t) {
        for (int r = 0; r < 3; ++ r)
            for (int c = 0; c < 4; ++ c)
                ss << t(r, c) << ',';
    };
    if (instance_idx >= 0 && size_t(instance_idx) < mo.instances.size())
        matrix(mo.instances[instance_idx]->get_matrix());
    for (const ModelVolume *mv : mo.volumes) {
        ss << '|' << int(mv->type()) << ':' << mv->get_mesh_shared_ptr().get() << ':' << mv->supported_facets.timestamp() << ':';
        matrix(mv->get_matrix());
        // A part's own settings (wall widths, filaments) change its slices' flow.
        for (const std::string &key : mv->config.keys())
            ss << key << '=' << mv->config.option(key)->serialize() << ';';
    }
    ss << "|profile:";
    for (coordf_t v : mo.layer_height_profile.get())
        ss << v << ',';
    for (const auto &range : mo.layer_config_ranges) {
        ss << "|range:" << range.first.first << '-' << range.first.second << ':';
        for (const std::string &key : range.second.keys())
            ss << key << '=' << range.second.option(key)->serialize() << ';';
    }
    ss << "|simulate:" << m_simulate_auto;
    if (! m_simulate_auto)
        ss << ':' << std::lround(m_threshold_angle) << ':' << std::lround(m_threshold_overlap);
    return ss.str();
}

// Any print / printer / project / object setting may change the generator's decision (support
// type and style, thresholds, bridges, small-overhang removal, wall widths, layer heights ...):
// compare them with what the last job saw. A few hundred option compares, cheap per frame.
bool GLGizmoFdmSupports::layer_preview_settings_changed(const ModelObject &mo)
{
    const PresetBundle &bundle = *wxGetApp().preset_bundle;
    const bool changed = bundle.prints.get_edited_preset().config != m_layer_preview_print_cfg ||
                         bundle.printers.get_edited_preset().config != m_layer_preview_printer_cfg ||
                         bundle.project_config != m_layer_preview_project_cfg || mo.config.get() != m_layer_preview_object_cfg ||
                         bundle.filament_presets != m_layer_preview_filaments;
    if (changed) {
        ++ m_layer_preview_settings_rev;
        m_layer_preview_print_cfg   = bundle.prints.get_edited_preset().config;
        m_layer_preview_printer_cfg = bundle.printers.get_edited_preset().config;
        m_layer_preview_project_cfg = bundle.project_config;
        m_layer_preview_object_cfg  = mo.config.get();
        m_layer_preview_filaments   = bundle.filament_presets;
    }
    return changed;
}

void GLGizmoFdmSupports::stop_layer_preview_job()
{
    if (m_layer_preview_job) {
        m_layer_preview_job->cancel = true;
        if (m_layer_preview_job->print)
            m_layer_preview_job->print->cancel();
    }
    if (m_layer_preview_thread.joinable())
        m_layer_preview_thread.join();
    m_layer_preview_job.reset();
}

void GLGizmoFdmSupports::clear_layer_preview()
{
    stop_layer_preview_job();
    if (m_layer_preview_layer != 0 && m_c != nullptr && m_c->object_clipper() != nullptr)
        m_c->object_clipper()->set_position_by_ratio(0., false);
    m_layer_preview_layer = 0;
    m_layer_preview.reset();
    m_layer_preview_intermediate = false;
    m_layer_surface_supported.reset();
    m_layer_surface_unsupported.reset();
    m_layer_cut_supported.reset();
    m_layer_cut_unsupported.reset();
    m_layer_preview_key.clear();
    m_layer_preview_pending_key.clear();
    m_layer_preview_print_cfg.clear();
    m_layer_preview_too_big = false;
}

void GLGizmoFdmSupports::update_layer_preview()
{
    if (!m_highlight_by_layer)
        return;
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;
    const int instance_idx = m_parent.get_selection().get_instance_idx();
    if (instance_idx < 0 || size_t(instance_idx) >= mo->instances.size())
        return;

    // A tree job's detection, while its tree is placed: show it meanwhile.
    if (m_layer_preview_job && !m_layer_preview_job->done && m_layer_preview_job->detected_ready && !m_layer_preview_job->detected_shown) {
        std::unique_ptr<SupportOverhangPreview> detected;
        {
            std::lock_guard<std::mutex> lock(m_layer_preview_job->mutex);
            detected = std::move(m_layer_preview_job->detected);
        }
        m_layer_preview_job->detected_shown = true;
        if (detected) {
            m_layer_preview              = std::move(detected);
            m_layer_preview_intermediate = true;
            rebuild_layer_preview_models();
            m_parent.use_slope(false);
            set_layer_preview_layer(m_layer_preview_layer > int(m_layer_preview->slice_z.size()) ? 0 : m_layer_preview_layer);
            m_parent.set_as_dirty();
        }
    }
    // A finished job: show it.
    if (m_layer_preview_job && m_layer_preview_job->done) {
        if (m_layer_preview_thread.joinable())
            m_layer_preview_thread.join();
        if (m_layer_preview_job->result) {
            m_layer_preview = std::move(m_layer_preview_job->result);
            m_layer_preview_intermediate = false;
            rebuild_layer_preview_models();
            // The overlay replaces the angle tint.
            m_parent.use_slope(false);
            if (m_layer_preview_layer > int(m_layer_preview->slice_z.size()))
                set_layer_preview_layer(0);
            else
                set_layer_preview_layer(m_layer_preview_layer);
            const SupportOverhangPreview &p = *m_layer_preview;
            BOOST_LOG_TRIVIAL(info) << "Support layer preview: " << p.slice_z.size() << " layers, steps " << p.slice_ms << " ms, tree "
                                    << p.generate_ms << " ms, map " << p.map_ms << " ms";
        } else if (!m_layer_preview_job->error.empty()) {
            BOOST_LOG_TRIVIAL(error) << "Support layer preview failed: " << m_layer_preview_job->error;
        }
        m_layer_preview_job.reset();
        m_parent.set_as_dirty();
    }

    // Any change of the object, its settings or the highlight's own values starts a new job after
    // a short pause (a dragged slider does not restart it every frame). The settings are compared
    // every frame, so a change in the sidebar shows up without touching the gizmo.
    layer_preview_settings_changed(*mo);
    const std::string key = layer_preview_key(*mo, instance_idx) + "|settings:" + std::to_string(m_layer_preview_settings_rev);
    if (key != m_layer_preview_key && key != m_layer_preview_pending_key) {
        m_layer_preview_pending_key = key;
        m_layer_preview_pending_ms  = layer_preview_now_ms();
    }
    const bool pending = ! m_layer_preview_pending_key.empty() && m_layer_preview_pending_key != m_layer_preview_key;
    if (pending && layer_preview_now_ms() - m_layer_preview_pending_ms >= 250) {
        m_layer_preview_key = m_layer_preview_pending_key;
        stop_layer_preview_job();

        m_layer_preview_facets = 0;
        for (const ModelVolume *mv : mo->volumes)
            if (mv->is_model_part())
                m_layer_preview_facets += mv->mesh().its.indices.size();
        m_layer_preview_too_big = m_layer_preview_facets > LAYER_PREVIEW_MAX_FACETS;
        if (m_layer_preview_too_big) {
            m_layer_preview.reset();
            m_layer_surface_supported.reset();
            m_layer_surface_unsupported.reset();
            m_parent.use_slope(true);
            return;
        }

        // A private copy of the object alone (its config carries its overrides; meshes are shared,
        // immutable), placed by the shown instance, and the full config. With free threshold values
        // they replace the object's own.
        auto job   = std::make_shared<LayerPreviewJob>();
        job->model = std::make_unique<Model>();
        ModelObject *copy = job->model->add_object(*mo);
        copy->clear_instances();
        copy->add_instance(*mo->instances[instance_idx]);
        job->config = wxGetApp().preset_bundle->full_config();
        if (! m_simulate_auto) {
            DynamicPrintConfig object_config = copy->config.get();
            object_config.set_key_value("support_threshold_angle", new ConfigOptionInt(int(std::lround(m_threshold_angle))));
            object_config.set_key_value("support_threshold_overlap", new ConfigOptionFloatOrPercent(double(m_threshold_overlap), true));
            copy->config.assign_config(std::move(object_config));
        }
        job->print = std::make_unique<Print>();
        m_layer_preview_job = job;
        m_layer_preview_thread = std::thread([job]() {
            try {
                SupportOverhangPreview result = support_overhang_preview_exact(*job->print, *job->model, job->config, true,
                    [&job](SupportOverhangPreview &&detected) {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        job->detected       = std::make_unique<SupportOverhangPreview>(std::move(detected));
                        job->detected_ready = true;
                    });
                if (! job->cancel)
                    job->result = std::make_unique<SupportOverhangPreview>(std::move(result));
            } catch (const CanceledException &) {
            } catch (const std::exception &ex) {
                job->error = ex.what();
            }
            job->done = true;
        });
    }
    // Look again for the result, the debounce and changes made outside the gizmo.
    m_parent.schedule_extra_frame(m_layer_preview_job || pending ? 100 : 500);
}

void GLGizmoFdmSupports::rebuild_layer_preview_models()
{
    m_layer_surface_supported.reset();
    m_layer_surface_unsupported.reset();
    if (!m_layer_preview)
        return;
    // The faces cut where each layer's area ends (world coordinates): a face only partly on the
    // support shows only partly red.
    auto build = [](const std::vector<Vec3f> &triangles, GLModel &model, const ColorRGBA &color) {
        if (triangles.size() < 3)
            return;
        GLModel::Geometry geometry;
        geometry.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        geometry.reserve_vertices(triangles.size());
        geometry.reserve_indices(triangles.size());
        unsigned int next = 0;
        for (size_t k = 0; k + 2 < triangles.size(); k += 3, next += 3) {
            geometry.add_vertex(triangles[k]);
            geometry.add_vertex(triangles[k + 1]);
            geometry.add_vertex(triangles[k + 2]);
            geometry.add_triangle(next, next + 1, next + 2);
        }
        model.init_from(std::move(geometry));
        model.set_color(color);
    };
    build(m_layer_preview->supported_surface, m_layer_surface_supported, LAYER_PREVIEW_SUPPORTED_COLOR);
    build(m_layer_preview->unsupported_surface, m_layer_surface_unsupported, LAYER_PREVIEW_UNSUPPORTED_COLOR);
}

void GLGizmoFdmSupports::set_layer_preview_layer(int layer)
{
    const bool was_cut = m_layer_preview_layer != 0;
    m_layer_preview_layer = layer;
    m_layer_cut_supported.reset();
    m_layer_cut_unsupported.reset();
    if (layer == 0 || !m_layer_preview || size_t(layer) > m_layer_preview->print_z.size()) {
        if (was_cut)
            m_c->object_clipper()->set_position_by_ratio(0., false);
        m_parent.set_as_dirty();
        return;
    }
    // Cut the object at the top of the layer (keep what is below) and draw the layer's areas there.
    const size_t i = size_t(layer - 1);
    const double z = m_layer_preview->print_z[i];
    m_c->object_clipper()->set_range_and_pos(Vec3d::UnitZ(), z, 0.5);
    auto build = [z](const ExPolygons &areas, GLModel &model, const ColorRGBA &color) {
        if (areas.empty())
            return;
        GLModel::Geometry geometry;
        geometry.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        const std::vector<Vec3d> triangles = triangulate_expolygons_3d(areas, z + 0.02, NORMALS_UP);
        unsigned int next = 0;
        for (const Vec3d &p : triangles) {
            geometry.add_vertex(Vec3f(p.cast<float>()));
            if (++ next % 3 == 0)
                geometry.add_triangle(next - 3, next - 2, next - 1);
        }
        model.init_from(std::move(geometry));
        model.set_color({ color.r(), color.g(), color.b(), 0.85f });
    };
    build(m_layer_preview->supported[i], m_layer_cut_supported, LAYER_PREVIEW_SUPPORTED_COLOR);
    build(m_layer_preview->unsupported[i], m_layer_cut_unsupported, LAYER_PREVIEW_UNSUPPORTED_COLOR);
    m_parent.set_as_dirty();
}

void GLGizmoFdmSupports::render_layer_preview() const
{
    if (!m_highlight_by_layer || !m_layer_preview || (!m_layer_surface_supported.is_initialized() && !m_layer_surface_unsupported.is_initialized()))
        return;
    const ModelObject *mo = m_c->selection_info()->model_object();
    const Selection   &selection = m_parent.get_selection();
    if (mo == nullptr || selection.get_instance_idx() < 0 || size_t(selection.get_instance_idx()) >= mo->instances.size() ||
        m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView)
        return;

    const Camera &camera = wxGetApp().plater()->get_camera();
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glDepthMask(GL_FALSE));
    // Drawn over the painted faces, which sit at the same depth.
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-1.f, -1.f));

    if (GLShaderProgram *shader = wxGetApp().get_shader("flat_clip"); shader != nullptr) {
        shader->start_using();
        const ClippingPlaneDataWrapper clp = this->get_clipping_plane_data();
        shader->set_uniform("clipping_plane", clp.clp_dataf);
        shader->set_uniform("z_range", clp.z_range);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        // The cut faces are in world coordinates already.
        shader->set_uniform("view_model_matrix", camera.get_view_matrix());
        shader->set_uniform("volume_world_matrix", Transform3d::Identity());
        if (m_layer_surface_supported.is_initialized())
            const_cast<GLModel&>(m_layer_surface_supported).render();
        if (m_layer_surface_unsupported.is_initialized())
            const_cast<GLModel&>(m_layer_surface_unsupported).render();
        shader->stop_using();
    }

    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    // The stepped layer's areas on the cut, in world coordinates.
    if (m_layer_preview_layer != 0 && (m_layer_cut_supported.is_initialized() || m_layer_cut_unsupported.is_initialized())) {
        if (GLShaderProgram *shader = wxGetApp().get_shader("flat"); shader != nullptr) {
            shader->start_using();
            shader->set_uniform("view_model_matrix", camera.get_view_matrix());
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            if (m_layer_cut_supported.is_initialized())
                const_cast<GLModel&>(m_layer_cut_supported).render();
            if (m_layer_cut_unsupported.is_initialized())
                const_cast<GLModel&>(m_layer_cut_unsupported).render();
            shader->stop_using();
        }
    }
    glsafe(::glDepthMask(GL_TRUE));
}

void GLGizmoFdmSupports::paint_layer_preview_facets(bool block)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || !m_layer_preview)
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), block ? _u8L("Block highlighted supports") : _u8L("Enforce highlighted supports"),
                                  UndoRedo::SnapshotType::GizmoAction);
    int mesh_id = -1;
    for (size_t v = 0; v < mo->volumes.size(); ++ v) {
        const ModelVolume *mv = mo->volumes[v];
        if (!mv->is_model_part())
            continue;
        ++ mesh_id;
        if (size_t(mesh_id) >= m_triangle_selectors.size())
            break;
        if (v >= m_layer_preview->part_facets.size())
            continue;
        const std::vector<uint8_t> &flags = m_layer_preview->part_facets[v];
        if (flags.size() != mv->mesh().its.indices.size())
            continue;
        bool changed = false;
        for (size_t f = 0; f < flags.size(); ++ f)
            if (flags[f] != 0) {
                m_triangle_selectors[mesh_id]->set_facet(int(f), block ? EnforcerBlockerType::BLOCKER : EnforcerBlockerType::ENFORCER);
                changed = true;
            }
        if (changed)
            m_triangle_selectors[mesh_id]->request_update_render_data();
    }
    update_model_object();
    m_parent.set_as_dirty();
}

void GLGizmoFdmSupports::select_facets_by_angle(float threshold_deg, bool block)
{
    float threshold = (float(M_PI)/180.f)*threshold_deg;
    const Selection& selection = m_parent.get_selection();
    const ModelObject* mo = m_c->selection_info()->model_object();
    const ModelInstance* mi = mo->instances[selection.get_instance_idx()];

    int mesh_id = -1;
    for (const ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;

        ++mesh_id;

        const Transform3d trafo_matrix = mi->get_matrix_no_offset() * mv->get_matrix_no_offset();
        Vec3f down  = (trafo_matrix.inverse() * (-Vec3d::UnitZ())).cast<float>().normalized();
        Vec3f limit = (trafo_matrix.inverse() * Vec3d(std::sin(threshold), 0, -std::cos(threshold))).cast<float>().normalized();

        float dot_limit = limit.dot(down);

        // Now calculate dot product of vert_direction and facets' normals.
        int idx = 0;
        const indexed_triangle_set &its = mv->mesh().its;
        for (const stl_triangle_vertex_indices &face : its.indices) {
            if (its_face_normal(its, face).dot(down) > dot_limit) {
                m_triangle_selectors[mesh_id]->set_facet(idx, block ? EnforcerBlockerType::BLOCKER : EnforcerBlockerType::ENFORCER);
                m_triangle_selectors.back()->request_update_render_data();
            }
            ++ idx;
        }
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), block ? "Block supports by angle"
                                                    : "Add supports by angle");
    update_model_object();
    m_parent.set_as_dirty();
}

//BBS: remove const
void GLGizmoFdmSupports::update_model_object()
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info()->model_object();
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->supported_facets.set(*m_triangle_selectors[idx].get());
    }

    if (updated) {
        const ModelObjectPtrs& mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());

        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }

    //BBS: invalid volume_support status
    invalid_support_volumes(true);
}

//BBS
void GLGizmoFdmSupports::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    const ModelObject* mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();
    //BBS: add timestamp logic
    m_volume_timestamps.clear();

    int volume_id = -1;
    std::vector<ColorRGBA> ebt_colors;
    ebt_colors.push_back(GLVolume::NEUTRAL_COLOR);
    ebt_colors.push_back(TriangleSelectorGUI::enforcers_color);
    ebt_colors.push_back(TriangleSelectorGUI::blockers_color);
    ebt_colors.push_back(STABILIZER_POINT_COLOR);
    for (const ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;

        ++volume_id;

        // This mesh does not account for the possible Z up SLA offset.
        const TriangleMesh* mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors));
        // Reset of TriangleSelector is done inside TriangleSelectorGUI's constructor, so we don't need it to perform it again in deserialize().
        // The colour table has NONE/ENFORCER/BLOCKER and the Edge STABILIZER point state only: a higher
        // state in the file is reset to NONE instead of indexing past ebt_colors.
        m_triangle_selectors.back()->deserialize(mv->supported_facets.get_data(), false, EnforcerBlockerType::STABILIZER);
        m_triangle_selectors.back()->request_update_render_data();

        //BBS: add timestamp logic
        m_volume_timestamps.emplace_back(mv->supported_facets.timestamp());
    }

    //BBS: invalid volume_support status
    invalid_support_volumes(true);
}

PainterGizmoType GLGizmoFdmSupports::get_painter_type() const
{
    return PainterGizmoType::FDM_SUPPORTS;
}

wxString GLGizmoFdmSupports::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    // BBS remove _L()
    wxString action_name;
    if (shift_down)
        action_name = ("Unselect all");
    else {
        if (m_paint_stabilizers)
            action_name = button_down == Button::Left ? ("Paint stabilizer points") : ("Erase painting");
        else if (button_down == Button::Left)
            action_name = ("Enforce supports");
        else
            action_name = ("Block supports");
    }
    return action_name;
}

//BBS
void GLGizmoFdmSupports::init_print_instance()
{
    const PrintObject* print_object = NULL;
    PrintInstance print_instance = { 0 };
    const Print *print = m_parent.fff_print();

    if (!m_c->selection_info() || (m_print_instance.print_object))
    {
        //no selection or already got a print instance before
        return;
    }
    const ModelObject* model_object = m_c->selection_info()->model_object();
    int instance_index = m_c->selection_info()->get_active_instance();
    const ModelInstance* model_instance = model_object->instances[instance_index];

    //check the print
    if (!print)
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",print invalid\n";
        return;
    }

    for (const PrintObject* object : print->objects())
    {
        if (object->model_object()->id() == model_object->id())
        {
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ",found a PrintObject, id is" << model_object->id().id;
            print_object = object;
            break;
        }
    }

    //check the pring object
    if (!print_object)
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",can not find a PrintObject\n";
        return;
    }

    //find the print instance
    for (const PrintInstance &instance : print_object->instances())
    {
        if (instance.model_instance->id() == model_instance->id())
        {
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ",found a PrintInstance, id is" << model_instance->id().id;
            m_print_instance = instance;
            break;
        }
    }

    //check the pring object
    if (!m_print_instance.print_object)
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",can not find a PrintInstance\n";
        return;
    }

    const PrintObjectConfig& config = m_print_instance.print_object->config();
    m_angle_threshold_deg = config.support_angle;
    m_is_tree_support = config.enable_support.value && is_tree(config.support_type.value);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",get support_angle "<< m_angle_threshold_deg<<", is_tree "<<m_is_tree_support;

    return;
}

void GLGizmoFdmSupports::invalid_support_volumes(bool invalid_step)
{
    std::unique_lock<std::mutex> lck(m_mutex);
    m_volume_valid = false;

    if ((invalid_step) && (m_edit_state == state_generating) && m_print_instance.print_object)
    {
        Print *print = m_print_instance.print_object->print();
        if (print) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "cancel the print";
            print->cancel();
        }
    }
    m_edit_state = state_idle;
    lck.unlock();

    return;
}

bool GLGizmoFdmSupports::need_regenerate_support_volumes()
{
    if (!m_support_volume)
        return true;

    const ModelObject* mo = m_c->selection_info()->model_object();

    if (m_object_id != m_print_instance.print_object->id().id)
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",object_id changed from " << m_object_id << " to " << m_print_instance.print_object->id().id << ", need to regenerate";
        return true;
    }

    int volume_id = -1;
    for (const ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;

        ++volume_id;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",volume_id "<<volume_id<<", record_timestamp "<< m_volume_timestamps[volume_id]
                <<", current_timestamp "<<mv->supported_facets.timestamp();
        if (m_volume_timestamps[volume_id] != mv->supported_facets.timestamp())
        {
            return true;
        }
    }

    return false;
}

void GLGizmoFdmSupports::update_support_volumes()
{
    //PrintInstance m_print_instance = get_current_print_instance();

    if ((!m_print_instance.print_object))
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",invalid param, m_volume_ready="<< m_volume_ready;
        return;
    }

    if (m_volume_valid || !need_regenerate_support_volumes())
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",no need to regenerate support volume, return directly";

        std::unique_lock<std::mutex> lck(m_mutex);
        m_volume_ready = true;
        m_volume_valid = true;
        m_edit_state = state_ready;
        lck.unlock();
        return;
    }

    //generate_support_preview in async mode
    std::unique_lock<std::mutex> lck(m_mutex);
    m_volume_ready = false;
    //destroy previous support volume
    if (m_support_volume)
    {
        delete m_support_volume;
        m_support_volume = NULL;
    }
    lck.unlock();

    if (m_thread.joinable()) {
        //join the thread in ui thread
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "try to join thread for 100 ms";
        auto ret = m_thread.try_join_for(boost::chrono::milliseconds(100));
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "join thread returns "<<ret;
    }
    m_cancel = false;
    m_thread = create_thread([this]{this->run_thread();});
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",created thread to generate support volumes";
    return;
}

void GLGizmoFdmSupports::run_thread()
{
    try {
        Print *print = m_print_instance.print_object->print();

        print->restart();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",before generate_support_preview";
        m_print_instance.print_object->generate_support_preview();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",after generate_support_preview";

        if (m_cancel)
        {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", cancelled";
            goto _finished;
        }

        std::unique_lock<std::mutex> lck(m_mutex);
        m_support_volume = new GLVolume(0.5f, 0.5f, 0.5f, 0.5f);
        //m_support_volume->is_support_part = true;
        m_support_volume->force_native_color = true;
        m_support_volume->set_render_color();
        lck.unlock();

        auto record_timestamp = [this]()
        {
            const ModelObject* mo = m_c->selection_info()->model_object();

            int volume_id = -1;
            for (const ModelVolume* mv : mo->volumes) {
                if (!mv->is_model_part())
                    continue;

                ++volume_id;
                m_volume_timestamps[volume_id] = mv->supported_facets.timestamp();
            }
        };

        if (m_cancel)
        {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", cancelled";
            goto _finished;
        }

        if (!m_print_instance.print_object->support_layers().size())
        {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",no support layer found, update status to 100%\n";
            print->set_status(100, L("Support Generated"));
            goto _finished;
        }
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
        for (const SupportLayer *support_layer : m_print_instance.print_object->support_layers())
        {
            for (const ExtrusionEntity *extrusion_entity : support_layer->support_fills.entities)
            {
                _3DScene::extrusionentity_to_verts(extrusion_entity, float(support_layer->print_z), m_print_instance.shift, init_data);
            }
        }
        m_support_volume->model.init_from(std::move(init_data));
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", finished extrusionentity_to_verts, update status to 100%";
        print->set_status(100, L("Support Generated"));
        
        record_timestamp();
    }
    catch (...) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ",exception catched, mostly cancelling from gui!";
        //wxTheApp->OnUnhandledException();
    }

_finished:
    std::unique_lock<std::mutex> lck(m_mutex);
    if (m_edit_state == state_generating)
        m_edit_state = state_ready;

    lck.unlock();
    m_parent.set_as_dirty();
    m_parent.post_event(SimpleEvent(wxEVT_PAINT));
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", finished all";
    return;
}

void GLGizmoFdmSupports::generate_support_volume()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",before finalize_geometry";

    std::unique_lock<std::mutex> lck(m_mutex);
    m_volume_ready = true;
    m_volume_valid = true;
    m_object_id = m_print_instance.print_object->id().id;
    lck.unlock();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",finished finalize_geometry";
}

} // namespace Slic3r::GUI
