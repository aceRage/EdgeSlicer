// The order of the filament list in the AMS slot settings dialog, and the ordered material list of
// the send dialog. Ported from OrcaSlicer #11293 / #11306 (what the AMS dropdown lists first) and
// #14408 (the send dialog's material list iterates in key order). No printer, no window.

#include <catch2/catch.hpp>

#include "slic3r/GUI/AmsFilamentOrder.hpp"
#include "slic3r/GUI/SelectMachine.hpp"

#include <map>
#include <string>
#include <type_traits>
#include <vector>

using namespace Slic3r::GUI;
using AmsFilamentOrder::Entry;

namespace {

std::vector<std::string> sorted_names(std::vector<Entry> entries, const std::map<std::string, int> &selected = {})
{
    AmsFilamentOrder::sort(entries, selected);
    std::vector<std::string> out;
    for (const Entry &e : entries)
        out.push_back(e.name);
    return out;
}

} // namespace

TEST_CASE("AMS filament list: selected filaments are ranked in extruder order", "[AmsFilamentOrder]")
{
    SECTION("the first extruder gets the most negative rank")
    {
        const auto ranks = AmsFilamentOrder::selected_ranks({"Generic PLA", "Bambu PETG HF", "Generic ABS"});
        REQUIRE(ranks.at("Generic PLA") == -3);
        REQUIRE(ranks.at("Bambu PETG HF") == -2);
        REQUIRE(ranks.at("Generic ABS") == -1);
    }
    SECTION("a filament on several extruders keeps the first one's rank")
    {
        const auto ranks = AmsFilamentOrder::selected_ranks({"Generic PLA", "Bambu PETG HF", "Generic PLA"});
        REQUIRE(ranks.size() == 2);
        REQUIRE(ranks.at("Generic PLA") == -3);
        REQUIRE(ranks.at("Bambu PETG HF") == -2);
    }
    SECTION("an extruder whose base preset is unknown is skipped but keeps its position")
    {
        const auto ranks = AmsFilamentOrder::selected_ranks({"", "Generic PLA"});
        REQUIRE(ranks.size() == 1);
        REQUIRE(ranks.at("Generic PLA") == -1);
    }
    SECTION("no filaments, no ranks")
    {
        REQUIRE(AmsFilamentOrder::selected_ranks({}).empty());
    }
}

TEST_CASE("AMS filament list: the filaments in use come first", "[AmsFilamentOrder]")
{
    const std::vector<Entry> entries = {
        {"Bambu PLA Basic", "Bambu Lab", "PLA"},
        {"Generic PETG", "Generic", "PETG"},
        {"Polymaker PolyLite PLA", "Polymaker", "PLA"},
        {"Generic PLA", "Generic", "PLA"},
    };

    SECTION("without anything selected, Bambu's own order leads")
    {
        const auto names = sorted_names(entries);
        REQUIRE(names.front() == "Bambu PLA Basic");
    }
    SECTION("a selected filament outranks Bambu's order, and extruder 1 leads extruder 2")
    {
        const auto ranks = AmsFilamentOrder::selected_ranks({"Polymaker PolyLite PLA", "Generic PETG"});
        const auto names = sorted_names(entries, ranks);
        REQUIRE(names.size() == 4);
        REQUIRE(names[0] == "Polymaker PolyLite PLA");
        REQUIRE(names[1] == "Generic PETG");
        REQUIRE(names[2] == "Bambu PLA Basic");
    }
}

TEST_CASE("AMS filament list: Bambu's favourites keep their listed order", "[AmsFilamentOrder]")
{
    const auto names = sorted_names({{"Bambu ABS", "Bambu Lab", "ABS"},
                                     {"Bambu PETG HF", "Bambu Lab", "PETG"},
                                     {"Bambu PLA Matte", "Bambu Lab", "PLA"},
                                     {"Bambu PLA Basic", "Bambu Lab", "PLA"}});
    REQUIRE(names == std::vector<std::string>{"Bambu PLA Basic", "Bambu PLA Matte", "Bambu PETG HF", "Bambu ABS"});
}

TEST_CASE("AMS filament list: vendor, then type, then name", "[AmsFilamentOrder]")
{
    SECTION("Bambu Lab before Generic before other vendors")
    {
        const auto names = sorted_names({{"Acme PLA", "Acme", "PLA"}, {"Generic PLA", "Generic", "PLA"}, {"Other PLA", "Bambu Lab", "PLA"}});
        REQUIRE(names == std::vector<std::string>{"Other PLA", "Generic PLA", "Acme PLA"});
    }
    SECTION("PLA, PETG, ABS, TPU, then other types, within a vendor")
    {
        const auto names = sorted_names({{"G PA", "Generic", "PA"},
                                         {"G TPU", "Generic", "TPU"},
                                         {"G ABS", "Generic", "ABS"},
                                         {"G PETG", "Generic", "PETG"},
                                         {"G PLA", "Generic", "PLA"}});
        REQUIRE(names == std::vector<std::string>{"G PLA", "G PETG", "G ABS", "G TPU", "G PA"});
    }
    SECTION("alphabetical when all else is equal")
    {
        const auto names = sorted_names({{"Beta", "Acme", "PLA"}, {"Alpha", "Acme", "PLA"}});
        REQUIRE(names == std::vector<std::string>{"Alpha", "Beta"});
    }
}

TEST_CASE("Send dialog material list iterates in key order", "[AmsFilamentOrder][MaterialHash]")
{
    STATIC_REQUIRE(std::is_same<MaterialHash, std::map<int, Material *>>::value);

    MaterialHash list;
    for (int i : {5, 1, 9, 0, 3, 2, 8, 4, 7, 6})
        list[i] = nullptr;

    int expected = 0;
    for (const auto &kv : list) {
        REQUIRE(kv.first == expected);
        ++expected;
    }
    REQUIRE(expected == 10);
}
