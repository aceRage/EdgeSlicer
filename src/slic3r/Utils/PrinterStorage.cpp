#include "PrinterStorage.hpp"

#include "Http.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace Slic3r {
namespace PrinterStorage {

using json = nlohmann::json;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

bool ends_with_ci(const std::string& name, const char* suffix)
{
    const std::string s = suffix;
    if (name.size() < s.size())
        return false;
    return lower(name.substr(name.size() - s.size())) == s;
}

// The object an answer carries: {"result": X} -> X, anything else as it is.
const json& unwrap(const json& answer)
{
    if (answer.is_object()) {
        auto it = answer.find("result");
        if (it != answer.end())
            return *it;
    }
    return answer;
}

double num(const json& obj, const char* key)
{
    if (!obj.is_object())
        return 0;
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number())
        return 0;
    const double v = it->get<double>();
    return std::isfinite(v) ? v : 0;
}

std::string str(const json& obj, const char* key)
{
    if (!obj.is_object())
        return {};
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string())
        return {};
    return it->get<std::string>();
}

std::string dir_of(const std::string& path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string last_segment(const std::string& path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "a/./b/../c" -> "a/c"; false when it climbs out of the root.
bool normalize_relative(const std::string& in, std::string& out)
{
    std::vector<std::string> parts;
    size_t                   start = 0;
    while (start <= in.size()) {
        size_t end = in.find('/', start);
        if (end == std::string::npos)
            end = in.size();
        const std::string seg = in.substr(start, end - start);
        if (seg == "..") {
            if (parts.empty())
                return false;
            parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        start = end + 1;
    }
    out.clear();
    for (size_t i = 0; i < parts.size(); ++i)
        out += (i ? "/" : "") + parts[i];
    return !out.empty();
}

bool hidden_path(const std::string& path)
{
    size_t start = 0;
    while (start < path.size()) {
        if (path[start] == '.')
            return true;
        const size_t slash = path.find('/', start);
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return false;
}

std::string url_encode_segment(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string        out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char) c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

} // namespace

// ---- parsers ------------------------------------------------------------------------------------

std::vector<FileEntry> parse_file_list(const json& answer)
{
    std::vector<FileEntry> out;
    const json&            list = unwrap(answer);
    if (!list.is_array())
        return out;
    for (const json& item : list) {
        if (!item.is_object())
            continue;
        std::string path = str(item, "path");
        if (path.empty())
            path = str(item, "filename");
        while (!path.empty() && path.front() == '/')
            path.erase(path.begin());
        if (!valid_path(path) || hidden_path(path) || !is_listed_name(path))
            continue;
        FileEntry e;
        e.path     = path;
        e.name     = last_segment(path);
        const double size = num(item, "size");
        e.size     = size > 0 ? (std::uint64_t) size : 0;
        e.modified = (std::int64_t) num(item, "modified");
        out.push_back(std::move(e));
    }
    return out;
}

FileMetadata parse_metadata(const json& answer, const std::string& file_path)
{
    FileMetadata m;
    const json&  r = unwrap(answer);
    if (!r.is_object())
        return m;
    m.valid              = true;
    m.estimated_time_s   = (int) std::lround(num(r, "estimated_time"));
    m.filament_total_mm  = num(r, "filament_total");
    m.filament_weight_g  = num(r, "filament_weight_total");
    m.filament_type      = str(r, "filament_type");
    m.filament_name      = str(r, "filament_name");
    m.layer_height       = num(r, "layer_height");
    m.first_layer_height = num(r, "first_layer_height");
    m.object_height      = num(r, "object_height");
    m.nozzle_diameter    = num(r, "nozzle_diameter");
    m.layer_count        = (int) num(r, "layer_count");
    m.slicer             = str(r, "slicer");
    m.slicer_version     = str(r, "slicer_version");
    if (m.estimated_time_s < 0)
        m.estimated_time_s = 0;
    auto it = r.find("thumbnails");
    if (it != r.end() && it->is_array()) {
        const std::string dir = dir_of(file_path);
        for (const json& t : *it) {
            if (!t.is_object())
                continue;
            // "relative_path" is relative to the file's directory; some front ends (and upstream
            // Orca) also read "thumbnail_path", which is already relative to the root.
            std::string rel  = str(t, "relative_path");
            std::string path;
            if (!rel.empty()) {
                if (!normalize_relative(dir.empty() ? rel : dir + "/" + rel, path))
                    continue;
            } else {
                rel = str(t, "thumbnail_path");
                if (rel.compare(0, 7, "gcodes/") == 0)
                    rel = rel.substr(7);
                if (rel.empty() || !normalize_relative(rel, path))
                    continue;
            }
            if (!valid_path(path))
                continue;
            Thumbnail th;
            th.width  = (int) num(t, "width");
            th.height = (int) num(t, "height");
            th.size   = (std::uint64_t) std::max(0.0, num(t, "size"));
            th.path   = path;
            m.thumbnails.push_back(std::move(th));
        }
    }
    return m;
}

const Thumbnail* pick_thumbnail(const std::vector<Thumbnail>& thumbnails, int want_width)
{
    const Thumbnail* best_above = nullptr; // the smallest that is wide enough
    const Thumbnail* widest     = nullptr;
    for (const Thumbnail& t : thumbnails) {
        if (!widest || t.width > widest->width)
            widest = &t;
        if (t.width >= want_width && (!best_above || t.width < best_above->width))
            best_above = &t;
    }
    return best_above ? best_above : widest;
}

std::string print_state_of(const json& answer, std::string* filename)
{
    const json& r = unwrap(answer);
    if (!r.is_object())
        return {};
    auto status = r.find("status");
    if (status == r.end() || !status->is_object())
        return {};
    auto stats = status->find("print_stats");
    if (stats == status->end() || !stats->is_object())
        return {};
    if (filename)
        *filename = str(*stats, "filename");
    return str(*stats, "state");
}

bool is_busy_state(const std::string& state) { return state == "printing" || state == "paused"; }

bool is_printable_name(const std::string& name)
{
    return ends_with_ci(name, ".gcode") || ends_with_ci(name, ".g") || ends_with_ci(name, ".gco");
}

bool is_listed_name(const std::string& name)
{
    return is_printable_name(name) || ends_with_ci(name, ".bgcode") || ends_with_ci(name, ".3mf");
}

bool valid_path(const std::string& path)
{
    if (path.empty() || path.size() > 1024 || path.front() == '/' || path.back() == '/')
        return false;
    for (unsigned char c : path)
        if (c < 0x20 || c == 0x7f || c == '\\')
            return false;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos)
            end = path.size();
        const std::string seg = path.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == "..")
            return false;
        start = end + 1;
    }
    return true;
}

std::string encode_path(const std::string& path)
{
    std::string out;
    size_t      start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos)
            end = path.size();
        if (start > 0)
            out += '/';
        out += url_encode_segment(path.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

// ---- targets ------------------------------------------------------------------------------------

std::string base_url_of(const std::string& address)
{
    std::string a = address;
    while (!a.empty() && std::isspace((unsigned char) a.back()))
        a.pop_back();
    while (!a.empty() && std::isspace((unsigned char) a.front()))
        a.erase(a.begin());
    if (a.empty())
        return {};
    std::string scheme = "http://";
    const std::string la = lower(a);
    if (la.compare(0, 7, "http://") == 0) {
        a = a.substr(7);
    } else if (la.compare(0, 8, "https://") == 0) {
        scheme = "https://";
        a      = a.substr(8);
    } else if (la.find("://") != std::string::npos) {
        return {}; // ws://, mqtt://: not an address this API answers on
    }
    // The origin only: Moonraker answers at the root whatever page the address points at.
    const size_t cut = a.find_first_of("/?#");
    if (cut != std::string::npos)
        a = a.substr(0, cut);
    // user:password@ never goes into a URL we build.
    const size_t at = a.rfind('@');
    if (at != std::string::npos)
        a = a.substr(at + 1);
    if (a.empty())
        return {};
    // The U1's MQTT address: its HTTP API is on the default port.
    for (const char* mqtt : { ":1884", ":8883" }) {
        const std::string m = mqtt;
        if (a.size() > m.size() && a.compare(a.size() - m.size(), m.size(), m) == 0)
            a = a.substr(0, a.size() - m.size());
    }
    return scheme + a;
}

Target target_for_host(const std::string& host_type, const std::string& address, const std::string& apikey,
                       const std::string& name, const std::string& key)
{
    Target t;
    t.key  = key;
    t.name = name;
    const std::string base = base_url_of(address);
    if (base.empty())
        return t; // Kind::None
    t.name = name.empty() ? base.substr(base.find("://") + 3) : name;
    const std::string ht = lower(host_type);
    if (ht.empty() || ht == "octoprint" || ht == "moonraker" || ht == "moonraker_mqtt") {
        t.kind     = Target::Kind::Moonraker;
        t.base_url = base;
        t.apikey   = apikey;
        return t;
    }
    t.kind = Target::Kind::Unsupported;
    if (ht == "elegoolink")
        t.reason = "This printer is reached through Elegoo Link (SDCP), which has no file API this tab speaks yet.";
    else if (ht == "flashforge")
        t.reason = "Flashforge printers are not supported by the Storage tab.";
    else if (ht == "prusalink" || ht == "prusaconnect")
        t.reason = "PrusaLink and Prusa Connect printers are not supported by the Storage tab.";
    else
        t.reason = "The Storage tab works with Klipper printers that run Moonraker; this printer's connection (" + host_type +
                   ") has no file API it speaks.";
    return t;
}

Target target_for_snapmaker(const std::string& ip, int port, const std::string& name, const std::string& key)
{
    std::string address = ip;
    if (!address.empty() && port > 0 && port != 80)
        address += ":" + std::to_string(port);
    Target t = target_for_host("moonraker", address, std::string(), name, key);
    return t;
}

Target unsupported_target(const std::string& name, const std::string& reason)
{
    Target t;
    t.kind   = Target::Kind::Unsupported;
    t.name   = name;
    t.reason = reason;
    return t;
}

// ---- results ------------------------------------------------------------------------------------

Result result_of(const HttpReply& reply, const std::string& what)
{
    if (reply.status >= 200 && reply.status < 300)
        return Result::success();
    if (reply.status == 0)
        return Result::failure(Status::Offline, "The printer did not answer" + (reply.error.empty() ? std::string() : " (" + reply.error + ")") + ".");
    std::string message;
    try {
        const json j = json::parse(reply.body);
        if (j.is_object() && j.contains("error")) {
            const json& e = j["error"];
            if (e.is_object())
                message = str(e, "message");
            else if (e.is_string())
                message = e.get<std::string>();
        }
    } catch (...) {}
    if (message.empty() && !reply.body.empty() && reply.body.size() < 200 && reply.body.find('<') == std::string::npos)
        message = reply.body;
    if (message.empty())
        message = "HTTP " + std::to_string(reply.status);
    if (reply.status == 401 || reply.status == 403)
        return Result::failure(Status::Unauthorized, "The printer refused to " + what + " (" + message +
                                                         "). Check the API key in the printer's connection settings.");
    if (reply.status == 404)
        return Result::failure(Status::NotFound, "Not found: " + message);
    return Result::failure(Status::Failed, "The printer could not " + what + ": " + message);
}

// ---- the Moonraker backend ----------------------------------------------------------------------

namespace {

bool parse_body(const HttpReply& reply, json& out)
{
    try {
        out = json::parse(reply.body);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

Result MoonrakerFiles::list(std::vector<FileEntry>& out)
{
    out.clear();
    const HttpReply reply = m_transport->request("GET", "/server/files/list?root=gcodes", 16u << 20, 15);
    if (reply.status == 404)
        return Result::failure(Status::NotSupported,
                               "This printer does not offer Moonraker's file API (an OctoPrint server, for instance, is not supported).");
    Result r = result_of(reply, "list its files");
    if (!r.ok())
        return r;
    json j;
    if (!parse_body(reply, j) || !(unwrap(j).is_array()))
        return Result::failure(Status::NotSupported, "This printer's answer is not a Moonraker file list.");
    out = parse_file_list(j);
    return r;
}

Result MoonrakerFiles::metadata(const std::string& path, FileMetadata& out)
{
    out = FileMetadata();
    if (!valid_path(path))
        return Result::failure(Status::NotFound, "Not a file on the printer: " + path);
    const HttpReply reply = m_transport->request("GET", "/server/files/metadata?filename=" + url_encode_segment(path), 1u << 20, 8);
    Result          r     = result_of(reply, "read the file's details");
    if (!r.ok())
        return r;
    json j;
    if (!parse_body(reply, j))
        return Result::failure(Status::Failed, "The printer's answer about " + path + " is not JSON.");
    out = parse_metadata(j, path);
    return r;
}

Result MoonrakerFiles::thumbnail(const std::string& thumbnail_path, std::string& bytes)
{
    bytes.clear();
    if (!valid_path(thumbnail_path))
        return Result::failure(Status::NotFound, "Not a picture on the printer: " + thumbnail_path);
    const HttpReply reply = m_transport->request("GET", "/server/files/gcodes/" + encode_path(thumbnail_path), 4u << 20, 10);
    Result          r     = result_of(reply, "send the preview picture");
    if (r.ok())
        bytes = reply.body;
    return r;
}

Result MoonrakerFiles::remove(const std::string& path)
{
    if (!valid_path(path))
        return Result::failure(Status::NotFound, "Not a file on the printer: " + path);
    const HttpReply reply = m_transport->request("DELETE", "/server/files/gcodes/" + encode_path(path), 1u << 20, 15);
    return result_of(reply, "delete the file");
}

Result MoonrakerFiles::printer_state(std::string& state, std::string& current_file)
{
    state.clear();
    current_file.clear();
    const HttpReply reply = m_transport->request("GET", "/printer/objects/query?print_stats", 1u << 20, 8);
    Result          r     = result_of(reply, "say what it is doing");
    if (!r.ok())
        return r;
    json j;
    if (!parse_body(reply, j))
        return Result::failure(Status::Failed, "The printer's state is not JSON.");
    state = print_state_of(j, &current_file);
    if (state.empty())
        return Result::failure(Status::Failed, "The printer did not report its print state (is Klipper running?).");
    return r;
}

Result MoonrakerFiles::start_print(const std::string& path)
{
    if (!valid_path(path) || !is_printable_name(path))
        return Result::failure(Status::NotFound, "Not a G-code file the printer can print: " + path);
    // Never over a running job: ask first, as the hub's send and reprint do (RemoteSend.cpp).
    std::string state, current;
    Result      r = printer_state(state, current);
    if (!r.ok())
        return r;
    if (is_busy_state(state))
        return Result::failure(Status::Busy, std::string("The printer is ") + (state == "paused" ? "paused mid-print" : "printing") +
                                                 (current.empty() ? std::string() : " (" + current + ")") +
                                                 ". Finish or cancel that job first.");
    const HttpReply reply = m_transport->request("POST", "/printer/print/start?filename=" + url_encode_segment(path), 1u << 20, 30);
    return result_of(reply, "start the print");
}

// ---- Http ---------------------------------------------------------------------------------------

namespace {

class HttpTransport : public Transport
{
public:
    HttpTransport(std::string base, std::string apikey) : m_base(std::move(base)), m_apikey(std::move(apikey)) {}

    HttpReply request(const std::string& method, const std::string& path_and_query, std::size_t size_limit, int timeout_s) override
    {
        HttpReply   out;
        const std::string url = m_base + path_and_query;
        auto        make      = [&method, &url]() {
            if (method == "POST")
                return Http::post(url);
            if (method == "DELETE")
                return Http::del(url);
            return Http::get(url);
        };
        Http http = make();
        http.tls_policy(Http::TlsPolicy::PrintHost) // printer: keep accepting self-signed certificates
            .timeout_connect(std::min(timeout_s, 5))
            .timeout_max(timeout_s)
            .size_limit(size_limit);
        if (!m_apikey.empty())
            http.header("X-Api-Key", m_apikey);
        // A POST needs a body or curl waits for one nobody writes (SnapmakerLan::start_print).
        if (method == "POST")
            http.header("Content-Type", "application/json").set_post_body(std::string("{}"));
        http.on_complete([&out](std::string body, unsigned status) {
                out.status = status;
                out.body   = std::move(body);
            })
            .on_error([&out](std::string body, std::string error, unsigned status) {
                out.status = status;
                out.body   = std::move(body);
                out.error  = std::move(error);
            })
            .perform_sync();
        return out;
    }

private:
    std::string m_base;
    std::string m_apikey;
};

} // namespace

std::shared_ptr<Transport> make_http_transport(const Target& target)
{
    return std::make_shared<HttpTransport>(target.base_url, target.apikey);
}

std::shared_ptr<IPrinterFiles> make_files(const Target& target)
{
    if (!target.supported())
        return nullptr;
    return std::make_shared<MoonrakerFiles>(make_http_transport(target));
}

// ---- display ------------------------------------------------------------------------------------

std::string format_duration(int seconds)
{
    if (seconds <= 0)
        return {};
    if (seconds < 60)
        return "< 1m";
    const int days = seconds / 86400, hours = (seconds % 86400) / 3600, minutes = (seconds % 3600) / 60;
    char      buf[64];
    if (days > 0)
        std::snprintf(buf, sizeof(buf), "%dd %dh", days, hours);
    else if (hours > 0)
        std::snprintf(buf, sizeof(buf), "%dh %dm", hours, minutes);
    else
        std::snprintf(buf, sizeof(buf), "%dm", minutes);
    return buf;
}

std::string format_size(std::uint64_t bytes)
{
    char buf[64];
    if (bytes < 1024)
        std::snprintf(buf, sizeof(buf), "%u B", (unsigned) bytes);
    else if (bytes < 1024ull * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    else
        std::snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

std::string format_filament(double mm, double grams)
{
    char        buf[64];
    std::string out;
    if (mm > 0) {
        std::snprintf(buf, sizeof(buf), "%.2f m", mm / 1000.0);
        out = buf;
    }
    if (grams > 0) {
        std::snprintf(buf, sizeof(buf), grams < 10 ? "%.1f g" : "%.0f g", grams);
        out += (out.empty() ? "" : ", ") + std::string(buf);
    }
    return out;
}

} // namespace PrinterStorage
} // namespace Slic3r
