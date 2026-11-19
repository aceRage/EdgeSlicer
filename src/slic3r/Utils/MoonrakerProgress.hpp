#ifndef slic3r_MoonrakerProgress_hpp_
#define slic3r_MoonrakerProgress_hpp_

#include <string>

#include <nlohmann/json.hpp>

// How far along a Moonraker (Klipper) printer is, and how long it has left, computed the way
// Snapmaker's own Device page computes it, so the hub's cards, the phone app, its push alerts and
// the Live Activity show the numbers the desktop shows.
//
// What the Device page does (resources/web/flutter_web, the device main panel; the U1's panel is
// that Flutter page fed by Moonraker through SSWCP):
//
//   it subscribes to virtual_sdcard { progress, file_position, is_active, file_size, file_path }
//   and print_stats { filename, state, total_duration, print_duration, filament_used, message,
//   info }, and asks server.files.metadata for the file in virtual_sdcard.file_path (the part after
//   "gcodes/"). Then, with pos = virtual_sdcard.file_position and the file's metadata:
//
//     pos > gcode_end_byte          -> 100 %
//     pos < gcode_start_byte        -> 0 %
//     otherwise                     percent = (pos - gcode_start_byte) * 100
//                                             / (gcode_end_byte - gcode_start_byte)
//                                   time left = round(estimated_time * (1 - percent / 100))
//     pos == 0 or no metadata       -> 0 % and no time
//
//   and the layer line is print_stats.info.current_layer / total_layer.
//
// So the percent is the share of the G-code *body* consumed (the thumbnails and the config block
// that bracket it do not count), and the time left is the slicer's estimate scaled by that share.
// display_status.progress, which the hub used before, is M73 or file_position / file_size over the
// whole file; with a big thumbnail header it runs ahead of the body share early in a print (the
// owner's U1: 8 % against the panel's 3 % at layer 11 of 322). The old time left was a linear
// extrapolation, print_duration / progress - print_duration, which counts heat-up, probing and the
// slow first layers as if the whole print went at that pace (4 h 16 m against the panel's 1 h 20 m).
//
// Order of preference used here:
//   percent    1. the Device page's body share (metadata has gcode_start_byte < gcode_end_byte and
//                 virtual_sdcard reported a file_position)
//              2. display_status.progress (M73 when the file sends it, else the file share)
//              3. virtual_sdcard.progress
//   time left  1. the slicer's estimated_time scaled by the percent above (what the panel does;
//                 Mainsail's and Fluidd's "slicer" estimate is the same idea). Klipper keeps no
//                 remaining time of its own - M73 R is not stored and display_status has only the
//                 progress - so there is no printer-side estimate to prefer over it.
//              2. linear extrapolation from print_duration, only once the print is at least
//                 MIN_LINEAR_FRACTION along: before that it overshoots by hours.
//              3. unknown, reported as 0 (every reader already treats 0 as "show no time").
//
// No network here: the caller fetches /printer/objects/query and /server/files/metadata and hands
// the answers in, which is what lets the Catch2 suite run on recorded JSON.

namespace Slic3r {
namespace MoonrakerProgress {

// The linear fallback is not trusted before this share of the print (5 %).
constexpr double MIN_LINEAR_FRACTION = 0.05;

// What server.files.metadata says about the file being printed. Absent fields stay negative.
struct FileMeta
{
    bool        valid { false };          // the answer was an object at all
    std::string filename;
    double      estimated_time { -1 };    // seconds, the slicer's estimate
    long long   gcode_start_byte { -1 };  // where the G-code body starts in the file
    long long   gcode_end_byte { -1 };    // and where it ends
    long long   size { -1 };              // file size in bytes
    double      modified { -1 };          // mtime, seconds since the epoch

    // The body range the Device page divides by.
    bool has_body_range() const { return gcode_start_byte >= 0 && gcode_end_byte > gcode_start_byte; }
};

// /server/files/metadata's answer ({"result": {...}}) or the bare result object.
FileMeta parse_metadata(const nlohmann::json& answer);

// The progress-related part of one /printer/objects/query answer.
struct Sample
{
    std::string state;                    // print_stats.state
    std::string filename;                 // print_stats.filename (relative to gcodes/)
    std::string file_path;                // virtual_sdcard.file_path (absolute on the printer)
    double      print_duration { 0 };     // print_stats.print_duration, seconds
    double      display_progress { -1 };  // display_status.progress, 0..1 (-1: not reported)
    double      sd_progress { -1 };       // virtual_sdcard.progress, 0..1 (-1: not reported)
    long long   file_position { -1 };     // virtual_sdcard.file_position (-1: not reported)
    long long   file_size { -1 };         // virtual_sdcard.file_size (-1: not reported)

    // The file to ask server.files.metadata about: print_stats.filename, else what follows
    // "gcodes/" in virtual_sdcard.file_path (the way the Device page derives it).
    std::string metadata_filename() const;
};

// The status object of a /printer/objects/query answer (result.status), or the whole answer.
Sample parse_sample(const nlohmann::json& status_or_answer);

// Which source a figure came from (for the log and the tests).
enum class Basis
{
    None,
    BodyBytes,        // percent: (file_position - gcode_start_byte) / body length
    DisplayStatus,    // percent: display_status.progress
    SdCard,           // percent: virtual_sdcard.progress
    SlicerEstimate,   // time left: estimated_time * (1 - fraction)
    Linear,           // time left: print_duration / fraction - print_duration
};
const char* basis_name(Basis b);

struct Estimate
{
    double fraction { 0 };          // 0..1
    int    percent { 0 };           // round(fraction * 100), what the Device page prints
    int    left_s { 0 };            // seconds left, 0 = unknown
    Basis  percent_basis { Basis::None };
    Basis  time_basis { Basis::None };
};

// The numbers for one sample. `meta` must describe the file the sample is printing (the caller
// checks the name); pass a default FileMeta when there is none.
Estimate estimate(const Sample& sample, const FileMeta& meta);

} // namespace MoonrakerProgress
} // namespace Slic3r

#endif // slic3r_MoonrakerProgress_hpp_
