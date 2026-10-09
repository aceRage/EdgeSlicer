// The row model behind File > Export > Export Preset Bundle > "Process presets (.zip)" (and, with
// FilamentPresetExport.hpp, "Filament presets (.zip)").
//
// The dialog shows one table row per user preset (name, printers, layer height or vendor and
// material, last modified) and exports exactly the rows that are ticked. Everything that does not
// need a window lives here so it can be unit tested: building the rows from the user's presets,
// the selection -> zip entries mapping, and the zip writer.
//
// The zip layout is the one the earlier "pick a printer" export produced, and the one
// PresetBundle::import_presets reads: a flat archive, one "<preset name>.json" entry per preset,
// holding the preset's own file.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

class Preset;
class PresetBundle;
class PresetCollection;

struct PresetExportRow
{
    size_t                   id = 0;          // index in PresetExportModel::rows
    std::string              name;            // preset name; the zip entry is name + ".json"
    std::string              file;            // the preset's json on disk
    std::string              inherits;        // system parent, empty for a preset with no parent
    double                   layer_height = 0.; // process presets: mm, 0 when the preset has none
    std::vector<std::string> printers;        // printer presets it is compatible with, sorted
    bool                     all_printers = false; // compatible with every printer in the model
    std::int64_t             mtime = 0;       // last write time of `file`, Unix seconds, 0 unknown
    std::string              vendor;          // filament presets: filament_vendor
    std::string              material;        // filament presets: filament_type
    std::string              group;           // filament presets: the name the earlier export grouped them by
};

struct PresetExportModel
{
    std::vector<PresetExportRow> rows;        // by name, case-insensitive
    std::vector<std::string>     printers;    // every printer some row is listed under, sorted
    std::vector<std::string>     vendors;     // filament presets: the distinct vendors, sorted
    std::vector<std::string>     materials;   // filament presets: the distinct materials, sorted
};

using ProcessExportRow   = PresetExportRow;
using ProcessExportModel = PresetExportModel;

// Printer preset name -> the user presets (of one kind) compatible with it.
using UserPresetsByPrinter    = std::map<std::string, std::vector<const Preset *>>;
using ProcessPresetsByPrinter = UserPresetsByPrinter;

// Which printers each preset is listed under. A preset is listed under the system printer presets
// it is compatible with (the same printers the earlier per-printer list showed: a printer that is a
// system preset and is its own base); one compatible with none of those, only with the user's own
// printer presets, is listed under those instead. A printer preset that cannot be found in
// `printers` is ignored.
class ExportPrinterIndex
{
public:
    ExportPrinterIndex(const UserPresetsByPrinter &by_printer, const PresetCollection &printers);
    // Fills row.printers and row.all_printers for the preset of that name (both empty when it is
    // listed under no printer).
    void fill(const std::string &preset_name, PresetExportRow &row) const;

private:
    struct Listed
    {
        std::set<std::string> system_printers, user_printers;
    };
    std::map<std::string, Listed> m_by_preset;
    size_t                        m_system_printers = 0;
};

// Last write time of a file, Unix seconds; 0 when unknown.
std::int64_t export_file_mtime(const std::string &path);

// Sorts the rows by name (case-insensitive), numbers them, and collects the distinct printers,
// vendors and materials. Used by the model builders.
void finish_export_model(PresetExportModel &model);

// What the dialog collects when it opens: for each visible printer preset, select it on `bundle`
// and keep its compatible user process presets (not system, not the default, not embedded in a
// project). `bundle` should be a scratch copy: its printer and print selections change. The
// pointers refer into `bundle.prints` and stay valid as long as it does.
UserPresetsByPrinter collect_user_process_presets(PresetBundle &bundle);

// One row per distinct user process preset in `by_printer`, listed as ExportPrinterIndex says, so
// every user process preset that is compatible with some printer can be exported.
PresetExportModel build_process_export_model(const UserPresetsByPrinter &by_printer, const PresetCollection &printers);

// Convenience for tests and tools: collect_user_process_presets() + build_process_export_model().
PresetExportModel build_process_export_model(PresetBundle &scratch_bundle);

// What the table page gets (resources/web/guide/export_presets/export_presets.js):
//   { "rows": [ { "id", "name", "printers": [..], "all", "inherits", "lh" (mm | null), "mtime" (Unix s | null),
//                 "vendor", "material" } ],
//     "printers": [..], "vendors": [..], "materials": [..], "selected": [ids ticked so far] }
nlohmann::json export_rows_json(const PresetExportModel &model, const std::vector<size_t> &selected);

struct PresetZipEntry
{
    std::string name;   // entry name inside the zip, e.g. "0.20mm Standard @U1 user.json"
    std::string path;   // the file to store
};

// The zip entries for the rows whose ids are in `selected`: one per preset, in row order, whatever
// order `selected` is in. Ids out of range and repeated ids are ignored. A preset whose file path
// is empty is skipped, and so is a second preset with the same entry name (the earlier export
// skipped repeated names the same way); `skipped`, when given, gets one line per skipped preset.
std::vector<PresetZipEntry> preset_export_entries(const PresetExportModel &model, const std::vector<size_t> &selected,
                                                  std::vector<std::string> *skipped = nullptr);

enum class PresetZipResult { Ok, InitFailed, AddFileFailed, FinalizeFailed };

// Writes `entries` into a new zip at zip_path_utf8 (the same miniz calls the export dialog's
// other modes use).
PresetZipResult write_presets_zip(const std::string &zip_path_utf8, const std::vector<PresetZipEntry> &entries);

} // namespace Slic3r
