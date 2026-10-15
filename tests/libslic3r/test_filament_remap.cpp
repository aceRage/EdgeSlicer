// Colour painting "Remap filaments" (OrcaSlicer #12016 / #12437), adapted to the Edge gizmo, which
// lists filaments in the sidebar's order (mixed filaments included) rather than by filament id.
#include <catch2/catch.hpp>

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>

#include <algorithm>
#include <vector>

using namespace Slic3r;

namespace {

EnforcerBlockerType ebt(unsigned int state) { return static_cast<EnforcerBlockerType>(state); }

// its_make_cube facets: 0,1 bottom; 2,3 top; 4,5 +X; 6,7 -Y; 8,9 -X; 10,11 +Y.
TriangleSelector::TriangleSplittingData paint_cube(const TriangleMesh &mesh, const std::vector<std::pair<int, unsigned int>> &facets)
{
    TriangleSelector selector(mesh);
    for (const auto &[facet, state] : facets)
        selector.set_facet(facet, ebt(state));
    return selector.serialize();
}

} // namespace

TEST_CASE("The used painted states are read from the serialized painting", "[TriangleSelector][FilamentRemap]")
{
    const TriangleMesh mesh(its_make_cube(10., 10., 10.));

    SECTION("Unpainted") {
        const std::vector<EnforcerBlockerType> states = TriangleSelector::extract_used_facet_states(paint_cube(mesh, {}));
        CHECK(std::find(states.begin(), states.end(), ebt(2)) == states.end());
        CHECK(std::find(states.begin(), states.end(), ebt(3)) == states.end());
    }
    SECTION("Painted with filaments 2 and 5") {
        const std::vector<EnforcerBlockerType> states = TriangleSelector::extract_used_facet_states(paint_cube(mesh, {{2, 2}, {3, 2}, {4, 5}}));
        CHECK(std::find(states.begin(), states.end(), ebt(2)) != states.end());
        CHECK(std::find(states.begin(), states.end(), ebt(5)) != states.end());
        CHECK(std::find(states.begin(), states.end(), ebt(3)) == states.end());
        CHECK(std::is_sorted(states.begin(), states.end()));
    }
}

TEST_CASE("A filament remap maps painted states through the gizmo's display order", "[TriangleSelector][FilamentRemap]")
{
    EnforcerBlockerStateMap state_map;

    SECTION("Identity remap changes nothing") {
        CHECK_FALSE(TriangleSelector::build_filament_remap_state_map({1, 2, 3}, {0, 1, 2}, state_map));
        for (size_t i = 0; i < state_map.size(); ++i)
            CHECK(state_map[i] == ebt(unsigned(i)));
    }
    SECTION("Slots follow the sidebar order, not the filament ids") {
        // The sidebar shows filament 3 first, then 1, then mixed filament 5 (physical 1-4).
        const std::vector<unsigned int> display = {3, 1, 5};
        // Slot 0 (filament 3) -> slot 2 (filament 5); slot 2 (filament 5) -> slot 0 (filament 3).
        REQUIRE(TriangleSelector::build_filament_remap_state_map(display, {2, 1, 0}, state_map));
        CHECK(state_map[3] == ebt(5));
        CHECK(state_map[5] == ebt(3));
        CHECK(state_map[1] == ebt(1));
        CHECK(state_map[2] == ebt(2));
        CHECK(state_map[0] == ebt(0));
    }
    SECTION("Out-of-range slots are ignored") {
        CHECK_FALSE(TriangleSelector::build_filament_remap_state_map({2, 3}, {7, 1}, state_map));
        CHECK(state_map[2] == ebt(2));
    }
}

TEST_CASE("Remapping painted triangles swaps two filaments", "[TriangleSelector][FilamentRemap]")
{
    const TriangleMesh mesh(its_make_cube(10., 10., 10.));
    TriangleSelector   selector(mesh);
    selector.set_facet(2, ebt(2));
    selector.set_facet(4, ebt(3));

    EnforcerBlockerStateMap state_map;
    REQUIRE(TriangleSelector::build_filament_remap_state_map({1, 2, 3}, {0, 2, 1}, state_map));
    selector.remap_triangle_state(state_map);

    const std::vector<EnforcerBlockerType> states = TriangleSelector::extract_used_facet_states(selector.serialize());
    CHECK(std::find(states.begin(), states.end(), ebt(2)) != states.end());
    CHECK(std::find(states.begin(), states.end(), ebt(3)) != states.end());
    CHECK(selector.get_facets(ebt(3)).indices.size() == 1);
    CHECK(selector.get_facets(ebt(2)).indices.size() == 1);
    // Facet 2 was filament 2 and is filament 3 now.
    const indexed_triangle_set now3 = selector.get_facets(ebt(3));
    REQUIRE(now3.indices.size() == 1);
    for (int v = 0; v < 3; ++v)
        CHECK(now3.vertices[now3.indices.front()[v]].z() == Approx(10.f));
}
