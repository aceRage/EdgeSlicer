#ifndef slic3r_GLGizmoMmuSegmentation_hpp_
#define slic3r_GLGizmoMmuSegmentation_hpp_

#include "GLGizmoPainterBase.hpp"
#include "libslic3r/MixedFilament.hpp"

#include <set>

namespace Slic3r::GUI {

class GLMmSegmentationGizmo3DScene
{
public:
    GLMmSegmentationGizmo3DScene() = delete;

    explicit GLMmSegmentationGizmo3DScene(size_t triangle_indices_buffers_count)
    {
    }

    virtual ~GLMmSegmentationGizmo3DScene() { release_geometry(); }

    [[nodiscard]] inline bool has_VBOs(size_t triangle_indices_idx) const
    {
        assert(triangle_indices_idx < this->triangle_patches.size());
        return this->triangle_indices_VBO_ids[triangle_indices_idx] != 0;
    }

    // Release the geometry data, release OpenGL VBOs.
    void release_geometry();
    // Finalize the initialization of the geometry, upload the geometry to OpenGL VBO objects
    // and possibly releasing it if it has been loaded into the VBOs.
    void finalize_vertices();
    // Finalize the initialization of the indices, upload the indices to OpenGL VBO objects
    // and possibly releasing it if it has been loaded into the VBOs.
    void finalize_triangle_indices();

    void clear()
    {
        this->vertices.clear();
        // BBS
        this->triangle_indices_VBO_ids.clear();
        this->triangle_indices_sizes.clear();

        for (TrianglePatch& patch : this->triangle_patches)
            patch.triangle_indices.clear();
        this->triangle_patches.clear();
    }

    void render(size_t triangle_indices_idx) const;

    std::vector<float>            vertices;
    //std::vector<std::vector<int>> triangle_indices;

    // BBS
    std::vector<TrianglePatch>    triangle_patches;

    // When the triangle indices are loaded into the graphics card as Vertex Buffer Objects,
    // the above mentioned std::vectors are cleared and the following variables keep their original length.
    std::vector<size_t> triangle_indices_sizes;

    // IDs of the Vertex Array Objects, into which the geometry has been loaded.
    // Zero if the VBOs are not sent to GPU yet.
    unsigned int              vertices_VAO_id{ 0 };
    unsigned int              vertices_VBO_id{0};
    std::vector<unsigned int> triangle_indices_VBO_ids;
};

class GLGizmoMmuSegmentation : public GLGizmoPainterBase
{
public:
    GLGizmoMmuSegmentation(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    ~GLGizmoMmuSegmentation() override;

    void render_painter_gizmo() override;

    void data_changed(bool is_serializing) override;

    // Edge (Airbrush P2): the tool's state, defined in GLGizmoMmuSegmentationAirbrush.cpp.
    struct AirbrushState;

    // Re-sync the in-memory painting session from the model after an external
    // rewrite of mmu_segmentation_facets (e.g. batch color-match apply).
    void refresh_from_model();

    // Keep this in sync with the shared triangle-selector state range.
    static const constexpr size_t EXTRUDERS_LIMIT = static_cast<size_t>(EnforcerBlockerType::ExtruderMax);

    const float get_cursor_radius_min() const override { return CursorRadiusMin; }

    // BBS
    bool on_number_key_down(int number);
    bool on_key_down_select_tool_type(int keyCode);

protected:
    // BBS
    ColorRGBA get_cursor_hover_color() const override;
    void on_set_state() override;

    EnforcerBlockerType get_left_button_state_type() const override
    {
        if (m_selected_extruder_idx < m_display_filament_ids.size())
            return EnforcerBlockerType(m_display_filament_ids[m_selected_extruder_idx]);
        return EnforcerBlockerType::Extruder1;
    }
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType(-1); }

    void on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;
    void show_tooltip_information(float caption_max, float x, float y);
    bool on_is_selectable() const override;
    bool on_is_activable() const override;

    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    // Edge (Airbrush P2, GLGizmoMmuSegmentationAirbrush.cpp)
    bool  airbrush_owns_mouse() const override;
    bool  on_airbrush_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool control_down) override;
    float airbrush_cursor_radius() const override;

    std::string get_gizmo_entering_text() const override { return "Entering color painting"; }
    std::string get_gizmo_leaving_text() const override { return "Leaving color painting"; }
    std::string get_action_snapshot_name() const override { return "Color painting editing"; }

    // BBS
    size_t                            m_selected_extruder_idx = 0;
    std::vector<ColorRGBA>            m_extruders_colors;
    std::vector<unsigned int>         m_display_filament_ids;
    std::vector<int>                  m_volumes_extruder_idxs;

    // BBS
    wchar_t                           m_current_tool = 0;
    bool                              m_detect_geometry_edge = true;
    
    // Filament remap feature
    bool                              m_show_remap_panel = false;
    std::vector<size_t>               m_extruder_remap;      // index → target extruder index
    // ORCA: Cache used filaments to filter UI. Edge: display slots (indices into m_display_filament_ids).
    std::set<size_t>                  m_used_filaments;

    // Minimal context for gradient rendering; only physical_colors is used
    MixedFilamentDisplayContext       m_mixed_display_context;

    static const constexpr float      CursorRadiusMin = 0.1f; // cannot be zero

private:
    bool on_init() override;

    // BBS. remove const.
    void update_model_object() override;
    //BBS: add logic to distinguish the first_time_update and later_update
    void update_from_model_object(bool first_update = false) override;
    void tool_changed(wchar_t old_tool, wchar_t new_tool);

    void on_opening() override;
    void on_shutdown() override;
    PainterGizmoType get_painter_type() const override;

    void init_model_triangle_selectors();
    // The object list, plate, background process and filament usage after the paint changed.
    void notify_paint_changed(ModelObject *mo);

    // Edge (Airbrush P2, GLGizmoMmuSegmentationAirbrush.cpp)
    std::unique_ptr<AirbrushState> m_airbrush;
    AirbrushState &airbrush();
    void airbrush_render_panel(float sliders_left_width, float sliders_width, float drag_left_width, float max_tooltip_width, float window_width);
    void airbrush_render_overlay();
    // The model was reloaded under the gizmo (undo/redo, another object): drop strokes waiting for a bake.
    void airbrush_reset();
    // The gizmo closes: finish the stroke, wait for the bakes and commit them.
    void airbrush_finish_pending();
    // Erase all painting also drops the fields of the object's parts.
    void airbrush_clear_fields(ModelObject *mo);
    void airbrush_tick();
    bool airbrush_press(const Vec2d &mouse_position, bool erase);
    void airbrush_drag(const Vec2d &mouse_position);
    void airbrush_release();
    void airbrush_schedule(int interval_ms);
    void airbrush_start_bake();
    void airbrush_poll_bake(bool wait);
    void airbrush_start_cost();
    void airbrush_poll_cost(bool wait);
    void airbrush_apply_params_to_object();
    void airbrush_sync_from_model();

    // BBS
    void update_triangle_selectors_colors();
    void init_extruders_data();
    void init_extruders_data(const std::vector<ColorRGBA> &extruder_colors);
    
    // Filament remapping methods
    void remap_filament_assignments();
    void render_filament_remap_ui(float window_width, float max_tooltip_width);
    // ORCA: Helper to update the cache of used filaments
    void update_used_filaments();
    void reset_filament_remap();

    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;
};

} // namespace Slic3r


#endif // slic3r_GLGizmoMmuSegmentation_hpp_
