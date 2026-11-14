#pragma once
#ifndef slic3r_CliOutput_hpp_
#define slic3r_CliOutput_hpp_

// Pure helpers of the command line (src/Snapmaker_Orca.cpp): output file names, the exit code table
// and the usage numbers that result.json carries. Kept out of the application's main file so Catch2
// can test them.

#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace Slic3r {

struct GCodeProcessorResult;

// ---- Output paths --------------------------------------------------------------------------------

// Where a file called `name` goes when `--outputdir` is `outputdir`. An empty outputdir, or a name that
// is already a rooted or absolute path (C:/x/y.3mf, /tmp/y.3mf, \\server\share\y.3mf), is used as given;
// anything else is placed under outputdir.
std::string cli_join_output_path(const std::string &outputdir, const std::string &name);

// The G-code file name for plate `plate_number` (1-based) of `--export-gcode <name>`.
//  * A name that contains the token {plate} gets the plate number in its place (every occurrence).
//  * Without the token and with more than one plate written, "_<plate>" is put before the extension
//    (out.gcode -> out_2.gcode; no extension: it is appended).
//  * One plate: the name is used as given.
std::string cli_gcode_file_name(const std::string &name, int plate_number, int plates_written);

// The file name of object `index` (1-based) of `--export-stl` / `--export-stls` and the other per-object
// exports: "obj_<index>_<name>", with `ext` (".stl") added unless the name already ends in it
// (ignoring case). `name` is the object's name, or its input file when it has none.
std::string cli_object_file_name(const std::string &name, unsigned int index, const std::string &ext);

// ---- Exit codes ----------------------------------------------------------------------------------

struct CliExitCode
{
    int         code;
    const char *name; // the CLI_* macro of libslic3r/Utils.hpp
};

// Every exit code the CLI can return, from 0 down to the most negative, one entry each.
const std::vector<CliExitCode> &cli_exit_code_names();
// "CLI_INVALID_PARAMS" for -2; empty for a code the table does not know.
std::string cli_exit_code_name(int code);
// {"exit_codes": [{"code": 0, "name": "CLI_SUCCESS", "message": "Success."}, ...]} in table order.
// `messages` is the CLI's code -> text map; a code without a text gets an empty message.
nlohmann::json cli_exit_codes_json(const std::map<int, std::string> &messages);
// The same as an aligned table for reading in a console.
std::string cli_exit_codes_text(const std::map<int, std::string> &messages);

// ---- result.json usage numbers -------------------------------------------------------------------

// What the CLI knows about one filament slot of the plate besides the G-code statistics.
struct CliFilamentSlot
{
    std::string preset; // filament preset name
    std::string type;   // PLA, PETG ...
    std::string colour; // #RRGGBB
    int         extruder{1}; // physical extruder it prints on, 1-based
};

// The usage part of one result.json plate entry, from the processed G-code (all new keys; the keys of
// the original entry are not touched):
//   "layers"    - layer count
//   "filaments" - one entry per filament slot that extruded: index (0-based), preset, type, colour,
//                 extruder, grams, mm, mm3, cost, price_per_kg, model_g, support_g, flush_g, tower_g
//   "extruders" - the same summed per physical extruder: index (1-based), filaments, grams, mm, mm3, cost
//   "cost"      - prices_known, machine_rate_known, machine_rate_per_h, material, machine, total,
//                 display_total (rounded to cents, as the cost panel shows it)
nlohmann::json cli_plate_usage_json(const GCodeProcessorResult &result, const std::vector<CliFilamentSlot> &slots);

// The totals over all plates of a result.json "sliced_plates" array: time_s, layers, filament_g, filament_mm,
// filament_mm3, cost (the plates' display_total added) and plates (how many were counted).
nlohmann::json cli_result_totals(const nlohmann::json &sliced_plates);

} // namespace Slic3r

#endif // slic3r_CliOutput_hpp_
