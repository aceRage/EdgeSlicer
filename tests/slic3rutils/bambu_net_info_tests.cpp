// The printer-reported LAN address the Device-page camera falls back to (BambuNetInfo.hpp).
//
// Owner, 2026-10-16: on a PC whose Wi-Fi is filed as Public, Windows Firewall dropped the Bambu SSDP
// announcements, a cloud-bound X1C never got an address, and every Play on the Device page ended in
// "Connection Failed ... [1:3]" although the printer showed connected. The printer reports its own
// address in push_status net.info; these cases pin how it is read and when the camera uses it.

#include <catch2/catch.hpp>

#include "slic3r/GUI/BambuNetInfo.hpp"

#include <nlohmann/json.hpp>

using namespace Slic3r::BambuNetInfo;
using nlohmann::json;

namespace {
// The printer's encoding: first octet in the lowest byte.
long long le(unsigned a, unsigned b, unsigned c, unsigned d) { return (long long) (a | (b << 8) | (c << 16) | ((unsigned long long) d << 24)); }
} // namespace

TEST_CASE("ipv4_from_le reads the printer's little-endian address", "[BambuNetInfo]")
{
    CHECK(ipv4_from_le(le(10, 0, 0, 7)) == "10.0.0.7");
    CHECK(ipv4_from_le(le(192, 168, 1, 200)) == "192.168.1.200"); // top byte above 127: > INT32_MAX
    CHECK(ipv4_from_le(0) == "");
    // The same value sent as a signed 32-bit number reads the same.
    const long long u = le(192, 168, 1, 200);
    CHECK(ipv4_from_le(u - 4294967296LL) == "192.168.1.200");
}

TEST_CASE("first_ip takes the first interface that has an address", "[BambuNetInfo]")
{
    json net = json::parse(R"({"conf": 16, "info": [{"ip": 0, "mask": 0}, {"ip": 0, "mask": 0}]})");
    CHECK(first_ip(net) == "");

    net["info"][1]["ip"] = le(172, 16, 4, 9);
    CHECK(first_ip(net) == "172.16.4.9");

    net["info"][0]["ip"] = le(10, 1, 2, 3);
    CHECK(first_ip(net) == "10.1.2.3");
}

TEST_CASE("first_ip never throws on shapes it does not expect", "[BambuNetInfo]")
{
    CHECK(first_ip(json()) == "");
    CHECK(first_ip(json::array()) == "");
    CHECK(first_ip(json::parse(R"({"conf": 1})")) == "");
    CHECK(first_ip(json::parse(R"({"info": {"ip": 5}})")) == "");
    CHECK(first_ip(json::parse(R"({"info": ["x", 3, null, {"mask": 1}]})")) == "");
    CHECK(first_ip(json::parse(R"({"info": [{"ip": "10.0.0.7"}, {"ip": 1.5}]})")) == "");
    CHECK(first_ip(json::parse(R"({"info": [{"ip": "junk"}, {"ip": 117440522}]})")) == "10.0.0.7");
}

TEST_CASE("camera_address prefers the discovered address", "[BambuNetInfo]")
{
    CHECK(camera_address("10.0.0.5", "10.0.0.7") == "10.0.0.5"); // SSDP / saved / typed wins
    CHECK(camera_address("", "10.0.0.7") == "10.0.0.7");         // nothing discovered: the report
    CHECK(camera_address("", "") == "");                          // neither: the camera says so
}
