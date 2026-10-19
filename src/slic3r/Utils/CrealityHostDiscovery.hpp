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
// meta-query (`_services._dns-sd._udp.local.`) via vendored mdns.
//
// Approach (do this, not Creality's cxmdns):
//
//   1. Vendor only public-domain mjansson/mdns:
//        deps_src/mdns/mdns.c + mdns.h   (~45 + 57 KB)
//        deps_src/mdns/CMakeLists.txt    add_library(mdns STATIC mdns.c)
//                                        MSVC: Iphlpapi + Ws2_32
//      Do **not** vendor CrealityPrint's cxmdns.cpp / cxmdns.h (AGPL-3.0,
//      ~8 KB). Write a thin wrapper of our own (~100 lines) that sends the
//      DNS-SD meta-query and keeps replies whose type contains "Creality".
//
//   2. src/slic3r/Utils/CrealityHostDiscovery.cpp
//        - scan() runs **off the UI thread**
//        - every multicast send/recv step has a real deadline (same idea as
//          CrealityWsClient: async + run_for, close the socket on expiry)
//        - overall budget ~5 s for the browse, plus optional GET /info
//          per host via CrealityPrint::fetch_info (already deadline-bound)
//        - dedupe by IP; fill model/MAC from /info using CrealityPrintModel
//
//   3. Do **not** port CrealityDiscoveryDialog. Feed IPs into PrintHostDevices
//      (alias = hostname, host_type = crealityprint) so the existing send-dialog
//      device picker sees them — same architecture as the CFS probe.
//
// CMake wiring (not done): add_subdirectory(deps_src/mdns), link `mdns` into
// libslic3r_gui, add CrealityHostDiscovery.cpp. That follow-up needs a CMake
// review; this stack does not edit CMake.
//
// Alternative that avoids the vendor lib: extend Bonjour.cpp to send a
// DNS-SD meta-query. More work; this fork's Bonjour client is service-name
// specific today.

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
    // Must be called off the UI thread, with a real deadline, once implemented.
    // Stub: returns nothing until mjansson/mdns is vendored (see header comment).
    static std::vector<CrealityHost> scan(bool /*probe_info*/ = true) { return {}; }
};

} // namespace Slic3r

#endif
