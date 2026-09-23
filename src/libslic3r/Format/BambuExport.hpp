#ifndef slic3r_Format_BambuExport_hpp_
#define slic3r_Format_BambuExport_hpp_

// "Export Bambu 3MF": rewrite this fork's configs into the shape Bambu Studio expects.
//
// Bambu Studio only loads a 3MF's project settings and embedded presets when the Application
// tag starts with "BambuStudio-" (bbs_3mf.cpp, _handle_end_metadata), and then drops every key
// its own print_config_def does not declare and rejects values that do not fit its option types.
// This module turns one of our configs into a Bambu-shaped key/value map:
//   * keys Bambu does not know are left out (the list is generated from Bambu Studio's
//     PrintConfig.cpp by scripts/gen_bambu_known_keys.py into BambuKnownKeys.cpp),
//   * keys Bambu spells differently are renamed (infill_anchor -> sparse_infill_anchor, ...),
//   * values are reshaped to Bambu's types (scalar <-> vector, float -> int, "mm or %" -> mm),
//   * enum values are translated (our "rectilinear" is Bambu's "zig-zag") or, when Bambu has
//     no equivalent, the key is left out so Bambu keeps its own default,
//   * per-extruder-variant vectors are laid out the way Bambu indexes them
//     (print/printer/filament_extruder_variant + *_id / filament_self_index).
// Nothing here invents a value: a key that cannot be represented faithfully is dropped and
// reported, never filled with a guess (in particular never with 0 for a speed).
//
// See docs/bambu-3mf-export.md.

#include <map>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;

namespace BambuExport {

// How Bambu sizes a vector option (DynamicPrintConfig::get_parameter_size in Bambu Studio).
enum class VariantClass {
    None,        // one slot per extruder / filament, or not per-anything
    Print,       // print_options_with_variant: one slot per print_extruder_variant entry
    Filament,    // filament_options_with_variant: one slot per filament_extruder_variant entry
    Printer,     // printer_options_with_variant_1: one slot per printer_extruder_variant entry
    PrinterPair, // printer_options_with_variant_2: (normal, silent) per printer_extruder_variant entry
    Extruder,    // printer_extruder_options: one slot per extruder (len(nozzle_diameter))
};

struct BambuKeyDef
{
    const char  *key;
    const char  *type;        // Bambu's ConfigOptionType name, e.g. "coFloats"
    bool         nullable;
    VariantClass variant;
    const char  *enum_values; // accepted enum strings joined by '|', nullptr if not an enum
};

struct EnumTranslation
{
    const char *key;
    const char *ours;
    const char *bambu; // "" = Bambu has no such value
};

// Generated (BambuKnownKeys.cpp).
const char                         *bambu_reference_version();
const char                         *bambu_reference_commit();
const std::vector<BambuKeyDef>     &bambu_key_defs();
const std::vector<EnumTranslation> &generated_enum_translations();

// nullptr when Bambu Studio would discard the key on load.
const BambuKeyDef *find_key(const std::string &bambu_key);
// Bambu's name for one of our keys (itself when not renamed).
std::string bambu_key_name(const std::string &our_key);

// Bambu Studio version written into the Application tag, the project settings and the embedded
// presets of a Bambu export: the major.minor line of the Bambu Studio the key list was generated
// from, patch 0 ("02.08.00.00"). See docs/bambu-3mf-export.md for why.
std::string export_version();
// "BambuStudio-" + export_version().
std::string application_tag();

// One Bambu-shaped value, already serialized element by element.
struct Value
{
    bool                     vector  = false;
    bool                     strings = false; // coString(s): values are raw text
    std::vector<std::string> values;
};
using Config = std::map<std::string, Value>;

// What an export changed, for the log and the one-line notification.
struct Report
{
    std::set<std::string>              dropped;   // our keys Bambu cannot take (our spelling)
    std::map<std::string, std::string> renamed;   // our key -> Bambu key
    std::set<std::string>              converted; // Bambu keys whose value was reshaped
    std::vector<std::string>           notes;     // one line per non-trivial change, for the log

    void        merge(const Report &other);
    std::string summary() const;
};

// A per-extruder-variant slot layout: slot i is (names[i], ids[i]).
// ids are 1-based extruder ids (print / printer) or 1-based filament indices (filament).
struct Layout
{
    std::vector<std::string> names;
    std::vector<int>         ids;
    size_t size() const { return names.size(); }
};

// What the conversions of one project need to know about it.
struct Context
{
    // From our project config.
    double                                        nozzle_diameter = 0.; // first extruder, mm
    size_t                                        extruder_count  = 1;  // len(nozzle_diameter)
    size_t                                        filament_count  = 1;  // len(filament_colour)
    std::vector<std::string>                      process_flow_support;
    std::map<std::string, std::vector<double>>    numbers;             // numeric project options
    std::map<std::string, std::vector<std::string>> lists;             // raw values of the layout keys

    // Bambu layout of the converted project (filled by convert_project()).
    Layout print, printer, filament;

    static Context from_project(const ConfigBase &project);
};

enum class Scope {
    Project,  // Metadata/project_settings.config (full config)
    Print,    // embedded print preset
    Filament, // embedded filament preset (one filament)
    Printer,  // embedded printer preset
    Object,   // per-object / per-part / per-layer-range overrides in model_settings.config
};

// Convert the full project config; also fills ctx's Bambu layouts for the calls below.
Config convert_project(const ConfigBase &project, Context &ctx, Report &report);
// Convert an embedded preset or a per-object config with the project's context.
Config convert(const ConfigBase &cfg, const Context &ctx, Scope scope, Report &report, const std::string &where);

// Serialize for Metadata/*.config (JSON, same layout as ConfigBase::save_to_json).
std::string to_json(const Config &cfg, const std::string &name, const std::string &from, const std::string &version);
// Serialize one value for a model_settings.config metadata attribute (like ConfigBase::opt_serialize).
std::string serialize(const Value &value);
// Bambu's spelling of one of our enum values for a Bambu key; empty when Bambu has no such value.
std::string translate_enum_value(const std::string &bambu_key, const std::string &our_value);

} // namespace BambuExport
} // namespace Slic3r

#endif // slic3r_Format_BambuExport_hpp_
