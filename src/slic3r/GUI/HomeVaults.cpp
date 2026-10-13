#include "HomeVaults.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "HomeDownload.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/UntrustedInput.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/LibraryIndex.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <random>
#include <sstream>
#include <thread>

#include <wx/secretstore.h>
#include <wx/stdpaths.h>
#include <wx/textdlg.h>
#include <wx/dirdlg.h>
#include <wx/utils.h>

namespace Slic3r {
namespace GUI {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

static constexpr const char* VAULTS_KEY        = "home_vaults";
static constexpr const char* HTTP_OK_KEY       = "home_vault_http_ok";
static constexpr const char* DOWNLOADS_KEY     = "vault_downloads_dir";
static constexpr const char* DOWNLOADS_LIB_KEY = "vault_downloads_library";
static constexpr size_t      LIST_LIMIT        = 32u * 1024 * 1024; // one page of a listing
static constexpr size_t      CACHE_LIMIT       = 64u * 1024 * 1024;

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

static std::string u8(const fs::path& p) { return boost::nowide::narrow(p.wstring()); }
static fs::path    path_of(const std::string& utf8) { return fs::path(boost::nowide::widen(utf8)); }

// One HTTP request for the engine, with the slicer's own client (TLS checked for internet hosts;
// a device on the local network may use its own certificate, as print hosts do). None of the app's
// own headers go along. Redirects are not followed here: Vaults::fetch() follows them and decides,
// per hop, whether the vault's credentials go along. A request with a sink streams its 2xx body there.
static Vaults::Response http_call(const Vaults::Request& req, size_t limit, int timeout_seconds)
{
    Vaults::Response out;
    if (Vaults::address_check(req.url.substr(0, req.url.find_first_of("?#")), nullptr) == Vaults::Transport::Refused) {
        out.error = "address not allowed";
        return out;
    }
    std::string raw_headers;
    auto        http = req.method == "GET" ? Http::get(req.url) : Http::custom(req.method, req.url);
    http.clear_headers().follow_redirects(false).timeout_connect(15).timeout_max(timeout_seconds).size_limit(limit);
    for (const auto& [k, v] : req.headers)
        http.header(k, v);
    if (!req.body.empty())
        http.set_post_body(req.body);
    if (req.sink)
        http.on_body([&req, &out](unsigned status, const char* data, size_t size) {
            if (status >= 200 && status < 300)
                return req.sink(data, size);
            if (out.body.size() < 64 * 1024) // an error page: kept for the message
                out.body.append(data, std::min(size, 64 * 1024 - out.body.size()));
            return true;
        });
    http.on_header_callback([&raw_headers](std::string h) { raw_headers = std::move(h); })
        .on_complete([&out](std::string body, unsigned status) {
            out.status = int(status);
            if (!body.empty())
                out.body = std::move(body);
        })
        .on_error([&out](std::string body, std::string error, unsigned status) {
            out.status = int(status);
            if (!body.empty())
                out.body = std::move(body);
            if (status == 0)
                out.error = error;
        })
        .perform_sync();
    const size_t       last = raw_headers.rfind("HTTP/");
    std::istringstream lines(last == std::string::npos ? raw_headers : raw_headers.substr(last));
    std::string        line;
    while (std::getline(lines, line)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
            value.pop_back();
        value.erase(0, value.find_first_not_of(' '));
        out.headers[lower(line.substr(0, colon))] = value;
    }
    return out;
}

static const Vaults::HttpFn LIST_HTTP = [](const Vaults::Request& q) { return http_call(q, LIST_LIMIT, 120); };
static const Vaults::HttpFn FILE_HTTP = [](const Vaults::Request& q) {
    return http_call(q, untrusted::MODEL_DOWNLOAD_SIZE_LIMIT + 1024 * 1024, 1800);
};

static bool write_atomic(const fs::path& path, const std::string& data)
{
    boost::system::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        boost::nowide::ofstream f(u8(tmp).c_str(), std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f.write(data.data(), std::streamsize(data.size()));
        if (!f)
            return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

static std::string read_file(const fs::path& path, size_t limit)
{
    boost::system::error_code ec;
    const uintmax_t           size = fs::file_size(path, ec);
    if (ec || size == 0 || size > limit)
        return std::string();
    boost::nowide::ifstream f(u8(path).c_str(), std::ios::binary);
    std::stringstream       ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string key_of(const std::string& vault, const std::string& file)
{
    return Library::entry_id("vault\x1f" + vault + "\x1f" + file);
}

static std::string type_of(const std::string& name)
{
    const std::string l = lower(name);
    const size_t      dot = l.rfind('.');
    return dot == std::string::npos ? std::string() : l.substr(dot + 1);
}

// The built-in vault templates shipped in resources/vaults (for now: Synology). Read once.
static const json& builtins()
{
    static json list = [] {
        json out = json::array();
        for (const char* id : {"synology"}) {
            const std::string text = read_file(fs::path(resources_dir()) / "vaults" / (std::string(id) + ".json"), 64 * 1024);
            try {
                json t = json::parse(text);
                if (t.value("format", std::string()) != "edgeslicer-vault-template" || !t.contains("spec") || !t["spec"].is_object())
                    continue;
                t["spec"]["builtin"] = id;
                t["id"]              = id;
                out.push_back(t);
            } catch (...) {
                BOOST_LOG_TRIVIAL(warning) << "HomeVaults: the built-in vault template " << id << " is unreadable";
            }
        }
        return out;
    }();
    return list;
}

// ------------------------------------------------------------------------------ setup ----

HomeVaults::HomeVaults(wxWindow* parent, SendFn send, std::function<void()> library_changed, std::function<void(const std::string&)> open_file)
    : m_parent(parent), m_send(std::move(send)), m_library_changed(std::move(library_changed)), m_open_file(std::move(open_file))
{
    load();
}

HomeVaults::~HomeVaults()
{
    *m_alive  = false;
    *m_cancel = true;
}

std::string HomeVaults::default_downloads_dir()
{
    return u8(path_of(into_u8(wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Downloads))) / "EdgeSlicer Vaults");
}

std::string HomeVaults::downloads_dir()
{
    const std::string set = wxGetApp().app_config->get(DOWNLOADS_KEY);
    return set.empty() ? default_downloads_dir() : set;
}

std::string HomeVaults::downloads_dir_of(const Vaults::Spec& spec) const
{
    return spec.downloads_dir.empty() ? downloads_dir() : spec.downloads_dir;
}

void HomeVaults::load()
{
    json list;
    try {
        list = json::parse(wxGetApp().app_config->get(VAULTS_KEY));
    } catch (...) {
        return;
    }
    if (!list.is_array())
        return;
    std::set<std::string> ids;
    for (const json& j : list) {
        try {
            Vault v;
            v.spec = Vaults::spec_from_json(j);
            if (!v.spec.id.empty() && ids.insert(v.spec.id).second)
                m_vaults.push_back(std::move(v));
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "HomeVaults: a saved vault is not usable: " << e.what();
        }
    }
}

void HomeVaults::save_specs()
{
    json list = json::array();
    for (const Vault& v : m_vaults)
        list.push_back(Vaults::spec_to_json(v.spec));
    wxGetApp().app_config->set(VAULTS_KEY, list.dump());
}

HomeVaults::Vault* HomeVaults::find(const std::string& id)
{
    for (Vault& v : m_vaults)
        if (v.spec.id == id)
            return &v;
    return nullptr;
}

std::string HomeVaults::cache_dir(const std::string& id) const { return u8(fs::path(data_dir()) / "vaults" / id); }

void HomeVaults::ensure_cache(Vault& v)
{
    if (v.cache_loaded)
        return;
    v.cache_loaded         = true;
    const std::string text = read_file(path_of(cache_dir(v.spec.id)) / "files.json", CACHE_LIMIT);
    if (text.empty())
        return;
    try {
        const json j = json::parse(text);
        v.synced_at  = j.value("synced_at", int64_t(0));
        v.partial    = j.value("partial", false);
        v.last_error = j.value("last_error", std::string());
        if (j.contains("files") && j["files"].is_array())
            for (const json& f : j["files"])
                if (Vaults::File file = Vaults::file_from_json(f); !file.id.empty() && Vaults::is_listed_file(file.name))
                    v.files.push_back(std::move(file));
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "HomeVaults: the cache of vault " << v.spec.id << " is unreadable, starting over";
    }
}

void HomeVaults::write_cache(const Vault& v) const
{
    json files = json::array();
    for (const Vaults::File& f : v.files)
        files.push_back(Vaults::file_to_json(f));
    const json j = {{"version", 1}, {"synced_at", v.synced_at}, {"partial", v.partial}, {"last_error", v.last_error}, {"files", files}};
    write_atomic(path_of(cache_dir(v.spec.id)) / "files.json", j.dump(-1, ' ', false, json::error_handler_t::replace));
}

// ------------------------------------------------------------------------------ secrets ----

#if wxUSE_SECRETSTORE
static wxString service_of(const std::string& id, const std::string& key) { return wxString::FromUTF8("EdgeSlicer vault/" + id + "/" + key); }
#endif

bool HomeVaults::secure_store() const
{
#if wxUSE_SECRETSTORE
    if (m_secure_store < 0) {
        wxString why;
        m_secure_store = wxSecretStore::GetDefault().IsOk(&why) ? 1 : 0;
    }
    return m_secure_store == 1;
#else
    return false;
#endif
}

const std::string* HomeVaults::secret(const std::string& id, const std::string& key) const
{
    const std::string name = id + "/" + key;
    auto              s    = m_session_secrets.find(name);
    if (s != m_session_secrets.end())
        return &s->second;
    auto c = m_secret_cache.find(name);
    if (c == m_secret_cache.end()) {
        std::string value;
#if wxUSE_SECRETSTORE
        if (secure_store()) {
            wxString      user;
            wxSecretValue v;
            if (wxSecretStore::GetDefault().Load(service_of(id, key), user, v) && v.GetSize() > 0)
                value.assign(static_cast<const char*>(v.GetData()), v.GetSize());
        }
#endif
        c = m_secret_cache.emplace(name, std::move(value)).first;
    }
    return c->second.empty() ? nullptr : &c->second;
}

Vaults::Secrets HomeVaults::secrets_of(const Vaults::Spec& spec) const
{
    Vaults::Secrets out;
    for (const auto& slot : Vaults::secret_slots(spec))
        if (const std::string* v = secret(spec.id, slot.first))
            out[slot.first] = *v;
    return out;
}

void HomeVaults::set_secret(const std::string& id, const std::string& key, const std::string& value)
{
    m_secret_cache.erase(id + "/" + key);
#if wxUSE_SECRETSTORE
    if (secure_store()) {
        if (value.empty())
            wxSecretStore::GetDefault().Delete(service_of(id, key));
        else if (wxSecretStore::GetDefault().Save(service_of(id, key), wxString::FromUTF8(key), wxSecretValue(value.size(), value.data()))) {
            m_session_secrets.erase(id + "/" + key);
            m_secret_cache[id + "/" + key] = value;
            return;
        } else
            BOOST_LOG_TRIVIAL(warning) << "HomeVaults: the credential store refused a secret; keeping it for this session only";
    }
#endif
    if (value.empty())
        m_session_secrets.erase(id + "/" + key);
    else
        m_session_secrets[id + "/" + key] = value;
}

void HomeVaults::forget_secrets(const Vaults::Spec& spec)
{
    // Every slot any kind of vault has, so a vault whose kind changed leaves nothing behind.
    for (const char* key : {"user", "password", "token", "sas", "access_key_id", "secret_access_key"})
        set_secret(spec.id, key, std::string());
    for (auto it = m_session_secrets.begin(); it != m_session_secrets.end();)
        it = it->first.rfind(spec.id + "/", 0) == 0 ? m_session_secrets.erase(it) : std::next(it);
    for (auto it = m_secret_cache.begin(); it != m_secret_cache.end();)
        it = it->first.rfind(spec.id + "/", 0) == 0 ? m_secret_cache.erase(it) : std::next(it);
}

void HomeVaults::ask_secret(const std::string& id, const std::string& key)
{
    Vault* v = find(id);
    if (v == nullptr)
        return;
    const auto slots = Vaults::secret_slots(v->spec);
    auto       slot  = std::find_if(slots.begin(), slots.end(), [&key](const auto& s) { return s.first == key; });
    if (slot == slots.end())
        return;
    if (key == "sas") {
        ask_sas(*v);
        return;
    }
    const wxString title   = wxString::FromUTF8(v->spec.name);
    const wxString message = wxString::Format(_L("%s for %s. Leave it empty to forget it."), _(wxString::FromUTF8(slot->second)), title) + "\n" +
                             (secure_store() ? _L("It is kept in your system's credential store, not in EdgeSlicer's settings.")
                                             : _L("This system has no credential store, so it is kept only until EdgeSlicer closes."));
    wxString value;
    if (key == "user" || key == "access_key_id") {
        wxTextEntryDialog dlg(m_parent, message, title);
        if (const std::string* cur = secret(id, key))
            dlg.SetValue(wxString::FromUTF8(*cur));
        if (dlg.ShowModal() != wxID_OK)
            return;
        value = dlg.GetValue();
    } else {
        wxPasswordEntryDialog dlg(m_parent, message, title);
        if (dlg.ShowModal() != wxID_OK)
            return;
        value = dlg.GetValue();
    }
    std::string s = into_u8(value);
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    set_secret(id, key, s);
    send_state();
}

// An Azure SAS link: pasted whole, split into the container (which must be this vault's) and the token
// (which goes to the credential store), and checked before it is kept.
void HomeVaults::ask_sas(Vault& v)
{
    const wxString title   = wxString::FromUTF8(v.spec.name);
    const wxString message = _L("Paste the container's SAS link (or only its token). EdgeSlicer needs Read and List; leave it empty to forget it.") +
                             "\n" +
                             (secure_store() ? _L("The token is kept in your system's credential store, not in EdgeSlicer's settings.")
                                             : _L("This system has no credential store, so it is kept only until EdgeSlicer closes."));
    wxPasswordEntryDialog dlg(m_parent, message, title);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string pasted = into_u8(dlg.GetValue());
    if (pasted.find_first_not_of(" \t\r\n") == std::string::npos) {
        set_secret(v.spec.id, "sas", std::string());
        send_state();
        return;
    }
    std::string base, token;
    if (!Vaults::split_sas_url(pasted, base, token)) {
        show_error(m_parent, _L("That is not a SAS link. It looks like https://<account>.blob.core.windows.net/<container>?sv=...&sig=..."));
        return;
    }
    if (!base.empty() && lower(base) != lower(v.spec.url)) {
        show_error(m_parent, wxString::Format(_L("This SAS link is for %s, not for this vault's container %s."),
                                              wxString::FromUTF8(Vaults::display_address(base)), wxString::FromUTF8(v.spec.url)));
        return;
    }
    const Vaults::SasCheck check = Vaults::check_sas(token, int64_t(std::time(nullptr)));
    if (!check.ok) {
        show_error(m_parent, wxString::FromUTF8(check.error));
        return;
    }
    if (!check.warnings.empty()) {
        std::string text;
        for (const std::string& w : check.warnings)
            text += w + "\n\n";
        MessageDialog warn(m_parent, wxString::FromUTF8(text) + _L("Keep this SAS link anyway?"), title, wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
        if (warn.ShowModal() != wxID_YES)
            return;
    }
    set_secret(v.spec.id, "sas", token);
    send_state();
}

// ------------------------------------------------------------------------------ http warning ----

bool HomeVaults::http_dismissed(const std::string& id) const
{
    try {
        const json j = json::parse(wxGetApp().app_config->get(HTTP_OK_KEY));
        if (j.is_array())
            for (const json& v : j)
                if (v.is_string() && v.get<std::string>() == id)
                    return true;
    } catch (...) {}
    return false;
}

void HomeVaults::set_http_dismissed(const std::string& id, bool dismissed)
{
    json out = json::array();
    try {
        const json j = json::parse(wxGetApp().app_config->get(HTTP_OK_KEY));
        if (j.is_array())
            for (const json& v : j)
                if (v.is_string() && v.get<std::string>() != id && find(v.get<std::string>()) != nullptr)
                    out.push_back(v);
    } catch (...) {}
    if (dismissed)
        out.push_back(id);
    wxGetApp().app_config->set(HTTP_OK_KEY, out.dump());
}

bool HomeVaults::confirm_transport(const Vault& v)
{
    if (Vaults::address_check(v.spec.url) != Vaults::Transport::LanHttp)
        return true;
    if (m_http_warned.count(v.spec.id) || http_dismissed(v.spec.id))
        return true;
    const bool     password = v.spec.auth != "none";
    const wxString text =
        wxString::Format(password ? _L("\"%s\" uses plain http. On your local network, anyone who can see the traffic can read your password "
                                       "for this vault and the files, because http sends them unencrypted. Use https if your device offers it.")
                                  : _L("\"%s\" uses plain http. On your local network, anyone who can see the traffic can read the files, "
                                       "because http sends them unencrypted. Use https if your device offers it."),
                         wxString::FromUTF8(v.spec.name)) +
        "\n\n" + _L("Connect anyway?");
    RichMessageDialog dlg(m_parent, text, _L("Unencrypted connection"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    dlg.ShowCheckBox(_L("Don't warn me again for this vault"));
    if (dlg.ShowModal() != wxID_YES)
        return false;
    m_http_warned.insert(v.spec.id);
    if (dlg.IsCheckBoxChecked())
        set_http_dismissed(v.spec.id, true);
    send_state();
    return true;
}

// ------------------------------------------------------------------------------ the page ----

void HomeVaults::notice(const std::string& text, bool error) { m_send({{"type", "vault_notice"}, {"text", text}, {"error", error}}); }

void HomeVaults::send_state()
{
    m_keys.clear();
    json       vaults = json::array();
    json       files  = json::array();
    const bool secure = secure_store();
    const int64_t now = int64_t(std::time(nullptr));
    for (Vault& v : m_vaults) {
        ensure_cache(v);
        json slots = json::array();
        for (const auto& [key, label] : Vaults::secret_slots(v.spec))
            slots.push_back({{"key", key}, {"label", _u8L(label.c_str())}, {"set", secret(v.spec.id, key) != nullptr}});
        int64_t sas_expires = 0;
        if (v.spec.kind == Vaults::Kind::AzureBlob && v.spec.auth == "sas")
            if (const std::string* t = secret(v.spec.id, "sas"))
                sas_expires = Vaults::check_sas(*t, now).expires_at;
        const bool http = Vaults::address_check(v.spec.url) == Vaults::Transport::LanHttp;
        vaults.push_back({{"id", v.spec.id},
                          {"name", v.spec.name},
                          {"kind", Vaults::kind_name(v.spec.kind)},
                          {"builtin", v.spec.builtin},
                          {"address", Vaults::display_address(v.spec.url)},
                          {"bucket", v.spec.bucket},
                          {"prefix", v.spec.prefix},
                          {"spec", Vaults::spec_to_json(v.spec)},
                          {"slots", slots},
                          {"secure", secure},
                          {"syncing", v.syncing},
                          {"synced_at", v.synced_at},
                          {"count", v.files.size()},
                          {"partial", v.partial},
                          {"last_error", v.last_error},
                          {"http", http},
                          {"http_ok", http && http_dismissed(v.spec.id)},
                          {"sas_expires", sas_expires},
                          {"downloads", downloads_dir_of(v.spec)}});
        for (const Vaults::File& f : v.files) {
            const std::string key = key_of(v.spec.id, f.id);
            m_keys[key]           = {v.spec.id, f.id};
            files.push_back({{"key", key}, {"vault", v.spec.id}, {"name", f.name}, {"folder", f.folder}, {"size", f.size},
                             {"mtime", f.mtime}, {"type", type_of(f.name)}});
        }
    }
    m_send({{"type", "vaults"}, {"vaults", vaults}, {"files", files}, {"builtins", builtins()}, {"downloads_default", downloads_dir()}});
}

bool HomeVaults::handle(const json& msg)
{
    const std::string command = msg.value("command", std::string());
    if (command.rfind("vault_", 0) != 0)
        return false;
    const std::string id = msg.value("id", std::string());
    if (command == "vault_state")
        send_state();
    else if (command == "vault_save") {
        if (msg.contains("spec"))
            save_vault(msg["spec"]);
    } else if (command == "vault_delete") {
        if (find(id))
            delete_vault(id);
    } else if (command == "vault_secret") {
        ask_secret(id, msg.value("slot", std::string()));
    } else if (command == "vault_forget") {
        if (Vault* v = find(id)) {
            forget_secrets(v->spec);
            send_state();
        }
    } else if (command == "vault_sync") {
        if (find(id))
            sync(id);
    } else if (command == "vault_test") {
        if (find(id))
            test(id);
    } else if (command == "vault_download") {
        const std::string key = msg.value("key", std::string());
        if (m_keys.count(key))
            download(key, msg.value("open", false));
    } else if (command == "vault_open_folder") {
        const Vault* v = find(id);
        const std::string dir = v ? downloads_dir_of(v->spec) : downloads_dir();
        boost::system::error_code ec;
        fs::create_directories(path_of(dir), ec);
        desktop_open_folder(dir);
    } else if (command == "vault_pick_dir") {
        const std::string current = msg.value("current", std::string());
        wxDirDialog dlg(m_parent, _L("Choose the folder for this vault's downloads"), wxString::FromUTF8(current.empty() ? downloads_dir() : current),
                        wxDD_DEFAULT_STYLE | wxDD_NEW_DIR_BUTTON);
        if (dlg.ShowModal() == wxID_OK)
            m_send({{"type", "vault_dir"}, {"path", into_u8(dlg.GetPath())}});
    } else
        BOOST_LOG_TRIVIAL(warning) << "HomeVaults: unknown command \"" << command << "\"";
    return true;
}

// ------------------------------------------------------------------------------ vaults ----

void HomeVaults::save_vault(const json& spec_json)
{
    Vaults::Spec spec;
    try {
        spec = Vaults::spec_from_json(spec_json);
    } catch (const std::exception& e) {
        m_send({{"type", "vault_invalid"}, {"error", e.what()}});
        return;
    }
    Vault* existing = spec.id.empty() ? nullptr : find(spec.id);
    if (existing == nullptr) {
        std::random_device rd;
        do
            spec.id = Vaults::make_id(spec.name, rd());
        while (find(spec.id));
        Vault v;
        v.spec         = spec;
        v.cache_loaded = true;
        m_vaults.push_back(std::move(v));
    } else {
        const auto now_slots = Vaults::secret_slots(spec);
        for (const auto& old : Vaults::secret_slots(existing->spec))
            if (std::none_of(now_slots.begin(), now_slots.end(), [&old](const auto& s) { return s.first == old.first; }))
                set_secret(spec.id, old.first, std::string());
        // Another place: what was listed is gone, and an http warning dismissed for the old address no longer counts.
        const bool moved = existing->spec.kind != spec.kind || existing->spec.url != spec.url || existing->spec.prefix != spec.prefix ||
                           existing->spec.bucket != spec.bucket || existing->spec.addressing != spec.addressing;
        if (moved || existing->spec.recursive != spec.recursive || existing->spec.max_depth != spec.max_depth) {
            ensure_cache(*existing);
            existing->files.clear();
            existing->synced_at  = 0;
            existing->last_error.clear();
            existing->partial = false;
            write_cache(*existing);
        }
        if (moved) {
            set_http_dismissed(spec.id, false);
            m_http_warned.erase(spec.id);
            // A secret made for the old place (a SAS for another container) is not sent to the new one.
            if (existing->spec.kind == spec.kind && spec.kind == Vaults::Kind::AzureBlob && existing->spec.url != spec.url)
                set_secret(spec.id, "sas", std::string());
        }
        existing->spec = spec;
    }
    save_specs();
    m_send({{"type", "vault_saved"}, {"id", spec.id}});
    send_state();
}

void HomeVaults::delete_vault(const std::string& id)
{
    Vault*        v = find(id);
    MessageDialog dlg(m_parent,
                      wxString::Format(_L("Remove the vault \"%s\"? Its saved credentials and the list it fetched are removed too. "
                                          "Files you downloaded stay where they are."),
                                       wxString::FromUTF8(v->spec.name)),
                      _L("Vaults"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    if (dlg.ShowModal() != wxID_YES)
        return;
    forget_secrets(v->spec);
    boost::system::error_code ec;
    fs::remove_all(path_of(cache_dir(id)), ec);
    m_vaults.erase(std::remove_if(m_vaults.begin(), m_vaults.end(), [&id](const Vault& x) { return x.spec.id == id; }), m_vaults.end());
    set_http_dismissed(id, false);
    m_http_warned.erase(id);
    save_specs();
    send_state();
}

void HomeVaults::sync(const std::string& id)
{
    Vault* v = find(id);
    if (v->syncing || !confirm_transport(*v))
        return;
    ensure_cache(*v);
    v->syncing = true;
    send_state();
    const Vaults::Spec    spec    = v->spec;
    const Vaults::Secrets secrets = secrets_of(spec);
    std::weak_ptr<bool>   alive   = m_alive;
    auto                  cancel  = m_cancel;
    std::thread([this, alive, cancel, spec, secrets]() {
        Vaults::Listing l = Vaults::make_provider(spec)->list(secrets, LIST_HTTP, *cancel, int64_t(std::time(nullptr)));
        if (*cancel)
            return;
        BOOST_LOG_TRIVIAL(info) << "HomeVaults: listed " << spec.id << ": " << l.files.size() << " files, " << l.requests << " requests"
                                << (l.error.empty() ? std::string() : ", stopped: " + l.error);
        wxGetApp().CallAfter([this, alive, id = spec.id, l = std::move(l)]() mutable {
            if (alive.expired())
                return;
            Vault* v = find(id);
            if (v == nullptr)
                return; // removed meanwhile
            v->syncing = false;
            if (l.ok) {
                v->files     = std::move(l.files);
                v->synced_at = int64_t(std::time(nullptr));
                v->partial   = l.partial;
                v->last_error.clear();
            } else
                v->last_error = l.error; // what was listed before stays
            write_cache(*v);
            send_state();
            if (!l.ok)
                notice(v->spec.name + ": " + l.error, true);
            else if (l.partial)
                notice(v->spec.name + ": " + _u8L("The listing stopped early (too many files or folders); what came is shown."));
        });
    }).detach();
}

void HomeVaults::test(const std::string& id)
{
    Vault* v = find(id);
    if (!confirm_transport(*v))
        return;
    const Vaults::Spec    spec    = v->spec;
    const Vaults::Secrets secrets = secrets_of(spec);
    std::weak_ptr<bool>   alive   = m_alive;
    notice(spec.name + ": " + _u8L("Testing the connection..."));
    std::thread([this, alive, spec, secrets]() {
        const Vaults::TestResult t = Vaults::test_connection(spec, secrets, LIST_HTTP, int64_t(std::time(nullptr)));
        wxGetApp().CallAfter([this, alive, name = spec.name, t]() {
            if (alive.expired())
                return;
            if (t.ok)
                notice(name + ": " + _u8L("Connected.") + " " + std::to_string(t.files) + " " + _u8L("model files in the first answer."));
            else
                notice(name + ": " + t.error, true);
        });
    }).detach();
}

void HomeVaults::download(const std::string& key, bool open_after)
{
    const std::string vault_id = m_keys[key].first;
    const std::string file_id  = m_keys[key].second;
    Vault*            v        = find(vault_id);
    if (v == nullptr)
        return;
    auto f = std::find_if(v->files.begin(), v->files.end(), [&](const Vaults::File& x) { return x.id == file_id; });
    if (f == v->files.end() || !confirm_transport(*v))
        return;
    const Vaults::Spec    spec    = v->spec;
    const Vaults::Secrets secrets = secrets_of(spec);
    const Vaults::File    file    = *f;
    const std::string     folder  = downloads_dir_of(spec);
    std::weak_ptr<bool>   alive   = m_alive;
    auto                  cancel  = m_cancel;
    notice(_u8L("Downloading") + " " + file.name + "...");
    std::thread([this, alive, cancel, spec, secrets, file, folder, open_after]() {
        Vaults::Request req = Vaults::make_provider(spec)->download_request(secrets, file, int64_t(std::time(nullptr)));
        const HomeDownload::Result r = HomeDownload::save_model(
            path_of(folder), file.name,
            [&req](const std::function<bool(const char*, size_t)>& sink, std::string& error) {
                req.sink                 = sink;
                const Vaults::Response resp = Vaults::fetch(FILE_HTTP, req);
                if (resp.status >= 200 && resp.status < 300)
                    return true;
                error = _u8L("The download failed:") + " " + Vaults::http_error(resp);
                return false;
            },
            *cancel);
        if (*cancel)
            return;
        BOOST_LOG_TRIVIAL(info) << "HomeVaults: download from " << spec.id << (r.ok ? " saved" : " failed: " + r.error);
        wxGetApp().CallAfter([this, alive, r, folder, name = file.name, open_after]() {
            if (alive.expired())
                return;
            if (!r.ok) {
                notice(name + ": " + r.error, true);
                return;
            }
            note_download_folder(folder);
            if (open_after && m_open_file)
                m_open_file(r.path);
            else
                notice(_u8L("Saved") + " " + u8(path_of(r.path).filename()) + " " + _u8L("to") + " " + folder);
        });
    }).detach();
}

// The first time a file lands in a downloads folder, that folder joins the Library (once: a folder the
// user later removes from the Library is not added again).
void HomeVaults::note_download_folder(const std::string& folder)
{
    AppConfig* cfg  = wxGetApp().app_config;
    json       done = json::array();
    try {
        done = json::parse(cfg->get(DOWNLOADS_LIB_KEY));
        if (!done.is_array())
            done = json::array();
    } catch (...) {}
    for (const json& d : done)
        if (d.is_string() && d.get<std::string>() == folder) {
            if (m_library_changed)
                m_library_changed(); // the new file shows up there
            return;
        }
    std::vector<Library::Folder> folders = Library::folders_from_json(cfg->get("home_library_folders"));
    const size_t                 before  = folders.size();
    Library::Folder              f;
    f.path     = folder;
    f.category = "Vaults";
    folders.push_back(f);
    folders = Library::folders_from_json(Library::folders_to_json(folders)); // normalized, a path already there kept once
    if (folders.size() > before)
        cfg->set("home_library_folders", Library::folders_to_json(folders));
    done.push_back(folder);
    cfg->set(DOWNLOADS_LIB_KEY, done.dump());
    if (m_library_changed)
        m_library_changed();
}

} // namespace GUI
} // namespace Slic3r
