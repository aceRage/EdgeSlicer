#include <catch2/catch.hpp>

#include <chrono>
#include <string>

#include "sentry_wrapper/SentryScrub.hpp"

using Slic3r::scrub_for_crash_report;

namespace {

bool contains(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }

// Every secret in `secrets` is gone from the scrubbed text.
void require_gone(const std::string& scrubbed, std::initializer_list<const char*> secrets)
{
    INFO("scrubbed: " << scrubbed);
    for (const char* s : secrets) {
        INFO("leaked: " << s);
        CHECK_FALSE(contains(scrubbed, s));
    }
}

} // namespace

TEST_CASE("Crash-report scrubber: Bambu LAN MQTT and camera", "[SentryScrub]")
{
    const std::string line1 = "[warning] MqttClient: connect ssl://bblp:12345678@192.168.1.23:8883 client_id=bs_4a1b "
                              "dev_id=01P00A451601234 access_code=12345678";
    const std::string out1  = scrub_for_crash_report(line1);
    require_gone(out1, {"12345678", "192.168.1.23", "01P00A451601234"});
    CHECK(contains(out1, "ssl://<redacted>@<host>:8883"));
    CHECK(contains(out1, "dev_id=<redacted>"));

    const std::string line2 = "subscribe device/01P00A451601234/report failed, rc=-3";
    const std::string out2  = scrub_for_crash_report(line2);
    require_gone(out2, {"01P00A451601234"});
    CHECK(contains(out2, "device/<serial>/report"));
    CHECK(contains(out2, "rc=-3"));

    const std::string line3 = R"(on_message: {"print":{"command":"push_status","sn":"00M09A350100123",)"
                              R"("ipcam":{"rtsp_url":"rtsps://bblp:87654321@192.168.1.23:322/streaming/live/1"},)"
                              R"("net":{"info":[{"ip":3232235799,"mask":16777215}]},"wifi_signal":"-45dBm"}})";
    const std::string out3 = scrub_for_crash_report(line3);
    require_gone(out3, {"00M09A350100123", "87654321", "192.168.1.23", "3232235799"});
    CHECK(contains(out3, "push_status"));
    CHECK(contains(out3, "-45dBm"));

    // Escaped JSON, as it appears when a payload is logged inside another string.
    const std::string line4 = R"(send \"access_code\":\"24681357\",\"dev_ip\":\"10.0.0.42\",\"dev_name\":\"Alice's X1C\")";
    const std::string out4  = scrub_for_crash_report(line4);
    require_gone(out4, {"24681357", "10.0.0.42", "Alice"});

    // A plain serial in running text, and a serial after a space-separated key.
    const std::string out5 = scrub_for_crash_report("printer 0309DA123456789 went offline; serial number 03W00X123456789");
    require_gone(out5, {"0309DA123456789", "03W00X123456789"});
    CHECK(contains(out5, "went offline"));
}

TEST_CASE("Crash-report scrubber: OctoPrint, Moonraker, U1", "[SentryScrub]")
{
    const std::string octo = "Http::perform POST http://octopi.local:5000/api/files/local header X-Api-Key: "
                             "0123456789ABCDEF0123456789ABCDEF status 401";
    const std::string out1 = scrub_for_crash_report(octo);
    require_gone(out1, {"octopi", "0123456789ABCDEF0123456789ABCDEF"});
    CHECK(contains(out1, "http://<host>:5000/api/files/local"));
    CHECK(contains(out1, "status 401"));

    const std::string moon = "Moonraker: ws://voron.lan:7125/websocket?token=AbC123xYz&client=edge failed; "
                             "GET http://192.168.1.50:7125/server/info?token=0a1b2c3d4e5f";
    const std::string out2 = scrub_for_crash_report(moon);
    require_gone(out2, {"voron", "AbC123xYz", "192.168.1.50", "0a1b2c3d4e5f", "client=edge"});
    CHECK(contains(out2, "ws://<host>:7125/websocket?token=<redacted>&client=<redacted>"));

    const std::string u1   = R"(SnapmakerU1: mqtt connect 192.168.1.60:1883 sn=SNU1A00123456 "oneshot_token": "Kx8Ls9Zq2" host: u1-kitchen)";
    const std::string out3 = scrub_for_crash_report(u1);
    require_gone(out3, {"192.168.1.60", "SNU1A00123456", "Kx8Ls9Zq2", "u1-kitchen"});
    CHECK(contains(out3, ":1883"));
}

TEST_CASE("Crash-report scrubber: FlashForge serial and check code", "[SentryScrub]")
{
    const std::string ff  = R"(FlashForge HTTP http://192.168.1.77:8898/detail body {"serialNumber":"SNMQRE9400123","checkCode":"a1b2c3d4"})";
    const std::string out = scrub_for_crash_report(ff);
    require_gone(out, {"192.168.1.77", "SNMQRE9400123", "a1b2c3d4"});
    CHECK(contains(out, "/detail"));

    const std::string legacy = "FlashForge legacy ~M601 S1 serial SNMOMC9900728 check code 12345678 on 192.168.1.78:8899";
    require_gone(scrub_for_crash_report(legacy), {"SNMOMC9900728", "12345678", "192.168.1.78"});
}

TEST_CASE("Crash-report scrubber: phone hub token and tokens in general", "[SentryScrub]")
{
    const std::string hub = "RemoteHub: spawn EdgeSlicer.exe --hidden --hub-token 3f9a2b7c1d0e4f5a6b7c8d9e0f1a2b3c --port 13640";
    const std::string out = scrub_for_crash_report(hub);
    require_gone(out, {"3f9a2b7c1d0e4f5a6b7c8d9e0f1a2b3c"});
    CHECK(contains(out, "--hub-token <redacted>"));
    CHECK(contains(out, "--port 13640"));

    require_gone(scrub_for_crash_report("RemoteHub: --hub-token=abcDEF123456 accepted"), {"abcDEF123456"});

    const std::string bearer = "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.c2lnbmF0dXJlMTIz";
    require_gone(scrub_for_crash_report(bearer), {"eyJhbGciOiJIUzI1NiJ9", "c2lnbmF0dXJlMTIz"});

    const std::string jwt = "cloud reply token eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.c2lnbmF0dXJlMTIz ok";
    require_gone(scrub_for_crash_report(jwt), {"eyJhbGciOiJIUzI1NiJ9"});

    const std::string cookie = "Set-Cookie: session=0a1b2c3d4e; token=zz9; Path=/";
    require_gone(scrub_for_crash_report(cookie), {"0a1b2c3d4e", "zz9"});

    const std::string cloud = "GET https://api.bambulab.com/v1/iot-service/api/user/bind?dev_id=01P00A451601234&access_token=xyz987";
    const std::string outc  = scrub_for_crash_report(cloud);
    require_gone(outc, {"01P00A451601234", "xyz987"});
    // Public service hosts stay: they say which API failed and identify nobody.
    CHECK(contains(outc, "https://api.bambulab.com/v1/iot-service/api/user/bind?dev_id=<redacted>&access_token=<redacted>"));

    require_gone(scrub_for_crash_report("password 12345678 rejected"), {"12345678"});
    require_gone(scrub_for_crash_report(R"({"password":"my secret, with comma","user":"bob"})"), {"my secret", "with comma", "bob"});
    require_gone(scrub_for_crash_report("api key 5f4dcc3b5aa765d61d8327deb882cf99 set"), {"5f4dcc3b5aa765d61d8327deb882cf99"});
}

TEST_CASE("Crash-report scrubber: user names in paths", "[SentryScrub]")
{
    const std::string win = R"(load C:\Users\Alice Smith\AppData\Roaming\EdgeSlicer\user\default\filament\PLA.json)";
    const std::string o1  = scrub_for_crash_report(win);
    require_gone(o1, {"Alice"});
    CHECK(contains(o1, R"(C:\Users\<user>\AppData\Roaming\EdgeSlicer)"));

    const std::string esc = R"({"path":"C:\\Users\\alice\\Desktop\\benchy.3mf"})";
    const std::string o2  = scrub_for_crash_report(esc);
    require_gone(o2, {"alice"});
    CHECK(contains(o2, R"(C:\\Users\\<user>\\Desktop\\benchy.3mf)"));

    const std::string fwd = "open C:/Users/bob/Documents/part.stl";
    CHECK(scrub_for_crash_report(fwd) == "open C:/Users/<user>/Documents/part.stl");

    const std::string mac = "datadir /Users/carol/Library/Application Support/EdgeSlicer";
    CHECK(scrub_for_crash_report(mac) == "datadir /Users/<user>/Library/Application Support/EdgeSlicer");

    const std::string lin = "config /home/dave/.config/EdgeSlicer and home /home/dave";
    CHECK(scrub_for_crash_report(lin) == "config /home/<user>/.config/EdgeSlicer and home /home/<user>");

    // Shared profile directories name nobody.
    CHECK(scrub_for_crash_report(R"(C:\Users\Public\Documents\x.3mf)") == R"(C:\Users\Public\Documents\x.3mf)");
}

TEST_CASE("Crash-report scrubber: e-mail, addresses, host names", "[SentryScrub]")
{
    const std::string o1 = scrub_for_crash_report("login ok for alice.smith+3d@example.co.uk uid=1234567890");
    require_gone(o1, {"alice", "example.co.uk", "1234567890"});
    CHECK(contains(o1, "<email>"));

    const std::string o2 = scrub_for_crash_report("serve at https://devwin11-lance.tail2dff02.ts.net:8443/phone and devwin11-lance.tail2dff02.ts.net");
    require_gone(o2, {"devwin11", "tail2dff02"});

    const std::string o3 = scrub_for_crash_report("connect [fe80::1c2d:3e4f:5a6b:7c8d]:8883 then 2001:db8:85a3:0:0:8a2e:370:7334");
    require_gone(o3, {"fe80::1c2d", "2001:db8"});

    const std::string o4 = scrub_for_crash_report("wifi mac aa-bb-cc-dd-ee-f1 and mac=AA:BB:CC:DD:EE:F2 id 123e4567-e89b-12d3-a456-426614174000");
    require_gone(o4, {"aa-bb-cc-dd-ee-f1", "AA:BB:CC:DD:EE:F2", "123e4567-e89b-12d3-a456-426614174000"});

    const std::string o5 = scrub_for_crash_report("printer at 192.168.0.5, NAS 10.1.2.3 and ssid=HomeNet5G");
    require_gone(o5, {"192.168.0.5", "10.1.2.3", "HomeNet5G"});
}

TEST_CASE("Crash-report scrubber: ordinary log lines survive", "[SentryScrub]")
{
    const char* keep[] = {
        "Slic3r::GUI::Plater::priv::on_slicing_completed took 12:34:56.789",
        "token expired, refreshing",
        "Basic settings loaded",
        "HMS 0300_0100_0001_0001 (fan speed too low)",
        "EdgeSlicer version 2.4.0.0 starting, User-Agent EdgeSlicer/2.4.0.0, tag v2.4.0.0",
        "[Version] EdgeSlicer: 2.4.0.0, Build: 01.10.01.50",
        "Slic3r::PresetBundle::load_vendor_configs_from_json::<lambda_1c5a7e2b9f0d3e4a6b8c1d2e3f405162>::operator ()",
        "firmware 01.08.02.00 on the printer",
        "listening on 127.0.0.1:13640 and 0.0.0.0:13641",
        "GET https://api.github.com/repos/aceRage/EdgeSlicer/releases/latest -> 200",
        "Layer 12 of 240: 3.2 s, extrusion width 0.45 mm",
        "",
    };
    for (const char* line : keep) {
        INFO("line: " << line);
        CHECK(scrub_for_crash_report(line) == line);
    }
}

TEST_CASE("Crash-report scrubber: long input is cut and still scrubbed", "[SentryScrub]")
{
    std::string big = "access_code=12345678 ";
    big += std::string(5000, 'x');
    const std::string out = scrub_for_crash_report(big, 256);
    CHECK(out.size() < 300);
    CHECK(contains(out, "...[truncated]"));
    require_gone(out, {"12345678"});

    // Never splits a UTF-8 sequence (the encoding check and Sentry both reject broken UTF-8).
    std::string utf = std::string(255, 'a') + "\xC3\xA9" + "tail";
    const std::string cut = scrub_for_crash_report(utf, 256);
    CHECK(cut.substr(0, 255) == std::string(255, 'a'));
    CHECK(cut.substr(255, 3) == "...");
}

TEST_CASE("Crash-report scrubber: cost per log line", "[SentryScrub]")
{
    // The scrubber runs on the logging thread for every forwarded warning, so it has to stay
    // cheap. Not a hard limit (CI machines vary), just a guard against a regex going quadratic.
    const std::string line = "[warning] MqttClient: connect ssl://bblp:12345678@192.168.1.23:8883 dev_id=01P00A451601234 "
                             R"(C:\Users\alice\AppData\Roaming\EdgeSlicer\log alice@example.com --hub-token 3f9a2b7c1d0e4f5a)";
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i)
        scrub_for_crash_report(line);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    WARN("200 scrubs took " << ms << " ms");
    CHECK(ms < 5000);
}
