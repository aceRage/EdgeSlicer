#include "ShowcaseHdriDialog.hpp"

#include "ShowcaseTextures.hpp"

#include "libslic3r/Showcase/ShowcaseHdriLibrary.hpp"
#include "libslic3r/Utils.hpp"

#include "../GUI.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../MsgDialog.hpp"
#include "slic3r/Utils/Http.hpp"

#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/progdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <mutex>

namespace Slic3r {
namespace GUI {

namespace {

wxString hdri_size_text(uint64_t bytes)
{
    return bytes > 0 ? wxString::Format(_L("%.1f MB"), double(bytes) / (1024. * 1024.)) : wxString();
}

// Writes `bytes` to `path` through a ".part" file next to it, so a half-written file never counts.
bool hdri_write_file(const boost::filesystem::path &path, const char *bytes, size_t size, std::string &error)
{
    boost::system::error_code ec;
    boost::filesystem::create_directories(path.parent_path(), ec);
    const boost::filesystem::path part = path.string() + ".part";
    {
        boost::nowide::ofstream f(part.string(), std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "cannot write " + part.string();
            return false;
        }
        f.write(bytes, std::streamsize(size));
        if (!f) {
            error = "cannot write " + part.string();
            return false;
        }
    }
    boost::filesystem::rename(part, path, ec);
    if (ec) {
        error = ec.message();
        boost::filesystem::remove(part, ec);
        return false;
    }
    return true;
}

} // namespace

ShowcaseHdriDialog::ShowcaseHdriDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, _L("Environments"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    const int em = wxGetApp().em_unit();
    auto *top = new wxBoxSizer(wxVERTICAL);
    m_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(52 * em, 30 * em), wxVSCROLL);
    m_list->SetScrollRate(0, em);
    m_list_sizer = new wxBoxSizer(wxVERTICAL);
    m_list->SetSizer(m_list_sizer);
    top->Add(m_list, 1, wxEXPAND | wxALL, em);

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    auto *add     = new wxButton(this, wxID_ANY, _L("Load my own HDRI..."));
    add->SetToolTip(_L("An equirectangular panorama (.hdr, or .exr with ZIP or no compression). It is copied into EdgeSlicer's data folder."));
    auto *close = new wxButton(this, wxID_CLOSE, _L("Close"));
    buttons->Add(add, 0, wxRIGHT, em);
    buttons->AddStretchSpacer(1);
    buttons->Add(close, 0);
    top->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em);
    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_add_own(); });
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CLOSE); });
    SetEscapeId(wxID_CLOSE);

    SetSizerAndFit(top);
    rebuild();
    wxGetApp().UpdateDlgDarkUI(this);
    CentreOnParent();
}

void ShowcaseHdriDialog::rebuild()
{
    const int em = wxGetApp().em_unit();
    m_list->Freeze();
    m_list_sizer->Clear(true);

    auto heading = [&](const wxString &text) {
        auto *t = new wxStaticText(m_list, wxID_ANY, text);
        t->SetFont(t->GetFont().Bold());
        m_list_sizer->Add(t, 0, wxTOP | wxBOTTOM, em / 2);
    };
    auto grid = [&]() {
        auto *g = new wxFlexGridSizer(3, em / 2, em);
        g->AddGrowableCol(0, 1);
        m_list_sizer->Add(g, 0, wxEXPAND | wxBOTTOM, em);
        return g;
    };

    heading(_L("Download (Poly Haven, CC0)"));
    wxFlexGridSizer *g = grid();
    for (const Showcase::HdriInfo &h : showcase_hdri_catalogue().hdris) {
        const bool installed = showcase_hdri_installed(h);
        const bool published = Showcase::hdri_download_published(h);
        auto *name = new wxStaticText(m_list, wxID_ANY, showcase_hdri_name(h));
        name->SetToolTip(wxString::FromUTF8(h.source));
        wxString state = installed ? _L("Downloaded") : published ? hdri_size_text(h.download_size) : _L("Not available yet");
        auto *status = new wxStaticText(m_list, wxID_ANY, state);
        auto *action = new wxButton(m_list, wxID_ANY, installed ? _L("Delete") : _L("Download"));
        action->Enable(installed || published);
        if (installed)
            action->SetToolTip(_L("Deletes the downloaded file. It can be downloaded again."));
        g->Add(name, 1, wxALIGN_CENTER_VERTICAL);
        g->Add(status, 0, wxALIGN_CENTER_VERTICAL);
        g->Add(action, 0);
        const Showcase::HdriInfo info = h;
        action->Bind(wxEVT_BUTTON, [this, info, installed](wxCommandEvent &) {
            // After the list is rebuilt this button is gone: run after the event.
            CallAfter([this, info, installed]() {
                if (installed)
                    on_delete_download(info);
                else
                    on_download(info);
            });
        });
    }

    m_list_sizer->Add(new wxStaticLine(m_list), 0, wxEXPAND | wxTOP | wxBOTTOM, em / 2);
    heading(_L("Your own HDRIs"));
    const std::vector<Showcase::HdriInfo> own = showcase_user_hdris();
    if (own.empty())
        m_list_sizer->Add(new wxStaticText(m_list, wxID_ANY, _L("None yet.")), 0, wxBOTTOM, em);
    else {
        wxFlexGridSizer *u = grid();
        for (const Showcase::HdriInfo &h : own) {
            u->Add(new wxStaticText(m_list, wxID_ANY, wxString::FromUTF8(h.name)), 1, wxALIGN_CENTER_VERTICAL);
            u->Add(new wxStaticText(m_list, wxID_ANY, wxString::FromUTF8(h.file)), 0, wxALIGN_CENTER_VERTICAL);
            auto *del = new wxButton(m_list, wxID_ANY, _L("Delete"));
            u->Add(del, 0);
            const Showcase::HdriInfo info = h;
            del->Bind(wxEVT_BUTTON, [this, info](wxCommandEvent &) { CallAfter([this, info]() { on_delete_own(info); }); });
        }
    }
    m_list->FitInside();
    m_list->Layout();
    m_list->Thaw();
    wxGetApp().UpdateDarkUIWin(m_list);
}

void ShowcaseHdriDialog::on_download(const Showcase::HdriInfo &hdri)
{
    if (!Showcase::hdri_download_published(hdri)) {
        MessageDialog(this, _L("This environment is not published on the download server yet."), _L("Environments"), wxOK | wxICON_INFORMATION)
            .ShowModal();
        return;
    }
    const std::string url = Showcase::hdri_download_url(Showcase::hdri_mirror_base(std::getenv(Showcase::HDRI_MIRROR_ENV)), hdri);
    struct State
    {
        std::atomic<size_t> now{ 0 }, total{ 0 };
        std::atomic<bool>   done{ false }, cancel{ false };
        std::mutex          lock;
        std::string         body, error;
        unsigned            status = 0;
    };
    auto state = std::make_shared<State>();
    BOOST_LOG_TRIVIAL(info) << "Showcase: downloading the HDRI " << hdri.id << " from " << url;
    Http::Ptr request = Http::get(url)
                            // The catalogue names the file, the mirror serves it, and the mirror is
                            // whatever EDGESLICER_HDRI_MIRROR (a local mock, a staging mirror) or the
                            // built-in URL says. Capping only the redirects leaves a plain-http
                            // mirror's own request alone while refusing to be sent off to http or
                            // into this machine's network from there.
                            .restrict_redirects(10)
                            .clear_headers()
                            .header("User-Agent", "EdgeSlicer")
                            .tls_policy(Http::TlsPolicy::Verify)
                            .timeout_connect(20)
                            .timeout_max(900)
                            .size_limit(size_t(std::min<uint64_t>(hdri.download_size + 4096, Showcase::HDRI_DOWNLOAD_MAX_BYTES)))
                            .on_progress([state](Http::Progress p, bool &cancel) {
                                state->now   = p.dlnow;
                                state->total = p.dltotal;
                                cancel       = state->cancel;
                            })
                            .on_complete([state](std::string body, unsigned status) {
                                std::lock_guard<std::mutex> guard(state->lock);
                                state->body   = std::move(body);
                                state->status = status;
                                state->done   = true;
                            })
                            .on_error([state](std::string, std::string error, unsigned status) {
                                std::lock_guard<std::mutex> guard(state->lock);
                                state->error  = error.empty() ? "HTTP " + std::to_string(status) : error;
                                state->status = status;
                                state->done   = true;
                            })
                            .perform();

    wxProgressDialog progress(_L("Downloading environment"), showcase_hdri_name(hdri), 1000, this,
                              wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME);
    auto cancelled_at = std::chrono::steady_clock::time_point();
    while (!state->done) {
        const size_t total = state->total > 0 ? size_t(state->total) : size_t(hdri.download_size);
        const size_t now   = state->now;
        const int    value = total > 0 ? int(std::min<double>(999., 1000. * double(now) / double(total))) : 0;
        if (!state->cancel && !progress.Update(value, wxString::Format(_L("%s of %s"), hdri_size_text(now), hdri_size_text(total)))) {
            state->cancel = true;
            cancelled_at  = std::chrono::steady_clock::now();
            if (request)
                request->cancel();
        }
        // A cancelled transfer normally ends at once; never wait on it for long.
        if (state->cancel && std::chrono::steady_clock::now() - cancelled_at > std::chrono::seconds(10))
            break;
        wxMilliSleep(40);
    }
    progress.Update(1000);
    if (state->cancel) {
        BOOST_LOG_TRIVIAL(info) << "Showcase: the HDRI download of " << hdri.id << " was cancelled";
        return;
    }
    std::string body, error;
    {
        std::lock_guard<std::mutex> guard(state->lock);
        body  = std::move(state->body);
        error = state->error;
    }
    if (!error.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Showcase: the HDRI download of " << hdri.id << " failed: " << error;
        MessageDialog(this, wxString::Format(_L("The download failed: %s"), wxString::FromUTF8(error)), _L("Environments"), wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }
    wxBusyCursor busy;
    const Showcase::DownloadCheck check = Showcase::verify_hdri_download(reinterpret_cast<const uint8_t *>(body.data()), body.size(), hdri);
    if (check != Showcase::DownloadCheck::Ok) {
        BOOST_LOG_TRIVIAL(warning) << "Showcase: the HDRI download of " << hdri.id << " was not kept: " << Showcase::to_string(check);
        MessageDialog(this, wxString::Format(_L("The downloaded file was not kept: %s"), wxString::FromUTF8(Showcase::to_string(check))),
                      _L("Environments"), wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }
    std::string write_error;
    if (!hdri_write_file(boost::filesystem::path(showcase_hdri_data_dir()) / hdri.file, body.data(), body.size(), write_error)) {
        MessageDialog(this, wxString::Format(_L("The file could not be saved: %s"), wxString::FromUTF8(write_error)), _L("Environments"),
                      wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "Showcase: the HDRI " << hdri.id << " was downloaded and checked (" << body.size() << " bytes)";
    m_changed  = true;
    m_added_id = hdri.id;
    showcase_hdris_reload();
    rebuild();
}

void ShowcaseHdriDialog::on_delete_download(const Showcase::HdriInfo &hdri)
{
    boost::system::error_code ec;
    boost::filesystem::remove(boost::filesystem::path(showcase_hdri_data_dir()) / hdri.file, ec);
    if (ec)
        BOOST_LOG_TRIVIAL(warning) << "Showcase: the HDRI " << hdri.id << " could not be deleted: " << ec.message();
    m_changed = true;
    showcase_hdris_reload();
    rebuild();
}

void ShowcaseHdriDialog::on_add_own()
{
    wxFileDialog dlg(this, _L("Load my own HDRI"), wxEmptyString, wxEmptyString, _L("HDRI panoramas (*.hdr;*.exr)") + "|*.hdr;*.HDR;*.exr;*.EXR",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string path = into_u8(dlg.GetPath());
    std::vector<uint8_t> bytes;
    {
        boost::nowide::ifstream f(path, std::ios::binary);
        if (f)
            bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    wxBusyCursor busy;
    Showcase::HdrImage image;
    std::string        error;
    if (bytes.empty())
        error = "the file could not be read";
    else if (Showcase::prepare_user_hdri(bytes.data(), bytes.size(), image, &error)) {
        // Baked once here, as the renderer will: a panorama without usable light is refused now.
        Showcase::HdrEnvironment baked;
        if (!Showcase::bake_hdr_environment(image, Showcase::HdrBakeOptions(), baked))
            error = "the panorama has no usable light";
    }
    if (!error.empty() || image.empty()) {
        MessageDialog(this, wxString::Format(_L("This HDRI cannot be used: %s"), wxString::FromUTF8(error)), _L("Load my own HDRI"),
                      wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }
    const std::string file_name = boost::filesystem::path(path).filename().string();
    std::vector<Showcase::HdriInfo> own = showcase_user_hdris();
    Showcase::HdriInfo info;
    info.id      = Showcase::unique_user_hdri_id(Showcase::user_hdri_slug(file_name), showcase_hdris().hdris);
    info.name    = Showcase::user_hdri_display_name(file_name);
    info.file    = info.id + ".hdr";
    info.licence = "your own";
    const std::string rgbe = Showcase::encode_rgbe(image);
    std::string       write_error;
    if (!hdri_write_file(boost::filesystem::path(showcase_hdri_data_dir()) / info.file, rgbe.data(), rgbe.size(), write_error)) {
        MessageDialog(this, wxString::Format(_L("The file could not be saved: %s"), wxString::FromUTF8(write_error)), _L("Load my own HDRI"),
                      wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }
    own.push_back(info);
    if (!showcase_save_user_hdris(own))
        BOOST_LOG_TRIVIAL(warning) << "Showcase: the list of your own HDRIs could not be saved";
    BOOST_LOG_TRIVIAL(info) << "Showcase: added the HDRI " << info.id << " (" << image.width << "x" << image.height << ")";
    m_changed  = true;
    m_added_id = info.id;
    showcase_hdris_reload();
    rebuild();
}

void ShowcaseHdriDialog::on_delete_own(const Showcase::HdriInfo &hdri)
{
    std::vector<Showcase::HdriInfo> own = showcase_user_hdris();
    own.erase(std::remove_if(own.begin(), own.end(), [&hdri](const Showcase::HdriInfo &h) { return h.id == hdri.id; }), own.end());
    showcase_save_user_hdris(own);
    boost::system::error_code ec;
    boost::filesystem::remove(boost::filesystem::path(showcase_hdri_data_dir()) / hdri.file, ec);
    m_changed = true;
    showcase_hdris_reload();
    rebuild();
}

} // namespace GUI
} // namespace Slic3r
