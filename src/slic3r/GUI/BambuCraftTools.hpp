#ifndef slic3r_GUI_BambuCraftTools_hpp_
#define slic3r_GUI_BambuCraftTools_hpp_

// The tools a Bambu printer can report in its nozzle list instead of a hotend (the "craft kit" /
// cutting module on the H2C, H3C and H2D): a cutter, a pen, a laser or a fan. Kept free of wx and
// DeviceManager so the classification and the send check can be unit tested
// (tests/slic3rutils/bambu_craft_tools_tests.cpp).
//
// Everything here is read from Bambu Studio's source (v02.09.01.70, DeviceCore/DevNozzleSystem.cpp
// s_parse_nozzle_type / s_parse_cut_type and DevExtensionTool.cpp), not from a real printer: no
// report with a craft tool has been seen yet, so the codes below need confirming on hardware.
//
//  * device.nozzle.info[].type: four or more characters, the first one says what the tool is:
//    'H' hotend ("HS01" = Standard flow, hardened steel), 'C' cutting tool, 'L' laser, 'F' fan.
//    For a cutting tool the second character is the cutter kind; 'E' is the pen.
//  * device.ext_tool: {"type": "CP00" | "LB00" | "F000", "mount_3d": 0..3, "calib": 0..2}, an
//    extension tool fitted beside the toolhead (cutting module, laser module, enhanced cooling fan).
//
// One deliberate difference from Bambu Studio: a type it does not recognise (shorter than four
// characters, empty, or a first letter other than H/C/L/F) is not a hotend there. Here it stays a
// nozzle (ToolKind::Unknown counts as printing), so a printer that reports only nozzles is read
// exactly as before; only the four named non-printing tools are kept out of nozzle matching.

#include <nlohmann/json.hpp>

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace BambuCraftTools {

enum class ToolKind : int {
    Nozzle  = 0, // a hotend
    Cutter  = 1, // a cutting tool other than the pen ('C' + A/P/U/0 or a kind not known yet)
    Pen     = 2, // 'C' + 'E'
    Laser   = 3,
    Fan     = 4,
    Unknown = 5, // a type this table does not know: treated as a nozzle, as EdgeSlicer always did
};

// The tool a nozzle-list "type" names. Upper or lower case is accepted.
inline ToolKind tool_kind_of(const std::string& type)
{
    // Bambu Studio checks the long names first ("hardened_steel", ...): those are hotends.
    if (type == "undefine" || type == "hardened_steel" || type == "stainless_steel" || type == "brass" ||
        type == "tungsten_carbide")
        return ToolKind::Nozzle;
    // Short, empty and "N/A" (a hotend the printer could not read): a nozzle, as before.
    if (type.size() < 4)
        return ToolKind::Nozzle;
    switch (std::toupper((unsigned char) type[0])) {
    case 'H': return ToolKind::Nozzle;
    case 'C': return std::toupper((unsigned char) type[1]) == 'E' ? ToolKind::Pen : ToolKind::Cutter;
    case 'L': return ToolKind::Laser;
    case 'F': return ToolKind::Fan;
    default: return ToolKind::Unknown;
    }
}

// Whether a filament can be printed with it. Unknown counts, see above.
inline bool prints(ToolKind k) { return k == ToolKind::Nozzle || k == ToolKind::Unknown; }
// A cutter, pen, laser or fan: never matched to a filament or a nozzle size.
inline bool is_craft_tool(ToolKind k) { return !prints(k); }

// A key for logs and JSON: "nozzle", "cutter", "pen", "laser", "fan", "unknown".
inline const char* tool_kind_key(ToolKind k)
{
    switch (k) {
    case ToolKind::Nozzle: return "nozzle";
    case ToolKind::Cutter: return "cutter";
    case ToolKind::Pen: return "pen";
    case ToolKind::Laser: return "laser";
    case ToolKind::Fan: return "fan";
    default: return "unknown";
    }
}

// English name, for the phone's (untranslated) messages. The desktop translates its own
// (craft_tool_label in DeviceManager.cpp).
inline const char* tool_kind_name(ToolKind k)
{
    switch (k) {
    case ToolKind::Cutter: return "Cutter";
    case ToolKind::Pen: return "Pen";
    case ToolKind::Laser: return "Laser";
    case ToolKind::Fan: return "Fan";
    case ToolKind::Nozzle: return "Nozzle";
    default: return "Unknown tool";
    }
}

// ---- device.ext_tool ----------------------------------------------------------------------------

enum class ExtToolKind : int { None = 0, Cutter = 1, Laser = 2, Fan = 3, Unknown = 4 };

struct ExtTool
{
    bool        reported{ false }; // the report had an ext_tool object
    ExtToolKind kind{ ExtToolKind::None };
    std::string code;              // "CP00", "LB00", "F000", as sent
    int         mount{ 0 };        // mount_3d: 0 not mounted, 1 mounted, 2 no module, 3 no cable
    int         calib{ 0 };        // 0 none, 1 first, 2 mount
    bool        mounted() const { return kind != ExtToolKind::None && mount == 1; }
};

inline ExtToolKind ext_tool_kind_of(const std::string& code)
{
    if (code.empty()) return ExtToolKind::None;
    if (code == "CP00") return ExtToolKind::Cutter;
    if (code == "LB00") return ExtToolKind::Laser;
    if (code == "F000") return ExtToolKind::Fan;
    return ExtToolKind::Unknown;
}

// The tool kind an extension tool shows as (None when there is none).
inline ToolKind ext_tool_display_kind(ExtToolKind k)
{
    switch (k) {
    case ExtToolKind::Cutter: return ToolKind::Cutter;
    case ExtToolKind::Laser: return ToolKind::Laser;
    case ExtToolKind::Fan: return ToolKind::Fan;
    default: return ToolKind::Unknown;
    }
}

// device.ext_tool, read the way Bambu Studio does (DevExtensionToolParser::ParseV2_0): a field that
// is missing or of the wrong type keeps its previous value. Never throws.
inline ExtTool parse_ext_tool(const nlohmann::json& j, ExtTool prev = ExtTool())
{
    if (!j.is_object()) return prev;
    ExtTool out  = prev;
    out.reported = true;
    if (j.contains("mount_3d") && j["mount_3d"].is_number_integer()) out.mount = j["mount_3d"].get<int>();
    if (j.contains("calib") && j["calib"].is_number_integer()) out.calib = j["calib"].get<int>();
    if (j.contains("type") && j["type"].is_string()) {
        out.code = j["type"].get<std::string>();
        out.kind = ext_tool_kind_of(out.code);
    }
    return out;
}

// ---- the send check -----------------------------------------------------------------------------

// One entry of device.nozzle.info as the check needs it.
struct Tool
{
    int      id{ 0 };                   // report id: 0/1 = on extruder 0/1, 0x10 + n = rack position n
    ToolKind kind{ ToolKind::Nozzle };
    bool     exist{ true };             // its bit in device.nozzle.exist
    bool on_rack() const { return ((id >> 4) & 0xF) == 1; }
};

enum class SendBlock : int {
    None = 0,
    NoHotend,       // the printer reports craft tools and no nozzle at all
    ExtruderNoHotend, // a used extruder has no nozzle (its own or, for the main one, on the rack)
};

struct SendCheck
{
    SendBlock block{ SendBlock::None };
    int       physical_extruder{ -1 };      // ExtruderNoHotend: which one (0 main/right, 1 deputy/left)
    ToolKind  mounted{ ToolKind::Unknown }; // the craft tool mounted on it, Unknown when none is
    bool      ok() const { return block == SendBlock::None; }
};

// Whether a job may be sent, as far as the tools go (Bambu Studio SelectMachineDialog
// CheckErrorRackStatus "no available hotend" and CheckErrorExtruderNozzleWithSlicing, which only
// count Nozzle-kind tools). `mounted` is (physical extruder id, report id of the tool on it, the
// extruder's "hnow"); `used_physical` the physical extruders the job prints with (empty: not known,
// only the whole-printer check runs). A rack belongs to the main extruder (physical 0).
//
// A printer that reports no craft tool always passes: nozzle-only printers are not affected.
inline SendCheck check_send(const std::vector<Tool>& tools, const std::vector<std::pair<int, int>>& mounted,
                            const std::vector<int>& used_physical)
{
    SendCheck out;
    bool      any_craft = false, any_hotend = false;
    for (const Tool& t : tools) {
        any_craft  = any_craft || is_craft_tool(t.kind);
        any_hotend = any_hotend || (t.exist && prints(t.kind));
    }
    if (!any_craft) return out;
    if (!any_hotend) {
        out.block = SendBlock::NoHotend;
        return out;
    }
    for (int p : used_physical) {
        bool has = false;
        for (const Tool& t : tools)
            if (t.exist && prints(t.kind) && (t.on_rack() ? p == 0 : (t.id & 0xF) == p)) has = true;
        if (has) continue;
        out.block             = SendBlock::ExtruderNoHotend;
        out.physical_extruder = p;
        for (const auto& m : mounted)
            if (m.first == p)
                for (const Tool& t : tools)
                    if (t.id == m.second && is_craft_tool(t.kind)) out.mounted = t.kind;
        return out;
    }
    return out;
}

// The check's refusal in plain English (the phone's preview); empty when it passes. `side_names`
// says whether the printer has two extruders (then "the left/right extruder").
inline std::string send_block_text(const SendCheck& c, bool side_names)
{
    switch (c.block) {
    case SendBlock::NoHotend:
        return "the printer reports no nozzle to print with, only tools such as a cutter or pen; fit a nozzle and try again";
    case SendBlock::ExtruderNoHotend: {
        const std::string ext = side_names ? std::string(c.physical_extruder == 1 ? "the left extruder" : "the right extruder") :
                                             std::string("the extruder");
        if (c.mounted == ToolKind::Unknown)
            return ext + " has no nozzle to print with; fit a nozzle there before printing";
        return ext + " holds a " + tool_kind_key(c.mounted) + ", not a nozzle, and the printer has no nozzle for it; "
                     "fit a nozzle there before printing";
    }
    default: return std::string();
    }
}

}}} // namespace Slic3r::GUI::BambuCraftTools

#endif
