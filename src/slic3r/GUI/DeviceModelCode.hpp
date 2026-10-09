#ifndef slic3r_GUI_DeviceModelCode_hpp_
#define slic3r_GUI_DeviceModelCode_hpp_

#include <map>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

// Bambu printers report a model code ("O1C2" for the H2C, "C12" for the P1S, ...) over SSDP, in the
// MQTT info push and in the cloud device list, and every printer definition in resources/printers
// is a file named after that code. Newer hardware revisions of a model report a sub-series code
// instead - an H2C from a later batch says "O1C2-V2" - and Bambu Studio lists those under a
// "subseries" key in the parent's definition. Without the mapping the code resolves to no
// definition at all, the printer shows as an unknown model and Send refuses with "incompatible
// model" (reported by an H2C owner, 2026-09-15). These helpers are pure so the mapping is testable
// without a device.

// The subseries table: parent model_id -> the sub-series codes it covers, read from every
// resources/printers/*.json that carries a "subseries" array. A missing or unreadable directory
// yields an empty table.
std::map<std::string, std::vector<std::string>> load_model_subseries(const std::string &printers_dir);

// The parent model_id whose subseries list contains `code`, or "" when none does.
std::string resolve_model_subseries(const std::string &code, const std::map<std::string, std::vector<std::string>> &table);

// "O1C2-V2" -> "O1C2": drop one trailing hardware-revision suffix of the form "-V<digits>". Codes
// without such a suffix come back unchanged. This is the fallback for a revision that is newer than
// the subseries table we ship; it must not touch codes where '-' is part of the name ("BL-P001").
std::string strip_model_revision(const std::string &code);

// The "printer_type" of <printers_dir>/<code>.json, or "" when there is no such definition (or the
// code could name a path outside the folder).
std::string read_definition_printer_type(const std::string &printers_dir, const std::string &code);

// A reported model code -> the printer_type of its definition: the code's own file, then a parent
// that lists it as a sub-series, then the code without its "-V<n>" revision. "" when none applies.
// This is what DeviceManager::parse_printer_type does against resources/printers.
std::string resolve_model_code(const std::string &code, const std::string &printers_dir,
                               const std::map<std::string, std::vector<std::string>> &subseries);

// The serial-number table: the first three characters of a Bambu serial ("20P" for an X2D, "094"
// for an H2D) -> the model_id of the definition that carries that "sn_prefix". Bambu Studio uses it
// to identify a printer that reported no usable model code (DevPrinterConfigUtil::
// get_printer_type_by_dev_id). When two definitions share a prefix the first file by name wins.
std::map<std::string, std::string> load_model_sn_prefixes(const std::string &printers_dir);

// The model_id whose sn_prefix matches the start of `dev_id`, or "" when none does.
std::string resolve_model_by_serial(const std::string &dev_id, const std::map<std::string, std::string> &sn_prefixes);

// What a device is: its reported code resolved as above, else its serial number's prefix. "" when
// neither identifies a definition in this build. Callers keep the raw code in that case so the
// send dialog can name it (DeviceManager::identify_printer_type).
std::string identify_device_model(const std::string &code, const std::string &dev_id, const std::string &printers_dir,
                                  const std::map<std::string, std::vector<std::string>> &subseries,
                                  const std::map<std::string, std::string>                &sn_prefixes);

// The send dialog's model check: the slicer profile's model id (`profile_model`) against the
// device's resolved printer_type (`device_model`). Equal ids pass, as does a profile model that the
// device's definition lists under "compatible_machine". An unidentified device (empty model)
// never matches a profile that has a model id.
bool device_matches_profile_model(const std::string &profile_model, const std::string &device_model,
                                  const std::vector<std::string> &device_compatible_machines);

} } // namespace Slic3r::GUI

#endif // slic3r_GUI_DeviceModelCode_hpp_
