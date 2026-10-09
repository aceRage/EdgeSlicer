// The row model behind File > Export > Export Preset Bundle > "Process presets (.zip)".
//
// The dialog shows one table row per user process preset (name, printers, inherits, layer height,
// last modified) and exports exactly the rows that are ticked. Everything that does not need a
// window lives here so it can be unit tested: building the rows from the user's presets, the
// selection -> zip entries mapping, and the zip writer.
//
// The zip layout is the one the earlier "pick a printer" export produced, and the one
// PresetBundle::import_presets reads: a flat archive, one "<preset name>.json" entry per preset,
// holding the preset's own file.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

class Preset;
class PresetBundle;
class PresetCollection;

struct ProcessExportRow
{
    size_t                   id = 0;          // index in ProcessExportModel::rows
    std::string              name;            // preset name; the zip entry is name + ".json"
    std::string              file;            // the preset's json on disk
    std::string              inherits;        // system parent, empty for a preset with no parent
    double                   layer_height = 0.; // mm, 0 when the preset has none
    std::vector<std::string> printers;        // printer presets it is compatible with, sorted
    bool                     all_printers = false; // compatible with every printer in the model
    std::int64_t             mtime = 0;       // last write time of `file`, Unix seconds, 0 unknown
};

struct ProcessExportModel
{
    std::vector<ProcessExportRow> rows;       // by name, case-insensitive
    std::vector<std::string>      printers;   // every printer some row is listed under, sorted
};

// Printer preset name -> the user process presets compatible with it.
using ProcessPresetsByPrinter = std::map<std::string, std::vector<const Preset *>>;

// What the dialog collects when it opens: for each visible printer preset, select it on `bundle`
// and keep its compatible user process presets (not system, not the default, not embedded in a
// project). `bundle` should be a scratch copy: its printer and print selections change. The
// pointers refer into `bundle.prints` and stay valid as long as it does.
ProcessPresetsByPrinter collect_user_process_presets(PresetBundle &bundle);

// One row per distinct user process preset in `by_printer`.
//
// A preset is listed under the system printer presets it is compatible with (the same printers
// the earlier per-printer list showed: a printer that is a system preset and is its own base).
// A preset that is compatible with none of those, only with the user's own printer presets, is
// listed under those instead, so every user process preset that is compatible with some printer
// can be exported. A printer preset that cannot be found in `printers` is ignored.
ProcessExportModel build_process_export_model(const ProcessPresetsByPrinter &by_printer, const PresetCollection &printers);

// Convenience for tests and tools: collect_user_process_presets() + build_process_export_model().
ProcessExportModel build_process_export_model(PresetBundle &scratch_bundle);

// What the table page gets (resources/web/guide/export_process/export_process.js):
//   { "rows": [ { "id", "name", "printers": [..], "all", "inherits", "lh" (mm | null), "mtime" (Unix s | null) } ],
//     "printers": [..], "selected": [ids ticked so far] }
nlohmann::json process_export_rows_json(const ProcessExportModel &model, const std::vector<size_t> &selected);

struct PresetZipEntry
{
    std::string name;   // entry name inside the zip, e.g. "0.20mm Standard @U1 user.json"
    std::string path;   // the file to store
};

// The zip entries for the rows whose ids are in `selected`: one per preset, in row order, whatever
// order `selected` is in. Ids out of range and repeated ids are ignored. A preset whose file path
// is empty is skipped, and so is a second preset with the same entry name (the earlier export
// skipped repeated names the same way); `skipped`, when given, gets one line per skipped preset.
std::vector<PresetZipEntry> process_export_entries(const ProcessExportModel &model, const std::vector<size_t> &selected,
                                                   std::vector<std::string> *skipped = nullptr);

enum class PresetZipResult { Ok, InitFailed, AddFileFailed, FinalizeFailed };

// Writes `entries` into a new zip at zip_path_utf8 (the same miniz calls the export dialog's
// other modes use).
PresetZipResult write_presets_zip(const std::string &zip_path_utf8, const std::vector<PresetZipEntry> &entries);

} // namespace Slic3r
