#include "PluginIpcProtocol.hpp"

#include <slic3r/plugin/PluginFsUtils.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace Slic3r::plugin_ipc {
namespace {

bool g_in_child_process = false;

} // namespace

uint32_t read_le32(const uint8_t bytes[4])
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

void write_le32(uint8_t bytes[4], uint32_t value)
{
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
    bytes[2] = static_cast<uint8_t>(value >> 16);
    bytes[3] = static_cast<uint8_t>(value >> 24);
}

bool encode_message(const nlohmann::json& message, std::vector<uint8_t>& out, std::string& error)
{
    if (!message.is_object()) {
        error = "IPC message must be a JSON object";
        return false;
    }
    const std::string body = message.dump();
    if (body.size() > k_max_payload_bytes) {
        error = "IPC payload exceeds 16 MiB limit";
        return false;
    }
    out.resize(4 + body.size());
    write_le32(out.data(), static_cast<uint32_t>(body.size()));
    std::copy(body.begin(), body.end(), out.begin() + 4);
    return true;
}

bool decode_payload(const uint8_t* data, uint32_t size, nlohmann::json& out, std::string& error)
{
    if (data == nullptr || size == 0) {
        error = "empty IPC payload";
        return false;
    }
    if (size > k_max_payload_bytes) {
        error = "oversized IPC payload";
        return false;
    }
    try {
        // H2: the depth check discards anything deeper than k_max_json_depth while parsing
        // (nlohmann's parser is iterative), then fails the whole message.
        bool too_deep = false;
        const nlohmann::json::parser_callback_t depth_guard = [&too_deep](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
            if (depth > k_max_json_depth) {
                too_deep = true;
                return false;
            }
            return true;
        };
        out = nlohmann::json::parse(data, data + size, depth_guard);
        if (too_deep) {
            out   = nlohmann::json();
            error = "IPC payload nested too deeply";
            return false;
        }
    } catch (const nlohmann::json::parse_error& ex) {
        error = std::string("garbage IPC payload: ") + ex.what();
        return false;
    } catch (const std::exception& ex) {
        // out_of_range (a number such as 1e400), type errors, allocation failures.
        error = std::string("garbage IPC payload: ") + ex.what();
        return false;
    }
    if (!out.is_object()) {
        error = "IPC payload is not a JSON object";
        return false;
    }
    if (!out.contains("type") || !out["type"].is_string()) {
        error = "IPC payload missing type";
        return false;
    }
    return true;
}

bool handshake_versions_compatible(int remote_version, std::string& error)
{
    if (remote_version != k_protocol_version) {
        error = "IPC protocol version mismatch: remote " + std::to_string(remote_version) + " local " +
                std::to_string(k_protocol_version);
        return false;
    }
    return true;
}

namespace {

using json = nlohmann::json;

bool check_string(const json& msg, const char* key, bool required, std::size_t max_bytes, std::string& error)
{
    const auto it = msg.find(key);
    if (it == msg.end()) {
        if (required)
            error = std::string("missing field '") + key + "'";
        return !required;
    }
    if (!it->is_string()) {
        error = std::string("field '") + key + "' must be a string";
        return false;
    }
    if (it->get_ref<const std::string&>().size() > max_bytes) {
        error = std::string("field '") + key + "' is too long";
        return false;
    }
    return true;
}

bool check_bool(const json& msg, const char* key, std::string& error)
{
    const auto it = msg.find(key);
    if (it == msg.end() || it->is_boolean())
        return true;
    error = std::string("field '") + key + "' must be a boolean";
    return false;
}

bool is_small_non_negative(const json& v, std::int64_t max_inclusive)
{
    if (v.is_number_unsigned())
        return v.get<std::uint64_t>() <= static_cast<std::uint64_t>(max_inclusive);
    if (v.is_number_integer()) {
        const std::int64_t i = v.get<std::int64_t>();
        return i >= 0 && i <= max_inclusive;
    }
    return false;
}

// Request ids are what the parent generated: non-negative integers.
bool check_id(const json& msg, bool required, std::string& error)
{
    const auto it = msg.find("id");
    if (it == msg.end()) {
        if (required)
            error = "missing field 'id'";
        return !required;
    }
    if (is_small_non_negative(*it, (std::numeric_limits<std::int64_t>::max)()))
        return true;
    error = "field 'id' must be a non-negative integer";
    return false;
}

bool check_status(const json& msg, std::string& error)
{
    const auto it = msg.find("status");
    // PluginResult: Success, Skipped, RecoverableError, FatalError.
    if (it == msg.end() || is_small_non_negative(*it, 3))
        return true;
    error = "field 'status' must be a PluginResult (0-3)";
    return false;
}

} // namespace

bool validate_child_message(const nlohmann::json& msg, std::string& error)
{
    try {
        if (!msg.is_object()) {
            error = "message is not a JSON object";
            return false;
        }
        if (!check_string(msg, "type", true, 64, error))
            return false;
        const std::string& type = msg["type"].get_ref<const std::string&>();
        if (type == k_type_result) {
            return check_id(msg, true, error) && check_status(msg, error) &&
                   check_string(msg, "message", false, k_max_message_bytes, error) &&
                   check_string(msg, "data", false, k_max_data_bytes, error) && check_bool(msg, "cancelled", error) &&
                   check_string(msg, "gcode", false, k_max_payload_bytes, error);
        }
        if (type == k_type_error) {
            return check_id(msg, false, error) && check_string(msg, "message", false, k_max_message_bytes, error) &&
                   check_bool(msg, "cancelled", error);
        }
        if (type == k_type_host_call) {
            if (!check_id(msg, true, error) || !check_string(msg, "method", true, 128, error) ||
                !check_string(msg, "plugin_key", false, k_max_short_string_bytes, error) ||
                !check_string(msg, "capability", false, k_max_short_string_bytes, error))
                return false;
            const auto args = msg.find("args");
            if (args != msg.end() && !args->is_object()) {
                error = "field 'args' must be an object";
                return false;
            }
            return true;
        }
        if (type == k_type_audit_check) {
            if (!check_id(msg, true, error) || !check_string(msg, "event", false, 256, error) ||
                !check_string(msg, "event_type", false, 256, error) || !check_bool(msg, "is_write", error) ||
                !check_string(msg, "plugin_key", false, k_max_short_string_bytes, error) ||
                !check_string(msg, "capability", false, k_max_short_string_bytes, error))
                return false;
            const auto targets = msg.find("targets");
            if (targets == msg.end())
                return true;
            if (!targets->is_array() || targets->size() > k_max_audit_targets) {
                error = "field 'targets' must be a list of at most " + std::to_string(k_max_audit_targets) + " paths";
                return false;
            }
            for (const json& t : *targets)
                if (!t.is_string() || t.get_ref<const std::string&>().size() > k_max_short_string_bytes) {
                    error = "field 'targets' must hold paths";
                    return false;
                }
            return true;
        }
        if (type == k_type_load_ok) {
            if (!check_id(msg, true, error))
                return false;
            // D3: a load the parent asked for metadata carries the capability list; an older
            // execute-time load_ok may omit it. When present it must parse.
            if (!msg.contains("capabilities"))
                return true;
            std::vector<HostedCapabilityInfo> ignored;
            return parse_hosted_capabilities(msg, ignored, error);
        }
        if (type == k_type_unload_ok)
            return check_id(msg, true, error);
        if (type == k_type_event_ok) {
            if (!check_id(msg, true, error))
                return false;
            for (const char* counter : {"delivered", "failed"}) {
                const auto it = msg.find(counter);
                if (it != msg.end() && !is_small_non_negative(*it, 1000000)) {
                    error = std::string("field '") + counter + "' must be a small non-negative integer";
                    return false;
                }
            }
            return true;
        }
        if (type == k_type_shutdown_ok)
            return check_id(msg, false, error);
        if (type == k_type_hello_ok) {
            const auto version = msg.find("protocol_version");
            if (version == msg.end() || !is_small_non_negative(*version, 1000000)) {
                error = "field 'protocol_version' must be a small non-negative integer";
                return false;
            }
            const auto sandbox = msg.find("sandbox");
            if (sandbox == msg.end())
                return true;
            if (!sandbox->is_object()) {
                error = "field 'sandbox' must be an object";
                return false;
            }
            for (const char* b : {"applied", "network", "exec", "degraded"})
                if (!check_bool(*sandbox, b, error))
                    return false;
            if (!check_string(*sandbox, "kind", false, 64, error) || !check_string(*sandbox, "detail", false, k_max_message_bytes, error))
                return false;
            const auto abi = sandbox->find("landlock_abi");
            if (abi != sandbox->end() && !is_small_non_negative(*abi, 1000)) {
                error = "field 'landlock_abi' must be a small integer";
                return false;
            }
            return true;
        }
        error = "unexpected message type '" + type.substr(0, 64) + "'";
        return false;
    } catch (const std::exception& ex) {
        error = std::string("malformed message: ") + ex.what();
        return false;
    }
}

namespace {

bool capability_name_ok(const std::string& name)
{
    if (name.empty() || name.size() > k_max_capability_name_bytes)
        return false;
    for (const unsigned char ch : name)
        if (ch < 0x20 || ch == 0x7f || ch == ';')
            return false;
    return true;
}

} // namespace

bool parse_hosted_capabilities(const nlohmann::json& load_ok, std::vector<HostedCapabilityInfo>& out, std::string& error)
{
    out.clear();
    try {
        if (!load_ok.is_object()) {
            error = "load reply is not an object";
            return false;
        }
        const auto caps = load_ok.find("capabilities");
        if (caps == load_ok.end() || !caps->is_array()) {
            error = "field 'capabilities' must be a list";
            return false;
        }
        if (caps->empty()) {
            error = "the plugin registered no capabilities";
            return false;
        }
        if (caps->size() > k_max_capabilities) {
            error = "field 'capabilities' lists more than " + std::to_string(k_max_capabilities) + " capabilities";
            return false;
        }
        std::set<std::pair<int, std::string>> seen;
        std::vector<HostedCapabilityInfo>     parsed;
        parsed.reserve(caps->size());
        for (const json& entry : *caps) {
            if (!entry.is_object()) {
                error = "a capability entry is not an object";
                return false;
            }
            HostedCapabilityInfo info;
            const auto           name = entry.find("name");
            if (name == entry.end() || !name->is_string() || !capability_name_ok(name->get_ref<const std::string&>())) {
                error = "a capability has a missing or invalid name";
                return false;
            }
            info.name       = name->get_ref<const std::string&>();
            const auto type = entry.find("type");
            if (type == entry.end() || !type->is_string() || type->get_ref<const std::string&>().size() > 64) {
                error = "capability '" + info.name + "' has a missing or invalid type";
                return false;
            }
            info.type = plugin_capability_type_from_string(type->get_ref<const std::string&>());
            if (info.type == PluginCapabilityType::Unknown) {
                error = "capability '" + info.name + "' has an unknown type";
                return false;
            }
            if (!check_bool(entry, "has_config_ui", error) || !check_bool(entry, "lifecycle_events", error) ||
                !check_string(entry, "config_ui", false, k_max_config_ui_bytes, error))
                return false;
            info.has_config_ui    = entry.value("has_config_ui", false);
            info.lifecycle_events = entry.value("lifecycle_events", false);
            if (info.has_config_ui && entry.contains("config_ui"))
                info.config_ui = entry["config_ui"].get<std::string>();
            const auto defaults = entry.find("default_config");
            if (defaults != entry.end()) {
                if (!defaults->is_object()) {
                    error = "capability '" + info.name + "' default_config must be an object";
                    return false;
                }
                info.default_config = *defaults;
            }
            if (!seen.emplace(static_cast<int>(info.type), info.name).second) {
                error = "capability '" + info.name + "' is listed twice";
                return false;
            }
            parsed.push_back(std::move(info));
        }
        out = std::move(parsed);
        return true;
    } catch (const std::exception& ex) {
        out.clear();
        error = std::string("malformed capability list: ") + ex.what();
        return false;
    }
}

nlohmann::json hosted_capabilities_to_json(const std::vector<HostedCapabilityInfo>& capabilities)
{
    json list = json::array();
    for (const HostedCapabilityInfo& info : capabilities) {
        json entry = {
            {"name", info.name},
            {"type", plugin_capability_type_to_string(info.type)},
            {"has_config_ui", info.has_config_ui},
            {"lifecycle_events", info.lifecycle_events},
            {"default_config", info.default_config.is_object() ? info.default_config : json::object()},
        };
        if (info.has_config_ui && !info.config_ui.empty() && info.config_ui.size() <= k_max_config_ui_bytes)
            entry["config_ui"] = info.config_ui;
        list.push_back(std::move(entry));
    }
    return list;
}

nlohmann::json permissions_to_json(const PluginPermissions& permissions)
{
    return json{
        {"fs_read", permissions.fs_read},
        {"fs_readwrite", permissions.fs_readwrite},
        {"network_http", permissions.network_http},
        {"network_socket", permissions.network_socket},
        {"process", permissions.process},
        {"events", permissions.events},
        {"threading", permissions.threading},
        {"full_access", permissions.full_access},
    };
}

bool permissions_from_json(const nlohmann::json& value, PluginPermissions& out, std::string& error)
{
    out = PluginPermissions{};
    try {
        if (!value.is_object()) {
            error = "permissions must be an object";
            return false;
        }
        static const std::set<std::string> known = {"fs_read", "fs_readwrite", "network_http", "network_socket",
                                                    "process", "events",  "threading",     "full_access"};
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (known.count(it.key()) == 0) {
                error = "unknown permission '" + it.key().substr(0, 64) + "'";
                return false;
            }
        }
        PluginPermissions parsed;
        const auto        read_list = [&value, &error](const char* key, std::vector<std::string>& dst) {
            const auto it = value.find(key);
            if (it == value.end())
                return true;
            if (!it->is_array() || it->size() > k_max_permission_entries) {
                error = std::string("permission '") + key + "' must be a list of at most " + std::to_string(k_max_permission_entries) +
                        " entries";
                return false;
            }
            for (const json& v : *it) {
                if (!v.is_string() || v.get_ref<const std::string&>().size() > k_max_short_string_bytes) {
                    error = std::string("permission '") + key + "' must hold short strings";
                    return false;
                }
                dst.push_back(v.get<std::string>());
            }
            return true;
        };
        if (!read_list("fs_read", parsed.fs_read) || !read_list("fs_readwrite", parsed.fs_readwrite) ||
            !read_list("network_http", parsed.network_http) || !read_list("network_socket", parsed.network_socket) ||
            !read_list("process", parsed.process) || !read_list("events", parsed.events))
            return false;
        if (!check_bool(value, "threading", error) || !check_bool(value, "full_access", error))
            return false;
        parsed.threading   = value.value("threading", false);
        parsed.full_access = value.value("full_access", false);
        out                = std::move(parsed);
        return true;
    } catch (const std::exception& ex) {
        error = std::string("malformed permissions: ") + ex.what();
        return false;
    }
}

// ---- Lifecycle events ----------------------------------------------------------------------

namespace {

bool is_key_char(unsigned char c) { return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.'; }

bool is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string to_lower_ascii(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool sensitive_key(const std::string& raw_key)
{
    const std::string key = to_lower_ascii(raw_key);
    static const char* const contains[] = {"password", "passwd",  "passphrase",    "secret",     "token",      "apikey",
                                           "api_key",  "api-key", "accesscode",    "access_code", "access-code", "authorization",
                                           "credential", "cookie", "session",      "private_key", "privatekey"};
    for (const char* part : contains)
        if (key.find(part) != std::string::npos)
            return true;
    static const char* const exact[] = {"pwd", "pass", "auth", "key", "sig", "signature", "pin", "otp"};
    for (const char* word : exact)
        if (key == word)
            return true;
    return false;
}

bool starts_with_ci(const std::string& text, std::size_t pos, const char* word)
{
    const std::size_t len = std::strlen(word);
    if (pos + len > text.size())
        return false;
    for (std::size_t i = 0; i < len; ++i) {
        char c = text[pos + i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != word[i])
            return false;
    }
    return true;
}

// End of an unquoted value: whitespace or a separator.
std::size_t value_end(const std::string& text, std::size_t pos)
{
    while (pos < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[pos]);
        if (is_space(c) || std::strchr("&,;\"')}]", static_cast<char>(c)) != nullptr)
            break;
        ++pos;
    }
    return pos;
}

// Cuts to max_bytes on a code point boundary and replaces bytes that are not valid UTF-8 with
// '?', so the snapshot always serialises (nlohmann refuses invalid UTF-8).
std::string clean_text(const std::string& in, std::size_t max_bytes)
{
    std::string out;
    out.reserve((std::min)(in.size(), max_bytes));
    std::size_t i = 0;
    while (i < in.size()) {
        const unsigned char c   = static_cast<unsigned char>(in[i]);
        std::size_t         len = 0;
        if (c < 0x80)
            len = 1;
        else if (c >= 0xC2 && c <= 0xDF)
            len = 2;
        else if (c >= 0xE0 && c <= 0xEF)
            len = 3;
        else if (c >= 0xF0 && c <= 0xF4)
            len = 4;
        bool valid = len > 0 && i + len <= in.size();
        for (std::size_t k = 1; valid && k < len; ++k)
            valid = (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
        if (valid && len == 3) {
            const unsigned char c1 = static_cast<unsigned char>(in[i + 1]);
            valid = !(c == 0xE0 && c1 < 0xA0) && !(c == 0xED && c1 >= 0xA0); // overlong, surrogates
        } else if (valid && len == 4) {
            const unsigned char c1 = static_cast<unsigned char>(in[i + 1]);
            valid = !(c == 0xF0 && c1 < 0x90) && !(c == 0xF4 && c1 >= 0x90);
        }
        const std::size_t take = valid ? len : 1;
        if (out.size() + (valid ? len : 1) > max_bytes)
            break;
        if (valid)
            out.append(in, i, len);
        else
            out.push_back('?');
        i += take;
    }
    return out;
}

std::string snapshot_text(const std::string& in) { return redact_secrets(clean_text(in, k_max_event_field_bytes)); }

const char* const k_event_string_fields[] = {"name", "msg", "id", "previous_name", "device_id", "job_id", "source"};

std::string* snapshot_field(LifecycleEventSnapshot& s, const std::string& key)
{
    if (key == "name")
        return &s.name;
    if (key == "msg")
        return &s.msg;
    if (key == "id")
        return &s.id;
    if (key == "previous_name")
        return &s.previous_name;
    if (key == "device_id")
        return &s.device_id;
    if (key == "job_id")
        return &s.job_id;
    if (key == "source")
        return &s.source;
    return nullptr;
}

} // namespace

std::string redact_secrets(const std::string& in)
{
    std::string       out;
    const std::size_t n = in.size();
    out.reserve(n);
    std::size_t i = 0;
    while (i < n) {
        // scheme://user:password@host -> scheme://***@host
        if (in.compare(i, 3, "://") == 0) {
            out += "://";
            i += 3;
            std::size_t end = i;
            while (end < n && std::strchr("/?#", in[end]) == nullptr && !is_space(static_cast<unsigned char>(in[end])))
                ++end;
            std::size_t at = std::string::npos;
            for (std::size_t k = i; k < end; ++k)
                if (in[k] == '@')
                    at = k;
            if (at != std::string::npos) {
                out += "***@";
                i = at + 1;
            }
            continue;
        }
        // Bearer / Basic tokens on their own.
        if ((i == 0 || !is_key_char(static_cast<unsigned char>(in[i - 1]))) &&
            (starts_with_ci(in, i, "bearer ") || starts_with_ci(in, i, "basic "))) {
            const std::size_t word = starts_with_ci(in, i, "bearer ") ? 6 : 5;
            out.append(in, i, word);
            i += word;
            while (i < n && is_space(static_cast<unsigned char>(in[i])))
                out.push_back(in[i++]);
            const std::size_t v = value_end(in, i);
            if (v > i)
                out += "***";
            i = v;
            continue;
        }
        const char c = in[i];
        if (c == '=' || c == ':') {
            // The key just before, skipping blanks and one closing quote ("token": "...").
            std::size_t k = i;
            while (k > 0 && (in[k - 1] == ' ' || in[k - 1] == '\t'))
                --k;
            if (k > 0 && (in[k - 1] == '"' || in[k - 1] == '\''))
                --k;
            std::size_t ks = k;
            while (ks > 0 && is_key_char(static_cast<unsigned char>(in[ks - 1])))
                --ks;
            if (ks < k && sensitive_key(in.substr(ks, k - ks))) {
                out.push_back(c);
                ++i;
                while (i < n && (in[i] == ' ' || in[i] == '\t'))
                    out.push_back(in[i++]);
                char quote = 0;
                if (i < n && (in[i] == '"' || in[i] == '\'')) {
                    quote = in[i];
                    out.push_back(in[i++]);
                }
                std::size_t v = i;
                if (quote != 0) {
                    while (v < n && in[v] != quote)
                        ++v;
                } else {
                    v = value_end(in, i);
                    // "Authorization: Bearer abc": the scheme word and its token both go.
                    const std::string word = to_lower_ascii(in.substr(i, v - i));
                    if (word == "bearer" || word == "basic") {
                        std::size_t t = v;
                        while (t < n && is_space(static_cast<unsigned char>(in[t])))
                            ++t;
                        v = value_end(in, t);
                    }
                }
                if (v > i)
                    out += "***";
                i = v;
                continue;
            }
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

bool looks_like_local_path(const std::string& text)
{
    if (text.size() >= 3 && std::isalpha(static_cast<unsigned char>(text[0])) != 0 && text[1] == ':' &&
        (text[2] == '\\' || text[2] == '/'))
        return true;
    if (text.size() >= 2 && ((text[0] == '\\' && text[1] == '\\') || (text[0] == '/' && text[1] == '/')))
        return true;
    if (!text.empty() && text[0] == '/')
        return true;
    if (text.size() >= 2 && text[0] == '~' && (text[1] == '/' || text[1] == '\\'))
        return true;
    return starts_with_ci(text, 0, "file:");
}

std::vector<std::string> lifecycle_snapshot_paths(const LifecycleEventSnapshot& s)
{
    std::vector<std::string> out;
    for (const std::string* field : {&s.name, &s.msg, &s.id, &s.previous_name, &s.device_id, &s.job_id, &s.source})
        if (looks_like_local_path(*field))
            out.push_back(*field);
    return out;
}

LifecycleEventSnapshot snapshot_lifecycle_event(LifecycleEvent event, const LifecycleEventContext& ctx)
{
    LifecycleEventSnapshot s;
    s.event         = event;
    s.code          = ctx.code;
    s.name          = snapshot_text(ctx.name);
    s.msg           = snapshot_text(ctx.msg);
    s.id            = snapshot_text(ctx.id);
    s.previous_name = snapshot_text(ctx.previous_name);
    s.device_id     = snapshot_text(ctx.device_id);
    s.job_id        = snapshot_text(ctx.job_id);
    s.source        = snapshot_text(ctx.source);
    s.index         = ctx.index;
    s.dirty         = ctx.dirty;
    // ctx.cancellation_check is a live callable: never copied.
    return s;
}

LifecycleEventContext context_from_snapshot(const LifecycleEventSnapshot& s)
{
    LifecycleEventContext ctx;
    ctx.name          = s.name;
    ctx.code          = s.code;
    ctx.msg           = s.msg;
    ctx.id            = s.id;
    ctx.previous_name = s.previous_name;
    ctx.device_id     = s.device_id;
    ctx.job_id        = s.job_id;
    ctx.source        = s.source;
    ctx.index         = s.index;
    ctx.dirty         = s.dirty;
    return ctx;
}

nlohmann::json lifecycle_event_to_json(const LifecycleEventSnapshot& s)
{
    return json{
        {"event", lifecycle_event_to_string(s.event)},
        {"code", lifecycle_evt_code_to_string(s.code)},
        {"name", s.name},
        {"msg", s.msg},
        {"id", s.id},
        {"previous_name", s.previous_name},
        {"device_id", s.device_id},
        {"job_id", s.job_id},
        {"source", s.source},
        {"index", s.index},
        {"dirty", s.dirty},
    };
}

bool lifecycle_event_from_name(const std::string& name, LifecycleEvent& out)
{
    for (int i = static_cast<int>(LifecycleEvent::NewProject); i <= static_cast<int>(LifecycleEvent::SendJobFinished); ++i) {
        const auto candidate = static_cast<LifecycleEvent>(i);
        if (lifecycle_event_to_string(candidate) == name) {
            out = candidate;
            return true;
        }
    }
    return false;
}

bool lifecycle_event_from_json(const nlohmann::json& value, LifecycleEventSnapshot& out, std::string& error)
{
    out = LifecycleEventSnapshot{};
    try {
        if (!value.is_object()) {
            error = "lifecycle event is not an object";
            return false;
        }
        LifecycleEventSnapshot parsed;
        const auto             event = value.find("event");
        if (event == value.end() || !event->is_string() || !lifecycle_event_from_name(event->get<std::string>(), parsed.event)) {
            error = "unknown lifecycle event";
            return false;
        }
        const auto code = value.find("code");
        if (code != value.end()) {
            if (!code->is_string()) {
                error = "lifecycle event code must be a string";
                return false;
            }
            const std::string c = code->get<std::string>();
            if (c == "Ok")
                parsed.code = LifecycleEvtCode::Ok;
            else if (c == "Error")
                parsed.code = LifecycleEvtCode::Error;
            else if (c == "Warn")
                parsed.code = LifecycleEvtCode::Warn;
            else {
                error = "unknown lifecycle event code";
                return false;
            }
        }
        for (const char* key : k_event_string_fields) {
            if (!check_string(value, key, false, k_max_event_field_bytes, error))
                return false;
            if (value.contains(key))
                *snapshot_field(parsed, key) = value[key].get<std::string>();
        }
        const auto index = value.find("index");
        if (index != value.end()) {
            if (!index->is_number_integer()) {
                error = "lifecycle event index must be an integer";
                return false;
            }
            const std::int64_t v = index->get<std::int64_t>();
            if (v < (std::numeric_limits<int>::min)() || v > (std::numeric_limits<int>::max)()) {
                error = "lifecycle event index out of range";
                return false;
            }
            parsed.index = static_cast<int>(v);
        }
        if (!check_bool(value, "dirty", error))
            return false;
        parsed.dirty = value.value("dirty", false);
        out          = std::move(parsed);
        return true;
    } catch (const std::exception& ex) {
        error = std::string("malformed lifecycle event: ") + ex.what();
        return false;
    }
}

bool in_child_process() { return g_in_child_process; }

void set_in_child_process() { g_in_child_process = true; }

} // namespace Slic3r::plugin_ipc
