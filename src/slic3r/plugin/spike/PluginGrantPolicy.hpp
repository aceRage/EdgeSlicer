#ifndef slic3r_plugin_spike_PluginGrantPolicy_hpp_
#define slic3r_plugin_spike_PluginGrantPolicy_hpp_

namespace Slic3r {
namespace PluginSpike {

// Edge-only grant, not present in Orca's PluginPermissionCategory. A Python printer plugin may
// send a job only after the user grants this for that specific plugin. Default is deny.
inline constexpr const char *k_printer_send_grant = "printer_send";

// Settled r4: Full access turns off the audit-hook *decision* for recognised events. It does not
// grant printer_send, and it does not give a plugin its own send stack.
bool full_access_implies_printer_send();

// Default for a newly installed plugin. Matches "Python printer plugins can't send print jobs
// without the user allowing it for that plugin first".
bool default_printer_send_granted();

// Plugins may request Full access. The native confirmation's default button is No.
bool plugins_may_request_full_access();
bool full_access_confirmation_defaults_to_no();

// With printer_send, send goes only through Edge's existing connections (UltraNet, vendor
// connectors, FlashForge). The plugin does not open its own printer socket.
enum class PrinterSendPath { ExistingEdgeConnectionsOnly };

PrinterSendPath printer_send_path();

} // namespace PluginSpike
} // namespace Slic3r

#endif
