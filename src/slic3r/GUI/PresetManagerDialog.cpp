#include "PresetManagerDialog.hpp"

#include <algorithm>
#include <cstdio>

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <wx/display.h>
#include <wx/sizer.h>
#include <wx/webview.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "CreatePresetsDialog.hpp"
#include "FavoriteFilamentsUI.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "ParamsDialog.hpp"
#include "Plater.hpp"
#include "SavePresetDialog.hpp"
#include "Tab.hpp"
#include "WebGuideDialog.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/WebView.hpp"

namespace Slic3r { namespace GUI {

namespace {

FavoriteFilamentsUI::Kind pm_fav_kind(PresetManagerKind kind)
{
    return kind == PresetManagerKind::Filament ? FavoriteFilamentsUI::Kind::Filament : FavoriteFilamentsUI::Kind::Process;
}

const char *pm_kind_name(PresetManagerKind kind) { return kind == PresetManagerKind::Filament ? "filament" : "process"; }

std::string pm_size_key(PresetManagerKind kind) { return std::string("preset_manager_size_") + pm_kind_name(kind); }

std::map<std::string, std::string> pm_section_of(const AppConfig &config, const std::string &name)
{
    return config.has_section(name) ? config.get_section(name) : std::map<std::string, std::string>();
}

} // namespace

PresetManagerDialog::PresetManagerDialog(wxWindow *parent, PresetManagerKind kind)
    : DPIDialog(parent, wxID_ANY, kind == PresetManagerKind::Filament ? _L("Filament manager") : _L("Process manager"), wxDefaultPosition,
                wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxSYSTEM_MENU | wxRESIZE_BORDER | wxMAXIMIZE_BOX)
    , m_kind(kind)
{
    // Esc is the page's to handle (a popover, an edit, then Close): never the dialog's default Cancel.
    SetEscapeId(wxID_NONE);
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

    wxString url = file_url_from_path(boost::filesystem::path(resources_dir()) / "web/guide/preset_manager/index.html");
    url += wxString("?kind=") + pm_kind_name(kind);
    const wxString lang = wxGetApp().current_language_code_safe();
    if (!lang.IsEmpty())
        url += "&lang=" + lang;
    m_view = WebView::CreateWebView(this, url);
    auto *sizer = new wxBoxSizer(wxVERTICAL);
    if (m_view != nullptr) {
        Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PresetManagerDialog::on_message, this, m_view->GetId());
        sizer->Add(m_view, 1, wxEXPAND);
    } else {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": could not create the web view";
    }
    SetSizer(sizer);

    Bind(wxEVT_CHAR_HOOK, &PresetManagerDialog::on_char_hook, this);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &evt) {
        save_size();
        evt.Skip();
    });

    m_fav_subscription = FavoriteFilamentsUI::subscribe([this]() { send_favorites(); }, pm_fav_kind(m_kind));

    // 1100 x 720 DIP (or the size it had last time), within 90 % of the screen; at least 760 x 520.
    const int    disp = wxDisplay::GetFromWindow(parent != nullptr ? parent : this);
    const wxRect area = wxDisplay(disp == wxNOT_FOUND ? 0u : unsigned(disp)).GetClientArea();
    wxSize       want = FromDIP(wxSize(1100, 720));
    int          w = 0, h = 0;
    if (std::sscanf(wxGetApp().app_config->get(pm_size_key(kind)).c_str(), "%d,%d", &w, &h) == 2 && w >= 300 && h >= 300 && w <= 10000 && h <= 10000)
        want = FromDIP(wxSize(w, h));
    const wxSize min_size = FromDIP(wxSize(760, 520));
    want.x = std::max(std::min(want.x, area.width * 90 / 100), std::min(min_size.x, area.width));
    want.y = std::max(std::min(want.y, area.height * 90 / 100), std::min(min_size.y, area.height));
    SetMinSize(wxSize(std::min(min_size.x, area.width), std::min(min_size.y, area.height)));
    SetSize(want);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    build_model();
}

PresetManagerDialog::~PresetManagerDialog()
{
    if (m_fav_subscription != 0)
        FavoriteFilamentsUI::unsubscribe(m_fav_subscription);
}

void PresetManagerDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    SetMinSize(FromDIP(wxSize(760, 520)));
    Layout();
    Refresh();
}

void PresetManagerDialog::save_size()
{
    if (IsMaximized() || IsIconized())
        return;
    const wxSize dip = ToDIP(GetSize());
    wxGetApp().app_config->set(pm_size_key(m_kind), std::to_string(dip.x) + "," + std::to_string(dip.y));
}

void PresetManagerDialog::build_model()
{
    PresetBundle &bundle   = *wxGetApp().preset_bundle;
    std::string   bed_key;
    if (m_kind == PresetManagerKind::Filament) {
        // The bed temperature of the current plate's bed type, as the Plater uses it.
        if (const auto *opt = bundle.project_config.option<ConfigOptionEnum<BedType>>("curr_bed_type"); opt != nullptr && opt->value != btDefault)
            bed_key = get_bed_temp_key(opt->value);
        else
            bed_key = get_bed_temp_key(btPEI);
    }
    m_model = build_preset_manager_model(m_kind, collect_preset_manager_items(bundle, m_kind, bed_key));
    m_row_index.clear();
    for (size_t i = 0; i < m_model.rows.size(); ++i)
        m_row_index.emplace(m_model.rows[i].key, i);
}

void PresetManagerDialog::send(const std::string &command, nlohmann::json payload)
{
    payload["command"]     = command;
    payload["sequence_id"] = "10001";
    const wxString js = "HandleStudio(" + wxString::FromUTF8(payload.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace)) + ")";
    // dialog-level CallAfter: discarded on destruction, never runs on a dangling `this`; never from
    // inside the script-message callback (WKWebView re-entrancy).
    CallAfter([this, js] {
        if (m_view != nullptr)
            WebView::RunScript(m_view, js);
    });
}

void PresetManagerDialog::send_rows()
{
    const PresetBundle &bundle = *wxGetApp().preset_bundle;
    std::set<std::string> favs = FavoriteFilamentsUI::load(pm_fav_kind(m_kind));
    send("pm_rows", preset_manager_rows_json(m_model, favs, bundle.printers.get_selected_preset_name()));
}

void PresetManagerDialog::send_favorites()
{
    nlohmann::json j;
    j["kind"] = pm_kind_name(m_kind);
    j["favs"] = nlohmann::json::array();
    for (const std::string &k : FavoriteFilamentsUI::load(pm_fav_kind(m_kind)))
        j["favs"].push_back(k);
    send("pm_stars", std::move(j));
}

void PresetManagerDialog::apply_enabled(const nlohmann::json &changes)
{
    if (!changes.is_array())
        return;
    AppConfig    &config = *wxGetApp().app_config;
    PresetBundle &bundle = *wxGetApp().preset_bundle;
    const std::string &section_name = m_kind == PresetManagerKind::Filament ? AppConfig::SECTION_FILAMENTS : AppConfig::SECTION_HIDDEN_PROCESSES;
    std::map<std::string, std::string> section = pm_section_of(config, section_name);
    const std::map<std::string, std::string> before = section;
    std::set<std::string> asked;
    for (const nlohmann::json &c : changes) {
        if (!c.is_object() || !c.contains("key") || !c["key"].is_string())
            continue;
        asked.insert(c["key"].get<std::string>());
        auto it = m_row_index.find(c["key"].get<std::string>());
        if (it == m_row_index.end())
            continue;
        const PresetManagerRow &row = m_model.rows[it->second];
        const bool on = c.value("on", true);
        // User presets are always shown; a preset in use stays shown.
        if (row.lock == "user" || (row.lock == "in_use" && !on))
            continue;
        section = m_kind == PresetManagerKind::Filament ? filament_section_with(std::move(section), row, on)
                                                        : hidden_process_section_with(std::move(section), row, on);
    }
    if (section == before)
        return refresh_enabled(asked);

    BOOST_LOG_TRIVIAL(info) << "Preset manager (" << pm_kind_name(m_kind) << "): " << changes.size() << " Enabled change(s) applied";
    if (m_kind == PresetManagerKind::Filament) {
        // Visibility only (no disk reload, edits stay); the full reload runs once when the dialog closes.
        apply_enabled_filaments(config, bundle, section, FilamentsReload::Visibility, {}, true);
        wxGetApp().sidebar().update_presets(Preset::TYPE_FILAMENT);
        if (Tab *tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT))
            tab->update_tab_ui();
    } else {
        if (section.empty())
            config.clear_section(section_name);
        else
            config.set_section(section_name, section);
        bundle.load_installed_prints(config);
        // update_presets(TYPE_PRINT) refreshes the process combo of the Process tab (the sidebar's).
        wxGetApp().sidebar().update_presets(Preset::TYPE_PRINT);
    }
    refresh_enabled(asked);
}

void PresetManagerDialog::refresh_enabled(const std::set<std::string> &asked)
{
    const PresetBundle     &bundle     = *wxGetApp().preset_bundle;
    const PresetCollection &collection = m_kind == PresetManagerKind::Filament ? bundle.filaments : bundle.prints;
    nlohmann::json          changed    = nlohmann::json::array();
    for (PresetManagerRow &row : m_model.rows) {
        if (!row.is_system) {
            if (asked.count(row.key))
                changed.push_back({{"key", row.key}, {"enabled", row.enabled}, {"lock", row.lock}});
            continue;
        }
        bool visible = false;
        for (const std::string &name : row.members)
            if (const Preset *p = collection.find_preset(name, false); p != nullptr && p->is_visible) {
                visible = true;
                break;
            }
        const std::string old_lock = row.lock;
        const bool        old_on   = row.enabled;
        row.enabled = visible;
        row.lock    = row.in_use && row.enabled ? "in_use" : "";
        if (row.enabled != old_on || row.lock != old_lock || asked.count(row.key))
            changed.push_back({{"key", row.key}, {"enabled", row.enabled}, {"lock", row.lock}});
    }
    // Always answer, so the page can drop its "applying" state.
    send("pm_enabled_state", {{"rows", std::move(changed)}});
}

void PresetManagerDialog::close_with(Then then)
{
    if (m_closing)
        return;
    m_closing = true;
    m_then    = then;
    save_size();
    CallAfter([this] { EndModal(wxID_CLOSE); });
}

void PresetManagerDialog::on_char_hook(wxKeyEvent &evt)
{
    if (evt.GetKeyCode() != WXK_ESCAPE) {
        evt.Skip();
        return;
    }
    // Inside the page the page sees the key itself; elsewhere in the dialog, ask the page what Esc means.
    wxWindow *focus = wxWindow::FindFocus();
    if (m_view != nullptr && focus != nullptr && (focus == m_view || m_view->IsDescendant(focus))) {
        evt.Skip();
        return;
    }
    send("pm_escape", nlohmann::json::object());
}

void PresetManagerDialog::on_message(wxWebViewEvent &evt)
{
    try {
        const nlohmann::json j   = nlohmann::json::parse(into_u8(evt.GetString()));
        const std::string    cmd = j.value("command", std::string());
        if (cmd == "pm_ready") {
            send_rows();
        } else if (cmd == "pm_star") {
            const std::string key = j.value("key", std::string());
            if (!key.empty())
                FavoriteFilamentsUI::toggle(key, pm_fav_kind(m_kind)); // the subscription sends pm_stars
        } else if (cmd == "pm_enabled") {
            if (j.contains("changes")) {
                const nlohmann::json changes = j["changes"];
                // Out of the script-message callback: the apply refreshes other windows.
                CallAfter([this, changes] { apply_enabled(changes); });
            }
        } else if (cmd == "pm_create") {
            // Child dialogs open from a CallAfter, never inside the script-message callback.
            if (m_kind == PresetManagerKind::Filament)
                CallAfter([this] { create_filament(); });
        } else if (cmd == "pm_edit") {
            if (m_kind == PresetManagerKind::Filament && j.contains("filament_id") && j["filament_id"].is_string()) {
                const std::string filament_id = j["filament_id"].get<std::string>();
                if (!filament_id.empty())
                    CallAfter([this, filament_id] { edit_filament(filament_id); });
            }
        } else if (cmd == "pm_check_name") {
            const std::string key  = j.value("id", std::string());
            const std::string name = j.value("name", std::string());
            const bool        all  = j.value("scope", std::string("family")) != "one";
            CallAfter([this, key, name, all] { check_name(key, name, all); });
        } else if (cmd == "pm_rename") {
            const std::string key  = j.value("id", std::string());
            const std::string name = j.value("name", std::string());
            const bool        all  = j.value("scope", std::string("family")) != "one";
            CallAfter([this, key, name, all] { rename(key, name, all); });
        } else if (cmd == "pm_delete") {
            const std::string key = j.value("id", std::string());
            CallAfter([this, key] { delete_row(key); });
        } else if (cmd == "pm_close") {
            close_with(Then::Nothing);
        }
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": bad message from the preset manager page: " << ex.what();
    }
}

void PresetManagerDialog::reload_rows()
{
    build_model();
    send_rows();
}

void PresetManagerDialog::refresh_app_presets()
{
    // What the plater does after the filament editors (Plater::priv::on_create_filament / on_modify_filament).
    if (wxGetApp().mainframe != nullptr)
        wxGetApp().mainframe->update_side_preset_ui();
    if (wxGetApp().plater() != nullptr)
        wxGetApp().plater()->update_ui_from_settings();
    wxGetApp().sidebar().update_all_preset_comboboxes();
    if (Tab *tab = wxGetApp().get_tab(m_kind == PresetManagerKind::Filament ? Preset::TYPE_FILAMENT : Preset::TYPE_PRINT))
        tab->update_tab_ui();
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
}

// ---- child dialogs: over the manager, which stays open and lists the result ----

void PresetManagerDialog::create_filament()
{
    if (m_busy || m_closing)
        return;
    m_busy = true;
    int res = wxID_CANCEL;
    {
        CreateFilamentPresetDialog dlg(this);
        res = dlg.ShowModal();
    }
    if (res == wxID_OK) {
        refresh_app_presets();
        CreatePresetSuccessfulDialog success_dlg(this, SuccessType::FILAMENT);
        success_dlg.ShowModal();
    }
    m_busy = false;
    reload_rows();
}

void PresetManagerDialog::edit_filament(const std::string &filament_id)
{
    if (m_busy || m_closing)
        return;
    m_busy = true;
    Filamentinformation info;
    info.filament_id = filament_id;
    int                     res = wxID_CANCEL;
    std::shared_ptr<Preset> need_edit_preset;
    {
        EditFilamentPresetDialog dlg(this, &info);
        res              = dlg.ShowModal();
        need_edit_preset = dlg.get_need_edit_preset();
    }
    refresh_app_presets();
    m_busy = false;
    if (res == wxID_EDIT && need_edit_preset) {
        // "Edit" on one of its presets: the settings window needs the manager out of the way.
        m_edit_preset_name = need_edit_preset->name;
        m_edit_filament_id = filament_id;
        close_with(Then::EditPresetSettings);
        return;
    }
    reload_rows();
}

// ---- rename / delete of user presets ----

PresetCollection &PresetManagerDialog::collection()
{
    PresetBundle &bundle = *wxGetApp().preset_bundle;
    return m_kind == PresetManagerKind::Filament ? static_cast<PresetCollection &>(bundle.filaments) : static_cast<PresetCollection &>(bundle.prints);
}

const PresetManagerRow *PresetManagerDialog::user_row(const std::string &key) const
{
    auto it = m_row_index.find(key);
    if (it == m_row_index.end())
        return nullptr;
    const PresetManagerRow &row = m_model.rows[it->second];
    return row.is_system || row.members.empty() ? nullptr : &row;
}

PresetRenamePlan PresetManagerDialog::plan_rename(const PresetManagerRow &row, const std::string &typed, bool whole_family)
{
    return plan_preset_rename(collection(), row.members.front(), typed,
                              m_kind == PresetManagerKind::Filament && whole_family ? row.family : std::vector<std::string>());
}

static const char *pm_level_name(PresetNameCheck::Level level)
{
    return level == PresetNameCheck::Level::Invalid ? "invalid" : level == PresetNameCheck::Level::Warning ? "warn" : "ok";
}

void PresetManagerDialog::check_name(const std::string &key, const std::string &typed, bool whole_family)
{
    nlohmann::json r = {{"id", key}, {"name", typed}};
    if (const PresetManagerRow *row = user_row(key); row == nullptr) {
        r["level"]   = "invalid";
        r["message"] = into_u8(_L("System presets cannot be renamed."));
    } else {
        const PresetRenamePlan plan = plan_rename(*row, typed, whole_family);
        r["level"]   = pm_level_name(plan.check.level);
        r["message"] = into_u8(preset_name_check_message(plan.check, plan.checked_name));
        r["count"]   = plan.renames.size();
    }
    send("pm_name_check", std::move(r));
}

void PresetManagerDialog::rename(const std::string &key, const std::string &typed, bool whole_family)
{
    auto result = [this, &key](bool ok, const wxString &message, const std::string &new_key) {
        send("pm_rename_result", {{"id", key}, {"ok", ok}, {"message", into_u8(message)}, {"new_key", new_key}});
    };
    if (m_busy || m_closing)
        return result(false, wxString(), std::string());
    const PresetManagerRow *row = user_row(key);
    if (row == nullptr)
        return result(false, _L("System presets cannot be renamed."), std::string());
    const std::string      row_name = row->members.front();
    const PresetRenamePlan plan     = plan_rename(*row, typed, whole_family);
    if (!plan.check.ok())
        return result(false, preset_name_check_message(plan.check, plan.checked_name), std::string());
    if (plan.renames.empty())
        return result(true, wxString(), key);

    PresetCollection  &presets = collection();
    const Preset::Type type    = m_kind == PresetManagerKind::Filament ? Preset::TYPE_FILAMENT : Preset::TYPE_PRINT;
    // A preset with unsaved edits is saved or discarded on its settings tab first.
    const std::string edited = presets.get_edited_preset().name;
    for (const auto &[from, to] : plan.renames) {
        if (from == edited && presets.current_is_dirty()) {
            m_busy = true;
            MessageDialog(this,
                          wxString::Format(_L("\"%s\" has unsaved changes. Save or discard them in its settings first, then rename it."), from_u8(from)),
                          _L("Rename preset"), wxOK | wxICON_WARNING)
                .ShowModal();
            m_busy = false;
            return result(false, _L("Save or discard the unsaved changes first."), std::string());
        }
    }

    const FavoriteFilamentsUI::Kind fav_kind = pm_fav_kind(m_kind);
    std::string                     new_key  = key;
    wxString                        error;
    for (const auto &[from, to] : plan.renames) {
        std::string why;
        if (!presets.rename_user_preset(from, to, &why)) {
            error = wxString::Format(_L("Could not rename \"%s\": %s"), from_u8(from), from_u8(why));
            break;
        }
        // Filament slots (filaments) / the per-nozzle processes (processes) of the project, the combos.
        wxGetApp().sidebar().update_presets_from_to(type, from, to);
        // A user preset's favourite key is its full name.
        FavoriteFilamentsUI::rename_key(from, to, fav_kind);
        if (from == row_name)
            new_key = "u|" + to;
    }
    BOOST_LOG_TRIVIAL(info) << "Preset manager (" << pm_kind_name(m_kind) << "): renamed " << plan.renames.size() << " preset(s) from " << row_name
                            << (error.empty() ? "" : " (stopped by an error)");
    refresh_app_presets();
    if (wxGetApp().plater() != nullptr)
        wxGetApp().plater()->update_project_dirty_from_presets();

    wxString note;
    if (error.empty() && wxGetApp().app_config->get_bool("sync_bambu_user_presets"))
        note = _L("Renamed. A preset copied from Bambu Studio can come back under its old name the next time presets are copied.");
    result(error.empty(), error.empty() ? note : error, new_key);
    reload_rows();
}

void PresetManagerDialog::delete_row(const std::string &key)
{
    auto result = [this, &key](bool ok, const wxString &message) {
        send("pm_delete_result", {{"id", key}, {"ok", ok}, {"message", into_u8(message)}});
    };
    if (m_busy || m_closing)
        return result(false, wxString());
    const PresetManagerRow *row = user_row(key);
    if (row == nullptr)
        return result(false, _L("System presets cannot be deleted."));
    const std::string  name    = row->members.front();
    PresetCollection  &presets = collection();
    PresetBundle      &bundle  = *wxGetApp().preset_bundle;
    const Preset::Type type    = m_kind == PresetManagerKind::Filament ? Preset::TYPE_FILAMENT : Preset::TYPE_PRINT;
    const Preset      *preset  = presets.find_preset(name, false);
    if (preset == nullptr || !preset->is_user())
        return result(false, _L("This preset cannot be deleted."));

    // A preset other presets inherit is refused, as the settings tab does.
    const std::vector<std::string> children = presets.user_presets_inheriting(name);
    if (!children.empty()) {
        wxString msg = _L("Presets inherited by other presets cannot be deleted!");
        msg += "\n";
        msg += _L_PLURAL("The following presets inherit this preset.", "The following preset inherits this preset.", children.size());
        for (const std::string &child : children)
            msg += "\n - " + from_u8(child);
        m_busy = true;
        MessageDialog(this, msg, _L("Delete Preset"), wxOK | wxICON_ERROR).ShowModal();
        m_busy = false;
        return result(false, wxString());
    }

    // "Used by...": what moves when it goes.
    const PresetBundle::PresetUsage usage = bundle.preset_usage(type, name);
    wxString msg = wxString::Format(_L("Delete the preset \"%s\"?"), from_u8(name));
    if (usage.in_use()) {
        wxString used;
        for (size_t slot : usage.filament_slots)
            used += "\n - " + wxString::Format(_L("Filament slot %d of this project"), int(slot + 1));
        if (usage.selected)
            used += "\n - " + (type == Preset::TYPE_FILAMENT ? _L("The filament shown in the filament settings") : _L("The plate's process"));
        for (size_t e : usage.nozzles)
            used += "\n - " + wxString::Format(_L("The process of nozzle %d"), int(e + 1));
        for (const std::string &id : usage.nozzle_slots)
            used += "\n - " + wxString::Format(_L("The process of rack slot %s"), from_u8(id));
        msg += "\n\n" + _L("Used by:") + used + "\n\n" +
               (type == Preset::TYPE_FILAMENT ? _L("They switch to the next visible filament preset.") :
                                                _L("The plate switches to the next visible process preset; a nozzle goes back to the plate's process."));
    }
    if (type == Preset::TYPE_FILAMENT && presets.get_preset_base(*preset) == preset)
        msg += "\n\n" + _L("If the preset corresponds to a filament currently in use on your printer, please reset the filament information for that slot.");
    if (wxGetApp().app_config->get_bool("sync_bambu_user_presets"))
        msg += "\n\n" + _L("A preset copied from Bambu Studio comes back the next time presets are copied from there.");
    m_busy           = true;
    const int answer = MessageDialog(this, msg, _L("Delete Preset"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION).ShowModal();
    m_busy           = false;
    if (answer != wxID_YES)
        return result(false, wxString());

    bool deleted = false;
    if (usage.selected) {
        // The selected preset goes through its settings tab, which picks the next visible preset.
        if (Tab *tab = wxGetApp().get_tab(type))
            tab->delete_preset(true);
        deleted = presets.find_preset(name, false) == nullptr;
    } else {
        std::string next_selected = presets.get_selected_preset_name();
        deleted = delete_user_preset_by_name(type, name, next_selected);
    }
    if (!deleted)
        return result(false, _L("The preset was not deleted."));

    const std::string next = presets.get_selected_preset_name();
    if (type == Preset::TYPE_FILAMENT) {
        // EditFilamentPresetDialog's repair: a slot naming a preset that is gone takes the selected one.
        for (std::string &slot : bundle.filament_presets)
            if (presets.find_preset(slot, false) == nullptr)
                slot = next;
        wxGetApp().sidebar().update_presets_from_to(type, name, next);
    } else {
        // The per-nozzle choices that named it: back to the plate's process.
        wxGetApp().sidebar().update_presets_from_to(type, name, std::string());
    }
    FavoriteFilamentsUI::remove_key(name, pm_fav_kind(m_kind));
    BOOST_LOG_TRIVIAL(info) << "Preset manager (" << pm_kind_name(m_kind) << "): deleted a user preset" << (usage.in_use() ? " that was in use" : "");
    refresh_app_presets();
    if (wxGetApp().plater() != nullptr)
        wxGetApp().plater()->update_project_dirty_from_presets();
    result(true, wxString());
    reload_rows();
}

}} // namespace Slic3r::GUI
