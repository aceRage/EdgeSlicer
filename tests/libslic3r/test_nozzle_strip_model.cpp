// The nozzle strip, the Extruder Settings dialog and the Process tab's chips (NozzleStripModel.hpp,
// tests/design_bambu29_mixed_nozzle_compare.md section 4): tiles from a printer config (labels, racks, empty positions,
// dividers), filaments on tiles (used state), chip grouping and label ranges, and edits in the project keys P7 / P9 write.

#include <catch2/catch.hpp>

#include "libslic3r/NozzleStripModel.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

DynamicPrintConfig printer(const std::vector<double> &nozzles, const std::vector<int> &capacity, const std::vector<std::string> &racks = {})
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.option<ConfigOptionFloats>("nozzle_diameter", true)->values             = nozzles;
    cfg.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = false;
    cfg.set_key_value("extruder_max_nozzle_count", new ConfigOptionIntsNullable(capacity));
    if (!racks.empty())
        cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_override", true)->values = racks;
    return cfg;
}

std::vector<std::string> labels_of(const std::vector<NozzleStripTile> &tiles)
{
    std::vector<std::string> out;
    for (const NozzleStripTile &t : tiles)
        out.push_back(t.label);
    return out;
}

RackNozzle nozzle(double d, NozzleVolumeType t = nvtStandard)
{
    RackNozzle n;
    n.diameter    = d;
    n.volume_type = t;
    return n;
}

} // namespace

TEST_CASE("Strip tiles: one per physical nozzle, named like the printer", "[NozzleStrip][Tiles]")
{
    SECTION("U1: T1..T4, no dividers")
    {
        const NozzleStripInput in    = nozzle_strip_input(printer({ 0.4, 0.4, 0.6, 0.4 }, { 1, 1, 1, 1 }), false);
        const auto             tiles = nozzle_strip_tiles(in);
        CHECK(labels_of(tiles) == std::vector<std::string>{ "T1", "T2", "T3", "T4" });
        CHECK(tiles[2].diameter == Approx(0.6));
        CHECK(std::none_of(tiles.begin(), tiles.end(), [](const NozzleStripTile &t) { return t.divider_before; }));
        CHECK(nozzle_strip_applies(in));
    }
    SECTION("H2D: L R")
    {
        const NozzleStripInput in = nozzle_strip_input(printer({ 0.4, 0.6 }, { 1, 1 }), true);
        CHECK(labels_of(nozzle_strip_tiles(in)) == std::vector<std::string>{ "L", "R" });
    }
    SECTION("H2C: L then R1..R6, empty positions are not usable, a divider before the rack")
    {
        const NozzleStripInput in    = nozzle_strip_input(printer({ 0.4, 0.4 }, { 1, 6 }, { "", "0.4,0.4,0.6:high_flow" }), true);
        const auto             tiles = nozzle_strip_tiles(in);
        CHECK(labels_of(tiles) == std::vector<std::string>{ "L", "R1", "R2", "R3", "R4", "R5", "R6" });
        CHECK(tiles[1].divider_before);
        CHECK_FALSE(tiles[2].divider_before);
        CHECK(tiles[3].diameter == Approx(0.6));
        CHECK(tiles[3].volume_type == nvtHighFlow);
        CHECK(tiles[4].empty());
        CHECK_FALSE(tiles[4].usable);
        CHECK(tiles[4].reason == "Empty slot");
        CHECK(tiles[1].usable);
    }
    SECTION("H2C without a rack in the project: R1 is the extruder's own nozzle")
    {
        const auto tiles = nozzle_strip_tiles(nozzle_strip_input(printer({ 0.4, 0.6 }, { 1, 6 }), true));
        REQUIRE(tiles.size() == 7);
        CHECK(tiles[1].label == "R1");
        CHECK(tiles[1].diameter == Approx(0.6));
        CHECK(tiles[2].empty());
    }
    SECTION("H3C-like single extruder with nine positions: 1..9")
    {
        const auto tiles = nozzle_strip_tiles(nozzle_strip_input(printer({ 0.4 }, { 9 }, { "0.4,0.4,0.2" }), false));
        CHECK(labels_of(tiles) == std::vector<std::string>{ "1", "2", "3", "4", "5", "6", "7", "8", "9" });
        CHECK(tiles[2].diameter == Approx(0.2));
    }
    SECTION("A single-nozzle printer keeps its old row")
    {
        CHECK_FALSE(nozzle_strip_applies(nozzle_strip_input(printer({ 0.4 }, { 1 }), false)));
    }
    SECTION("A rack left over from another printer is ignored")
    {
        const auto tiles = nozzle_strip_tiles(nozzle_strip_input(printer({ 0.4, 0.4 }, { 1, 1 }, { "", "0.4,0.6" }), false));
        CHECK(labels_of(tiles) == std::vector<std::string>{ "T1", "T2" });
    }
    SECTION("A toolchanger with a rack names its positions T2.1..")
    {
        const auto tiles = nozzle_strip_tiles(nozzle_strip_input(printer({ 0.4, 0.4 }, { 1, 3 }, { "", "0.4,0.6" }), false));
        CHECK(labels_of(tiles) == std::vector<std::string>{ "T1", "T2.1", "T2.2", "T2.3" });
    }
}

TEST_CASE("Strip flow types follow the project's nozzle_volume_type", "[NozzleStrip][Flow]")
{
    NozzleStripInput in = nozzle_strip_input(printer({ 0.4, 0.4 }, { 1, 6 }, { "", "0.4,0.6:standard" }), true);
    nozzle_strip_set_volume_types(in, { nvtStandard, nvtHighFlow });
    const auto tiles = nozzle_strip_tiles(in);
    CHECK(tiles[1].volume_type == nvtHighFlow); // follows the extruder
    CHECK(tiles[2].volume_type == nvtStandard); // its own
    CHECK(nozzle_flow_short(nvtHighFlow) == "HF");
    CHECK(nozzle_flow_short(nvtStandard) == "Std");
    CHECK(nozzle_flow_short(nvtTPUHighFlow) == "TPU");
}

TEST_CASE("Filaments are placed on the tile they print on", "[NozzleStrip][Used]")
{
    const NozzleStripInput in = nozzle_strip_input(printer({ 0.4, 0.4 }, { 1, 6 }, { "", "0.4,0.4,0.6,0.4" }), true);
    auto                   tiles = nozzle_strip_tiles(in);
    std::vector<NozzleStripPlace> places(5);
    places[0].extruder = 0;               // left
    places[1].extruder = 1;               // right, unpinned: first 0.4
    places[2].extruder = 1;
    places[2].size_pin = 0.6;             // right 0.6: R3
    places[3].extruder = 1;
    places[3].slot_pin = 4;               // R4
    places[4].extruder = -1;              // Auto before slicing: nowhere
    place_nozzle_strip_filaments(tiles, in, places, { 0, 1, 2, 3, 4 });
    CHECK(tiles[0].filaments == std::vector<unsigned int>{ 0 });
    CHECK(tiles[1].filaments == std::vector<unsigned int>{ 1 });
    CHECK(tiles[3].filaments == std::vector<unsigned int>{ 2 });
    CHECK(tiles[4].filaments == std::vector<unsigned int>{ 3 });
    CHECK(tiles[2].filaments.empty());
    CHECK_FALSE(tiles[2].used());
    CHECK(tiles[1].used());

    SECTION("Only the plate's filaments count")
    {
        place_nozzle_strip_filaments(tiles, in, places, { 2 });
        CHECK_FALSE(tiles[0].used());
        CHECK(tiles[3].used());
    }
    SECTION("A pin to a position that is empty falls back to the size")
    {
        places[3].slot_pin = 6;
        place_nozzle_strip_filaments(tiles, in, places, { 3 });
        CHECK(tiles[1].filaments == std::vector<unsigned int>{ 3 });
    }
}

TEST_CASE("Chip labels collapse tile ranges", "[NozzleStrip][Chips]")
{
    CHECK(collapse_nozzle_strip_labels({ "R1", "R2", "R3", "R4", "R5" }) == "R1-R5");
    CHECK(collapse_nozzle_strip_labels({ "T3", "T4" }) == "T3-T4");
    CHECK(collapse_nozzle_strip_labels({ "R1", "R3" }) == "R1, R3");
    CHECK(collapse_nozzle_strip_labels({ "R1", "R2", "R4" }) == "R1-R2, R4");
    CHECK(collapse_nozzle_strip_labels({ "1", "2", "3" }) == "1-3");
    CHECK(collapse_nozzle_strip_labels({ "L" }) == "L");
    CHECK(collapse_nozzle_strip_labels({ "L", "R" }) == "L, R");
    CHECK(collapse_nozzle_strip_labels({}).empty());

    const NozzleStripInput in    = nozzle_strip_input(printer({ 0.4, 0.4 }, { 1, 6 }, { "", "0.4,0.4,0.4,0.4,0.4,0.6" }), true);
    const auto             tiles = nozzle_strip_tiles(in);
    const NozzleChip       left  = nozzle_chip_for(tiles, 0, 0.4);
    CHECK(left.size == "0.4");
    CHECK(left.flow == "Std");
    CHECK(left.tiles == "L");
    CHECK(nozzle_chip_for(tiles, 1, 0.4).tiles == "R1-R5");
    CHECK(nozzle_chip_for(tiles, 1, 0.6).tiles == "R6");

    SECTION("A size with two flow types names both")
    {
        const auto mixed = nozzle_strip_tiles(nozzle_strip_input(printer({ 0.4, 0.4 }, { 1, 6 }, { "", "0.4,0.4:high_flow" }), true));
        CHECK(nozzle_chip_for(mixed, 1, 0.4).flow == "Std/HF");
    }
}

TEST_CASE("Strip edits write the project keys", "[NozzleStrip][Edit]")
{
    SECTION("A tool without a rack: its size and flow type")
    {
        const NozzleStripInput in   = nozzle_strip_input(printer({ 0.4, 0.4, 0.4, 0.4 }, { 1, 1, 1, 1 }), false);
        const auto             tiles = nozzle_strip_tiles(in);
        NozzleStripEdit        edit = nozzle_strip_edit_start(in);
        CHECK(nozzle_strip_edit_tile(edit, in, tiles[2], 0.6, nvtHighFlow).empty());
        CHECK(edit.diameters == std::vector<double>{ 0.4, 0.4, 0.6, 0.4 });
        CHECK(edit.volume_types[2] == nvtHighFlow);
        CHECK(std::none_of(edit.rack_written.begin(), edit.rack_written.end(), [](bool b) { return b; }));
        CHECK_FALSE(nozzle_strip_edit_tile(edit, in, tiles[0], 0., nvtStandard).empty()); // no nozzle at all: refused
    }
    SECTION("A rack position: the rack text, the extruder's size kept while the rack holds it")
    {
        const NozzleStripInput in   = nozzle_strip_input(printer({ 0.4, 0.4 }, { 1, 6 }, { "", "0.4,0.4,0.4" }), true);
        const auto             tiles = nozzle_strip_tiles(in);
        NozzleStripEdit        edit = nozzle_strip_edit_start(in);
        CHECK(nozzle_strip_edit_tile(edit, in, tiles[3], 0.6, nvtStandard).empty()); // R3
        CHECK(edit.rack_written[1]);
        CHECK(edit.rack_texts[1] == "0.4,0.4,0.6");
        CHECK(edit.diameters[1] == Approx(0.4));
        // An empty position is filled at the end of the rack.
        CHECK(nozzle_strip_edit_tile(edit, in, tiles[6], 0.2, nvtHighFlow).empty()); // R6 -> 4th nozzle
        CHECK(edit.rack_texts[1] == "0.4,0.4,0.6,0.2:high_flow");
        // Removing a nozzle closes the gap.
        CHECK(nozzle_strip_edit_tile(edit, in, tiles[1], 0., nvtStandard).empty()); // R1
        CHECK(edit.rack_texts[1] == "0.4,0.6,0.2:high_flow");
    }
    SECTION("The extruder's size follows when the rack no longer holds it")
    {
        const NozzleStripInput in   = nozzle_strip_input(printer({ 0.4, 0.4 }, { 1, 6 }, { "", "0.4,0.6" }), true);
        const auto             tiles = nozzle_strip_tiles(in);
        NozzleStripEdit        edit = nozzle_strip_edit_start(in);
        CHECK(nozzle_strip_edit_tile(edit, in, tiles[1], 0.8, nvtHighFlow).empty());
        CHECK(edit.diameters[1] == Approx(0.8)); // the first nozzle's
        CHECK(edit.volume_types[1] == nvtHighFlow);
        CHECK(edit.rack_texts[1] == "0.8,0.6:standard");
        CHECK(validate_rack_nozzle_text(edit.rack_texts[1], edit.diameters[1]).empty());
    }
    SECTION("A rack not set up: R1 is the extruder, R2 starts the rack")
    {
        const NozzleStripInput in   = nozzle_strip_input(printer({ 0.4, 0.4 }, { 1, 6 }), true);
        const auto             tiles = nozzle_strip_tiles(in);
        NozzleStripEdit        edit = nozzle_strip_edit_start(in);
        CHECK(nozzle_strip_edit_tile(edit, in, tiles[2], 0.6, nvtStandard).empty());
        CHECK(edit.rack_texts[1] == "0.4,0.6");
        CHECK(edit.diameters[1] == Approx(0.4));
    }
    SECTION("More nozzles than the rack holds are refused")
    {
        const NozzleStripInput in   = nozzle_strip_input(printer({ 0.4, 0.4 }, { 1, 2 }, { "", "0.4,0.4" }), true);
        NozzleStripEdit        edit = nozzle_strip_edit_start(in);
        CHECK_FALSE(nozzle_strip_edit_set_rack(edit, in, 1, { nozzle(0.4), nozzle(0.4), nozzle(0.6) }).empty());
        CHECK_FALSE(edit.rack_written[1]);
    }
}

TEST_CASE("Pins to sizes or positions that go away are found before applying", "[NozzleStrip][Pins]")
{
    const NozzleStripInput in   = nozzle_strip_input(printer({ 0.4, 0.4 }, { 1, 6 }, { "", "0.4,0.4,0.6" }), true);
    NozzleStripEdit        edit = nozzle_strip_edit_start(in);
    REQUIRE(nozzle_strip_edit_set_rack(edit, in, 1, { nozzle(0.4), nozzle(0.4) }).empty());
    // Filament 1 pinned to 0.6 on the right, 2 to position 3 on the right, 3 to 0.4 on the right, 4 on the left with a stale pin.
    const auto orphaned = nozzle_strip_orphaned_pins(edit, in, { 1, 1, 1, 0 }, { 0.6, 0., 0.4, 0.6 }, { 0, 3, 0, 0 });
    CHECK(orphaned == std::vector<unsigned int>{ 0, 1 });
}
