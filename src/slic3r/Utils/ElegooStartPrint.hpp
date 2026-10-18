#ifndef slic3r_ElegooStartPrint_hpp_
#define slic3r_ElegooStartPrint_hpp_

// The SDCP "start print" command (Cmd 128) that ElegooLink sends over the printer's websocket after
// an upload. This is the wx-free, socket-free part: it builds the JSON text, so a test can pin what
// the printer is sent without a printer, a socket or wx (tests/slic3rutils/elegoo_start_print_tests.cpp).
//
// The send dialog's choices reach this through PrintHostUpload::extended_info, a map of strings:
//   "timeLapse"          "1" / "0"
//   "heatedBedLeveling"  "1" / "0"
//   "bedType"            the BedType enum value as text
// A key that is absent is "off" (0), never an empty value: an empty value produced
//   "Calibration_switch":,   "Tlp_Switch":
// which is not JSON, and the printer never answered it.

#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace ElegooStartPrint {

// "1" is on, anything else - including a missing key - is off.
inline int flag(const std::map<std::string, std::string>& extended_info, const std::string& key)
{
    const auto it = extended_info.find(key);
    return (it != extended_info.end() && it->second == "1") ? 1 : 0;
}

// 1 when the dialog's bed type is the smooth PC plate (the BedType value passed as pc_bed_type),
// 0 for everything else including a missing key. The printer only knows the two sides of its plate.
inline int platform_type(const std::map<std::string, std::string>& extended_info, int pc_bed_type)
{
    const auto it = extended_info.find("bedType");
    return (it != extended_info.end() && it->second == std::to_string(pc_bed_type)) ? 1 : 0;
}

// The command as JSON text. filename is the name the file was uploaded under (no directory); it is
// escaped by the JSON writer, so a quote or a backslash in it cannot break the message.
inline std::string build_payload(const std::map<std::string, std::string>& extended_info,
                                 int                                       pc_bed_type,
                                 const std::string&                        filename,
                                 const std::string&                        request_id,
                                 long long                                 timestamp_ms,
                                 int                                       command = 128)
{
    using json = nlohmann::ordered_json;
    json data;
    data["Filename"]           = "/local/" + filename;
    data["StartLayer"]         = 0;
    data["Calibration_switch"] = flag(extended_info, "heatedBedLeveling");
    data["PrintPlatformType"]  = platform_type(extended_info, pc_bed_type);
    data["Tlp_Switch"]         = flag(extended_info, "timeLapse");

    json inner;
    inner["Cmd"]         = command;
    inner["Data"]        = std::move(data);
    inner["RequestID"]   = request_id;
    inner["MainboardID"] = "";
    inner["TimeStamp"]   = timestamp_ms;
    inner["From"]        = 1;

    json root;
    root["Id"]   = "";
    root["Data"] = std::move(inner);
    return root.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace ElegooStartPrint
} // namespace Slic3r

#endif
