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

// The tray number a send mapping names a slot by (ams_mapping, the "ams" of ams_mapping_info):
// Bambu Studio's DevFilaSystem::GetTrayIndexMap / DevMapping for a rack AMS (AMS 3H 28 + slot,
// AMS-B 37 + slot); this fork's ams_id * 4 + slot for every other unit, as it has always sent
// them (Bambu Studio sends an AMS HT as its ams id; this fork keeps 4 * ams id, unchanged here).
inline int mapping_tray_index(int type, int ams_id, int slot)
{
    if (type == RACK_A)
        return 28 + slot;
    if (type == RACK_B)
        return 37 + slot;
    return ams_id * 4 + slot;
}

// What a rack printer (H3C) calls a mapped tray number: an AMS 3H slot by its number ("1".."9",
// Bambu Studio transition_tridid: the rack nozzle position the slot feeds), an AMS-B slot likewise,
// a spool holder "1-Ext".."9-Ext". Empty for a number that is neither.
inline std::string rack_tray_label(int tray_index)
{
    if (tray_index >= 28 && tray_index < 37)
        return std::to_string(tray_index - 28 + 1);
    if (tray_index >= 37 && tray_index < 46)
        return std::to_string(tray_index - 37 + 1);
    if (is_rack_virtual_slot(tray_index))
        return rack_ext_label(tray_index);
    return {};
}

// The rack AMS slot, or the rack spool holder, a rack nozzle is fed from (Bambu Studio
// DevNozzleSystemParser::sTraverseBindAmsSlot, direct-connected rack):
//  1. the slot the nozzle reports feeding now (its "snow", ams << 8 | slot), when that slot exists;
//  2. otherwise, for a nozzle that is not a TPU-flow one, slot <addr> of the rack AMS whose slot
//     count equals the number of rack nozzles (the AMS 3H: slot N feeds rack nozzle N);
//  3. otherwise the spool holder 255 - <addr>, when it exists.
// exists(ams_id, slot_id) says whether the printer reports that slot (a holder is slot 0).
struct RackNozzle
{
    int  addr{-1};
    int  snow{-1};
    bool tpu_flow{false};
};
struct RackSlot
{
    int  ams_id{-1};
    int  slot_id{-1};
    bool valid() const { return ams_id >= 0 && slot_id >= 0; }
};
template<class Exists>
RackSlot bound_slot(const RackNozzle& n, int rack_ams_id, int rack_ams_slots, int rack_size, const Exists& exists)
{
    if (n.snow >= 0) {
        const int ams = (n.snow >> 8) & 0xFF, slot = n.snow & 0xFF;
        if (exists(ams, slot))
            return { ams, slot };
    }
    if (n.addr < 0)
        return {};
    if (!n.tpu_flow && rack_ams_id >= 0 && rack_ams_slots == rack_size && exists(rack_ams_id, n.addr))
        return { rack_ams_id, n.addr };
    if (exists(255 - n.addr, 0))
        return { 255 - n.addr, 0 };
    return {};
}

}}} // namespace Slic3r::GUI::AmsTypes
