#include <catch2/catch.hpp>

#include "libslic3r/NozzleSync.hpp"

using namespace Slic3r;
using nlohmann::json;
namespace NS = Slic3r::NozzleSync;

// "Synchronize nozzle information" on a Snapmaker U1: the printer reports the nozzle diameter of every head
// and the slicer applies it. Fake reports only; nothing here talks to a printer.

TEST_CASE("A reported array of numbers or strings keeps head order", "[NozzleSync][U1]")
{
    CHECK(NS::parse_reported_nozzles(json::parse("[0.4, 0.4, 0.4, 0.4]")) == std::vector<std::string>{ "0.4", "0.4", "0.4", "0.4" });
    CHECK(NS::parse_reported_nozzles(json::parse(R"(["0.4", "0.4", "0.6", "0.6"])")) == std::vector<std::string>{ "0.4", "0.4", "0.6", "0.6" });
    // float32 values as a printer firmware sends them
    CHECK(NS::parse_reported_nozzles(json::parse("[0.4000000059604645, 0.6000000238418579]")) == std::vector<std::string>{ "0.4", "0.6" });
}

TEST_CASE("A single reported nozzle size maps to itself, not to 0.2", "[NozzleSync][U1]")
{
    // The scalar branch of the old parser answered "0.2" for 0.4, 0.6 and 0.8.
    CHECK(NS::parse_reported_nozzles(json(0.4)) == std::vector<std::string>{ "0.4" });
    CHECK(NS::parse_reported_nozzles(json(0.6)) == std::vector<std::string>{ "0.6" });
    CHECK(NS::parse_reported_nozzles(json(0.8)) == std::vector<std::string>{ "0.8" });
    CHECK(NS::parse_reported_nozzles(json(0.2)) == std::vector<std::string>{ "0.2" });
    CHECK(NS::parse_reported_nozzles(json("0.4")) == std::vector<std::string>{ "0.4" });
    CHECK(NS::parse_reported_nozzles(json(0.35)).empty());
}

TEST_CASE("Reported diameters are read leniently and positions survive", "[NozzleSync][U1]")
{
    CHECK(NS::parse_reported_diameter("0.4") == Approx(0.4));
    CHECK(NS::parse_reported_diameter(" 0.6 mm ") == Approx(0.6));
    CHECK(NS::parse_reported_diameter("0.4mm") == Approx(0.4));
    CHECK(NS::parse_reported_diameter("") == 0.);
    CHECK(NS::parse_reported_diameter("none") == 0.);
    CHECK(NS::parse_reported_diameter("0.4x") == 0.);
    CHECK(NS::parse_reported_diameter("-1") == 0.);
    CHECK(NS::parse_reported_diameter("7") == 0.);
    CHECK(NS::variant_name(0.4) == "0.4");
    CHECK(NS::variant_name(0.25) == "0.25");
    CHECK(NS::variant_name(1.0) == "1");
    const NS::Plan p = NS::plan({ "0.4", "", "0.4", "0.4" });
    REQUIRE(p.per_head.size() == 4);
    CHECK(p.per_head[1] == 0.); // unreadable head 2 does not shift heads 3 and 4
    CHECK(p.per_head[2] == Approx(0.4));
}

TEST_CASE("Four 0.4 heads reported to a 0.2 preset select the 0.4 machine", "[NozzleSync][U1]")
{
    const NS::Plan p = NS::plan({ "0.4", "0.4", "0.4", "0.4" });
    CHECK(p.uniform);
    CHECK(p.variant == "0.4");
    // Head 1 included: the machine for 0.4 is selected and every head carries it.
    CHECK(p.per_head == std::vector<double>{ 0.4, 0.4, 0.4, 0.4 });
    // Also when the strings carry a unit.
    CHECK(NS::plan({ "0.4mm", "0.4 mm", "0.4", "0.40" }).uniform);
    CHECK(NS::plan({ "0.4mm", "0.4 mm", "0.4", "0.40" }).variant == "0.4");
    // A head that reports nothing readable does not make the rest mixed.
    CHECK(NS::plan({ "", "0.4", "0.4", "0.4" }).uniform);
    CHECK_FALSE(NS::plan({}).uniform);
    CHECK_FALSE(NS::plan({ "", "" }).uniform);
}

TEST_CASE("A mixed report overwrites every head including head 1", "[NozzleSync][U1]")
{
    const NS::Plan mixed = NS::plan({ "0.4", "0.4", "0.6", "0.6" });
    CHECK_FALSE(mixed.uniform);
    // Base preset picked in the dialog is 0.2: heads 1-4 all start at 0.2.
    bool changed = false;
    const std::vector<double> out = NS::apply_per_head({ 0.2, 0.2, 0.2, 0.2 }, mixed.per_head, &changed);
    CHECK(changed);
    CHECK(out == std::vector<double>{ 0.4, 0.4, 0.6, 0.6 });

    // Head 1 differs from the others: it is still written.
    CHECK(NS::apply_per_head({ 0.2, 0.4, 0.4, 0.4 }, NS::plan({ "0.6", "0.4", "0.4", "0.4" }).per_head) ==
          std::vector<double>{ 0.6, 0.4, 0.4, 0.4 });
    // Uniform 0.4 applied onto a preset that has 0.2 on head 1 only gives 0.4 everywhere.
    CHECK(NS::apply_per_head({ 0.2, 0.4, 0.4, 0.4 }, NS::plan({ "0.4", "0.4", "0.4", "0.4" }).per_head) ==
          std::vector<double>{ 0.4, 0.4, 0.4, 0.4 });
    // Nothing to change.
    NS::apply_per_head({ 0.4, 0.4 }, { 0.4, 0.4 }, &changed);
    CHECK_FALSE(changed);
    // Fewer reported heads than the machine has: the rest is left alone; extra reports are ignored.
    CHECK(NS::apply_per_head({ 0.2, 0.2, 0.2, 0.2 }, { 0.4, 0.4 }) == std::vector<double>{ 0.4, 0.4, 0.2, 0.2 });
    CHECK(NS::apply_per_head({ 0.2, 0.2 }, { 0.4, 0.4, 0.4, 0.4 }) == std::vector<double>{ 0.4, 0.4 });
    // An unreadable head keeps the preset's value and does not shift the rest.
    CHECK(NS::apply_per_head({ 0.2, 0.2, 0.2, 0.2 }, NS::plan({ "0.4", "?", "0.6", "0.6" }).per_head) ==
          std::vector<double>{ 0.4, 0.2, 0.6, 0.6 });
}

TEST_CASE("The reported heads select the matching U1 machine variant", "[NozzleSync][U1]")
{
    const std::vector<NS::MachineVariant> machines = {
        { "0.2", { 0.2, 0.2, 0.2, 0.2 } }, { "0.4", { 0.4, 0.4, 0.4, 0.4 } }, { "0.4+0.6", { 0.4, 0.4, 0.6, 0.6 } },
        { "0.6", { 0.6, 0.6, 0.6, 0.6 } }, { "0.8", { 0.8, 0.8, 0.8, 0.8 } } };
    auto match = [&](std::initializer_list<const char *> reported) {
        std::vector<std::string> r(reported.begin(), reported.end());
        return NS::match_machine_variant(NS::plan(r).per_head, machines);
    };
    CHECK(match({ "0.4", "0.4", "0.4", "0.4" }) == "0.4");
    CHECK(match({ "0.4", "0.4", "0.6", "0.6" }) == "0.4+0.6");
    CHECK(match({ "0.8", "0.8", "0.8", "0.8" }) == "0.8");
    // No machine for these: the caller falls back to a base machine and writes every head, head 1 included.
    CHECK(match({ "0.6", "0.4", "0.4", "0.4" }).empty());
    CHECK(match({ "0.4", "0.6", "0.4", "0.6" }).empty());
    CHECK(match({ "0.2", "0.4", "0.4", "0.4" }).empty());
    // An unreadable head matches anything.
    CHECK(match({ "0.4", "?", "0.6", "0.6" }) == "0.4+0.6");
    // Heads reported by a two-head machine do not match a four-head one.
    CHECK(match({ "0.4", "0.4" }).empty());
    CHECK(NS::match_machine_variant({}, machines).empty());
    // For a mix with no machine: base machine + per-head write leaves no stale head 1.
    CHECK(NS::apply_per_head({ 0.2, 0.2, 0.2, 0.2 }, NS::plan({ "0.6", "0.4", "0.4", "0.4" }).per_head) ==
          std::vector<double>{ 0.6, 0.4, 0.4, 0.4 });
}

TEST_CASE("Apply-all on a mixed machine lists each filament once and gives each head its own nozzle variant", "[NozzleSync][U1]")
{
    const std::vector<NS::FilamentChoice> choices = {
        { "Snapmaker PLA SnapSpeed @U1", "Snapmaker PLA SnapSpeed", 0.4 },
        { "Snapmaker PLA SnapSpeed @U1 0.6 nozzle", "Snapmaker PLA SnapSpeed", 0.6 },
        { "Snapmaker PLA @U1", "Snapmaker PLA", 0.4 },
        { "Generic PLA", "Generic PLA", 0. },
        { "Snapmaker TPU High-Flow @U1", "Snapmaker TPU High-Flow", 0.4 },
        { "Snapmaker TPU High-Flow @U1 0.6 nozzle", "Snapmaker TPU High-Flow", 0.6 },
    };
    CHECK(NS::families(choices) == std::vector<std::string>{ "Snapmaker PLA SnapSpeed", "Snapmaker PLA", "Generic PLA", "Snapmaker TPU High-Flow" });

    const std::vector<double> heads = { 0.4, 0.4, 0.6, 0.6 };
    auto a = NS::assign_family_to_slots(choices, "Snapmaker PLA SnapSpeed", heads);
    REQUIRE(a.size() == 4);
    CHECK(a[0].preset == "Snapmaker PLA SnapSpeed @U1");
    CHECK(a[1].preset == "Snapmaker PLA SnapSpeed @U1");
    CHECK(a[2].preset == "Snapmaker PLA SnapSpeed @U1 0.6 nozzle");
    CHECK(a[3].preset == "Snapmaker PLA SnapSpeed @U1 0.6 nozzle");

    // A filament with no 0.6 variant: the 0.6 heads are skipped, not given the 0.4 preset.
    a = NS::assign_family_to_slots(choices, "Snapmaker PLA", heads);
    CHECK(a[0].preset == "Snapmaker PLA @U1");
    CHECK(a[1].preset == "Snapmaker PLA @U1");
    CHECK(a[2].skipped);
    CHECK(a[3].skipped);
    CHECK(a[2].preset.empty());

    // A preset that fits any nozzle goes to every head.
    a = NS::assign_family_to_slots(choices, "Generic PLA", heads);
    for (const auto &s : a) {
        CHECK_FALSE(s.skipped);
        CHECK(s.preset == "Generic PLA");
    }

    // Unknown slot nozzle (a slot beyond the machine's heads): the first variant.
    a = NS::assign_family_to_slots(choices, "Snapmaker PLA SnapSpeed", { 0.4, 0. });
    CHECK(a[1].preset == "Snapmaker PLA SnapSpeed @U1");
    // A family that is not offered at all skips everything.
    a = NS::assign_family_to_slots(choices, "Nope", heads);
    for (const auto &s : a)
        CHECK(s.skipped);
    // Equal nozzles on every head (a normal U1): each slot gets the variant for that size, as before.
    a = NS::assign_family_to_slots(choices, "Snapmaker TPU High-Flow", { 0.6, 0.6, 0.6, 0.6 });
    for (const auto &s : a)
        CHECK(s.preset == "Snapmaker TPU High-Flow @U1 0.6 nozzle");
}
