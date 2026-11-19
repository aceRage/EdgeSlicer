#ifndef slic3r_PresetSyncPolicy_hpp_
#define slic3r_PresetSyncPolicy_hpp_

// Whether user presets (Printer / Filament / Process) are synchronised with a cloud account.
//
// Owner decision (2026-10-09): they are not. User presets stay on this machine, in user/default/...,
// whoever is signed in. Nothing may ask a network plug-in for a setting id, upload, download or delete
// a preset, and a cloud list must never remove or replace a local preset.
//
// Why: the cloud sync only ever worked against Bambu Lab's own plug-in. With EdgeSlicer's UltraNet
// plug-in its request_setting_id answers "HTTP 200, empty id", which the sync thread read as "try
// again", so the same ~590 presets were requested over and over (about 5,600 calls per log file) and
// never marked as synced. The sync is switched off at one place instead of being made to cope with a
// cloud that has nothing to offer.
//
// This one switch gates every entry point in GUI_App (start_sync_user_preset, sync_preset,
// reload_settings, delete_preset_from_cloud) and hides the Preferences / dialog entries that turned it on.
// The code behind it is left in place so a future, deliberate cloud-sync feature starts from one
// switch; flipping it is a source change on purpose, not a setting. Kept free of wx so a test can pin it
// (tests/slic3rutils/preset_sync_policy_tests.cpp).
namespace Slic3r {
namespace PresetSync {

constexpr bool cloud_sync_enabled() { return false; }

} // namespace PresetSync
} // namespace Slic3r

#endif // slic3r_PresetSyncPolicy_hpp_
