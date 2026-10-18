#include "MoonrakerHost.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <wx/arrstr.h>
#include <wx/string.h>

#include "Http.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

using json = nlohmann::json;

namespace Slic3r {

namespace {

json parse_object(const std::string &body)
{
    const json j = json::parse(body, nullptr, false);
    return j.is_object() ? j : json::object();
}

json result_object(const json &root)
{
    const auto it = root.find("result");
    return (it != root.end() && it->is_object()) ? *it : json::object();
}

} // namespace

bool moonraker_host_is_server_info(const std::string &body)
{
    // GET /server/info: { "result": { "klippy_state": "ready|startup|shutdown|error|disconnected", ... } }
    const json result = result_object(parse_object(body));
    const auto it     = result.find("klippy_state");
    return it != result.end() && it->is_string() && !it->get_ref<const std::string &>().empty();
}

bool moonraker_host_is_printer_info(const std::string &body)
{
    // GET /printer/info: { "result": { "state": "ready", "software_version": "...", ... } }
    // Used when /server/info is missing (older shims) or when the host was mis-selected as OctoPrint.
    const json result = result_object(parse_object(body));
    const auto state  = result.find("state");
    if (state != result.end() && state->is_string() && !state->get_ref<const std::string &>().empty())
        return true;
    const auto ver = result.find("software_version");
    return ver != result.end() && ver->is_string() && !ver->get_ref<const std::string &>().empty();
}

std::vector<std::string> moonraker_host_writable_roots(const std::string &body)
{
    // GET /server/files/roots: { "result": [ { "name": "gcodes", "permissions": "rwd" }, ... ] }
    std::vector<std::string> out;
    const json               root = parse_object(body);
    const auto               it   = root.find("result");
    if (it == root.end() || !it->is_array())
        return out;
    for (const json &child : *it) {
        if (!child.is_object())
            continue;
        const auto name  = child.find("name");
        const auto perms = child.find("permissions");
        if (name == child.end() || !name->is_string())
            continue;
        const std::string &root_name = name->get_ref<const std::string &>();
        if (root_name.empty())
            continue;
        const std::string perm = (perms != child.end() && perms->is_string()) ? perms->get<std::string>() : std::string();
        if (perm.find('w') == std::string::npos)
            continue;
        out.push_back(root_name);
    }
    return out;
}

MoonrakerHostUploadReply moonraker_host_parse_upload_reply(const std::string &body)
{
    // FileUploadHandler writes finalize_upload()'s dict at the top level: item.path, print_started,
    // print_queued. There is no {"result": ...} envelope.
    MoonrakerHostUploadReply out;
    const json               root = parse_object(body);
    const auto               item = root.find("item");
    if (item != root.end() && item->is_object()) {
        const auto path = item->find("path");
        if (path != item->end() && path->is_string())
            out.stored_path = path->get<std::string>();
    }
    auto flag = [&root](const char *key) {
        const auto it = root.find(key);
        return it != root.end() && it->is_boolean() && it->get<bool>();
    };
    out.started_or_queued = flag("print_started") || flag("print_queued");
    return out;
}

MoonrakerHost::MoonrakerHost(DynamicPrintConfig *config)
    : m_host(config->opt_string("print_host"))
    , m_apikey(config->opt_string("printhost_apikey"))
    , m_cafile(config->opt_string("printhost_cafile"))
{}

const char *MoonrakerHost::get_name() const { return "Moonraker"; }

wxString MoonrakerHost::get_test_ok_msg() const
{
    return _(L("Connection to Moonraker is working correctly."));
}

wxString MoonrakerHost::get_test_failed_msg(wxString &msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to Moonraker"), msg);
}

std::string MoonrakerHost::make_url(const std::string &path) const
{
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0) {
        if (!m_host.empty() && m_host.back() == '/')
            return (boost::format("%1%%2%") % m_host % path).str();
        return (boost::format("%1%/%2%") % m_host % path).str();
    }
    return (boost::format("http://%1%/%2%") % m_host % path).str();
}

void MoonrakerHost::set_auth(Http &http) const
{
    if (!m_apikey.empty())
        http.header("X-Api-Key", m_apikey);
}

bool MoonrakerHost::test(wxString &msg) const
{
    // /server/info is the Moonraker envelope. klippy_state is logged but does not fail the test:
    // buddy-fork firmwares report non-ready states at idle, and a real upload problem surfaces at
    // upload() time. /printer/info is the fallback when /server/info is missing or not Moonraker.
    const char *name = get_name();
    bool        res  = true;
    auto        get  = [&](const std::string &path, std::string &body_out, wxString &err_out) {
        bool        ok  = false;
        const auto  url = make_url(path);
        BOOST_LOG_TRIVIAL(info) << boost::format("%1%: GET %2%") % name % url;
        auto http = Http::get(url);
        apply_tls(http);
        set_auth(http);
        http.on_error([&](std::string body, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error GET %2%: %3%, HTTP %4%, body: `%5%`") % name % url % error %
                                                status % body;
                err_out = format_error(body, error, status);
                body_out = std::move(body);
            })
            .on_complete([&](std::string body, unsigned) {
                body_out = std::move(body);
                ok       = true;
            })
            .perform_sync();
        return ok;
    };

    std::string body;
    wxString    err;
    if (get("server/info", body, err) && moonraker_host_is_server_info(body)) {
        BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: /server/info body: %2%") % name % body;
        return true;
    }
    std::string printer_body;
    wxString    printer_err;
    if (get("printer/info", printer_body, printer_err) && moonraker_host_is_printer_info(printer_body)) {
        BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: /printer/info body: %2%") % name % printer_body;
        return true;
    }

    res = false;
    if (!printer_err.empty())
        msg = printer_err;
    else if (!err.empty())
        msg = err;
    else
        msg = _L("The host responded but it doesn't look like Moonraker (missing result.klippy_state).");
    return res;
}

bool MoonrakerHost::get_storage(wxArrayString &storage_path, wxArrayString &storage_name) const
{
    // Optional: older Moonraker and some shims 404/501. upload() then uses the "gcodes" root.
    const char *name    = get_name();
    bool        got_any = false;
    const auto  url     = make_url("server/files/roots");
    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Enumerating storage roots at: %2%") % name % url;

    auto http = Http::get(url);
    apply_tls(http);
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            if (status == 404 || status == 501) {
                BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: /server/files/roots not implemented (HTTP %2%); upload() will fall back to "
                                                          "the \"gcodes\" root.") %
                                                name % status;
            } else {
                BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Could not enumerate roots: %2%, HTTP %3%, body: `%4%`") % name % error %
                                                  status % body;
            }
        })
        .on_complete([&](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: /server/files/roots body: %2%") % name % body;
            for (const std::string &root : moonraker_host_writable_roots(body)) {
                storage_path.Add(wxString::FromUTF8(root));
                storage_name.Add(wxString::FromUTF8(root));
                got_any = true;
            }
        })
        .perform_sync();

    return got_any;
}

bool MoonrakerHost::start_print(wxString &error_msg, const std::string &filename) const
{
    const char *name = get_name();
    bool        res  = true;
    const auto  url  = make_url("printer/print/start");
    json        body_json;
    body_json["filename"] = filename;
    const std::string body = body_json.dump();

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Starting print of %2% at %3%") % name % filename % url;

    auto http = Http::post(url);
    apply_tls(http);
    set_auth(http);
    http.header("Content-Type", "application/json")
        .set_post_body(body)
        .on_complete([&](std::string reply, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: print/start HTTP %2%: %3%") % name % status % reply;
        })
        .on_error([&](std::string reply, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error starting print at %2%: %3%, HTTP %4%, body: `%5%`") % name % url % error %
                                            status % reply;
            res       = false;
            error_msg = format_error(reply, error, status);
        })
        .perform_sync();

    return res;
}

bool MoonrakerHost::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn /* info_fn */) const
{
    // POST /server/files/upload with print=true so Moonraker starts (or queues) the job itself.
    // print_started / print_queued mean Moonraker owns the print: do not also POST /printer/print/start.
    wxString test_msg;
    if (!test(test_msg)) {
        error_fn(std::move(test_msg));
        return false;
    }

    const char *name                 = get_name();
    const auto  upload_filename      = upload_data.upload_path.filename();
    const std::string root           = upload_data.storage.empty() ? std::string("gcodes") : upload_data.storage;
    const bool        want_start     = upload_data.post_action == PrintHostPostUploadAction::StartPrint;
    const std::string url            = make_url("server/files/upload");
    bool              result         = true;
    std::string       uploaded_path;
    bool              moonraker_started_print = false;

    std::string plateindex;
    {
        const auto it = upload_data.extended_info.find("plateindex");
        if (it != upload_data.extended_info.end())
            plateindex = it->second;
    }

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% to %3% (root=%4%, filename=%5%, plateindex=%6%, start_print=%7%)") %
                                   name % upload_data.source_path % url % root % upload_filename.string() %
                                   (plateindex.empty() ? "-" : plateindex) % (want_start ? "true" : "false");

    auto http = Http::post(url);
    apply_tls(http);
    set_auth(http);
    http.form_add("root", root);
    if (!plateindex.empty())
        http.form_add("plateindex", plateindex);
    http.form_add("print", want_start ? "true" : "false");
    http.form_add_file("file", upload_data.source_path.string(), upload_filename.string())
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: upload HTTP %2%: %3%") % name % status % body;
            const MoonrakerHostUploadReply reply = moonraker_host_parse_upload_reply(body);
            if (!reply.stored_path.empty()) {
                uploaded_path = reply.stored_path;
            } else {
                uploaded_path = upload_filename.string();
                BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: upload response missing item.path, falling back to original filename `%2%`") %
                                                  name % uploaded_path;
            }
            moonraker_started_print = reply.started_or_queued;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading to %2%: %3%, HTTP %4%, body: `%5%`") % name % url % error %
                                            status % body;
            error_fn(format_error(body, error, status));
            result = false;
        })
        .on_progress([&](Http::Progress progress, bool &cancel) {
            progress_fn(std::move(progress), cancel);
            if (cancel) {
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                result = false;
            }
        })
        .perform_sync();

    if (!result)
        return false;

    if (want_start && !uploaded_path.empty() && !moonraker_started_print) {
        wxString start_msg;
        if (!start_print(start_msg, uploaded_path)) {
            error_fn(std::move(start_msg));
            return false;
        }
    }
    return true;
}

} // namespace Slic3r
