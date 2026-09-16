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

} } // namespace Slic3r::GUI

#endif // slic3r_GUI_DeviceModelCode_hpp_
