#pragma once

namespace Slic3r { namespace GUI {

// EDGESLICER_TEST_AMS_PREVIEW=<folder> renders the two-extruder filament area (light and dark)
// and the AMS Dryness Control dialog for a made-up H2D, fed through the real MQTT parser, into
// PNG files in that folder, then ends the process.
//
// A test aid for checking the layout without a printer. It runs during start-up before the main
// window, the hub or the phone listener exist, from windows placed off-screen and never
// activated, and the made-up printer has no network agent, so nothing can be sent or contacted.
// Nothing sets the variable but a tester; without it this returns false and does nothing.
bool run_ams_ui_preview_if_asked();

}} // namespace Slic3r::GUI
