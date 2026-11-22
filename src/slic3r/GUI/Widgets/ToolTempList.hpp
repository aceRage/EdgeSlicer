#ifndef slic3r_GUI_ToolTempList_hpp_
#define slic3r_GUI_ToolTempList_hpp_

// The Device tab's tool temperature list for printers with more than two tools and for the H3C
// nozzle rack (phase 3 of tests/design_device_multi_tool_temps.md; approved mockup
// tests/mockups/device_tool_temps.html). The layout decision is ToolTempLayout.hpp's
// tool_temp_layout(); this file only builds and updates the controls.
//
// One and two tools keep StatusPanel's own two TempInput rows untouched (same sizes, same set
// path, same events), so this list stays empty and hidden for them. For 3-8 tools it fills the
// temperature column (compact rows, then a two-column grid); for 9+ tools and for a rack it fills
// a strip under the temperature box.
//
// Every window is a child of the temperature box (the StaticBox StatusPanel builds), as the two
// fixed rows are, so backgrounds, tab order and the TempInput finish event (posted to the parent)
// behave the same. The list is not a window itself: it owns two sizers.

#include "TempInput.hpp"
#include "StaticLine.hpp"
#include "../ToolTempLayout.hpp"

#include <wx/sizer.h>
#include <wx/window.h>

#include <functional>
#include <string>
#include <vector>

class ToolTempCell;
class ToolGroupHeader;

class ToolTempList
{
public:
    // Phase 5 off switch (owner decision D4): rack nozzles stay read-only until an H3C hand test
    // has proven which command heats which rack nozzle. Turning this on gives waiting rack
    // nozzles a target field; the send path for rack ids still refuses (see on_finish).
    static constexpr bool kRackTargetsSettable = false;

    // `send`: StatusPanel's nozzle send path (ctrl, extruder id, hold counter).
    using SendFn = std::function<void(TempInput *ctrl, int extruder_id, int &hold_count)>;

    // `tab_anchor`: the window the column rows follow in tab order (the fixed nozzle row).
    ToolTempList(wxWindow *box, wxWindow *tab_anchor, const StateColor &text_colour, const StateColor &border_colour, int min_temp, int max_temp);
    ~ToolTempList();

    // Goes into the temperature column, right under the two fixed nozzle rows.
    wxSizer *column_sizer() const { return m_column; }
    // Goes into the temperature box under its content row.
    wxSizer *strip_sizer() const { return m_strip; }

    // Called after a change of the list's size; StatusPanel lays itself and the box out.
    void set_layout_callback(std::function<void()> cb) { m_on_layout = std::move(cb); }

    // `tools`: the extruder entries (used only when the plan does not use the fixed rows);
    // `rack`: rack slots (empty = no rack). Rebuilds the controls when the shape changes, then
    // refreshes values, keeping a target the user is typing and the per-row hold counter.
    void update(const Slic3r::GUI::ToolTempLayoutPlan &plan,
                const std::vector<Slic3r::GUI::ToolTempEntry> &tools,
                const std::vector<Slic3r::GUI::ToolTempEntry> &rack,
                int nozzle_max);

    // Hide everything (printer gone, or one / two tools).
    void clear();

    // The TempInput finish event (wxCUSTOMEVT_SET_TEMP_FINISH, by TempInput::GetType()). Returns
    // false when the id is not one of this list's rows.
    bool on_finish(int temp_type, const SendFn &send);

    bool carries_tools() const { return m_tools_in_list; }

    void enable(bool on);
    void reset_values(); // the "_" blanks reset_temp_misc_control shows while disconnected
    void rescale();
    void sys_color_changed();

private:
    struct Row
    {
        TempInput  *ctrl{nullptr};
        std::string id;
        int         extruder_id{-1};
        bool        rack{false};
        int         hold{0};
        bool        editing{false};
        bool        cancelled{false}; // Esc restored the printer's target; do not send it back
        int         shown_target{0};
        TempInput::RowMode mode{TempInput::RowMode::Compact};
        wxSize      min_dip;          // minimum size in DIP, for rescale
    };
    struct Group
    {
        Slic3r::GUI::ToolStripGroup::What what;
        ToolGroupHeader                  *header{nullptr};
        wxGridSizer                      *cells{nullptr};
        // One slot per entry, in order: a read-only cell, or the index of a settable row in m_rows.
        std::vector<ToolTempCell *>       ro_cells;
        std::vector<int>                  row_index;
        bool                              open{true};
    };

    TempInput *make_row(const Slic3r::GUI::ToolTempEntry &e, TempInput::RowMode mode, const wxSize &min_size, bool rack);
    void       rebuild(const Slic3r::GUI::ToolTempLayoutPlan &plan,
                       const std::vector<Slic3r::GUI::ToolTempEntry> &tools,
                       const std::vector<Slic3r::GUI::ToolTempEntry> &rack);
    void       destroy_all();
    void       update_row(Row &row, const Slic3r::GUI::ToolTempEntry &e, int nozzle_max);
    void       update_header(Group &g, const std::vector<Slic3r::GUI::ToolTempEntry> &entries);
    void       toggle(Group &g);
    void       relayout();
    Row       *find_row(int temp_type);
    std::string shape_key(const Slic3r::GUI::ToolTempLayoutPlan &plan,
                          const std::vector<Slic3r::GUI::ToolTempEntry> &tools,
                          const std::vector<Slic3r::GUI::ToolTempEntry> &rack) const;

    static std::string config_key(Slic3r::GUI::ToolStripGroup::What what);

    wxWindow   *m_box;
    wxWindow   *m_tab_anchor;
    StateColor  m_text_colour;
    StateColor  m_border_colour;
    int         m_min_temp;
    int         m_max_temp;
    wxBoxSizer *m_column;     // compact rows or the grid
    wxBoxSizer *m_strip;      // line + groups
    StaticLine *m_strip_line{nullptr};
    wxFlexGridSizer *m_grid{nullptr};
    std::vector<Row>   m_rows;
    std::vector<Group> m_groups;
    std::string        m_shape;
    bool               m_tools_in_list{false};
    bool               m_enabled{true};
    std::function<void()> m_on_layout;

    // The last update's input, so rescale / toggle can rebuild or recount without new data.
    Slic3r::GUI::ToolTempLayoutPlan          m_last_plan;
    std::vector<Slic3r::GUI::ToolTempEntry>  m_last_tools;
    std::vector<Slic3r::GUI::ToolTempEntry>  m_last_rack;
    int                                      m_last_nozzle_max{0};
    bool                                     m_has_last{false};
};

#endif // slic3r_GUI_ToolTempList_hpp_
