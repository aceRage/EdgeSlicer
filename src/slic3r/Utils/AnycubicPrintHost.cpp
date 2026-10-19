#include "AnycubicPrintHost.hpp"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

namespace Slic3r {

namespace {

std::string trimmed(const std::string &s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

std::string lower_copy(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool istarts_with(const std::string &s, const char *prefix)
{
    const std::string p = prefix;
    if (s.size() < p.size())
        return false;
    for (size_t i = 0; i < p.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(p[i])))
            return false;
    }
    return true;
}

bool icontains(const std::string &s, const char *needle)
{
    const std::string hay = lower_copy(s);
    const std::string n   = lower_copy(needle);
    return hay.find(n) != std::string::npos;
}

// Authority (host[:port]) inside a print_host value, without userinfo or path.
bool split_authority(const std::string &address, std::string &auth, size_t &auth_begin, size_t &auth_end)
{
    std::string a = trimmed(address);
    if (a.empty())
        return false;

    size_t host_start = 0;
    const size_t scheme = a.find("://");
    if (scheme != std::string::npos)
        host_start = scheme + 3;

    const size_t at = a.rfind('@');
    if (at != std::string::npos && at >= host_start)
        host_start = at + 1;

    size_t host_end = a.size();
    if (host_start < a.size() && a[host_start] == '[') {
        const size_t br = a.find(']', host_start);
        if (br == std::string::npos)
            return false;
        host_end = br + 1;
        if (host_end < a.size() && a[host_end] == ':') {
            size_t p = host_end + 1;
            while (p < a.size() && std::isdigit(static_cast<unsigned char>(a[p])))
                ++p;
            host_end = p;
        }
    } else {
        auto cut = [&](char c) {
            const size_t p = a.find(c, host_start);
            if (p != std::string::npos)
                host_end = std::min(host_end, p);
        };
        cut('/');
        cut('?');
        cut('#');
    }

    if (host_start >= host_end)
        return false;
    auth       = a.substr(host_start, host_end - host_start);
    auth_begin = host_start;
    auth_end   = host_end;
    return true;
}

bool port_digits(const std::string &s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

const nlohmann::json *compat_object(const nlohmann::json &root)
{
    if (!root.is_object())
        return nullptr;
    if (root.contains("result") && root["result"].is_object())
        return &root["result"];
    return &root;
}

std::string json_string_or_number(const nlohmann::json &obj, const char *key)
{
    const auto it = obj.find(key);
    if (it == obj.end())
        return {};
    if (it->is_string())
        return it->get<std::string>();
    if (it->is_number_integer())
        return std::to_string(it->get<std::int64_t>());
    if (it->is_number_unsigned())
        return std::to_string(it->get<std::uint64_t>());
    if (it->is_number_float())
        return std::to_string(it->get<double>());
    return {};
}

bool version_text_ok(const std::string &text, bool anycubic)
{
    if (text.empty())
        return true;
    if (istarts_with(text, "OctoPrint"))
        return true;
    if (!anycubic)
        return false;
    if (istarts_with(text, "Moonraker"))
        return true;
    if (istarts_with(text, "Anycubic"))
        return true;
    if (istarts_with(text, "Kobra"))
        return true;
    if (icontains(text, "OctoPrint"))
        return true;
    return false;
}

bool looks_like_html(const std::string &body)
{
    const size_t n    = std::min<size_t>(body.size(), 1024);
    const std::string head = lower_copy(body.substr(0, n));
    return head.find("<html") != std::string::npos || head.find("<!doctype") != std::string::npos;
}

} // namespace

bool is_anycubic_printer(const std::string &printer_model)
{
    return istarts_with(trimmed(printer_model), "Anycubic");
}

int anycubic_default_port(const std::string &host_type_key)
{
    if (host_type_key == "moonraker")
        return 7125;
    if (host_type_key == "octoprint")
        return 80;
    return 0;
}

bool print_host_has_explicit_port(const std::string &address)
{
    std::string auth;
    size_t      b = 0, e = 0;
    if (!split_authority(address, auth, b, e) || auth.empty())
        return false;
    if (auth.front() == '[') {
        const size_t br = auth.find(']');
        return br != std::string::npos && br + 1 < auth.size() && auth[br + 1] == ':';
    }
    const size_t colon = auth.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= auth.size())
        return false;
    return port_digits(auth.substr(colon + 1));
}

std::string print_host_with_port(const std::string &address, int port)
{
    if (port <= 0 || print_host_has_explicit_port(address))
        return address;
    const std::string a = trimmed(address);
    std::string       auth;
    size_t            b = 0, e = 0;
    if (!split_authority(a, auth, b, e))
        return a.empty() ? a : a + ":" + std::to_string(port);
    return a.substr(0, e) + ":" + std::to_string(port) + a.substr(e);
}

std::string anycubic_apply_default_port(const std::string &address,
                                        const std::string &host_type_key,
                                        const std::string &printer_model)
{
    if (!is_anycubic_printer(printer_model))
        return address;
    // Stock octoprint-compat is already on implicit 80; do not write :80 into saved URLs.
    if (host_type_key != "moonraker")
        return address;
    if (print_host_has_explicit_port(address))
        return address;
    return print_host_with_port(address, 7125);
}

bool should_send_api_key(const std::string &apikey)
{
    return !trimmed(apikey).empty();
}

AnycubicOctoVersion parse_octoprint_compat_version(const std::string &body, bool anycubic)
{
    AnycubicOctoVersion out;
    if (looks_like_html(body)) {
        out.error = "HTML response (not the OctoPrint-compatible API)";
        return out;
    }
    if (trimmed(body).empty()) {
        // Some Anycubic shims answer 200 with an empty body; treat that as "reachable".
        out.ok = anycubic;
        if (!anycubic)
            out.error = "empty response";
        return out;
    }

    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        out.error = "Could not parse server response";
        return out;
    }

    const nlohmann::json *obj = compat_object(parsed);
    if (obj == nullptr) {
        out.error = "Could not parse server response";
        return out;
    }

    out.api  = json_string_or_number(*obj, "api");
    out.text = json_string_or_number(*obj, "text");
    const std::string server = json_string_or_number(*obj, "server");

    const bool has_api = obj->contains("api");
    if (!has_api && out.text.empty() && server.empty()) {
        if (anycubic) {
            // A JSON object with none of the usual keys is still not HTML; stock firmware has
            // answered this way on some builds. Accept it so Test Connection is not a false fail.
            out.ok = true;
            return out;
        }
        out.error = "missing api field";
        return out;
    }

    if (!version_text_ok(out.text, anycubic)) {
        out.error = "Mismatched type of print host: " + out.text;
        return out;
    }
    out.ok = true;
    return out;
}

bool looks_like_http_portal(unsigned status, const std::string &body)
{
    if (status == 401 || status == 403)
        return true;
    if (looks_like_html(body))
        return true;
    return icontains(body.substr(0, std::min<size_t>(body.size(), 1024)), "rinkhals");
}

bool anycubic_upload_reply_ok(unsigned status, const std::string &body)
{
    if (status != 0 && (status < 200 || status >= 300))
        return false;
    if (looks_like_http_portal(status, body))
        return false;
    if (trimmed(body).empty())
        return true;
    if (looks_like_html(body))
        return false;

    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded())
        return true; // non-JSON 2xx, not HTML: treat as success
    if (!parsed.is_object())
        return true;

    if (parsed.contains("error"))
        return false;
    if (parsed.contains("result") && parsed["result"].is_object() && parsed["result"].contains("error"))
        return false;
    return true;
}

bool anycubic_prefer_start_print(const std::string &printer_model)
{
    return is_anycubic_printer(printer_model);
}

const char *anycubic_connection_hint()
{
    return "Anycubic: stock firmware uses Host Type Octo/Klipper on port 80 (the printer IP; API key optional). "
           "Rinkhals: uploads go to Moonraker on port 7125 (http://IP:7125). Since Rinkhals PR #68 the bare IP is "
           "the web portal, not the slicer API. Native LAN mode is Anycubic Slicer Next only.";
}

const char *anycubic_upload_only_warning()
{
    return "Stock Anycubic firmware can hang on \"Checking files\" after Upload (OrcaSlicer #14349). "
           "Upload and Print works. Use Upload and Print, or confirm you want to upload only.";
}

const char *anycubic_rinkhals_port_hint()
{
    return "If this printer runs Rinkhals, put :7125 on the hostname (Moonraker). Port 80 is the Rinkhals web portal.";
}

} // namespace Slic3r
