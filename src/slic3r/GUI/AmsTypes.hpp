#pragma once

// The AMS unit kinds a Bambu printer reports in bits 0..3 of print.ams.ams[].info (Bambu Studio
// DevAmsType, DevDefs.h), with the bit positions and labels Bambu Studio gives them. No wx, so the
// unit tests can use it directly.

#include <string>

namespace Slic3r { namespace GUI { namespace AmsTypes {

enum : int {
    EXT_SPOOL      = 0, // external spool holder
    AMS            = 1, // AMS (4 slots)
    AMS_LITE       = 2, // AMS lite (4 slots)
    N3F            = 3, // AMS 2 Pro (4 slots, heated)
    N3S            = 4, // AMS HT (1 slot, heated)
    AMS_LITE_MIXED = 5, // AMS lite on an N9
    RACK_A         = 6, // AMS 3H (H3C, 9 slots, two drying zones), Bambu Studio ZMULTI_A
    RACK_B         = 7, // AMS-B, Bambu Studio ZMULTI_B
};

// The H3C's AMS 3H (and the AMS-B) feed a nozzle rack directly: slot N belongs to rack nozzle N.
inline bool is_rack_ams(int type) { return type == RACK_A || type == RACK_B; }

// The unit's bit in ams.ams_exist_bits (Bambu Studio DevAms::IsExist): -1 when it has none.
inline int unit_exist_bit(int type, int ams_id)
{
    switch (type) {
    case EXT_SPOOL: // "dummy": read as an AMS, as before
    case AMS:
    case AMS_LITE:
    case N3F: return ams_id;
    case AMS_LITE_MIXED: return 12;
    case RACK_A: return 13;
    case RACK_B: return 14;
    default: return ams_id >= 128 ? 4 + (ams_id - 128) : -1; // AMS HT and anything older
    }
}

// The slot's bit in ams.tray_exist_bits / tray_is_bbl_bits / tray_read_done_bits / tray_reading_bits
// (Bambu Studio DevAms::GetTrayId / DevAmsTray::get_tray_flag_bit): -1 when it has none.
inline int tray_bit(int type, int ams_id, int slot)
{
    switch (type) {
    case EXT_SPOOL: // "dummy": read as an AMS, as before
    case AMS:
    case AMS_LITE:
    case N3F: return ams_id * 4 + slot;
    case AMS_LITE_MIXED: return 24 + slot;
    case RACK_A: return 28 + slot;
    case RACK_B: return 37 + slot;
    default: return ams_id >= 128 ? 16 + (ams_id - 128) : -1; // AMS HT: one slot
    }
}

// What a person calls the unit (Bambu Studio s_ams_display_formats, without the number).
inline std::string display_name(int type)
{
    switch (type) {
    case AMS: return "AMS";
    case AMS_LITE:
    case AMS_LITE_MIXED: return "AMS Lite";
    case N3F: return "AMS 2 Pro";
    case N3S: return "AMS HT";
    case RACK_A: return "AMS 3H";
    case RACK_B: return "AMS-B";
    default: return "AMS";
    }
}

// A rack AMS slot is labelled by its number alone, "1".."9" (Bambu Studio get_slot_name).
inline std::string rack_slot_label(int slot) { return std::to_string(slot + 1); }

// A rack printer reports one external spool holder per rack position, ids 255 down to 247;
// Bambu Studio labels them "1-Ext".."9-Ext" (256 - id) when there are more holders than extruders.
inline bool is_rack_virtual_slot(int id) { return id >= 247 && id <= 255; }
inline std::string rack_ext_label(int id) { return std::to_string(256 - id) + "-Ext"; }

}}} // namespace Slic3r::GUI::AmsTypes
