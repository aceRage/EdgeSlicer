#include "LayerlingPanel.hpp"

#include "LayerlingBrowser.hpp"
#include "LayerlingServer.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "ObjectDataViewModel.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "3DScene.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BRep/CadEdit.hpp"
#include "libslic3r/Format/STEPExport.hpp"
#include "libslic3r/Layerling.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/UntrustedInput.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>

#include <boost/algorithm/string/predicate.hpp>

#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/gauge.h>
#include <wx/hyperlink.h>
#include <wx/menu.h>
#include <wx/platinfo.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/utils.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <thread>

namespace Slic3r {
namespace GUI {

namespace fs = boost::filesystem;

// --------------------------------------------------------------------------- availability ----

bool layerling_supported()
{
#if defined(_WIN32)
    return true;
#elif defined(__APPLE__)
    // WKDownload (the exports) needs macOS 12.
    return wxPlatformInfo::Get().CheckOSVersion(12, 0);
#else
    return true;
#endif
}

bool layerling_tab_enabled()
{
    return layerling_supported() && wxGetApp().app_config != nullptr && wxGetApp().app_config->get_bool("show_layerling_tab");
}

namespace {

// app_config keys
constexpr const char *KEY_VERSION     = "layerling_version";          // the installed version in use
constexpr const char *KEY_PREVIOUS    = "layerling_previous_version"; // kept until the new one has loaded once
constexpr const char *KEY_VERIFIED    = "layerling_verified";         // "1" once the version in use has loaded
constexpr const char *KEY_LAST_CHECK  = "layerling_last_check";       // seconds since the epoch
constexpr const char *KEY_LATEST     = "layerling_latest_version";   // newest release seen by the last check
constexpr const char *KEY_BASE_URL    = "layerling_base_url";         // the user's own Layerling (Source::Custom)
constexpr const char *KEY_SOURCE      = "layerling_source";           // "builtin" / "custom" (layerling::select_source)
constexpr const char *KEY_MCP_CHECKOUT = "layerling_mcp_checkout";    // a Layerling source folder, for the MCP setup text
constexpr const char *KEY_ONE_OBJECT  = "layerling_send_as_one_object"; // "0": Send to Plate keeps bodies as separate objects

std::int64_t now_seconds() { return std::int64_t(std::time(nullptr)); }

fs::path layerling_dir() { return fs::path(data_dir()) / "layerling"; }

fs::path temp_export_dir()
{
    return fs::temp_directory_path() / ("edgeslicer-layerling-" + std::to_string(wxGetProcessId()));
}

std::string cfg(const char *key) { return wxGetApp().app_config ? wxGetApp().app_config->get(key) : std::string(); }
void        set_cfg(const char *key, const std::string &value)
{
    if (wxGetApp().app_config)
        wxGetApp().app_config->set(key, value);
}

// Send to Plate: several bodies become one object with parts (default) or stay separate objects.
bool send_as_one_object() { return cfg(KEY_ONE_OBJECT) != "0"; }

fs::path installed_app_root(const std::string &version)
{
    if (!layerling::is_safe_version(version))
        return fs::path();
    boost::system::error_code ec;
    const fs::path            dir = layerling_dir() / version;
    if (!fs::is_directory(dir, ec))
        return fs::path();
    return layerling::find_app_root(dir);
}

fs::path downloads_folder()
{
    const fs::path dir = into_path(wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Downloads));
    boost::system::error_code ec;
    if (!dir.empty() && fs::is_directory(dir, ec))
        return dir;
    return fs::path();
}

wxString size_mb(std::uint64_t bytes) { return wxString::Format("%.1f MB", double(bytes) / (1024.0 * 1024.0)); }

// Edit in Layerling: what goes from the plate.
struct EditItem
{
    size_t           object   = 0;
    int              instance = 0;
    std::vector<int> volumes; // model parts only
};
// The plate's selection as model parts: the selected parts (part mode), or every part of each
// selected object, in its first selected instance. `whole` says the second.
std::vector<EditItem> selection_items(bool *whole = nullptr);

} // namespace

// ----------------------------------------------------------------------------------- Impl ----

struct LayerlingPanel::Impl
{
    LayerlingPanel       *owner;
    std::shared_ptr<bool> alive { std::make_shared<bool>(true) };

    // ---- the "Get Layerling" page
    wxPanel      *get_page { nullptr };
    wxStaticText *get_title { nullptr };
    wxStaticText *get_text { nullptr };
    wxStaticText *get_licence { nullptr };
    wxHyperlinkCtrl *get_source { nullptr };
    wxStaticText *get_support { nullptr };
    wxBoxSizer   *get_funding_row { nullptr };
    Button       *btn_get { nullptr };
    Button       *btn_cancel { nullptr };
    Button       *btn_own { nullptr };
    wxGauge      *gauge { nullptr };
    wxStaticText *get_status { nullptr };

    // ---- the tab with Layerling in it
    wxPanel      *toolbar { nullptr };
    Button       *btn_send { nullptr };
    Button       *btn_reload { nullptr };
    Button       *btn_update { nullptr };
    Button       *btn_support { nullptr };
    Button       *btn_about { nullptr };
    wxStaticText *status { nullptr };
    wxPanel      *notice { nullptr };
    wxStaticText *notice_text { nullptr };
    Button       *notice_action { nullptr };
    Button       *notice_close { nullptr };
    std::function<void()> notice_callback;
    wxTimer       notice_timer;
    wxWindow     *host { nullptr };

    std::unique_ptr<LayerlingBrowser> browser;
    LayerlingServer                   server;
    bool                              active { false };
    bool                              started { false };
    std::string                       loaded_url;

    // ---- releases and downloads
    std::vector<layerling::FundingLink> funding { layerling::default_funding_links() };
    bool                                funding_fetched { false };
    layerling::ReleaseInfo              latest;
    bool                                have_latest { false };
    bool                                checking { false };
    Http::Ptr                           http;
    std::shared_ptr<std::atomic<bool>>  cancel_flag;
    bool                                downloading { false };
    std::thread                         installer;
    std::string                         installing_version;

    // ---- exports
    fs::path              last_export;
    std::string           last_export_name;
    layerling::DesignLink last_export_link; // the design it was exported from, when the page showed one
    bool        sending { false };

    // ---- Edit in Layerling: a file handed to Layerling's import (layerling::import_file_script)
    std::string          handover_name;
    std::string          handover_bytes;
    fs::path             handover_file;      // the exported file, for the fallback hint
    ObjectID             handover_object;    // the object to link to the design the import makes
    std::string          handover_origin;
    std::string          handover_before;    // the page's project before the import
    int                  handover_ticks { 0 };
    bool                 handover_sent { false };
    bool                 probing { false };
    wxTimer              handover_timer;

    explicit Impl(LayerlingPanel *owner) : owner(owner) {}
    ~Impl();

    void build_ui();
    void build_get_page();
    void build_main_page();
    void apply_colours();
    void rebuild_funding_row();

    void show_get_page();
    void show_main_page();
    void refresh();
    std::string base_url() const;
    // The user's own Layerling (layerling_source "custom" with a usable address) instead of the
    // downloaded copy. No downloads or update checks then.
    bool        custom_base() const { return layerling::select_source(cfg(KEY_SOURCE), cfg(KEY_BASE_URL)) == layerling::Source::Custom; }
    // The origin the tab shows (or will show): the custom address, else the local server.
    std::string current_origin() const;
    // Layerling can be shown: a custom address, or an installed copy.
    bool        can_show() const { return custom_base() || !installed_app_root(cfg(KEY_VERSION)).empty(); }

    enum class NoticeKind { Info, Warning, Error };
    NoticeKind notice_kind { NoticeKind::Info };
    void show_notice(const wxString &text, NoticeKind kind, const wxString &action_label = wxString(), std::function<void()> action = nullptr,
                     bool sticky = false);
    void hide_notice();
    void apply_notice_colours();

    // releases
    void fetch_funding();
    void check_for_update(bool force);
    void fetch_release(std::function<void(bool ok, const layerling::ReleaseInfo &, const std::string &error)> done);
    void start_download(bool is_update);
    void download(const layerling::ReleaseInfo &r, bool is_update);
    void download_finished(const layerling::ReleaseInfo &r, const fs::path &zip, bool is_update, const std::string &error);
    void install_finished(const layerling::ReleaseInfo &r, bool ok, const std::string &error, bool is_update);
    void cancel_download();
    void set_get_busy(bool busy);
    void offer_update();
    void on_page_loaded(bool ok, const std::string &url, const std::string &error);
    void prune_versions();
    void roll_back();

    // exports
    void on_export(const std::string &name, const fs::path &file);
    void send_to_plate(const fs::path &file, const std::string &name, const layerling::DesignLink &link);
    void apply_colours_to_objects(const std::vector<size_t> &objects, const std::vector<layerling::BodyColour> &bodies);
    void on_send_button();
    void on_support_button();
    void on_about_button();
    void reset_data();

    // Edit in Layerling, the round trip, Replace original
    void edit_selection();
    void open_design(const layerling::DesignLink &link);
    void start_handover(const std::string &name, std::string bytes, const fs::path &file, ObjectID link_object);
    void handover_tick();
    void handover_failed(const wxString &why);
    void stop_handover();
    std::vector<size_t> linked_objects(const std::string &origin, const std::string &project) const;
    void link_objects(const std::vector<size_t> &objects, const layerling::DesignLink &link);
    bool replace_object(size_t old_idx, size_t new_idx, const layerling::DesignLink &link, bool keep_colours);
    fs::path keep_step_file(const fs::path &file, const std::string &name);

    // Use my own Layerling, MCP
    void choose_source();
    void apply_source();
    void show_mcp_setup();
};

// ------------------------------------------------------------------------------------- UI ----

void LayerlingPanel::Impl::build_ui()
{
    auto *main = new wxBoxSizer(wxVERTICAL);
    build_get_page();
    build_main_page();
    main->Add(get_page, 1, wxEXPAND);
    main->Add(toolbar, 0, wxEXPAND);
    main->Add(notice, 0, wxEXPAND);
    main->Add(host, 1, wxEXPAND);
    owner->SetSizer(main);
    toolbar->Hide();
    notice->Hide();
    host->Hide();
    notice_timer.SetOwner(owner);
    owner->Bind(wxEVT_TIMER, [this](wxTimerEvent &) { hide_notice(); }, notice_timer.GetId());
    handover_timer.SetOwner(owner);
    owner->Bind(wxEVT_TIMER, [this](wxTimerEvent &) { handover_tick(); }, handover_timer.GetId());
    apply_colours();
}

void LayerlingPanel::Impl::build_get_page()
{
    get_page    = new wxPanel(owner, wxID_ANY);
    const int w = owner->FromDIP(560);

    get_title = new wxStaticText(get_page, wxID_ANY, _L("Layerling"));
    get_title->SetFont(Label::Head_18);
    get_text = new wxStaticText(get_page, wxID_ANY,
                                _L("Layerling is a free CAD for 3D printing that runs in a browser: put shapes on the plate, size them, "
                                   "cut holes, group them, and send the result straight to EdgeSlicer's plate.\n\n"
                                   "EdgeSlicer can download the official Layerling web app from its GitHub releases (about 50 MB) and run it "
                                   "here, on this computer, also without internet. Your designs stay on this computer. Layerling is not part "
                                   "of EdgeSlicer and is not installed with it."));
    get_text->SetFont(Label::Body_14);
    get_text->Wrap(w);
    get_licence = new wxStaticText(get_page, wxID_ANY,
                                   format_wxstr(_L("Layerling is made by %1% and licensed under the %2%."), layerling::AUTHOR,
                                                _L("GNU Affero General Public License v3.0 (AGPL-3.0)")));
    get_licence->SetFont(Label::Body_13);
    get_licence->Wrap(w);
    get_source = new wxHyperlinkCtrl(get_page, wxID_ANY, _L("Layerling's source code"), layerling::REPO_URL);
    get_support = new wxStaticText(get_page, wxID_ANY, format_wxstr(_L("Layerling is free and made by %1% - support the project:"), layerling::AUTHOR));
    get_support->SetFont(Label::Body_13);
    get_funding_row = new wxBoxSizer(wxHORIZONTAL);

    btn_get = new Button(get_page, _L("Download Layerling"));
    btn_get->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    btn_cancel = new Button(get_page, _L("Cancel"));
    btn_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    btn_cancel->Hide();
    gauge = new wxGauge(get_page, wxID_ANY, 1000, wxDefaultPosition, wxSize(w, owner->FromDIP(8)));
    gauge->Hide();
    get_status = new wxStaticText(get_page, wxID_ANY, wxEmptyString);
    btn_own = new Button(get_page, _L("Use my own Layerling..."));
    btn_own->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    btn_own->SetToolTip(_L("Show a Layerling you already run (a NAS, a home server, Docker) or layerling.com instead of downloading it"));
    get_status->SetFont(Label::Body_13);

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(btn_get, 0, wxRIGHT, owner->FromDIP(8));
    buttons->Add(btn_cancel, 0, wxRIGHT, owner->FromDIP(8));
    buttons->Add(btn_own, 0);

    auto *col = new wxBoxSizer(wxVERTICAL);
    col->Add(get_title, 0, wxBOTTOM, owner->FromDIP(12));
    col->Add(get_text, 0, wxBOTTOM, owner->FromDIP(12));
    col->Add(get_licence, 0, wxBOTTOM, owner->FromDIP(4));
    col->Add(get_source, 0, wxBOTTOM, owner->FromDIP(16));
    col->Add(buttons, 0, wxBOTTOM, owner->FromDIP(10));
    col->Add(gauge, 0, wxBOTTOM, owner->FromDIP(6));
    col->Add(get_status, 0, wxBOTTOM, owner->FromDIP(16));
    col->Add(get_support, 0, wxBOTTOM, owner->FromDIP(4));
    col->Add(get_funding_row, 0);

    auto *outer = new wxBoxSizer(wxVERTICAL);
    outer->AddStretchSpacer(1);
    outer->Add(col, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, owner->FromDIP(24));
    outer->AddStretchSpacer(2);
    get_page->SetSizer(outer);
    rebuild_funding_row();

    btn_get->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { start_download(false); });
    btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { cancel_download(); });
    btn_own->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { choose_source(); });
}

void LayerlingPanel::Impl::rebuild_funding_row()
{
    if (get_funding_row == nullptr)
        return;
    get_funding_row->Clear(true);
    for (const layerling::FundingLink &l : funding) {
        auto *link = new wxHyperlinkCtrl(get_page, wxID_ANY, from_u8(l.label), from_u8(l.url));
        link->SetToolTip(from_u8(l.url));
        // Always the system browser, never a view of ours.
        link->Bind(wxEVT_HYPERLINK, [url = l.url](wxHyperlinkEvent &) {
            if (untrusted::is_safe_to_open_externally(url))
                wxLaunchDefaultBrowser(from_u8(url));
        });
        get_funding_row->Add(link, 0, wxRIGHT, owner->FromDIP(16));
    }
    get_page->Layout();
}

void LayerlingPanel::Impl::build_main_page()
{
    toolbar = new wxPanel(owner, wxID_ANY);
    auto compact = [this](const wxString &label, const wxString &tip) {
        auto *b = new Button(toolbar, label);
        b->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        if (!tip.empty())
            b->SetToolTip(tip);
        return b;
    };
    btn_send = compact(_L("Send to Plate"), _L("Put what you export from Layerling (3MF, STL, OBJ or STEP) on EdgeSlicer's plate"));
    btn_send->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
    btn_reload  = compact(_L("Reload"), _L("Reload Layerling"));
    btn_update  = compact(wxEmptyString, _L("Download and install the newer Layerling"));
    btn_update->Hide();
    btn_support = compact(_L("Support Layerling"), _L("Layerling is free and made by henmedia - support the project"));
    btn_about   = compact(_L("About"), _L("Layerling's version, licence and source code; reset its data"));
    status      = new wxStaticText(toolbar, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    status->SetFont(Label::Body_12);

    auto *row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(btn_send, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, owner->FromDIP(8));
    row->Add(btn_reload, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, owner->FromDIP(8));
    row->Add(status, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, owner->FromDIP(10));
    row->Add(btn_update, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, owner->FromDIP(8));
    row->Add(btn_support, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, owner->FromDIP(8));
    row->Add(btn_about, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, owner->FromDIP(8));
    auto *tb = new wxBoxSizer(wxVERTICAL);
    tb->Add(row, 0, wxEXPAND | wxTOP | wxBOTTOM, owner->FromDIP(6));
    toolbar->SetSizer(tb);

    notice      = new wxPanel(owner, wxID_ANY);
    notice_text = new wxStaticText(notice, wxID_ANY, wxEmptyString);
    notice_text->SetFont(Label::Body_13);
    notice_action = new Button(notice, wxEmptyString);
    notice_action->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    notice_close = new Button(notice, _L("Close"));
    notice_close->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    auto *nrow = new wxBoxSizer(wxHORIZONTAL);
    nrow->Add(notice_text, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, owner->FromDIP(12));
    nrow->Add(notice_action, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, owner->FromDIP(8));
    nrow->Add(notice_close, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, owner->FromDIP(8));
    auto *ns = new wxBoxSizer(wxVERTICAL);
    ns->Add(nrow, 0, wxEXPAND | wxTOP | wxBOTTOM, owner->FromDIP(6));
    notice->SetSizer(ns);

    // The web view fills this window (WebView2 and WKWebView as native children, wxWebView in a sizer).
    host = new wxWindow(owner, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxCLIP_CHILDREN);

    btn_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_send_button(); });
    btn_reload->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (browser)
            browser->reload();
        else
            refresh();
    });
    btn_update->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { start_download(true); });
    btn_support->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_support_button(); });
    btn_about->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_about_button(); });
    notice_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { hide_notice(); });
    notice_action->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        auto cb = notice_callback;
        hide_notice();
        if (cb)
            cb();
    });
    host->Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) {
        evt.Skip();
        if (browser)
            browser->resize();
    });
    host->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent &evt) {
        evt.Skip();
        if (browser)
            browser->focus();
    });
}

void LayerlingPanel::Impl::apply_colours()
{
    const wxColour bg    = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
    const wxColour bar   = StateColor::darkModeColorFor(wxColour("#F8F8F8"));
    const wxColour text  = StateColor::darkModeColorFor(wxColour("#262E30"));
    const wxColour muted = StateColor::darkModeColorFor(wxColour("#6B6B6B"));
    owner->SetBackgroundColour(bg);
    get_page->SetBackgroundColour(bg);
    for (wxStaticText *t : {get_title, get_text, get_licence, get_support})
        t->SetForegroundColour(text);
    get_status->SetForegroundColour(muted);
    toolbar->SetBackgroundColour(bar);
    status->SetForegroundColour(muted);
    host->SetBackgroundColour(bg);
    apply_notice_colours();
    for (Button *b : {btn_get, btn_cancel, btn_own, btn_send, btn_reload, btn_update, btn_support, btn_about, notice_action, notice_close})
        b->Rescale();
    if (browser)
        browser->set_dark(wxGetApp().dark_mode());
    owner->Layout();
    owner->Refresh();
}

void LayerlingPanel::Impl::apply_notice_colours()
{
    const char *bg = notice_kind == NoticeKind::Error ? "#FDE8E8" : notice_kind == NoticeKind::Warning ? "#FFF3EB" : "#F3F4F6";
    notice->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(bg)));
    notice_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    notice->Refresh();
}

void LayerlingPanel::Impl::show_notice(const wxString &text, NoticeKind kind, const wxString &action_label, std::function<void()> action,
                                       bool sticky)
{
    notice_kind = kind;
    apply_notice_colours();
    notice_text->SetLabel(text);
    notice_text->Wrap((std::max)(owner->GetClientSize().x - owner->FromDIP(300), owner->FromDIP(300)));
    notice_callback = std::move(action);
    notice_action->SetLabel(action_label);
    notice_action->Show(!action_label.empty() && notice_callback != nullptr);
    notice->Show();
    owner->Layout();
    if (sticky)
        notice_timer.Stop();
    else
        notice_timer.StartOnce(20000);
}

void LayerlingPanel::Impl::hide_notice()
{
    notice_timer.Stop();
    notice_callback = nullptr;
    if (notice->IsShown()) {
        notice->Hide();
        owner->Layout();
    }
}

void LayerlingPanel::Impl::show_get_page()
{
    toolbar->Hide();
    notice->Hide();
    host->Hide();
    if (browser)
        browser->set_visible(false);
    get_page->Show();
    owner->Layout();
    if (!funding_fetched)
        fetch_funding();
}

void LayerlingPanel::Impl::show_main_page()
{
    get_page->Hide();
    toolbar->Show();
    host->Show();
    owner->Layout();
    if (browser) {
        browser->set_visible(active);
        browser->resize();
    }
}

std::string LayerlingPanel::Impl::base_url() const
{
    if (custom_base())
        return layerling::start_url(cfg(KEY_BASE_URL));
    return server.running() ? server.origin() + "/" : std::string();
}

std::string LayerlingPanel::Impl::current_origin() const
{
    const std::string shown = browser ? layerling::origin_of(browser->current_url()) : std::string();
    if (!shown.empty())
        return shown;
    return layerling::origin_of(base_url());
}

// Shows what fits the state: Layerling when it is installed (or a custom address is set), else
// the Get page.
void LayerlingPanel::Impl::refresh()
{
    std::string url;
    if (custom_base()) {
        server.stop();
        url = layerling::start_url(cfg(KEY_BASE_URL));
    } else {
        const fs::path root = installed_app_root(cfg(KEY_VERSION));
        if (root.empty()) {
            server.stop();
            show_get_page();
            return;
        }
        std::string err;
        if (!server.start(root, err)) {
            show_main_page();
            show_notice(format_wxstr(_L("Layerling could not be started: the local server did not start (%1%)."), from_u8(err)), NoticeKind::Error,
                        _L("Retry"), [this]() { refresh(); }, true);
            return;
        }
        if (server.moved())
            show_notice(format_wxstr(_L("Layerling's usual port %1% is in use by another program, so it runs on port %2% this time. Designs "
                                        "saved in Layerling before are kept, but show only when it runs on port %1% again: close the other "
                                        "program and restart EdgeSlicer."),
                                     server.wanted_port(), server.port()),
                        NoticeKind::Warning, wxString(), nullptr, true);
        url = layerling::start_url(server.origin());
    }
    const std::string origin = layerling::origin_of(url);
    show_main_page();
    if (!browser) {
        LayerlingBrowser::Callbacks cb;
        auto alive_ = alive;
        cb.on_loaded = [this, alive_](bool ok, const std::string &u, const std::string &e) {
            if (*alive_)
                on_page_loaded(ok, u, e);
        };
        cb.on_download = [this, alive_](const std::string &name, const fs::path &file) {
            if (*alive_)
                owner->CallAfter([this, alive_, name, file]() {
                    if (*alive_)
                        on_export(name, file);
                });
        };
        cb.on_download_failed = [this, alive_](const std::string &name, const std::string &error) {
            if (*alive_)
                owner->CallAfter([this, alive_, name, error]() {
                    if (*alive_)
                        show_notice(format_wxstr(_L("The export \"%1%\" did not arrive: %2%"), from_u8(name), from_u8(error)), NoticeKind::Error);
                });
        };
        cb.on_error = [this, alive_](const std::string &error) {
            if (*alive_)
                owner->CallAfter([this, alive_, error]() {
                    if (!*alive_)
                        return;
                    browser.reset();
                    show_notice(from_u8(error), NoticeKind::Error, _L("Retry"), [this]() { refresh(); }, true);
                });
        };
        browser = LayerlingBrowser::create(host, origin, temp_export_dir(), std::move(cb));
        if (!browser) {
            show_notice(_L("Layerling needs macOS 12 or later."), NoticeKind::Error, wxString(), nullptr, true);
            return;
        }
        browser->set_dark(wxGetApp().dark_mode());
    }
    browser->set_allowed_origin(origin);
    browser->set_visible(active);
    if (url != loaded_url) {
        loaded_url = url;
        BOOST_LOG_TRIVIAL(info) << "Layerling: loading " << url;
        browser->load(url);
    }
    browser->resize();
    const std::string v = cfg(KEY_VERSION);
    status->SetLabel(custom_base() ? from_u8(layerling::origin_of(url)) : format_wxstr(_L("Layerling %1%"), from_u8(v)));
    toolbar->Layout();
}

void LayerlingPanel::Impl::on_page_loaded(bool ok, const std::string &url, const std::string &error)
{
    if (custom_base())
        return;
    const bool ours = layerling::origin_of(url) == server.origin();
    if (ok && ours) {
        if (cfg(KEY_VERIFIED) != "1") {
            set_cfg(KEY_VERIFIED, "1");
            prune_versions();
        }
        return;
    }
    if (!ok && ours) {
        BOOST_LOG_TRIVIAL(warning) << "Layerling: page did not load: " << error;
        const std::string prev = cfg(KEY_PREVIOUS);
        if (cfg(KEY_VERIFIED) != "1" && !installed_app_root(prev).empty())
            show_notice(format_wxstr(_L("Layerling %1% did not load."), from_u8(cfg(KEY_VERSION))), NoticeKind::Error,
                        format_wxstr(_L("Go back to %1%"), from_u8(prev)), [this]() { roll_back(); }, true);
        else
            show_notice(_L("Layerling did not load."), NoticeKind::Error, _L("Reload"), [this]() {
                if (browser)
                    browser->reload();
            });
    }
}

// The new version has loaded: the older ones go. Only folders named like a version, directly in
// the Layerling folder, are removed - never the browser data (.profile) or anything else.
void LayerlingPanel::Impl::prune_versions()
{
    const std::string keep = cfg(KEY_VERSION);
    const fs::path    dir  = layerling_dir();
    boost::system::error_code ec;
    std::vector<fs::path> old;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name == keep || !layerling::is_safe_version(name) || !fs::is_directory(it->symlink_status()))
            continue;
        if (!std::isdigit((unsigned char) name[0]))
            continue;
        old.push_back(it->path());
    }
    for (const fs::path &p : old) {
        if (!untrusted::is_path_within_root(dir, p))
            continue;
        BOOST_LOG_TRIVIAL(info) << "Layerling: removing the older version " << p.filename().string();
        fs::remove_all(p, ec);
    }
    set_cfg(KEY_PREVIOUS, "");
}

void LayerlingPanel::Impl::roll_back()
{
    const std::string prev = cfg(KEY_PREVIOUS);
    if (installed_app_root(prev).empty())
        return;
    const std::string failed = cfg(KEY_VERSION);
    BOOST_LOG_TRIVIAL(warning) << "Layerling: going back from " << failed << " to " << prev;
    set_cfg(KEY_VERSION, prev);
    set_cfg(KEY_PREVIOUS, "");
    set_cfg(KEY_VERIFIED, "1");
    std::string err;
    server.start(installed_app_root(prev), err);
    loaded_url.clear();
    refresh();
}

// ------------------------------------------------------------------------------- releases ----

void LayerlingPanel::Impl::fetch_funding()
{
    funding_fetched = true;
    // The copy fetched last time, if any, until the fresh one arrives.
    const fs::path cache = layerling_dir() / ".funding.yml";
    {
        boost::nowide::ifstream in(cache.string(), std::ios::binary);
        if (in) {
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto        links = layerling::parse_funding_yml(text);
            if (!links.empty()) {
                funding = links;
                rebuild_funding_row();
            }
        }
    }
    auto alive_ = alive;
    Http::get(layerling::FUNDING_YML_URL)
        .timeout_connect(10)
        .timeout_max(30)
        .size_limit(64 * 1024)
        .on_complete([this, alive_, cache](std::string body, unsigned status) {
            if (status != 200)
                return;
            auto links = layerling::parse_funding_yml(body);
            if (links.empty())
                return;
            wxGetApp().CallAfter([this, alive_, links, body, cache]() {
                if (!*alive_)
                    return;
                funding = links;
                rebuild_funding_row();
                boost::system::error_code ec;
                fs::create_directories(cache.parent_path(), ec);
                boost::nowide::ofstream out(cache.string(), std::ios::binary | std::ios::trunc);
                out << body;
            });
        })
        .on_error([](std::string, std::string error, unsigned) { BOOST_LOG_TRIVIAL(info) << "Layerling: FUNDING.yml not fetched: " << error; })
        .perform();
}

void LayerlingPanel::Impl::fetch_release(std::function<void(bool, const layerling::ReleaseInfo &, const std::string &)> done)
{
    auto alive_ = alive;
    Http::get(layerling::LATEST_RELEASE_API)
        .header("Accept", "application/vnd.github+json")
        .header("X-GitHub-Api-Version", "2022-11-28")
        .timeout_connect(10)
        .timeout_max(30)
        .size_limit(4 * 1024 * 1024)
        .on_complete([alive_, done](std::string body, unsigned status) {
            layerling::ReleaseInfo r;
            std::string            err;
            const bool             ok = status == 200 && layerling::parse_release_json(body, r, &err);
            if (status != 200)
                err = "GitHub answered " + std::to_string(status);
            wxGetApp().CallAfter([alive_, done, ok, r, err]() {
                if (*alive_)
                    done(ok, r, err);
            });
        })
        .on_error([alive_, done](std::string, std::string error, unsigned status) {
            const std::string err = !error.empty() ? error : "GitHub answered " + std::to_string(status);
            wxGetApp().CallAfter([alive_, done, err]() {
                if (*alive_)
                    done(false, layerling::ReleaseInfo(), err);
            });
        })
        .perform();
}

// When the tab opens, at most once a day: is there a newer Layerling? Offered, never installed.
void LayerlingPanel::Impl::check_for_update(bool force)
{
    if (checking || downloading || custom_base() || cfg(KEY_VERSION).empty())
        return;
    std::int64_t last = 0;
    try {
        last = std::stoll(cfg(KEY_LAST_CHECK));
    } catch (...) {
        last = 0;
    }
    if (!force && !layerling::update_check_due(last, now_seconds())) {
        // The answer of the last check still stands.
        const std::string seen = cfg(KEY_LATEST);
        if (have_latest || (!seen.empty() && layerling::compare_versions(seen, cfg(KEY_VERSION)) > 0))
            offer_update();
        return;
    }
    checking = true;
    set_cfg(KEY_LAST_CHECK, std::to_string(now_seconds()));
    fetch_release([this](bool ok, const layerling::ReleaseInfo &r, const std::string &err) {
        checking = false;
        if (!ok) {
            BOOST_LOG_TRIVIAL(info) << "Layerling: update check failed: " << err;
            return;
        }
        latest      = r;
        have_latest = true;
        set_cfg(KEY_LATEST, r.version);
        offer_update();
    });
    fetch_funding();
}

void LayerlingPanel::Impl::offer_update()
{
    const std::string current = cfg(KEY_VERSION);
    const std::string newest  = have_latest ? latest.version : cfg(KEY_LATEST);
    if (newest.empty() || current.empty() || layerling::compare_versions(newest, current) <= 0) {
        btn_update->Hide();
        toolbar->Layout();
        return;
    }
    btn_update->SetLabel(format_wxstr(_L("Update to %1%"), from_u8(newest)));
    btn_update->Show();
    toolbar->Layout();
}

void LayerlingPanel::Impl::set_get_busy(bool busy)
{
    btn_get->Enable(!busy);
    btn_cancel->Show(busy);
    gauge->Show(busy);
    if (!busy)
        gauge->SetValue(0);
    btn_update->Enable(!busy);
    get_page->Layout();
    toolbar->Layout();
}

void LayerlingPanel::Impl::start_download(bool is_update)
{
    if (downloading || installer.joinable())
        return;
    downloading = true;
    set_get_busy(true);
    const wxString looking = _L("Looking up the latest Layerling release...");
    get_status->SetLabel(looking);
    if (is_update)
        status->SetLabel(looking);
    get_page->Layout();
    fetch_release([this, is_update](bool ok, const layerling::ReleaseInfo &r, const std::string &err) {
        if (!downloading)
            return; // cancelled meanwhile
        if (!ok) {
            downloading = false;
            set_get_busy(false);
            const wxString msg = format_wxstr(_L("The Layerling release could not be looked up: %1%"), from_u8(err));
            get_status->SetLabel(msg);
            get_status->Wrap(owner->FromDIP(560));
            if (is_update)
                show_notice(msg, NoticeKind::Error);
            get_page->Layout();
            return;
        }
        latest      = r;
        have_latest = true;
        set_cfg(KEY_LATEST, r.version);
        download(r, is_update);
    });
}

void LayerlingPanel::Impl::download(const layerling::ReleaseInfo &r, bool is_update)
{
    const fs::path dir = layerling_dir() / ".download";
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path zip = dir / (layerling::sanitize_export_name(r.asset_name) + ".part");
    FILE          *f   = ec ? nullptr : boost::nowide::fopen(zip.string().c_str(), "wb");
    if (f == nullptr) {
        download_finished(r, zip, is_update, "cannot write " + zip.string());
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "Layerling: downloading " << r.asset_url << " (" << r.asset_size << " bytes)";
    cancel_flag         = std::make_shared<std::atomic<bool>>(false);
    auto cancel         = cancel_flag;
    auto alive_         = alive;
    auto last_permille  = std::make_shared<int>(-1);
    // Written, hashed and closed on the transfer's thread (on_body, then on_complete or on_error),
    // so the file is closed before the GUI thread hears about the result.
    struct Sink
    {
        FILE             *file = nullptr;
        layerling::Sha256 sha;
        std::uint64_t     written = 0;
        std::string       digest;
        void close()
        {
            if (file != nullptr) {
                fclose(file);
                file   = nullptr;
                digest = sha.hex_digest();
            }
        }
        ~Sink()
        {
            if (file != nullptr)
                fclose(file);
        }
    };
    auto sink  = std::make_shared<Sink>();
    sink->file = f;
    const std::uint64_t expected = r.asset_size;
    get_status->SetLabel(format_wxstr(_L("Downloading Layerling %1% (%2%)..."), from_u8(r.version), size_mb(expected)));
    get_page->Layout();

    http = Http::get(r.asset_url)
               .tls_policy(Http::TlsPolicy::Verify)
               .timeout_connect(15)
               .size_limit(size_t(expected + 1024 * 1024))
               .on_body([cancel, sink, expected](unsigned status, const char *data, size_t size) {
                   if (*cancel || status >= 400 || sink->file == nullptr)
                       return false;
                   if (sink->written + size > expected)
                       return false; // more than GitHub said: stop
                   if (fwrite(data, 1, size, sink->file) != size)
                       return false;
                   sink->sha.update(data, size);
                   sink->written += size;
                   return true;
               })
               .on_progress([this, alive_, cancel, last_permille, expected](Http::Progress p, bool &stop) {
                   if (*cancel) {
                       stop = true;
                       return;
                   }
                   const size_t total = p.dltotal > 0 ? p.dltotal : size_t(expected);
                   if (total == 0)
                       return;
                   const int permille = int(std::min<std::uint64_t>(1000, std::uint64_t(p.dlnow) * 1000 / total));
                   if (permille == *last_permille)
                       return;
                   *last_permille     = permille;
                   const size_t now   = p.dlnow;
                   wxGetApp().CallAfter([this, alive_, permille, now, total]() {
                       if (!*alive_ || !downloading)
                           return;
                       gauge->SetValue(permille);
                       const wxString line = format_wxstr(_L("Downloading Layerling... %1% of %2%"), size_mb(now), size_mb(total));
                       get_status->SetLabel(line);
                       status->SetLabel(line);
                   });
               })
               .on_complete([this, alive_, r, zip, is_update, sink, expected](std::string, unsigned status) {
                   sink->close();
                   std::string err;
                   if (status != 200)
                       err = "GitHub answered " + std::to_string(status);
                   else if (sink->written != expected)
                       err = "the download is " + std::to_string(sink->written) + " bytes, GitHub said " + std::to_string(expected);
                   else if (!r.sha256.empty() && sink->digest != r.sha256)
                       err = "the download's SHA-256 checksum does not match the one GitHub published";
                   if (err.empty())
                       BOOST_LOG_TRIVIAL(info) << "Layerling: download complete, sha256 " << sink->digest
                                               << (r.sha256.empty() ? " (GitHub published none)" : " (matches)");
                   wxGetApp().CallAfter([this, alive_, r, zip, is_update, err]() {
                       if (*alive_)
                           download_finished(r, zip, is_update, err);
                   });
               })
               .on_error([this, alive_, r, zip, is_update, cancel, sink](std::string, std::string error, unsigned status) {
                   sink->close();
                   const std::string err = *cancel ? std::string("cancelled") : !error.empty() ? error : "GitHub answered " + std::to_string(status);
                   wxGetApp().CallAfter([this, alive_, r, zip, is_update, err]() {
                       if (*alive_)
                           download_finished(r, zip, is_update, err);
                   });
               })
               .perform();
}

void LayerlingPanel::Impl::cancel_download()
{
    if (cancel_flag)
        *cancel_flag = true;
    if (http)
        http->cancel();
    if (downloading && !installer.joinable()) {
        downloading = false;
        set_get_busy(false);
        get_status->SetLabel(_L("The download was cancelled."));
        get_page->Layout();
    }
}

void LayerlingPanel::Impl::download_finished(const layerling::ReleaseInfo &r, const fs::path &zip, bool is_update, const std::string &error)
{
    http.reset();
    boost::system::error_code ec;
    if (!error.empty() || !downloading) {
        fs::remove(zip, ec);
        if (!downloading || error == "cancelled")
            return; // cancelled: the Get page says so already
        downloading = false;
        set_get_busy(false);
        BOOST_LOG_TRIVIAL(error) << "Layerling: download failed: " << error;
        const wxString msg = format_wxstr(_L("Layerling could not be downloaded: %1%"), from_u8(error));
        get_status->SetLabel(msg);
        get_status->Wrap(owner->FromDIP(560));
        get_page->Layout();
        if (is_update)
            show_notice(msg, NoticeKind::Error);
        return;
    }
    // Unpack on a thread of its own: about 700 files.
    btn_cancel->Hide();
    gauge->Pulse();
    const wxString unpacking = format_wxstr(_L("Unpacking Layerling %1%..."), from_u8(r.version));
    get_status->SetLabel(unpacking);
    status->SetLabel(unpacking);
    get_page->Layout();
    installing_version = r.version;
    auto           alive_ = alive;
    const fs::path versions = layerling_dir();
    installer = std::thread([this, alive_, r, zip, versions, is_update]() {
        fs::path    root;
        std::string err;
        const bool  ok = layerling::install_archive(zip, versions, r.version, root, err);
        boost::system::error_code ec;
        fs::remove(zip, ec);
        wxGetApp().CallAfter([this, alive_, r, ok, err, is_update]() {
            if (*alive_)
                install_finished(r, ok, err, is_update);
        });
    });
}

void LayerlingPanel::Impl::install_finished(const layerling::ReleaseInfo &r, bool ok, const std::string &error, bool is_update)
{
    if (installer.joinable())
        installer.join();
    downloading = false;
    set_get_busy(false);
    if (!ok) {
        BOOST_LOG_TRIVIAL(error) << "Layerling: install failed: " << error;
        const wxString msg = format_wxstr(_L("Layerling could not be installed: %1%"), from_u8(error));
        get_status->SetLabel(msg);
        get_status->Wrap(owner->FromDIP(560));
        get_page->Layout();
        if (is_update)
            show_notice(msg, NoticeKind::Error);
        return;
    }
    const std::string old = cfg(KEY_VERSION);
    BOOST_LOG_TRIVIAL(info) << "Layerling: installed " << r.version << (old.empty() ? "" : " (was " + old + ")");
    if (!old.empty() && old != r.version && !installed_app_root(old).empty())
        set_cfg(KEY_PREVIOUS, old);
    set_cfg(KEY_VERSION, r.version);
    set_cfg(KEY_VERIFIED, "0");
    get_status->SetLabel(wxEmptyString);
    btn_update->Hide();
    // Same address, new files: the page reloads from the new folder; its storage stays.
    loaded_url.clear();
    refresh();
    if (is_update)
        show_notice(format_wxstr(_L("Layerling was updated to %1%. Your designs are kept."), from_u8(r.version)), NoticeKind::Info);
}

// --------------------------------------------------------------------------------- exports ----

void LayerlingPanel::Impl::on_export(const std::string &name, const fs::path &file)
{
    boost::system::error_code ec;
    const uintmax_t           size = fs::file_size(file, ec);
    if (ec || size == 0) {
        fs::remove_all(file.parent_path(), ec);
        show_notice(format_wxstr(_L("The export \"%1%\" is empty."), from_u8(name)), NoticeKind::Error);
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "Layerling: export \"" << name << "\" arrived (" << size << " bytes)";
    switch (layerling::classify_export(name)) {
    case layerling::ExportKind::Model: {
        if (size > untrusted::MODEL_DOWNLOAD_SIZE_LIMIT) {
            fs::remove_all(file.parent_path(), ec);
            show_notice(format_wxstr(_L("\"%1%\" is larger than the 500 MB limit for models."), from_u8(name)), NoticeKind::Error);
            return;
        }
        std::string head(512, '\0');
        {
            boost::nowide::ifstream in(file.string(), std::ios::binary);
            in.read(&head[0], std::streamsize(head.size()));
            head.resize(size_t(std::max<std::streamsize>(in.gcount(), 0)));
        }
        std::string why;
        if (!untrusted::content_matches_extension(name, head, size, &why)) {
            fs::remove_all(file.parent_path(), ec);
            show_notice(format_wxstr(_L("\"%1%\" was not opened: its content does not match its type (%2%)."), from_u8(name), from_u8(why)),
                        NoticeKind::Error);
            return;
        }
        // Keep it for "send again"; the one before goes.
        if (!last_export.empty() && last_export.parent_path() != file.parent_path())
            fs::remove_all(last_export.parent_path(), ec);
        last_export      = file;
        last_export_name = name;
        // The design it came from: the editor's address holds "?editor=1&project=<id>".
        last_export_link         = layerling::DesignLink();
        const std::string page   = browser ? browser->current_url() : std::string();
        last_export_link.origin  = layerling::origin_of(page);
        last_export_link.project = layerling::project_from_page_url(page);
        last_export_link.name    = layerling::strip_model_extension(name);
        if (!last_export_link.valid())
            last_export_link = layerling::DesignLink();
        send_to_plate(file, name, last_export_link);
        return;
    }
    case layerling::ExportKind::Project:
    case layerling::ExportKind::Other: {
        const fs::path folder = downloads_folder();
        const bool     project = layerling::classify_export(name) == layerling::ExportKind::Project;
        const wxString msg     = project ? format_wxstr(_L("\"%1%\" is a Layerling project file. Save it to your Downloads folder?"), from_u8(name)) :
                                           format_wxstr(_L("Save \"%1%\" to your Downloads folder?"), from_u8(name));
        MessageDialog dlg(wxGetTopLevelParent(owner), msg, _L("Layerling"), wxYES_NO | wxICON_QUESTION);
        dlg.SetButtonLabel(wxID_YES, _L("Save"));
        dlg.SetButtonLabel(wxID_NO, _L("Cancel"), true);
        if (folder.empty() || dlg.ShowModal() != wxID_YES) {
            fs::remove_all(file.parent_path(), ec);
            return;
        }
        std::string saved = name;
        fs::path    dest;
        if (!untrusted::place_download_file(file, folder, saved, dest, ec)) {
            fs::remove_all(file.parent_path(), ec);
            show_notice(format_wxstr(_L("\"%1%\" could not be saved to your Downloads folder."), from_u8(name)), NoticeKind::Error);
            return;
        }
        fs::remove_all(file.parent_path(), ec);
        show_notice(format_wxstr(_L("Saved \"%1%\" to your Downloads folder."), from_u8(saved)), NoticeKind::Info, _L("Show in folder"),
                    [dest]() { desktop_open_any_folderEx(dest.string()); });
        return;
    }
    }
}

void LayerlingPanel::Impl::send_to_plate(const fs::path &file_in, const std::string &name, const layerling::DesignLink &link)
{
    Plater    *plater = wxGetApp().plater();
    MainFrame *frame  = wxGetApp().mainframe;
    if (plater == nullptr || frame == nullptr || sending)
        return;
    boost::system::error_code ec;
    if (!fs::is_regular_file(file_in, ec)) {
        show_notice(format_wxstr(_L("\"%1%\" is no longer there. Export it from Layerling again."), from_u8(name)), NoticeKind::Warning);
        return;
    }
    // An object on the plate came from this design: replace it, or add the design as a new object?
    int                       replace_idx = -1;
    const std::vector<size_t> linked      = link.valid() ? linked_objects(link.origin, link.project) : std::vector<size_t>();
    if (!linked.empty()) {
        const ModelObject *old = plater->model().objects[linked.front()];
        MessageDialog      dlg(wxGetTopLevelParent(owner),
                               format_wxstr(_L("\"%1%\" on the plate came from this Layerling design.\n\nReplace it with the new version (it keeps its "
                                               "place, filament, settings and modifiers), or add the design as a new object?"),
                                            from_u8(old->name)),
                               _L("Send to Plate"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
        dlg.SetButtonLabel(wxID_YES, _L("Replace"), true);
        dlg.SetButtonLabel(wxID_NO, _L("Add as new object"));
        const int answer = dlg.ShowModal();
        if (answer == wxID_CANCEL)
            return;
        if (answer == wxID_YES)
            replace_idx = int(linked.front());
    } else if (!plater->model().objects.empty()) {
        MessageDialog dlg(wxGetTopLevelParent(owner),
                          format_wxstr(_L("Send \"%1%\" to the plate.\n\nAdd it to the current plate, or start a new project with it?"), from_u8(name)),
                          _L("Send to Plate"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
        dlg.SetButtonLabel(wxID_YES, _L("Add to current plate"), true);
        dlg.SetButtonLabel(wxID_NO, _L("New project"));
        const int answer = dlg.ShowModal();
        if (answer == wxID_CANCEL)
            return;
        if (answer == wxID_NO && plater->new_project() == wxID_CANCEL)
            return;
    }
    sending = true;
    // A STEP keeps its exact geometry only while its file is there: keep it (keep_step_file).
    const bool     step = boost::iends_with(name, ".step") || boost::iends_with(name, ".stp");
    const fs::path file = step ? keep_step_file(file_in, name) : file_in;
    std::vector<layerling::BodyColour> bodies;
    if (boost::iends_with(name, ".3mf")) {
        std::string err;
        if (!layerling::read_3mf_body_colours(file, bodies, &err))
            BOOST_LOG_TRIVIAL(warning) << "Layerling: colours not read from \"" << name << "\": " << err;
    }
    // Replacing keeps the old object's filament unless the design has several colours.
    const bool several_colours = layerling::distinct_colours(bodies).size() > 1;
    if (replace_idx >= 0 && !several_colours)
        bodies.clear();
    frame->select_tab(size_t(MainFrame::tp3DEditor));
    wxGetApp().mark_web_download(file);
    std::vector<size_t> added;
    bool                replaced = false;
    {
        Plater::TakeSnapshot snapshot(plater, replace_idx >= 0 ? "Replace from Layerling" : "Send to Plate from Layerling");
        // Geometry only: Layerling's 3MF carries no print settings, and the plate's stay as they are.
        // No "multi-part object?" question: the tab's setting answers it, for this load only (a
        // replacement is always one object).
        struct MultipartAnswer
        {
            Plater *plater;
            int     previous;
            MultipartAnswer(Plater *p, int answer) : plater(p), previous(p->multipart_answer()) { plater->set_multipart_answer(answer); }
            ~MultipartAnswer() { plater->set_multipart_answer(previous); }
        } answer(plater, replace_idx >= 0 || send_as_one_object() ? 1 : 0);
        added = plater->load_files(std::vector<fs::path>{file}, LoadStrategy::LoadModel);
        if (!added.empty() && !bodies.empty())
            apply_colours_to_objects(added, bodies);
        if (link.valid())
            link_objects(added, link);
        if (replace_idx >= 0 && added.size() == 1)
            replaced = replace_object(size_t(replace_idx), added.front(), link, several_colours);
    }
    sending = false;
    if (added.empty())
        BOOST_LOG_TRIVIAL(warning) << "Layerling: \"" << name << "\" did not load";
    else
        BOOST_LOG_TRIVIAL(info) << "Layerling: \"" << name << "\" sent to the plate, " << added.size() << " object(s)"
                                << (replaced ? ", replacing the object it came from" : "")
                                << (link.valid() ? ", design " + link.project : std::string());
    if (replace_idx >= 0 && !added.empty() && !replaced)
        show_notice(_L("The new version was added, but the old object could not be replaced: delete it yourself."), NoticeKind::Warning);
}

// One filament per colour: an existing filament of that colour, else a new one. Bodies are matched
// to the loaded objects in order (the 3MF loader keeps the file's object order), or to the parts of
// one object when the import merged them ("multi-part object" question).
void LayerlingPanel::Impl::apply_colours_to_objects(const std::vector<size_t> &objects, const std::vector<layerling::BodyColour> &bodies)
{
    Plater *plater = wxGetApp().plater();
    Model  &model  = plater->model();
    const std::vector<std::string> colours = layerling::distinct_colours(bodies);
    if (colours.empty())
        return;

    struct Target { size_t obj; int volume; std::string colour; };
    std::vector<Target> targets;
    if (objects.size() == bodies.size()) {
        for (size_t i = 0; i < objects.size(); ++i)
            targets.push_back({objects[i], -1, bodies[i].colour});
    } else if (objects.size() == 1 && objects[0] < model.objects.size()) {
        const ModelObject *o     = model.objects[objects[0]];
        std::vector<int>   parts;
        for (size_t v = 0; v < o->volumes.size(); ++v)
            if (o->volumes[v]->type() == ModelVolumeType::MODEL_PART)
                parts.push_back(int(v));
        if (parts.size() == bodies.size())
            for (size_t i = 0; i < parts.size(); ++i)
                targets.push_back({objects[0], parts[i], bodies[i].colour});
    }
    if (targets.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Layerling: " << bodies.size() << " coloured bodies, " << objects.size()
                                   << " loaded objects: colours not applied";
        show_notice(_L("The design is on the plate, but its colours could not be matched to the objects. Pick the filaments in the object list."),
                    NoticeKind::Warning);
        return;
    }

    const std::vector<std::string> existing = plater->get_extruder_colors_from_plater_config(nullptr, false);
    std::vector<std::string>       to_add;
    const std::vector<int>         ids =
        layerling::map_colours_to_filaments(colours, existing, std::min(MAXIMUM_EXTRUDER_NUMBER, MAXIMUM_FILAMENT_NUMBER), to_add);
    for (const std::string &c : to_add)
        wxGetApp().sidebar().add_custom_filament(wxColour(from_u8(c)));
    const size_t now_count = plater->get_extruder_colors_from_plater_config(nullptr, false).size();

    ObjectList          *list  = wxGetApp().obj_list();
    ObjectDataViewModel *vm    = list ? list->GetModel() : nullptr;
    for (const Target &t : targets) {
        if (t.obj >= model.objects.size() || t.colour.empty())
            continue;
        const auto it = std::find(colours.begin(), colours.end(), t.colour);
        int        id = it == colours.end() ? 1 : ids[size_t(it - colours.begin())];
        if (id < 1 || size_t(id) > now_count)
            id = 1; // a filament that could not be added
        ModelObject *o = model.objects[t.obj];
        if (t.volume < 0) {
            o->config.set_key_value("extruder", new ConfigOptionInt(id));
            for (ModelVolume *v : o->volumes)
                if (v->type() == ModelVolumeType::MODEL_PART && v->config.has("extruder"))
                    v->config.erase("extruder");
            if (vm)
                vm->SetExtruder(wxString::Format("%d", id), vm->GetItemById(int(t.obj)));
        } else if (size_t(t.volume) < o->volumes.size()) {
            o->volumes[size_t(t.volume)]->config.set_key_value("extruder", new ConfigOptionInt(id));
            if (vm)
                vm->SetExtruder(wxString::Format("%d", id), vm->GetItemByVolumeId(int(t.obj), t.volume));
        }
    }
    if (list)
        list->Refresh();
    plater->update();
    plater->notify_filament_usage_changed();
    if (!to_add.empty())
        show_notice(format_wxstr(_L("The design's colours were added as %1% new filament(s). Pick the material for each in the sidebar."),
                                 int(to_add.size())),
                    NoticeKind::Info);
}

void LayerlingPanel::Impl::on_send_button()
{
    const wxString how = _L("In Layerling, export the design (Export > 3MF keeps its colours; STL, OBJ and STEP work too). "
                            "EdgeSlicer takes the export and puts it on the plate.");
    boost::system::error_code ec;
    const bool has_last = !last_export.empty() && fs::is_regular_file(last_export, ec);
    wxMenu     menu;
    const int  again = wxWindow::NewControlId();
    const int  help  = wxWindow::NewControlId();
    const int  whole = wxWindow::NewControlId();
    if (has_last)
        menu.Append(again, format_wxstr(_L("Send \"%1%\" to the plate again"), from_u8(last_export_name)));
    menu.Append(help, _L("How to send a design"));
    const int edit = wxWindow::NewControlId();
    menu.Append(edit, _L("Edit the plate's selection in Layerling"),
                _L("Open the selected object or parts in Layerling: a design sent from Layerling opens again, anything else is "
                   "imported into a new design"))
        ->Enable(!selection_items().empty());
    menu.AppendSeparator();
    menu.AppendCheckItem(whole, _L("Send multi-body designs as one object"),
                         _L("On: a design of several bodies becomes one object with a part per body (each keeps its filament). "
                            "Off: each body becomes an object of its own."))
        ->Check(send_as_one_object());
    const int chosen = toolbar->GetPopupMenuSelectionFromUser(menu, btn_send->GetPosition() + wxPoint(0, btn_send->GetSize().y));
    if (chosen == again && has_last)
        send_to_plate(last_export, last_export_name, last_export_link);
    else if (chosen == edit)
        edit_selection();
    else if (chosen == help)
        show_notice(how, NoticeKind::Info);
    else if (chosen == whole)
        set_cfg(KEY_ONE_OBJECT, send_as_one_object() ? "0" : "1");
}

void LayerlingPanel::Impl::on_support_button()
{
    if (!funding_fetched)
        fetch_funding();
    wxMenu menu;
    std::map<int, std::string> urls;
    auto *title = menu.Append(wxID_ANY, format_wxstr(_L("Layerling is free and made by %1% - support the project:"), layerling::AUTHOR));
    title->Enable(false);
    for (const layerling::FundingLink &l : funding) {
        const int id = wxWindow::NewControlId();
        urls[id]     = l.url;
        menu.Append(id, from_u8(l.label));
    }
    const wxPoint where  = btn_support->GetPosition() + wxPoint(0, btn_support->GetSize().y);
    const int     chosen = toolbar->GetPopupMenuSelectionFromUser(menu, where);
    auto          it     = urls.find(chosen);
    if (it != urls.end() && untrusted::is_safe_to_open_externally(it->second))
        wxLaunchDefaultBrowser(from_u8(it->second));
}

void LayerlingPanel::Impl::on_about_button()
{
    wxMenu    menu;
    const int id_source  = wxWindow::NewControlId();
    const int id_licence = wxWindow::NewControlId();
    const int id_check   = wxWindow::NewControlId();
    const int id_reset   = wxWindow::NewControlId();
    const int id_source  = wxWindow::NewControlId();
    const int id_mcp     = wxWindow::NewControlId();
    const std::string v  = cfg(KEY_VERSION);
    auto *head = menu.Append(wxID_ANY, custom_base() ? format_wxstr(_L("Layerling at %1%"), from_u8(layerling::origin_of(cfg(KEY_BASE_URL)))) :
                                                       format_wxstr(_L("Layerling %1% by %2%"), from_u8(v), layerling::AUTHOR));
    head->Enable(false);
    auto *lic = menu.Append(wxID_ANY, _L("Licence: GNU AGPL-3.0. Not modified by EdgeSlicer."));
    lic->Enable(false);
    menu.AppendSeparator();
    menu.Append(id_source, _L("Source code on GitHub"));
    menu.Append(id_licence, _L("Licence text"));
    if (!custom_base())
        menu.Append(id_check, _L("Check for a newer version"));
    menu.AppendSeparator();
    menu.Append(id_source, _L("Layerling source..."), _L("The downloaded copy, or your own Layerling (a NAS, a home server, layerling.com)"));
    menu.Append(id_mcp, _L("Connect an AI client (MCP)..."), _L("The settings for an AI client that drives Layerling through its MCP server"));
    menu.AppendSeparator();
    auto *reset = menu.Append(id_reset, _L("Reset Layerling data..."));
    reset->Enable(browser != nullptr && browser->can_clear_data());
    const wxPoint where  = btn_about->GetPosition() + wxPoint(0, btn_about->GetSize().y);
    const int     chosen = toolbar->GetPopupMenuSelectionFromUser(menu, where);
    if (chosen == id_source)
        wxLaunchDefaultBrowser(layerling::REPO_URL);
    else if (chosen == id_licence)
        wxLaunchDefaultBrowser(layerling::LICENSE_URL);
    else if (chosen == id_check)
        check_for_update(true);
    else if (chosen == id_reset)
        reset_data();
    else if (chosen == id_source)
        choose_source();
    else if (chosen == id_mcp)
        show_mcp_setup();
}

void LayerlingPanel::Impl::reset_data()
{
    if (!browser || !browser->can_clear_data())
        return;
    MessageDialog dlg(wxGetTopLevelParent(owner),
                      _L("Delete everything Layerling stored in EdgeSlicer: all designs, custom shapes and settings?\n\n"
                         "This cannot be undone. Use \"Back up all\" in Layerling first if you want to keep your designs."),
                      _L("Reset Layerling data"), wxYES_NO | wxICON_WARNING);
    dlg.SetButtonLabel(wxID_YES, _L("Delete"));
    dlg.SetButtonLabel(wxID_NO, _L("Cancel"), true);
    if (dlg.ShowModal() != wxID_YES)
        return;
    auto alive_ = alive;
    browser->clear_data([this, alive_](bool ok) {
        if (!*alive_)
            return;
        BOOST_LOG_TRIVIAL(info) << "Layerling: data reset " << (ok ? "done" : "failed");
        if (ok) {
            show_notice(_L("Layerling's data was deleted."), NoticeKind::Info);
            if (browser)
                browser->reload();
        } else {
            show_notice(_L("Layerling's data could not be deleted."), NoticeKind::Error);
        }
    });
}

// ------------------------------------------------------------------------ Edit in Layerling ----

namespace {

// A UTF-8 path string (ModelVolume::source.input_file) as a path, and back.
fs::path u8_path(const std::string &s)
{
#ifdef _WIN32
    return fs::path(boost::nowide::widen(s));
#else
    return fs::path(s);
#endif
}
std::string path_u8(const fs::path &p)
{
#ifdef _WIN32
    return boost::nowide::narrow(p.wstring());
#else
    return p.string();
#endif
}

std::vector<EditItem> selection_items(bool *whole)
{
    std::vector<EditItem> out;
    if (whole)
        *whole = false;
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return out;
    const Selection &sel   = plater->get_selection();
    const Model     &model = plater->model();
    if (sel.is_empty() || sel.is_wipe_tower())
        return out;
    auto is_part = [&model](size_t o, int v) {
        return o < model.objects.size() && v >= 0 && size_t(v) < model.objects[o]->volumes.size() &&
               model.objects[o]->volumes[size_t(v)]->is_model_part();
    };
    if (sel.is_any_volume() || sel.is_any_modifier()) {
        std::map<size_t, EditItem> by_object;
        for (unsigned int idx : sel.get_volume_idxs()) {
            const GLVolume *gv = sel.get_volume(idx);
            if (gv == nullptr || gv->object_idx() < 0 || !is_part(size_t(gv->object_idx()), gv->volume_idx()))
                continue;
            EditItem &it = by_object[size_t(gv->object_idx())];
            it.object    = size_t(gv->object_idx());
            it.instance  = (std::max)(0, gv->instance_idx());
            if (std::find(it.volumes.begin(), it.volumes.end(), gv->volume_idx()) == it.volumes.end())
                it.volumes.push_back(gv->volume_idx());
        }
        for (auto &kv : by_object) {
            std::sort(kv.second.volumes.begin(), kv.second.volumes.end());
            out.push_back(kv.second);
        }
        return out;
    }
    for (const auto &[o, instances] : sel.get_content()) {
        if (o < 0 || size_t(o) >= model.objects.size())
            continue;
        EditItem it;
        it.object   = size_t(o);
        it.instance = instances.empty() ? 0 : *instances.begin();
        if (it.instance < 0 || size_t(it.instance) >= model.objects[size_t(o)]->instances.size())
            it.instance = 0;
        for (int v = 0; v < int(model.objects[size_t(o)]->volumes.size()); ++v)
            if (is_part(size_t(o), v))
                it.volumes.push_back(v);
        if (!it.volumes.empty())
            out.push_back(it);
    }
    if (whole)
        *whole = !out.empty();
    return out;
}

// The part can go to Layerling as an exact B-rep: a CAD body, or still the tessellation of the
// STEP file it was imported from (STEPExport's step_source_brep then finds it).
bool part_has_exact_geometry(const ModelVolume &v)
{
    if (BRep::attached_cad_body(v))
        return true;
    const std::string &f = v.source.input_file;
    if (f.empty() || !(boost::iends_with(f, ".step") || boost::iends_with(f, ".stp")))
        return false;
    boost::system::error_code ec;
    return fs::is_regular_file(u8_path(f), ec);
}

std::string part_colour(const ModelVolume &v, const std::vector<std::string> &colours)
{
    const int e = v.extruder_id();
    if (e >= 1 && size_t(e) <= colours.size())
        return colours[size_t(e) - 1];
    return colours.empty() ? std::string() : colours.front();
}

TriangleMesh world_mesh(const ModelObject &o, int instance, const ModelVolume &v)
{
    TriangleMesh m = v.mesh();
    m.transform(o.instances[size_t(instance)]->get_matrix() * v.get_matrix(), true);
    return m;
}

bool read_file(const fs::path &file, std::string &out)
{
    boost::nowide::ifstream in(path_u8(file), std::ios::binary);
    if (!in)
        return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

bool layerling_can_edit_selection() { return layerling_tab_enabled() && !selection_items().empty(); }

void layerling_edit_selection()
{
    MainFrame *frame = wxGetApp().mainframe;
    if (frame == nullptr)
        return;
    if (!layerling_tab_enabled()) {
        MessageDialog dlg(frame, _L("Turn on Preferences > General > \"Show the Layerling tab\" to edit models in Layerling."), _L("Layerling"),
                          wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }
    if (LayerlingPanel *panel = frame->layerling_panel())
        panel->edit_selection();
}

void LayerlingPanel::edit_selection() { p->edit_selection(); }

void LayerlingPanel::Impl::edit_selection()
{
    Plater    *plater = wxGetApp().plater();
    MainFrame *frame  = wxGetApp().mainframe;
    if (plater == nullptr || frame == nullptr)
        return;
    bool                        whole = false;
    const std::vector<EditItem> items = selection_items(&whole);
    if (items.empty())
        return;
    if (!can_show()) {
        frame->select_tab(owner);
        get_status->SetLabel(_L("Download Layerling (or choose your own Layerling below), then use \"Edit in Layerling\" again."));
        get_status->Wrap(owner->FromDIP(560));
        get_page->Layout();
        return;
    }
    Model &model = plater->model();

    // An object that came from a design of this origin: open that design, nothing is exported.
    const std::string origin = current_origin();
    if (whole && items.size() == 1) {
        layerling::DesignLink link;
        if (layerling::parse_link(model.objects[items[0].object]->layerling_design, link)) {
            if (link.origin == origin) {
                open_design(link);
                return;
            }
            BOOST_LOG_TRIVIAL(info) << "Layerling: the object's design is at " << link.origin << ", the tab shows " << origin << ": exporting it";
        }
    }

    layerling::EditSource          src;
    const std::vector<std::string> colours = plater->get_extruder_colors_from_plater_config(nullptr, false);
    std::vector<std::string>       used;
    src.all_exact = true;
    for (const EditItem &it : items)
        for (int v : it.volumes) {
            const ModelVolume &vol = *model.objects[it.object]->volumes[size_t(v)];
            src.all_exact          = src.all_exact && part_has_exact_geometry(vol);
            const std::string c    = part_colour(vol, colours);
            if (!c.empty() && std::find(used.begin(), used.end(), c) == used.end())
                used.push_back(c);
        }
    src.colours                        = used.size();
    const layerling::EditFormat format = layerling::choose_edit_format(src);

    // The file: named after the object (or the part), in a folder of its own.
    const ModelObject &first = *model.objects[items[0].object];
    std::string        stem  = items.size() > 1 ? std::string("EdgeSlicer selection") :
                               (!whole && items[0].volumes.size() == 1) ? first.volumes[size_t(items[0].volumes[0])]->name :
                                                                          first.name;
    if (stem.empty())
        stem = "EdgeSlicer model";
    stem                   = layerling::strip_model_extension(layerling::sanitize_export_name(stem));
    const std::string name = stem + layerling::edit_format_extension(format);
    boost::system::error_code ec;
    const fs::path            unique = fs::unique_path("%%%%%%%%", ec);
    const fs::path            dir    = temp_export_dir() / ("edit-" + unique.string());
    if (!ec)
        fs::create_directories(dir, ec);
    const fs::path file  = dir / u8_path(name);
    std::string    error = ec ? std::string("no usable temporary folder") : std::string();
    bool           ok    = !ec;
    if (ok && format == layerling::EditFormat::Step) {
        Model tmp;
        for (const EditItem &it : items) {
            ModelObject *copy = tmp.add_object(*model.objects[it.object]);
            for (int v = int(copy->volumes.size()) - 1; v >= 0; --v)
                if (std::find(it.volumes.begin(), it.volumes.end(), v) == it.volumes.end())
                    copy->delete_volume(size_t(v));
            for (int i = int(copy->instances.size()) - 1; i >= 0; --i)
                if (i != it.instance)
                    copy->delete_instance(size_t(i));
        }
        StepExportParams params;
        params.extruder_colours = colours;
        params.product_name     = stem;
        StepExportReport report;
        ok = store_step(file.string(), tmp, params, report);
        if (!ok)
            error = report.error;
        else
            BOOST_LOG_TRIVIAL(info) << "Layerling: STEP for Layerling, " << report.exact_parts << " exact part(s), " << report.mesh_parts
                                    << " from the mesh";
    } else if (ok && format == layerling::EditFormat::ThreeMF) {
        std::vector<layerling::ColouredMesh> bodies;
        for (const EditItem &it : items) {
            const ModelObject &o = *model.objects[it.object];
            for (int v : it.volumes) {
                const ModelVolume      &vol = *o.volumes[size_t(v)];
                const TriangleMesh      m   = world_mesh(o, it.instance, vol);
                layerling::ColouredMesh b;
                b.name   = vol.name.empty() ? o.name : vol.name;
                b.colour = part_colour(vol, colours);
                for (const stl_vertex &p : m.its.vertices)
                    b.vertices.insert(b.vertices.end(), {p.x(), p.y(), p.z()});
                for (const stl_triangle_vertex_indices &t : m.its.indices)
                    b.triangles.insert(b.triangles.end(), {t[0], t[1], t[2]});
                if (!b.triangles.empty())
                    bodies.push_back(std::move(b));
            }
        }
        ok = layerling::write_colour_3mf(file, bodies, &error);
    } else if (ok) {
        TriangleMesh all;
        for (const EditItem &it : items) {
            const ModelObject &o = *model.objects[it.object];
            for (int v : it.volumes)
                all.merge(world_mesh(o, it.instance, *o.volumes[size_t(v)]));
        }
        ok = !all.empty() && its_write_stl_binary(path_u8(file).c_str(), stem.c_str(), all.its);
        if (!ok)
            error = "the STL could not be written";
    }
    std::string bytes;
    if (ok && !read_file(file, bytes)) {
        ok    = false;
        error = "the file could not be read back";
    }
    if (!ok) {
        BOOST_LOG_TRIVIAL(error) << "Layerling: Edit in Layerling, export failed: " << error;
        frame->select_tab(owner);
        show_notice(format_wxstr(_L("The model could not be prepared for Layerling: %1%"), from_u8(error)), NoticeKind::Error);
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "Layerling: Edit in Layerling, \"" << name << "\" (" << bytes.size() << " bytes)";
    // The design the import makes is linked to the object when one whole object goes.
    const ObjectID link_to = whole && items.size() == 1 ? model.objects[items[0].object]->id() : ObjectID();
    start_handover(name, std::move(bytes), file, link_to);
}

void LayerlingPanel::Impl::open_design(const layerling::DesignLink &link)
{
    wxGetApp().mainframe->select_tab(owner);
    if (!browser)
        refresh();
    const std::string url = layerling::editor_url(base_url(), link.project);
    if (!browser || url.empty())
        return;
    BOOST_LOG_TRIVIAL(info) << "Layerling: opening the linked design " << link.project;
    loaded_url = url;
    browser->load(url);
    show_notice(format_wxstr(_L("Opened the design \"%1%\" in Layerling. Change it, export it, and Send to Plate offers to replace the "
                                "object. (If the design was deleted in Layerling, its start page shows instead.)"),
                             from_u8(link.name.empty() ? link.project : link.name)),
                NoticeKind::Info);
}

void LayerlingPanel::Impl::start_handover(const std::string &name, std::string bytes, const fs::path &file, ObjectID link_object)
{
    stop_handover();
    wxGetApp().mainframe->select_tab(owner);
    if (!browser)
        refresh();
    handover_name   = name;
    handover_file   = file;
    handover_object = link_object;
    handover_origin = current_origin();
    handover_before = browser ? layerling::project_from_page_url(browser->current_url()) : std::string();
    handover_ticks  = 0;
    handover_sent   = false;
    probing         = false;
    if (bytes.size() > layerling::MAX_HANDOVER_BYTES) {
        handover_failed(format_wxstr(_L("it is larger than %1%"), size_mb(layerling::MAX_HANDOVER_BYTES)));
        return;
    }
    handover_bytes = std::move(bytes);
    show_notice(format_wxstr(_L("Handing \"%1%\" to Layerling..."), from_u8(name)), NoticeKind::Info, wxString(), nullptr, true);
    handover_timer.Start(500);
}

// Every 500 ms: wait for Layerling's import input, hand the file over once, then wait for the new
// design to open (its project id in the page address) and link the object to it.
void LayerlingPanel::Impl::handover_tick()
{
    ++handover_ticks;
    if (!browser) {
        if (handover_ticks > 60)
            handover_failed(_L("Layerling did not start"));
        return;
    }
    if (!handover_sent) {
        if (probing)
            return;
        if (handover_ticks > 60) {
            handover_failed(_L("Layerling's import did not appear in time"));
            return;
        }
        probing     = true;
        auto alive_ = alive;
        browser->run_script(layerling::import_probe_script(), [this, alive_](bool ok, const std::string &result) {
            if (!*alive_)
                return;
            probing = false;
            if (!ok || result != layerling::SCRIPT_READY || handover_sent || handover_bytes.empty() || !browser)
                return;
            probing = true;
            browser->run_script(layerling::import_file_script(handover_name, handover_bytes), [this, alive_](bool ok2, const std::string &r2) {
                if (!*alive_)
                    return;
                probing = false;
                if (ok2 && r2 == layerling::SCRIPT_OK) {
                    BOOST_LOG_TRIVIAL(info) << "Layerling: \"" << handover_name << "\" handed to Layerling's import";
                    handover_sent  = true;
                    handover_ticks = 0;
                    handover_bytes.clear();
                    handover_bytes.shrink_to_fit();
                    show_notice(format_wxstr(_L("\"%1%\" was imported into a new Layerling design. Change it there, export it, and Send to "
                                                "Plate brings it back."),
                                             from_u8(handover_name)),
                                NoticeKind::Info);
                    if (!handover_object.valid())
                        stop_handover();
                } else {
                    BOOST_LOG_TRIVIAL(warning) << "Layerling: hand-over failed: " << r2;
                    handover_failed(r2 == layerling::SCRIPT_NO_INPUT ? _L("Layerling's import was not found") : from_u8(r2));
                }
            });
        });
        return;
    }
    // Handed over: Layerling opens the new design and shows "?editor=1&project=<id>".
    const std::string url     = browser->current_url();
    const std::string project = layerling::project_from_page_url(url);
    if (!project.empty() && project != handover_before && layerling::origin_of(url) == handover_origin) {
        Plater *plater = wxGetApp().plater();
        if (plater != nullptr) {
            for (size_t i = 0; i < plater->model().objects.size(); ++i)
                if (plater->model().objects[i]->id() == handover_object) {
                    layerling::DesignLink link;
                    link.origin  = handover_origin;
                    link.project = project;
                    link.name    = layerling::strip_model_extension(handover_name);
                    link_objects({i}, link);
                    BOOST_LOG_TRIVIAL(info) << "Layerling: object " << i << " linked to the design " << project;
                    break;
                }
        }
        stop_handover();
        return;
    }
    if (handover_ticks > 60)
        stop_handover(); // no design id seen: Send to Plate adds the design as a new object
}

void LayerlingPanel::Impl::handover_failed(const wxString &why)
{
    const fs::path file = handover_file;
    stop_handover();
    BOOST_LOG_TRIVIAL(warning) << "Layerling: the file was not handed over: " << into_u8(why);
    show_notice(format_wxstr(_L("\"%1%\" could not be handed to Layerling (%2%). Import it in Layerling yourself: on its start page choose "
                                "Import and pick the file from this folder."),
                             from_u8(file.filename().string()), why),
                NoticeKind::Warning, _L("Show in folder"), [file]() { desktop_open_any_folderEx(file.string()); }, true);
}

void LayerlingPanel::Impl::stop_handover()
{
    handover_timer.Stop();
    handover_bytes.clear();
    handover_bytes.shrink_to_fit();
    handover_object = ObjectID();
    handover_sent   = false;
    probing         = false;
}

std::vector<size_t> LayerlingPanel::Impl::linked_objects(const std::string &origin, const std::string &project) const
{
    std::vector<size_t> out;
    Plater             *plater = wxGetApp().plater();
    if (plater == nullptr || project.empty())
        return out;
    const Model &model = plater->model();
    for (size_t i = 0; i < model.objects.size(); ++i) {
        layerling::DesignLink l;
        if (layerling::parse_link(model.objects[i]->layerling_design, l) && l.origin == origin && l.project == project)
            out.push_back(i);
    }
    return out;
}

void LayerlingPanel::Impl::link_objects(const std::vector<size_t> &objects, const layerling::DesignLink &link)
{
    Plater           *plater = wxGetApp().plater();
    const std::string text   = layerling::serialize_link(link);
    if (plater == nullptr || text.empty())
        return;
    bool changed = false;
    for (size_t i : objects)
        if (i < plater->model().objects.size() && plater->model().objects[i]->layerling_design != text) {
            plater->model().objects[i]->layerling_design = text;
            changed                                      = true;
        }
    if (changed)
        plater->set_plater_dirty(true);
}

// Send to Plate's "Replace": the freshly loaded design takes the old object's place. It keeps the
// old object's placement (every instance: position, rotation, scale), its settings and filament
// (the parts keep the design's colours when it has several), its modifiers, negative volumes and
// support blockers/enforcers (relative to the object's centre: they may need a nudge when the
// design changed a lot), its name and its link. Then the old object goes.
bool LayerlingPanel::Impl::replace_object(size_t old_idx, size_t new_idx, const layerling::DesignLink &link, bool keep_colours)
{
    Plater     *plater = wxGetApp().plater();
    Model      &model  = plater->model();
    ObjectList *list   = wxGetApp().obj_list();
    if (old_idx >= model.objects.size() || new_idx >= model.objects.size() || old_idx == new_idx || list == nullptr)
        return false;
    ModelObject *oldo = model.objects[old_idx];
    ModelObject *newo = model.objects[new_idx];

    newo->center_around_origin(false);
    newo->instances[0]->set_transformation(oldo->instances[0]->get_transformation());
    for (size_t i = 1; i < oldo->instances.size(); ++i)
        newo->add_instance(*oldo->instances[i]);
    // Settings: the old object's; each part keeps its own colour when the design has several.
    newo->config.assign_config(oldo->config);
    if (!keep_colours)
        for (ModelVolume *v : newo->volumes)
            if (v->config.has("extruder"))
                v->config.erase("extruder");
    for (const ModelVolume *v : oldo->volumes)
        if (!v->is_model_part())
            newo->add_volume(*v);
    newo->layer_config_ranges = oldo->layer_config_ranges;
    newo->printable           = oldo->printable;
    newo->name                = oldo->name;
    newo->layerling_design    = layerling::serialize_link(link);
    newo->sort_volumes(true);
    newo->invalidate_bounding_box();
    newo->ensure_on_bed();

    // The object list shows the new object again (now with the modifiers), then the old one goes.
    list->delete_object_from_list(new_idx);
    list->add_object_to_list(new_idx);
    list->delete_from_model_and_list(ItemType::itObject, int(old_idx), -1);
    plater->update();
    BOOST_LOG_TRIVIAL(info) << "Layerling: replaced object " << old_idx << " with the design " << link.project;
    return true;
}

// A STEP that goes onto the plate is kept in <data dir>/layerling/plate/: the parts remember their
// source file, and while it is there they keep their exact CAD geometry (STEP export, CAD fillet,
// Edit in Layerling as STEP). The temporary export folder is gone after a restart.
fs::path LayerlingPanel::Impl::keep_step_file(const fs::path &file, const std::string &name)
{
    std::string bytes;
    if (!read_file(file, bytes))
        return file;
    layerling::Sha256 sha;
    sha.update(bytes.data(), bytes.size());
    const std::string         digest = sha.hex_digest().substr(0, 12);
    const fs::path            dir    = layerling_dir() / "plate";
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path n    = u8_path(layerling::sanitize_export_name(name));
    const fs::path dest = dir / (n.stem().wstring() + L"-" + fs::path(digest).wstring() + n.extension().wstring());
    if (!fs::exists(dest, ec))
        fs::copy_file(file, dest, ec);
    if (ec || !fs::is_regular_file(dest, ec)) {
        BOOST_LOG_TRIVIAL(warning) << "Layerling: the STEP could not be kept in " << dir.string() << ": " << ec.message();
        return file;
    }
    return dest;
}

// ------------------------------------------------------------------ Use my own Layerling ----

namespace {

// Is there a Layerling at `base` ("scheme://host[:port]/")? Its /version.json, then its start page.
// `done` runs on the GUI thread.
void probe_layerling(const std::string &base, long timeout, std::function<void(const layerling::ProbeResult &)> done)
{
    struct State
    {
        unsigned    vstatus = 0;
        std::string vbody;
    };
    auto st   = std::make_shared<State>();
    auto page = [base, timeout, st, done]() {
        Http::get(base)
            .timeout_connect(timeout)
            .timeout_max(timeout * 2)
            .size_limit(2 * 1024 * 1024)
            .on_complete([st, done](std::string body, unsigned status) {
                const layerling::ProbeResult r = layerling::classify_probe(st->vstatus, st->vbody, status, body);
                wxGetApp().CallAfter([done, r]() { done(r); });
            })
            .on_error([st, done](std::string body, std::string, unsigned status) {
                const layerling::ProbeResult r = layerling::classify_probe(st->vstatus, st->vbody, status, body);
                wxGetApp().CallAfter([done, r]() { done(r); });
            })
            .perform();
    };
    Http::get(base + "version.json")
        .timeout_connect(timeout)
        .timeout_max(timeout * 2)
        .size_limit(256 * 1024)
        .on_complete([st, page, done](std::string body, unsigned status) {
            st->vstatus                    = status;
            st->vbody                      = body;
            const layerling::ProbeResult r = layerling::classify_probe(status, body, 0, std::string());
            if (r.ok)
                wxGetApp().CallAfter([done, r]() { done(r); });
            else
                page();
        })
        .on_error([st, page](std::string, std::string, unsigned status) {
            st->vstatus = status;
            page();
        })
        .perform();
}

class LayerlingSourceDialog : public wxDialog
{
public:
    LayerlingSourceDialog(wxWindow *parent, bool custom, const std::string &url)
        : wxDialog(parent, wxID_ANY, _L("Layerling source"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        const int w = FromDIP(520);
        m_builtin   = new wxRadioButton(this, wxID_ANY, _L("The downloaded copy (runs on this computer, also offline)"), wxDefaultPosition,
                                        wxDefaultSize, wxRB_GROUP);
        m_custom    = new wxRadioButton(this, wxID_ANY, _L("My own Layerling at this address:"));
        m_url       = new wxTextCtrl(this, wxID_ANY, from_u8(url), wxDefaultPosition, wxSize(w, -1));
        m_url->SetHint("http://nas.local:3000");
        auto *check   = new wxButton(this, wxID_ANY, _L("Check"));
        auto *site    = new wxButton(this, wxID_ANY, "layerling.com");
        auto *find    = new wxButton(this, wxID_ANY, _L("Find on this computer"));
        m_status      = new wxStaticText(this, wxID_ANY, wxEmptyString);
        auto *storage = new wxStaticText(this, wxID_ANY,
                                         _L("Layerling keeps designs in the browser storage of the address it runs at: every address has its own "
                                            "designs, and designs made in the downloaded copy do not show at another address (use \"Back up all\" "
                                            "and Restore in Layerling to move them). A self-hosted Layerling server with a shared project folder "
                                            "keeps designs in that folder instead, the same on every device.\n\n"
                                            "Layerling needs an address of its own (not a subfolder). Its Docker image and dev server use port "
                                            "3000. Your own Layerling is yours to update: EdgeSlicer does not download or update it."));
        storage->Wrap(w);
        m_builtin->SetValue(!custom);
        m_custom->SetValue(custom);

        auto *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(check, 0, wxRIGHT, FromDIP(6));
        row->Add(site, 0, wxRIGHT, FromDIP(6));
        row->Add(find, 0);
        auto *col = new wxBoxSizer(wxVERTICAL);
        col->Add(m_builtin, 0, wxBOTTOM, FromDIP(10));
        col->Add(m_custom, 0, wxBOTTOM, FromDIP(4));
        col->Add(m_url, 0, wxEXPAND | wxLEFT | wxBOTTOM, FromDIP(20));
        col->Add(row, 0, wxLEFT | wxBOTTOM, FromDIP(20));
        col->Add(m_status, 0, wxEXPAND | wxLEFT | wxBOTTOM, FromDIP(20));
        col->Add(storage, 0, wxBOTTOM, FromDIP(12));
        col->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND);
        auto *outer = new wxBoxSizer(wxVERTICAL);
        outer->Add(col, 1, wxEXPAND | wxALL, FromDIP(16));
        SetSizerAndFit(outer);
        CentreOnParent();

        check->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { check_url(false); });
        site->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            m_url->ChangeValue("https://layerling.com/");
            m_custom->SetValue(true);
            m_checked.clear();
            check_url(false);
        });
        find->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { find_local(); });
        m_url->Bind(wxEVT_TEXT, [this](wxCommandEvent &) {
            m_checked.clear();
            if (!m_url->GetValue().empty())
                m_custom->SetValue(true);
        });
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_ok(); }, wxID_OK);
    }
    ~LayerlingSourceDialog() override { *m_alive = false; }

    bool        custom() const { return m_custom->GetValue(); }
    std::string url() const { return m_result; }

private:
    wxRadioButton        *m_builtin;
    wxRadioButton        *m_custom;
    wxTextCtrl           *m_url;
    wxStaticText         *m_status;
    std::shared_ptr<bool> m_alive { std::make_shared<bool>(true) };
    std::string           m_checked; // the address the last check found Layerling at
    std::string           m_result;
    int                   m_pending { 0 };

    void set_status(const wxString &text)
    {
        m_status->SetLabel(text);
        m_status->Wrap(FromDIP(500));
        Layout();
        Fit();
    }

    void check_url(bool then_close)
    {
        const layerling::CustomUrl u = layerling::normalize_custom_url(into_u8(m_url->GetValue()));
        if (!u.ok) {
            set_status(format_wxstr(_L("Not usable: %1%."), from_u8(u.error)));
            return;
        }
        if (from_u8(u.url) != m_url->GetValue())
            m_url->ChangeValue(from_u8(u.url));
        set_status(u.path_dropped ? format_wxstr(_L("Layerling runs at the root of an address: checking %1% ..."), from_u8(u.url)) :
                                    format_wxstr(_L("Checking %1% ..."), from_u8(u.url)));
        auto              alive_ = m_alive;
        const std::string url    = u.url;
        probe_layerling(url, 5, [this, alive_, url, then_close](const layerling::ProbeResult &r) {
            if (!*alive_)
                return;
            if (r.ok) {
                m_checked = url;
                set_status(r.version.empty() ? format_wxstr(_L("Layerling answers at %1%."), from_u8(url)) :
                                               format_wxstr(_L("Layerling %1% answers at %2%."), from_u8(r.version), from_u8(url)));
                if (then_close) {
                    m_result = url;
                    EndModal(wxID_OK);
                }
                return;
            }
            set_status(format_wxstr(_L("No Layerling found at %1%: %2%."), from_u8(url), from_u8(r.error)));
            if (then_close) {
                MessageDialog dlg(this, format_wxstr(_L("No Layerling answered at %1% (%2%).\n\nUse this address anyway?"), from_u8(url), from_u8(r.error)),
                                  _L("Layerling source"), wxYES_NO | wxICON_QUESTION);
                if (dlg.ShowModal() == wxID_YES) {
                    m_result = url;
                    EndModal(wxID_OK);
                }
            }
        });
    }

    // Only the usual local addresses (layerling::local_candidates), never the network.
    void find_local()
    {
        if (m_pending > 0)
            return;
        const std::vector<std::string> candidates = layerling::local_candidates();
        set_status(_L("Looking for a Layerling on this computer..."));
        m_pending   = int(candidates.size());
        auto alive_ = m_alive;
        auto found  = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
        for (const std::string &c : candidates)
            probe_layerling(c, 2, [this, alive_, c, found, candidates](const layerling::ProbeResult &r) {
                if (!*alive_)
                    return;
                if (r.ok)
                    found->push_back({c, r.version});
                if (--m_pending > 0)
                    return;
                if (found->empty()) {
                    set_status(_L("No Layerling answers on this computer (tried ports 3000, 3100 and 8080)."));
                    return;
                }
                // In the order of the candidates, not of the answers.
                std::string first;
                wxString    list;
                for (const std::string &cand : candidates)
                    for (const auto &f : *found)
                        if (f.first == cand) {
                            if (first.empty())
                                first = cand;
                            list += "\n" + from_u8(cand) + (f.second.empty() ? wxString() : " (" + from_u8(f.second) + ")");
                        }
                m_url->ChangeValue(from_u8(first));
                m_custom->SetValue(true);
                m_checked = first;
                set_status(_L("Found:") + list);
            });
    }

    void on_ok()
    {
        if (!m_custom->GetValue()) {
            EndModal(wxID_OK);
            return;
        }
        const layerling::CustomUrl u = layerling::normalize_custom_url(into_u8(m_url->GetValue()));
        if (!u.ok) {
            set_status(format_wxstr(_L("Not usable: %1%."), from_u8(u.error)));
            return;
        }
        if (u.url == m_checked) {
            m_result = u.url;
            EndModal(wxID_OK);
            return;
        }
        check_url(true); // closes when Layerling answers (or the user takes the address anyway)
    }
};

} // namespace

void LayerlingPanel::Impl::choose_source()
{
    LayerlingSourceDialog dlg(wxGetTopLevelParent(owner), custom_base(), cfg(KEY_BASE_URL));
    if (dlg.ShowModal() != wxID_OK)
        return;
    const bool        was_custom = custom_base();
    const std::string was_url    = cfg(KEY_BASE_URL);
    if (dlg.custom() && !dlg.url().empty()) {
        set_cfg(KEY_BASE_URL, dlg.url());
        set_cfg(KEY_SOURCE, layerling::source_key(layerling::Source::Custom));
    } else if (!dlg.custom()) {
        set_cfg(KEY_SOURCE, layerling::source_key(layerling::Source::BuiltIn));
    }
    if (was_custom != custom_base() || (custom_base() && was_url != cfg(KEY_BASE_URL))) {
        BOOST_LOG_TRIVIAL(info) << "Layerling: the source is now " << (custom_base() ? cfg(KEY_BASE_URL) : std::string("the downloaded copy"));
        apply_source();
    }
}

void LayerlingPanel::Impl::apply_source()
{
    stop_handover();
    loaded_url.clear();
    btn_update->Hide();
    if (custom_base())
        server.stop();
    refresh();
    if (custom_base())
        show_notice(format_wxstr(_L("Layerling now runs from %1%. Its designs are kept by that address; the designs of the downloaded copy "
                                    "are still there when you switch back."),
                                 from_u8(layerling::origin_of(cfg(KEY_BASE_URL)))),
                    NoticeKind::Info);
}

// ------------------------------------------------------------------------------------ MCP ----

namespace {

class LayerlingMcpDialog : public wxDialog
{
public:
    LayerlingMcpDialog(wxWindow *parent, const std::string &origin, bool custom)
        : wxDialog(parent, wxID_ANY, _L("Connect an AI client to Layerling (MCP)"), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_origin(custom && !origin.empty() ? origin : std::string(layerling::MCP_DEFAULT_URL))
    {
        const int      w     = FromDIP(600);
        const wxString intro =
            custom ? format_wxstr(_L("Layerling's MCP server lets an AI client (Claude, Codex, Gemini) read and build the design in an open "
                                     "Layerling editor. It talks to the Layerling server at %1%, which must have it switched on: a dev server "
                                     "(npm run dev) has it; a Docker or NAS copy needs LAYERLING_MCP_REMOTE=true and LAYERLING_MCP_TOKEN on the "
                                     "server. layerling.com and static copies have no MCP bridge.\n\n"
                                     "The MCP server is a script in Layerling's source (scripts/layerling-mcp-server.mjs) and needs Node.js: "
                                     "clone Layerling and pick that folder below. Then add the text below to your AI client's settings "
                                     "(EdgeSlicer does not change other programs' settings) and keep an editor of %1% open, here or in a "
                                     "browser."),
                                  from_u8(m_origin)) :
                     format_wxstr(_L("Layerling's MCP server lets an AI client (Claude, Codex, Gemini) read and build the design in an open "
                                     "Layerling editor. It needs a Layerling server: the downloaded copy in this tab is Layerling's static web "
                                     "build, which has no MCP bridge.\n\n"
                                     "To use it: clone Layerling, run its dev server (npm run dev, at %1%), point this tab at it (Layerling "
                                     "source > My own Layerling) or open it in a browser, and pick the clone below. Then add the text below "
                                     "to your AI client's settings (EdgeSlicer does not change other programs' settings)."),
                                  from_u8(m_origin));
        auto *text = new wxStaticText(this, wxID_ANY, intro);
        text->Wrap(w);
        m_bridge = new wxStaticText(this, wxID_ANY, format_wxstr(_L("Checking %1%%2% ..."), from_u8(m_origin), layerling::MCP_ROUTE));

        auto *folder_label = new wxStaticText(this, wxID_ANY, _L("Layerling source folder (a clone of its repository):"));
        m_folder           = new wxTextCtrl(this, wxID_ANY, from_u8(cfg(KEY_MCP_CHECKOUT)));
        auto         *browse = new wxButton(this, wxID_ANY, _L("Browse..."));
        wxArrayString clients;
        clients.Add(_L("Claude Desktop, Claude Code (.mcp.json), Gemini CLI, Antigravity (JSON)"));
        clients.Add(_L("Codex (config.toml)"));
        m_client = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, clients);
        m_client->SetSelection(0);
        m_config = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(w, FromDIP(200)),
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        m_config->SetFont(wxFont(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE)));
        auto *copy  = new wxButton(this, wxID_ANY, _L("Copy to clipboard"));
        auto *close = new wxButton(this, wxID_CANCEL, _L("Close"));
        m_copied    = new wxStaticText(this, wxID_ANY, wxEmptyString);

        auto *frow = new wxBoxSizer(wxHORIZONTAL);
        frow->Add(m_folder, 1, wxRIGHT, FromDIP(6));
        frow->Add(browse, 0);
        auto *brow = new wxBoxSizer(wxHORIZONTAL);
        brow->Add(copy, 0, wxRIGHT, FromDIP(8));
        brow->Add(m_copied, 1, wxALIGN_CENTER_VERTICAL);
        brow->Add(close, 0);
        auto *col = new wxBoxSizer(wxVERTICAL);
        col->Add(text, 0, wxBOTTOM, FromDIP(10));
        col->Add(m_bridge, 0, wxBOTTOM, FromDIP(12));
        col->Add(folder_label, 0, wxBOTTOM, FromDIP(4));
        col->Add(frow, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
        col->Add(m_client, 0, wxBOTTOM, FromDIP(6));
        col->Add(m_config, 1, wxEXPAND | wxBOTTOM, FromDIP(10));
        col->Add(brow, 0, wxEXPAND);
        auto *outer = new wxBoxSizer(wxVERTICAL);
        outer->Add(col, 1, wxEXPAND | wxALL, FromDIP(16));
        SetSizerAndFit(outer);
        CentreOnParent();

        browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            wxDirDialog dlg(this, _L("Layerling source folder"), m_folder->GetValue());
            if (dlg.ShowModal() == wxID_OK)
                m_folder->SetValue(dlg.GetPath());
        });
        m_folder->Bind(wxEVT_TEXT, [this](wxCommandEvent &) {
            set_cfg(KEY_MCP_CHECKOUT, into_u8(m_folder->GetValue()));
            update_text();
        });
        m_client->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { update_text(); });
        copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            if (wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(m_config->GetValue()));
                wxTheClipboard->Close();
                m_copied->SetLabel(_L("Copied."));
            }
        });
        update_text();
        probe_bridge();
    }
    ~LayerlingMcpDialog() override { *m_alive = false; }

private:
    std::string           m_origin;
    wxStaticText         *m_bridge;
    wxTextCtrl           *m_folder;
    wxChoice             *m_client;
    wxTextCtrl           *m_config;
    wxStaticText         *m_copied;
    layerling::McpBridge  m_state { layerling::McpBridge::Unknown };
    std::shared_ptr<bool> m_alive { std::make_shared<bool>(true) };

    void update_text()
    {
        const std::string script = layerling::mcp_script_in_checkout(into_u8(m_folder->GetValue()));
        const auto        client = m_client->GetSelection() == 1 ? layerling::McpClient::Codex : layerling::McpClient::Json;
        m_config->SetValue(from_u8(layerling::mcp_config_text(client, m_origin, script, layerling::mcp_needs_token(m_origin, m_state))));
        m_copied->SetLabel(wxEmptyString);
    }

    // Only the address the user chose (or the dev server's on this computer).
    void probe_bridge()
    {
        auto              alive_ = m_alive;
        const std::string url    = m_origin + layerling::MCP_ROUTE;
        auto              done   = [this, alive_](unsigned status) {
            wxGetApp().CallAfter([this, alive_, status]() {
                if (!*alive_)
                    return;
                m_state = layerling::mcp_bridge_from_status(status);
                switch (m_state) {
                case layerling::McpBridge::Open:
                    m_bridge->SetLabel(format_wxstr(_L("The MCP bridge of %1% is open."), from_u8(m_origin)));
                    break;
                case layerling::McpBridge::NeedsToken:
                    m_bridge->SetLabel(format_wxstr(_L("The MCP bridge of %1% wants its token (LAYERLING_MCP_TOKEN): put it into the text below."),
                                                    from_u8(m_origin)));
                    break;
                case layerling::McpBridge::Off:
                    m_bridge->SetLabel(format_wxstr(_L("%1% has no MCP bridge switched on (a static copy, or a server without "
                                                       "LAYERLING_MCP_REMOTE)."),
                                                    from_u8(m_origin)));
                    break;
                default:
                    m_bridge->SetLabel(status == 0 ? format_wxstr(_L("Nothing answers at %1% now: start Layerling's server first."), from_u8(m_origin)) :
                                                     format_wxstr(_L("%1% answered HTTP %2% for the MCP bridge."), from_u8(m_origin), status));
                }
                m_bridge->Wrap(FromDIP(600));
                update_text();
                Layout();
            });
        };
        Http::get(url)
            .timeout_connect(3)
            .timeout_max(6)
            .size_limit(64 * 1024)
            .on_complete([done](std::string, unsigned status) { done(status); })
            .on_error([done](std::string, std::string, unsigned status) { done(status); })
            .perform();
    }
};

} // namespace

void LayerlingPanel::Impl::show_mcp_setup()
{
    LayerlingMcpDialog dlg(wxGetTopLevelParent(owner), layerling::origin_of(cfg(KEY_BASE_URL)), custom_base());
    dlg.ShowModal();
}

LayerlingPanel::Impl::~Impl()
{
    *alive = false;
    notice_timer.Stop();
    handover_timer.Stop();
    if (cancel_flag)
        *cancel_flag = true;
    if (http)
        http->cancel();
    if (installer.joinable())
        installer.join();
    browser.reset();
    server.stop();
    boost::system::error_code ec;
    const fs::path            exports = temp_export_dir();
    if (fs::is_directory(exports, ec))
        fs::remove_all(exports, ec);
}

// ---------------------------------------------------------------------------------- panel ----

LayerlingPanel::LayerlingPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL), p(std::make_unique<Impl>(this))
{
    p->build_ui();
    Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent &evt) {
        evt.Skip();
        CallAfter([this]() {
            p->apply_colours();
            if (p->browser)
                p->browser->resize();
        });
    });
}

LayerlingPanel::~LayerlingPanel() = default;

void LayerlingPanel::set_active(bool active)
{
    p->active = active;
    if (active) {
        // First time, after shutdown(), or still on the Get page (installed meanwhile?).
        if (!p->started || !p->browser || p->get_page->IsShown()) {
            p->started = true;
            p->refresh();
        }
        if (p->browser) {
            p->browser->set_visible(p->toolbar->IsShown());
            p->browser->resize();
        }
        p->check_for_update(false);
    } else if (p->browser) {
        p->browser->set_visible(false);
    }
}

void LayerlingPanel::sys_color_changed() { p->apply_colours(); }

void LayerlingPanel::shutdown()
{
    p->cancel_download();
    if (p->installer.joinable())
        p->installer.join();
    p->browser.reset();
    p->server.stop();
    p->loaded_url.clear();
}

} // namespace GUI
} // namespace Slic3r
