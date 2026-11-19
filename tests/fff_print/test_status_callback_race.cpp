#include <catch2/catch.hpp>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SlicingStatusCollector.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp" // get access to init_print, etc

using namespace Slic3r;
using namespace Slic3r::Test;

// Regression test for the CLI's heap-corrupting status-callback race.
//
// Print::process() runs PrintObject::generate_support_material() from a tbb::parallel_for over the
// objects, and each object that "needs support" raises its warning through the Print status callback
// on its own worker thread. The CLI's callbacks used to push_back into a plain global vector, which
// crashed (0xC0000005) or deadlocked on the heap lock in about 1 of 13 slices of a 4-object model.
// The CLI now collects into SlicingStatusCollector; this test drives that same class the way the CLI
// callbacks do, with every object raising a warning at once, many times over.

namespace {

// A 40x40mm cap on an 8x8mm stem: the cap reaches ~22mm past the stem, beyond the 6mm cantilever limit
// of PrintObject::is_support_necessary(), so slicing it with support off raises SlicingNeedSupportOn.
TriangleMesh overhang_mesh()
{
    TriangleMesh model = make_cube(8, 8, 13);
    model.translate(16., 16., 0.);
    TriangleMesh cap = make_cube(40, 40, 2);
    cap.translate(0., 0., 12.);
    model.merge(cap);
    return model;
}

// Stands in for a mutex that does nothing: the pre-fix behaviour of the CLI's callbacks.
struct NoLock
{
    void lock() {}
    void unlock() {}
};

// Slices `objects` copies of the overhang model with support off and collects every status that
// carries a step warning, exactly as the CLI callbacks do. Returns the number of SlicingNeedSupportOn
// warnings collected (expected: one per object).
template<class Collector>
size_t slice_and_count_need_support(size_t objects, size_t &total_statuses)
{
    Slic3r::Print print;
    Slic3r::Model model;
    std::vector<TriangleMesh> meshes(objects, overhang_mesh());
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",             "0" },
        { "enforce_support_layers",     "0" },
        { "layer_height",               "0.3" },
        { "initial_layer_print_height", "0.3" }
    });
    Slic3r::Test::init_print(std::move(meshes), print, model, config);

    Collector collector;
    print.set_status_callback([&collector](const PrintBase::SlicingStatus &status) {
        if (status.warning_step != -1)
            collector.add(status);
    });
    print.process();

    const std::vector<PrintBase::SlicingStatus> statuses = collector.take();
    total_statuses = statuses.size();
    size_t need_support = 0;
    for (const PrintBase::SlicingStatus &status : statuses)
        if (status.message_type == PrintStateBase::SlicingNeedSupportOn)
            ++need_support;
    return need_support;
}

} // namespace

TEST_CASE("Status callbacks raised in parallel by every object are collected without loss or corruption", "[Print][StatusCallback][Threading]")
{
    constexpr size_t objects     = 8;
    constexpr size_t repetitions = 25;
    for (size_t rep = 0; rep < repetitions; ++rep) {
        size_t total = 0;
        const size_t need_support = slice_and_count_need_support<SlicingStatusCollector>(objects, total);
        INFO("repetition " << rep);
        // One "please enable support" warning per object, none lost, none duplicated.
        REQUIRE(need_support == objects);
        REQUIRE(total >= objects);
    }
}

TEST_CASE("SlicingStatusCollector take() and restore_front() keep arrival order", "[Print][StatusCallback]")
{
    SlicingStatusCollector collector;
    CHECK(collector.empty());
    collector.add(PrintBase::SlicingStatus(1, "a"));
    collector.add(PrintBase::SlicingStatus(2, "b"));
    std::vector<PrintBase::SlicingStatus> first = collector.take();
    REQUIRE(first.size() == 2);
    CHECK(collector.empty());
    collector.add(PrintBase::SlicingStatus(3, "c"));
    collector.restore_front(std::move(first));
    const std::vector<PrintBase::SlicingStatus> all = collector.take();
    REQUIRE(all.size() == 3);
    CHECK(all[0].text == "a");
    CHECK(all[1].text == "b");
    CHECK(all[2].text == "c");
}

// Not part of the normal run: shows what the CLI's old callbacks did. The lock-free collector corrupts
// the heap, so this crashes or hangs some of the time; it only runs when asked for explicitly with
//   EDGESLICER_STATUS_RACE_DEMO=<repetitions> fff_print_tests "[StatusCallbackRaceDemo]"
// and does nothing otherwise, so test discovery and CI never trip over it.
TEST_CASE("Lock-free status collector demo (heap corruption expected)", "[.][StatusCallbackRaceDemo]")
{
    const char *env = std::getenv("EDGESLICER_STATUS_RACE_DEMO");
    if (env == nullptr || *env == '\0')
        return;
    const size_t repetitions = size_t(std::max(1, std::atoi(env)));
    for (size_t rep = 0; rep < repetitions; ++rep) {
        size_t total = 0;
        slice_and_count_need_support<BasicSlicingStatusCollector<NoLock>>(8, total);
    }
    SUCCEED("survived (the race is probabilistic)");
}
