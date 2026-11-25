// The generated list of option lines Bambu Studio shows on its tabs (Format/BambuUiKeys.cpp, from
// scripts/gen_bambu_ui_keys.py), which the "Bambu" settings view filters by. See
// tests/design_view_modes.md.
#include <catch2/catch.hpp>

#include "libslic3r/Format/BambuKeyAliases.hpp"
#include "libslic3r/Format/BambuUiKeys.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cstring>
#include <set>
#include <string>
#include <utility>

using namespace Slic3r;

namespace {

bool ours(const std::string &key) { return print_config_def.get(key) != nullptr; }

const BambuUiKeys::InferredEquivalent *inferred(const std::string &bambu_key)
{
    for (const auto &e : BambuUiKeys::inferred_equivalents())
        if (bambu_key == e.bambu)
            return &e;
    return nullptr;
}

bool reviewed_missing(const std::string &bambu_key)
{
    for (const char *k : BambuUiKeys::reviewed_missing())
        if (bambu_key == k)
            return true;
    return false;
}

} // namespace

TEST_CASE("Bambu UI key list loads and names its source", "[BambuUiKeys]")
{
    REQUIRE(std::strlen(BambuUiKeys::bambu_version()) > 0);
    REQUIRE(std::strlen(BambuUiKeys::bambu_commit()) > 0);

    const auto &list = BambuUiKeys::entries();
    size_t shown = 0, process = 0, filament = 0, printer = 0;
    for (const auto &e : list) {
        if (e.level == BambuUiKeys::Level::Develop)
            continue;
        ++shown;
        process += e.tab == BambuUiKeys::Tab::Process;
        filament += e.tab == BambuUiKeys::Tab::Filament;
        printer += e.tab == BambuUiKeys::Tab::Printer;
    }
    // Bambu Studio 02.09.01.70 with Advanced on: 254 process, 106 filament, 62 printer lines.
    CHECK(shown > 400);
    CHECK(process > 200);
    CHECK(filament > 80);
    CHECK(printer > 40);
}

TEST_CASE("Bambu UI key list has no duplicate key on a tab", "[BambuUiKeys]")
{
    std::set<std::pair<int, std::string>> seen;
    for (const auto &e : BambuUiKeys::entries()) {
        INFO(e.key);
        CHECK(seen.emplace(int(e.tab), std::string(e.key)).second);
    }
}

TEST_CASE("Every Bambu view key is ours, renamed, an inferred equivalent or reviewed as missing", "[BambuUiKeys]")
{
    // A failure here after a refresh means Bambu Studio shows a setting we have not reviewed:
    // port it, add an alias, or add it to REVIEWED_MISSING in scripts/gen_bambu_ui_keys.py.
    for (const auto &e : BambuUiKeys::entries()) {
        if (e.level == BambuUiKeys::Level::Develop)
            continue;
        const std::string key = e.key;
        INFO(key);
        const auto *alias = BambuKeyAliases::by_bambu(key);
        const auto *eq    = inferred(key);
        const bool  known = ours(key) || (alias && ours(alias->ours)) || (eq && ours(eq->ours)) || reviewed_missing(key);
        CHECK(known);
    }
}

TEST_CASE("Reviewed missing keys are really missing and really listed", "[BambuUiKeys]")
{
    std::set<std::string> listed;
    for (const auto &e : BambuUiKeys::entries())
        listed.insert(e.key);
    for (const char *k : BambuUiKeys::reviewed_missing()) {
        INFO(k);
        CHECK(listed.count(k) == 1);
        // Once a key is ported (or aliased), it must leave the missing list.
        CHECK_FALSE(ours(k));
        CHECK(BambuKeyAliases::by_bambu(k) == nullptr);
    }
}

TEST_CASE("Bambu UI renames map to our keys", "[BambuUiKeys]")
{
    std::set<std::string> listed;
    for (const auto &e : BambuUiKeys::entries())
        listed.insert(e.key);

    const std::pair<const char *, const char *> renames[] = {
        { "top_one_wall_type", "only_one_wall_top" },
        { "prime_tower_rib_wall", "wipe_tower_wall_type" },
        { "sparse_infill_anchor", "infill_anchor" },
        { "chamber_temperatures", "chamber_temperature" },
        { "process_notes", "notes" },
        { "role_base_wipe_speed", "role_based_wipe_speed" },
    };
    for (const auto &r : renames) {
        INFO(r.first);
        CHECK(listed.count(r.first) == 1);
        const auto *alias = BambuKeyAliases::by_bambu(r.first);
        REQUIRE(alias != nullptr);
        CHECK(std::string(alias->ours) == r.second);
        CHECK(ours(alias->ours));
    }

    for (const auto &e : BambuUiKeys::inferred_equivalents()) {
        INFO(e.bambu);
        CHECK(listed.count(e.bambu) == 1);
        CHECK_FALSE(ours(e.bambu));
        CHECK(ours(e.ours));
        CHECK(BambuKeyAliases::by_bambu(e.bambu) == nullptr);
    }
}

TEST_CASE("Lines Bambu hides on Bambu Lab printers are ours and listed", "[BambuUiKeys]")
{
    std::set<std::string> listed;
    for (const auto &e : BambuUiKeys::entries())
        listed.insert(e.key);
    const auto &hidden = BambuUiKeys::bbl_printer_hidden();
    CHECK(hidden.size() == 7); // the seven *_jerk lines at 02.09.01.70
    for (const char *k : hidden) {
        INFO(k);
        CHECK(listed.count(k) == 1);
        CHECK(ours(k));
    }
}
