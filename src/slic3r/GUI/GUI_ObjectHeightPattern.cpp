// ObjectList glue for the height colour pattern (tests/design_height_colour_pattern.md, phase 2):
// opening the dialog, applying / resetting / detaching / removing a pattern with one undo snapshot
// each, and the pattern's row in the object list. The maths and the wording are in
// libslic3r/HeightPatternInput.hpp (unit-tested); this file only connects them to the list.

#include "GUI_ObjectList.hpp"

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "GLCanvas3D.hpp"
#include "Selection.hpp"
#include "MsgDialog.hpp"
#include "HeightPatternDialog.hpp"

#include "libslic3r/HeightPatternInput.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Slicing.hpp"

#include <algorithm>
#include <set>

namespace Slic3r { namespace GUI {

namespace {

struct HpoGeometry
{
    std::vector<double> tops;
    double              layer_height = 0.2;
    double              height       = 0.;
};

// The object's real layer grid, the way the slicer will lay it out without the pattern's own
// ranges: the hand-edited layer height profile when there is one, else the fixed layer height.
HpoGeometry hpo_geometry(const ModelObject &obj, const DynamicPrintConfig &full_config)
{
    HpoGeometry g;
    const float max_z = float(obj.max_z());
    const SlicingParameters sp = PrintObject::slicing_parameters(full_config, obj, max_z, Vec3d::Ones());
    const std::vector<coordf_t> profile = obj.layer_height_profile.empty() ?
        layer_height_profile_from_ranges(sp, t_layer_config_ranges{}) :
        obj.layer_height_profile.get();
    g.tops         = height_pattern_layer_tops(generate_object_layers(sp, profile, false));
    g.layer_height = sp.layer_height;
    g.height       = max_z;
    return g;
}

bool hpo_is_sla()
{
    return wxGetApp().preset_bundle != nullptr &&
           wxGetApp().preset_bundle->printers.get_selected_preset().printer_technology() == ptSLA;
}

// Physical + mixed filaments of the project.
int hpo_filament_count()
{
    if (wxGetApp().preset_bundle == nullptr)
        return 0;
    const size_t physical = size_t(std::max(wxGetApp().filaments_cnt(), 0));
    return int(wxGetApp().preset_bundle->mixed_filaments.total_filaments(physical));
}

wxString hpo_object_name(const ModelObject &obj)
{
    return obj.name.empty() ? _L("Object") : from_u8(obj.name);
}

wxString hpo_range_list(const std::vector<std::pair<double, double>> &ranges)
{
    wxString s;
    for (size_t i = 0; i < ranges.size() && i < 6; ++i)
        s += (i ? ", " : "") + wxString::Format("%.2f-%.2f", ranges[i].first, ranges[i].second);
    if (ranges.size() > 6)
        s += ", ...";
    return s + " " + _L("mm");
}

// The text of a pattern's row: what it is, then what is wrong with it.
wxString hpo_row_label(const ModelObject &obj, const HeightColourPattern &p, int filament_count, double layer_height)
{
    wxString label = from_u8(height_pattern_row_label(p));
    const HeightPatternHealth h = height_pattern_health(obj, p, filament_count, layer_height);
    if (h.modified)
        label += " - " + _L("modified");
    for (int f : h.missing_filaments)
        label += " - " + wxString::Format(_L("filament %d missing"), f);
    if (h.layer_height_stale)
        label += " - " + _L("layer height changed");
    return label;
}

} // namespace

void ObjectList::add_height_pattern_rows(const wxDataViewItem obj_item)
{
    const int obj_idx = m_objects_model->GetIdByItem(obj_item);
    if (obj_idx < 0 || obj_idx >= int(m_objects->size()) || hpo_is_sla())
        return;
    const ModelObject *mo = (*m_objects)[size_t(obj_idx)];
    if (mo->height_colour_patterns.empty())
        return;

    // The slicing parameters are only needed for the layer height; one per object is cheap.
    double layer_height = 0.;
    if (wxGetApp().preset_bundle != nullptr)
        layer_height = hpo_geometry(*mo, wxGetApp().preset_bundle->full_config()).layer_height;
    const int filaments = hpo_filament_count();

    for (const HeightColourPattern &p : mo->height_colour_patterns)
        m_objects_model->AddHeightPatternChild(obj_item, p.id, hpo_row_label(*mo, p, filaments, layer_height));
}

void ObjectList::refresh_height_pattern_rows(int obj_idx)
{
    if (obj_idx < 0 || obj_idx >= int(m_objects->size()))
        return;
    const wxDataViewItem obj_item = m_objects_model->GetItemById(obj_idx);
    if (!obj_item.IsOk())
        return;

    m_prevent_list_events = true;
    UnselectAll();
    // The Height ranges row hides the ranges a pattern owns, so it changes with every pattern edit.
    wxDataViewItem layers_root = m_objects_model->GetLayerRootItem(obj_item);
    if (layers_root.IsOk())
        m_objects_model->Delete(layers_root);
    m_objects_model->DeleteHeightPatternItems(obj_item);
    add_layer_root_item(obj_item);
    add_height_pattern_rows(obj_item);
    Expand(obj_item);
    m_prevent_list_events = false;
    select_item(obj_item);
}

void ObjectList::height_pattern_dialog(int obj_hint, int pattern_id)
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr || wxGetApp().preset_bundle == nullptr || hpo_is_sla())
        return;

    // Which objects: the selected ones for a new pattern, the one object for an edit.
    std::vector<int> targets;
    if (pattern_id > 0) {
        targets.push_back(obj_hint);
    } else {
        std::vector<int> vol_idxs;
        get_selection_indexes(targets, vol_idxs);
        if (targets.empty() && obj_hint >= 0)
            targets.push_back(obj_hint);
        if (targets.empty())
            if (const int idx = plater->get_view3D_canvas3D()->get_selection().get_object_idx(); idx >= 0)
                targets.push_back(idx);
    }
    targets.erase(std::remove_if(targets.begin(), targets.end(), [this](int i) { return i < 0 || i >= int(m_objects->size()); }), targets.end());
    if (targets.empty())
        return;
    const int primary = targets.front();
    ModelObject *primary_obj = (*m_objects)[size_t(primary)];

    const HeightColourPattern *existing = pattern_id > 0 ? find_height_colour_pattern(*primary_obj, pattern_id) : nullptr;
    if (pattern_id > 0 && existing == nullptr)
        return;

    const DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
    const HpoGeometry geo = hpo_geometry(*primary_obj, full_config);
    const int filaments   = std::max(hpo_filament_count(), 1);

    HeightPatternDialogContext ctx;
    ctx.object_name    = primary_obj->name;
    ctx.object         = primary_obj;
    ctx.own_pattern_id = pattern_id;
    ctx.layer_tops     = geo.tops;
    ctx.layer_height   = geo.layer_height;
    ctx.object_height  = geo.height;
    ctx.max_filament   = filaments;
    ctx.filament_colours = plater->get_extruder_colors_from_plater_config();

    // The rest of the current plate, for the "All objects on this plate" tick.
    std::set<int> in_targets(targets.begin(), targets.end());
    if (pattern_id <= 0) {
        if (PartPlate *plate = plater->get_partplate_list().get_curr_plate()) {
            for (ModelObject *mo : plate->get_objects_on_this_plate()) {
                const auto it = std::find(m_objects->begin(), m_objects->end(), mo);
                if (it == m_objects->end() || in_targets.count(int(it - m_objects->begin())))
                    continue;
                ++ctx.other_plate_objects;
                if (std::abs(mo->max_z() - primary_obj->max_z()) > 0.05)
                    ctx.other_heights_differ = true;
            }
        }
        for (int t : targets)
            if (std::abs((*m_objects)[size_t(t)]->max_z() - primary_obj->max_z()) > 0.05)
                ctx.other_heights_differ = true;
    }

    const HeightPatternInput initial = existing != nullptr ? height_pattern_input_from_pattern(*existing) :
                                                              HeightPatternDialog::load_last_input();

    HeightPatternDialog dlg(nullptr, ctx, initial);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const HeightPatternDialogResult res = dlg.result();

    // ---- plan every object first; nothing is touched until all of them are known to work ----------
    std::vector<int> apply_objs = targets;
    if (res.all_plate_objects)
        if (PartPlate *plate = plater->get_partplate_list().get_curr_plate())
            for (ModelObject *mo : plate->get_objects_on_this_plate()) {
                const auto it = std::find(m_objects->begin(), m_objects->end(), mo);
                if (it != m_objects->end() && !in_targets.count(int(it - m_objects->begin())))
                    apply_objs.push_back(int(it - m_objects->begin()));
            }

    struct Plan
    {
        int                                    obj_idx = -1;
        HpoGeometry                            geo;
        HeightColourPattern                    pattern;
        std::vector<std::pair<double, double>> clashes;
    };
    std::vector<Plan> plans;
    wxString skipped;
    for (int idx : apply_objs) {
        ModelObject *mo = (*m_objects)[size_t(idx)];
        Plan plan;
        plan.obj_idx = idx;
        plan.geo     = idx == primary ? geo : hpo_geometry(*mo, full_config);
        plan.pattern = height_pattern_from_input(res.input, plan.geo.layer_height, plan.geo.tops);
        plan.pattern.id = idx == primary ? pattern_id : 0;
        const HeightPatternExpansion exp = expand_height_colour_pattern(plan.pattern, plan.geo.tops, plan.geo.layer_height,
                                                                         plan.geo.height, filaments);
        if (!exp.ok() || exp.ranges.empty()) {
            const wxString why = exp.ok() ? _L("no band fits inside the object") : from_u8(exp.error);
            if (idx == primary) {
                MessageDialog err(nullptr, why, _L("Height colour pattern"), wxOK | wxICON_WARNING);
                err.ShowModal();
                return;
            }
            skipped += "\n" + hpo_object_name(*mo) + ": " + why;
            continue;
        }
        plan.clashes = find_height_pattern_clashes(*mo, exp.ranges, plan.pattern.id);
        plans.push_back(std::move(plan));
    }
    if (plans.empty())
        return;

    // The dialog already showed the primary object's clash on its button; ask about the others.
    wxString clash_msg;
    for (const Plan &plan : plans) {
        if (plan.clashes.empty() || (plan.obj_idx == primary && res.replace_confirmed))
            continue;
        clash_msg += "\n" + hpo_object_name(*(*m_objects)[size_t(plan.obj_idx)]) + ": " + hpo_range_list(plan.clashes);
    }
    if (!clash_msg.empty()) {
        MessageDialog ask(nullptr,
                          _L("The pattern overlaps height ranges these objects already have. Replace them?") + "\n" + clash_msg,
                          _L("Height colour pattern"), wxYES_NO | wxICON_QUESTION);
        if (ask.ShowModal() != wxID_YES)
            return;
    }

    // ---- apply, one undo step ------------------------------------------------------------------------
    wxString failed;
    {
        Plater::TakeSnapshot snapshot(plater, "Height colour pattern");
        for (const Plan &plan : plans) {
            ModelObject *mo = (*m_objects)[size_t(plan.obj_idx)];
            const HeightPatternApplyResult r = apply_height_colour_pattern(*mo, plan.pattern, plan.geo.tops, plan.geo.layer_height,
                                                                           true, plan.geo.height, filaments);
            if (!r.ok) {
                failed += "\n" + hpo_object_name(*mo) + ": " + from_u8(r.error);
                continue;
            }
            changed_object(plan.obj_idx);
        }
    }
    for (const Plan &plan : plans)
        refresh_height_pattern_rows(plan.obj_idx);

    // Leave the first object selected, on its pattern row when that is what was edited.
    if (pattern_id > 0)
        if (wxDataViewItem row = m_objects_model->GetHeightPatternItem(m_objects_model->GetItemById(primary), pattern_id); row.IsOk())
            select_item(row);

    if (!skipped.empty() || !failed.empty()) {
        MessageDialog info(nullptr,
                           (skipped.empty() ? wxString() : _L("Not applied to:") + skipped) + (failed.empty() ? wxString() : "\n" + _L("Failed:") + failed),
                           _L("Height colour pattern"), wxOK | wxICON_INFORMATION);
        info.ShowModal();
    }
}

void ObjectList::height_pattern_remove(int obj_idx, int pattern_id)
{
    if (obj_idx < 0 || obj_idx >= int(m_objects->size()))
        return;
    ModelObject *mo = (*m_objects)[size_t(obj_idx)];
    if (find_height_colour_pattern(*mo, pattern_id) == nullptr)
        return;
    {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Remove height colour pattern");
        remove_height_colour_pattern(*mo, pattern_id);
        changed_object(obj_idx);
    }
    refresh_height_pattern_rows(obj_idx);
}

void ObjectList::height_pattern_detach(int obj_idx, int pattern_id)
{
    if (obj_idx < 0 || obj_idx >= int(m_objects->size()))
        return;
    ModelObject *mo = (*m_objects)[size_t(obj_idx)];
    if (find_height_colour_pattern(*mo, pattern_id) == nullptr)
        return;
    {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Detach height colour pattern");
        // The ranges stay exactly as they are; they just stop being a pattern, so they show as
        // ordinary Height ranges from now on. Nothing about the slicing changes.
        detach_height_colour_pattern(*mo, pattern_id);
        changed_object(obj_idx);
    }
    refresh_height_pattern_rows(obj_idx);
}

void ObjectList::height_pattern_reset(int obj_idx, int pattern_id)
{
    if (obj_idx < 0 || obj_idx >= int(m_objects->size()) || wxGetApp().preset_bundle == nullptr)
        return;
    ModelObject *mo = (*m_objects)[size_t(obj_idx)];
    const HeightColourPattern *rec = find_height_colour_pattern(*mo, pattern_id);
    if (rec == nullptr)
        return;

    // Regenerate from what the user entered, for the project's current filaments and layer height.
    const HeightColourPattern pattern = *rec;
    const int filaments = std::max(hpo_filament_count(), 1);
    const HpoGeometry geo = hpo_geometry(*mo, wxGetApp().preset_bundle->full_config());
    const HeightPatternExpansion exp = expand_height_colour_pattern(pattern, geo.tops, geo.layer_height, geo.height, filaments);
    if (!exp.ok() || exp.ranges.empty()) {
        wxString why = exp.ok() ? _L("No band fits inside the object.") : from_u8(exp.error);
        if (!exp.ok())
            why += "\n\n" + _L("Use Edit to pick filaments the project has.");
        MessageDialog err(nullptr, why, _L("Height colour pattern"), wxOK | wxICON_WARNING);
        err.ShowModal();
        return;
    }

    const auto clashes = find_height_pattern_clashes(*mo, exp.ranges, pattern.id);
    if (!clashes.empty()) {
        MessageDialog ask(nullptr,
                          _L("Resetting the pattern overlaps height ranges that were added by hand. Replace them?") + "\n" + hpo_range_list(clashes),
                          _L("Height colour pattern"), wxYES_NO | wxICON_QUESTION);
        if (ask.ShowModal() != wxID_YES)
            return;
    }

    {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset height colour pattern");
        const HeightPatternApplyResult r = apply_height_colour_pattern(*mo, pattern, geo.tops, geo.layer_height, true, geo.height, filaments);
        if (!r.ok) {
            MessageDialog err(nullptr, from_u8(r.error), _L("Height colour pattern"), wxOK | wxICON_WARNING);
            err.ShowModal();
            return;
        }
        changed_object(obj_idx);
    }
    refresh_height_pattern_rows(obj_idx);
}

void ObjectList::show_height_pattern_menu(const wxDataViewItem &item)
{
    const int obj_idx    = m_objects_model->GetObjectIdByItem(item);
    const int pattern_id = m_objects_model->GetHeightPatternId(item);
    if (obj_idx < 0 || obj_idx >= int(m_objects->size()) || pattern_id <= 0)
        return;
    const ModelObject *mo = (*m_objects)[size_t(obj_idx)];
    const HeightColourPattern *rec = find_height_colour_pattern(*mo, pattern_id);
    if (rec == nullptr || wxGetApp().preset_bundle == nullptr)
        return;

    const HeightPatternHealth health = height_pattern_health(*mo, *rec, hpo_filament_count(),
                                                             hpo_geometry(*mo, wxGetApp().preset_bundle->full_config()).layer_height);

    wxMenu menu;
    const int id_edit = wxWindow::NewControlId(), id_reset = wxWindow::NewControlId(),
              id_detach = wxWindow::NewControlId(), id_remove = wxWindow::NewControlId();
    menu.Append(id_edit, _L("Edit..."));
    wxMenuItem *reset = menu.Append(id_reset, _L("Reset to the pattern"));
    reset->Enable(!health.ok());
    menu.Append(id_detach, _L("Detach (keep the ranges as plain height ranges)"));
    menu.AppendSeparator();
    menu.Append(id_remove, _L("Remove"));

    // Run after the menu has closed: every action rebuilds the rows this menu came from.
    auto defer = [this, obj_idx, pattern_id](void (ObjectList::*fn)(int, int)) {
        return [this, obj_idx, pattern_id, fn](wxCommandEvent &) {
            wxGetApp().CallAfter([this, obj_idx, pattern_id, fn]() { (this->*fn)(obj_idx, pattern_id); });
        };
    };
    menu.Bind(wxEVT_MENU, defer(&ObjectList::height_pattern_dialog), id_edit);
    menu.Bind(wxEVT_MENU, defer(&ObjectList::height_pattern_reset), id_reset);
    menu.Bind(wxEVT_MENU, defer(&ObjectList::height_pattern_detach), id_detach);
    menu.Bind(wxEVT_MENU, defer(&ObjectList::height_pattern_remove), id_remove);
    wxGetApp().plater()->PopupMenu(&menu);
}

void ObjectList::check_height_patterns_after_load()
{
    if (wxGetApp().preset_bundle == nullptr || hpo_is_sla())
        return;

    const DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
    const int filaments = hpo_filament_count();

    struct Issue { int obj_idx; int pattern_id; bool stale; };
    std::vector<Issue> issues;
    wxString lines;
    for (size_t i = 0; i < m_objects->size(); ++i) {
        const ModelObject *mo = (*m_objects)[i];
        if (mo->height_colour_patterns.empty())
            continue;
        const double layer_height = hpo_geometry(*mo, full_config).layer_height;
        for (const HeightColourPattern &p : mo->height_colour_patterns) {
            const HeightPatternHealth h = height_pattern_health(*mo, p, filaments, layer_height);
            if (h.layer_height_stale)
                lines += "\n" + hpo_object_name(*mo) + ": " + _L("made for another layer height; the bands would print with the old one");
            for (int f : h.missing_filaments)
                lines += "\n" + hpo_object_name(*mo) + ": " + wxString::Format(_L("uses filament %d, but the project has %d"), f, filaments);
            if (h.layer_height_stale || !h.missing_filaments.empty())
                issues.push_back({ int(i), p.id, h.layer_height_stale });
        }
    }
    if (issues.empty())
        return;

    const bool any_stale = std::any_of(issues.begin(), issues.end(), [](const Issue &i) { return i.stale; });
    wxString msg = _L("Some height colour patterns in this project no longer match it:") + lines;
    if (!any_stale) {
        msg += "\n\n" + _L("Open the pattern's row in the object list and use Edit to pick other filaments.");
        MessageDialog dlg(nullptr, msg, _L("Height colour pattern"), wxOK | wxICON_WARNING);
        dlg.ShowModal();
        return;
    }
    msg += "\n\n" + _L("Reset the patterns to the current layer height? A pattern that uses a missing filament cannot be reset and stays as it is.");
    MessageDialog dlg(nullptr, msg, _L("Height colour pattern"), wxYES_NO | wxICON_QUESTION);
    if (dlg.ShowModal() != wxID_YES)
        return;
    for (const Issue &i : issues)
        if (i.stale)
            height_pattern_reset(i.obj_idx, i.pattern_id);
}

}} // namespace Slic3r::GUI
