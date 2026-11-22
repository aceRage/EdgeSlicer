#ifndef slic3r_GUI_ToolTempLayout_hpp_
#define slic3r_GUI_ToolTempLayout_hpp_

// The data and the layout decisions behind the Device tab's tool temperature list
// (Widgets/ToolTempList; phase 3 of tests/design_device_multi_tool_temps.md, mockup
// tests/mockups/device_tool_temps.html). Kept free of wx and of DeviceManager so every decision
// can be unit tested on its own (tests/slic3rutils/tool_temp_layout_tests.cpp).
//
// ToolTempEntry is the small adapter between what DeviceManager reports and what the widget
// draws: one entry per tool on a carriage (extruder nozzle) or per rack slot (H3C). StatusPanel
// fills it from MachineObject::m_extder_data and m_nozzle_data.
//
// Layout, as approved by the owner on 2026-11-21:
//   1 tool        one full-height unbadged row (unchanged)
//   2 tools       two rows, L above R (unchanged)
//   3-4 tools     compact rows (32 DIP, no degree glyph) in the 122 DIP column
//   5-8 tools     a two-column grid of two-line cells in the column
//   9+ tools      a strip under the temperature box (no scrolling column)
//   rack          always a strip under the box, one cell per rack slot, read-only for now

#include "NozzleTempRows.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

// What sits in a tool position. Until feat/craft-tool-kinds lands every tool is a Nozzle.
enum class ToolKind { Nozzle, Cutter, Pen, Laser, Fan, Unknown };

// Kinds with no heater: never a temperature, never a target.
inline bool tool_kind_has_heater(ToolKind k) { return k == ToolKind::Nozzle || k == ToolKind::Unknown; }

struct ToolTempEntry
{
    std::string id;               // "nozzle<N>" for a tool on a carriage, "rack<P>" for a rack slot (P 0-based)
    std::string badge;            // "L", "R", "1".. ; empty for the single-nozzle row
    ToolKind    kind{ToolKind::Nozzle};
    int         extruder_id{-1};  // the id a set-temperature command addresses; -1 = none (rack slots)
    int         slot{-1};         // rack position, 0-based; -1 for a tool on a carriage
    bool        has_temp{true};   // the printer reported a temperature for it
    int         temp{0};
    int         target{0};
    bool        settable{false};  // a target may be sent (decided by the caller, never for a non-nozzle)
    bool        heating{false};
    bool        active{false};    // the tool in use (highlighted badge)
    bool        present{true};    // false = empty slot
    bool        mounted{false};   // rack slot whose nozzle is on the extruder now
    std::string size_text;        // "0.4", "0.4 HF"; empty when unknown
};

// ---- layout ---------------------------------------------------------------------------------

enum class ToolColumn {
    Single,  // today's one row
    Dual,    // today's two rows
    Compact, // 3-4 compact rows
    Grid,    // 5-8 two-column grid
    InStrip, // 9+ tools: nothing in the column but the strip under the box carries them
};

struct ToolStripGroup
{
    enum class What { Tools, Rack };
    What what{What::Rack};
    int  count{0};
    int  cols{0};
    int  rows{0};
};

struct ToolTempLayoutPlan
{
    ToolColumn                  column{ToolColumn::Single};
    std::vector<ToolStripGroup> strip; // empty = no strip under the box
    bool has_strip() const { return !strip.empty(); }
    // The two fixed rows of today's panel carry the tools (1 or 2 tools).
    bool uses_fixed_rows() const { return column == ToolColumn::Single || column == ToolColumn::Dual; }
};

constexpr int TOOL_TEMP_COMPACT_MAX  = 4;
constexpr int TOOL_TEMP_GRID_MAX     = 8;
constexpr int TOOL_TEMP_STRIP_COLS   = 9; // nine cells of about 59 DIP fit the 586 DIP box
constexpr int TOOL_TEMP_WARM_DEGREES = 50;

// Cells per strip row: one row up to nine cells, two rows above that (16 tools: 2 x 8).
inline int tool_strip_columns(int count)
{
    if (count <= 0) return 0;
    if (count <= TOOL_TEMP_STRIP_COLS) return count;
    return std::min(TOOL_TEMP_STRIP_COLS, (count + 1) / 2);
}

inline ToolStripGroup tool_strip_group(ToolStripGroup::What what, int count)
{
    ToolStripGroup g;
    g.what  = what;
    g.count = count;
    g.cols  = tool_strip_columns(count);
    g.rows  = g.cols > 0 ? (count + g.cols - 1) / g.cols : 0;
    return g;
}

// `tool_count`: tools on carriages (extruder rows); `rack_count`: rack slots to show (0 = none).
inline ToolTempLayoutPlan tool_temp_layout(int tool_count, int rack_count)
{
    ToolTempLayoutPlan plan;
    if (tool_count <= 1)
        plan.column = ToolColumn::Single; // also 0 tools: today's panel always shows the single row
    else if (tool_count == 2)
        plan.column = ToolColumn::Dual;
    else if (tool_count <= TOOL_TEMP_COMPACT_MAX)
        plan.column = ToolColumn::Compact;
    else if (tool_count <= TOOL_TEMP_GRID_MAX)
        plan.column = ToolColumn::Grid;
    else {
        plan.column = ToolColumn::InStrip;
        plan.strip.push_back(tool_strip_group(ToolStripGroup::What::Tools, tool_count));
    }
    if (rack_count > 0) plan.strip.push_back(tool_strip_group(ToolStripGroup::What::Rack, rack_count));
    return plan;
}

// ---- tools on carriages ----------------------------------------------------------------------

// The extruder rows as entries. 1 and 2 rows keep nozzle_temp_rows' order (L above R); 3+ rows are
// listed by number, 1 at the top, as the mockup shows. Every extruder row is settable.
inline std::vector<ToolTempEntry> tool_entries_from_rows(const std::vector<NozzleTempRow> &rows)
{
    std::vector<ToolTempEntry> out;
    for (const NozzleTempRow &r : rows) {
        ToolTempEntry e;
        e.id          = "nozzle" + std::to_string(r.extruder_id);
        e.badge       = r.badge;
        e.extruder_id = r.extruder_id;
        e.temp        = r.temp;
        e.target      = r.target;
        e.heating     = r.heating;
        e.active      = r.active;
        e.settable    = true;
        out.push_back(e);
    }
    if (out.size() > 2)
        std::stable_sort(out.begin(), out.end(), [](const ToolTempEntry &a, const ToolTempEntry &b) { return a.extruder_id < b.extruder_id; });
    return out;
}

// ---- rack (H3C) -------------------------------------------------------------------------------

// One device.nozzle.info[] entry, as far as the rack strip needs it (DeviceManager's struct Nozzle).
struct RackNozzleSample
{
    int      report_id{0};   // 0 / 1 extruder nozzle, 0x10 + n rack position n
    int      addr_id{-1};    // rack position, -1 = not reported
    bool     exist{true};
    float    diameter{0.f};  // 0 = unknown
    bool     high_flow{false};
    bool     has_temp{false};
    int      temp{0};
    int      target{0};
    ToolKind kind{ToolKind::Nozzle};
};

inline bool rack_sample_on_rack(int report_id) { return ((report_id >> 4) & 0xF) == 1; }

inline std::string rack_size_text(float diameter, bool high_flow)
{
    std::string s;
    if (diameter > 0.f) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1f", double(diameter));
        s = buf;
    }
    if (high_flow) s += s.empty() ? "HF" : " HF";
    return s;
}

struct RackEntries
{
    std::vector<ToolTempEntry> slots;        // in rack order
    int                        mounted_slot{-1}; // position of the nozzle on the extruder, -1 = none known
};

// The rack slots to show, read-only (owner decision D4). Empty unless at least one rack entry
// reported a temperature, so a rack that sends none (H2C, as far as we know) and every printer
// without a rack look exactly as before. A slot is "mounted" when an extruder nozzle (report id
// 0 / 1) names its position in addr_id (Bambu Studio PageHotend IsNozzleOnExtruder); a mounted
// position the report does not list in the rack is added as a mounted slot.
inline RackEntries rack_entries(const std::vector<RackNozzleSample> &nozzles)
{
    RackEntries out;

    bool any_temp = false;
    for (const RackNozzleSample &n : nozzles)
        if (rack_sample_on_rack(n.report_id) && n.has_temp) any_temp = true;
    if (!any_temp) return out;

    const RackNozzleSample *on_extruder = nullptr;
    for (const RackNozzleSample &n : nozzles)
        if (!rack_sample_on_rack(n.report_id) && n.addr_id >= 0) { on_extruder = &n; break; }
    if (on_extruder) out.mounted_slot = on_extruder->addr_id;

    auto make = [](const RackNozzleSample &n, int pos) {
        ToolTempEntry e;
        e.id        = "rack" + std::to_string(pos);
        e.slot      = pos;
        e.badge     = std::to_string(pos + 1);
        e.kind      = n.kind;
        e.present   = n.exist;
        e.has_temp  = n.has_temp && n.exist && tool_kind_has_heater(n.kind);
        e.temp      = e.has_temp ? n.temp : 0;
        e.target    = e.has_temp ? n.target : 0;
        e.heating   = e.has_temp && nozzle_is_heating(e.temp, e.target);
        e.size_text = n.exist && n.kind == ToolKind::Nozzle ? rack_size_text(n.diameter, n.high_flow) : std::string();
        e.settable  = false;
        return e;
    };

    for (const RackNozzleSample &n : nozzles) {
        if (!rack_sample_on_rack(n.report_id)) continue;
        const int pos = n.addr_id >= 0 ? n.addr_id : (n.report_id & 0xF);
        bool      dup = false;
        for (const ToolTempEntry &e : out.slots)
            if (e.slot == pos) { dup = true; break; }
        if (dup) continue;
        out.slots.push_back(make(n, pos));
    }

    if (on_extruder) {
        bool listed = false;
        for (ToolTempEntry &e : out.slots)
            if (e.slot == out.mounted_slot) {
                listed    = true;
                e.mounted = true;
                e.present = true;
            }
        if (!listed) {
            ToolTempEntry e = make(*on_extruder, out.mounted_slot);
            e.present       = true;
            e.mounted       = true;
            out.slots.push_back(e);
        }
    }
    // A mounted slot's temperature is the extruder's row; never shown twice.
    for (ToolTempEntry &e : out.slots)
        if (e.mounted) {
            e.has_temp = false;
            e.temp = e.target = 0;
            e.heating         = false;
        }

    std::sort(out.slots.begin(), out.slots.end(), [](const ToolTempEntry &a, const ToolTempEntry &b) { return a.slot < b.slot; });
    return out;
}

// "Rack - n warm": nozzles waiting in the rack that have a target or are still hot.
inline int rack_warm_count(const std::vector<ToolTempEntry> &slots)
{
    int n = 0;
    for (const ToolTempEntry &e : slots)
        if (e.present && e.kind == ToolKind::Nozzle && !e.mounted && e.has_temp && (e.target > 0 || e.temp >= TOOL_TEMP_WARM_DEGREES)) ++n;
    return n;
}

// "Tools - n of N heating or hot": tools with a target set.
inline int tools_with_target_count(const std::vector<ToolTempEntry> &tools)
{
    int n = 0;
    for (const ToolTempEntry &e : tools)
        if (e.has_temp && e.target > 0) ++n;
    return n;
}

// ---- what one strip cell shows ---------------------------------------------------------------

enum class ToolCellWord { None, Empty, OnTool, Cutter, Pen, Laser, Fan, Unknown };

struct ToolCellView
{
    ToolCellWord word{ToolCellWord::None};
    bool         show_temp{false};      // current temperature
    bool         show_target{false};    // "/ 220" (read-only) or the target field (settable)
    bool         settable{false};
    bool         heating{false};        // orange heater icon
    bool         show_heater_icon{false};
    bool         highlight_badge{false}; // mounted or active: green badge
    bool         dim_badge{false};       // empty slot
    bool         dashed_border{false};   // empty slot
    bool         accent_border{false};   // mounted slot
    bool         show_size{false};
};

inline ToolCellWord tool_kind_word(ToolKind k)
{
    switch (k) {
    case ToolKind::Cutter: return ToolCellWord::Cutter;
    case ToolKind::Pen: return ToolCellWord::Pen;
    case ToolKind::Laser: return ToolCellWord::Laser;
    case ToolKind::Fan: return ToolCellWord::Fan;
    case ToolKind::Unknown: return ToolCellWord::Unknown;
    default: return ToolCellWord::None;
    }
}

// `rack_targets_settable` is ToolTempList's off switch for phase 5 (setting a rack nozzle's
// target): while it is false no rack slot is ever settable. A tool on a carriage is settable when
// the entry says so. Mounted, empty, non-heating and unknown slots never are.
inline ToolCellView tool_cell_view(const ToolTempEntry &e, bool rack_targets_settable)
{
    ToolCellView v;
    v.highlight_badge = e.mounted || e.active;
    if (!e.present) {
        v.word          = ToolCellWord::Empty;
        v.dim_badge     = true;
        v.dashed_border = true;
        return v;
    }
    if (!tool_kind_has_heater(e.kind)) {
        v.word = tool_kind_word(e.kind);
        return v;
    }
    if (e.mounted) {
        v.word          = ToolCellWord::OnTool;
        v.accent_border = true;
        v.show_size     = !e.size_text.empty();
        return v;
    }
    if (e.kind == ToolKind::Unknown)
        v.word = ToolCellWord::Unknown;
    else
        v.show_size = !e.size_text.empty();
    v.show_temp = e.has_temp;
    const bool is_rack = e.slot >= 0;
    v.settable         = e.kind == ToolKind::Nozzle && e.has_temp && (is_rack ? rack_targets_settable : e.settable);
    v.show_target      = v.settable || (e.has_temp && e.target > 0);
    v.show_heater_icon = e.has_temp && e.target > 0;
    v.heating          = e.has_temp && e.heating;
    return v;
}

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ToolTempLayout_hpp_
