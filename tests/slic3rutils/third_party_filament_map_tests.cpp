// The shared Bambu-style filament map of the third-party send dialogs (ThirdPartyFilamentMap.hpp):
// how each host's slots are labelled on the swatch tiles, which slot each filament row starts on
// (remembered mapping, closest colour, QIDI Box material-first), the mapping string it sends, the
// mismatch rules and the nozzle badges.
//
// wx-free and printer-free: the CANVAS and Moonraker answers are mocks, the remembered mappings an
// in-memory map.

#include <catch2/catch.hpp>

#include <map>
#include <string>
#include <vector>

#include "slic3r/GUI/ThirdPartyFilamentMap.hpp"
#include "slic3r/Utils/ElegooCanvas.hpp"
#include "slic3r/Utils/ElegooCanvasMapping.hpp"
#include "slic3r/Utils/PrintHostDeviceStatus.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using PrintHostDevices::Slot;
using PrintHostDevices::Status;

namespace {

// CANVAS units as the printer may send them: `canvas_id` and the first `tray_id` are whatever base
// the firmware counts from. Each unit: PLA red, PETG black, empty, PLA white.
std::string canvas_unit(int canvas_id, int tray_base)
{
    auto t = [&](int k) { return std::to_string(tray_base + k); };
    return R"({"canvas_id":)" + std::to_string(canvas_id) + R"(,"connected":1,"tray_list":[
        {"tray_id":)" + t(0) + R"(,"brand":"ELEGOO","filament_type":"PLA","filament_name":"PLA","filament_color":"#FF0000","min_nozzle_temp":190,"max_nozzle_temp":230,"status":2},
        {"tray_id":)" + t(1) + R"(,"brand":"ELEGOO","filament_type":"PETG","filament_name":"PETG","filament_color":"#000000","min_nozzle_temp":220,"max_nozzle_temp":250,"status":2},
        {"tray_id":)" + t(2) + R"(,"brand":"","filament_type":"","filament_name":"","filament_color":"","status":0},
        {"tray_id":)" + t(3) + R"(,"brand":"","filament_type":"PLA","filament_name":"","filament_color":"#FFFFFF","min_nozzle_temp":190,"max_nozzle_temp":230,"status":1}]})";
}

Status canvas_status(const std::vector<std::string>& units)
{
    std::string list;
    for (const std::string& u : units)
        list += (list.empty() ? "" : ",") + u;
    const std::string payload = R"({"active_canvas_id":0,"active_tray_id":-1,"canvas_list":[)" + list + "]}";
    CanvasQueryResult r;
    std::string       error;
    r.reached            = true;
    r.ams_connect_status = 1;
    r.ok                 = canvas_parse_payload(payload, r.info, error);
    REQUIRE(r.ok);
    return PrintHostDevices::status_from_canvas(r);
}

// A Snapmaker U1: four toolheads named by print_task_config.
Status u1_status()
{
    return PrintHostDevices::parse_moonraker_status(
        "{\"print_stats\":{\"state\":\"standby\"},"
        " \"print_task_config\":{"
        "   \"filament_type\":[\"PLA\",\"PETG\",\"PLA\",\"\"],"
        "   \"filament_color_rgba\":[\"FF0000FF\",\"0000FFFF\",\"FFFFFFFF\",\"\"],"
        "   \"filament_exist\":[true,true,true,false]}}",
        "octoprint");
}

// A QIDI X-Max 4 with one Box: white PLA, dark ASA, an unloaded slot, ABS with no colour.
Status qidi_status()
{
    return PrintHostDevices::parse_moonraker_status(
        "{\"print_stats\":{\"state\":\"standby\"},"
        " \"extruder\":{\"nozzle_diameter\":0.4},"
        " \"multi_color_controller\":{\"box_count\":1,\"box\":{\"boxes\":[{"
        "   \"box_inserted_0\":1,\"box_inserted_1\":1,\"box_inserted_2\":0,\"box_inserted_3\":1,"
        "   \"filament_info\":["
        "     {\"filament_type\":\"PLA\",\"color_rgb_str\":[\"#FFFFFF\"]},"
        "     {\"filament_type\":\"ASA\",\"color_rgb_str\":[\"#FF1010\"]},"
        "     {\"filament_type\":\"PETG\",\"color_rgb_str\":[\"#00FF00\"]},"
        "     {\"filament_type\":\"ABS\",\"color_rgb_str\":[]}]}]}}}",
        "octoprint");
}

std::vector<std::string> labels_of(const std::vector<ThirdPartyDeviceSlot>& slots)
{
    std::vector<std::string> out;
    for (const ThirdPartyDeviceSlot& s : slots)
        out.push_back(slot_display_label(s));
    return out;
}

} // namespace

TEST_CASE("Third-party filament map: CANVAS tiles are labelled by position, whatever base the tray ids use", "[ThirdPartyFilamentMap]")
{
    const std::vector<std::string> one_unit = { "1A", "1B", "1C", "1D" };

    SECTION("0-based tray ids (the CC1 capture)") {
        const Status st = canvas_status({ canvas_unit(0, 0) });
        REQUIRE(st.slots.size() == 4);
        const auto tiles = third_party_slots_from(st.slots, st.qidi_box);
        CHECK(labels_of(tiles) == one_unit);
        CHECK(tiles[2].empty);
        CHECK(tiles[1].type == "PETG");
    }
    SECTION("1-based tray ids never read B, C, D, A") {
        const Status st = canvas_status({ canvas_unit(0, 1) });
        CHECK(labels_of(third_party_slots_from(st.slots, st.qidi_box)) == one_unit);
    }
    SECTION("a 1-based canvas_id is still unit 1") {
        const Status st = canvas_status({ canvas_unit(1, 1) });
        CHECK(labels_of(third_party_slots_from(st.slots, st.qidi_box)) == one_unit);
    }
    SECTION("two units: the second is 2A..2D, and the protocol index is kept") {
        const Status st    = canvas_status({ canvas_unit(0, 0), canvas_unit(1, 0) });
        const auto   tiles = third_party_slots_from(st.slots, st.qidi_box);
        REQUIRE(tiles.size() == 8);
        CHECK(slot_display_label(tiles[4]) == "2A");
        CHECK(slot_display_label(tiles[7]) == "2D");
        for (size_t i = 0; i < tiles.size(); ++i)
            CHECK(tiles[i].index == st.slots[i].index);
        const auto groups = group_slots(tiles);
        REQUIRE(groups.size() == 2);
        CHECK(groups[1].slot_positions == std::vector<int>{ 4, 5, 6, 7 });
    }
}

TEST_CASE("Third-party filament map: U1 toolheads are T1..T4", "[ThirdPartyFilamentMap]")
{
    const Status st = u1_status();
    REQUIRE(st.slots.size() == 4);
    REQUIRE_FALSE(st.qidi_box);
    const auto tiles = third_party_slots_from(st.slots, st.qidi_box);
    CHECK(labels_of(tiles) == std::vector<std::string>{ "T1", "T2", "T3", "T4" });
    CHECK(tiles[0].unit_id == -1);
    CHECK(tiles[3].empty);
    CHECK(tiles[1].colour == "#0000FF");
    // One group, no unit heading.
    REQUIRE(group_slots(tiles).size() == 1);
}

TEST_CASE("Third-party filament map: QIDI Box slots are 1A..1D", "[ThirdPartyFilamentMap]")
{
    const Status st = qidi_status();
    REQUIRE(st.qidi_box);
    REQUIRE(st.slots.size() == 4);
    const auto tiles = third_party_slots_from(st.slots, st.qidi_box);
    CHECK(labels_of(tiles) == std::vector<std::string>{ "1A", "1B", "1C", "1D" });
    CHECK(tiles[1].index == 1); // Box slot b * 4 + n, what value_t<n> names
    CHECK(tiles[2].empty);

    // A second Box: slots 4..7 are 2A..2D.
    std::vector<Slot> two(st.slots);
    for (int i = 0; i < 4; ++i) {
        Slot s  = st.slots[i];
        s.index = 4 + i;
        two.push_back(s);
    }
    CHECK(labels_of(third_party_slots_from(two, true)) ==
          std::vector<std::string>{ "1A", "1B", "1C", "1D", "2A", "2B", "2C", "2D" });
}

TEST_CASE("Third-party filament map: each host keeps its own auto-match", "[ThirdPartyFilamentMap]")
{
    SECTION("U1 and CANVAS: the closest colour among loaded slots") {
        const Status u1 = u1_status();
        // Dark red PETG goes to the red PLA head: colour only, the type does not count.
        CHECK(initial_slot_position(u1.slots, -1, "#E00000", "PETG", u1.qidi_box) == 0);
        CHECK(initial_slot_position(u1.slots, -1, "#0010F0", "PLA", u1.qidi_box) == 1);
        // The unloaded T4 is never picked, even for a colour it would match.
        CHECK(initial_slot_position(u1.slots, -1, "#FFFFFF", "PLA", u1.qidi_box) == 2);

        const Status cv = canvas_status({ canvas_unit(0, 1) });
        CHECK(initial_slot_position(cv.slots, -1, "#101010", "PLA", cv.qidi_box) == 1); // black PETG tray
    }
    SECTION("QIDI Box: the filament's own material first, never colour alone") {
        const Status q = qidi_status();
        // Red PLA: the only PLA is white, still preferred over the red ASA.
        CHECK(initial_slot_position(q.slots, -1, "#FF0000", "PLA", q.qidi_box) == 0);
        // ABS: the loaded ABS with no colour.
        CHECK(initial_slot_position(q.slots, -1, "#FF0000", "ABS", q.qidi_box) == 3);
        // PETG: the only PETG slot is unloaded, so "As sliced".
        CHECK(initial_slot_position(q.slots, -1, "#00FF00", "PETG", q.qidi_box) == -1);
        // No type: "As sliced".
        CHECK(initial_slot_position(q.slots, -1, "#FFFFFF", "", q.qidi_box) == -1);
        // The same slots treated as a non-Box host would follow the colour instead.
        CHECK(initial_slot_position(q.slots, -1, "#FF0000", "PLA", false) == 1);
    }
}

TEST_CASE("Third-party filament map: a remembered CANVAS mapping wins over the colour match", "[ThirdPartyFilamentMap]")
{
    const Status st = canvas_status({ canvas_unit(0, 1) });

    ElegooCanvasMapping::UsedFilament red;
    red.index  = 0;
    red.type   = "PLA";
    red.colour = "#FF0000";
    red.alias  = "Generic PLA";

    // Without memory the red filament goes to the red tray (1A).
    std::map<std::string, ElegooCanvasMapping::Remembered> memory;
    int remembered = ElegooCanvasMapping::recall(memory, red, st.slots);
    CHECK(remembered == -1);
    CHECK(initial_slot_position(st.slots, remembered, red.colour, red.type, false) == 0);

    // Last time it printed from the white PLA tray (1D): that is where it starts now.
    ElegooCanvasMapping::remember(memory, { red }, { 3 }, st.slots);
    remembered = ElegooCanvasMapping::recall(memory, red, st.slots);
    CHECK(remembered == 3);
    CHECK(initial_slot_position(st.slots, remembered, red.colour, red.type, false) == 3);

    // The row encodes to the protocol index and the tile reads 1D.
    ThirdPartyFilamentMapModel model;
    ThirdPartySlicerFilament   f;
    f.index         = 0;
    f.type          = "PLA";
    f.colour        = "#FF0000";
    model.filaments = { f };
    model.slots     = third_party_slots_from(st.slots, st.qidi_box);
    model.mapping   = { initial_slot_position(st.slots, remembered, f.colour, f.type, false) };
    CHECK(slot_display_label(model.slots[model.mapping[0]]) == "1D");
    CHECK(encode_filament_mapping(model) == "0:" + std::to_string(st.slots[3].index));

    // A remembered position outside the slot list is ignored.
    CHECK(initial_slot_position(st.slots, 99, red.colour, red.type, false) == 0);
}

TEST_CASE("Third-party filament map: Bambu-style slot labels and grouping", "[ThirdPartyFilamentMap]")
{
    CHECK(bambu_style_slot_label(0, 0) == "1A");
    CHECK(bambu_style_slot_label(0, 1) == "1B");
    CHECK(bambu_style_slot_label(0, 3) == "1D");
    CHECK(bambu_style_slot_label(0, 4) == "1E"); // positions, so no wrap back to A
    CHECK(bambu_style_slot_label(1, 0) == "2A");
    CHECK(bambu_style_slot_label(0, 26) == "1-27");
    CHECK(bambu_style_slot_label(-1, 0) == "1");
    CHECK(bambu_style_slot_label(-1, 2) == "3");
    CHECK(bambu_style_slot_label(-1, 4) == "5");
    CHECK(bambu_style_slot_label(-1, 7) == "8");

    ThirdPartyDeviceSlot unitless;
    unitless.unit_id = -1;
    unitless.slot_id = -1;
    unitless.index   = 5;
    CHECK(slot_display_label(unitless) == "6");

    ThirdPartyDeviceSlot a;
    a.unit_id = 0;
    a.slot_id = 0;
    a.label   = "custom";
    CHECK(slot_display_label(a) == "custom");
    a.label.clear();
    CHECK(slot_display_label(a) == "1A");

    std::vector<ThirdPartyDeviceSlot> slots(6);
    slots[0].unit_id   = 0;
    slots[0].slot_id   = 0;
    slots[1].unit_id   = 0;
    slots[1].slot_id   = 1;
    slots[2].unit_id   = 1;
    slots[2].slot_id   = 0;
    slots[2].nozzle_id = 1;
    slots[3].unit_id   = 1;
    slots[3].slot_id   = 1;
    slots[3].nozzle_id = 1;
    slots[4].unit_id   = 0;
    slots[4].slot_id   = 2;
    slots[5].unit_id   = 0;
    slots[5].slot_id   = 3;
    const auto groups = group_slots(slots);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].unit_id == 0);
    CHECK(groups[0].slot_positions == std::vector<int>{ 0, 1, 4, 5 });
    CHECK(groups[1].unit_id == 1);
    CHECK(groups[1].nozzle_id == 1);
    CHECK(groups[1].slot_positions == std::vector<int>{ 2, 3 });
}

TEST_CASE("Third-party filament map: encode matches the send dialog's filament:slot string", "[ThirdPartyFilamentMap]")
{
    ThirdPartyFilamentMapModel model;
    ThirdPartySlicerFilament   f0;
    f0.index = 0;
    ThirdPartySlicerFilament f1;
    f1.index        = 2;
    model.filaments = { f0, f1 };
    ThirdPartyDeviceSlot s0;
    s0.index = 5;
    ThirdPartyDeviceSlot s1;
    s1.index      = 7;
    model.slots   = { s0, s1 };
    model.mapping = { 1, -1 };
    CHECK(encode_filament_mapping(model) == "0:7");
    model.mapping = { 0, 1 };
    CHECK(encode_filament_mapping(model) == "0:5,2:7");
    model.mapping = { -1, -1 };
    CHECK(encode_filament_mapping(model).empty());
    model.mapping   = { 0, 1 };
    model.read_only = true;
    CHECK(encode_filament_mapping(model).empty());
    model.read_only = false;
    CHECK(encode_filament_mapping(model) == "0:5,2:7");
}

TEST_CASE("Third-party filament map: read-only mismatch uses T-number identity, not auto-match", "[ThirdPartyFilamentMap]")
{
    ThirdPartyFilamentMapModel model;
    ThirdPartySlicerFilament   f;
    f.index         = 0;
    f.type          = "PLA";
    f.colour        = "#FF0000";
    model.filaments = { f };

    ThirdPartyDeviceSlot t0;
    t0.index  = 0;
    t0.type   = "PETG";
    t0.colour = "#0000FF";
    ThirdPartyDeviceSlot t1;
    t1.index      = 1;
    t1.type       = "PLA";
    t1.colour     = "#FF0000";
    model.slots   = { t0, t1 };
    model.mapping = { 1 }; // visual auto-match to the matching holder

    CHECK(mismatch_at(model, 0) == ThirdPartyMapMismatch::None);
    CHECK(warning_slot_position(model, 0) == 1);

    model.read_only = true;
    CHECK(encode_filament_mapping(model).empty());
    CHECK(warning_slot_position(model, 0) == 0);
    CHECK(mismatch_at(model, 0) == ThirdPartyMapMismatch::TypeAndColour);
    CHECK(model.mapping == std::vector<int>{ 1 });
}

TEST_CASE("Third-party filament map: type and colour mismatch, colour parse", "[ThirdPartyFilamentMap]")
{
    CHECK(normalize_map_colour("#ff00aa") == "#FF00AA");
    CHECK(normalize_map_colour("00FF00BB") == "#00FF00");
    CHECK(normalize_map_colour("not-a-colour").empty());
    CHECK(types_match("PLA", "pla"));
    CHECK(types_match("PLA", ""));
    CHECK_FALSE(types_match("PLA", "PETG"));
    CHECK_FALSE(colours_mismatch("#FF0000", "#EE0000"));
    CHECK(colours_mismatch("#FF0000", "#0000FF"));
    CHECK_FALSE(colours_mismatch("#FF0000", ""));

    ThirdPartySlicerFilament f;
    f.index  = 0;
    f.type   = "PLA";
    f.colour = "#FF0000";
    ThirdPartyDeviceSlot s;
    s.type   = "PETG";
    s.colour = "#0000FF";
    CHECK(mismatch_for(f, s) == ThirdPartyMapMismatch::TypeAndColour);
    s.type = "PLA";
    CHECK(mismatch_for(f, s) == ThirdPartyMapMismatch::Colour);
    s.colour = "#FF0000";
    CHECK(mismatch_for(f, s) == ThirdPartyMapMismatch::None);
    s.empty = true;
    s.type  = "PETG";
    CHECK(mismatch_for(f, s) == ThirdPartyMapMismatch::None);
}

TEST_CASE("Third-party filament map: nozzle badges", "[ThirdPartyFilamentMap]")
{
    ThirdPartyFilamentMapModel model;
    ThirdPartySlicerFilament   a;
    a.index         = 0;
    a.nozzle_id     = -1;
    model.filaments = { a };
    CHECK(filament_badge_text(a, model) == "1");
    a.index         = 3;
    model.filaments = { a };
    CHECK(filament_badge_text(a, model) == "4");

    ThirdPartySlicerFilament left;
    left.index     = 1;
    left.nozzle_id = 1;
    ThirdPartySlicerFilament right;
    right.index     = 0;
    right.nozzle_id = 0;

    SECTION("slots that name no nozzle (U1 toolheads, CANVAS): the filament number, whatever filament_map says") {
        model.filaments = { right, left };
        model.slots     = third_party_slots_from(u1_status().slots, false);
        CHECK(has_multiple_nozzles(model));
        CHECK(filament_badge_text(right, model) == "1");
        CHECK(filament_badge_text(left, model) == "2");
    }
    SECTION("slots grouped by nozzle: L / R on two nozzles, numbers on more") {
        ThirdPartyDeviceSlot r;
        r.nozzle_id = 0;
        ThirdPartyDeviceSlot l;
        l.nozzle_id     = 1;
        model.slots     = { r, l };
        model.filaments = { right, left };
        CHECK(filament_badge_text(right, model) == "R");
        CHECK(filament_badge_text(left, model) == "L");

        ThirdPartySlicerFilament third;
        third.nozzle_id = 2;
        model.filaments = { right, left, third };
        CHECK(filament_badge_text(right, model) == "1");
        CHECK(filament_badge_text(third, model) == "3");
    }
    SECTION("one nozzle is not several") {
        model.filaments = { right };
        CHECK_FALSE(has_multiple_nozzles(model));
    }
}
