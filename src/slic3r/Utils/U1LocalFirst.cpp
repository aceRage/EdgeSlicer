#include "U1LocalFirst.hpp"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <cctype>

namespace Slic3r {
namespace U1LocalFirst {

using nlohmann::json;

// ---------------------------------------------------------------- topics ----

DeviceTopic device_topic_kind(const std::string& topic)
{
    const size_t slash = topic.find('/');
    if (slash == std::string::npos || slash == 0)
        return DeviceTopic::None;
    const std::string head = topic.substr(0, slash);
    if (head.find('#') != std::string::npos || (head.find('+') != std::string::npos && head != "+"))
        return DeviceTopic::None;
    const std::string tail = topic.substr(slash + 1);
    if (tail == "request") return DeviceTopic::Request;
    if (tail == "response") return DeviceTopic::Response;
    if (tail == "status") return DeviceTopic::Status;
    if (tail == "notification") return DeviceTopic::Notification;
    return DeviceTopic::None;
}

std::string serial_of_topic(const std::string& topic)
{
    if (!is_device_topic(topic))
        return {};
    const std::string head = topic.substr(0, topic.find('/'));
    return head == "+" ? std::string() : head;
}

bool RouteTable::want(const std::string& topic, int qos)
{
    if (!is_device_topic(topic))
        return false;
    m_wanted[topic] = qos;
    return true;
}

bool RouteTable::unwant(const std::string& topic)
{
    if (!is_device_topic(topic))
        return false;
    m_wanted.erase(topic);
    return true;
}

bool RouteTable::accept(Via source, const std::string& topic) const
{
    if (!is_device_topic(topic))
        return source == Via::Cloud; // the LAN session carries device topics only
    return source == m_via;
}

// ----------------------------------------------------------- addresses ----

static bool parse_ipv4(const std::string& s, int out[4])
{
    int part = 0, digits = 0, value = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            if (digits == 0 || value > 255 || part > 3)
                return false;
            out[part++] = value;
            digits = value = 0;
        } else if (std::isdigit((unsigned char) s[i]) && digits < 3) {
            value = value * 10 + (s[i] - '0');
            ++digits;
        } else {
            return false;
        }
    }
    return part == 4;
}

bool is_private_ipv4(const std::string& ip)
{
    int a[4];
    if (!parse_ipv4(ip, a))
        return false;
    return a[0] == 10 || (a[0] == 172 && a[1] >= 16 && a[1] <= 31) || (a[0] == 192 && a[1] == 168) ||
           (a[0] == 169 && a[1] == 254) || (a[0] == 100 && a[1] >= 64 && a[1] <= 127); // CGNAT / Tailscale
}

std::vector<std::string> lan_ipv4_from_system_info(const json& response)
{
    std::vector<std::string> out;
    try {
        const json* r = &response;
        if (r->is_object() && r->contains("result"))
            r = &(*r)["result"];
        if (r->is_object() && r->contains("system_info"))
            r = &(*r)["system_info"];
        if (!r->is_object() || !r->contains("network") || !(*r)["network"].is_object())
            return out;
        for (const auto& iface : (*r)["network"].items()) {
            const json& v = iface.value();
            if (!v.is_object() || !v.contains("ip_addresses") || !v["ip_addresses"].is_array())
                continue;
            for (const json& a : v["ip_addresses"]) {
                if (!a.is_object() || !a.contains("address") || !a["address"].is_string())
                    continue;
                const std::string fam  = a.contains("family") && a["family"].is_string() ? a["family"].get<std::string>() : "ipv4";
                const std::string addr = a["address"].get<std::string>();
                if (fam == "ipv4" && is_private_ipv4(addr) && std::find(out.begin(), out.end(), addr) == out.end())
                    out.push_back(addr);
            }
        }
    } catch (...) {}
    return out;
}

// ---------------------------------------------------------------- pacing ----

long long retry_after_ms(const Timing& t, int failures)
{
    if (failures <= 0 || t.retry_ms.empty())
        return 0;
    const size_t i = std::min<size_t>((size_t) failures - 1, t.retry_ms.size() - 1);
    return t.retry_ms[i];
}

bool should_fall_back(const Timing& t, const LanHealth& h)
{
    if (!h.mqtt_connected && h.down_for_ms >= t.lan_down_grace_ms)
        return true;
    return h.probe_fails >= t.lan_probe_fails;
}

// ------------------------------------------------------------- approval ----

AuthAnswer parse_auth_answer(const json& result)
{
    if (!result.is_object() || !result.contains("state") || !result["state"].is_string())
        return AuthAnswer::NoAnswer;
    const std::string s = result["state"].get<std::string>();
    if (s == "success") {
        // Only an answer that carries the certificate can be used.
        auto has = [&result](const char* k) { return result.contains(k) && result[k].is_string() && !result[k].get<std::string>().empty(); };
        return has("ca") && has("cert") && has("key") ? AuthAnswer::Success : AuthAnswer::NotAvailable;
    }
    if (s == "unauthorized") return AuthAnswer::Unauthorized;
    if (s == "authorizing") return AuthAnswer::Authorizing;
    return AuthAnswer::NotAvailable; // "error", "modeNotAvailable", ...
}

AuthStep next_auth_step(const Timing& t, AuthAnswer check, const AskState& s)
{
    switch (check) {
    case AuthAnswer::Success: return AuthStep::UseLan;
    case AuthAnswer::Unauthorized:
    case AuthAnswer::Authorizing:
        if (s.asked_this_session || s.asks >= t.max_unanswered_asks)
            return AuthStep::StayCloud;
        return AuthStep::AskApproval;
    default: return AuthStep::StayCloud;
    }
}

// ---------------------------------------------------------- slot store ----

std::string new_client_id()
{
    static boost::uuids::random_generator gen;
    return "orca-" + boost::uuids::to_string(gen());
}

SlotStore SlotStore::from_json(const json& j)
{
    SlotStore s;
    if (j.is_object() && j.contains("slots") && j["slots"].is_object())
        s.m_j = j;
    else
        s.m_j = json::object();
    s.m_j["version"] = 1;
    if (!s.m_j.contains("slots") || !s.m_j["slots"].is_object())
        s.m_j["slots"] = json::object();
    return s;
}

std::string SlotStore::client_id(int slot, const Protector& p)
{
    json& sl = m_j["slots"][std::to_string(slot)];
    if (!sl.is_object())
        sl = json::object();
    if (sl.contains("client_id") && sl["client_id"].is_string()) {
        const std::string stored = sl["client_id"].get<std::string>();
        const std::string plain  = p.unprotect ? p.unprotect(stored) : stored;
        if (!plain.empty())
            return plain;
    }
    // None yet, or unreadable: a new id, and whatever the printers said about the old one no
    // longer applies.
    const std::string id = new_client_id();
    sl["client_id"]      = p.protect ? p.protect(id) : id;
    sl["printers"]       = json::object();
    return id;
}

json& SlotStore::printer(int slot, const std::string& sn)
{
    json& sl = m_j["slots"][std::to_string(slot)];
    if (!sl.is_object())
        sl = json::object();
    if (!sl.contains("printers") || !sl["printers"].is_object())
        sl["printers"] = json::object();
    json& p = sl["printers"][sn];
    if (!p.is_object())
        p = json::object();
    return p;
}

AskState SlotStore::ask_state(int slot, const std::string& sn) const
{
    AskState s;
    const std::string key = std::to_string(slot);
    if (!m_j.contains("slots") || !m_j["slots"].contains(key))
        return s;
    const json& sl = m_j["slots"][key];
    if (!sl.is_object() || !sl.contains("printers") || !sl["printers"].is_object() || !sl["printers"].contains(sn))
        return s;
    const json& p = sl["printers"][sn];
    s.authorized  = p.value("authorized", false);
    s.asks        = p.value("asks", 0);
    return s;
}

void SlotStore::set_authorized(int slot, const std::string& sn, bool authorized)
{
    json& p          = printer(slot, sn);
    p["authorized"]  = authorized;
    if (authorized)
        p["asks"] = 0;
}

void SlotStore::count_unanswered_ask(int slot, const std::string& sn)
{
    json& p   = printer(slot, sn);
    p["asks"] = p.value("asks", 0) + 1;
}

void SlotStore::merge_into(json& other, int slot) const
{
    if (!other.is_object() || !other.contains("slots") || !other["slots"].is_object()) {
        json fresh       = json::object();
        fresh["version"] = 1;
        fresh["slots"]   = json::object();
        other            = fresh;
    }
    const std::string key = std::to_string(slot);
    if (m_j.contains("slots") && m_j["slots"].contains(key))
        other["slots"][key] = m_j["slots"][key];
}

int pick_slot(const std::function<bool(int)>& try_claim, int max_slots)
{
    for (int i = 0; i < max_slots; ++i)
        if (try_claim(i))
            return i;
    return -1;
}

// --------------------------------------------- phone hub key store ----

bool should_remember_keys(const json& p)
{
    if (!p.is_object())
        return false;
    // A connect with no link mode is the Device page's LAN default (sw_mqtt_set_engine).
    const std::string mode = p.contains("link_mode") && p["link_mode"].is_string() ? p["link_mode"].get<std::string>() : "lan";
    if (mode != "lan")
        return false;
    for (const char* k : { "ca", "cert", "key" })
        if (!p.contains(k) || !p[k].is_string() || p[k].get<std::string>().empty())
            return false;
    return true;
}

json keys_put(json all, const std::string& sn, const std::string& origin, const json& entry)
{
    if (!all.is_object() || all.value("version", 1) < 2)
        all = json::object(); // version-1 entries were mostly cloud ids; they are dropped
    all["version"] = 2;
    if (sn.empty() || origin.empty())
        return all;
    if (!all.contains(sn) || !all[sn].is_object())
        all[sn] = json::object();
    all[sn][origin] = entry;
    return all;
}

json keys_get(const json& all, const std::string& sn, const std::string& origin)
{
    if (!all.is_object() || all.value("version", 1) < 2 || sn.empty() || origin.empty())
        return nullptr;
    if (!all.contains(sn) || !all[sn].is_object() || !all[sn].contains(origin) || !all[sn][origin].is_object())
        return nullptr;
    return all[sn][origin];
}

} // namespace U1LocalFirst
} // namespace Slic3r
