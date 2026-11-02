#include <catch2/catch.hpp>

#include "slic3r/GUI/CadLazy.hpp"

#include <memory>
#include <string>
#include <vector>

using Slic3r::GUI::LazyInstance;

namespace {
struct FakePanel : LazyInstance<FakePanel>
{
    FakePanel() { register_instance(); }
    std::vector<std::string> ran;
};
} // namespace

TEST_CASE("LazyInstance keeps one pending callback per key until the panel is built", "[CadLazy][P4]")
{
    REQUIRE(FakePanel::if_built() == nullptr);
    FakePanel::drop_all_deferred();

    // A tab that is never shown must not grow the queue: repeated K/W presses replace.
    for (int i = 0; i < 100; ++i) {
        FakePanel::when_built("launch", [](FakePanel &p) { p.ran.push_back("sketch"); });
        FakePanel::when_built("launch", [](FakePanel &p) { p.ran.push_back("primitive"); });
    }
    FakePanel::when_built("rescale", [](FakePanel &p) { p.ran.push_back("rescale"); });
    REQUIRE(FakePanel::deferred_count() == 2);

    {
        auto panel = std::make_unique<FakePanel>();
        REQUIRE(FakePanel::if_built() == panel.get());
        REQUIRE(panel->ran == std::vector<std::string>{"primitive", "rescale"});   // latest wins, once
        REQUIRE(FakePanel::deferred_count() == 0);

        // Built: runs immediately, nothing queued.
        FakePanel::when_built("launch", [](FakePanel &p) { p.ran.push_back("now"); });
        REQUIRE(panel->ran.back() == "now");
        REQUIRE(FakePanel::deferred_count() == 0);
    }
    REQUIRE(FakePanel::if_built() == nullptr);
}

TEST_CASE("LazyInstance drops a pending launch on clear and on destroy", "[CadLazy][P4]")
{
    FakePanel::drop_all_deferred();
    FakePanel::when_built("launch", [](FakePanel &p) { p.ran.push_back("primitive"); });
    FakePanel::when_built("rescale", [](FakePanel &p) { p.ran.push_back("rescale"); });
    FakePanel::drop_deferred("launch");   // New Project before the Design tab was ever built
    REQUIRE(FakePanel::deferred_count() == 1);

    auto panel = std::make_unique<FakePanel>();
    REQUIRE(panel->ran == std::vector<std::string>{"rescale"});
    panel.reset();
    REQUIRE(FakePanel::deferred_count() == 0);
    FakePanel::drop_all_deferred();
}
