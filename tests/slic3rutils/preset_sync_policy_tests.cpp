#include <catch2/catch.hpp>

#include "slic3r/Utils/PresetSyncPolicy.hpp"

// User presets are never synchronised with a cloud account (owner decision 2026-10-09). The switch is
// a compile-time constant that GUI_App::start_sync_user_preset / sync_preset / reload_settings /
// delete_preset_from_cloud and the Preferences / preset dialogs consult. This pins it, so turning the
// sync back on is a deliberate edit that has to change this test too, and a reviewer sees it.

static_assert(!Slic3r::PresetSync::cloud_sync_enabled(), "user preset cloud sync must stay off");

TEST_CASE("Preset cloud sync is off", "[PresetSync]")
{
    CHECK_FALSE(Slic3r::PresetSync::cloud_sync_enabled());
}
