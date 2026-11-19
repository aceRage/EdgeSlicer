#include "ElegooSdcpFiles.hpp"

#include "ElegooCanvas.hpp"
#include "ElegooSdcp.hpp"
#include "ElegooSdcpMonitor.hpp"
#include "ElegooStartPrint.hpp"
#include "libslic3r/PrintConfig.hpp" // BedType::btPC

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <set>
#include <thread>

namespace Slic3r {
namespace SdcpFiles {

using json = nlohmann::json;
namespace pstore = PrinterStorage;

namespace {

// ---- the one-time "unverified" log ------------------------------------------------------------------

std::mutex               sf_g_log_mutex;
std::set<std::string>    sf_g_logged;
std::vector<std::string> sf_g_lines;

// Logs `text` once per process under `key`: what the owner's first run should tell us. Never an
// address, a mainboard id or a frame (those carry ids); only keys, counts and Acks.
void sf_log_unverified_once(const std::string& key, const std::string& text)
{
    std::lock_guard<std::mutex> lock(sf_g_log_mutex);
    if (!sf_g_logged.insert(key).second)
        return;
    const std::string line = "[Storage][CC1][unverified] " + text;
    sf_g_lines.push_back(line);
    BOOST_LOG_TRIVIAL(warning) << line;
}

// ---- json helpers -----------------------------------------------------------------------------------

const json* sf_object_at(const json& j, const char* key)
{
    if (!j.is_object())
        return nullptr;
    auto it = j.find(key);
    return (it != j.end() && it->is_object()) ? &*it : nullptr;
}

const json* sf_array_at(const json& j, const char* key)
{
    if (!j.is_object())
        return nullptr;
    auto it = j.find(key);
    return (it != j.end() && it->is_array()) ? &*it : nullptr;
}

int sf_int_at(const json& j, const char* key, int fallback)
{
    if (!j.is_object())
        return fallback;
    auto it = j.find(key);
    if (it == j.end())
        return fallback;
    if (it->is_number_integer() || it->is_number_unsigned())
        return it->get<int>();
    if (it->is_number_float()) {
        const double v = it->get<double>();
        return std::isfinite(v) ? (int) v : fallback;
    }
    return fallback;
}

std::string sf_str_at(const json& j, const char* key)
{
    if (!j.is_object())
        return {};
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// A number, or a string of digits; false otherwise.
bool sf_number_of(const json& v, double& out)
{
    if (v.is_number()) {
        out = v.get<double>();
        return std::isfinite(out);
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s.empty() || s.size() > 20 || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }))
            return false;
        try {
            out = std::stod(s);
            return true;
        } catch (...) {}
    }
    return false;
}

// The first of `keys` the object carries.
const json* sf_first_of(const json& obj, std::initializer_list<const char*> keys)
{
    for (const char* k : keys) {
        auto it = obj.find(k);
        if (it != obj.end() && !it->is_null())
            return &*it;
    }
    return nullptr;
}

std::string sf_base_name(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

long long sf_steady_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

// ---- requests ---------------------------------------------------------------------------------------

json list_request_data()
{
    // Exactly the printer's own web UI request (pycentauri): any other shape risks the firmware.
    json d;
    d["Url"] = LOCAL_ROOT;
    return d;
}

std::string full_path(const std::string& relative)
{
    return std::string(LOCAL_ROOT) + "/" + relative;
}

json delete_request_data(const std::vector<std::string>& relative_paths)
{
    json list = json::array();
    for (const std::string& p : relative_paths)
        list.push_back(full_path(p));
    json d;
    d["FileList"] = std::move(list);
    return d;
}

std::string relative_path(const std::string& printer_path)
{
    std::string p = printer_path;
    while (!p.empty() && std::isspace((unsigned char) p.back()))
        p.pop_back();
    while (!p.empty() && std::isspace((unsigned char) p.front()))
        p.erase(p.begin());
    for (const char* prefix : { "/local/", "local/" }) {
        const std::string pre = prefix;
        if (p.compare(0, pre.size(), pre) == 0) {
            p = p.substr(pre.size());
            return pstore::valid_path(p) ? p : std::string();
        }
    }
    if (!p.empty() && p.front() == '/')
        return std::string(); // another storage ("/usb/...") or the root itself
    return pstore::valid_path(p) ? p : std::string();
}

bool same_file(const std::string& a, const std::string& b)
{
    const std::string na = sf_base_name(a), nb = sf_base_name(b);
    return !na.empty() && na == nb;
}

// ---- answers ----------------------------------------------------------------------------------------

int ack_of(const std::string& frame_text)
{
    const json j = json::parse(frame_text, nullptr, false);
    if (j.is_discarded())
        return -1;
    if (const json* d = sf_object_at(j, "Data")) {
        if (const json* dd = sf_object_at(*d, "Data")) {
            const int a = sf_int_at(*dd, "Ack", -1);
            if (a >= 0)
                return a;
        }
        return sf_int_at(*d, "Ack", -1);
    }
    return -1;
}

ListAnswer parse_list_answer(const std::string& frame_text)
{
    ListAnswer a;
    const json j = json::parse(frame_text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return a;
    a.ack = ack_of(frame_text);
    const json* list = nullptr;
    if (const json* d = sf_object_at(j, "Data")) {
        if (const json* dd = sf_object_at(*d, "Data"))
            list = sf_array_at(*dd, "FileList");
        if (!list)
            list = sf_array_at(*d, "FileList");
    }
    if (!list)
        list = sf_array_at(j, "FileList");
    if (!list)
        return a;
    a.parsed = true;
    bool time_form_seen = false;
    for (const json& item : *list) {
        if (!item.is_object()) {
            ++a.unlisted;
            continue;
        }
        if (a.first_entry_keys.empty())
            for (auto it = item.begin(); it != item.end(); ++it)
                a.first_entry_keys.push_back(it.key());
        std::string name = sf_str_at(item, "name");
        if (name.empty())
            name = sf_str_at(item, "Name");
        if (name.empty())
            name = sf_str_at(item, "path");
        if (name.empty()) {
            ++a.unlisted;
            continue;
        }
        // The specification: type 0 a folder, 1 a file. The tab lists the storage root only.
        if (sf_int_at(item, "type", 1) == 0) {
            ++a.folders;
            continue;
        }
        if (name.compare(0, 7, "/local/") == 0 || name.compare(0, 6, "local/") == 0)
            a.names_prefixed = true;
        else if (!name.empty() && name.front() == '/') {
            ++a.other_storage;
            continue;
        }
        const std::string rel = relative_path(name);
        if (rel.empty() || rel.front() == '.' || sf_base_name(rel).front() == '.' || !pstore::is_listed_name(rel)) {
            ++a.unlisted;
            continue;
        }
        ListedFile f;
        f.entry.path = rel;
        f.entry.name = sf_base_name(rel);
        double v     = 0;
        if (const json* s = sf_first_of(item, { "FileSize", "size", "Size" }); s && sf_number_of(*s, v) && v > 0)
            f.entry.size = (std::uint64_t) v;
        if (const json* l = sf_first_of(item, { "TotalLayers", "layer", "Layers" }); l && sf_number_of(*l, v) && v > 0 && v < 1e7)
            f.total_layers = (int) v;
        std::string form = "absent";
        if (const json* t = sf_first_of(item, { "CreateTime", "create_time", "date" })) {
            if (sf_number_of(*t, v) && v > 0) {
                // Unix seconds are about 1.7e9 now; milliseconds about 1.7e12.
                const bool ms = v > 1e11;
                f.entry.modified = (std::int64_t) (ms ? v / 1000.0 : v);
                form             = t->is_string() ? "string" : (ms ? "milliseconds" : "seconds");
            } else {
                form = "other";
            }
        }
        if (!time_form_seen) {
            a.create_time_form = form;
            time_form_seen     = true;
        }
        a.files.push_back(std::move(f));
    }
    if (!time_form_seen)
        a.create_time_form = "absent";
    return a;
}

DeleteAnswer parse_delete_answer(const std::string& frame_text)
{
    DeleteAnswer a;
    const json j = json::parse(frame_text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return a;
    a.parsed = true;
    a.ack    = ack_of(frame_text);
    const json* err = nullptr;
    if (const json* d = sf_object_at(j, "Data")) {
        if (const json* dd = sf_object_at(*d, "Data"))
            err = sf_array_at(*dd, "ErrData");
        if (!err)
            err = sf_array_at(*d, "ErrData");
    }
    if (err)
        for (const json& e : *err)
            if (e.is_string()) {
                const std::string rel = relative_path(e.get<std::string>());
                a.failed.push_back(rel.empty() ? e.get<std::string>() : rel);
            }
    return a;
}

std::string start_ack_text(int ack)
{
    switch (ack) {
    case 0: return std::string();
    case 1: return "The printer is busy and did not start the print.";
    case 2: return "The printer could not find the file.";
    case 3: return "The printer says the file is corrupted (MD5 check failed).";
    case 4: return "The printer could not read the file.";
    case 5: return "The printer says the file's resolution does not match.";
    case 6: return "The printer does not recognise the file format.";
    case 7: return "The file was sliced for another printer model.";
    default: return "The printer refused to start the print (Ack " + std::to_string(ack) + ").";
    }
}

// ---- the start plan ---------------------------------------------------------------------------------

StartPlan plan_start(const PrintHostDevices::Status& st, const std::map<std::string, std::string>& options, int pc_bed_type,
                     const std::string& printer)
{
    using PrintHostDevices::SlotSendPolicy;
    StartPlan        plan;
    const std::string who = printer.empty() ? std::string("The printer") : printer;
    PrintHostDevices::SlotSendFacts& f = plan.facts;
    f.probe_ok       = st.online;
    f.slot_unit      = st.online;
    f.busy           = PrintHostDevices::busy_printing(st.state);
    f.unit_in_use    = st.canvas && st.slots_have_filaments;
    f.used_filaments = 0; // a stored file's filaments are not known here
    f.as_sliced_ok   = false;
    plan.policy      = PrintHostDevices::slot_send_policy(f);

    if (!st.online) {
        // slot_send_policy says NotApplicable for a printer that did not answer (nothing to map);
        // a stored file is never started blind, though: busy or not is unknown.
        plan.refusal = who + " did not answer, so the print was not started" + (st.error.empty() ? std::string(".") : " (" + st.error + ").");
        return plan;
    }
    switch (plan.policy) {
    case SlotSendPolicy::Start: plan.may_start = true; break;
    case SlotSendPolicy::NeedsMapping:
        // A CANVAS with loaded trays, and nothing to map them from: the send dialog's "every
        // filament As sliced", with the person told so.
        plan.may_start              = true;
        plan.as_sliced_confirmation = true;
        break;
    case SlotSendPolicy::UploadOnlyBusy:
        plan.refusal = who + " is " + (st.state == "paused" ? "paused mid-print" : "printing") + ", so the print was not started. Finish or cancel that job first.";
        return plan;
    default:
        plan.refusal = who + " is not in a state to start a print from its storage.";
        return plan;
    }

    // The send dialog's remembered choices, as ElegooPrintHostSendDialog::extendedInfo gives them.
    auto opt = [&options](const char* key, const std::string& fallback) {
        auto it = options.find(key);
        return (it == options.end() || it->second.empty()) ? fallback : it->second;
    };
    plan.extended_info["timeLapse"]         = opt("timeLapse", "0");
    plan.extended_info["heatedBedLeveling"] = opt("heatedBedLeveling", "0");
    plan.extended_info["bedType"]           = opt("bedType", std::string());
    if (plan.extended_info["bedType"].empty())
        plan.extended_info.erase("bedType");
    if (st.canvas) {
        // Only when a CANVAS answered, as the dialog does: an empty slot_map, the printer decides.
        plan.extended_info["canvasAttached"] = "1";
        plan.extended_info["slotMap"]        = std::string();
    }

    const bool leveling = ElegooStartPrint::flag(plan.extended_info, "heatedBedLeveling") == 1;
    const bool timelapse = ElegooStartPrint::flag(plan.extended_info, "timeLapse") == 1;
    const bool pc_side   = ElegooStartPrint::platform_type(plan.extended_info, pc_bed_type) == 1;
    plan.advice = std::string("Bed leveling before printing: ") + (leveling ? "on" : "off") + ". Timelapse: " + (timelapse ? "on" : "off") +
                  ". Plate: " + (pc_side ? "smooth PC side" : "textured PTE side") +
                  ". (The choices of your last Elegoo send; change them in the send dialog.)";
    if (plan.as_sliced_confirmation)
        plan.advice += "\n\nA CANVAS with loaded trays is attached. EdgeSlicer cannot tell which filaments this file uses, so no "
                       "tray mapping is sent: the printer feeds from its own choice of trays, as when every filament is left "
                       "\"As sliced\" in the send dialog. Make sure the trays hold the filaments the file was sliced for.";
    return plan;
}

json start_request_data(const std::map<std::string, std::string>& extended_info, int pc_bed_type, const std::string& relative)
{
    const json envelope = json::parse(ElegooStartPrint::build_payload(extended_info, pc_bed_type, relative, std::string(), 0,
                                                                      CMD_START_PRINT),
                                      nullptr, false);
    if (envelope.is_discarded())
        return json::object();
    const json* d = sf_object_at(envelope, "Data");
    const json* dd = d ? sf_object_at(*d, "Data") : nullptr;
    return dd ? *dd : json::object();
}

// ---- the real channel -------------------------------------------------------------------------------

namespace {

class sf_MonitorChannel : public SdcpChannel
{
public:
    sf_MonitorChannel(std::string address, std::string model)
        : m_address(std::move(address)), m_host(sdcp_host_of(m_address)), m_model(std::move(model))
    {}

    Reply command(int cmd, const json& data, int timeout_ms) override
    {
        Reply r;
        if (m_host.empty()) {
            r.error = "this printer has no address";
            return r;
        }
        const SdcpMonitor::CommandResult c = SdcpMonitor::command(m_host, cmd, data, timeout_ms);
        r.sent     = c.sent;
        r.answered = c.answered;
        r.ack      = c.ack;
        r.frame    = c.frame;
        r.error    = c.error;
        return r;
    }

    PrinterRead read_state() override
    {
        PrinterRead out;
        if (m_host.empty()) {
            out.error = "this printer has no address";
            return out;
        }
        // The shared connection (opened on demand; it asks Cmd 1 and Cmd 0 itself when it opens).
        Sdcp::Snapshot s = SdcpMonitor::demand(m_host);
        for (const long long until = sf_steady_ms() + 8000; !s.connected && !s.other_window && sf_steady_ms() < until;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            s = SdcpMonitor::touch(m_host);
        }
        if (!s.connected) {
            out.error = s.other_window ? "another EdgeSlicer window is connected to this printer" :
                                         (s.error.empty() ? std::string("the printer did not connect in time") : s.error);
            return out;
        }
        // Then a fresh Cmd 0. Some firmware answers it with an Ack and the status as its own frame,
        // some with the status alone, so the command's own answer is not waited for long: the
        // status frame that arrives after the ask is what counts.
        const long long asked = sf_steady_ms();
        SdcpMonitor::command_if_connected(m_host, Sdcp::CMD_STATUS, json::object(), 2500);
        s = SdcpMonitor::touch(m_host);
        for (const long long until = sf_steady_ms() + 2500; s.status_at_ms < asked && sf_steady_ms() < until && s.connected;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            s = SdcpMonitor::touch(m_host);
        }
        if (!s.status.valid) {
            out.error = "the printer did not report its status";
            return out;
        }
        if (s.status_at_ms < asked)
            BOOST_LOG_TRIVIAL(info) << "[Storage][CC1] no fresh status after Cmd 0; using the connection's last one";
        out.reached  = true;
        out.state    = Sdcp::hub_state(s.status);
        out.filename = s.status.filename;
        out.status.probed = out.status.online = true;
        out.status.state  = out.state;
        return out;
    }

    PrinterRead read_printer() override
    {
        PrinterRead out;
        // The send dialog's read (PrintHostDevices::probe_elegoo): through the shared connection.
        CanvasQueryOptions opts;
        opts.frame_timeout_ms        = 3000;
        opts.step_timeout_ms         = 6000;
        const CanvasQueryResult q    = canvas_query(m_address, opts);
        out.status                   = PrintHostDevices::status_from_canvas(q);
        out.reached                  = q.reached && !q.print_state.empty();
        out.error                    = q.error.empty() && q.reached && q.print_state.empty() ? "the printer did not report its status" : q.error;
        out.state                    = q.print_state;
        if (!out.reached)
            out.status.online = false; // what it is doing is unknown: never "not busy"
        if (!m_host.empty())
            out.filename = SdcpMonitor::touch(m_host).status.filename;
        return out;
    }

private:
    std::string m_address;
    std::string m_host;
    std::string m_model;
};

} // namespace

std::shared_ptr<SdcpChannel> make_monitor_channel(const std::string& address, const std::string& printer_model)
{
    return std::make_shared<sf_MonitorChannel>(address, printer_model);
}

// ---- the backend ------------------------------------------------------------------------------------

Cc1Files::Cc1Files(std::shared_ptr<SdcpChannel> channel, std::map<std::string, std::string> start_options, int pc_bed_type,
                   std::string printer_name)
    : m_channel(std::move(channel)), m_options(std::move(start_options)), m_pc_bed_type(pc_bed_type), m_printer(std::move(printer_name))
{}

namespace {

pstore::Result sf_not_sent(const Reply& r)
{
    return pstore::Result::failure(pstore::Status::Offline,
                                   "The printer could not be reached" + (r.error.empty() ? std::string(".") : " (" + r.error + ")."));
}

} // namespace

pstore::Result Cc1Files::list_raw(std::vector<ListedFile>& out)
{
    out.clear();
    const Reply r = m_channel->command(CMD_FILE_LIST, list_request_data(), 10000);
    if (!r.sent)
        return sf_not_sent(r);
    if (!r.answered) {
        sf_log_unverified_once("258-unanswered", "Cmd 258 (file list) got no answer: " + r.error);
        return pstore::Result::failure(pstore::Status::Failed, "The printer did not answer the file list request (" +
                                                                   (r.error.empty() ? std::string("no answer") : r.error) + ").");
    }
    const ListAnswer a = parse_list_answer(r.frame);
    if (!a.parsed) {
        sf_log_unverified_once("258-shape-none", "Cmd 258 answered with Ack " + std::to_string(a.ack >= 0 ? a.ack : r.ack) +
                                                  " but no FileList where expected (Data.Data / Data / top level)");
        if (a.ack > 0 || r.ack > 0)
            return pstore::Result::failure(pstore::Status::Failed,
                                           "The printer refused to list its files (Ack " + std::to_string(a.ack > 0 ? a.ack : r.ack) + ").");
        return pstore::Result::failure(pstore::Status::NotSupported, "This printer's answer to the file list request carries no file list.");
    }
    std::string keys;
    for (const std::string& k : a.first_entry_keys)
        keys += (keys.empty() ? "" : ",") + k;
    sf_log_unverified_once("258-shape", "Cmd 258 answered: Ack " + std::to_string(a.ack) + ", " + std::to_string(a.files.size()) +
                                         " listed, " + std::to_string(a.folders) + " folders, " + std::to_string(a.other_storage) +
                                         " other storage, " + std::to_string(a.unlisted) + " skipped; first entry keys [" + keys +
                                         "]; names " + (a.names_prefixed ? "carry" : "do not carry") + " the /local/ prefix; CreateTime " +
                                         a.create_time_form);
    if (a.ack > 0)
        return pstore::Result::failure(pstore::Status::Failed, "The printer refused to list its files (Ack " + std::to_string(a.ack) + ").");
    out = a.files;
    return pstore::Result::success();
}

pstore::Result Cc1Files::list(std::vector<pstore::FileEntry>& out)
{
    out.clear();
    std::vector<ListedFile> files;
    pstore::Result          r = list_raw(files);
    if (!r.ok())
        return r;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_layers.clear();
    for (ListedFile& f : files) {
        m_layers[f.entry.path] = f.total_layers;
        out.push_back(std::move(f.entry));
    }
    return r;
}

pstore::Result Cc1Files::metadata(const std::string& path, pstore::FileMetadata& out)
{
    out = pstore::FileMetadata();
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_layers.find(path);
    if (it == m_layers.end())
        return pstore::Result::failure(pstore::Status::NotFound, "Not listed: " + path);
    out.valid       = true;
    out.layer_count = it->second;
    return pstore::Result::success();
}

pstore::Result Cc1Files::thumbnail(const std::string&, std::string& bytes)
{
    bytes.clear();
    return pstore::Result::failure(pstore::Status::NotFound, "This printer's file list has no preview pictures.");
}

pstore::Result Cc1Files::remove(const std::string& path)
{
    if (!pstore::valid_path(path))
        return pstore::Result::failure(pstore::Status::NotFound, "Not a file on the printer: " + path);
    // Never the file a job is streaming from (pycentauri refuses it too).
    const PrinterRead st = m_channel->read_state();
    if (!st.reached)
        return pstore::Result::failure(pstore::Status::Offline, "The printer could not be asked what it is printing" +
                                                                    (st.error.empty() ? std::string(".") : " (" + st.error + ")."));
    if (PrintHostDevices::busy_printing(st.state) && same_file(st.filename, path))
        return pstore::Result::failure(pstore::Status::Busy, "The printer is printing this file. Finish or cancel the print first.");

    const Reply r = m_channel->command(CMD_DELETE_FILES, delete_request_data({ path }), 10000);
    if (!r.sent)
        return sf_not_sent(r);
    if (!r.answered) {
        sf_log_unverified_once("259-unanswered", "Cmd 259 (delete) got no answer: " + r.error);
        return pstore::Result::failure(pstore::Status::Failed, "The printer did not answer the delete request (" +
                                                                   (r.error.empty() ? std::string("no answer") : r.error) + ").");
    }
    const DeleteAnswer a = parse_delete_answer(r.frame);
    sf_log_unverified_once("259-shape", "Cmd 259 answered: Ack " + std::to_string(a.ack) + ", ErrData " +
                                         (a.failed.empty() ? std::string("empty or absent") : std::to_string(a.failed.size()) + " entries"));
    if (a.ack > 0)
        return pstore::Result::failure(pstore::Status::Failed, "The printer refused to delete the file (Ack " + std::to_string(a.ack) + ").");
    for (const std::string& failed : a.failed)
        if (same_file(failed, path))
            return pstore::Result::failure(pstore::Status::Failed, "The printer could not delete the file.");

    // The Ack alone is not trusted yet (unverified on this firmware): the file must be gone.
    std::vector<ListedFile> files;
    const pstore::Result    again = list_raw(files);
    if (!again.ok()) {
        BOOST_LOG_TRIVIAL(info) << "[Storage][CC1] delete acknowledged; the list after it failed: " << again.message;
        return pstore::Result::success();
    }
    for (const ListedFile& f : files)
        if (f.entry.path == path) {
            sf_log_unverified_once("259-noop", "Cmd 259 answered Ack " + std::to_string(a.ack) + " but the file is still listed");
            return pstore::Result::failure(pstore::Status::Failed,
                                           "The printer acknowledged the delete, but it still lists the file.");
        }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_layers.erase(path);
    m_confirmed_plan.erase(path);
    return pstore::Result::success();
}

pstore::Result Cc1Files::printer_state(std::string& state, std::string& current_file)
{
    state.clear();
    current_file.clear();
    const PrinterRead st = m_channel->read_state();
    if (!st.reached)
        return pstore::Result::failure(pstore::Status::Offline, "The printer did not say what it is doing" +
                                                                    (st.error.empty() ? std::string(".") : " (" + st.error + ")."));
    state        = st.state;
    current_file = sf_base_name(st.filename);
    return pstore::Result::success();
}

pstore::Result Cc1Files::start_check(const std::string& path, std::string& state, std::string& current_file, std::string& advice)
{
    state.clear();
    current_file.clear();
    advice.clear();
    const PrinterRead st = m_channel->read_printer();
    if (!st.reached)
        return pstore::Result::failure(pstore::Status::Offline, "The printer did not say what it is doing" +
                                                                    (st.error.empty() ? std::string(".") : " (" + st.error + ")."));
    state        = st.state;
    current_file = sf_base_name(st.filename);
    if (PrintHostDevices::busy_printing(state))
        return pstore::Result::success(); // the panel says it is busy, as for any printer
    const StartPlan plan = plan_start(st.status, m_options, m_pc_bed_type, m_printer);
    if (!plan.may_start)
        return pstore::Result::failure(pstore::Status::Failed, plan.refusal);
    advice = plan.advice;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_confirmed_plan[path] = plan.advice;
    return pstore::Result::success();
}

pstore::Result Cc1Files::start_print(const std::string& path)
{
    if (!pstore::valid_path(path) || !pstore::is_printable_name(path))
        return pstore::Result::failure(pstore::Status::NotFound, "Not a G-code file the printer can print: " + path);
    std::string shown;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_confirmed_plan.find(path);
        if (it == m_confirmed_plan.end())
            return pstore::Result::failure(pstore::Status::Failed, "Confirm the print first.");
        shown = it->second;
        m_confirmed_plan.erase(it); // one confirmation, one start
    }
    // Read again just before the start: a print begun meanwhile, a CANVAS loaded since.
    const PrinterRead st = m_channel->read_printer();
    if (!st.reached)
        return pstore::Result::failure(pstore::Status::Offline, "The printer did not say what it is doing" +
                                                                    (st.error.empty() ? std::string(".") : " (" + st.error + ")."));
    const StartPlan plan = plan_start(st.status, m_options, m_pc_bed_type, m_printer);
    if (plan.policy == PrintHostDevices::SlotSendPolicy::UploadOnlyBusy)
        return pstore::Result::failure(pstore::Status::Busy, plan.refusal);
    if (!plan.may_start)
        return pstore::Result::failure(pstore::Status::Failed, plan.refusal);
    if (plan.advice != shown)
        return pstore::Result::failure(pstore::Status::Failed,
                                       "The printer changed since you confirmed (its CANVAS trays or the send choices). Press Print again "
                                       "to see what it will do now.");
    if (plan.extended_info.count("canvasAttached"))
        // As the send dialog logs it, so the owner's test settles what "As sliced" does (design D4).
        BOOST_LOG_TRIVIAL(info) << "CANVAS: slot_map for this print: [] (a start from the printer's storage: every filament as sliced, "
                                   "an empty slot_map is sent)";
    const json  data = start_request_data(plan.extended_info, m_pc_bed_type, path);
    const Reply r    = m_channel->command(CMD_START_PRINT, data, 15000);
    if (!r.sent)
        return sf_not_sent(r);
    if (!r.answered)
        return pstore::Result::failure(pstore::Status::Failed,
                                       "The printer did not answer the start request in time; it may still start. Check the printer.");
    const int ack = r.ack >= 0 ? r.ack : ack_of(r.frame);
    BOOST_LOG_TRIVIAL(info) << "[Storage][CC1] start from storage answered, Ack " << ack;
    if (ack > 0)
        return pstore::Result::failure(ack == 1 ? pstore::Status::Busy : ack == 2 ? pstore::Status::NotFound : pstore::Status::Failed,
                                       start_ack_text(ack));
    return pstore::Result::success();
}

std::shared_ptr<pstore::IPrinterFiles> make_cc1_files(const pstore::Target& target)
{
    if (target.kind != pstore::Target::Kind::ElegooSdcp || target.base_url.empty())
        return nullptr;
    return std::make_shared<Cc1Files>(make_monitor_channel(target.base_url, target.printer_model), target.start_options,
                                      static_cast<int>(BedType::btPC), target.name);
}

// ---- tests ------------------------------------------------------------------------------------------

void reset_logged_for_tests()
{
    std::lock_guard<std::mutex> lock(sf_g_log_mutex);
    sf_g_logged.clear();
    sf_g_lines.clear();
}

std::vector<std::string> logged_for_tests()
{
    std::lock_guard<std::mutex> lock(sf_g_log_mutex);
    return sf_g_lines;
}

} // namespace SdcpFiles
} // namespace Slic3r
