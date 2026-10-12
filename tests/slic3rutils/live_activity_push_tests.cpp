// The hub's half of the Live Activity lag fix (tests/research_live_activity_lag.md, plan A4): APNs
// alerts for print events also carry `content-available`, so iOS may wake the app to bring its Live
// Activity up to date ("Finished" instead of "Finishing" until the app is next opened).
//
// Pure functions only: the payload builders and the policy. No socket, no push service.

#include <catch2/catch.hpp>

#include "slic3r/GUI/AppPush.hpp"
#include "slic3r/GUI/AppPushProvider.hpp"
#include "slic3r/GUI/HostedPush.hpp"
#include "slic3r/GUI/RemoteEvents.hpp"

#include <nlohmann/json.hpp>

#include <string>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::AppPush;
using json = nlohmann::json;

namespace {

PushRequest alert_request()
{
    PushRequest r;
    r.platform        = "apns";
    r.device_token    = std::string(64, 'a');
    r.env             = "production";
    r.bundle          = "dev.acerage.ultra1";
    r.ciphertext_b64u = "Q2lwaGVydGV4dA";
    r.collapse_id     = "AbCdEfGhIjKlMnOpQrStUvWx";
    r.thread_id       = "tAbCdEfGhIjKlMnOp";
    r.priority        = 10;
    r.ttl_seconds     = 1800;
    return r;
}

} // namespace

TEST_CASE("every print-state kind wakes the app; nothing else does", "[LiveActivityPush]")
{
    for (const std::string& k : RemoteEvents::all_kinds()) CHECK(policy::wakes_app(k));
    CHECK(policy::wakes_app("finished"));
    CHECK(policy::wakes_app("runout"));
    CHECK_FALSE(policy::wakes_app(""));
    CHECK_FALSE(policy::wakes_app("progress"));
    CHECK_FALSE(policy::wakes_app("FINISHED"));
}

TEST_CASE("the APNs alert payload adds content-available only when asked", "[LiveActivityPush]")
{
    PushRequest r = alert_request();
    json        plain = json::parse(detail::apns_alert_payload(r));
    CHECK_FALSE(plain["aps"].contains("content-available"));
    CHECK(plain["aps"]["alert"]["title"] == "Printer update");
    CHECK(plain["aps"]["alert"]["body"] == "Tap to open");
    CHECK(plain["aps"]["mutable-content"] == 1);
    CHECK(plain["aps"]["thread-id"] == "tAbCdEfGhIjKlMnOp");
    CHECK(plain["e"] == "Q2lwaGVydGV4dA");
    CHECK(plain["v"] == 1);

    r.content_available = true;
    const json woken = json::parse(detail::apns_alert_payload(r));
    CHECK(woken["aps"]["content-available"] == 1);
    // Still an alert with the same placeholder: the wake adds one flag and nothing readable.
    CHECK(woken["aps"]["alert"] == plain["aps"]["alert"]);
    CHECK(woken["aps"]["mutable-content"] == 1);
    CHECK(woken["e"] == plain["e"]);
    json without = woken;
    without["aps"].erase("content-available");
    CHECK(without == plain);
}

TEST_CASE("the hosted send body says wake only for an APNs request that asks for it", "[LiveActivityPush]")
{
    PushRequest r = alert_request();
    CHECK_FALSE(json::parse(Hosted::body_for_send(r, 1789000000, "00000000000000000000000000000001")).contains("wake"));
    r.content_available = true;
    const json b = json::parse(Hosted::body_for_send(r, 1789000000, "00000000000000000000000000000001"));
    CHECK(b["wake"] == true);
    // FCM has no such thing: its data messages already reach the app's service.
    r.platform = "fcm";
    CHECK_FALSE(json::parse(Hosted::body_for_send(r, 1789000000, "00000000000000000000000000000001")).contains("wake"));
}
