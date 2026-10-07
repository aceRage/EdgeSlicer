// The hub's half of the Live Activity lag fix (tests/research_live_activity_lag.md):
//   A: APNs alerts for print events also carry `content-available`, so iOS may wake the app to
//      bring its Live Activity up to date ("Finished" instead of "Finishing" until it is opened).
//   B: hub-driven Live Activity pushes (LiveActivityPush.hpp), opt-in per phone: the update rules,
//      the payloads (and that they name nothing), the providers' envelopes and the registration.
//
// No socket and no push service: pure functions, and the routes driven through their handlers.

#include <catch2/catch.hpp>

#include "slic3r/GUI/AppPush.hpp"
#include "slic3r/GUI/AppPushProvider.hpp"
#include "slic3r/GUI/HostedPush.hpp"
#include "slic3r/GUI/LiveActivityPush.hpp"
#include "slic3r/GUI/PushIds.hpp"
#include "slic3r/GUI/RemoteEvents.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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

// ============================================================== hub-driven Live Activity (B) ====

namespace {

using namespace Slic3r::GUI::LiveActivity;

const long long T0 = 1789000000000LL; // ms

// A Bambu-like hub row with everything a privacy leak could come from: serial id, a name, an
// address, a file name.
json printing_row(int percent = 42, int left = 3600, const std::string& state = "RUNNING", int age_s = 4)
{
    return json{ { "id", "01P00A451201234" }, { "name", "Ace's X1C in the garage" }, { "model", "BL-P001" },
                 { "kind", "bambu" }, { "print_status", state }, { "online", true }, { "printing", true },
                 { "percent", percent }, { "left_time_s", left }, { "layer", 120 }, { "total_layers", 300 },
                 { "task", "secret_project_v3.gcode.3mf" }, { "ip", "192.168.1.77" }, { "print_error", nullptr },
                 { "stale", false }, { "age_s", age_s }, { "instance", 4242 } };
}

Row row_of(const json& j) { return row_from_json(j); }

Track registered(long long at_ms = T0)
{
    Track t;
    t.activity_token = std::string(160, 'a');
    t.job_key        = job_key("secret_project_v3.gcode.3mf");
    t.started_ms     = at_ms;
    return t;
}

// The track after a decision, as AppPush applies it when the push went out.
Track sent(const Track& t, const json& row, long long now_ms, bool can_start = false)
{
    const Row      r = row_of(row);
    const Decision d = decide(t, &r, can_start, now_ms);
    return apply(t, d, &r, now_ms);
}

void check_names_nothing(const json& payload)
{
    const std::string s = payload.dump();
    for (const char* secret : { "01P00A451201234", "Ace", "garage", "192.168", "secret_project", "BL-P001", "bambu", "4242" })
        CHECK(s.find(secret) == std::string::npos);
}

} // namespace

TEST_CASE("the phases are the app's", "[LiveActivityPush]")
{
    CHECK(base_phase("RUNNING", false) == Phase::Printing);
    CHECK(base_phase("PAUSE", true) == Phase::Paused);
    CHECK(base_phase("FINISH", false) == Phase::Finished);
    CHECK(base_phase("complete", false) == Phase::Finished);
    CHECK(base_phase("error", false) == Phase::Failed);
    CHECK(base_phase("cancelled", false) == Phase::Cancelled);
    CHECK(base_phase("PREPARE", true) == Phase::Preparing);
    CHECK(base_phase("standby", false) == Phase::Idle);
    CHECK(base_phase("whatever", true) == Phase::Printing);
    CHECK(classify("paused", true, true) == Phase::Attention);
    CHECK(classify("standby", false, true) == Phase::Idle);
    CHECK(std::string(phase_name(Phase::Attention)) == "attention");
}

TEST_CASE("the content counts from the hub's read of the row", "[LiveActivityPush]")
{
    const Content c = content_for(row_of(printing_row(25, 3600, "RUNNING", 4)), T0);
    CHECK(c.state == Phase::Printing);
    CHECK(c.percent == 25);
    CHECK(c.as_of_s == T0 / 1000 - 4);
    CHECK(c.eta_s == T0 / 1000 - 4 + 3600);
    // 25 % done with an hour left: 80 minutes in all, so the bar started 20 minutes before the read.
    CHECK(c.bar_start_s == T0 / 1000 - 4 - 20 * 60);
    CHECK(c.layer == 120);
    CHECK(c.total_layers == 300);
    CHECK_FALSE(c.stale);

    json paused = printing_row(50, 1800, "PAUSE");
    CHECK(content_for(row_of(paused), T0).eta_s == 0);
    json done = printing_row(97, 0, "FINISH");
    CHECK(content_for(row_of(done), T0).percent == 100);
    json offline = printing_row();
    offline["online"] = false;
    CHECK(content_for(row_of(offline), T0).stale);
    json error = printing_row();
    error["print_error"] = json{ { "code", 50348044 }, { "message", "Filament ran out" } };
    CHECK(content_for(row_of(error), T0).state == Phase::Attention);
}

TEST_CASE("the job key is the app's", "[LiveActivityPush]")
{
    CHECK(job_key("benchy.gcode") == "a9e667fc64ee062e");
    CHECK(job_key(" benchy.gcode ") == "a9e667fc64ee062e");
    CHECK(job_key("").empty());
}

TEST_CASE("the content-state is the app's ProgressContent and names nothing", "[LiveActivityPush]")
{
    const Content c = content_for(row_of(printing_row()), T0);
    const json    s = content_state(c);
    for (auto it = s.begin(); it != s.end(); ++it) CHECK(content_state_key_allowed(it.key()));
    CHECK(s["state"] == "printing");
    CHECK(s["percent"] == 42);
    CHECK(s["totalLayers"] == 300);
    CHECK(s["stale"] == false);
    // Dates are seconds since 2001-01-01, as Swift's default JSONDecoder reads a Date.
    CHECK(s["asOf"] == T0 / 1000 - 4 - APPLE_REFERENCE_UNIX);
    CHECK(s["eta"] == T0 / 1000 - 4 + 3600 - APPLE_REFERENCE_UNIX);
    check_names_nothing(s);
    // Absent optionals are left out, as Swift's synthesised Decodable expects.
    Content bare;
    bare.state = Phase::Paused;
    const json b = content_state(bare);
    CHECK(b.size() == 2);
    CHECK_FALSE(b.contains("percent"));
}

TEST_CASE("the aps objects carry ActivityKit's fields and nothing identifying", "[LiveActivityPush]")
{
    const Content     c      = content_for(row_of(printing_row()), T0);
    const long long   now_s  = T0 / 1000;
    const std::string opaque = "tAbCdEfGhIjKlMnOp";

    const json u = aps_update(c, now_s);
    CHECK(u["event"] == "update");
    CHECK(u["timestamp"] == now_s);
    CHECK(u["stale-date"] == now_s + 300);
    CHECK(u["relevance-score"].get<double>() == Approx(34.2));
    CHECK(u.size() == 5);
    check_names_nothing(u);

    const json e = aps_end(c, now_s, now_s + 600);
    CHECK(e["event"] == "end");
    CHECK(e["dismissal-date"] == now_s + 600);
    check_names_nothing(e);

    const json s = aps_start(c, opaque, now_s);
    CHECK(s["event"] == "start");
    CHECK(s["attributes-type"] == "PrintActivityAttributes");
    for (auto it = s["attributes"].begin(); it != s["attributes"].end(); ++it) CHECK(attribute_key_allowed(it.key()));
    CHECK(s["attributes"]["printerId"] == opaque);
    CHECK(s["attributes"]["printerName"] == "");
    CHECK(s["attributes"]["jobKey"] == "");
    CHECK(s["alert"]["title"].is_string());
    check_names_nothing(s);
    // Within ActivityKit's 4 KB with room to spare.
    CHECK(json{ { "aps", s } }.dump().size() < 1024);
}

TEST_CASE("the opaque printer id is the alerts' keyed thread id, not the serial", "[LiveActivityPush]")
{
    const std::string id = push_id("01P00A451201234");
    CHECK(id == PushIds::thread_id(PushIds::key(), "01P00A451201234"));
    CHECK(id.find("01P00A") == std::string::npos);
    CHECK(id == push_id("01P00A451201234"));
    CHECK(id != push_id("01P00A451201235"));
}

TEST_CASE("an activity registered by the app gets a routine first update", "[LiveActivityPush]")
{
    const Row      r = row_of(printing_row());
    const Decision d = decide(registered(), &r, false, T0);
    CHECK(d.action == Action::Update);
    CHECK(d.priority == 5);
}

TEST_CASE("a percent step goes at priority 5, never closer than 30 s", "[LiveActivityPush]")
{
    Track t = sent(registered(), printing_row(42), T0);
    REQUIRE(t.has_sent);
    const Row step = row_of(printing_row(43, 3500));
    CHECK(decide(t, &step, false, T0 + 10000).action == Action::None);
    const Decision d = decide(t, &step, false, T0 + 30000);
    CHECK(d.action == Action::Update);
    CHECK(d.priority == 5);
    CHECK(std::string(d.why) == "percent");
    // The layer alone is not a reason (it rides along with the next step).
    json layer_only = printing_row(42, 3600 - 60); // a minute later, the same ETA
    layer_only["layer"] = 121;
    const Row lr = row_of(layer_only);
    CHECK(decide(t, &lr, false, T0 + 60000).action == Action::None);
}

TEST_CASE("the ETA moving a minute goes at priority 5", "[LiveActivityPush]")
{
    Track     t     = sent(registered(), printing_row(42, 3600), T0);
    const Row nudge = row_of(printing_row(42, 3600 - 30 + 20)); // 30 s later with 10 s less left: the ETA moved 20 s
    CHECK(decide(t, &nudge, false, T0 + 30000).action == Action::None);
    const Row moved = row_of(printing_row(42, 3600 + 60 - 30)); // 30 s later the ETA moved 60 s
    const Decision d = decide(t, &moved, false, T0 + 30000);
    CHECK(d.action == Action::Update);
    CHECK(std::string(d.why) == "eta");
}

TEST_CASE("a state change goes at priority 10, never two within 10 s", "[LiveActivityPush]")
{
    Track     t      = sent(registered(), printing_row(), T0);
    const Row paused = row_of(printing_row(42, 3600, "PAUSE"));
    Decision  d      = decide(t, &paused, false, T0 + 2000);
    CHECK(d.action == Action::Update);
    CHECK(d.priority == 10);
    t = apply(t, d, &paused, T0 + 2000);
    const Row running = row_of(printing_row(42, 3500, "RUNNING"));
    CHECK(decide(t, &running, false, T0 + 8000).action == Action::None); // coalesced...
    d = decide(t, &running, false, T0 + 12000);                         // ...and sent on the next poll
    CHECK(d.action == Action::Update);
    CHECK(d.priority == 10);
    // An error code that changes is a state change too.
    t = apply(t, d, &running, T0 + 12000);
    json err = printing_row(42, 3500, "RUNNING");
    err["print_error"] = json{ { "code", 1 } };
    const Row er = row_of(err);
    d = decide(t, &er, false, T0 + 30000);
    CHECK(d.priority == 10);
    CHECK(d.content.state == Phase::Attention);
}

TEST_CASE("nothing new still means a heartbeat every 3 minutes", "[LiveActivityPush]")
{
    Track     t     = sent(registered(), printing_row(), T0);
    // The same percent and the same ETA (the time left shrinking with the clock).
    const Row early = row_of(printing_row(42, 3600 - 170));
    CHECK(decide(t, &early, false, T0 + 170000).action == Action::None);
    const Row same  = row_of(printing_row(42, 3600 - 180));
    const Decision d = decide(t, &same, false, T0 + 180000);
    CHECK(d.action == Action::Update);
    CHECK(std::string(d.why) == "heartbeat");
    // Which moves the stale date to five minutes past the heartbeat.
    CHECK(aps_update(d.content, (T0 + 180000) / 1000)["stale-date"] == (T0 + 180000) / 1000 + 300);
}

TEST_CASE("the end: shown ten minutes after an outcome, at once after a cancel", "[LiveActivityPush]")
{
    Track     t    = sent(registered(), printing_row(), T0);
    const Row done = row_of(printing_row(100, 0, "FINISH"));
    Decision  d    = decide(t, &done, false, T0 + 5000);
    CHECK(d.action == Action::End);
    CHECK(d.priority == 10);
    CHECK(d.dismissal_s == (T0 + 5000) / 1000 + 600);
    CHECK(d.content.state == Phase::Finished);
    const Row cancelled = row_of(printing_row(40, 0, "cancelled"));
    d = decide(t, &cancelled, false, T0 + 5000);
    CHECK(d.dismissal_s == (T0 + 5000) / 1000);
    // After an end the activity's token is dead and nothing more is sent for that print.
    const Track ended = apply(t, decide(t, &done, false, T0 + 5000), &done, T0 + 5000);
    CHECK(ended.activity_token.empty());
    CHECK(decide(ended, &done, false, T0 + 60000).action == Action::None);
    // A printer the hub no longer lists ends its activity.
    CHECK(decide(t, nullptr, false, T0 + 5000).action == Action::End);
}

TEST_CASE("at 7 h 20 m the activity is ended and started again for the same print", "[LiveActivityPush]")
{
    Track     t   = sent(registered(T0), printing_row(), T0);
    const long long later = T0 + (7 * 60 + 20) * 60 * 1000LL;
    const Row r   = row_of(printing_row(80, 3000));
    Decision  d   = decide(t, &r, true, later);
    CHECK(d.action == Action::EndAndStart);
    // Without a push-to-start token there is no restart: the updates go on until iOS ends it.
    CHECK(decide(t, &r, false, later).action != Action::EndAndStart);
    const Track restarted = apply(t, d, &r, later);
    CHECK(restarted.activity_token.empty());
    CHECK(restarted.start_sent_job == t.job_key);
    // Waiting for the app to hand over the new activity's token: no second start.
    CHECK(decide(restarted, &r, true, later + 60000).action == Action::None);
}

TEST_CASE("push-to-start: a print seen for 30 s with no activity, unless dismissed", "[LiveActivityPush]")
{
    Track t;
    t.active_since_ms = T0;
    const Row r = row_of(printing_row());
    CHECK(decide(t, &r, true, T0 + 10000).action == Action::None);  // grace: the app may start it
    CHECK(decide(t, &r, false, T0 + 40000).action == Action::None); // no push-to-start token
    Decision d = decide(t, &r, true, T0 + 40000);
    CHECK(d.action == Action::Start);
    CHECK(d.priority == 10);
    Track dismissed = t;
    dismissed.dismissed_job = job_key("secret_project_v3.gcode.3mf");
    CHECK(decide(dismissed, &r, true, T0 + 40000).action == Action::None);
    json idle_row = printing_row(0, 0, "IDLE");
    idle_row["printing"] = false;
    const Row really_idle = row_of(idle_row);
    CHECK(decide(t, &really_idle, true, T0 + 40000).action == Action::None);
}

TEST_CASE("a new print on the printer replaces the activity", "[LiveActivityPush]")
{
    Track t        = sent(registered(), printing_row(), T0);
    json  next     = printing_row(1, 7200);
    next["task"]   = "another.gcode";
    const Row r    = row_of(next);
    CHECK(decide(t, &r, true, T0 + 60000).action == Action::EndAndStart);
    CHECK(decide(t, &r, false, T0 + 60000).action == Action::End);
}

TEST_CASE("Android: a progress update stream with no tokens", "[LiveActivityPush]")
{
    Track t;
    t.virtual_activity = true;
    json  idle         = printing_row(0, 0, "IDLE");
    idle["printing"]   = false;
    const Row ir       = row_of(idle);
    CHECK(decide(t, &ir, false, T0).action == Action::None);
    const Row r  = row_of(printing_row());
    Decision  d  = decide(t, &r, false, T0);
    CHECK(d.action == Action::Update);
    t = apply(t, d, &r, T0);
    const Row done = row_of(printing_row(100, 0, "FINISH"));
    d = decide(t, &done, false, T0 + 20000);
    CHECK(d.action == Action::End);
    t = apply(t, d, &done, T0 + 20000);
    CHECK(t.virtual_activity);
    CHECK(decide(t, &done, false, T0 + 60000).action == Action::None);

    // The plaintext is end-to-end encrypted, so it may carry the row; it is still small.
    json big = printing_row();
    big["controls"] = json::array();
    for (int i = 0; i < 50; ++i) big["controls"].push_back(json{ { "id", "nozzle" + std::to_string(i) }, { "temp", 210 } });
    const json p = progress_plaintext(big, T0);
    CHECK(p["kind"] == "progress");
    CHECK(p["printer_id"] == "01P00A451201234");
    CHECK(p["row"]["percent"] == 42);
    CHECK(p["row"]["left_time_s"] == 3600);
    CHECK_FALSE(p["row"].contains("controls"));
    CHECK(p.dump().size() < 1200);
}

TEST_CASE("a four-hour print polled every 10 s costs about a push per percent", "[LiveActivityPush]")
{
    Track t = registered(T0);
    int   pushes = 0, p10 = 0;
    long long last_ms = -1, min_gap = 1LL << 60;
    const long long total_ms = 4LL * 3600 * 1000;
    for (long long el = 0; el <= total_ms; el += 10000) {
        const int  done = (int) (el * 100 / total_ms);
        const json row  = printing_row(done, (int) ((total_ms - el) / 1000), done >= 100 ? "FINISH" : "RUNNING", 3);
        const Row  r    = row_of(row);
        const Decision d = decide(t, &r, false, T0 + el);
        if (d.action == Action::None) continue;
        ++pushes;
        if (d.priority >= 10) ++p10;
        if (last_ms >= 0) min_gap = std::min(min_gap, T0 + el - last_ms);
        last_ms = T0 + el;
        t = apply(t, d, &r, T0 + el);
        if (d.action == Action::End) break;
    }
    // 100 percent steps (one every 2.4 min, so the 3 min heartbeat never fires), the first update
    // and the end; only the end is priority 10.
    CHECK(pushes >= 95);
    CHECK(pushes <= 110);
    CHECK(p10 == 1);
    CHECK(min_gap >= 30000);
}

TEST_CASE("the providers build liveactivity pushes ActivityKit's way", "[LiveActivityPush]")
{
    PushRequest r;
    r.platform     = "apns";
    r.device_token = std::string(160, 'b');
    r.bundle       = "dev.acerage.ultra1";
    r.push_type    = "liveactivity";
    r.la_aps       = aps_update(content_for(row_of(printing_row()), T0), T0 / 1000).dump();
    r.priority     = 5;
    r.ttl_seconds  = 60;
    CHECK(detail::apns_topic_for(r, "dev.acerage.ultra1") == "dev.acerage.ultra1.push-type.liveactivity");
    CHECK(std::string(detail::apns_push_type_for(r)) == "liveactivity");
    const json body = json::parse(detail::apns_liveactivity_payload(r));
    CHECK(body.size() == 1);
    CHECK(body["aps"]["event"] == "update");

    const json h = json::parse(Hosted::body_for_send(r, 1789000000, "00000000000000000000000000000001"));
    CHECK(h["push_type"] == "liveactivity");
    CHECK(h["la"]["event"] == "update");
    CHECK(h["priority"] == 5);
    CHECK(h["ttl"] == 60);
    CHECK_FALSE(h.contains("e"));
    CHECK_FALSE(h.contains("collapse"));
    CHECK_FALSE(h.contains("thread"));
    check_names_nothing(h);

    PushRequest alert;
    alert.push_type = "";
    CHECK(detail::apns_topic_for(alert, "dev.acerage.ultra1") == "dev.acerage.ultra1");
    CHECK(std::string(detail::apns_push_type_for(alert)) == "alert");

    PushRequest progress;
    progress.platform        = "fcm";
    progress.push_type       = "progress";
    progress.ciphertext_b64u = "Q2lwaGVy";
    const json pb = json::parse(Hosted::body_for_send(progress, 1, "00000000000000000000000000000001"));
    CHECK(pb["push_type"] == "progress");
    CHECK(pb["e"] == "Q2lwaGVy");
}

TEST_CASE("a phone opts in, hands over activity tokens, and opting out forgets them", "[LiveActivityPush]")
{
    AppPush::start(json::object());
    const char* P256DH = "BGD-1LolWp0xyWHrdMY1bWjASbiSO2H6bOZpYi5g8p-2eQP-EAi4vJmkGunpVii8ZPLxsgwtfp9Rd6PClNRGIpk";
    const char* AUTH   = "vYwvHnpJNtBcix4vOk1caw";
    const std::string token = "ab12cd34ab12cd34ab12cd34ab12cd34ab12cd34ab12cd34ab12cd34ab12cd34";
    json reg = { { "platform", "apns" }, { "env", "sandbox" }, { "token", token }, { "bundle", "dev.acerage.ultra1" },
                 { "p256dh", P256DH }, { "auth", AUTH }, { "label", "LA iPhone" }, { "app", "0.3.0" }, { "os", "iOS 26.1" } };
    REQUIRE(register_device(reg.dump()).first == 200);
    CHECK_FALSE(live_activity_wanted());

    const json activity = { { "platform", "apns" }, { "token", token }, { "printer", "01P00A451201234" },
                            { "job", "a9e667fc64ee062e" }, { "activity_token", std::string(160, 'c') }, { "started_at", 1789000000 } };
    CHECK(register_activity(activity.dump()).first == 409); // not opted in

    reg["live_activity"] = { { "enabled", true }, { "start_token", std::string(160, 'd') }, { "frequent", false } };
    const auto first = register_device(reg.dump());
    REQUIRE(first.first == 200);
    const json features = json::parse(first.second)["features"];
    CHECK(std::find(features.begin(), features.end(), json("live_activity")) != features.end());
    CHECK(live_activity_wanted());

    CHECK(register_activity(activity.dump()).first == 200);
    // A push-started activity the app could only name by its push_id resolves to the printer.
    json by_push_id = activity;
    by_push_id["printer"] = push_id("01P00A451201234");
    const auto resolved   = register_activity(by_push_id.dump());
    CHECK(resolved.first == 200);
    // (No printer poll has run in this test, so it resolves through the existing track.)
    CHECK(json::parse(resolved.second)["printer"] == "01P00A451201234");

    CHECK(register_activity(json{ { "platform", "apns" }, { "token", "nope" }, { "printer", "p" }, { "activity_token", std::string(160, 'c') } }.dump()).first == 404);
    CHECK(register_activity(json{ { "platform", "apns" }, { "token", token }, { "printer", "p" }, { "activity_token", "xyz" } }.dump()).first == 400);
    CHECK(register_activity(json{ { "platform", "fcm" }, { "token", token }, { "printer", "p" }, { "activity_token", std::string(160, 'c') } }.dump()).first == 400);

    // The hub page sees that it is on and how many activities, never a token.
    const json masked = masked_json();
    bool       seen   = false;
    for (const auto& d : masked["devices"])
        if (d.value("label", "") == "LA iPhone") {
            seen = true;
            CHECK(d["live_activity"]["enabled"] == true);
            CHECK(d["live_activity"]["activities"] == 1);
            CHECK(d["live_activity"]["start_token"] == true);
        }
    CHECK(seen);
    CHECK(masked.dump().find(std::string(160, 'c')) == std::string::npos);
    CHECK(masked.dump().find(std::string(160, 'd')) == std::string::npos);
    // settings.json keeps them, so a hub restart mid-print keeps updating.
    CHECK(settings_json().dump().find(std::string(160, 'c')) != std::string::npos);

    // The app ended it.
    CHECK(forget_activity(json{ { "platform", "apns" }, { "token", token }, { "printer", "01P00A451201234" } }.dump()).first == 200);
    CHECK(settings_json().dump().find(std::string(160, 'c')) == std::string::npos);

    // Opting out (or an app that stops sending the field) forgets everything at once.
    CHECK(register_activity(activity.dump()).first == 200);
    reg.erase("live_activity");
    REQUIRE(register_device(reg.dump()).first == 200);
    CHECK_FALSE(live_activity_wanted());
    CHECK(settings_json().dump().find(std::string(160, 'c')) == std::string::npos);
    CHECK(settings_json().dump().find(std::string(160, 'd')) == std::string::npos);

    // The hub page's switch.
    reg["live_activity"] = { { "enabled", true } };
    REQUIRE(register_device(reg.dump()).first == 200);
    CHECK(live_activity_wanted());
    REQUIRE(set_options(json{ { "live_activity", false } }.dump()).first == 200);
    CHECK_FALSE(live_activity_wanted());
    REQUIRE(set_options(json{ { "live_activity", true } }.dump()).first == 200);

    forget_device(json{ { "platform", "apns" }, { "token", token } }.dump());
}
