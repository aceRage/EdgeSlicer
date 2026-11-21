#ifndef slic3r_ProjectDirtyStateManager_hpp_
#define slic3r_ProjectDirtyStateManager_hpp_

#include "libslic3r/Preset.hpp"

namespace Slic3r {
namespace GUI {

class ProjectDirtyStateManager
{
public:    
    void update_from_undo_redo_stack(bool dirty);
    void update_from_presets();
    void reset_after_save();
    void reset_initial_presets();

#ifdef EDGE_PYTHON_PLUGINS
    void set_plater_dirty(bool is_dirty)
    {
        const bool was_dirty = this->is_dirty();
        m_plater_dirty       = is_dirty;
        notify_dirty_change(was_dirty, "plater");
    }
#else
    void set_plater_dirty(bool is_dirty) { m_plater_dirty = is_dirty; }
#endif
    bool is_dirty() const { return m_plater_dirty || m_project_config_dirty || m_presets_dirty; }
    bool is_presets_dirty() const { return m_presets_dirty; }

#ifdef EDGE_PYTHON_PLUGINS
    // Python plugins (OrcaSlicer's ProjectDirtyChanged): RAII guard coalescing dirty-state
    // updates. While any guard is alive, ProjectDirtyChanged is held back; when the outermost
    // guard ends, at most one event fires, for the net change across the guarded scope.
    class NotificationSuppressor
    {
    public:
        explicit NotificationSuppressor(ProjectDirtyStateManager &owner) : m_owner(owner) { m_owner.begin_suppress_notifications(); }
        ~NotificationSuppressor() { m_owner.end_suppress_notifications(); }
        NotificationSuppressor(const NotificationSuppressor &)            = delete;
        NotificationSuppressor &operator=(const NotificationSuppressor &) = delete;

    private:
        ProjectDirtyStateManager &m_owner;
    };
#endif // EDGE_PYTHON_PLUGINS

#if ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW
    void render_debug_window() const;
#endif // ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW

private:
#ifdef EDGE_PYTHON_PLUGINS
    // Fires ProjectDirtyChanged when the aggregate dirty state changed (and no guard is alive).
    void notify_dirty_change(bool was_dirty, const char *source);
    void begin_suppress_notifications();
    void end_suppress_notifications();
    // NotificationSuppressor nesting depth and the dirty state seen when the outermost guard began.
    int  m_suppress_depth { 0 };
    bool m_suppress_entry_dirty { false };
#endif // EDGE_PYTHON_PLUGINS

    // Does the Undo / Redo stack indicate the project is dirty?
    bool                                        m_plater_dirty { false };
    // Do the presets indicate the project is dirty?
    bool                                        m_presets_dirty { false };
    // Is the project config dirty?
    bool                                        m_project_config_dirty { false };
    // Keeps track of preset names selected at the time of last project save.
    std::array<std::string, Preset::TYPE_COUNT> m_initial_presets;
    DynamicPrintConfig                          m_initial_project_config;

    // filament preset independent of the m_initial_presets
    std::vector<std::string>                    m_initial_filament_presets_names;   // all filament preset type name
    std::vector<std::string>                    m_initial_filament_presets_colors;    // all filament preset color
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_ProjectDirtyStateManager_hpp_
