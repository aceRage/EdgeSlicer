#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
struct AuditDecision;
}

namespace Slic3r::plugin_ipc {

inline constexpr int         k_protocol_version     = 1;
inline constexpr uint32_t    k_max_payload_bytes    = 16u * 1024u * 1024u;
inline constexpr const char* k_helper_name          = "edge-plugin-host";
inline constexpr int         k_default_timeout_ms   = 120000;
inline constexpr int         k_handshake_timeout_ms = 15000;

inline constexpr const char* k_type_hello         = "hello";
inline constexpr const char* k_type_hello_ok      = "hello_ok";
inline constexpr const char* k_type_load          = "load";
inline constexpr const char* k_type_load_ok       = "load_ok";
inline constexpr const char* k_type_unload        = "unload";
inline constexpr const char* k_type_unload_ok     = "unload_ok";
inline constexpr const char* k_type_execute       = "execute";
inline constexpr const char* k_type_result        = "result";
inline constexpr const char* k_type_error         = "error";
inline constexpr const char* k_type_cancel        = "cancel";
inline constexpr const char* k_type_shutdown      = "shutdown";
inline constexpr const char* k_type_shutdown_ok   = "shutdown_ok";
inline constexpr const char* k_type_host_call     = "host_call";
inline constexpr const char* k_type_host_result   = "host_result";
inline constexpr const char* k_type_audit_check   = "audit_check";
inline constexpr const char* k_type_audit_result  = "audit_result";

inline constexpr const char* k_kind_script       = "script";
inline constexpr const char* k_kind_post_process = "post_process";

struct DecodeError
{
    std::string message;
};

// Little-endian length-prefixed UTF-8 JSON. Returns false and sets error on
// oversized, truncated, empty, or non-object payloads.
bool encode_message(const nlohmann::json& message, std::vector<uint8_t>& out, std::string& error);
bool decode_payload(const uint8_t* data, uint32_t size, nlohmann::json& out, std::string& error);
uint32_t read_le32(const uint8_t bytes[4]);
void     write_le32(uint8_t bytes[4], uint32_t value);

bool handshake_versions_compatible(int remote_version, std::string& error);

bool in_child_process();
void set_in_child_process();

// Child-only: RPC a host method back to Edge. Throws std::runtime_error on denial or transport failure.
nlohmann::json call_host(const std::string& method, const nlohmann::json& args = nlohmann::json::object());

// Child-only: Edge-side audit decision for a filesystem event.
AuditDecision rpc_audit_check(const std::string& event_name,
                              const std::vector<std::string>& targets,
                              bool is_write,
                              const std::string& event_type);

} // namespace Slic3r::plugin_ipc
