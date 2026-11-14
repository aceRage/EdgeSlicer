#include <catch2/catch.hpp>

#include <climits>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "slic3r/GUI/AmsMatch.hpp"
#include "slic3r/Utils/ColorSpaceConvert.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

// The AMS auto-match of the Bambu send dialog and the AMS filament blacklist
// (slic3r/GUI/AmsMatch.cpp, called by MachineObject::ams_filament_mapping and
// DeviceManager::check_filaments_in_blacklist).
//
// "main" below is the code as it was before this change (DeviceManager.cpp at 156e9591fe,
// l.891-898, l.1130-1180, l.8680-8700), copied here so each test shows what it did.

namespace {

// main's colour distance: RGB2Lab handed 0..255 although it expects 0..1.
float main_colour_distance(const AmsMatch::Rgba& a, const AmsMatch::Rgba& b)
{
    float lab[2][3];
    RGB2Lab(a.r, a.g, a.b, &lab[0][0], &lab[0][1], &lab[0][2]);
    RGB2Lab(b.r, b.g, b.b, &lab[1][0], &lab[1][1], &lab[1][2]);
    return DeltaE76(lab[0][0], lab[0][1], lab[0][2], lab[1][0], lab[1][1], lab[1][2]);
}

// CIEDE2000 on 0..1, an independent judge for the table below.
float de2000(const AmsMatch::Rgba& a, const AmsMatch::Rgba& b)
{
    float lab[2][3];
    RGB2Lab(a.r / 255.f, a.g / 255.f, a.b / 255.f, &lab[0][0], &lab[0][1], &lab[0][2]);
    RGB2Lab(b.r / 255.f, b.g / 255.f, b.b / 255.f, &lab[1][0], &lab[1][1], &lab[1][2]);
    return DeltaE00(lab[0][0], lab[0][1], lab[0][2], lab[1][0], lab[1][1], lab[1][2]);
}

// main's tie-break: it looked the tray up in the tray map (keyed by tray index) by column number,
// with operator[] (so a missing key is an empty tray). Returns the picked tray id per filament.
std::vector<int> main_pick(const std::vector<FilamentInfo>& filaments, std::map<int, FilamentInfo> tray_filaments,
                           const std::vector<std::vector<AmsMatch::Distance>>& distance_map)
{
    std::vector<int> picked(filaments.size(), -1);
    std::set<int>    picked_src, picked_tar;
    for (size_t k = 0; k < distance_map.size(); k++) {
        float min_val        = INT_MAX;
        int   picked_src_idx = -1;
        int   picked_tar_idx = -1;
        for (int i = 0; i < int(distance_map.size()); i++) {
            if (picked_src.count(i))
                continue;
            for (int j = 0; j < int(distance_map[i].size()); j++) {
                if (picked_tar.count(j)) {
                    if (distance_map[i][j].is_type_match && distance_map[i][j].distance < (float) 0.0001) {
                        min_val        = distance_map[i][j].distance;
                        picked_src_idx = i;
                        picked_tar_idx = j;
                    }
                    continue;
                }
                if (distance_map[i][j].is_type_match) {
                    if (min_val > distance_map[i][j].distance) {
                        min_val        = distance_map[i][j].distance;
                        picked_src_idx = i;
                        picked_tar_idx = j;
                    } else if (min_val == distance_map[i][j].distance &&
                               filaments[picked_src_idx].filament_id != tray_filaments[picked_tar_idx].filament_id &&
                               filaments[i].filament_id == tray_filaments[j].filament_id) {
                        picked_src_idx = i;
                        picked_tar_idx = j;
                    }
                }
            }
        }
        if (picked_src_idx >= 0 && picked_tar_idx >= 0) {
            picked[picked_src_idx] = distance_map[picked_src_idx][picked_tar_idx].tray_id;
            picked_src.insert(picked_src_idx);
            picked_tar.insert(picked_tar_idx);
        }
    }
    return picked;
}

// main's blacklist decision (vendor compare as written there).
bool main_in_blacklist(const nlohmann::json& list, std::string tag_vendor, std::string tag_type, std::string& action)
{
    for (auto f : list["blacklist"]) {
        std::string vendor = f["vendor"].get<std::string>(), type = f["type"].get<std::string>();
        for (std::string* s : {&vendor, &tag_vendor, &tag_type, &type})
            std::transform(s->begin(), s->end(), s->begin(), ::tolower);
        if (vendor == "third party") {
            if ("bambulab" != vendor && tag_type == type) { action = f["action"]; return true; }
        } else if (vendor == tag_vendor && tag_type == type) { action = f["action"]; return true; }
    }
    return false;
}

std::string hex2(int v)
{
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02X", v);
    return buf;
}

FilamentInfo filament(int id, const std::string& type, const AmsMatch::Rgba& c, const std::string& preset = "")
{
    FilamentInfo f;
    f.id          = id;
    f.type        = type;
    f.color       = "#" + hex2(c.r) + hex2(c.g) + hex2(c.b) + hex2(c.a);
    f.filament_id = preset;
    return f;
}

FilamentInfo tray(int index, const std::string& type, const AmsMatch::Rgba& c, const std::string& preset = "")
{
    FilamentInfo t;
    t.id          = index;
    t.type        = type;
    t.color       = hex2(c.r) + hex2(c.g) + hex2(c.b) + hex2(c.a);
    t.filament_id = preset;
    t.ams_id      = std::to_string(index / 4);
    t.slot_id     = std::to_string(index % 4);
    return t;
}

int pick(const std::vector<FilamentInfo>& filaments, const std::map<int, FilamentInfo>& trays, size_t i = 0)
{
    return AmsMatch::nearest_colour(filaments, trays, AmsMatch::distances(filaments, trays))[i].tray_id;
}

const std::vector<std::pair<const char*, AmsMatch::Rgba>> kNamed = {
    {"red", {255, 0, 0}},   {"orange", {255, 128, 0}}, {"dark red", {128, 0, 0}}, {"pink", {255, 105, 180}}, {"black", {0, 0, 0}},
    {"grey", {128, 128, 128}}, {"white", {255, 255, 255}}, {"navy", {0, 0, 128}},  {"blue", {0, 0, 255}},     {"purple", {128, 0, 128}},
};

} // namespace

TEST_CASE("AMS match: the colour distance is CIE76 on sRGB in 0..1", "[AmsMatch]")
{
    const AmsMatch::Rgba black{0, 0, 0}, white{255, 255, 255}, red{255, 0, 0};
    CHECK(AmsMatch::colour_distance(black, white) == Approx(100.f).margin(0.05f));
    CHECK(AmsMatch::colour_distance(red, red) == 0.f);
    // sRGB red is L*a*b* (53.24, 80.09, 67.20): its distance to black is the vector's length.
    CHECK(AmsMatch::colour_distance(black, red) == Approx(117.34f).margin(0.1f));
    // main's numbers were nowhere near (black to white about 9500).
    CHECK(main_colour_distance(black, white) > 5000.f);
}

TEST_CASE("AMS match: colours are read as the send dialog and the printer write them", "[AmsMatch]")
{
    const AmsMatch::Rgba f = AmsMatch::filament_colour("#FF8000FF");
    CHECK((f.r == 255 && f.g == 128 && f.b == 0 && f.a == 255));
    const AmsMatch::Rgba g = AmsMatch::filament_colour("#00FF0080");
    CHECK((g.g == 255 && g.a == 128));
    const AmsMatch::Rgba t = AmsMatch::tray_colour("FF800000");
    CHECK((t.r == 255 && t.g == 128 && t.b == 0 && t.a == 0));
    const AmsMatch::Rgba w = AmsMatch::tray_colour("");
    CHECK((w.r == 255 && w.g == 255 && w.b == 255 && w.a == 255));
}

TEST_CASE("AMS match: an orange filament goes to the grey tray, not the pink one", "[AmsMatch]")
{
    const std::vector<FilamentInfo>   filaments = {filament(0, "PLA", {255, 128, 0})};
    const std::map<int, FilamentInfo> trays     = {{0, tray(0, "PLA", {255, 105, 180})}, {1, tray(1, "PLA", {128, 128, 128})}};
    CHECK(pick(filaments, trays) == 1);
    // main took the pink one.
    CHECK(main_pick(filaments, trays, AmsMatch::distances(filaments, trays, main_colour_distance))[0] == 0);
}

TEST_CASE("AMS match: picks with an obvious answer are the same as main's", "[AmsMatch]")
{
    // Same colour, a near shade, the only tray of the type, type mismatch, see-through mismatch.
    const std::vector<FilamentInfo> filaments = {
        filament(0, "PLA", {255, 0, 0}),       // red
        filament(1, "PLA", {20, 20, 200}),     // blue-ish
        filament(2, "PETG", {0, 255, 0}),      // only PETG tray is white
        filament(3, "PLA", {250, 250, 250}),   // near white
    };
    const std::map<int, FilamentInfo> trays = {
        {0, tray(0, "PLA", {255, 0, 0})},
        {1, tray(1, "PLA", {0, 0, 255})},
        {2, tray(2, "PETG", {255, 255, 255})},
        {3, tray(3, "PLA", {255, 255, 255})},
    };
    const auto now  = AmsMatch::nearest_colour(filaments, trays, AmsMatch::distances(filaments, trays));
    const auto then = main_pick(filaments, trays, AmsMatch::distances(filaments, trays, main_colour_distance));
    for (size_t i = 0; i < filaments.size(); ++i) {
        INFO("filament " << i);
        CHECK(now[i].tray_id == then[i]);
    }
    CHECK(now[0].tray_id == 0);
    CHECK(now[1].tray_id == 1);
    CHECK(now[2].tray_id == 2);
    CHECK(now[3].tray_id == 3);
}

TEST_CASE("AMS match: two filaments of one colour share the tray of exactly that colour", "[AmsMatch]")
{
    const std::vector<FilamentInfo>   filaments = {filament(0, "PLA", {0, 0, 0}), filament(1, "PLA", {0, 0, 0})};
    const std::map<int, FilamentInfo> trays     = {{0, tray(0, "PLA", {0, 0, 0})}, {1, tray(1, "PLA", {255, 255, 255})}};
    const auto r = AmsMatch::nearest_colour(filaments, trays, AmsMatch::distances(filaments, trays));
    CHECK(r[0].tray_id == 0);
    CHECK(r[1].tray_id == 0);
}

TEST_CASE("AMS match: a tie goes to the tray holding the filament's own preset, whatever the tray numbers", "[AmsMatch]")
{
    // Second AMS only (trays 4 and 7), both black PLA; tray 7 holds the filament's preset.
    const std::vector<FilamentInfo>   filaments = {filament(0, "PLA", {0, 0, 0}, "GFA01")};
    const std::map<int, FilamentInfo> trays     = {{4, tray(4, "PLA", {0, 0, 0}, "GFL99")}, {7, tray(7, "PLA", {0, 0, 0}, "GFA01")}};
    CHECK(pick(filaments, trays) == 7);
    // main looked up trays "0" and "1", which do not exist, and kept tray 4.
    CHECK(main_pick(filaments, trays, AmsMatch::distances(filaments, trays, main_colour_distance))[0] == 4);
}

TEST_CASE("AMS match: on the first AMS with all four trays the tie-break is unchanged", "[AmsMatch]")
{
    const std::vector<FilamentInfo>   filaments = {filament(0, "PLA", {0, 0, 0}, "GFA01")};
    const std::map<int, FilamentInfo> trays     = {{0, tray(0, "PLA", {0, 0, 0}, "GFL99")}, {1, tray(1, "PLA", {0, 0, 0}, "GFA01")},
                                                   {2, tray(2, "PLA", {9, 9, 9})}, {3, tray(3, "PLA", {0, 0, 0})}};
    CHECK(pick(filaments, trays) == 1);
    CHECK(main_pick(filaments, trays, AmsMatch::distances(filaments, trays, main_colour_distance))[0] == 1);
}

TEST_CASE("AMS match: a pick keeps its own distance; the last fallback drops only an opacity mismatch", "[AmsMatch]")
{
    const std::vector<FilamentInfo> filaments = {filament(0, "PLA", {255, 0, 0}), filament(1, "PLA", {0, 0, 255, 128})};
    const std::map<int, FilamentInfo> trays   = {{0, tray(0, "PLA", {200, 0, 0})}, {1, tray(1, "PLA", {0, 0, 255})}};
    auto r = AmsMatch::nearest_colour(filaments, trays, AmsMatch::distances(filaments, trays));
    REQUIRE(r[0].tray_id == 0);
    CHECK(r[0].distance == Approx(AmsMatch::colour_distance({255, 0, 0}, {200, 0, 0})));
    REQUIRE(r[1].tray_id == 1); // the only PLA left, though see-through against opaque
    CHECK(r[1].distance == AmsMatch::FAR_DISTANCE);
    AmsMatch::drop_far_picks(r);
    CHECK(r[0].tray_id == 0);
    CHECK(r[1].tray_id == -1);
}

TEST_CASE("AMS match: the exclude list excludes", "[AmsMatch]")
{
    CHECK(AmsMatch::excluded(5, {1, 5}));
    CHECK_FALSE(AmsMatch::excluded(4, {1, 5}));
    CHECK_FALSE(AmsMatch::excluded(4, {}));
}

// Not run by default: prints the before/after table for the PR ("[.ams_table]" to run it).
TEST_CASE("AMS match: before/after table of one filament and two trays", "[.ams_table]")
{
    int cases = 0, changed = 0, judge_sides_with_fix = 0;
    for (const auto& [fname, fc] : kNamed)
        for (size_t a = 0; a < kNamed.size(); ++a)
            for (size_t b = a + 1; b < kNamed.size(); ++b) {
                if (kNamed[a].first == std::string(fname) || kNamed[b].first == std::string(fname))
                    continue;
                ++cases;
                const std::vector<FilamentInfo>   filaments = {filament(0, "PLA", fc)};
                const std::map<int, FilamentInfo> trays     = {{0, tray(0, "PLA", kNamed[a].second)}, {1, tray(1, "PLA", kNamed[b].second)}};
                const int now  = pick(filaments, trays);
                const int then = main_pick(filaments, trays, AmsMatch::distances(filaments, trays, main_colour_distance))[0];
                if (now == then)
                    continue;
                ++changed;
                const int judge = de2000(fc, kNamed[a].second) < de2000(fc, kNamed[b].second) ? 0 : 1;
                judge_sides_with_fix += judge == now;
                std::cout << "| " << fname << " | " << kNamed[a].first << ", " << kNamed[b].first << " | " << kNamed[then == 0 ? a : b].first
                          << " | " << kNamed[now == 0 ? a : b].first << " | " << kNamed[judge == 0 ? a : b].first << " |\n";
            }
    std::cout << "cases " << cases << ", changed " << changed << ", CIEDE2000 sides with the new pick in " << judge_sides_with_fix << "\n";
}

TEST_CASE("AMS blacklist: Bambu Lab's PET-CF and PA6-CF are prohibited in the AMS", "[AmsMatch][AmsBlacklist]")
{
    const nlohmann::json list = nlohmann::json::parse(R"({"blacklist":[
        {"vendor":"Third Party","type":"TPU","action":"prohibition","description":"TPU: not supported"},
        {"vendor":"Bambulab","type":"TPU","action":"prohibition","description":"TPU: not supported"},
        {"vendor":"Third Party","type":"PVA","action":"warning","description":"PVA: flexible"},
        {"vendor":"Third Party","type":"PLA-CF","action":"warning","description":"CF/GF: hard and brittle"},
        {"vendor":"Bambulab","type":"PET-CF","action":"prohibition","description":"Bambu PET-CF/PA6-CF: not supported"},
        {"vendor":"Bambulab","type":"PA6-CF","action":"prohibition","description":"Bambu PET-CF/PA6-CF: not supported"}]})");

    for (const char* type : {"PET-CF", "PA6-CF"}) {
        INFO(type);
        const AmsMatch::BlacklistHit hit = AmsMatch::check_blacklist(list, "Bambu Lab", type, false);
        CHECK(hit.in_blacklist);
        CHECK(hit.action == "prohibition");
        CHECK(hit.description == "Bambu PET-CF/PA6-CF: not supported");
        std::string action;
        CHECK_FALSE(main_in_blacklist(list, "Bambu Lab", type, action)); // main: "bambu lab" != "bambulab"
    }
    // Somebody else's PET-CF is not Bambu's.
    CHECK_FALSE(AmsMatch::check_blacklist(list, "Polymaker", "PET-CF", false).in_blacklist);

    // Everything main decided for the other entries stays as it was, Bambu Lab's filaments included.
    const std::vector<std::pair<std::string, std::string>> unchanged = {
        {"Bambu Lab", "TPU"}, {"Generic", "TPU"}, {"Bambu Lab", "PVA"}, {"Generic", "PVA"}, {"Bambu Lab", "PLA-CF"},
        {"eSUN", "PLA-CF"},   {"Bambu Lab", "PLA"}, {"", "PVA"},      {"", "PLA"}};
    for (const auto& [vendor, type] : unchanged) {
        INFO(vendor << " " << type);
        std::string                  action;
        const bool                   was = main_in_blacklist(list, vendor, type, action);
        const AmsMatch::BlacklistHit hit = AmsMatch::check_blacklist(list, vendor, type, false);
        CHECK(hit.in_blacklist == was);
        if (was)
            CHECK(hit.action == action);
    }

    // The "skip the AMS blacklist check" preference turns a prohibition into a warning.
    CHECK(AmsMatch::check_blacklist(list, "Bambu Lab", "PET-CF", true).action == "warning");
    CHECK(AmsMatch::vendor_key("Bambu Lab") == AmsMatch::vendor_key("Bambulab"));
}
