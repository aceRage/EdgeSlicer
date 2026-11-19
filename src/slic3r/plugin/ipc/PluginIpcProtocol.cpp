#include "PluginIpcProtocol.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

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
        if (type == k_type_load_ok || type == k_type_unload_ok)
            return check_id(msg, true, error);
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

bool in_child_process() { return g_in_child_process; }

void set_in_child_process() { g_in_child_process = true; }

} // namespace Slic3r::plugin_ipc
