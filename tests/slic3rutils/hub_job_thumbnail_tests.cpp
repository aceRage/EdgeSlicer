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

// The owner's rows of 2026-10-26, as name shapes: the archive held the project's plates as sent to
// other printers, and the Bambu printers ran them under the names a Bambu send gives a job.
TEST_CASE("Job thumbnail: a Bambu send's sanitised project_plate name finds the plate", "[HubJobThumbnail]")
{
    CHECK(same_job_name("Dragon-Egg_120_(X1C)(1)_plate_2.3mf", "Dragon-Egg_120%(X1C)(1)_plate_2.gcode"));
    CHECK(same_job_name("Dragon_Egg_-_Seamless_+_Twist-Click", "Dragon_Egg_-_Seamless_+_Twist-Click.gcode.3mf"));
    CHECK(same_job_name("PolyLite_PETG", "PolyLite PETG"));

    ArchiveRecord plate2;
    plate2.title = "Dragon-Egg_120%(X1C)(1)";
    plate2.plate = 1; // 0-based: the project's plate 2
    CHECK(same_project_plate("Dragon-Egg_120_(X1C)(1)_plate_2.3mf", plate2));
    CHECK_FALSE(same_project_plate("Dragon-Egg_120_(X1C)(1)_plate_3.3mf", plate2));
    CHECK_FALSE(same_project_plate("Dragon-Egg_120_(X1C)(1).3mf", plate2)); // a one-plate name, not plate 2

    ArchiveRecord bottom;
    bottom.title      = "Dragon-Egg_120%(X1C)(1)";
    bottom.plate      = 0;
    bottom.plate_name = "Bottom";
    CHECK(same_project_plate("Dragon-Egg_120_(X1C)(1)_Bottom.3mf", bottom));
    CHECK(same_project_plate("Dragon-Egg_120_(X1C)(1)_plate_1.3mf", bottom));
    CHECK_FALSE(same_project_plate("Dragon-Egg_120_(X1C)(1)_Top.3mf", bottom));

    ArchiveRecord single;
    single.title = "Cube";
    single.plate = 0;
    CHECK(same_project_plate("Cube.3mf", single));

    // A different project that only shares a prefix is never matched.
    ArchiveRecord zephyra;
    zephyra.title = "Zephyra-Snake_Multicolor_v1.1(1)";
    zephyra.plate = 1;
    CHECK_FALSE(same_project_plate("Zephyra-Snake_Multicolor_PETG_plate_2.3mf", zephyra));
}

TEST_CASE("Job thumbnail: the matching order - this printer by name, by plate, then another printer", "[HubJobThumbnail]")
{
    auto rec = [](const char* printer, const char* id, const char* sent, long long t, const char* title, int plate,
                  const char* plate_name) {
        ArchiveRecord r;
        r.printer = printer; r.id = id; r.sent_name = sent; r.file = std::string("x_") + sent; r.time = t;
        r.title = title; r.plate = plate; r.plate_name = plate_name;
        return r;
    };
    const std::vector<ArchiveRecord> recs = {
        rec("H2S", "h2s-bottom", "Dragon_Egg_-_Seamless_+_Twist-Click.gcode.3mf", 100, "Dragon-Egg_120%(X1C)(1)", 0, "Bottom"),
        rec("CC-1", "cc-plate2-old", "ECC_0.4_Dragon-Egg_120%(X1C)(1)_PolyLite PETG _0.28_10h46m.gcode", 200, "Dragon-Egg_120%(X1C)(1)", 1, ""),
        rec("CC-2", "cc-plate2-new", "Dragon-Egg_120%(X1C)(1)_plate_2.gcode", 300, "Dragon-Egg_120%(X1C)(1)", 1, ""),
        rec("U1-A", "u1-snake", "Zephyra-Snake_Multicolor_v1.1(1)_PLA_17h38m.gcode", 400, "Zephyra-Snake_Multicolor_v1.1(1)", 0, ""),
        rec("X1C", "x1c-own", "Dragon-Egg_120%(X1C)(1)_plate_2.gcode.3mf", 50, "Dragon-Egg_120%(X1C)(1)", 1, ""),
    };
    const std::map<std::string, std::string> jobs = {
        { "X1C", "Dragon-Egg_120_(X1C)(1)_plate_2.3mf" },   // its own (older) send: step 1 beats newer ones elsewhere
        { "H2D", "Dragon-Egg_120_(X1C)(1)_Bottom.3mf" },     // only the H2S has the Bottom plate: step 3
        { "H2C", "Zephyra-Snake_Multicolor_PETG_plate_2.3mf" }, // never sliced under that project here: none
        { "H2S", "Dragon_Egg_-_Seamless_+_Twist-Click" },    // step 1
        { "U1-B", "Zephyra-Snake_Multicolor_v1.1(1)_PLA_17h38m.gcode" }, // the same file on a second U1: step 3
        { "P1S", "Dragon-Egg_120_(X1C)(1)_plate_2.3mf" },   // no own record: the newest plate-2 record elsewhere
    };
    const std::map<std::string, std::string> got = pick_job_thumbnails(recs, jobs);
    CHECK(got.at("X1C") == "x1c-own");
    CHECK(got.at("H2D") == "h2s-bottom");
    CHECK(got.count("H2C") == 0);
    CHECK(got.at("H2S") == "h2s-bottom");
    CHECK(got.at("U1-B") == "u1-snake");
    CHECK(got.at("P1S") == "cc-plate2-new");

    // Step 2 on the same printer beats step 3: the X1C's own record found by project and plate.
    std::vector<ArchiveRecord> own = recs;
    own.back().sent_name = "renamed.gcode.3mf";
    own.back().file      = "x_renamed.gcode.3mf";
    CHECK(pick_job_thumbnails(own, jobs).at("X1C") == "x1c-own");
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
