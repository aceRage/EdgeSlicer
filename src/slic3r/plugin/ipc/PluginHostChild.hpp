#pragma once

#include <string>

namespace Slic3r::plugin_ipc {

class Channel;

// Distinct helper exit codes. Handshake/protocol/python/spec stay 2-5; 10-14 are
// crash diagnostics so the parent can tell abort from a clean handshake failure.
inline constexpr int k_host_exit_handshake        = 2;
inline constexpr int k_host_exit_protocol         = 3;
inline constexpr int k_host_exit_python           = 4;
inline constexpr int k_host_exit_spec             = 5;
inline constexpr int k_host_exit_uncaught_std     = 10;
inline constexpr int k_host_exit_uncaught_unknown = 11;
inline constexpr int k_host_exit_terminate        = 12;
inline constexpr int k_host_exit_invalid_param    = 13;
inline constexpr int k_host_exit_abort            = 14;

// Open (or append) a diagnostic log and dup it over stderr so CPython fatal
// errors and faulthandler dump there. Best-effort: a missing path is ignored.
void set_child_diag_log_path(const std::string& path);
void child_diag(const std::string& msg);

// Child-side session. Blocks until shutdown or a fatal transport error.
int run_child(Channel& channel);

} // namespace Slic3r::plugin_ipc
