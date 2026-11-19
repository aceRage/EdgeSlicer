// The "Flushing volumes" dialog (WipeTowerDialog.cpp). The project stores the matrix from -> to
// (matrix[n * from + to]); each box shows the stored volume times the multiplier, cut to a whole number.
// Before the fix:
//  - OK divided every box back by the multiplier, so a box nobody touched saved its cut-down value:
//    418 at 0.3 shows 125 and was saved as 416.67, and each OK lowered the purge again;
//  - with the multiplier at 0, OK saved a matrix of zeros (and a box edited at 0 stored inf/NaN), so
//    raising the multiplier again did not bring the volumes back;
//  - a box was checked against the least flush of the filament flushed in, where Re-calculate and
//    get_min_flush_volumes use the filament flushed out.
//
// GUI_App.hpp first: Windows headers that the GUI headers' boost::asio path needs.
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/WipeTowerDialog.hpp"

#include <catch2/catch.hpp>

#include <cmath>

using namespace Slic3r::GUI::FlushVolumes;

TEST_CASE("Flushing volumes: OK with nothing edited saves what was there", "[FlushingVolumesDialog]")
{
    const float multiplier = 0.3f;
    // Two filaments, from -> to; the diagonal is 0.
    std::vector<float> stored = {0.f, 418.f, 307.f, 0.f};
    std::vector<int>   shown;
    for (float volume : stored)
        shown.push_back(shown_volume(volume, multiplier));
    REQUIRE(shown == std::vector<int>{0, 125, 92, 0});

    // Confirm the dialog five times in a row: nothing may creep.
    for (int round = 0; round < 5; ++round) {
        std::vector<float> saved = read_back_matrix(stored, shown, multiplier);
        CHECK(saved == stored);
        stored = saved;
    }
    CHECK(stored[1] == 418.f);
}

TEST_CASE("Flushing volumes: an edited box saves what was typed and shows it again", "[FlushingVolumesDialog]")
{
    const float multiplier = 0.3f;
    std::vector<float> stored = {0.f, 418.f, 307.f, 0.f};
    std::vector<int>   shown  = {0, 150, 92, 0}; // 125 overwritten with 150
    std::vector<float> saved  = read_back_matrix(stored, shown, multiplier);
    CHECK_THAT(saved[1], Catch::Matchers::WithinAbs(500., 0.01));
    CHECK(saved[2] == 307.f);
    CHECK(shown_volume(saved[1], multiplier) == 150);

    // Typed values come back as typed when the dialog opens again (15 / 0.3 * 0.3 is 14.99999 in float).
    for (float m : {0.3f, 0.7f, 1.1f, 0.15f, 2.9f}) {
        for (int typed = 1; typed <= 20000; typed += 7) {
            const float s = stored_volume(0.f, typed, m);
            INFO("multiplier " << m << ", typed " << typed);
            REQUIRE(shown_volume(s, m) == typed);
        }
    }
}

TEST_CASE("Flushing volumes: a multiplier of 0 keeps the stored volumes", "[FlushingVolumesDialog]")
{
    std::vector<float> stored = {0.f, 418.f, 307.f, 0.f};
    std::vector<int>   shown;
    for (float volume : stored)
        shown.push_back(shown_volume(volume, 0.f));
    REQUIRE(shown == std::vector<int>{0, 0, 0, 0});

    std::vector<float> saved = read_back_matrix(stored, shown, 0.f);
    CHECK(saved == stored);
    // Raising the multiplier again brings the volumes back.
    CHECK(shown_volume(saved[1], 1.f) == 418);

    // A box typed into at 0 cannot be stored (it would be 50 / 0): the stored volume stays, finite.
    const float edited = stored_volume(418.f, 50, 0.f);
    CHECK(std::isfinite(edited));
    CHECK(edited == 418.f);
}

TEST_CASE("Flushing volumes: a box is checked against the least flush of the filament flushed out", "[FlushingVolumesDialog]")
{
    // Filament 0 needs at least 100 mm3 flushed out of it; filament 1 needs none.
    const std::vector<int> least = {100, 0};
    const int              most  = 20000;
    // Flushing filament 0 out for filament 1 with 50: below filament 0's least.
    CHECK(volume_out_of_range(50, /*from*/ 0, /*to*/ 1, least, most));
    // Flushing filament 1 out for filament 0 with 50: filament 1 has no least.
    CHECK_FALSE(volume_out_of_range(50, /*from*/ 1, /*to*/ 0, least, most));
    CHECK(volume_out_of_range(most + 1, 1, 0, least, most));
}
