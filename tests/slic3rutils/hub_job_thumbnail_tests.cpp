#include <catch2/catch.hpp>

#include "slic3r/GUI/RemoteHub.hpp"

using namespace Slic3r::GUI::RemoteHub::Testing;

// The hub's /printers/<id>/thumbnail.png and the row's `thumbnail` URL. Synthetic printer ids only.

TEST_CASE("Job thumbnail: a Bambu subtask_name matches the archived .gcode.3mf name", "[HubJobThumbnail]")
{
    CHECK(same_job_name("Dragon-Egg_plate_2", "Dragon-Egg_plate_2.gcode.3mf"));
    CHECK(same_job_name("Dragon-Egg_plate_2.3mf", "Dragon-Egg_plate_2.gcode.3mf"));
    CHECK(same_job_name("cube.gcode", "cube.gcode"));
    CHECK(same_job_name(" Cube ", "cube.3mf"));
    CHECK_FALSE(same_job_name("", ""));
    CHECK_FALSE(same_job_name("cube", "cube_plate_2.gcode.3mf"));
}

TEST_CASE("Job thumbnail: only the running job's picture, never the previous job's", "[HubJobThumbnail]")
{
    const std::vector<ArchiveRecord> recs = {
        { "PRN-A", "r1", "benchy.gcode.3mf", "a_benchy.gcode.3mf", 100 },
        { "PRN-A", "r2", "cube.gcode.3mf", "a_cube.gcode.3mf", 200 },
        { "PRN-A", "r3", "benchy.gcode.3mf", "a_benchy.gcode.3mf", 300 }, // benchy sent again, later
        { "PRN-B", "r4", "egg_plate_2.gcode.3mf", "b_egg.gcode.3mf", 150 },
    };
    std::map<std::string, std::string> jobs = { { "PRN-A", "benchy" }, { "PRN-B", "egg_plate_2" } };
    std::map<std::string, std::string> got  = pick_job_thumbnails(recs, jobs);
    CHECK(got["PRN-A"] == "r3"); // the newest record of that job
    CHECK(got["PRN-B"] == "r4");

    // PRN-A now runs a job this PC never sent: no picture (it used to fall back to r3).
    jobs["PRN-A"] = "sent_from_the_phone";
    got           = pick_job_thumbnails(recs, jobs);
    CHECK(got.count("PRN-A") == 0);

    // Idle, or a printer that has not reported: no picture either.
    jobs["PRN-A"] = "";
    CHECK(pick_job_thumbnails(recs, jobs).count("PRN-A") == 0);
    CHECK(pick_job_thumbnails(recs, {}).empty());
}

TEST_CASE("Job thumbnail: the URL version changes per job and per picture", "[HubJobThumbnail]")
{
    const std::string a = thumbnail_version("benchy", "r1");
    CHECK(a.size() == 12);
    CHECK(a == thumbnail_version("benchy", "r1")); // stable
    CHECK(a != thumbnail_version("benchy", "r3")); // same name, new send
    CHECK(a != thumbnail_version("cube", "r1"));
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
}
