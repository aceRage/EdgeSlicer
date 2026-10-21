#pragma once

// The LAN address a Bambu printer reports for itself, and the one the Device-page camera uses.
//
// A cloud-bound printer gets its address (MachineObject::dev_ip) only from its SSDP announcements
// on UDP 2021 / 1990, from a saved LAN printer or from the "enter the printer's IP" dialog. When
// Windows Firewall drops the announcements - the PC's network is filed as Public and our rules
// cover Private and Domain only - dev_ip stays empty for the whole session even though the printer
// is connected through the cloud. The live view (go2rtc, RTSPS on port 322, an outbound connection
// the firewall does not stop) then had no address, fell through to the Bambu player's cloud path,
// got no cloud URL and showed "Connection Failed ... [1:3]" on every Play (owner, 2026-10-16).
//
// The printer says where it is in every full push_status: "net": {"conf": .., "info": [{"ip": <u32>,
// "mask": <u32>}, ...]}, the address as a little-endian u32 (first octet in the lowest byte).
// Bambu Studio copies it into dev_ip on every push. Here it is kept apart (net_reported_ip) and
// only the camera falls back to it, so nothing that decides how prints are sent changes.
//
// Pure: no wx, no network, so slic3rutils_tests pins it.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace Slic3r {
namespace BambuNetInfo {

// "a.b.c.d" for the printer's little-endian u32, "" for 0 (an interface with no address).
// A signed 32-bit rendering of the same value (a negative number) reads the same.
inline std::string ipv4_from_le(long long value)
{
    const std::uint32_t v = static_cast<std::uint32_t>(value & 0xFFFFFFFFLL);
    if (v == 0) return std::string();
    return std::to_string(v & 0xFF) + "." + std::to_string((v >> 8) & 0xFF) + "." + std::to_string((v >> 16) & 0xFF) + "." +
           std::to_string((v >> 24) & 0xFF);
}

// The first non-zero address in a push_status "net" object, "" when it has none or an unexpected
// shape (never throws: this runs inside the push parser).
inline std::string first_ip(const nlohmann::json& net)
{
    if (!net.is_object()) return std::string();
    const auto it = net.find("info");
    if (it == net.end() || !it->is_array()) return std::string();
    for (const auto& item : *it) {
        if (!item.is_object()) continue;
        const auto ip = item.find("ip");
        if (ip == item.end()) continue;
        long long v = 0;
        if (ip->is_number_integer())
            v = ip->get<long long>();
        else if (ip->is_number_unsigned())
            v = static_cast<long long>(ip->get<unsigned long long>());
        else
            continue;
        const std::string text = ipv4_from_le(v);
        if (!text.empty()) return text;
    }
    return std::string();
}

// The address the camera connects to: the discovered / saved / typed one (dev_ip) always wins;
// the printer's own report only fills in while nothing else is known.
inline std::string camera_address(const std::string& dev_ip, const std::string& reported)
{
    return dev_ip.empty() ? reported : dev_ip;
}

} // namespace BambuNetInfo
} // namespace Slic3r
