// Percent and time left for a Moonraker printer (MoonrakerProgress), on the basis Snapmaker's own
// Device page uses, against recorded-shape Moonraker answers in tests/data/moonraker_progress.
//
// Background (2026-10-11): on one U1 print at one moment the desktop Device page showed 3 %,
// 1 h 20 m left, layer 11/322, while the hub (phone app, Live Activity, Lock Screen) showed 8 %,
// 4 h 16 m left. The hub took display_status.progress (the share of the whole file, thumbnails
// included) and extrapolated linearly from print_duration; the Device page divides the G-code
// body by file_position and scales the slicer's estimated_time. The fixtures reproduce that moment.

#include <catch2/catch.hpp>

#include "slic3r/Utils/MoonrakerProgress.hpp"

#include <boost/nowide/fstream.hpp>

#include <sstream>
#include <string>

using namespace Slic3r::MoonrakerProgress;
using nlohmann::json;

namespace {

json fixture(const std::string& name)
{
    const std::string          path = std::string(TEST_DATA_DIR) + "/moonraker_progress/" + name;
    boost::nowide::ifstream    in(path);
    INFO("fixture " << path);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

// The Device page's formula, written out from the shipped Flutter bundle, for the comparison.
double device_page_percent(long long pos, long long start, long long end)
{
    if (pos > end) return 100.0;
    if (pos < start) return 0.0;
    return double(pos - start) * 100.0 / double(end - start);
}

} // namespace

TEST_CASE("metadata: the U1's answer for a sliced file", "[MoonrakerProgress]")
{
    const FileMeta m = parse_metadata(fixture("u1_metadata.json"));
    REQUIRE(m.valid);
    CHECK(m.filename == "plate_1.gcode");
    CHECK(m.estimated_time == 4948);
    CHECK(m.gcode_start_byte == 330500);
    CHECK(m.gcode_end_byte == 6330500);
    CHECK(m.size == 6380500);
    CHECK(m.has_body_range());
}

TEST_CASE("metadata: a file without the slicer's numbers, and a 404", "[MoonrakerProgress]")
{
    const FileMeta bare = parse_metadata(fixture("metadata_no_body.json"));
    CHECK(bare.valid);
    CHECK(bare.estimated_time < 0);
    CHECK_FALSE(bare.has_body_range());
    CHECK(bare.size == 120345);

    const FileMeta missing = parse_metadata(fixture("metadata_404.json"));
    CHECK_FALSE(missing.valid);
    CHECK_FALSE(parse_metadata(json()).valid);
    CHECK_FALSE(parse_metadata(json::array()).valid);
}

TEST_CASE("sample: the fields of a U1 status query", "[MoonrakerProgress]")
{
    const Sample s = parse_sample(fixture("u1_query_early.json"));
    CHECK(s.state == "printing");
    CHECK(s.filename == "plate_1.gcode");
    CHECK(s.file_position == 510500);
    CHECK(s.file_size == 6380500);
    CHECK_THAT(s.display_progress, Catch::Matchers::WithinAbs(0.08, 1e-9));
    CHECK_THAT(s.sd_progress, Catch::Matchers::WithinAbs(0.08, 1e-9));
    CHECK_THAT(s.print_duration, Catch::Matchers::WithinAbs(1335.7, 1e-9));
    CHECK(s.metadata_filename() == "plate_1.gcode");

    // The bare status object (what SnapmakerLan's probe already holds) reads the same.
    const Sample t = parse_sample(fixture("u1_query_early.json")["result"]["status"]);
    CHECK(t.file_position == s.file_position);
    CHECK(t.state == s.state);
}

TEST_CASE("sample: the metadata name comes from file_path when print_stats has none", "[MoonrakerProgress]")
{
    Sample s;
    s.file_path = "/home/pi/printer_data/gcodes/sub dir/plate_2.gcode";
    CHECK(s.metadata_filename() == "sub dir/plate_2.gcode");
    s.filename = "plate_3.gcode";
    CHECK(s.metadata_filename() == "plate_3.gcode");
    Sample none;
    CHECK(none.metadata_filename().empty());
}

TEST_CASE("the owner's report: 3 % and 1 h 20 m, as the Device page showed", "[MoonrakerProgress]")
{
    const Sample   s = parse_sample(fixture("u1_query_early.json"));
    const FileMeta m = parse_metadata(fixture("u1_metadata.json"));
    const Estimate e = estimate(s, m);

    CHECK(e.percent_basis == Basis::BodyBytes);
    CHECK(e.time_basis == Basis::SlicerEstimate);
    CHECK(e.percent == 3);
    CHECK_THAT(e.fraction * 100, Catch::Matchers::WithinAbs(device_page_percent(510500, 330500, 6330500), 1e-9));
    // 4948 s * 0.97 = 4799.56 -> 4800 s = 1 h 20 m.
    CHECK(e.left_s == 4800);

    // What the hub said before: the file share and a linear extrapolation.
    const double old_fraction = s.display_progress;
    const int    old_left     = (int) (s.print_duration / old_fraction - s.print_duration);
    CHECK((int) (old_fraction * 100 + 0.5) == 8);
    CHECK(old_left / 60 == 256); // 4 h 16 m
}

TEST_CASE("mid-print: half the body, half the estimate", "[MoonrakerProgress]")
{
    const Estimate e = estimate(parse_sample(fixture("u1_query_mid.json")), parse_metadata(fixture("u1_metadata.json")));
    CHECK(e.percent == 50);
    CHECK(e.left_s == 2474); // 4948 / 2
    CHECK(e.percent_basis == Basis::BodyBytes);
}

TEST_CASE("before the body and past it, as the Device page clamps", "[MoonrakerProgress]")
{
    const FileMeta m = parse_metadata(fixture("u1_metadata.json"));
    Sample         s = parse_sample(fixture("u1_query_early.json"));

    SECTION("still reading the thumbnails: 0 %, the whole estimate left")
    {
        s.file_position  = 1200;
        const Estimate e = estimate(s, m);
        CHECK(e.percent == 0);
        CHECK(e.left_s == 4948);
    }
    SECTION("in the config block after the last move: 100 %, nothing left")
    {
        s.file_position  = 6350000;
        const Estimate e = estimate(s, m);
        CHECK(e.percent == 100);
        CHECK(e.left_s == 0);
    }
}

TEST_CASE("a finished or idle printer has no time left", "[MoonrakerProgress]")
{
    const FileMeta m = parse_metadata(fixture("u1_metadata.json"));

    const Estimate done = estimate(parse_sample(fixture("u1_query_complete.json")), m);
    CHECK(done.percent == 100);
    CHECK(done.left_s == 0);

    const Estimate idle = estimate(parse_sample(fixture("u1_query_standby.json")), FileMeta());
    CHECK(idle.percent == 0);
    CHECK(idle.left_s == 0);
}

TEST_CASE("paused keeps its time left", "[MoonrakerProgress]")
{
    Sample s = parse_sample(fixture("u1_query_mid.json"));
    s.state  = "paused";
    CHECK(estimate(s, parse_metadata(fixture("u1_metadata.json"))).left_s == 2474);
}

TEST_CASE("without the body range: display_status for percent, the estimate still scales", "[MoonrakerProgress]")
{
    FileMeta m          = parse_metadata(fixture("u1_metadata.json"));
    m.gcode_start_byte  = -1;
    m.gcode_end_byte    = -1;
    const Estimate e    = estimate(parse_sample(fixture("u1_query_mid.json")), m);
    CHECK(e.percent_basis == Basis::DisplayStatus);
    CHECK(e.percent == 52);
    CHECK(e.time_basis == Basis::SlicerEstimate);
    CHECK(e.left_s == 2365); // 4948 * (1 - 0.522)
}

TEST_CASE("no metadata at all: linear only past the first few percent", "[MoonrakerProgress]")
{
    SECTION("2 % in (a generic Klipper host): no time rather than an hours-long guess")
    {
        const Sample   s = parse_sample(fixture("klipper_query_no_sdcard.json"));
        const Estimate e = estimate(s, parse_metadata(fixture("metadata_404.json")));
        CHECK(e.percent_basis == Basis::DisplayStatus);
        CHECK(e.percent == 2);
        CHECK(e.left_s == 0);
        CHECK(e.time_basis == Basis::None);
    }
    SECTION("past the threshold the extrapolation is allowed")
    {
        Sample s           = parse_sample(fixture("klipper_query_no_sdcard.json"));
        s.display_progress = 0.25;
        s.print_duration   = 600;
        const Estimate e   = estimate(s, parse_metadata(fixture("metadata_no_body.json")));
        CHECK(e.percent == 25);
        CHECK(e.time_basis == Basis::Linear);
        CHECK(e.left_s == 1800);
    }
    SECTION("the threshold itself")
    {
        Sample s           = parse_sample(fixture("klipper_query_no_sdcard.json"));
        s.display_progress = MIN_LINEAR_FRACTION - 0.001;
        CHECK(estimate(s, FileMeta()).left_s == 0);
        s.display_progress = MIN_LINEAR_FRACTION;
        CHECK(estimate(s, FileMeta()).left_s > 0);
    }
}

TEST_CASE("virtual_sdcard.progress is the last resort for percent", "[MoonrakerProgress]")
{
    Sample s;
    s.state       = "printing";
    s.sd_progress = 0.4;
    const Estimate e = estimate(s, FileMeta());
    CHECK(e.percent_basis == Basis::SdCard);
    CHECK(e.percent == 40);
}
