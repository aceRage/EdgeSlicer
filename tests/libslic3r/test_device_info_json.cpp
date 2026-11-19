#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"

#include <string>
#include <vector>

using namespace Slic3r;

// Snapmaker #768 adds nozzle_volume_type ("standard" / "high_flow" per nozzle) to the stored device
// (the Device page's high-flow hotend support). A config written before the field existed has no
// such key and must still load; everything else about the stored shape stays as it was.

namespace {
const char *OLD_DEVICE = R"json({
    "ip": "192.168.1.50", "dev_id": "SN123", "dev_name": "Bench U1", "model_name": "Snapmaker U1",
    "preset_name": "Snapmaker U1 (0.4 nozzle)", "connected": true, "img": "", "nozzle_sizes": ["0.4", "0.6"],
    "sn": "SN123", "protocol": 3, "api_key": "", "user": "", "password": "", "ca": "", "cert": "", "key": "",
    "clientId": "", "port": 1884, "link_mode": "lan", "userid": "", "id": ""
})json";
} // namespace

TEST_CASE("A device stored before nozzle_volume_type existed still loads", "[AppConfig][DeviceInfo]")
{
    const json j = json::parse(OLD_DEVICE);
    REQUIRE_FALSE(j.contains("nozzle_volume_type"));

    const DeviceInfo d = j.get<DeviceInfo>();
    CHECK(d.dev_id == "SN123");
    CHECK(d.nozzle_sizes == std::vector<std::string>{"0.4", "0.6"});
    CHECK(d.nozzle_volume_type.empty());
    CHECK(d.port == 1884);
    CHECK(d.connected);
}

TEST_CASE("nozzle_volume_type is written and read back", "[AppConfig][DeviceInfo]")
{
    DeviceInfo d = json::parse(OLD_DEVICE).get<DeviceInfo>();
    d.nozzle_volume_type = {"standard", "high_flow"};

    const json written = d;
    REQUIRE(written.contains("nozzle_volume_type"));
    CHECK(written["nozzle_volume_type"] == json::parse(R"(["standard", "high_flow"])"));

    const DeviceInfo back = written.get<DeviceInfo>();
    CHECK(back.nozzle_volume_type == d.nozzle_volume_type);
    CHECK(back.preset_name == d.preset_name);
    CHECK(back.nozzle_sizes == d.nozzle_sizes);
    CHECK(back.link_mode == "lan");
}

TEST_CASE("A device list round trips as the config stores it", "[AppConfig][DeviceInfo]")
{
    const json list = json::array({json::parse(OLD_DEVICE), json::parse(OLD_DEVICE)});
    const std::vector<DeviceInfo> devices = list.get<std::vector<DeviceInfo>>();
    REQUIRE(devices.size() == 2);
    const json again = devices;
    CHECK(again.size() == 2);
    CHECK(again[1]["dev_id"] == "SN123");
}

TEST_CASE("A device missing a required key is still rejected", "[AppConfig][DeviceInfo]")
{
    json j = json::parse(OLD_DEVICE);
    j.erase("dev_id");
    CHECK_THROWS(j.get<DeviceInfo>());
}
