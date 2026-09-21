#include <catch2/catch.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ShortestPath.hpp"

using namespace Slic3r;

static ExtrusionPath make_path(coord_t x0, coord_t y0, coord_t x1, coord_t y1)
{
    ExtrusionPath path(erPerimeter, 0.04, 0.4f, 0.2f);
    path.polyline = Polyline{Point(x0, y0), Point(x1, y1)};
    return path;
}

TEST_CASE("chain_and_reorder_extrusion_entities drops nullptr entries", "[ShortestPath]")
{
    std::vector<ExtrusionEntity *> entities{nullptr};
    REQUIRE_NOTHROW(chain_and_reorder_extrusion_entities(entities));
    REQUIRE(entities.empty());
}

TEST_CASE("chain_and_reorder_extrusion_entities drops empty front-child paths and collections", "[ShortestPath]")
{
    SECTION("empty path") {
        ExtrusionPath empty(erPerimeter);
        std::vector<ExtrusionEntity *> entities{&empty};
        REQUIRE(empty.polyline.points.empty());
        REQUIRE_NOTHROW(chain_and_reorder_extrusion_entities(entities));
        REQUIRE(entities.empty());
    }

    SECTION("collection whose only child is an empty path") {
        ExtrusionEntityCollection col;
        col.append(ExtrusionPath(erPerimeter));
        REQUIRE_FALSE(col.empty());
        std::vector<ExtrusionEntity *> entities{&col};
        REQUIRE_NOTHROW(chain_and_reorder_extrusion_entities(entities));
        REQUIRE(entities.empty());
    }

    SECTION("collection with an empty front child and a valid back child") {
        ExtrusionEntityCollection col;
        col.append(ExtrusionPath(erPerimeter));
        col.append(make_path(0, 0, 1000, 0));
        std::vector<ExtrusionEntity *> entities{&col};
        REQUIRE_NOTHROW(chain_and_reorder_extrusion_entities(entities));
        REQUIRE(entities.empty());
    }
}

TEST_CASE("chain_and_reorder_extrusion_entities drops a degenerate middle entity and keeps neighbors", "[ShortestPath]")
{
    ExtrusionPath left  = make_path(0, 0, 1000, 0);
    ExtrusionPath empty(erPerimeter);
    ExtrusionPath right = make_path(4000, 0, 5000, 0);
    std::vector<ExtrusionEntity *> entities{&left, nullptr, &empty, &right};

    REQUIRE_NOTHROW(chain_and_reorder_extrusion_entities(entities));
    REQUIRE(entities.size() == 2);
    CHECK(entities[0] != nullptr);
    CHECK(entities[1] != nullptr);
    CHECK_FALSE(entities[0]->first_point() == entities[1]->first_point());
}

TEST_CASE("chain_and_reorder_extrusion_entities reorders a valid mixed set", "[ShortestPath]")
{
    ExtrusionPath far  = make_path(8000, 0, 9000, 0);
    ExtrusionPath near = make_path(0, 0, 1000, 0);
    ExtrusionEntityCollection col;
    col.append(make_path(2000, 0, 3000, 0));

    std::vector<ExtrusionEntity *> entities{&far, &col, &near};
    const Point                    start_near(0, 0);
    REQUIRE_NOTHROW(chain_and_reorder_extrusion_entities(entities, &start_near));
    REQUIRE(entities.size() == 3);
    CHECK(entities.front()->first_point() == start_near);
}
