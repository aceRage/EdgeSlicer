#ifndef slic3r_MoonRakerJson_hpp_
#define slic3r_MoonRakerJson_hpp_

// wx-free helpers for the Snapmaker Moonraker HTTP/MQTT JSON that used to throw
// on the Http and Paho callback stacks (Orca #15947 Stage B). Tests include this
// header without MoonRaker.hpp, which pulls wx and MQTT.

#include <cstdint>
#include <string>

#include "nlohmann/json.hpp"

namespace Slic3r {

// Non-throwing parse. Discarded input leaves `out` as an empty object and returns false.
// A valid non-object (array, number, string) is stored as-is and returns true.
inline bool try_parse_json(const std::string &body, nlohmann::json &out)
{
    nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded()) {
        out = nlohmann::json::object();
        return false;
    }
    out = std::move(parsed);
    return true;
}

struct MoonrakerAuthInfo
{
    std::string state;
    std::string sn;
    std::string clientid;
    std::string ca;
    std::string cert;
    std::string key;
    int         port{0};
};

// TLS fields from the MQTT auth `result` object. Missing or wrong-typed fields,
// a non-object, or state != "success" return false. Never throws: a throw here
// would escape into Paho's C callback and terminate the process.
inline bool parse_moonraker_auth_result(const nlohmann::json &result, MoonrakerAuthInfo &out)
{
    if (!result.is_object())
        return false;

    auto take_string = [&](const char *key, std::string &dest) -> bool {
        const auto it = result.find(key);
        if (it == result.end() || !it->is_string())
            return false;
        dest = it->get<std::string>();
        return true;
    };

    MoonrakerAuthInfo tmp;
    if (!take_string("state", tmp.state))
        return false;
    if (tmp.state != "success")
        return false;
    if (!take_string("sn", tmp.sn))
        return false;
    if (!take_string("clientid", tmp.clientid))
        return false;
    if (!take_string("ca", tmp.ca))
        return false;
    if (!take_string("cert", tmp.cert))
        return false;
    if (!take_string("key", tmp.key))
        return false;

    const auto port_it = result.find("port");
    if (port_it == result.end() || !port_it->is_number_integer())
        return false;
    try {
        tmp.port = port_it->get<int>();
    } catch (...) {
        return false;
    }

    out = std::move(tmp);
    return true;
}

// JSON-RPC `id` must be an integer. A string/float/missing id must not throw
// via .get<int64_t>() on the Paho thread.
inline bool moonraker_jsonrpc_id(const nlohmann::json &body, int64_t &out)
{
    if (!body.is_object())
        return false;
    const auto it = body.find("id");
    if (it == body.end() || !it->is_number_integer())
        return false;
    try {
        out = it->get<int64_t>();
    } catch (...) {
        return false;
    }
    return true;
}

// Log `method` without .get<std::string>(), which throws when the field is not a string.
inline std::string moonraker_method_for_log(const nlohmann::json &body)
{
    if (!body.is_object())
        return {};
    const auto it = body.find("method");
    if (it == body.end())
        return {};
    return it->is_string() ? it->get<std::string>() : it->dump();
}

} // namespace Slic3r

#endif
