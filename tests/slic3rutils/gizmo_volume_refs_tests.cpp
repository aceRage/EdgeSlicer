#include <catch2/catch.hpp>

#include "slic3r/GUI/Gizmos/GizmoVolumeRefs.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

using namespace Slic3r::GUI::GizmoVolumeRefs;

// EDGESLICER-8: the Measure / Assembly gizmo cached GLVolume* -> pick item (a raw pointer to
// GLVolume::mesh_raycaster) and ray-cast through it after GLCanvas3D::reload_scene() had deleted the
// volume. GLVolume and MeshRaycaster need GL, so the bookkeeping lives in GizmoVolumeRefs.hpp as
// templates and is driven here with stand-ins: a "volume" owns a "raycaster", the canvas is the list
// of live volume pointers, and a pick item remembers the raycaster it was built from.
//
// The stand-in volumes that "the canvas deleted" are kept alive by the test and only dropped from the
// live list, so no freed pointer is ever touched here either.

namespace {

struct FakeRaycaster {};

struct FakeVolume
{
    std::unique_ptr<FakeRaycaster> raycaster = std::make_unique<FakeRaycaster>();
};

struct FakeItem
{
    const FakeRaycaster *raycaster;
    const FakeRaycaster *get_raycaster() const { return raycaster; }
};

using Live  = LiveVolumes<FakeVolume, FakeRaycaster>;
using Cache = std::map<FakeVolume *, std::shared_ptr<FakeItem>>;

Live live_of(const std::vector<FakeVolume *> &canvas)
{
    return Live(canvas, [](const FakeVolume &v) { return v.raycaster.get(); });
}

std::shared_ptr<FakeItem> pick_of(const FakeVolume &v) { return std::make_shared<FakeItem>(FakeItem{v.raycaster.get()}); }

} // namespace

TEST_CASE("A cache built from the current selection is not stale", "[GizmoVolumeRefs]")
{
    FakeVolume a, b, c;
    const std::vector<FakeVolume *> canvas{&a, &b, &c};
    const Cache cache{{&a, pick_of(a)}, {&b, pick_of(b)}};

    REQUIRE(stale_pick_keys(cache, live_of(canvas)).empty());
    REQUIRE_FALSE(has_dead_key(cache, live_of(canvas)));
}

TEST_CASE("Assemble: both picked volumes leave the canvas, a new one arrives", "[GizmoVolumeRefs]")
{
    // The crash sequence: picks on two objects, then Assemble (or merge / delete / undo) makes
    // reload_scene() delete their GLVolumes and load one for the new object.
    FakeVolume a, b, other, assembled;
    const Cache cache{{&a, pick_of(a)}, {&b, pick_of(b)}};

    const std::vector<FakeVolume *> after{&other, &assembled};
    const Live live = live_of(after);

    std::vector<FakeVolume *> stale = stale_pick_keys(cache, live);
    std::sort(stale.begin(), stale.end());
    std::vector<FakeVolume *> expected{&a, &b};
    std::sort(expected.begin(), expected.end());
    REQUIRE(stale == expected);
    REQUIRE(has_dead_key(cache, live));
}

TEST_CASE("Only the removed volume is stale when one of two picks goes", "[GizmoVolumeRefs]")
{
    FakeVolume a, b;
    const Cache cache{{&a, pick_of(a)}, {&b, pick_of(b)}};

    const std::vector<FakeVolume *> after{&b};
    REQUIRE(stale_pick_keys(cache, live_of(after)) == std::vector<FakeVolume *>{&a});
}

TEST_CASE("A live volume whose raycaster was rebuilt is stale too", "[GizmoVolumeRefs]")
{
    // reload_scene() can keep the GLVolume and give it a new mesh_raycaster (SLA hollowing reload),
    // and a freed GLVolume's address can be reused for a new one: either way the pointer key still
    // matches a live volume, but the item points at a raycaster that volume no longer owns.
    FakeVolume a;
    const Cache cache{{&a, pick_of(a)}};
    std::unique_ptr<FakeRaycaster> old = std::move(a.raycaster); // keep the old one alive for the test
    a.raycaster                        = std::make_unique<FakeRaycaster>();

    const std::vector<FakeVolume *> canvas{&a};
    REQUIRE(stale_pick_keys(cache, live_of(canvas)) == std::vector<FakeVolume *>{&a});
    // The volume itself is alive, so a map that only keys on it is fine.
    REQUIRE_FALSE(has_dead_key(cache, live_of(canvas)));
}

TEST_CASE("An empty pick item counts as stale", "[GizmoVolumeRefs]")
{
    FakeVolume a;
    const Cache cache{{&a, nullptr}};
    const std::vector<FakeVolume *> canvas{&a};
    REQUIRE(stale_pick_keys(cache, live_of(canvas)) == std::vector<FakeVolume *>{&a});
}

TEST_CASE("reset_volumes(): an empty canvas makes every cached pointer dead", "[GizmoVolumeRefs]")
{
    FakeVolume a, b;
    const Cache cache{{&a, pick_of(a)}, {&b, pick_of(b)}};
    const std::vector<FakeVolume *> empty_canvas;
    const Live live = live_of(empty_canvas);

    REQUIRE(stale_pick_keys(cache, live).size() == 2);
    REQUIRE(has_dead_pointer(std::vector<FakeVolume *>{&a}, live));
    REQUIRE_FALSE(live.contains(&a));
    REQUIRE(live.raycaster_of(&a) == nullptr);
}

TEST_CASE("Hit lists: nullptr means nothing picked, any other dead pointer is stale", "[GizmoVolumeRefs]")
{
    FakeVolume a, b;
    const std::vector<FakeVolume *> canvas{&a};
    const Live live = live_of(canvas);

    REQUIRE_FALSE(has_dead_pointer(std::vector<FakeVolume *>{}, live));
    REQUIRE_FALSE(has_dead_pointer(std::vector<FakeVolume *>{nullptr, &a}, live));
    REQUIRE(has_dead_pointer(std::vector<FakeVolume *>{&a, &b}, live));
    REQUIRE(live.raycaster_of(&a) == a.raycaster.get());
}

TEST_CASE("Null entries in the canvas list are ignored", "[GizmoVolumeRefs]")
{
    FakeVolume a;
    const std::vector<FakeVolume *> canvas{nullptr, &a};
    const Live live = live_of(canvas);
    REQUIRE(live.contains(&a));
    REQUIRE_FALSE(live.contains(nullptr));
}
