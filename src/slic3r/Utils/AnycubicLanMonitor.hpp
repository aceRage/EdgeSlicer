#ifndef slic3r_AnycubicLanMonitor_hpp_
#define slic3r_AnycubicLanMonitor_hpp_

// Hub-side MQTT session for an Anycubic LAN printer. Same role as ElegooSdcpMonitor: one
// connection per address, demand/touch, fill_row from AnycubicLan::Snapshot. Handshake and
// TLS MQTT are untested against hardware.

#include "AnycubicLan.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r {
namespace AnycubicLanMonitor {

AnycubicLan::Snapshot touch(const std::string &host);
AnycubicLan::Snapshot demand(const std::string &host);

struct CommandResult
{
    bool        sent { false };
    bool        answered { false };
    std::string error;
};

CommandResult command(const std::string &host, const std::string &type, const std::string &action, const nlohmann::json &inner,
                      int timeout_ms);

void shutdown();
void reset_for_tests();

} // namespace AnycubicLanMonitor
} // namespace Slic3r

#endif
