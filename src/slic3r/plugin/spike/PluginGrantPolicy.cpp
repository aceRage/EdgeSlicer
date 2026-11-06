#include "PluginGrantPolicy.hpp"

namespace Slic3r {
namespace PluginSpike {

bool full_access_implies_printer_send() { return false; }

bool default_printer_send_granted() { return false; }

bool plugins_may_request_full_access() { return true; }

bool full_access_confirmation_defaults_to_no() { return true; }

PrinterSendPath printer_send_path() { return PrinterSendPath::ExistingEdgeConnectionsOnly; }

} // namespace PluginSpike
} // namespace Slic3r
