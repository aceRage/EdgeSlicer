#ifndef slic3r_CrealityHostDiscovery_hpp_
#define slic3r_CrealityHostDiscovery_hpp_

#include <string>
#include <vector>

// DRAFT PLAN (C7 / upstream Orca #13752). This header is the intended API. It is
// not compiled into the app and scan() is not implemented.
//
// Why this PR stops: K-series printers announce a *per-device* service type
// `_Creality-<MAC-derived-hex>._udp.local.`, so a fixed-name Bonjour query
// (the Physical Printer "Browse" button, which CrealityPrint::has_auto_discovery
// already enables) does not see them. Upstream finds them with a DNS-SD
// meta-query (`_services._dns-sd._udp.local.`) via a vendored library:
//
//   deps_src/mdns/          (~110 KB source, new CMake static lib)
//     mdns.c / mdns.h       mjansson/mdns, public domain (~45 + 57 KB)
//     cxmdns.cpp / cxmdns.h CrealityPrint v7.1.1 wrapper, AGPL-3.0 (~8 KB)
//     CMakeLists.txt        add_library(mdns STATIC ...); MSVC + Iphlpapi Ws2_32
//
//   src/slic3r/Utils/CrealityHostDiscovery.cpp  (~120 lines)
//     cxnet::syncDiscoveryService({"Creality","creality"}), dedupe by IP,
//     optional GET http://<ip>/info for model/MAC (CrealityPrintModel table).
//
//   src/slic3r/GUI/CrealityDiscoveryDialog.*    (upstream #13752 UI)
//     Better fit here: feed IPs into PrintHostDevices (the existing device
//     store) rather than a second dialog — same pattern as the CFS probe.
//
// CMake wiring (not done): add_subdirectory(deps_src/mdns), link `mdns` into
// libslic3r_gui, add the two .cpp files to that target. That is a dependency
// bump and needs a review of the AGPL wrapper vs this fork's license.
//
// Alternative that avoids the vendor lib: extend Bonjour.cpp to send a
// DNS-SD meta-query and keep replies whose type contains "Creality". That is
// more work and still needs a multicast DNS sender/receiver; this fork's
// Bonjour client is service-name specific today.

namespace Slic3r {

struct CrealityHost
{
    std::string ip;
    std::string service_name;
    std::string hostname;
    std::string model_code;
    std::string model_name;
    std::string mac;
    bool        cfs_capable = false;
};

class CrealityHostDiscovery
{
public:
    // Would block ~5 s plus ~2-4 s per host when probe_info is true.
    // Stub: returns nothing until deps_src/mdns is vendored (see header comment).
    static std::vector<CrealityHost> scan(bool /*probe_info*/ = true) { return {}; }
};

} // namespace Slic3r

#endif
