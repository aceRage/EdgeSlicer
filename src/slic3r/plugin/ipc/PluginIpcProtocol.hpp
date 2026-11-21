#pragma once

#include <nlohmann/json.hpp>

#include <slic3r/plugin/PythonPluginInterface.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
struct AuditDecision;
struct PluginPermissions;
}

namespace Slic3r::plugin_ipc {

// 2 (D3, 2026-11-20): load_ok carries the capabilities the helper imported, and the helper may
// forward a load-time permission request ("plugin.request_permissions") to EdgeSlicer.
inline constexpr int         k_protocol_version     = 2;
inline constexpr uint32_t    k_max_payload_bytes    = 16u * 1024u * 1024u;
inline constexpr const char* k_helper_name          = "edge-plugin-host";
inline constexpr int         k_default_timeout_ms   = 120000;
inline constexpr int         k_max_timeout_ms       = 30 * 60 * 1000;
inline constexpr int         k_handshake_timeout_ms = 15000;
inline constexpr int         k_cancel_grace_ms      = 300;
inline constexpr int         k_pump_poll_ms         = 200;

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

// H2: limits on what the parent accepts from the helper. decode_payload refuses deeper nesting;
// validate_child_message refuses longer strings and lists.
inline constexpr int         k_max_json_depth          = 64;
inline constexpr std::size_t k_max_short_string_bytes  = 4096;               // ids, keys, method names, paths
inline constexpr std::size_t k_max_message_bytes       = 64u * 1024u;        // human-readable message / reason
inline constexpr std::size_t k_max_data_bytes          = 8u * 1024u * 1024u; // ExecutionResult.data
inline constexpr std::size_t k_max_audit_targets       = 64;

// H2: strict schema check of a message the helper sent to Edge (result, error, host_call,
// audit_check, load_ok, unload_ok, hello_ok, shutdown_ok). Every field the parent reads must
// have the right JSON type and be in range (ids non-negative integers, status a PluginResult,
// strings and lists within the limits above). Returns false with a reason otherwise; the parent
// then kills the helper and fails the run. Never throws.
bool validate_child_message(const nlohmann::json& msg, std::string& error);

// D3: plugin import runs in the helper. Its load_ok reply lists what the plugin registered, so
// EdgeSlicer can show and dispatch the capabilities without importing the plugin itself.
inline constexpr std::size_t k_max_capabilities           = 64;
inline constexpr std::size_t k_max_capability_name_bytes  = 256;
inline constexpr std::size_t k_max_config_ui_bytes        = 1024u * 1024u;
inline constexpr std::size_t k_max_permission_entries     = 256;

struct HostedCapabilityInfo
{
    std::string          name;
    PluginCapabilityType type{PluginCapabilityType::Unknown};
    bool                 has_config_ui{false};
    std::string          config_ui;      // the plugin's custom config HTML, empty without one
    nlohmann::json       default_config = nlohmann::json::object();
    bool                 lifecycle_events{false}; // the capability overrides on_lifecycle_event
};

// Strict parse of load_ok's "capabilities" list, the helper being untrusted input. At least one
// and at most k_max_capabilities entries; each an object with a non-empty "name" (at most
// k_max_capability_name_bytes, no ';' and no control characters), a known "type" string, and
// optional "has_config_ui" (bool), "config_ui" (string, at most k_max_config_ui_bytes),
// "default_config" (object) and "lifecycle_events" (bool). A (type, name) pair may appear once.
// Anything else refuses the whole reply. Never throws.
bool parse_hosted_capabilities(const nlohmann::json& load_ok, std::vector<HostedCapabilityInfo>& out, std::string& error);
// The helper side of the same list.
nlohmann::json hosted_capabilities_to_json(const std::vector<HostedCapabilityInfo>& capabilities);

// Load-time permission requests forwarded from the helper. Strict: only the known buckets, lists
// of at most k_max_permission_entries strings of at most k_max_short_string_bytes. Never throws.
nlohmann::json permissions_to_json(const PluginPermissions& permissions);
bool           permissions_from_json(const nlohmann::json& value, PluginPermissions& out, std::string& error);

inline constexpr const char* k_method_request_permissions = "plugin.request_permissions";

bool in_child_process();
void set_in_child_process();

// Child-only: RPC a host method back to Edge. Throws std::runtime_error on denial or transport failure.
nlohmann::json call_host(const std::string& method, const nlohmann::json& args = nlohmann::json::object());

// Child-only: Edge-side audit decision for a filesystem event.
AuditDecision rpc_audit_check(const std::string& event_name,
                              const std::vector<std::string>& targets,
                              bool is_write,
                              const std::string& event_type);

// Child-only (D3): a permission request a plugin made while the helper imported it. EdgeSlicer
// asks the user (it owns the install-state sidecar and the UI) and answers. False on denial or
// any transport failure. Safe with or without the GIL held.
bool rpc_request_permissions(const std::string& plugin_key, const PluginPermissions& requested);

} // namespace Slic3r::plugin_ipc
