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

#include "FavoriteFilamentsUI.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
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
            if (m_kind == PresetManagerKind::Filament)
                close_with(Then::CreateFilament);
        } else if (cmd == "pm_edit") {
            if (m_kind == PresetManagerKind::Filament && j.contains("filament_id") && j["filament_id"].is_string()) {
                m_edit_filament_id = j["filament_id"].get<std::string>();
                if (!m_edit_filament_id.empty())
                    close_with(Then::EditFilament);
            }
        } else if (cmd == "pm_close") {
            close_with(Then::Nothing);
        }
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": bad message from the preset manager page: " << ex.what();
    }
}

}} // namespace Slic3r::GUI
