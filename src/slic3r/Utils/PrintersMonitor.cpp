#include "PrintersMonitor.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

namespace Slic3r {
namespace PrintersMonitor {

using json = nlohmann::json;

static bool starts_with(const std::string& s, const char* prefix)
{
    const size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool valid_printer_id(const std::string& id)
{
    if (id.empty() || id.size() > 200)
        return false;
    for (unsigned char c : id) {
        if (c <= 0x20 || c >= 0x7f) return false; // control, space, non-ASCII
        if (c == '"' || c == '\'' || c == '\\' || c == '<' || c == '>' || c == '`') return false;
    }
    return true;
}

bool valid_action(const std::string& action) { return action == "pause" || action == "resume" || action == "stop"; }

Message parse_message(const std::string& msg)
{
    Message m;
    if (msg == "printers_get") { m.kind = Message::Kind::Get; return m; }
    if (msg == "printers_groups_get") { m.kind = Message::Kind::Groups; return m; }
    if (msg == "printers_tasks") { m.kind = Message::Kind::Tasks; return m; }
    if (starts_with(msg, "printers_thumb:")) {
        const std::string id = msg.substr(15);
        if (!valid_archive_id(id)) return m;
        m.id   = id;
        m.kind = Message::Kind::Thumb;
        return m;
    }
    if (starts_with(msg, "printers_job:")) {
        const std::string n = msg.substr(13);
        if (n.empty() || n.size() > 9 || !std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            return m;
        m.job  = std::stoi(n);
        m.kind = m.job > 0 ? Message::Kind::Job : Message::Kind::Invalid;
        return m;
    }
    if (starts_with(msg, "printers_rename:")) {
        std::string id, name;
        if (!parse_rename(msg.substr(16), id, name)) return m;
        m.id   = id;
        m.name = name;
        m.kind = Message::Kind::Rename;
        return m;
    }
    if (starts_with(msg, "printers_remove:")) {
        const std::string id = msg.substr(16);
        if (!valid_printer_id(id)) return m;
        m.id   = id;
        m.kind = Message::Kind::Remove;
        return m;
    }
    if (starts_with(msg, "printers_forget:")) {
        const std::string id = msg.substr(16);
        if (!valid_printer_id(id)) return m;
        m.id   = id;
        m.kind = Message::Kind::Forget;
        return m;
    }
    if (starts_with(msg, "printers_open:")) {
        const std::string id = msg.substr(14);
        if (!valid_printer_id(id)) return m;
        m.id   = id;
        m.kind = Message::Kind::Open;
        return m;
    }
    if (starts_with(msg, "printers_control:")) {
        // <action>:<0|1>:<id>
        const std::string rest = msg.substr(17);
        const size_t      a    = rest.find(':');
        if (a == std::string::npos) return m;
        const size_t b = rest.find(':', a + 1);
        if (b == std::string::npos) return m;
        const std::string action  = rest.substr(0, a);
        const std::string confirm = rest.substr(a + 1, b - a - 1);
        const std::string id      = rest.substr(b + 1);
        if (!valid_action(action) || (confirm != "0" && confirm != "1") || !valid_printer_id(id)) return m;
        m.action  = action;
        m.confirm = confirm == "1";
        m.id      = id;
        m.kind    = Message::Kind::Control;
        return m;
    }
    return m;
}

static bool is_identifier(const std::string& s)
{
    if (s.empty() || s.size() > 64) return false;
    if (std::isdigit((unsigned char) s[0])) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '$'; });
}

std::string script_call(const std::string& fn, const json& arg)
{
    if (!is_identifier(fn))
        return std::string();
    // ensure_ascii: U+2028 / U+2029 and anything else outside ASCII become \uXXXX escapes.
    // replace: a name with broken UTF-8 must not throw here.
    const std::string a = arg.dump(-1, ' ', true, json::error_handler_t::replace);
    return "if (window." + fn + ") window." + fn + "(" + a + ");";
}

json page_payload(int status, const std::string& body, long long now_ms)
{
    json out;
    out["at"] = now_ms;
    out["printers"] = json::array();
    if (status != 200) {
        out["ok"] = false;
        std::string why;
        try {
            const json e = json::parse(body);
            if (e.is_object() && e.contains("error") && e["error"].is_string()) why = e["error"].get<std::string>();
        } catch (...) {}
        out["error"] = why.empty() ? std::string("the printer list is not available right now") : why;
        return out;
    }
    try {
        json j = json::parse(body);
        if (j.is_object() && j.contains("printers") && j["printers"].is_array()) {
            for (json& p : j["printers"])
                if (p.is_object() && p.contains("id") && p["id"].is_string() && valid_printer_id(p["id"].get<std::string>()))
                    out["printers"].push_back(std::move(p));
            out["ok"] = true;
            return out;
        }
    } catch (...) {}
    out["ok"]    = false;
    out["error"] = "the printer list could not be read";
    return out;
}

void add_leftovers(json& payload, int hub_status, const std::string& hub_body)
{
    if (hub_status != 200 || !payload.is_object() || !payload.value("ok", false) || !payload.contains("printers") ||
        !payload["printers"].is_array())
        return;
    json hub;
    try { hub = json::parse(hub_body); } catch (...) { return; }
    if (!hub.is_object() || !hub.contains("printers") || !hub["printers"].is_array()) return;
    std::set<std::string> have;
    for (const json& p : payload["printers"])
        if (p.is_object() && p.contains("id") && p["id"].is_string()) have.insert(p["id"].get<std::string>());
    for (const json& row : hub["printers"]) {
        if (!row.is_object() || !row.contains("id") || !row["id"].is_string()) continue;
        if (!(row.contains("configured") && row["configured"].is_boolean() && !row["configured"].get<bool>())) continue;
        const std::string id = row["id"].get<std::string>();
        if (!valid_printer_id(id) || have.count(id)) continue;
        json p = row;
        p["leftover"] = true;
        p["online"]   = false;
        for (const char* k : { "can_pause", "can_resume", "can_stop", "can_print", "can_upload", "controls", "print_error" })
            p.erase(k);
        if (row.contains("age_s") && row["age_s"].is_number()) p["seen_s"] = row["age_s"];
        have.insert(id);
        payload["printers"].push_back(std::move(p));
    }
}

// ---- "Remove printer" ----

static std::string str_field(const json& o, const char* k)
{
    return o.is_object() && o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string();
}

std::vector<std::string> RemovalPlan::stores() const
{
    std::vector<std::string> out;
    if (!ph_device_id.empty()) out.push_back("print_host_devices");
    if (!sm_id.empty() || !sm_ip.empty()) out.push_back("snapmaker_lan");
    if (app_config_devices) out.push_back("app_config_devices");
    if (!bambu_dev_id.empty()) out.push_back("bambu_lan");
    if (tombstone) out.push_back("tombstone");
    if (hub_row) out.push_back("hub_printers");
    return out;
}

RemovalPlan removal_plan(const json& row)
{
    RemovalPlan plan;
    const std::string id   = str_field(row, "id");
    const std::string kind = str_field(row, "kind");
    if (id.empty() || !valid_printer_id(id)) {
        plan.hub_row = false;
        plan.reason  = "This printer has no id the slicer can remove.";
        return plan;
    }
    // A row only the hub remembers: the slicer has nothing of it any more, only the hub row is left.
    if (row.is_object() && row.contains("leftover") && row["leftover"].is_boolean() && row["leftover"].get<bool>()) {
        plan.mode        = RemovalPlan::Mode::Remove;
        plan.reason_kind = "leftover";
        return plan;
    }
    if (id == "host") {
        plan.hub_row     = false;
        plan.reason_kind = "preset_host";
        plan.reason  = "This printer is the selected printer preset's own print host. Clear its address in the preset "
                       "(the printer settings' connection / Physical Printer field) to remove it.";
        return plan;
    }
    if (id == "connect" || kind == "connect") {
        plan.hub_row     = false;
        plan.reason_kind = "device_connection";
        plan.reason  = "This is the Device tab's current connection. Disconnect it on the Device tab.";
        return plan;
    }
    if (id.compare(0, 3, "ph:") == 0) {
        plan.mode         = RemovalPlan::Mode::Remove;
        plan.reason_kind  = "print_host_device";
        plan.ph_device_id = str_field(row, "device_id");
        if (plan.ph_device_id.empty()) plan.ph_device_id = id.substr(3);
        plan.ph_model_key = str_field(row, "model_key");
        // Nothing re-adds a print-host device by itself (the preset migration remembers what it
        // imported), but the slicer's report skips a tombstoned id too, so a cache that still holds
        // the device until its next read cannot show it again. By id only: a device added again
        // gets a new id.
        plan.tombstone    = true;
        return plan;
    }
    if (id.compare(0, 3, "sm:") == 0) {
        plan.mode               = RemovalPlan::Mode::Remove;
        plan.reason_kind        = "lan_snapmaker";
        plan.sm_id              = id.substr(3);
        plan.sm_ip              = str_field(row, "ip");
        plan.app_config_devices = true;
        plan.tombstone          = true;
        return plan;
    }
    if (kind == "bambu") {
        const bool lan = row.contains("lan_mode") && row["lan_mode"].is_boolean() && row["lan_mode"].get<bool>();
        if (!lan) {
            plan.hub_row     = false;
            plan.reason_kind = "bambu_cloud";
            plan.reason  = "This printer is bound to your Bambu Lab account, which is where the list comes from. Unbind it "
                           "on the Device tab (or in Bambu Handy) to remove it.";
            return plan;
        }
        plan.mode         = RemovalPlan::Mode::Logout;
        plan.reason_kind  = "bambu_lan";
        plan.bambu_dev_id = id;
        // A logged-out LAN printer that still announces itself on the LAN would otherwise be
        // listed again as a discovered printer; the report skips it until it is logged in again.
        plan.tombstone    = true;
        return plan;
    }
    // Any other kind the slicer lists: nothing to delete but the remembered row.
    plan.mode        = RemovalPlan::Mode::Remove;
    plan.reason_kind = "other";
    return plan;
}

json removal_json(const json& row)
{
    const RemovalPlan plan = removal_plan(row);
    json r;
    r["mode"] = plan.mode == RemovalPlan::Mode::Remove ? "remove" : plan.mode == RemovalPlan::Mode::Logout ? "logout" : "explain";
    if (!plan.reason.empty()) r["reason"] = plan.reason;
    if (!plan.reason_kind.empty()) r["kind"] = plan.reason_kind;
    return r;
}

void annotate_removal(json& printers)
{
    if (!printers.is_array()) return;
    for (json& p : printers)
        if (p.is_object()) p["remove"] = removal_json(p);
}

static bool names(const json& e, const std::string& row_id, const std::string& ip)
{
    return (!row_id.empty() && str_field(e, "id") == row_id) || (!ip.empty() && str_field(e, "ip") == ip);
}

bool tombstoned(const json& store, const std::string& row_id, const std::string& ip)
{
    if (!store.is_object() || !store.contains("removed") || !store["removed"].is_array()) return false;
    for (const json& e : store["removed"])
        if (names(e, row_id, ip)) return true;
    return false;
}

void add_tombstone(json& store, const std::string& row_id, const std::string& ip, long long now_s)
{
    if (!store.is_object()) store = json::object();
    if (!store.contains("removed") || !store["removed"].is_array()) store["removed"] = json::array();
    clear_tombstone(store, row_id, ip);
    json e;
    e["id"] = row_id;
    if (!ip.empty()) e["ip"] = ip;
    e["at"] = now_s;
    store["removed"].push_back(e);
    // A long-lived PC that removes printers for years keeps the newest few hundred.
    while (store["removed"].size() > 256) store["removed"].erase(store["removed"].begin());
}

bool clear_tombstone(json& store, const std::string& row_id, const std::string& ip)
{
    if (!store.is_object() || !store.contains("removed") || !store["removed"].is_array()) return false;
    json kept    = json::array();
    bool removed = false;
    for (const json& e : store["removed"]) {
        if (names(e, row_id, ip)) { removed = true; continue; }
        kept.push_back(e);
    }
    store["removed"] = kept;
    return removed;
}

int filter_tombstoned(json& printers, const json& store)
{
    if (!printers.is_array() || !store.is_object() || !store.contains("removed") || !store["removed"].is_array() ||
        store["removed"].empty())
        return 0;
    json kept = json::array();
    int  gone = 0;
    for (json& p : printers) {
        const std::string id = str_field(p, "id");
        // The address only counts for a LAN Snapmaker: that is what its discovery keys on.
        const std::string ip = id.compare(0, 3, "sm:") == 0 ? str_field(p, "ip") : std::string();
        if (!id.empty() && tombstoned(store, id, ip)) { ++gone; continue; }
        kept.push_back(std::move(p));
    }
    printers = std::move(kept);
    return gone;
}

json load_tombstones(const std::string& path) { return load_store_file(path); }
bool save_tombstones(const std::string& path, const json& store) { return save_store_file(path, store); }

json load_store_file(const std::string& path)
{
    try {
        boost::nowide::ifstream in(path, std::ios::binary);
        if (!in.good()) return json::object();
        json j;
        in >> j;
        return j.is_object() ? j : json::object();
    } catch (...) {}
    return json::object();
}

bool save_store_file(const std::string& path, const json& store)
{
    try {
        const std::string tmp = path + ".tmp";
        {
            boost::nowide::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.good()) return false;
            out << store.dump(1);
        }
        boost::system::error_code ec;
        boost::filesystem::rename(tmp, path, ec); // replaces an existing file
        return !ec;
    } catch (...) {}
    return false;
}

// ---- "Rename" ----

std::string clean_alias(const std::string& name)
{
    std::string out;
    for (unsigned char c : name)
        if (c >= 0x20 && c != 0x7f) out += (char) c; // control characters go; UTF-8 bytes stay
    size_t a = 0, b = out.size();
    while (a < b && out[a] == ' ') ++a;
    while (b > a && out[b - 1] == ' ') --b;
    out = out.substr(a, b - a);
    // At most 64 characters, counted as UTF-8 code points, and never more than 160 bytes.
    size_t bytes = 0, chars = 0;
    while (bytes < out.size() && chars < 64) {
        size_t len = 1;
        const unsigned char c = (unsigned char) out[bytes];
        if (c >= 0xF0) len = 4; else if (c >= 0xE0) len = 3; else if (c >= 0xC0) len = 2;
        if (bytes + len > out.size() || bytes + len > 160) break;
        bytes += len;
        ++chars;
    }
    out.resize(bytes);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool set_alias(json& store, const std::string& row_id, const std::string& name)
{
    if (!valid_printer_id(row_id)) return false;
    if (!store.is_object()) store = json::object();
    if (!store.contains("aliases") || !store["aliases"].is_object()) store["aliases"] = json::object();
    const std::string clean = clean_alias(name);
    if (clean.empty()) store["aliases"].erase(row_id);
    else store["aliases"][row_id] = clean;
    return true;
}

std::string alias_of(const json& store, const std::string& row_id)
{
    if (!store.is_object() || !store.contains("aliases") || !store["aliases"].is_object()) return "";
    const json& a = store["aliases"];
    return a.contains(row_id) && a[row_id].is_string() ? a[row_id].get<std::string>() : std::string();
}

void apply_aliases(json& printers, const json& store)
{
    if (!printers.is_array()) return;
    for (json& p : printers) {
        if (!p.is_object() || !p.contains("id") || !p["id"].is_string()) continue;
        const std::string alias = alias_of(store, p["id"].get<std::string>());
        if (alias.empty()) continue;
        if (!p.contains("reported_name")) p["reported_name"] = str_field(p, "name");
        p["name"]        = alias;
        p["custom_name"] = alias;
    }
}

bool parse_rename(const std::string& payload, std::string& id, std::string& name)
{
    try {
        const json j = json::parse(payload);
        if (!j.is_object() || !j.contains("id") || !j["id"].is_string()) return false;
        id = j["id"].get<std::string>();
        if (!valid_printer_id(id)) return false;
        name = j.contains("name") && j["name"].is_string() ? clean_alias(j["name"].get<std::string>()) : std::string();
        return true;
    } catch (...) {}
    return false;
}

std::map<std::string, Target> targets_of(const json& printers)
{
    std::map<std::string, Target> out;
    if (!printers.is_array()) return out;
    for (const json& p : printers) {
        if (!p.is_object() || !p.contains("id") || !p["id"].is_string()) continue;
        const std::string id = p["id"].get<std::string>();
        if (!valid_printer_id(id) || out.count(id)) continue;
        Target t;
        if (p.contains("kind") && p["kind"].is_string()) t.kind = p["kind"].get<std::string>();
        if (p.contains("url") && p["url"].is_string()) t.url = p["url"].get<std::string>();
        t.device_page = uses_device_page(p);
        out.emplace(id, t);
    }
    return out;
}

bool is_web_url(const std::string& url)
{
    std::string rest;
    if (starts_with(url, "http://")) rest = url.substr(7);
    else if (starts_with(url, "https://")) rest = url.substr(8);
    else return false;
    if (rest.empty() || rest[0] == '/' || rest[0] == ':' || url.size() > 2048) return false;
    for (unsigned char c : url)
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\'' || c == '\\' || c == '<' || c == '>' || c == '`') return false;
    return true;
}

OpenWay open_way(const Target& t, bool bambu_monitor_shown, bool printer_view_shown)
{
    // The shared device page needs no web address of the printer's own, only the Device tab's view.
    if (t.device_page && printer_view_shown)
        return OpenWay::DevicePage;
    if (t.kind == "bambu")
        return bambu_monitor_shown ? OpenWay::BambuMonitor : OpenWay::NotHere;
    if (!is_web_url(t.url))
        return OpenWay::NotHere;
    return printer_view_shown ? OpenWay::PrinterWebView : OpenWay::Browser;
}

std::string not_here_reason(const Target& t)
{
    if (t.kind == "bambu")
        return "The Device tab shows Bambu Lab printers while a Bambu Lab printer preset is selected. Select one, then try again.";
    return "This printer has no web page to open.";
}

bool uses_device_page(const json& row)
{
    if (!row.is_object()) return false;
    if (row.contains("device_page") && row["device_page"].is_boolean() && row["device_page"].get<bool>()) return true;
    return row.contains("cc2") && row["cc2"].is_object();
}

std::string device_page_path(const std::string& id, bool dark)
{
    if (!valid_printer_id(id)) return std::string();
    return std::string("/web/orca/device.html?embed=1&theme=") + (dark ? "dark" : "light") + "&id=" + url_encode_segment(id);
}

std::string device_page_id(const std::string& url)
{
    const size_t page = url.find("/web/orca/device.html?");
    if (page == std::string::npos) return std::string();
    const size_t q = url.find('?', page);
    std::string  query = url.substr(q + 1);
    const size_t hash  = query.find('#');
    if (hash != std::string::npos) query = query.substr(0, hash);
    size_t pos = 0;
    while (pos <= query.size()) {
        const size_t amp  = query.find('&', pos);
        const std::string kv = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (kv.compare(0, 3, "id=") == 0) {
            std::string out;
            const std::string v = kv.substr(3);
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i] == '%' && i + 2 < v.size() && std::isxdigit((unsigned char) v[i + 1]) && std::isxdigit((unsigned char) v[i + 2])) {
                    out += (char) std::stoi(v.substr(i + 1, 2), nullptr, 16);
                    i += 2;
                } else {
                    out += v[i];
                }
            }
            return valid_printer_id(out) ? out : std::string();
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return std::string();
}

bool valid_archive_id(const std::string& id)
{
    if (id.empty() || id.size() > 128 || id[0] == '.') return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-'; });
}

static std::string lower(std::string s)
{
    for (char& c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

std::string url_encode_segment(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out += (char) c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

CoverIndex index_covers(const std::vector<std::pair<std::string, std::string>>& vendor_files)
{
    static const std::string suffix = "_cover.png";
    CoverIndex out;
    for (const auto& vf : vendor_files) {
        const std::string& f = vf.second;
        if (vf.first.empty() || f.size() <= suffix.size() || lower(f.substr(f.size() - suffix.size())) != suffix) continue;
        const std::string model = lower(f.substr(0, f.size() - suffix.size()));
        // The first vendor folder wins (several vendors ship the same generic names).
        out.emplace(model, "/profiles/" + url_encode_segment(vf.first) + "/" + url_encode_segment(f));
    }
    return out;
}

std::string cover_for(const CoverIndex& index, const std::string& model_name)
{
    if (model_name.empty()) return "";
    auto it = index.find(lower(model_name));
    return it == index.end() ? std::string() : it->second;
}

std::string short_model_name(const std::string& kind, const std::string& model_name, const std::string& model_code)
{
    if (model_name.empty()) return model_code;
    static const std::string bambu = "Bambu Lab ";
    if (kind == "bambu" && model_name.size() > bambu.size() && model_name.compare(0, bambu.size(), bambu) == 0)
        return model_name.substr(bambu.size());
    return model_name;
}

std::string row_job(const json& row)
{
    if (!row.is_object()) return "";
    std::string job;
    if (row.contains("task") && row["task"].is_string()) job = row["task"].get<std::string>();
    if (job.empty() && row.contains("subtask_name") && row["subtask_name"].is_string()) job = row["subtask_name"].get<std::string>();
    return job;
}

std::map<std::string, std::string> pick_thumbnails(const std::vector<ArchiveEntry>& entries, const std::map<std::string, std::string>& jobs)
{
    std::map<std::string, std::pair<long long, std::string>> best;
    for (const ArchiveEntry& e : entries) {
        if (e.printer_id.empty() || !valid_archive_id(e.record_id)) continue;
        const auto job = jobs.find(e.printer_id);
        if (job == jobs.end() || job->second.empty()) continue;
        if (e.sent_name != job->second && e.file != job->second) continue;
        auto& b = best[e.printer_id];
        if (b.second.empty() || e.time > b.first) b = { e.time, e.record_id };
    }
    std::map<std::string, std::string> out;
    for (const auto& kv : best) out[kv.first] = kv.second.second;
    return out;
}

void enrich_rows(json& printers, const CoverIndex& covers, const std::map<std::string, std::string>& thumbs)
{
    if (!printers.is_array()) return;
    for (json& p : printers) {
        if (!p.is_object()) continue;
        const std::string kind = p.contains("kind") && p["kind"].is_string() ? p["kind"].get<std::string>() : std::string();
        const std::string code = p.contains("model") && p["model"].is_string() ? p["model"].get<std::string>() : std::string();
        std::string       name = p.contains("model_name") && p["model_name"].is_string() ? p["model_name"].get<std::string>() : std::string();
        if (name.empty() && kind != "bambu") name = code; // a Bambu model code is not a name
        p["model_short"] = short_model_name(kind, name, code);
        const std::string pic = cover_for(covers, name);
        if (!pic.empty()) p["picture"] = pic;
        const std::string id = p.contains("id") && p["id"].is_string() ? p["id"].get<std::string>() : std::string();
        auto t = thumbs.find(id);
        if (t != thumbs.end()) p["thumb_id"] = t->second;
    }
}

// ---- groups ----

static std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char) s[a])) ++a;
    while (b > a && std::isspace((unsigned char) s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<Group> parse_groups(const std::string& stored)
{
    std::vector<Group> out;
    if (trim(stored).empty()) return out;
    json j;
    try { j = json::parse(stored); } catch (...) { return out; }
    if (j.is_object() && j.contains("groups")) j = j["groups"];
    if (!j.is_array()) return out;
    std::set<std::string> seen;
    for (const json& g : j) {
        if (!g.is_object() || !g.contains("id") || !g["id"].is_string()) continue;
        Group grp;
        grp.id = g["id"].get<std::string>();
        if (!valid_printer_id(grp.id) || grp.id == "all" || seen.count(grp.id)) continue;
        if (g.contains("name") && g["name"].is_string()) grp.name = trim(g["name"].get<std::string>());
        if (grp.name.size() > 64) grp.name = grp.name.substr(0, 64);
        if (grp.name.empty()) grp.name = grp.id;
        std::set<std::string> members;
        if (g.contains("printers") && g["printers"].is_array())
            for (const json& p : g["printers"])
                if (p.is_string() && valid_printer_id(p.get<std::string>()) && members.insert(p.get<std::string>()).second)
                    grp.printers.push_back(p.get<std::string>());
        seen.insert(grp.id);
        out.push_back(std::move(grp));
    }
    return out;
}

std::string save_groups(const std::vector<Group>& groups)
{
    json arr = json::array();
    for (const Group& g : groups)
        arr.push_back({ { "id", g.id }, { "name", g.name }, { "printers", g.printers } });
    return json { { "version", 1 }, { "groups", arr } }.dump();
}

std::vector<Group> seed_from_multi_devices(const std::vector<std::string>& dev_ids)
{
    Group g;
    g.id   = "multi-device";
    g.name = "Multi-device";
    std::set<std::string> seen;
    for (const std::string& raw : dev_ids) {
        const std::string id = trim(raw);
        if (valid_printer_id(id) && seen.insert(id).second) g.printers.push_back(id);
    }
    if (g.printers.empty()) return {};
    return { g };
}

json groups_json(const std::vector<Group>& groups)
{
    json arr = json::array();
    arr.push_back({ { "id", "all" }, { "name", "All printers" }, { "all", true }, { "printers", json::array() } });
    for (const Group& g : groups)
        arr.push_back({ { "id", g.id }, { "name", g.name }, { "all", false }, { "printers", g.printers } });
    return arr;
}

} // namespace PrintersMonitor
} // namespace Slic3r
