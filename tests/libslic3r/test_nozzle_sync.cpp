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
