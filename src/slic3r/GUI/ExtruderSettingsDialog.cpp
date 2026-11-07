#include "ExtruderSettingsDialog.hpp"

#include "libslic3r/NozzleOverride.hpp"
#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/NozzleRackMemory.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"

#include "FlowTypeHelper.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>

#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r { namespace GUI {

bool nozzle_strip_sides()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return false;
    const DynamicPrintConfig &printer = bundle->printers.get_edited_preset().config;
    const auto               *nozzles = printer.option<ConfigOptionFloats>("nozzle_diameter");
    return nozzles != nullptr && nozzles->values.size() == 2 && is_bambu_grouping_machine(printer, 2);
}

NozzleStripInput current_nozzle_strip_input()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return NozzleStripInput();
    NozzleStripInput              in = nozzle_strip_input(bundle->printer_config_with_project_sizes(), nozzle_strip_sides());
    // The flow types are the project's (the sidebar's Flow setting), not the printer preset's.
    std::vector<NozzleVolumeType> types;
    for (const std::string &t : FlowType::nozzle_volume_types())
        types.push_back(t == FLOW_MODE_HIGH_FLOW ? nvtHighFlow : nvtStandard);
    nozzle_strip_set_volume_types(in, types);
    return in;
}

// The extruder each filament prints on as the sidebar knows it (-1 = decided when slicing).
static std::vector<int> strip_filament_extruders(const NozzleStripInput &in)
{
    PresetBundle    *bundle = wxGetApp().preset_bundle;
    std::vector<int> out;
    if (bundle == nullptr)
        return out;
    const size_t count = bundle->filament_presets.size();
    out.assign(count, -1);
    if (in.naming == NozzleStripNaming::Sides) {
        // The filament's own pin, else the plate's manual grouping, else the grouping of the last valid slice.
        Plater                 *plater = wxGetApp().plater();
        PartPlate              *plate  = plater != nullptr ? plater->get_partplate_list().get_curr_plate() : nullptr;
        const auto             *pins   = bundle->project_config.option<ConfigOptionInts>("filament_side_pin");
        const std::vector<int>  manual = plate != nullptr ? plate->get_manual_filament_map() : std::vector<int>();
        const std::vector<int>  sliced = plate != nullptr && plate->is_slice_result_valid() && plater != nullptr ? plater->get_global_filament_map() :
                                                                                                                   std::vector<int>();
        for (size_t f = 0; f < count; ++f) {
            int side = pins != nullptr && f < pins->values.size() ? pins->values[f] : 0;
            if (side != 1 && side != 2)
                side = f < manual.size() ? manual[f] : f < sliced.size() ? sliced[f] : 0;
            out[f] = side == 1 || side == 2 ? side - 1 : -1;
        }
        return out;
    }
    const std::vector<size_t> tools = bundle->filament_nozzles();
    for (size_t f = 0; f < count && f < tools.size(); ++f)
        out[f] = tools[f] < in.diameters.size() ? int(tools[f]) : -1;
    return out;
}

std::vector<NozzleStripTile> current_nozzle_strip_tiles(const NozzleStripInput &in, bool with_state)
{
    std::vector<NozzleStripTile> tiles  = nozzle_strip_tiles(in);
    PresetBundle                *bundle = wxGetApp().preset_bundle;
    if (!with_state || bundle == nullptr)
        return tiles;
    // The plate's filaments on their nozzles.
    const std::vector<int>        extruders = strip_filament_extruders(in);
    std::vector<NozzleStripPlace> places(extruders.size());
    for (size_t f = 0; f < extruders.size(); ++f) {
        places[f].extruder = extruders[f];
        places[f].slot_pin = bundle->filament_rack_slot_choice(f);
        places[f].size_pin = filament_rack_pin(bundle->project_config, unsigned(f + 1));
    }
    std::vector<unsigned int> used;
    if (Plater *plater = wxGetApp().plater())
        if (PartPlate *plate = plater->get_partplate_list().get_curr_plate())
            for (int id : plate->get_extruders())
                if (id >= 1)
                    used.push_back(unsigned(id - 1));
    place_nozzle_strip_filaments(tiles, in, places, used);
    // A nozzle size no process exists for (Process tab "No process fits this nozzle size").
    for (size_t tool : bundle->nozzle_selector_tools()) {
        const PresetBundle::NozzleToolRef ref = bundle->nozzle_tool_ref(tool);
        if (!ref.valid || bundle->nozzle_tool_process_info(tool).kind != PresetBundle::NozzleToolProcessKind::Missing)
            continue;
        for (NozzleStripTile &t : tiles)
            if (t.extruder == ref.extruder && !t.empty() && same_nozzle_diameter(t.diameter, ref.diameter)) {
                t.usable = false;
                t.reason = "No process fits this nozzle size";
            }
    }
    return tiles;
}

std::vector<double> nozzle_strip_size_choices()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return {};
    std::vector<double>       current = bundle->effective_nozzle_diameters();
    const std::vector<double> preset  = bundle->preset_nozzle_diameters();
    current.insert(current.end(), preset.begin(), preset.end());
    // The racks' sizes too, so a rack nozzle of an unusual size keeps its entry.
    for (const auto &rack : extruder_rack_nozzles(bundle->printer_config_with_project_sizes()))
        for (const RackNozzle &n : rack)
            current.push_back(n.diameter);
    return nozzle_size_choices(current, bundle->printers.diameters_of_selected_printer());
}

std::vector<NozzleVolumeType> nozzle_strip_type_choices()
{
    std::vector<NozzleVolumeType> out{ nvtStandard };
    if (FlowType::printer_supports_high_flow())
        out.push_back(nvtHighFlow);
    // A printer that declares a TPU285 nozzle (the H3C's rack slot 8) offers it too. Owner bug 2026-11-06: declared by any
    // nozzle variant of the selected printer's model (extruder_variant_list, default_nozzle_volume_type), read whenever the
    // list is shown, so the list is the same before and after the printer variant follows the nozzles (the H3C 0.2 variant
    // names no TPU285, but its rack can hold a 0.4 TPU285 nozzle), not only after a restart.
    if (PresetBundle *bundle = wxGetApp().preset_bundle) {
        bool tpu285 = nozzle_strip_printer_has_type(bundle->printer_config_with_project_sizes(), nvtTPU285);
        const NozzleRackLimits limits = bundle->nozzle_rack_limits();
        for (size_t e = 0; !tpu285 && e < limits.types.size(); ++e) {
            const std::vector<NozzleVolumeType> types = nozzle_rack_types(limits, e);
            tpu285 = std::find(types.begin(), types.end(), nvtTPU285) != types.end();
        }
        if (tpu285)
            out.push_back(nvtTPU285);
    }
    return out;
}

wxString nozzle_type_name(NozzleVolumeType type)
{
    switch (type) {
    case nvtHighFlow: return _L("High Flow");
    case nvtTPUHighFlow: return _L("TPU High Flow");
    case nvtTPU285: return _L("TPU285");
    case nvtE3DHighFlow: return _L("E3D High Flow");
    default: return _L("Standard");
    }
}

wxString nozzle_strip_extruder_name(const NozzleStripInput &in, size_t extruder)
{
    switch (in.naming) {
    case NozzleStripNaming::Sides: return extruder == 0 ? _L("Left") : _L("Right");
    case NozzleStripNaming::Numbers: return _L("Extruder");
    default: return wxString::Format("T%d", int(extruder + 1));
    }
}

bool apply_nozzle_strip_edit(wxWindow *parent, const NozzleStripInput &in, const NozzleStripEdit &edit, bool *kept_editing)
{
    if (kept_editing != nullptr)
        *kept_editing = false;
    PresetBundle *bundle = wxGetApp().preset_bundle;
    Plater       *plater = wxGetApp().plater();
    if (bundle == nullptr || plater == nullptr)
        return false;
    const size_t filaments = bundle->filament_presets.size();

    // Filaments pinned to a rack size or position the edit takes away (design 4.3: confirm before moving filaments).
    std::vector<double> size_pins(filaments, 0.);
    std::vector<int>    slot_pins(filaments, 0);
    for (size_t f = 0; f < filaments; ++f) {
        size_pins[f] = filament_rack_pin(bundle->project_config, unsigned(f + 1));
        slot_pins[f] = bundle->filament_rack_slot_choice(f);
    }
    const std::vector<unsigned int> orphaned = nozzle_strip_orphaned_pins(edit, in, strip_filament_extruders(in), size_pins, slot_pins);
    if (!orphaned.empty()) {
        wxString list;
        for (unsigned int f : orphaned)
            list += (list.empty() ? "" : ", ") + wxString::Format("%d", int(f + 1));
        MessageDialog dlg(parent,
                          wxString::Format(_L("Filament %s is pinned to a rack nozzle size or slot that is no longer there. It will print on "
                                              "its side's own nozzle instead."),
                                           list),
                          _L("Extruder settings"), wxICON_QUESTION | wxYES_NO);
        // YES is the harmless answer (MessageDialog answers YES for a phone / agent request): nothing is applied.
        dlg.SetButtonLabel(wxID_YES, _L("Keep editing"), true);
        dlg.SetButtonLabel(wxID_NO, _L("Apply"));
        if (dlg.ShowModal() != wxID_NO) {
            if (kept_editing != nullptr)
                *kept_editing = true;
            return false;
        }
        auto *pins = bundle->project_config.option<ConfigOptionFloats>("filament_rack_nozzle_diameter", true);
        for (unsigned int f : orphaned) {
            bundle->set_filament_rack_slot(f, 0);
            pins->values = Slic3r::set_filament_rack_pin(pins->values, f, filaments, 0.);
        }
    }

    bool                      changed = !orphaned.empty();
    const std::vector<double> preset  = bundle->preset_nozzle_diameters();
    const std::vector<double> current = bundle->effective_nozzle_diameters();
    for (size_t e = 0; e < edit.diameters.size() && e < current.size(); ++e)
        if (!same_nozzle_diameter(edit.diameters[e], current[e])) {
            // The preset's own size is stored as "no override".
            const double d = e < preset.size() && same_nozzle_diameter(edit.diameters[e], preset[e]) ? 0. : edit.diameters[e];
            changed |= bundle->set_tool_nozzle_diameter(e, d);
        }
    for (size_t e = 0; e < edit.rack_texts.size(); ++e)
        if (edit.rack_written[e] && edit.rack_texts[e] != bundle->rack_nozzle_override(e))
            changed |= bundle->set_rack_nozzle_override(e, edit.rack_texts[e]);
    std::vector<std::string>       types = FlowType::nozzle_volume_types();
    const std::vector<std::string> old   = types;
    for (size_t e = 0; e < types.size() && e < edit.volume_types.size(); ++e)
        types[e] = edit.volume_types[e] == nvtHighFlow ? FLOW_MODE_HIGH_FLOW : FLOW_MODE_STANDARD;
    if (types != old) {
        FlowType::set_nozzle_volume_types(types);
        changed = true;
    }
    if (!changed)
        return false;
    BOOST_LOG_TRIVIAL(info) << "nozzle strip: nozzles in this project now " << nozzle_diameter_summary(bundle->effective_nozzle_diameters());
    // One rebuild for the whole batch.
    plater->sidebar().on_nozzle_sizes_changed();
    // Owner hand test 2026-11-06: the printer variant follows when every extruder now carries one size (the Extruder settings
    // dialog and the strip's picker alike), after the dialog is closed.
    wxGetApp().CallAfter([]() {
        if (Plater *p = wxGetApp().plater(); p != nullptr)
            p->sidebar().follow_nozzle_sizes_with_printer_variant();
    });
    return true;
}

// ---- The dialog ---------------------------------------------------------------------------------------------------

ExtruderSettingsDialog::ExtruderSettingsDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Extruder settings"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_in(current_nozzle_strip_input())
    , m_sizes(nozzle_strip_size_choices())
    , m_types(nozzle_strip_type_choices())
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    auto *top    = new wxBoxSizer(wxVERTICAL);
    const int em = em_unit();

    // Sync, as in the sidebar's strip header.
    auto *sync_row = new wxBoxSizer(wxHORIZONTAL);
    auto *hint     = new wxStaticText(this, wxID_ANY, _L("The nozzles in this project. The printer preset is not changed."));
    hint->SetFont(Label::Body_12);
    hint->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6A")));
    auto *sync = new Button(this, _L("Sync from printer"));
    sync->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    sync->SetToolTip(_L("Read the nozzles from the connected printer (the Device page's state; nothing is sent)."));
    sync->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_REFRESH); });
    sync_row->Add(hint, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    sync_row->Add(sync, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(sync_row, 0, wxEXPAND | wxALL, FromDIP(16));

    auto size_index = [this](double d) {
        for (size_t i = 0; i < m_sizes.size(); ++i)
            if (same_nozzle_diameter(m_sizes[i], d))
                return int(i);
        return -1;
    };
    auto type_index = [this](NozzleVolumeType t) {
        for (size_t i = 0; i < m_types.size(); ++i)
            if (m_types[i] == t)
                return int(i);
        return 0;
    };

    for (size_t e = 0; e < m_in.diameters.size(); ++e) {
        const bool rack = nozzle_strip_has_rack(m_in, e);
        wxString   head = m_in.naming == NozzleStripNaming::Sides ? (e == 0 ? _L("Left extruder") : _L("Right extruder")) :
                          m_in.naming == NozzleStripNaming::Numbers ? _L("Extruder") :
                                                                      wxString::Format(_L("Toolhead T%d"), int(e + 1));
        if (rack)
            head += " (" + _L("nozzle rack") + ")";
        auto *title = new wxStaticText(this, wxID_ANY, head);
        title->SetFont(Label::Head_13);
        top->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

        // Nine positions (H3C) take two columns.
        const std::vector<RackNozzle> nozzles = rack ? nozzle_strip_rack(m_in, e) : std::vector<RackNozzle>();
        const int                     count   = rack ? std::max(m_in.capacity[e], int(nozzles.size())) : 1;
        const int                     columns = count > 6 ? 2 : 1;
        auto                         *grid    = new wxFlexGridSizer(3 * columns, FromDIP(6), FromDIP(10));
        for (int k = 0; k < count; ++k) {
            const wxString label = rack ? wxString::Format(_L("Nozzle %d"), k + 1) : _L("Nozzle");
            auto          *name  = new wxStaticText(this, wxID_ANY, label);
            name->SetFont(Label::Body_13);
            Row row;
            row.extruder = e;
            row.slot     = rack ? k + 1 : 0;
            row.size = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(em * 11, -1), 0, nullptr, wxCB_READONLY);
            row.type = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(em * 13, -1), 0, nullptr, wxCB_READONLY);
            // A rack position can be empty; an extruder always has a nozzle.
            if (rack)
                row.size->Append(_L("Empty"));
            for (double d : m_sizes)
                row.size->Append(from_u8(format_diameter_to_str(d)) + " mm");
            for (NozzleVolumeType t : m_types)
                row.type->Append(nozzle_type_name(t));
            const bool   filled = !rack || size_t(k) < nozzles.size();
            const double d      = !rack ? m_in.diameters[e] : filled ? nozzles[size_t(k)].diameter : 0.;
            const auto   t      = !rack ? m_in.volume_types[e] : filled ? nozzles[size_t(k)].volume_type : m_in.volume_types[e];
            const int    si     = filled ? size_index(d) : -1;
            row.size->SetSelection(rack ? (filled ? si + 1 : 0) : std::max(0, si));
            row.type->SetSelection(type_index(t));
            row.type->Enable(filled && m_types.size() > 1);
            ComboBox *size_combo = row.size, *type_combo = row.type;
            const bool has_types = m_types.size() > 1;
            row.size->Bind(wxEVT_COMBOBOX, [size_combo, type_combo, rack, has_types](wxCommandEvent &) {
                type_combo->Enable(has_types && !(rack && size_combo->GetSelection() == 0));
            });
            grid->Add(name, 0, wxALIGN_CENTER_VERTICAL);
            grid->Add(row.size, 0, wxALIGN_CENTER_VERTICAL);
            grid->Add(row.type, 0, wxALIGN_CENTER_VERTICAL | (columns > 1 && k % 2 == 0 ? wxRIGHT : 0), FromDIP(16));
            m_rows.push_back(row);
        }
        top->Add(grid, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    }

    auto *buttons = new DialogButtons(this, { "OK", "Cancel" });
    buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_ok(); });
    top->AddSpacer(FromDIP(16));
    top->Add(buttons, 0, wxEXPAND);
    SetSizer(top);
    top->SetSizeHints(this);
    wxGetApp().UpdateDlgDarkUI(this);
    CenterOnParent();
}

void ExtruderSettingsDialog::on_ok()
{
    NozzleStripEdit edit = nozzle_strip_edit_start(m_in);
    for (size_t e = 0; e < m_in.diameters.size(); ++e) {
        const bool              rack = nozzle_strip_has_rack(m_in, e);
        std::vector<RackNozzle> nozzles;
        for (const Row &row : m_rows) {
            if (row.extruder != e)
                continue;
            const int si = row.size->GetSelection() - (rack ? 1 : 0);
            if (si < 0 || size_t(si) >= m_sizes.size())
                continue; // an empty rack position
            RackNozzle n;
            n.diameter    = m_sizes[size_t(si)];
            const int ti  = row.type->GetSelection();
            n.volume_type = ti >= 0 && size_t(ti) < m_types.size() ? m_types[size_t(ti)] : nvtStandard;
            nozzles.push_back(n);
        }
        // A rack that was never written and still is just the extruder's own nozzle stays unwritten.
        if (rack && m_in.racks[e].empty() && nozzles.size() == 1 && same_nozzle_diameter(nozzles.front().diameter, m_in.diameters[e]) &&
            nozzles.front().volume_type == m_in.volume_types[e])
            continue;
        const std::string error = nozzle_strip_edit_set_rack(edit, m_in, e, nozzles);
        if (!error.empty()) {
            MessageDialog(this, nozzle_strip_extruder_name(m_in, e) + ": " + from_u8(error), _L("Extruder settings"), wxOK | wxICON_WARNING)
                .ShowModal();
            return;
        }
    }
    bool kept_editing = false;
    apply_nozzle_strip_edit(this, m_in, edit, &kept_editing);
    if (!kept_editing)
        EndModal(wxID_OK);
}

void ExtruderSettingsDialog::on_dpi_changed(const wxRect &)
{
    Layout();
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
