#include "PluginIpcProtocol.hpp"

#include <algorithm>

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
        out = nlohmann::json::parse(data, data + size);
    } catch (const nlohmann::json::parse_error& ex) {
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

bool in_child_process() { return g_in_child_process; }

void set_in_child_process() { g_in_child_process = true; }

} // namespace Slic3r::plugin_ipc
