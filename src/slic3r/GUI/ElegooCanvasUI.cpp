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

#include <wx/progdlg.h>
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

} // namespace GUI
} // namespace Slic3r
