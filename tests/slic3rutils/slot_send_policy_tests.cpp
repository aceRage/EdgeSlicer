// The slot send policy (PrintHostDevices::slot_send_policy) and its Elegoo CANVAS reading
// (ElegooCanvasMapping::plan_unattended_send): what the hub / phone / archive reprint does when it
// starts a print on a printer whose slots feed the tools and nobody is at a dialog.
//
// The gaps these pin down (found by the printer emulator's E2 CC1 gate, PR #659): the hub's send to
// an Elegoo Centauri Carbon 1 never read the CANVAS, so Cmd 128 never carried a slot_map (C2); a PLA
// plate went to a CANVAS holding only ASA without a word (C4); and a busy printer was uploaded to
// before the start was refused (C6). The same class of bug as the QIDI stale-mapping incident (#643).
//
// Pure functions, no network: nothing here talks to a printer.

#include <catch2/catch.hpp>

#include "slic3r/Utils/ElegooCanvas.hpp"
#include "slic3r/Utils/ElegooCanvasMapping.hpp"
#include "slic3r/Utils/PrintHostDeviceStatus.hpp"

#include <map>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::PrintHostDevices;
using Slic3r::ElegooCanvasMapping::UnattendedPlan;
using Slic3r::ElegooCanvasMapping::UsedFilament;
using Slic3r::ElegooStartPrint::SlotMapEntry;

namespace {

const std::string RED = "#D02A2A", WHITE = "#F5F5F5", BLUE = "#2050C0";

Slot tray(int pos, const std::string& type, const std::string& colour, bool loaded = true, const std::string& name = "")
{
    Slot s;
    s.index  = pos;
    s.unit   = 0;
    s.tray   = pos;
    s.loaded = loaded;
    if (loaded) {
        s.type  = type;
        s.color = colour;
        s.name  = name.empty() ? "ELEGOO " + type : name;
    }
    return s;
}

// The gate's CC1: a CANVAS with red PLA, white PLA, blue PETG and an empty tray; idle.
Status cc1(std::vector<Slot> slots, const std::string& state = "standby", int active = 0)
{
    Status st;
    st.probed               = true;
    st.online               = true;
    st.state                = state;
    st.canvas               = true;
    st.slots                = std::move(slots);
    st.active_slot          = active;
    for (const Slot& s : st.slots)
        st.slots_have_filaments = st.slots_have_filaments || s.loaded;
    return st;
}

std::vector<Slot> pla_trays() { return { tray(0, "PLA", RED), tray(1, "PLA", WHITE, true, "Generic PLA PRO"), tray(2, "PETG", BLUE), tray(3, "", "", false) }; }
std::vector<Slot> asa_trays() { return { tray(0, "ASA", RED), tray(1, "ASA", WHITE), tray(2, "", "", false), tray(3, "", "", false) }; }

UsedFilament used(int index, const std::string& type, const std::string& colour, const std::string& alias = "Elegoo PLA @ECC")
{
    UsedFilament u;
    u.index  = index;
    u.type   = type;
    u.colour = colour;
    u.alias  = alias;
    return u;
}

const std::vector<UsedFilament> RED_WHITE_PLA = { used(0, "PLA", RED), used(1, "PLA", WHITE) };
const std::map<std::string, ElegooCanvasMapping::Remembered> NO_MEMORY;
const std::map<int, int> AUTO;

UnattendedPlan plan(const Status& st, const std::vector<UsedFilament>& u = RED_WHITE_PLA, const std::map<int, int>& mapping = AUTO,
                    const std::map<std::string, ElegooCanvasMapping::Remembered>& memory = NO_MEMORY)
{
    return ElegooCanvasMapping::plan_unattended_send(st, u, memory, mapping, "CC1 test");
}

} // namespace

TEST_CASE("slot_send_policy: one rule for every slot printer", "[SlotSendPolicy]")
{
    SlotSendFacts f;
    SECTION("a printer that did not answer: not ours, unless its slots must be read")
    {
        CHECK(slot_send_policy(f) == SlotSendPolicy::NotApplicable);
        f.slots_required = true;
        CHECK(slot_send_policy(f) == SlotSendPolicy::UploadOnlyNoProbe);
        f.probe_ok = true; // answered, but not as a slot printer
        CHECK(slot_send_policy(f) == SlotSendPolicy::UploadOnlyNoProbe);
    }
    f.probe_ok  = true;
    f.slot_unit = true;
    SECTION("busy wins over everything it can see")
    {
        f.busy = true;
        CHECK(slot_send_policy(f) == SlotSendPolicy::UploadOnlyBusy);
        f.unit_in_use    = true;
        f.used_filaments = 1;
        CHECK(slot_send_policy(f) == SlotSendPolicy::UploadOnlyBusy);
    }
    SECTION("no slot unit in use: start as before")
    {
        CHECK(slot_send_policy(f) == SlotSendPolicy::Start);
    }
    f.unit_in_use    = true;
    f.used_filaments = 2;
    SECTION("every filament on a loaded slot of its material: start")
    {
        f.mapped_filaments = 2;
        CHECK(slot_send_policy(f) == SlotSendPolicy::Start);
        f.type_mismatches = 1;
        CHECK(slot_send_policy(f) == SlotSendPolicy::ConfirmTypeMismatch);
    }
    SECTION("an unmapped filament: never started, as_sliced only when nothing is mapped")
    {
        f.mapped_filaments = 1;
        CHECK(slot_send_policy(f) == SlotSendPolicy::NeedsMapping);
        f.as_sliced_ok = true;
        CHECK(slot_send_policy(f) == SlotSendPolicy::NeedsMapping);
        f.mapped_filaments = 0;
        CHECK(slot_send_policy(f) == SlotSendPolicy::StartAsSliced);
        f.as_sliced_ok = false;
        CHECK(slot_send_policy(f) == SlotSendPolicy::NeedsMapping);
    }
    SECTION("no used filament known is not 'nothing to map'")
    {
        f.used_filaments = 0;
        CHECK(slot_send_policy(f) == SlotSendPolicy::NeedsMapping);
    }
}

TEST_CASE("qidi_send_policy is the slot send policy's QIDI reading", "[SlotSendPolicy]")
{
    QidiSendFacts q;
    q.model_may_have_box = true;
    CHECK(qidi_send_policy(q) == QidiSendPolicy::UploadOnlyNoProbe);
    q.probe_ok = true;
    q.qidi     = true;
    q.box_present = true;
    q.use_box  = true;
    q.used_filaments = 1;
    CHECK(qidi_send_policy(q) == QidiSendPolicy::NeedsMapping); // never "as sliced" on a Box
    q.mapped_filaments = 1;
    CHECK(qidi_send_policy(q) == QidiSendPolicy::Start);
    q.use_box = false;
    q.mapped_filaments = 0;
    CHECK(qidi_send_policy(q) == QidiSendPolicy::Start);
}

TEST_CASE("Tray tile labels are the send dialog's", "[SlotSendPolicy]")
{
    std::vector<Slot> slots = pla_trays();
    CHECK(slot_tile_label(slots, 0) == "1A");
    CHECK(slot_tile_label(slots, 1) == "1B");
    CHECK(slot_tile_label(slots, 3) == "1D");
    Slot second = tray(0, "PLA", RED);
    second.unit = 5; // a second unit's raw id: numbered by appearance
    slots.push_back(second);
    CHECK(slot_tile_label(slots, 4) == "2A");
}

TEST_CASE("An unattended CC1 send maps the CANVAS like the desktop dialog", "[SlotSendPolicy][ElegooCanvas]")
{
    SECTION("C1: no mapping asked for - the closest colours, every tray PLA")
    {
        const UnattendedPlan p = plan(cc1(pla_trays()));
        CHECK(p.policy == SlotSendPolicy::Start);
        CHECK(p.may_start());
        CHECK(p.refusal.empty());
        CHECK(p.entries == std::vector<SlotMapEntry>{ { 0, 0, 0 }, { 1, 0, 1 } });
        std::map<std::string, std::string> info;
        ElegooCanvasMapping::add_to_extended_info(p, info);
        CHECK(info["canvasAttached"] == "1");
        CHECK(info["slotMap"] == "0:0:0,1:0:1");
    }
    SECTION("C2: the request's mapping T0 -> tray 1, T1 -> tray 0")
    {
        const UnattendedPlan p = plan(cc1(pla_trays()), RED_WHITE_PLA, { { 0, 1 }, { 1, 0 } });
        CHECK(p.policy == SlotSendPolicy::Start);
        CHECK(p.explicit_mapping);
        CHECK(p.entries == std::vector<SlotMapEntry>{ { 0, 0, 1 }, { 1, 0, 0 } });
    }
    SECTION("the remembered tray comes before the colour match")
    {
        std::map<std::string, ElegooCanvasMapping::Remembered> memory;
        ElegooCanvasMapping::remember(memory, { RED_WHITE_PLA[0] }, { 1 }, pla_trays()); // red went to the white PLA PRO
        const UnattendedPlan p = plan(cc1(pla_trays()), { RED_WHITE_PLA[0] }, AUTO, memory);
        CHECK(p.entries == std::vector<SlotMapEntry>{ { 0, 0, 1 } });
    }
    SECTION("C4: a PLA plate and only ASA in the CANVAS - refused, with the trays named")
    {
        const UnattendedPlan p = plan(cc1(asa_trays()));
        CHECK(p.policy == SlotSendPolicy::ConfirmTypeMismatch);
        CHECK(!p.may_start());
        CHECK(p.facts.type_mismatches == 2);
        CHECK(p.refusal.find("Filament 1 is PLA but tray 1A holds ASA") != std::string::npos);
        CHECK(p.refusal.find("Filament 2 is PLA but tray 1B holds ASA") != std::string::npos);
        CHECK(p.refusal.find("remap in EdgeSlicer or upload only") != std::string::npos);
    }
    SECTION("C4 with a mapping the phone chose onto ASA: still refused")
    {
        const UnattendedPlan p = plan(cc1(asa_trays()), RED_WHITE_PLA, { { 0, 1 }, { 1, 0 } });
        CHECK(p.policy == SlotSendPolicy::ConfirmTypeMismatch);
        CHECK(p.refusal.find("Filament 1 is PLA but tray 1B holds ASA") != std::string::npos);
    }
    SECTION("C6: a printing or paused printer is refused before anything goes up")
    {
        for (const char* state : { "printing", "paused" }) {
            const UnattendedPlan p = plan(cc1(pla_trays(), state));
            CHECK(p.policy == SlotSendPolicy::UploadOnlyBusy);
            CHECK(p.refusal.find("CC1 test is busy") != std::string::npos);
        }
        Status no_canvas;
        no_canvas.probed = no_canvas.online = true;
        no_canvas.state  = "printing";
        CHECK(plan(no_canvas).policy == SlotSendPolicy::UploadOnlyBusy);
    }
    SECTION("a finished print is not busy")
    {
        CHECK(plan(cc1(pla_trays(), "complete")).policy == SlotSendPolicy::Start);
    }
    SECTION("a filament mapped to an empty tray, or to a tray the CANVAS does not have")
    {
        UnattendedPlan p = plan(cc1(pla_trays()), RED_WHITE_PLA, { { 0, 0 }, { 1, 3 } });
        CHECK(p.policy == SlotSendPolicy::NeedsMapping);
        CHECK(p.refusal.find("Filament 2 (PLA) is mapped to tray 1D, which is empty") != std::string::npos);
        p = plan(cc1(pla_trays()), RED_WHITE_PLA, { { 0, 0 }, { 1, 9 } });
        CHECK(p.policy == SlotSendPolicy::NeedsMapping);
        CHECK(p.refusal.find("Filament 2 (PLA)") != std::string::npos);
    }
    SECTION("as sliced (design D4): only a one-filament plate whose material is in the active tray")
    {
        const std::vector<UsedFilament> one = { used(0, "PLA", RED) };
        UnattendedPlan                  p   = plan(cc1(pla_trays(), "standby", 1), one, { { 0, -1 } });
        CHECK(p.policy == SlotSendPolicy::StartAsSliced);
        CHECK(p.entries.empty());
        std::map<std::string, std::string> info;
        ElegooCanvasMapping::add_to_extended_info(p, info);
        CHECK(info["canvasAttached"] == "1");
        CHECK(info["slotMap"].empty());
        // The active tray holds PETG: the printer would feed it, so not as sliced.
        p = plan(cc1(pla_trays(), "standby", 2), one, { { 0, -1 } });
        CHECK(p.policy == SlotSendPolicy::NeedsMapping);
        // Two filaments as sliced: the printer cannot know which tray is which.
        p = plan(cc1(pla_trays(), "standby", 0), RED_WHITE_PLA, { { 0, -1 }, { 1, -1 } });
        CHECK(p.policy == SlotSendPolicy::NeedsMapping);
    }
    SECTION("no CANVAS attached: started as before, the command keeps its old shape")
    {
        Status st;
        st.probed = st.online = true;
        st.state  = "standby";
        const UnattendedPlan p = plan(st);
        CHECK(p.policy == SlotSendPolicy::Start);
        CHECK(!p.canvas);
        std::map<std::string, std::string> info;
        ElegooCanvasMapping::add_to_extended_info(p, info);
        CHECK(info.empty());
    }
    SECTION("a CANVAS with nothing loaded: canvasAttached with an empty slot_map, as the dialog sends")
    {
        const UnattendedPlan p = plan(cc1({ tray(0, "", "", false), tray(1, "", "", false) }));
        CHECK(p.policy == SlotSendPolicy::Start);
        CHECK(p.canvas);
        CHECK(p.entries.empty());
    }
    SECTION("a printer that did not answer is left to the upload and the start command")
    {
        Status st;
        st.probed = true;
        CHECK(plan(st).policy == SlotSendPolicy::NotApplicable);
        CHECK(plan(st).may_start());
    }
}

TEST_CASE("The CANVAS read says what the printer is doing", "[SlotSendPolicy][ElegooCanvas]")
{
    CanvasQueryResult r;
    r.reached              = true;
    r.ok                   = true;
    r.print_state          = "printing";
    r.info.active_canvas_id = 0;
    r.info.active_tray_id  = 1;
    for (int i = 0; i < 2; ++i) {
        CanvasTray t;
        t.slot_index   = i;
        t.canvas_id    = 0;
        t.tray_id      = i;
        t.has_filament = true;
        t.tray_type    = "PLA";
        t.tray_color   = "FF0000FF";
        r.info.trays.push_back(t);
    }
    const Status st = status_from_canvas(r);
    CHECK(st.state == "printing");
    CHECK(busy_printing(st.state));
    CHECK(st.active_slot == 1);

    CanvasQueryResult none;
    none.reached     = true;
    none.no_canvas   = true;
    none.print_state = "paused";
    CHECK(status_from_canvas(none).state == "paused");
}
