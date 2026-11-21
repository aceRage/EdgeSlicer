#ifndef slic3r_Preferences_hpp_
#define slic3r_Preferences_hpp_

#include "GUI.hpp"
#include "GUI_Utils.hpp"

#include <wx/simplebook.h>
#include <wx/dialog.h>
#include <wx/timer.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <vector>
#include <functional>
#include <list>
#include <map>
#include <deque>
#include <string>
#include <utility>
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r { namespace GUI {


#define DESIGN_SELECTOR_NOMORE_COLOR wxColour(248, 248, 248)
#define DESIGN_GRAY900_COLOR wxColour(38, 46, 48)
#define DESIGN_GRAY800_COLOR wxColour(50, 58, 61)
#define DESIGN_GRAY600_COLOR wxColour(144, 144, 144)
#define DESIGN_GRAY400_COLOR wxColour(166, 169, 170)

class Selector
{
public:
    int       m_index;
    wxWindow *m_tab_button;
    wxWindow *m_tab_text;
};
WX_DECLARE_HASH_MAP(int, Selector *, wxIntegerHash, wxIntegerEqual, SelectorHash);

class RadioBox;
class RadioSelector
{
public:
    wxString  m_param_name;
    int       m_groupid;
    RadioBox *m_radiobox;
    bool      m_selected = false;
};

WX_DECLARE_LIST(RadioSelector, RadioSelectorList);
class CheckBox;
class TextInput;



#define DESIGN_RESOUTION_PREFERENCES wxSize(FromDIP(540), -1)
#define DESIGN_TITLE_SIZE wxSize(FromDIP(100), -1)
#define DESIGN_COMBOBOX_SIZE wxSize(FromDIP(140), -1)
#define DESIGN_LARGE_COMBOBOX_SIZE wxSize(FromDIP(160), -1)
#define DESIGN_INPUT_SIZE wxSize(FromDIP(100), -1)


class PreferencesDialog : public DPIDialog
{
private:
    AppConfig *app_config;

protected:
    wxBoxSizer *  m_sizer_body;
    wxScrolledWindow* m_scrolledWindow;

    // bool								m_settings_layout_changed {false};
    bool m_seq_top_layer_only_changed{false};
    bool m_recreate_GUI{false};

public:
    bool seq_top_layer_only_changed() const { return m_seq_top_layer_only_changed; }
    bool recreate_GUI() const { return m_recreate_GUI; }
    void on_dpi_changed(const wxRect &suggested_rect) override;

public:
    PreferencesDialog(wxWindow *      parent,
                      wxWindowID      id    = wxID_ANY,
                      const wxString &title = wxT(""),
                      const wxPoint & pos   = wxDefaultPosition,
                      const wxSize &  size  = wxDefaultSize,
                      long            style = wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX);

    ~PreferencesDialog();

    wxString m_backup_interval_time;

    void      create();
    wxWindow *create_tab_button(int id, wxString text);

    // debug mode
    ::CheckBox * m_developer_mode_ckeckbox   = {nullptr};
    ::CheckBox * m_internal_developer_mode_ckeckbox = {nullptr};

    ::CheckBox * m_dark_mode_ckeckbox        = {nullptr};
    ::TextInput *m_backup_interval_textinput = {nullptr};
    ::TextInput *m_autosave_interval_textinput = {nullptr};
    // Ultra (G-code archive): the folder row and the retention field follow the checkbox.
    wxWindow *   m_gcode_archive_dir_item      = {nullptr};
    ::TextInput *m_gcode_archive_max_textinput = {nullptr};
    ::CheckBox * m_legacy_networking_ckeckbox     = {nullptr};

    wxString m_developer_mode_def;
    wxString m_internal_developer_mode_def;
    wxString m_websocket_debug_def;
    wxString m_backup_interval_def;
    wxString m_iot_environment_def;

    SelectorHash      m_hash_selector;
    RadioSelectorList m_radio_group;
    // ComboBoxSelectorList    m_comxbo_group;

    wxBoxSizer *create_item_title(wxString title, wxWindow *parent, wxString tooltip);
    wxBoxSizer *create_item_combobox(wxString title, wxWindow *parent, wxString tooltip, std::string param, std::vector<wxString> vlist);
    wxBoxSizer *create_item_combobox(wxString title, wxWindow *parent, wxString tooltip, std::string param, std::vector<wxString> vlist, std::vector<std::string> config_name_index);
    wxBoxSizer *create_item_region_combobox(wxString title, wxWindow *parent, wxString tooltip, std::vector<wxString> vlist);
    wxBoxSizer *create_item_language_combobox(wxString title, wxWindow *parent, wxString tooltip, int padding_left, std::string param, std::vector<const wxLanguageInfo *> vlist);
    wxBoxSizer *create_item_loglevel_combobox(wxString title, wxWindow *parent, wxString tooltip, std::vector<wxString> vlist);
    wxBoxSizer *create_item_checkbox(wxString title, wxWindow *parent, wxString tooltip, int padding_left, std::string param, std::function<bool(bool new_val, bool old_val)> confirm_cb = nullptr);
    wxBoxSizer *create_item_darkmode_checkbox(wxString title, wxWindow *parent, wxString tooltip, int padding_left, std::string param);
    void set_dark_mode();
    wxBoxSizer *create_item_button(wxString title, wxString title2, wxWindow *parent, wxString tooltip, wxString tooltip2, std::function<void()> onclick, bool button_on_left = false);
    wxWindow* create_item_downloads(wxWindow* parent, int padding_left, std::string param);
    wxBoxSizer *create_item_input(wxString title, wxString title2, wxWindow *parent, wxString tooltip, std::string param, std::function<void(wxString)> onchange = {});
    wxBoxSizer *create_item_text_input(wxString title, wxWindow *parent, wxString tooltip, std::string param);
    wxBoxSizer *create_camera_orbit_mult_input(wxString title, wxWindow *parent, wxString tooltip);
    wxBoxSizer *create_item_backup_input(wxString title, wxWindow *parent, wxString tooltip, std::string param);
    wxBoxSizer *create_item_autosave_input(wxString title, wxWindow *parent, wxString tooltip, std::string param);
    // Orca #11407: "Auto slice after changes" with its delay in seconds, on one row.
    wxBoxSizer *create_item_auto_reslice(wxString title, wxWindow *parent, wxString checkbox_tooltip, wxString delay_tooltip);
    // Ultra (G-code archive): a folder row with a Browse button, and the retention count.
    wxWindow   *create_item_gcode_archive_dir(wxWindow *parent, wxString tooltip);
    // Home > Vaults: where vault downloads go (a folder row with Browse and Default).
    wxWindow   *create_item_vault_downloads_dir(wxWindow *parent, wxString tooltip);
    // ZIP files (PR C): extraction folder, its size, Open folder and Clean now.
    wxWindow   *create_item_zip_work_folder(wxWindow *parent, wxString tooltip);
    wxBoxSizer *create_item_gcode_archive_max(wxWindow *parent, wxString tooltip);
    wxBoxSizer *create_item_gizmo_panel_opacity(wxWindow *parent, wxString tooltip);
    // Selected-object highlight: style combo (Glow / Thin outline) plus the Glow strength slider.
    wxBoxSizer *create_item_selection_highlight(wxWindow *parent);
    wxBoxSizer *create_item_multiple_combobox(
        wxString title, wxWindow *parent, wxString tooltip, int padding_left, std::string parama, std::vector<wxString> vlista, std::vector<wxString> vlistb);
    wxBoxSizer *create_item_switch(wxString title, wxWindow *parent, wxString tooltip, std::string param);
    wxWindow *  create_item_radiobox(wxString title, wxWindow *parent, wxString tooltip, int padding_left, int groupid, std::string param);
#ifdef WIN32
    wxBoxSizer* create_item_link_association(wxWindow* parent, wxString url_prefix, wxString website_name);
#endif // WIN32

    // Layout: tabs -> collapsible sections -> table rows. A page is built from sections; each
    // setting is one row: add_row(sec, create_item_...(..., row(sec), ...)).
    struct PrefSection
    {
        std::string   id;                // key of the remembered open / collapsed state
        size_t        index{0};
        wxWindow *    page{nullptr};
        wxWindow *    header{nullptr};
        wxWindow *    body{nullptr};
        wxStaticText *chevron{nullptr};
        wxBoxSizer *  sizer{nullptr};
        wxWindow *    pending{nullptr};  // the row panel handed out by row(), until add_row() takes it
        int           rows{0};
        bool          open{true};
    };
    struct PrefRow
    {
        wxWindow *panel{nullptr};
        wxString  text;                  // labels and tooltips, lower case, for the search box
        size_t    section{0};
    };
    wxWindow *   new_page();
    PrefSection *begin_section(wxWindow *page, const std::string &id, const wxString &title, bool open_by_default = true);
    // The parent for the next row's controls.
    wxWindow *   row(PrefSection *sec);
    // indent: the row depends on the one above it.
    void         add_row(PrefSection *sec, wxSizer *item, bool indent = false);
    void         add_row(PrefSection *sec, wxWindow *item, bool indent = false);
    void         finish_row(PrefSection *sec, wxSizer *content, bool indent);
    void         tabulate_row(wxSizer *sizer);
    void         set_section_open(PrefSection &sec, bool open, bool remember);
    void         set_page_sections_open(bool open);
    void         load_section_states();
    void         save_section_states();
    void         select_page(size_t index);
    void         apply_search(const wxString &query);
    void         relayout();

    std::deque<PrefSection>                      m_sections; // deque: begin_section() hands out stable pointers
    std::vector<PrefRow>                         m_rows;
    std::vector<std::pair<wxString, wxWindow *>> m_pages;
    std::vector<Button *>                        m_tab_buttons;
    std::map<std::string, bool>                  m_section_open;
    size_t                                       m_current_page{0};
    ::TextInput *                                m_search_input{nullptr};
    wxStaticText *                               m_no_results{nullptr};

    wxWindow* create_page_general();
    wxWindow* create_page_view();
    wxWindow* create_page_projects();
    wxWindow* create_page_slicing();
    wxWindow* create_page_devices();
    wxWindow* create_page_advanced();
    void create_gui_page();
    void create_sync_page();
    void create_shortcuts_page();
    wxWindow* create_debug_page();

    void     on_select_radio(std::string param);
    wxString get_select_radio(int groupid);
    // BBS
    void create_select_domain_widget();

    void Split(const std::string &src, const std::string &separator, std::vector<wxString> &dest);
    int m_current_language_selected = {0};

protected:
    void OnSelectTabel(wxCommandEvent &event);
    void OnSelectRadio(wxMouseEvent &event);

private:
    std::tuple<wxBoxSizer*, ComboBox*> create_item_combobox_base(wxString title, wxWindow* parent, wxString tooltip, std::string param, std::vector<wxString> vlist, unsigned int current_index);
};

wxDECLARE_EVENT(EVT_PREFERENCES_SELECT_TAB, wxCommandEvent);

}} // namespace Slic3r::GUI

#endif /* slic3r_Preferences_hpp_ */
