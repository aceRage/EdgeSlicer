#include "SupportSetEditDialog.hpp"

#include <algorithm>

#include <wx/sizer.h>
#include <wx/stattext.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "OptionsGroup.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/StaticLine.hpp"
#include "format.hpp"

namespace Slic3r {
namespace GUI {

// The rows, in the order the process tab's Support page shows them, grouped under the same
// headings. Mostly the curated part-level key set of PrintConfig.cpp's part_support_keys(), minus
// support_interface_filament, which a set never stores as a slot index: it travels as the
// portable interface_filament_type and gets its own row below.
//
// support_type is the one row here that is NOT a part-level key. The plan's 3.5 puts it in tier C -
// never per-part - and it stays out of part_support_keys(); a support GROUP therefore does not
// resolve it and this branch does not change that. But it IS a Support-category PrintObjectConfig
// key, so support_set_keys() carries it and support_set_apply_to() already writes it into the
// project. Editing a set without it was the confusing part: "Grid" and "Tree Slim" sit in the same
// Style list, and which half of that list applies is decided by a value the window did not show.
// So it is a SET-level value here: shown, saved, applied - and object-wide, like the type is.
//
// enable_support is deliberately still absent. A set says how support is built, not whether the
// object gets any; that stays a per-project decision on the Support page. (support_set_keys()
// carries it all the same - see the note in collect().)
static const std::vector<std::string>& keys_support()
{
    static const std::vector<std::string> s_keys = { "support_type", "support_style", "support_threshold_angle" };
    return s_keys;
}

static const std::vector<std::string>& keys_ironing()
{
    static const std::vector<std::string> s_keys = {
        "support_ironing", "support_ironing_pattern", "support_ironing_flow", "support_ironing_spacing"
    };
    return s_keys;
}

static const std::vector<std::string>& keys_advanced()
{
    static const std::vector<std::string> s_keys = {
        "support_top_z_distance",
        "support_interface_top_layers", "support_interface_bottom_layers",
        "support_interface_pattern", "support_interface_spacing", "support_bottom_interface_spacing",
        // support_interface_loop_pattern is deliberately NOT here: the process tab hides it
        // (Tab.cpp, its line is commented out) and it is untested on this fork, so the editor
        // does not expose it either. A set still carries the key silently, like every other
        // hidden support key (decision 2026-09-05).
    };
    return s_keys;
}

// The groups draw themselves the way the process tab's groups do - the OG_CustomCtrl path,
// which ConfigOptionsGroup only takes when is_tab_opt is true: a Head_14 caption over a rule,
// Body_14 labels with no trailing colon, painted straight onto the window's own background
// rather than a static box's lighter one.
//
// That flag has one side effect this window must not have. ConfigOptionsGroup::get_option()
// then registers every key with the settings searcher under THIS group's title, overwriting
// the entries the real Support page put there and changing what the search box says about
// them. So the lines are appended here instead: the same Option, and the same opt-map entry
// the group needs in order to write an edited value back into the config, without the
// searcher call.
class SetEditorOptionsGroup : public ConfigOptionsGroup
{
public:
    SetEditorOptionsGroup(wxWindow *parent, const wxString &title, DynamicPrintConfig *config)
        : ConfigOptionsGroup(parent, title, wxEmptyString, config, true /* is_tab_opt */)
    {}

    void append_set_option_line(const std::string &opt_key)
    {
        m_opt_map.emplace(opt_key, std::make_pair(opt_key, -1));
        append_single_option_line(Option(*print_config_def.get(opt_key), opt_key));
    }
};

SupportSetEditDialog::SupportSetEditDialog(wxWindow                 *parent,
                                           const SupportSet         &set,
                                           const DynamicPrintConfig &filament_config)
    : wxDialog(parent, wxID_ANY, format_wxstr(_L("Edit Support Set - %1%"), from_u8(set.name)),
               wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_set(set)
    , m_filament_config(filament_config)
{
    // The window's own background colour, set before a single child exists. The fork's dialogs are
    // white in light mode and let UpdateDlgDarkUI() at the end of this constructor map that to the
    // dark background; a dialog that skips this keeps the system's lighter grey and its captions
    // then look nothing like the process tab's. It has to be FIRST because the two widgets that
    // draw the group headings copy their parent's background in their constructors and never look
    // again - OG_CustomCtrl (OG_CustomCtrl.cpp, "SetBackgroundColour(parent->GetBackgroundColour())")
    // and ::StaticLine (StaticLine.cpp, the same line).
    SetBackgroundColour(*wxWHITE);

    // The editor's own config: the set's values where it has them, the option's own default
    // otherwise, so every row has something to show. Nothing here is connected to the project -
    // the whole point of the window is that changing a set does not change what is being sliced.
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
    for (const std::vector<std::string>* list : { &keys_support(), &keys_ironing(), &keys_advanced() })
        for (const std::string& key : *list) {
            const ConfigOptionDef* def = print_config_def.get(key);
            if (def == nullptr || def->default_value.get() == nullptr || !support_set_is_allowed_key(key))
                continue;
            m_config.set_key_value(key, def->default_value->clone());
            if (auto it = m_set.values.find(key); it != m_set.values.end())
                m_config.set_deserialize(key, it->second, substitutions);
        }

    const int em = wxGetApp().em_unit();
    wxBoxSizer* main = new wxBoxSizer(wxVERTICAL);

    main->AddSpacer(em / 2);

    add_group(main, _L("Support"),  keys_support());
    add_interface_filament_row(main);
    add_group(main, _L("Ironing"),  keys_ironing());
    add_group(main, _L("Advanced"), keys_advanced());

    wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    Button* save = new Button(this, _L("Save"));
    save->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    save->SetToolTip(_L("Write these values back to this support set"));
    save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        collect();
        EndModal(wxID_OK);
    });
    Button* cancel = new Button(this, _L("Cancel"));
    cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    buttons->Add(save, 0, wxRIGHT, em / 2);
    buttons->Add(cancel, 0);
    main->Add(buttons, 0, wxEXPAND | wxALL, em / 2);

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { EndModal(wxID_CANCEL); });

    toggle_fields();
    filter_support_styles();
    SetSizerAndFit(main);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void SupportSetEditDialog::add_group(wxSizer *parent_sizer, const wxString &title, const std::vector<std::string> &keys)
{
    // See SetEditorOptionsGroup above: the process tab's own group drawing, minus the searcher.
    auto og = std::make_shared<SetEditorOptionsGroup>(this, title, &m_config);
    og->m_on_change = [this](t_config_option_key opt_key, boost::any) {
        if (opt_key == "support_ironing")
            toggle_fields();
        else if (opt_key == "support_type")
            filter_support_styles();
    };
    for (const std::string& key : keys)
        if (m_config.has(key))
            og->append_set_option_line(key);
    og->activate();
    // Both of these are what Tab::Page::activate() does after activate(), and the custom
    // control needs them: update_visibility() is the only thing that gives it a minimum size
    // (without it the group collapses to nothing), and reload_config() is what puts the set's
    // values into the fields - a freshly built field shows the option's default until then.
    // comDevelop, not the app's mode: a set carries every one of these keys whatever mode the
    // window behind this one is in (support_type is comSimple, support_style comAdvanced), so
    // every one of them stays on the window.
    og->update_visibility(comDevelop);
    og->reload_config();
    m_groups.push_back(og);
    parent_sizer->Add(og->sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, wxGetApp().em_unit() / 2);
}

void SupportSetEditDialog::add_interface_filament_row(wxSizer *parent_sizer)
{
    const int em = wxGetApp().em_unit();

    // The caption the option groups get, built by hand because this row is not an option group:
    // same widget, same font, same colour as OptionsGroup::activate() gives the others.
    ::StaticLine* caption = new ::StaticLine(this, false, _L("Filament"));
    caption->SetFont(Label::Head_14);
    caption->SetForegroundColour("#363636"); // StaticLine maps it for dark mode when it paints
    parent_sizer->Add(caption, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em / 2);
    parent_sizer->AddSpacer(8);

    wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);

    // OG_CustomCtrl starts a row's label 2 em + 4 px in and its control one label width (20 em)
    // plus a 0.2 em gap from the left edge. Repeating those two numbers is what lines this row
    // up with the option rows above and below it.
    const int label_x = 2 * em + 4;
    const int field_x = 22 * em + em / 5;

    wxStaticText* label = new wxStaticText(this, wxID_ANY, _L("Support/raft interface"));
    label->SetFont(Label::Body_14);
    // A wxStaticText starts on the system button face (#F0F0F0), which UpdateDlgDarkUI does not
    // map, so it kept a lighter box behind it in dark mode. Give it the dialog's own white; that
    // one IS mapped, to the same dark grey as the window.
    label->SetBackgroundColour(*wxWHITE);
    label->SetMinSize(wxSize(field_x - label_x, -1));
    row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, label_x);

    m_filament_combo = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                    wxSize(18 * em, -1), 0, nullptr, wxCB_READONLY);
    m_filament_combo->SetToolTip(_L("A set travels between printers, so it stores the interface filament as a "
                                    "TYPE rather than a slot number. The slot is worked out when the set is "
                                    "applied, on whatever printer that happens to be."));

    // "same" and "soluble" always; then every filament type loaded on this printer, so the usual
    // choice is one click away. An existing value that is none of those is kept as its own entry
    // rather than silently rewritten.
    auto add_entry = [this](const wxString& label, const std::string& value) {
        if (std::find(m_filament_type_values.begin(), m_filament_type_values.end(), value) != m_filament_type_values.end())
            return;
        m_filament_combo->Append(label);
        m_filament_type_values.push_back(value);
    };
    add_entry(_L("Same as the part"), "same");
    add_entry(_L("Soluble"), "soluble");
    if (const ConfigOptionStrings* types = m_filament_config.option<ConfigOptionStrings>("filament_type"); types != nullptr)
        for (const std::string& type : types->values)
            if (!type.empty())
                add_entry(from_u8(type), type);
    if (!m_set.interface_filament_type.empty())
        add_entry(from_u8(m_set.interface_filament_type), m_set.interface_filament_type);

    int sel = 0;
    for (size_t i = 0; i < m_filament_type_values.size(); ++ i)
        if (m_filament_type_values[i] == m_set.interface_filament_type)
            sel = int(i);
    m_filament_combo->SetSelection(sel);
    m_filament_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& evt) {
        evt.Skip();
        update_filament_note();
    });
    row->Add(m_filament_combo, 0, wxALIGN_CENTER_VERTICAL);

    parent_sizer->Add(row, 0, wxEXPAND);

    m_filament_note = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_filament_note->SetFont(Label::Body_14);
    m_filament_note->SetBackgroundColour(*wxWHITE); // same reason as the label above
    parent_sizer->Add(m_filament_note, 0, wxEXPAND | wxLEFT, label_x);
    parent_sizer->AddSpacer(em / 2);

    update_filament_note();
}

void SupportSetEditDialog::update_filament_note()
{
    if (m_filament_note == nullptr || m_filament_combo == nullptr)
        return;
    const int sel = m_filament_combo->GetSelection();
    if (sel < 0 || size_t(sel) >= m_filament_type_values.size())
        return;
    const std::string& type = m_filament_type_values[size_t(sel)];

    std::string warning;
    const int   slot = resolve_interface_filament(type, m_filament_config, &warning);
    wxString    text;
    bool        is_warning = !warning.empty();
    if (is_warning)
        text = from_u8(warning);
    else if (slot > 0)
        text = format_wxstr(_L("On this printer that is filament %1%."), slot);
    else
        text = _L("The support interface uses the same filament as the part.");
    m_filament_note->SetLabel(text);
    m_filament_note->SetForegroundColour(is_warning ? wxColour("#ED6B21")
                                                    : StateColor::darkModeColorFor(wxColour("#8F8F8F")));
    m_filament_note->GetParent()->Layout();
}

// The Style list only offers the styles that belong to the chosen Type - Default/Grid/Snug for
// normal, Default and the Tree styles for tree - the same filter TabPrint::toggle_options applies
// on the process tab (Tab.cpp), so the editor cannot save a Grid style onto a tree set. A style
// that does not fit the new type falls back to Default, in the field and in the set alike.
void SupportSetEditDialog::filter_support_styles()
{
    Field*                              field = nullptr;
    std::shared_ptr<ConfigOptionsGroup> owner;
    for (const std::shared_ptr<ConfigOptionsGroup>& og : m_groups)
        if ((field = og->get_field("support_style")) != nullptr) {
            owner = og;
            break;
        }
    auto choice = dynamic_cast<Choice*>(field);
    if (choice == nullptr)
        return;
    auto cb = dynamic_cast<ComboBox*>(choice->window);
    const ConfigOptionDef* def = print_config_def.get("support_style");
    if (cb == nullptr || def == nullptr)
        return;
    const SupportType      type  = m_config.opt_enum<SupportType>("support_type");
    const SupportMaterialStyle cur = m_config.opt_enum<SupportMaterialStyle>("support_style");
    static const std::vector<int> set_normal = { smsDefault, smsGrid, smsSnug };
    static const std::vector<int> set_tree   = { smsDefault, smsTreeSlim, smsTreeStrong, smsTreeHybrid, smsTreeOrganic };
    const std::vector<int>& set = is_tree(type) ? set_tree : set_normal;

    auto& opt = const_cast<ConfigOptionDef&>(field->m_opt);
    opt.enum_values.clear();
    opt.enum_labels.clear();
    cb->Clear();
    for (int i : set) {
        opt.enum_values.push_back(def->enum_values[i]);
        opt.enum_labels.push_back(def->enum_labels[i]);
        cb->Append(_(def->enum_labels[i]));
    }
    const bool fits = std::find(set.begin(), set.end(), int(cur)) != set.end();
    if (! fits)
        m_config.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(smsDefault));
    // Re-read the (possibly reset) value into the rebuilt list, the way the group filled it at start.
    owner->reload_config();
}

void SupportSetEditDialog::toggle_fields()
{
    const ConfigOption* ironing = m_config.option("support_ironing");
    const bool          on      = ironing != nullptr && ironing->getBool();
    for (const std::string& key : { "support_ironing_pattern", "support_ironing_flow", "support_ironing_spacing" })
        for (const std::shared_ptr<ConfigOptionsGroup>& og : m_groups)
            if (Field* field = og->get_field(key); field != nullptr)
                field->toggle(on);
}

void SupportSetEditDialog::collect()
{
    // Only the keys this window shows are written back. A set carries more than these
    // (support_set_keys() is the whole Support category minus the excluded list - enable_support
    // and support_object_xy_distance among them), and those values are none of the editor's
    // business: they stay exactly as they were on disk.
    for (const std::string& key : m_config.keys())
        m_set.values[key] = m_config.opt_serialize(key);
    if (m_filament_combo != nullptr) {
        const int sel = m_filament_combo->GetSelection();
        if (sel >= 0 && size_t(sel) < m_filament_type_values.size())
            m_set.interface_filament_type = m_filament_type_values[size_t(sel)];
    }
}

} // namespace GUI
} // namespace Slic3r
