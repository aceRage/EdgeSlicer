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
//   "canvasAttached"     "1" when the dialog read a CANVAS from the printer (Centauri Carbon 1)
//   "slotMap"            "t:canvas_id:tray_id,..." - which CANVAS tray each project filament
//                        (T-number, 0-based) prints from; "" = every filament as sliced
// A key that is absent is "off" (0), never an empty value: an empty value produced
//   "Calibration_switch":,   "Tlp_Switch":
// which is not JSON, and the printer never answered it.
//
// slot_map (design D4, tests/design_elegoo_canvas_sync_device_page.md): with a CANVAS attached the
// command carries Data.slot_map = [{"t":..,"canvas_id":..,"tray_id":..}], only for the filaments
// the person mapped, and EMPTY when every filament was left "As sliced" - the printer decides then.
// Without a CANVAS the key is not sent at all, so that command is byte-identical to before.
// AS_SLICED_IDENTITY_MAP is the one-line switch to the alternative (T0..Tn onto the trays in
// order) should the owner's test print show the printer needs it.

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace ElegooStartPrint {

// false: "As sliced" sends no entry for that filament (and an empty slot_map when nothing was
// mapped). true: "As sliced" maps filament t onto the t-th tray of the CANVAS that answered.
constexpr bool AS_SLICED_IDENTITY_MAP = false;

// One entry of Data.slot_map.
struct SlotMapEntry
{
    int t         = 0; // the project filament, 0-based: the T-number in the G-code
    int canvas_id = 0; // raw, as the printer reported it
    int tray_id   = 0; // raw, as the printer reported it

    bool operator==(const SlotMapEntry& o) const { return t == o.t && canvas_id == o.canvas_id && tray_id == o.tray_id; }
};

// "t:canvas:tray,..." <-> entries. Malformed items are skipped, never guessed at.
inline std::string format_slot_map(const std::vector<SlotMapEntry>& entries)
{
    std::string out;
    for (const SlotMapEntry& e : entries) {
        if (!out.empty())
            out += ",";
        out += std::to_string(e.t) + ":" + std::to_string(e.canvas_id) + ":" + std::to_string(e.tray_id);
    }
    return out;
}

inline std::vector<SlotMapEntry> parse_slot_map(const std::string& text)
{
    std::vector<SlotMapEntry> out;
    size_t                    pos = 0;
    while (pos <= text.size()) {
        const size_t      comma = text.find(',', pos);
        const std::string item  = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const size_t      c1    = item.find(':');
        const size_t      c2    = c1 == std::string::npos ? std::string::npos : item.find(':', c1 + 1);
        if (c1 != std::string::npos && c2 != std::string::npos) {
            try {
                size_t       u1 = 0, u2 = 0, u3 = 0;
                SlotMapEntry e;
                e.t         = std::stoi(item.substr(0, c1), &u1);
                e.canvas_id = std::stoi(item.substr(c1 + 1, c2 - c1 - 1), &u2);
                e.tray_id   = std::stoi(item.substr(c2 + 1), &u3);
                if (u1 == c1 && u2 == c2 - c1 - 1 && u3 == item.size() - c2 - 1 && e.t >= 0)
                    out.push_back(e);
            } catch (...) {}
        }
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    return out;
}

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

// The slot_map the command carries: none without a CANVAS (has_slot_map false), else the
// dialog's entries (possibly none).
inline std::vector<SlotMapEntry> slot_map(const std::map<std::string, std::string>& extended_info, bool& has_slot_map)
{
    has_slot_map = flag(extended_info, "canvasAttached") == 1;
    if (!has_slot_map)
        return {};
    const auto it = extended_info.find("slotMap");
    return it == extended_info.end() ? std::vector<SlotMapEntry>() : parse_slot_map(it->second);
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
    bool                            has_slot_map = false;
    const std::vector<SlotMapEntry> entries      = slot_map(extended_info, has_slot_map);
    if (has_slot_map) {
        json arr = json::array();
        for (const SlotMapEntry& e : entries) {
            json item;
            item["t"]         = e.t;
            item["canvas_id"] = e.canvas_id;
            item["tray_id"]   = e.tray_id;
            arr.push_back(std::move(item));
        }
        data["slot_map"] = std::move(arr);
    }

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
