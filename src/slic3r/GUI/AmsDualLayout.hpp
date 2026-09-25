#pragma once

// Pure layout rules for the Device tab's filament area on Bambu printers, kept free of wx so the
// unit tests can drive them with no window, no printer and no broker.
//
// Bambu Studio (DevFilaSystem.cpp, AMSControl.cpp) splits the filament area into one side per
// extruder on two-extruder machines (H2D, H2D Pro, H2C, X2D): the deputy extruder (id 1) is drawn
// on the LEFT, the main extruder (id 0) on the RIGHT. Which extruder an AMS unit feeds is carried
// in bits 8-11 of the unit's "info" hex string; the external spool holders are the virtual slots
// 254 (deputy) and 255 (main). This header reproduces those rules.

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace AmsDual {

// Unit kinds as reported in bits 0-3 of the AMS "info" field (Bambu DevAmsType).
enum UnitType : int {
    UNIT_EXT_SPOOL = 0, // external spool holder (virtual slot)
    UNIT_AMS       = 1, // AMS (4 slots)
    UNIT_AMS_LITE  = 2, // AMS lite (4 slots)
    UNIT_N3F       = 3, // AMS 2 Pro (4 slots, heated)
    UNIT_N3S       = 4, // AMS HT (1 slot, heated)
};

constexpr int MAIN_EXTRUDER   = 0; // right-hand extruder on two-extruder machines
constexpr int DEPUTY_EXTRUDER = 1; // left-hand extruder

constexpr int VIRTUAL_SLOT_MAIN   = 255; // external spool feeding the main extruder
constexpr int VIRTUAL_SLOT_DEPUTY = 254; // external spool feeding the deputy extruder

// Read `count` bits starting at `start` from a hex string of any length (Bambu's
// DevUtil::get_flag_bits_no_border): bit 0 is the least significant bit of the last digit, a
// "0x" prefix and whitespace are ignored, bits past the end read as 0, garbage reads as 0.
uint32_t flag_bits_no_border(const std::string &hex, int start, int count = 1);

// What the "info" hex string of one entry of print.ams.ams[] says about the unit.
struct UnitInfoBits
{
    int type           = UNIT_AMS;
    int extruder_id    = MAIN_EXTRUDER; // 0xE = "not initialised yet", such units are skipped
    int dry_status     = 0;             // bits 4-7
    int dry_fan1       = 0;             // bits 18-19
    int dry_fan2       = 0;             // bits 20-21
    int dry_sub_status = 0;             // bits 22-23
};
UnitInfoBits parse_unit_info(const std::string &info_hex);

// fun2 bit 5: the firmware accepts remote drying commands ("ams_filament_drying").
bool fun2_supports_remote_dry(const std::string &fun2_hex);

// A unit as the layout sees it.
struct UnitRef
{
    std::string ams_id;              // "0".."3", "128".."135", or "254"/"255" for an external spool
    int         type        = UNIT_AMS;
    int         extruder_id = MAIN_EXTRUDER;
    int         slot_count  = 4;
};

// Which extruder an external spool feeds on a machine with `extruder_count` extruders.
int  extruder_of_virtual_slot(int virtual_slot_id, int extruder_count);
bool is_virtual_slot(const std::string &ams_id);

// One side of the filament area.
struct Side
{
    int                           extruder_id = MAIN_EXTRUDER;
    std::vector<UnitRef>          units; // selector order: numeric ams id, external spool last
    std::vector<std::vector<int>> pages; // indices into `units`; a page is what the body shows
};

// Sides in display order: {deputy (left), main (right)} for two extruders, {main} for one.
// Units that name an extruder the machine does not have are shown on the main side.
// Pages: a multi-slot unit gets a page of its own; consecutive single-slot units (AMS HT,
// external spool) share a page two at a time, as Bambu Studio pairs them.
std::vector<Side> group_units(const std::vector<UnitRef> &units, int extruder_count);

// Page of `side` that shows `ams_id`, or -1.
int page_of_unit(const Side &side, const std::string &ams_id);

// Index into group_units()'s result of the side that holds `ams_id`, or -1.
int side_of_unit(const std::vector<Side> &sides, const std::string &ams_id);

// Label drawn above a slot: "A1".."D4" for 4-slot units, "A".."H" for AMS HT, "Ext" for a spool.
std::string slot_label(const UnitRef &unit, int slot_index);

// The extruder's "snow" (slot now) pair decoded by DeviceManager: an unloaded extruder reports
// slot 255 (and usually ams 255). An external spool is ams 254/255 with slot 0.
bool slot_is_loaded(const std::string &ams_id, const std::string &slot_id);

}}} // namespace Slic3r::GUI::AmsDual
