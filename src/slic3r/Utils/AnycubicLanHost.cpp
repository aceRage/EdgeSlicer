#include "AnycubicLanHost.hpp"

#include "AnycubicLan.hpp"
#include "Http.hpp"
#include "MQTT.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <chrono>
#include <thread>

namespace fs = boost::filesystem;

namespace Slic3r {

AnycubicLanHost::AnycubicLanHost(DynamicPrintConfig *config)
{
    m_host = config != nullptr && config->has("print_host") ? config->opt_string("print_host") : std::string();
}

const char *AnycubicLanHost::get_name() const { return "Anycubic LAN"; }

wxString AnycubicLanHost::get_test_ok_msg() const
{
    return _L("Connection to Anycubic LAN succeeded. The printer answered GET /info and the signed /ctrl handshake.");
}

wxString AnycubicLanHost::get_test_failed_msg(wxString &msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to Anycubic LAN"), msg);
}

bool AnycubicLanHost::test(wxString &msg) const
{
    AnycubicLan::Info        info;
    AnycubicLan::Credentials creds;
    std::string              error;
    if (!AnycubicLan::handshake(m_host, info, creds, error)) {
        msg = wxString::FromUTF8(error.c_str());
        return false;
    }
    msg = wxString::FromUTF8(
        (info.model_name.empty() ? info.device_name : info.model_name + (info.device_name.empty() ? "" : " / " + info.device_name))
            .c_str());
    if (msg.empty())
        msg = wxString::FromUTF8(info.model_id.c_str());
    BOOST_LOG_TRIVIAL(info) << "AnycubicLan: handshake ok model=" << info.model_id << " broker=" << creds.broker_host << ":"
                            << creds.broker_port;
    return true;
}

bool AnycubicLanHost::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    AnycubicLan::Info        info;
    AnycubicLan::Credentials creds;
    std::string              error;
    if (info_fn) info_fn("stg_connect", _L("Connecting to Anycubic LAN"));
    if (!AnycubicLan::handshake(m_host, info, creds, error)) {
        if (error_fn) error_fn(wxString::FromUTF8(error.c_str()));
        return false;
    }
    if (info.file_upload_url.empty()) {
        if (error_fn) error_fn(_L("The printer did not return a file upload URL."));
        return false;
    }

    const std::string filename = upload_data.upload_path.filename().string();
    bool              ok       = false;
    std::string       up_err;
    Http http = Http::post(info.file_upload_url);
    apply_tls(http);
    http.timeout_max(0)
        .form_add_file("file", upload_data.source_path, filename)
        .on_progress([&](Http::Progress progress, bool &cancel) {
            if (progress_fn)
                progress_fn(std::move(progress), cancel);
        })
        .on_error([&](std::string /*body*/, std::string err, unsigned status) {
            up_err = err.empty() ? ("HTTP " + std::to_string(status)) : err;
        })
        .on_complete([&](std::string /*body*/, unsigned) { ok = true; })
        .perform_sync();
    if (!ok) {
        if (error_fn) error_fn(wxString::FromUTF8(up_err.empty() ? "upload failed" : up_err.c_str()));
        return false;
    }
    if (upload_data.post_action != PrintHostPostUploadAction::StartPrint)
        return true;

    if (info_fn) info_fn("stg_print", _L("Starting the print over MQTT"));

    AnycubicLan::PrintStart job;
    job.filename = filename;
    job.filesize = 0;
    boost::system::error_code ec;
    const auto sz = fs::file_size(upload_data.source_path, ec);
    if (!ec) job.filesize = (long long) sz;
    std::string md5;
    if (bbl_calc_md5(upload_data.source_path.string(), md5))
        job.md5 = md5;
    const auto map_it = upload_data.extended_info.find("filament_mapping");
    if (map_it != upload_data.extended_info.end()) {
        job.ams     = AnycubicLan::ams_from_filament_mapping(map_it->second);
        job.use_ams = !job.ams.empty();
    }
    const auto level_it = upload_data.extended_info.find("auto_leveling");
    if (level_it != upload_data.extended_info.end() && level_it->second == "1")
        job.auto_leveling = 1;
    const auto vib_it = upload_data.extended_info.find("vibration_compensation");
    if (vib_it != upload_data.extended_info.end() && vib_it->second == "1")
        job.vibration_compensation = 1;

    const std::string cn       = creds.device_id.empty() ? info.cn : creds.device_id;
    const std::string model_id = creds.model_id.empty() ? info.model_id : creds.model_id;
    const std::string broker   = creds.broker_host.empty() ? AnycubicLan::bare_host(m_host) : creds.broker_host;
    const std::string uri      = AnycubicLan::mqtt_broker_uri(broker, creds.broker_port);
    const std::string client_id = "edgeslicer-" + AnycubicLan::make_msgid();
    try {
        auto mqtt = MqttClient::create(uri, client_id, creds.cert, creds.cert, creds.key, creds.username, creds.password, true);
        std::string mqtt_msg;
        if (!mqtt->Connect(mqtt_msg)) {
            if (error_fn) error_fn(wxString::FromUTF8(("MQTT connect: " + mqtt_msg).c_str()));
            return false;
        }
        const std::string filter = AnycubicLan::mqtt_public_filter(model_id, cn);
        mqtt->Subscribe(filter, 0, mqtt_msg);
        const auto env = AnycubicLan::command_envelope(AnycubicLan::make_msgid(), cn, model_id, "print", "start",
                                                       AnycubicLan::print_start_data(job));
        const std::string topic = AnycubicLan::mqtt_slicer_topic(model_id, cn, "print");
        if (!mqtt->Publish(topic, env.dump(), 0, mqtt_msg)) {
            if (error_fn) error_fn(wxString::FromUTF8(("MQTT print/start: " + mqtt_msg).c_str()));
            mqtt->Disconnect(mqtt_msg);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        mqtt->Disconnect(mqtt_msg);
    } catch (const std::exception &e) {
        if (error_fn) error_fn(wxString::FromUTF8(e.what()));
        return false;
    }
    return true;
}

} // namespace Slic3r
