// Print-head clearance model: the loader, the nozzle outline lookup and the shipped data files.

#include <catch2/catch.hpp>

#include <cmath>
#include <set>
#include <string>

#include <boost/filesystem.hpp>

#include <libslic3r/HeadClearance.hpp>
#include <libslic3r/Utils.hpp>

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

    // Curved tops P1: the X1 head's sock is at the owner's 2.5 mm measurement (it was a 4 mm estimate),
    // the CC1's lowered to the same 2.5 mm (a guess), and only the X1 Carbon and the CC1 record a
    // curved print. The X1 and X1E share the X1 Carbon's file but are not verified.
    for (const HeadClearance &h : heads) {
        INFO(h.id);
        if (h.id == "bambu-x1" || h.id == "elegoo-centauri-carbon") {
            CHECK(h.lowest_box_z() == Approx(2.5));
            REQUIRE(h.curved_tops_verified.has_value());
            // P4: the CC1 record is the owner's 2026-11-17 arm print (2.37 mm, 20 degrees).
            const bool cc1 = h.id == "elegoo-centauri-carbon";
            CHECK(h.curved_tops_verified->rise_mm == Approx(cc1 ? 2.37 : 1.8));
            CHECK(h.curved_tops_verified->angle_deg == Approx(cc1 ? 20.0 : 10.3));
            CHECK(h.nozzle_flank_angle_deg(0.1) > 35.);
        } else
            CHECK_FALSE(h.curved_tops_verified.has_value());
        CHECK(h.curved_tops_verified_for("Bambu Lab X1 Carbon") == (h.id == "bambu-x1"));
        CHECK(h.curved_tops_verified_for("Elegoo Centauri Carbon") == (h.id == "elegoo-centauri-carbon"));
        CHECK_FALSE(h.curved_tops_verified_for("Bambu Lab X1E"));
        CHECK_FALSE(h.curved_tops_verified_for("Bambu Lab X1"));
    }
}

TEST_CASE("Head clearance: the optional curved_tops record", "[HeadClearance]")
{
    // A file without it (MINIMAL_HEAD) still loads, with no record.
    auto plain = parse_head_clearance_json(MINIMAL_HEAD);
    REQUIRE(plain.has_value());
    CHECK_FALSE(plain->curved_tops_verified.has_value());
    CHECK_FALSE(plain->curved_tops_verified_for("Test Printer"));

    const std::string base = R"({"schema": "edgeslicer.head_clearance.v1", "id": "x", "printer_models": ["A", "B"],
        "nozzle": {"profile": [[0, 0.5], [1, 1.5]]}, "boxes": [{"min": [-1, -1, 3], "max": [1, 1, 4]}], )";
    auto with = parse_head_clearance_json(base + R"("curved_tops": {"verified_printer_models": ["A"], "verified_rise_mm": 1.5,
        "verified_angle_deg": 9, "verified_on": "2026-11-16"}})");
    REQUIRE(with.has_value());
    REQUIRE(with->curved_tops_verified.has_value());
    CHECK(with->curved_tops_verified->on == "2026-11-16");
    CHECK(with->curved_tops_verified_for("A"));
    CHECK_FALSE(with->curved_tops_verified_for("B"));
    CHECK_FALSE(with->curved_tops_verified_for(""));

    // Refused: no printer models, no numbers, a model the file does not list, not an object.
    CHECK_FALSE(parse_head_clearance_json(base + R"("curved_tops": {"verified_rise_mm": 1.5, "verified_angle_deg": 9}})").has_value());
    CHECK_FALSE(parse_head_clearance_json(base + R"("curved_tops": {"verified_printer_models": ["A"], "verified_angle_deg": 9}})").has_value());
    CHECK_FALSE(parse_head_clearance_json(base + R"("curved_tops": {"verified_printer_models": ["C"], "verified_rise_mm": 1.5,
        "verified_angle_deg": 9}})").has_value());
    CHECK_FALSE(parse_head_clearance_json(base + R"("curved_tops": 3})").has_value());
}

TEST_CASE("Head clearance: nozzle flank angle", "[HeadClearance]")
{
    auto head = parse_head_clearance_json(MINIMAL_HEAD);
    REQUIRE(head.has_value());
    // Land radius 0.5. (1, 1.5): atan(1 / 1) = 45. (2, 1.5): atan(2 / 1) = 63.4. (3, 3.0): atan(3 / 2.5) = 50.2.
    CHECK(head->nozzle_flank_angle_deg(0.1) == Approx(45.).margin(1e-6));
    // A tiny chamfer at the tip counts as land below the tolerance, and binds above it.
    auto chamfer = parse_head_clearance_json(R"({"schema": "edgeslicer.head_clearance.v1", "id": "c", "printer_models": [],
        "nozzle": {"profile": [[0, 0.3], [0.05, 0.48], [1, 1.3]]}})");
    REQUIRE(chamfer.has_value());
    CHECK(chamfer->nozzle_flank_angle_deg(0.1) == Approx(std::atan2(1., 1.0) * 180. / M_PI).margin(1e-6));
    CHECK(chamfer->nozzle_flank_angle_deg(0.01) == Approx(std::atan2(0.05, 0.18) * 180. / M_PI).margin(1e-6));
}

// Design 2.1: one resolver. Precedence in P1: the first file (in load order) that lists the
// printer_model; no generic default when none does.
TEST_CASE("Head clearance: resolver precedence", "[HeadClearance]")
{
    auto make = [](const std::string &id, const std::string &models) {
        auto h = parse_head_clearance_json(R"({"schema": "edgeslicer.head_clearance.v1", "id": ")" + id +
                                           R"(", "printer_models": [)" + models + R"(], "nozzle": {"profile": [[0, 0.5]]}})");
        REQUIRE(h.has_value());
        return std::make_shared<const HeadClearance>(std::move(*h));
    };
    const HeadClearancePtrs heads = { make("a", R"("P1", "P2")"), make("b", R"("P2", "P3")") };
    REQUIRE(resolve_head_clearance("P1", heads) != nullptr);
    CHECK(resolve_head_clearance("P1", heads)->id == "a");
    CHECK(resolve_head_clearance("P2", heads)->id == "a"); // listed twice: the first file wins
    CHECK(resolve_head_clearance("P3", heads)->id == "b");
    CHECK(resolve_head_clearance("P4", heads) == nullptr); // nothing: no generic default
    CHECK(resolve_head_clearance("", heads) == nullptr);
    CHECK(resolve_head_clearance("P1", HeadClearancePtrs{}) == nullptr);

    // The shipped set, read the way the app reads it.
    const std::string saved = resources_dir();
    set_resources_dir((boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources").string());
    const HeadClearancePtr x1c = resolve_head_clearance("Bambu Lab X1 Carbon");
    const HeadClearancePtr x1e = resolve_head_clearance("Bambu Lab X1E");
    const HeadClearancePtr none = resolve_head_clearance("Generic Printer");
    set_resources_dir(saved);
    REQUIRE(x1c != nullptr);
    CHECK(x1c->id == "bambu-x1");
    CHECK(x1e == x1c);
    CHECK(none == nullptr);
}

// ---------------------------------------------------------------------------------------------
// Collision checker
// ---------------------------------------------------------------------------------------------

#include <libslic3r/GCode/HeadCollisionCheck.hpp>

namespace {

using MV = GCodeProcessorResult::MoveVertex;

MV vertex(EMoveType type, float x, float y, float z, unsigned layer = 0, ExtrusionRole role = erExternalPerimeter)
{
    MV m;
    m.type           = type;
    m.position       = Vec3f(x, y, z);
    m.layer_id       = layer;
    m.extrusion_role = type == EMoveType::Extrude ? role : erNone;
    m.delta_extruder = type == EMoveType::Extrude ? 0.1f : 0.f;
    m.width          = 0.4f;
    m.height         = 0.2f;
    return m;
}

HeadClearance test_head()
{
    auto head = parse_head_clearance_json(MINIMAL_HEAD);
    REQUIRE(head.has_value());
    return *head;
}

// A 10 mm square patch of parallel lines at height z, line spacing 0.4.
void add_patch(std::vector<MV> &moves, float x0, float y0, float z, unsigned layer, ExtrusionRole role = erTopSolidInfill)
{
    for (int i = 0; i <= 25; ++i) {
        const float y = y0 + 0.4f * i;
        moves.push_back(vertex(EMoveType::Travel, x0, y, z, layer));
        moves.push_back(vertex(EMoveType::Extrude, x0 + 10.f, y, z, layer, role));
    }
}

} // namespace

TEST_CASE("Head collision check: planar layers never hit", "[HeadClearance]")
{
    std::vector<MV> moves;
    moves.push_back(vertex(EMoveType::Travel, 0, 0, 0.2f));
    for (unsigned l = 0; l < 5; ++l)
        add_patch(moves, 0.f, 0.f, 0.2f * (l + 1), l);
    const HeadCollisionReport r = check_head_collisions(moves, test_head());
    CHECK(r.ran);
    CHECK(r.moves_checked > 200);
    CHECK(r.hit_moves == 0);
    CHECK(r.hits.empty());
}

TEST_CASE("Head collision check: travel across raised material hits the nozzle", "[HeadClearance]")
{
    std::vector<MV> moves;
    moves.push_back(vertex(EMoveType::Travel, 0, 0, 0.4f));
    // A contoured bead raised to 0.7 while the layer's nominal top is 0.4.
    moves.push_back(vertex(EMoveType::Travel, 5.f, 0.f, 0.7f, 1));
    moves.push_back(vertex(EMoveType::Extrude, 5.f, 10.f, 0.7f, 1));
    // Back down to the nominal Z, then a travel straight through it.
    moves.push_back(vertex(EMoveType::Travel, 0.f, 5.f, 0.4f, 1));
    moves.push_back(vertex(EMoveType::Travel, 10.f, 5.f, 0.4f, 1));
    const HeadCollisionReport r = check_head_collisions(moves, test_head());
    REQUIRE(r.hit_moves >= 1);
    const HeadCollisionHit &h = r.hits.back();
    CHECK(h.travel);
    CHECK(h.part == "nozzle");
    CHECK(h.layer_id == 1);
    CHECK(h.depth == Approx(0.3).margin(0.02));
    CHECK(r.max_depth == Approx(0.3).margin(0.02));

    // The same travel lifted clear of it (Z hop) does not hit.
    moves[3].position.z() = 1.0f;
    moves[4].position.z() = 1.0f;
    CHECK(check_head_collisions(moves, test_head()).hit_moves == 0);
}

TEST_CASE("Head collision check: tolerance absorbs small steps", "[HeadClearance]")
{
    std::vector<MV> moves;
    moves.push_back(vertex(EMoveType::Travel, 0, 0, 0.4f));
    moves.push_back(vertex(EMoveType::Travel, 5.f, 0.f, 0.45f, 1));
    moves.push_back(vertex(EMoveType::Extrude, 5.f, 10.f, 0.45f, 1));
    moves.push_back(vertex(EMoveType::Travel, 0.f, 5.f, 0.4f, 1));
    moves.push_back(vertex(EMoveType::Travel, 10.f, 5.f, 0.4f, 1));
    CHECK(check_head_collisions(moves, test_head()).hit_moves == 0);
}

TEST_CASE("Head collision check: a tall neighbour reaches a box", "[HeadClearance]")
{
    std::vector<MV> moves;
    moves.push_back(vertex(EMoveType::Travel, 20.f, 0.f, 6.f));
    // A tall bead 6 mm high at x = 20 (only its top matters to the height map).
    moves.push_back(vertex(EMoveType::Extrude, 20.f, 10.f, 6.f, 29));
    // A low move 5 mm beside it: clear of the nozzle (radius 3), inside the duct box (|x| <= 10, z >= 4).
    moves.push_back(vertex(EMoveType::Travel, 15.f, 0.f, 1.f, 4));
    moves.push_back(vertex(EMoveType::Extrude, 15.f, 10.f, 1.f, 4));
    const HeadCollisionReport r = check_head_collisions(moves, test_head());
    REQUIRE(r.hit_moves >= 1);
    CHECK(r.hits.front().part == "duct");
    CHECK(r.hits.front().depth == Approx(1.).margin(0.05));
}

TEST_CASE("Head collision check: ironing and start G-code are not reported", "[HeadClearance]")
{
    std::vector<MV> moves;
    // Start G-code purge line far above everything, then lowered: prepare_stage moves are ignored.
    MV purge_a = vertex(EMoveType::Travel, 0.f, 5.f, 3.f);
    MV purge_b = vertex(EMoveType::Extrude, 10.f, 5.f, 3.f);
    purge_a.prepare_stage = purge_b.prepare_stage = true;
    moves.push_back(purge_a);
    moves.push_back(purge_b);
    moves.push_back(vertex(EMoveType::Travel, 5.f, 0.f, 0.6f, 1));
    moves.push_back(vertex(EMoveType::Extrude, 5.f, 10.f, 0.6f, 1));
    // Ironing deliberately rubs: below the bead by 0.2, no nozzle report.
    moves.push_back(vertex(EMoveType::Extrude, 0.f, 5.f, 0.4f, 1, erIroning));
    moves.push_back(vertex(EMoveType::Extrude, 10.f, 5.f, 0.4f, 1, erIroning));
    CHECK(check_head_collisions(moves, test_head()).hit_moves == 0);
}

// ---------------------------------------------------------------------------------------------
// Curved tops: HeadCollisionParams::extrusion_surface_slope_deg (the tip land on a sloped neighbour)
// ---------------------------------------------------------------------------------------------

namespace {

// A neighbour bead 0.4 wide along y at x = nx, top nz (layer 0), then a move of `type` along x = 0 at
// z 1.0 on `layer`, reached from above out of the bead's reach. MINIMAL_HEAD: tip land radius 0.5,
// underside u(d) = d - 0.5 up to d = 1.5, so the land region (u <= 0.1 tolerance) ends at d = 0.6.
std::vector<MV> neighbour_case(float nx, float nz, EMoveType type, unsigned layer = 1)
{
    std::vector<MV> moves;
    moves.push_back(vertex(EMoveType::Travel, nx, 0.f, nz, 0));
    moves.push_back(vertex(EMoveType::Extrude, nx, 10.f, nz, 0, erTopSolidInfill));
    moves.push_back(vertex(EMoveType::Travel, nx, 10.f, nz + 6.f, layer));
    moves.push_back(vertex(EMoveType::Travel, 0.f, -5.f, nz + 6.f, layer));
    moves.push_back(vertex(EMoveType::Travel, 0.f, -5.f, 1.f, layer));
    moves.push_back(vertex(type, 0.f, 10.f, 1.f, layer, erTopSolidInfill));
    return moves;
}

HeadCollisionParams sloped(double deg, unsigned min_layer = 0)
{
    HeadCollisionParams p;
    p.extrusion_surface_slope_deg       = deg;
    p.extrusion_surface_slope_min_layer = min_layer;
    return p;
}

} // namespace

TEST_CASE("Head collision check: the curved-tops slope allowance covers only the tip land on extrusions", "[HeadClearance][CurvedTops]")
{
    const HeadClearance head = test_head();

    // A neighbour 0.15 above the tip at 0.25-0.65 from the axis, inside the land: a hit with no slope
    // (0.15 > 0.1), the surface being followed with a 20 degree slope (0.15 - 0.25 * tan 20 < 0.1).
    const auto neighbour = neighbour_case(0.45f, 1.15f, EMoveType::Extrude);
    {
        const HeadCollisionReport r = check_head_collisions(neighbour, head);
        REQUIRE(r.hit_moves == 1);
        CHECK(r.hits.front().part == "nozzle");
        CHECK_FALSE(r.hits.front().travel);
    }
    CHECK(check_head_collisions(neighbour, head, sloped(20.)).hit_moves == 0);
    // Only on the block's layers: an extrusion on an earlier layer gets no allowance.
    CHECK(check_head_collisions(neighbour, head, sloped(20., 2)).hit_moves == 1);

    // Higher than d * tan(a) + tolerance inside the land: still a hit with the slope.
    CHECK(check_head_collisions(neighbour_case(0.45f, 1.4f, EMoveType::Extrude), head, sloped(20.)).hit_moves == 1);

    // The same neighbour met by a travel or a wipe: a hit, the allowance is for extrusions only.
    for (const EMoveType type : { EMoveType::Travel, EMoveType::Wipe }) {
        const HeadCollisionReport r = check_head_collisions(neighbour_case(0.45f, 1.15f, type), head, sloped(20.));
        INFO("move type " << int(type));
        REQUIRE(r.hit_moves == 1);
        CHECK(r.hits.front().travel);
    }

    // Beyond the land, under the cone (d 0.75-1.05, u 0.25-0.55): 0.5 above the tip is a hit even
    // with the steepest slope the check takes, which would cover it if it were applied to the cone.
    {
        const HeadCollisionReport r = check_head_collisions(neighbour_case(0.9f, 1.5f, EMoveType::Extrude), head, sloped(60.));
        REQUIRE(r.hit_moves == 1);
        CHECK(r.hits.front().part == "nozzle");
    }

    // Support and stabilizer extrusions are obstacles like any other (curved tops P2 allows supports):
    // a support bead beyond the land, under the cone, met by a curved extrusion on the block's layer.
    for (const ExtrusionRole role : { erSupportMaterial, erSupportMaterialInterface, erSupportTransition }) {
        auto moves = neighbour_case(0.9f, 1.5f, EMoveType::Extrude);
        moves[1].extrusion_role = role;
        const HeadCollisionReport r = check_head_collisions(moves, head, sloped(20.));
        INFO("support role " << int(role));
        REQUIRE(r.hit_moves == 1);
        CHECK(r.hits.front().part == "nozzle");
        CHECK_FALSE(r.hits.front().travel);
    }

    // Boxes are never relaxed: material reaching 0.5 into the duct (bottom 4 above the tip) 5 mm away.
    // (The descent before it meets the duct too; the extrusion's own hit is the one checked.)
    {
        const HeadCollisionReport r = check_head_collisions(neighbour_case(5.f, 5.5f, EMoveType::Extrude), head, sloped(60.));
        bool extrusion_duct_hit = false;
        for (const HeadCollisionHit &h : r.hits)
            if (!h.travel && h.part == "duct" && h.depth > 0.45f && h.depth < 0.55f)
                extrusion_duct_hit = true;
        CHECK(extrusion_duct_hit);
    }
}

// Curved tops P2, partial curving (review b): the lowest curved bead lies one layer (0.2) above the flat
// plateau at the cut. Here the curved extrusion runs at Z 1.0 = cut + h, so the cut is Z 0.8. With the
// curved block's slope allowance (10 degrees) on, as in a real curved print:
//  - the plateau at the cut, right beside the tip in the land: clean;
//  - the plateau level with the lowest bead (the flat layer at cut + h): clean;
//  - a flat flank one layer above the lowest bead under the cone: a near miss, clean;
//  - a flat flank two layers above it there (what the planner's cut must prevent): a nozzle contact,
//    which stops a curved print's export.
TEST_CASE("Head collision check: a curved bead beside the flat plateau and a flank above it", "[HeadClearance][CurvedTops]")
{
    const HeadClearance head = test_head();
    CHECK(check_head_collisions(neighbour_case(0.45f, 0.8f, EMoveType::Extrude), head, sloped(10.)).hit_moves == 0);
    CHECK(check_head_collisions(neighbour_case(0.45f, 1.0f, EMoveType::Extrude), head, sloped(10.)).hit_moves == 0);
    CHECK(check_head_collisions(neighbour_case(0.9f, 1.2f, EMoveType::Extrude), head, sloped(10.)).hit_moves == 0);
    const HeadCollisionReport r = check_head_collisions(neighbour_case(0.9f, 1.4f, EMoveType::Extrude), head, sloped(10.));
    REQUIRE(r.hit_moves == 1);
    CHECK(r.hits.front().part == "nozzle");
    CHECK_FALSE(r.hits.front().travel);
    CHECK(r.max_depth > 0.1f);
}

// ---------------------------------------------------------------------------------------------
// Curved tops P4: the nozzle's own contact tolerance (HeadCollisionParams::nozzle_contact_tolerance)
// ---------------------------------------------------------------------------------------------

namespace {

HeadCollisionParams nozzle_tolerant(double tol, unsigned min_layer = 0)
{
    HeadCollisionParams p;
    p.nozzle_contact_tolerance = tol;
    p.nozzle_contact_min_layer = min_layer;
    return p;
}

} // namespace

// MINIMAL_HEAD: tip land radius 0.5, so a neighbour bead at x = 0.45 (0.25 to 0.65 from the axis) sits under
// the land; its top above the tip (z 1.0) is the contact depth.
TEST_CASE("Head collision check P4: the nozzle tolerance splits nozzle contacts from box contacts", "[HeadClearance][CurvedTops]")
{
    const HeadClearance head = test_head();

    // Off (the default, and Z contouring's): a 0.3 mm nozzle contact is a hit, nothing is tolerated.
    {
        const HeadCollisionReport r = check_head_collisions(neighbour_case(0.45f, 1.3f, EMoveType::Extrude), head);
        CHECK(r.hit_moves == 1);
        CHECK(r.tolerated_moves == 0);
    }
    // On: the same extrusion is tolerated, counted and measured, and is no hit.
    {
        const HeadCollisionReport r = check_head_collisions(neighbour_case(0.45f, 1.3f, EMoveType::Extrude), head, nozzle_tolerant(0.4));
        CHECK(r.hit_moves == 0);
        CHECK(r.hits.empty());
        CHECK(r.tolerated_moves == 1);
        CHECK(r.tolerated_max_depth > 0.25f);
        CHECK(r.tolerated_max_depth < 0.35f);
        // The per-part statistics still show the nozzle contact.
        REQUIRE(r.parts.size() >= 2);
        CHECK(r.parts[0].hit_moves + r.parts[1].hit_moves >= 1);
    }
    // A wipe back along the bead and a vertical drop onto it are tolerated too; a horizontal travel is not.
    CHECK(check_head_collisions(neighbour_case(0.45f, 1.3f, EMoveType::Wipe), head, nozzle_tolerant(0.4)).hit_moves == 0);
    {
        const HeadCollisionReport r = check_head_collisions(neighbour_case(0.45f, 1.3f, EMoveType::Travel), head, nozzle_tolerant(0.4));
        CHECK(r.hit_moves == 1);
        CHECK(r.tolerated_moves == 0);
    }
    {
        std::vector<MV> moves;
        moves.push_back(vertex(EMoveType::Travel, 0.45f, 0.f, 1.3f, 0));
        moves.push_back(vertex(EMoveType::Extrude, 0.45f, 10.f, 1.3f, 0, erTopSolidInfill));
        moves.push_back(vertex(EMoveType::Travel, 0.f, 5.f, 3.f, 1));
        moves.push_back(vertex(EMoveType::Travel, 0.f, 5.f, 1.f, 1)); // straight down beside the bead
        const HeadCollisionReport r = check_head_collisions(moves, head, nozzle_tolerant(0.4));
        CHECK(r.hit_moves == 0);
        CHECK(r.tolerated_moves == 1);
    }
    // Only from the block's layer on.
    CHECK(check_head_collisions(neighbour_case(0.45f, 1.3f, EMoveType::Extrude), head, nozzle_tolerant(0.4, 2)).hit_moves == 1);

    // A box contact is never tolerated, however large the nozzle tolerance: material 0.5 into the duct.
    {
        const HeadCollisionReport r = check_head_collisions(neighbour_case(5.f, 5.5f, EMoveType::Extrude), head, nozzle_tolerant(1.0));
        bool extrusion_duct_hit = false;
        for (const HeadCollisionHit &h : r.hits)
            if (!h.travel && h.part == "duct")
                extrusion_duct_hit = true;
        CHECK(extrusion_duct_hit);
    }
    // One move touching both the nozzle (within the tolerance) and the duct: a hit, not tolerated.
    {
        std::vector<MV> moves;
        moves.push_back(vertex(EMoveType::Travel, 0.45f, 0.f, 1.3f, 0));
        moves.push_back(vertex(EMoveType::Extrude, 0.45f, 10.f, 1.3f, 0, erTopSolidInfill));
        moves.push_back(vertex(EMoveType::Travel, 5.f, 0.f, 5.5f, 0));
        moves.push_back(vertex(EMoveType::Extrude, 5.f, 10.f, 5.5f, 0, erTopSolidInfill));
        moves.push_back(vertex(EMoveType::Travel, 0.f, -5.f, 12.f, 1));
        moves.push_back(vertex(EMoveType::Travel, 0.f, -12.f, 12.f, 1));
        moves.push_back(vertex(EMoveType::Travel, 0.f, -12.f, 1.f, 1)); // the duct (|y| <= 10) is clear of y 0-10 here
        moves.push_back(vertex(EMoveType::Extrude, 0.f, 10.f, 1.f, 1, erTopSolidInfill));
        const HeadCollisionReport r = check_head_collisions(moves, head, nozzle_tolerant(1.0));
        REQUIRE(r.hit_moves >= 1);
        bool both = false;
        for (const HeadCollisionHit &h : r.hits)
            both |= !h.travel && h.part == "duct";
        CHECK(both);
        CHECK(r.tolerated_moves == 0);
    }
}

TEST_CASE("Head collision check P4: a nozzle contact just inside the tolerance passes, just outside it is a hit", "[HeadClearance][CurvedTops]")
{
    const HeadClearance head = test_head();
    // Contact 0.38 and 0.42 against a 0.4 tolerance.
    {
        const HeadCollisionReport r = check_head_collisions(neighbour_case(0.45f, 1.38f, EMoveType::Extrude), head, nozzle_tolerant(0.4));
        CHECK(r.hit_moves == 0);
        CHECK(r.tolerated_moves == 1);
        CHECK(r.tolerated_max_depth <= 0.4f);
    }
    {
        const HeadCollisionReport r = check_head_collisions(neighbour_case(0.45f, 1.42f, EMoveType::Extrude), head, nozzle_tolerant(0.4));
        REQUIRE(r.hit_moves == 1);
        CHECK(r.hits.front().part == "nozzle");
        CHECK(r.max_depth > 0.4f);
        CHECK(r.tolerated_moves == 0);
    }
}

TEST_CASE("Head clearance P4: the curved tops record may carry the deepest nozzle contact printed", "[HeadClearance]")
{
    const std::string base = R"({"schema": "edgeslicer.head_clearance.v1", "id": "c", "printer_models": ["C"],
        "nozzle": {"profile": [[0, 0.5]]}, )";
    auto with = parse_head_clearance_json(base + R"("curved_tops": {"verified_printer_models": ["C"], "verified_rise_mm": 2.37,
        "verified_angle_deg": 20, "verified_nozzle_contact_mm": 0.61, "verified_on": "2026-11-17"}})");
    REQUIRE(with.has_value());
    REQUIRE(with->curved_tops_verified.has_value());
    CHECK(with->curved_tops_verified->nozzle_contact_mm == Approx(0.61));
    auto without = parse_head_clearance_json(base + R"("curved_tops": {"verified_printer_models": ["C"], "verified_rise_mm": 1.8,
        "verified_angle_deg": 10.3}})");
    REQUIRE(without.has_value());
    CHECK(without->curved_tops_verified->nozzle_contact_mm == Approx(0.));

    // The shipped Centauri Carbon file records the owner's 2026-11-17 prints.
    const std::string saved = resources_dir();
    set_resources_dir((boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources").string());
    const HeadClearancePtr cc1 = resolve_head_clearance("Elegoo Centauri Carbon");
    set_resources_dir(saved);
    REQUIRE(cc1 != nullptr);
    REQUIRE(cc1->curved_tops_verified.has_value());
    CHECK(cc1->curved_tops_verified->rise_mm == Approx(2.37));
    CHECK(cc1->curved_tops_verified->angle_deg == Approx(20.0));
    CHECK(cc1->curved_tops_verified->nozzle_contact_mm == Approx(0.61));
    CHECK(cc1->curved_tops_verified->on == "2026-11-17");
}
