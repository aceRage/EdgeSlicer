// Favourite filaments: the keys, the stored set and the grouped order every filament picker shows.
// Orca #13946 (AMS dialog only) extended to every picker. No window, no app config.

#include <catch2/catch.hpp>

#include "slic3r/GUI/FavoriteFilaments.hpp"
#include "libslic3r/Config.hpp"

#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r::GUI;
using FavoriteFilaments::Row;

namespace {

// Rows as short strings: "F" / "A" for the headers, "name" for an item, "*name" for the favourites copy.
std::vector<std::string> describe(const std::vector<Row> &rows, const std::vector<std::string> &names)
{
    std::vector<std::string> out;
    for (const Row &r : rows) {
        if (r.kind == Row::FavoritesHeader)
            out.push_back("F");
        else if (r.kind == Row::AllHeader)
            out.push_back("A");
        else
            out.push_back((r.in_favorites ? "*" : "") + names[r.index]);
    }
    return out;
}

} // namespace

TEST_CASE("Favourite keys: system presets by alias, user presets by full name", "[FavoriteFilaments]")
{
    CHECK(FavoriteFilaments::strip_printer_suffix("Bambu PLA Basic @BBL X1C") == "Bambu PLA Basic");
    CHECK(FavoriteFilaments::strip_printer_suffix("Snapmaker PLA @U1 0.4 nozzle") == "Snapmaker PLA");
    CHECK(FavoriteFilaments::strip_printer_suffix("Generic PETG") == "Generic PETG");
    CHECK(FavoriteFilaments::strip_printer_suffix("Odd name  @x") == "Odd name");
    // only " @" starts a suffix, an @ inside a word does not
    CHECK(FavoriteFilaments::strip_printer_suffix("PLA@home") == "PLA@home");

    // A system preset: its alias, the same on every printer.
    CHECK(FavoriteFilaments::key_for("Bambu PLA Basic @BBL X1C", "Bambu PLA Basic", true) == "Bambu PLA Basic");
    CHECK(FavoriteFilaments::key_for("Bambu PLA Basic @BBL H2D", "Bambu PLA Basic", true) == "Bambu PLA Basic");
    // A system preset without an alias falls back to the name without the suffix.
    CHECK(FavoriteFilaments::key_for("Generic PLA @System", "", true) == "Generic PLA");
    // A user copy keeps its full name: starring the system filament does not star the copy.
    CHECK(FavoriteFilaments::key_for("Bambu PLA Basic @BBL X1C(1)", "", false) == "Bambu PLA Basic @BBL X1C(1)");
    CHECK(FavoriteFilaments::key_for("My PLA", "", false) == "My PLA");
}

TEST_CASE("Favourite toggle adds and removes", "[FavoriteFilaments]")
{
    std::set<std::string> keys;
    CHECK(FavoriteFilaments::toggle(keys, "Generic PLA"));
    CHECK(keys.count("Generic PLA") == 1);
    CHECK_FALSE(FavoriteFilaments::toggle(keys, "Generic PLA"));
    CHECK(keys.empty());
    CHECK_FALSE(FavoriteFilaments::toggle(keys, ""));
    CHECK(keys.empty());
}

TEST_CASE("Favourite set survives the app config's escaped list", "[FavoriteFilaments]")
{
    // The set is stored with the same C-style escaping as every other string list.
    const std::vector<std::string> in{"Bambu PLA Basic", "My \"silk\" PLA @BBL X1C", "PLA; with, punctuation",
                                      "\xC3\x9C" "n" "\xC3\xAF" "code PETG" /* UTF-8 */};
    const std::string              stored = Slic3r::escape_strings_cstyle(in);
    std::vector<std::string>       out;
    REQUIRE(Slic3r::unescape_strings_cstyle(stored, out));
    CHECK(out == in);
}

TEST_CASE("Favourites follow a vendor rename (renamed_from)", "[FavoriteFilaments]")
{
    // Panchroma "Stain" became "Satin" (#494): the new presets list the old names in renamed_from.
    const auto renames = FavoriteFilaments::rename_map({
        {"Panchroma PLA Satin", "Panchroma PLA Stain @BBL A1"},
        {"Panchroma PLA Satin", "Panchroma PLA Stain @BBL X1C"},
        {"Bambu PLA Basic", "Bambu PLA Basic @BBL X1"}, // only the printer part changed: no rename
    });
    REQUIRE(renames.size() == 1);
    CHECK(renames.at("Panchroma PLA Stain") == "Panchroma PLA Satin");

    SECTION("an orphaned key moves to the new name") {
        std::set<std::string> keys{"Panchroma PLA Stain", "Generic PLA"};
        CHECK(FavoriteFilaments::migrate(keys, {"Panchroma PLA Satin", "Generic PLA"}, renames));
        CHECK(keys == std::set<std::string>{"Panchroma PLA Satin", "Generic PLA"});
    }
    SECTION("a key still in use stays (another printer may still list it)") {
        std::set<std::string> keys{"Panchroma PLA Stain"};
        CHECK_FALSE(FavoriteFilaments::migrate(keys, {"Panchroma PLA Stain", "Panchroma PLA Satin"}, renames));
        CHECK(keys == std::set<std::string>{"Panchroma PLA Stain"});
    }
    SECTION("an unknown key stays (a preset of another printer, or a removed user preset)") {
        std::set<std::string> keys{"Nobody PLA"};
        CHECK_FALSE(FavoriteFilaments::migrate(keys, {"Generic PLA"}, renames));
        CHECK(keys == std::set<std::string>{"Nobody PLA"});
    }
    SECTION("a chain of renames is followed, a cycle is not followed forever") {
        const auto chain = FavoriteFilaments::rename_map({{"B", "A @x"}, {"C", "B @x"}});
        std::set<std::string> keys{"A"};
        CHECK(FavoriteFilaments::migrate(keys, {"C"}, chain));
        CHECK(keys == std::set<std::string>{"C"});

        const auto cycle = FavoriteFilaments::rename_map({{"B", "A @x"}, {"A", "B @x"}});
        std::set<std::string> loop{"A"};
        CHECK_FALSE(FavoriteFilaments::migrate(loop, {"Z"}, cycle));
        CHECK(loop == std::set<std::string>{"A"});
    }
}

TEST_CASE("Favourite group: favourites on top, every filament below", "[FavoriteFilaments]")
{
    const std::vector<std::string> names{"Generic PLA", "Bambu PLA Basic", "Generic PETG", "My PLA"};

    SECTION("no favourites: the list is unchanged and has no headers") {
        CHECK(describe(FavoriteFilaments::group(names, {}), names) ==
              std::vector<std::string>{"Generic PLA", "Bambu PLA Basic", "Generic PETG", "My PLA"});
    }
    SECTION("favourites keep the list's own order (the AMS sort, the sidebar's name order)") {
        const std::vector<Row> rows = FavoriteFilaments::group(names, {"My PLA", "Generic PLA"});
        CHECK(describe(rows, names) ==
              std::vector<std::string>{"F", "*Generic PLA", "*My PLA", "A", "Generic PLA", "Bambu PLA Basic", "Generic PETG", "My PLA"});
        // every item row knows whether its filament is a favourite, wherever it is listed
        CHECK(rows[1].favorite);
        CHECK(rows[4].favorite);
        CHECK_FALSE(rows[5].favorite);
    }
    SECTION("favourites not in this list (another printer's filaments) add nothing") {
        CHECK(describe(FavoriteFilaments::group(names, {"Elegoo PLA"}), names) ==
              std::vector<std::string>{"Generic PLA", "Bambu PLA Basic", "Generic PETG", "My PLA"});
    }
    SECTION("without the 'all' header") {
        CHECK(describe(FavoriteFilaments::group(names, {"Generic PETG"}, false), names) ==
              std::vector<std::string>{"F", "*Generic PETG", "Generic PLA", "Bambu PLA Basic", "Generic PETG", "My PLA"});
    }
}

TEST_CASE("Flat favourite list for native combos (macOS)", "[FavoriteFilaments]")
{
    const std::vector<std::string> names{"Generic PLA", "Bambu PLA Basic", "Generic PETG"};
    const std::vector<Row>         rows = FavoriteFilaments::flat(names, {"Generic PETG"});
    CHECK(describe(rows, names) == std::vector<std::string>{"*Generic PETG", "Generic PLA", "Bambu PLA Basic", "Generic PETG"});
    for (const Row &r : rows)
        CHECK(r.kind == Row::Item);

    const std::string shown = FavoriteFilaments::star_prefix() + "Generic PETG";
    CHECK(FavoriteFilaments::strip_star(shown) == "Generic PETG");
    CHECK(FavoriteFilaments::strip_star("Generic PETG") == "Generic PETG");
    CHECK(FavoriteFilaments::star_prefix() == "\xE2\x98\x85 ");
}
