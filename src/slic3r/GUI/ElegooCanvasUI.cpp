#include "ElegooCanvasUI.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/PrintHostDevices.hpp"

#include "edgeslicer_version.h"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"

#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>

namespace Slic3r {
namespace GUI {

namespace {

bool preset_host_is_elegoolink(const DynamicPrintConfig& cfg)
{
    const auto* opt = cfg.option<ConfigOptionEnum<PrintHostType>>("host_type");
    return opt != nullptr && opt->value == htElegooLink;
}

std::vector<PrintHostDevices::Device> model_devices(const Preset& preset, std::string& last_used)
{
    std::vector<PrintHostDevices::Device> out;
    try {
        const std::string key = PrintHostDevices::model_key_for(preset);
        out                   = PrintHostDevices::devices(key);
        last_used             = PrintHostDevices::last_used_id(key);
    } catch (...) {}
    return out;
}

} // namespace

bool current_printer_reads_canvas()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return false;
    const Preset&             preset = bundle->printers.get_edited_preset();
    const DynamicPrintConfig& cfg    = preset.config;
    const auto*               model  = cfg.option<ConfigOptionString>("printer_model");
    if (model == nullptr || !canvas_capable_model(model->value))
        return false;
    if (preset_host_is_elegoolink(cfg))
        return true;
    std::string last;
    for (const PrintHostDevices::Device& d : model_devices(preset, last))
        if (d.host_type == "elegoolink" && !d.address.empty())
            return true;
    return false;
}

std::string canvas_address_for_current_printer()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return std::string();
    const Preset&                         preset = bundle->printers.get_edited_preset();
    std::string                           last;
    std::vector<PrintHostDevices::Device> devices = model_devices(preset, last);
    for (const PrintHostDevices::Device& d : devices)
        if (d.id == last && d.host_type == "elegoolink" && !d.address.empty())
            return d.address;
    const std::string own = preset.config.opt_string("print_host");
    if (!own.empty() && preset_host_is_elegoolink(preset.config))
        return own;
    for (const PrintHostDevices::Device& d : devices)
        if (d.host_type == "elegoolink" && !d.address.empty())
            return d.address;
    return std::string();
}

void run_canvas_job(wxWindow* parent, const wxString& message, std::function<void()> job)
{
    std::atomic<bool> done { false };
    std::thread       worker([&job, &done] {
        try {
            job();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "CANVAS: " << e.what();
        } catch (...) {}
        done = true;
    });
    {
        wxProgressDialog progress(_L("CANVAS"), message, 100, parent, wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH);
        while (!done) {
            progress.Pulse();
            wxMilliSleep(80);
        }
    }
    worker.join();
}

CanvasQueryResult query_canvas_with_progress(wxWindow* parent, const std::string& address)
{
    CanvasQueryResult result;
    if (address.empty()) {
        result.error = "this printer has no address";
        return result;
    }
    run_canvas_job(parent, _L("Reading the CANVAS on the printer..."), [&result, address] { result = canvas_query(address); });
    return result;
}

std::vector<FilamentData> canvas_machine_filaments(const CanvasInfo& info)
{
    std::vector<FilamentData> out;
    for (const CanvasTray& t : info.trays) {
        FilamentData fd;
        fd.m_index = static_cast<unsigned int>(t.slot_index);
        if (t.has_filament) {
            fd.m_name   = canvas_tray_name(t);
            fd.m_type   = t.tray_type;
            fd.m_vendor = canvas_tray_vendor(t);
            fd.m_color  = FilamentColor::FromColors({ t.colour_hex() }, FilamentColorMode::Segment);
        }
        out.push_back(std::move(fd));
    }
    return out;
}

wxString canvas_failure_text(const CanvasQueryResult& result)
{
    if (result.no_canvas)
        return _L("The printer reports no CANVAS attached.");
    if (!result.reached)
        return format_wxstr(_L("The printer did not answer (%1%). Check that it is on and on the network."), from_u8(result.error));
    return format_wxstr(_L("The printer answered, but sent no CANVAS data (%1%)."), from_u8(result.error));
}

void capture_canvas_data(wxWindow* parent, const std::string& address, const std::string& printer_model)
{
    if (address.empty()) {
        show_error(parent, _L("This printer has no address to read the CANVAS from."));
        return;
    }
    nlohmann::json ctx;
    ctx["app"]           = std::string("EdgeSlicer ") + EDGESLICER_FULL_VERSION;
    ctx["printer_model"] = printer_model;
    {
        const std::time_t now = std::time(nullptr);
        char              buf[32] {};
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        ctx["captured_at"] = buf;
    }
    std::string text;
    run_canvas_job(parent, _L("Capturing CANVAS data from the printer..."),
                   [&text, address, c = ctx.dump()] { text = canvas_capture(address, c); });
    if (text.empty()) {
        show_error(parent, _L("The CANVAS data could not be captured."));
        return;
    }

    char stamp[32] {};
    {
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    }
    const boost::filesystem::path dir  = boost::filesystem::path(data_dir()) / "log";
    const boost::filesystem::path file = dir / (std::string("elegoo_canvas_capture_") + stamp + ".json");
    bool                          ok   = false;
    try {
        boost::filesystem::create_directories(dir);
        boost::nowide::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        ok = out.good();
    } catch (...) {
        ok = false;
    }
    if (!ok) {
        show_error(parent, _L("The CANVAS data could not be written to the log folder."));
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "CANVAS: capture written to " << file.filename().string();
    MessageDialog dlg(parent,
                      format_wxstr(_L("The CANVAS data was saved to:\n%1%\n\nMainboard ids, serial numbers and addresses were "
                                      "removed. Open the folder?"),
                                   from_u8(file.string())),
                      _L("Capture Canvas data"), wxYES_NO | wxICON_INFORMATION);
    if (dlg.ShowModal() == wxID_YES)
        desktop_open_any_folderEx(file.string());
}

// ----------------------------------------------------------------- the Device tab card ----

namespace {

long long canvas_card_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

const int CANVAS_CARD_THROTTLE_MS = 10000;

wxString canvas_state_word(CanvasTrayState s)
{
    switch (s) {
    case CanvasTrayState::Empty: return _L("empty");
    case CanvasTrayState::Preloaded: return _L("preloaded");
    case CanvasTrayState::Loaded: return _L("loaded");
    default: return _L("unknown");
    }
}

} // namespace

ElegooCanvasPanel::ElegooCanvasPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY), m_alive(std::make_shared<std::atomic<bool>>(true))
{
    auto* top    = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    m_title      = new wxStaticText(this, wxID_ANY, _L("CANVAS"));
    m_title->SetFont(::Label::Head_14);
    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_status->SetFont(::Label::Body_13);
    m_btn_refresh = new ::Button(this, _L("Refresh"));
    m_btn_refresh->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    m_btn_refresh->SetToolTip(_L("Read the CANVAS trays from the printer again"));
    m_btn_capture = new ::Button(this, _L("Capture Canvas data"));
    m_btn_capture->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    m_btn_capture->SetToolTip(_L("Read the CANVAS of this printer and save it, without its serial number, "
                                 "mainboard id or address, as a JSON file in the log folder"));
    header->Add(m_title, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    header->Add(m_status, 1, wxALIGN_CENTER_VERTICAL);
    header->Add(m_btn_refresh, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    header->Add(m_btn_capture, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    top->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    m_units = new wxPanel(this, wxID_ANY);
    m_units->SetSizer(new wxBoxSizer(wxVERTICAL));
    top->Add(m_units, 0, wxEXPAND | wxALL, FromDIP(8));
    SetSizer(top);

    m_btn_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh(false); });
    m_btn_capture->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_address.empty())
            capture_canvas_data(this, m_address, m_model);
    });
    Hide();
}

ElegooCanvasPanel::~ElegooCanvasPanel()
{
    // A query still running posts into nothing.
    *m_alive = false;
}

bool ElegooCanvasPanel::set_target(const std::string& address, const std::string& printer_model)
{
    const bool        wanted       = !address.empty() && canvas_capable_model(printer_model);
    const std::string next_address = wanted ? address : std::string();
    if (next_address == m_address && printer_model == m_model)
        return wanted;
    m_address = next_address;
    m_model   = printer_model;
    ++m_generation; // an answer for the previous printer is dropped
    m_busy            = false;
    m_last_started_ms = 0;
    m_units->GetSizer()->Clear(true); // the tiles and their sizers
    set_status(wanted ? _L("Not read yet. Press Refresh, or open the Device tab.") : wxString());
    Show(wanted);
    if (GetParent())
        GetParent()->Layout();
    if (wanted && IsShownOnScreen())
        refresh(false);
    return wanted;
}

void ElegooCanvasPanel::refresh(bool throttled)
{
    if (m_address.empty() || m_busy)
        return;
    const long long now = canvas_card_now_ms();
    if (throttled && m_last_started_ms != 0 && now - m_last_started_ms < CANVAS_CARD_THROTTLE_MS)
        return;
    m_busy            = true;
    m_last_started_ms = now;
    m_btn_refresh->Enable(false);
    set_status(_L("Reading the CANVAS on the printer..."));
    const int                          generation = m_generation;
    const std::string                  address    = m_address;
    std::shared_ptr<std::atomic<bool>> alive      = m_alive;
    // Detached: it holds nothing of the card, only its own copies, and is bounded by the query's
    // timeouts. The answer comes back on the GUI thread and is dropped if the card is gone.
    std::thread([this, generation, address, alive] {
        CanvasQueryResult r;
        try {
            r = canvas_query(address);
        } catch (...) {
            r.error = "CANVAS query failed";
        }
        if (wxTheApp != nullptr)
            wxTheApp->CallAfter([this, generation, alive, r] {
                if (*alive)
                    on_result(generation, r);
            });
    }).detach();
}

void ElegooCanvasPanel::on_result(int generation, const CanvasQueryResult& result)
{
    if (generation != m_generation)
        return;
    m_busy = false;
    m_btn_refresh->Enable(true);
    m_units->GetSizer()->Clear(true); // the tiles and their sizers
    if (!result.ok) {
        set_status(canvas_failure_text(result));
    } else {
        const std::time_t now = std::time(nullptr);
        char              buf[16] {};
        std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&now));
        set_status(format_wxstr(_L("%1% of %2% trays loaded. Read at %3%."), result.info.loaded_count(), result.info.trays.size(),
                                from_u8(buf)));
        rebuild_units(result.info);
    }
    Layout();
    if (GetParent())
        GetParent()->Layout();
}

void ElegooCanvasPanel::rebuild_units(const CanvasInfo& info)
{
    wxSizer*  units_sizer = m_units->GetSizer();
    const int swatch      = FromDIP(22);
    for (const CanvasUnit& unit : info.units) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* name = new wxStaticText(m_units, wxID_ANY, format_wxstr(_L("Unit %1%"), unit.canvas_id + 1));
        name->SetFont(::Label::Head_13);
        row->Add(name, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        if (!unit.connected) {
            row->Add(new wxStaticText(m_units, wxID_ANY, _L("not connected")), 0, wxALIGN_CENTER_VERTICAL);
            units_sizer->Add(row, 0, wxBOTTOM, FromDIP(6));
            continue;
        }
        for (const CanvasTray& t : info.trays) {
            if (t.canvas_id != unit.canvas_id)
                continue;
            const bool active = info.active_canvas_id == t.canvas_id && info.active_tray_id == t.tray_id && info.active_tray_id >= 0;
            auto*      tile   = new wxBoxSizer(wxHORIZONTAL);
            // The swatch: the tray's colour, a thick dark frame for the tray in the extruder.
            auto* frame = new wxPanel(m_units, wxID_ANY, wxDefaultPosition, wxSize(swatch + FromDIP(4), swatch + FromDIP(4)));
            frame->SetBackgroundColour(active ? wxColour(40, 40, 40) : wxColour(170, 170, 170));
            auto* fill = new wxPanel(frame, wxID_ANY, wxPoint(FromDIP(active ? 3 : 1), FromDIP(active ? 3 : 1)),
                                     wxSize(swatch + FromDIP(active ? -2 : 2), swatch + FromDIP(active ? -2 : 2)));
            const wxColour colour = t.has_filament ? wxColour(from_u8(t.colour_hex())) : wxColour(235, 235, 235);
            fill->SetBackgroundColour(colour.IsOk() ? colour : wxColour(235, 235, 235));
            tile->Add(frame, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

            auto*    text  = new wxBoxSizer(wxVERTICAL);
            wxString line1 = wxString::Format("%d  ", t.position + 1) + (t.has_filament ? from_u8(t.filament_type) : _L("empty"));
            if (active)
                line1 += "  " + _L("(in use)");
            auto* l1 = new wxStaticText(m_units, wxID_ANY, line1);
            l1->SetFont(active ? ::Label::Head_12 : ::Label::Body_12);
            text->Add(l1);
            if (t.has_filament) {
                // The same name the sync uses: "ELEGOO PLA Silk", "Generic PLA PRO" for an untagged
                // spool, just the filament's name when the brand is empty.
                auto* l2 = new wxStaticText(m_units, wxID_ANY, from_u8(canvas_tray_name(t)));
                l2->SetFont(::Label::Body_10);
                text->Add(l2);
            }
            tile->Add(text, 0, wxALIGN_CENTER_VERTICAL);

            // The raw facts for anyone comparing with the printer's screen (design R1/R2).
            wxString tip = format_wxstr(_L("Tray %1% (tray_id %2%)"), t.position + 1, t.tray_id);
            if (t.has_filament) {
                tip += "\n" + from_u8(canvas_tray_name(t)) + "  " + from_u8(t.colour_hex());
                if (t.min_nozzle_temp > 0 || t.nozzle_temp > 0)
                    tip += "\n" + wxString::Format("%d - %d ", t.min_nozzle_temp, t.nozzle_temp) + wxString::FromUTF8("\xC2\xB0") + "C";
            }
            if (t.status >= 0)
                tip += "\n" + format_wxstr(_L("Status %1% (%2%)"), t.status, canvas_state_word(t.state()));
            frame->SetToolTip(tip);
            fill->SetToolTip(tip);
            l1->SetToolTip(tip);
            row->Add(tile, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(18));
        }
        units_sizer->Add(row, 0, wxBOTTOM, FromDIP(6));
    }
    m_units->Layout();
}

void ElegooCanvasPanel::set_status(const wxString& text)
{
    if (m_status)
        m_status->SetLabel(text);
}

void ElegooCanvasPanel::sys_color_changed()
{
    if (m_btn_refresh)
        m_btn_refresh->Rescale();
    if (m_btn_capture)
        m_btn_capture->Rescale();
    Refresh();
}

void ElegooCanvasPanel::msw_rescale() { sys_color_changed(); }

} // namespace GUI
} // namespace Slic3r
