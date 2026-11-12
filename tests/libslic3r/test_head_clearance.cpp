// Print-head clearance model: the loader, the nozzle outline lookup and the shipped data files.

#include <catch2/catch.hpp>

#include <cmath>
#include <set>
#include <string>

#include <boost/filesystem.hpp>

#include <libslic3r/HeadClearance.hpp>

using namespace Slic3r;

static const char *MINIMAL_HEAD = R"({
    "schema": "edgeslicer.head_clearance.v1",
    "id": "test-head",
    "printer_models": ["Test Printer"],
    "nozzle": { "profile": [[0, 0.5], [1, 1.5], [2, 1.5], [3, 3.0]] },
    "boxes": [
        {"name": "duct", "min": [-10, -10, 4], "max": [10, 10, 20]},
        {"name": "gantry", "min": [0, -20, 30], "max": [0, 20, 60], "spans_x": true}
    ]
})";

TEST_CASE("Head clearance: parse a minimal head", "[HeadClearance]")
{
    std::string err;
    auto        head = parse_head_clearance_json(MINIMAL_HEAD, &err);
    REQUIRE(head.has_value());
    CHECK(err.empty());
    CHECK(head->id == "test-head");
    CHECK(head->name == "test-head");
    CHECK_FALSE(head->calibrated);
    CHECK(head->nozzle_estimated);
    CHECK(head->matches_printer_model("Test Printer"));
    CHECK_FALSE(head->matches_printer_model("Other"));
    CHECK_FALSE(head->matches_printer_model(""));
    REQUIRE(head->boxes.size() == 2);
    CHECK(head->boxes[1].spans_x);
    CHECK(head->boxes[0].estimated);
    CHECK(head->lowest_box_z() == Approx(4.));
    CHECK(head->nozzle_max_radius() == Approx(3.));
}

TEST_CASE("Head clearance: nozzle underside height", "[HeadClearance]")
{
    auto head = parse_head_clearance_json(MINIMAL_HEAD);
    REQUIRE(head.has_value());
    CHECK(head->nozzle_underside_at(0.) == Approx(0.));
    CHECK(head->nozzle_underside_at(0.5) == Approx(0.));
    CHECK(head->nozzle_underside_at(-0.3) == Approx(0.));
    CHECK(head->nozzle_underside_at(1.0) == Approx(0.5));
    // A vertical stretch of the outline (radius 1.5 from z 1 to z 2): the underside is its bottom.
    CHECK(head->nozzle_underside_at(1.5) == Approx(1.0));
    CHECK(head->nozzle_underside_at(2.25) == Approx(2.5));
    CHECK(std::isinf(head->nozzle_underside_at(3.01)));
}

TEST_CASE("Head clearance: malformed files are refused", "[HeadClearance]")
{
    std::string err;
    CHECK_FALSE(parse_head_clearance_json("not json", &err).has_value());
    CHECK_FALSE(err.empty());
    CHECK_FALSE(parse_head_clearance_json(R"({"schema": "other", "id": "x"})").has_value());
    // Profile must start at the tip.
    CHECK_FALSE(parse_head_clearance_json(R"({"schema": "edgeslicer.head_clearance.v1", "id": "x", "printer_models": [],
        "nozzle": {"profile": [[0.5, 1]]}})").has_value());
    // Radii must not shrink going up.
    CHECK_FALSE(parse_head_clearance_json(R"({"schema": "edgeslicer.head_clearance.v1", "id": "x", "printer_models": [],
        "nozzle": {"profile": [[0, 1], [1, 0.5]]}})").has_value());
    // A box may not reach down to the tip plane.
    CHECK_FALSE(parse_head_clearance_json(R"({"schema": "edgeslicer.head_clearance.v1", "id": "x", "printer_models": [],
        "nozzle": {"profile": [[0, 1]]}, "boxes": [{"min": [-1, -1, 0], "max": [1, 1, 2]}]})").has_value());
    // Empty extent.
    CHECK_FALSE(parse_head_clearance_json(R"({"schema": "edgeslicer.head_clearance.v1", "id": "x", "printer_models": [],
        "nozzle": {"profile": [[0, 1]]}, "boxes": [{"min": [1, -1, 3], "max": [1, 1, 4]}]})").has_value());
}

TEST_CASE("Head clearance: shipped data files load and cover the first printers", "[HeadClearance]")
{
    const std::string dir = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "head_clearance").string();
    std::vector<std::string> errors;
    const auto               heads = load_head_clearances(dir, &errors);
    INFO((errors.empty() ? std::string() : errors.front()));
    CHECK(errors.empty());
    REQUIRE(heads.size() >= 4);

    std::set<std::string> ids;
    for (const HeadClearance &h : heads) {
        ids.insert(h.id);
        // Nothing shipped is calibrated yet; flip this when a head is measured.
        CHECK_FALSE(h.calibrated);
        CHECK_FALSE(h.source.empty());
        CHECK(h.nozzle_underside_at(0.) == Approx(0.));
    }
    CHECK(ids.size() == heads.size());

    for (const char *model : {"Elegoo Centauri Carbon", "Bambu Lab X1 Carbon", "Bambu Lab H2D", "Snapmaker U1"}) {
        int n = 0;
        for (const HeadClearance &h : heads)
            n += h.matches_printer_model(model) ? 1 : 0;
        INFO(model);
        CHECK(n == 1);
    }
}
