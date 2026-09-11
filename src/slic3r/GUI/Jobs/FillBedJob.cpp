#include "FillBedJob.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "libnest2d/common.hpp"
#include "libslic3r/FillBedPack.hpp"

#include <numeric>

namespace Slic3r {
namespace GUI {

//BBS: add partplate related logic
void FillBedJob::prepare()
{
    PartPlateList& plate_list = m_plater->get_partplate_list();

    m_locked.clear();
    m_selected.clear();
    m_unselected.clear();
    m_bedpts.clear();

    params = init_arrange_params(m_plater);
    // The dialog gap goes in AFTER init_arrange_params, which sets min_obj_distance from the
    // arrange toolbar spacing (and zeroes it when the plate's print sequence differs from the
    // global one). It still goes through params.min_obj_distance, so update_selected_items_inflation
    // can raise it for sequential-print clearance. 0 keeps today's "auto" behaviour.
    if (m_settings.gap > 0.)
        params.min_obj_distance = scaled(m_settings.gap);
    params.allow_rotations = m_settings.allow_rotation;

    m_object_idx = m_plater->get_selected_object_idx();
    if (m_object_idx == -1)
        return;

    //select current plate at first
    int sel_id = m_plater->get_selection().get_instance_idx();
    sel_id = std::max(sel_id, 0);

    // The live-estimate probe must not move the user's plate selection out from under the open
    // dialog - it runs on every keystroke. The plate is already the right one by then: the
    // dialog was opened on this object, and the real run re-selects it a moment later anyway.
    if (!m_estimate_only) {
        int sel_ret = plate_list.select_plate_by_obj(m_object_idx, sel_id);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":select plate obj_id %1%, ins_id %2%, ret %3%}") % m_object_idx % sel_id % sel_ret;
    }

    PartPlate* plate = plate_list.get_curr_plate();
    Model& model = m_plater->model();
    BoundingBox plate_bb = plate->get_bounding_box_crd();
    int plate_cols = plate_list.get_plate_cols();
    int cur_plate_index = plate->get_index();

    ModelObject *model_object = m_plater->model().objects[m_object_idx];
    if (model_object->instances.empty()) return;

    const Slic3r::DynamicPrintConfig& global_config = wxGetApp().preset_bundle->full_config();
    m_selected.reserve(model_object->instances.size());
    for (size_t oidx = 0; oidx < model.objects.size(); ++oidx)
    {
        ModelObject* mo = model.objects[oidx];
        for (size_t inst_idx = 0; inst_idx < mo->instances.size(); ++inst_idx)
        {
            bool selected = (oidx == m_object_idx);

            ArrangePolygon ap = get_instance_arrange_poly(mo->instances[inst_idx], global_config);
            BoundingBox ap_bb = ap.transformed_poly().contour.bounding_box();
            ap.name = mo->name;

            if (selected)
            {
                if (mo->instances[inst_idx]->printable)
                {
                    ++ap.priority;
                    ap.itemid = m_selected.size();
                    m_selected.emplace_back(ap);
                }
                else
                {
                    if (plate_bb.contains(ap_bb))
                    {
                        ap.bed_idx = 0;
                        ap.itemid = m_unselected.size();
                        ap.row = cur_plate_index / plate_cols;
                        ap.col = cur_plate_index % plate_cols;
                        ap.translation(X) -= bed_stride_x(m_plater) * ap.col;
                        ap.translation(Y) += bed_stride_y(m_plater) * ap.row;
                        m_unselected.emplace_back(ap);
                    }
                    else
                    {
                        ap.bed_idx = PartPlateList::MAX_PLATES_COUNT;
                        ap.itemid = m_locked.size();
                        m_locked.emplace_back(ap);
                    }
                }
            }
            else
            {
                if (plate_bb.contains(ap_bb))
                {
                    ap.bed_idx = 0;
                    ap.itemid = m_unselected.size();
                    ap.row = cur_plate_index / plate_cols;
                    ap.col = cur_plate_index % plate_cols;
                    ap.translation(X) -= bed_stride_x(m_plater) * ap.col;
                    ap.translation(Y) += bed_stride_y(m_plater) * ap.row;
                    m_unselected.emplace_back(ap);
                }
                else
                {
                    ap.bed_idx = PartPlateList::MAX_PLATES_COUNT;
                    ap.itemid = m_locked.size();
                    m_locked.emplace_back(ap);
                }
            }
        }
    }
    /*
    for (ModelInstance *inst : model_object->instances)
        if (inst->printable) {
            ArrangePolygon ap = get_arrange_poly(inst);
            // Existing objects need to be included in the result. Only
            // the needed amount of object will be added, no more.
            ++ap.priority;
            m_selected.emplace_back(ap);
        }*/

    if (m_selected.empty()) return;

    //add the virtual object into unselect list if has
    double scaled_exclusion_gap = scale_(1);
    plate_list.preprocess_exclude_areas(params.excluded_regions, 1, scaled_exclusion_gap);
    plate_list.preprocess_exclude_areas(m_unselected);

    m_bedpts = get_bed_shape(*m_plater->config());

    auto &objects = m_plater->model().objects;
    /*BoundingBox bedbb = get_extents(m_bedpts);

    for (size_t idx = 0; idx < objects.size(); ++idx)
        if (int(idx) != m_object_idx)
            for (ModelInstance *mi : objects[idx]->instances) {
                ArrangePolygon ap = get_arrange_poly(mi);
                auto ap_bb = ap.transformed_poly().contour.bounding_box();

                if (ap.bed_idx == 0 && !bedbb.contains(ap_bb))
                    ap.bed_idx = arrangement::UNARRANGED;

                m_unselected.emplace_back(ap);
            }*/
    if (auto wt = get_wipe_tower_arrangepoly(*m_plater))
        m_unselected.emplace_back(std::move(*wt));

    double sc = scaled<double>(1.) * scaled(1.);

    double unsel_area = std::accumulate(m_unselected.begin(),
                                        m_unselected.end(), 0.,
                                        [cur_plate_index](double s, const auto &ap) {
                                            //BBS: m_unselected instance is in the same partplate
                                            return s + (ap.bed_idx == cur_plate_index) * ap.poly.area();
                                        }) / sc;

    // The bed the packer will really get: arrange's own shrink, then the user's edge margins.
    // update_arrange_params() +=s the skirt distance into bed_shrink_x/y, so it must not run on
    // the member `params` here - process() runs it there. A copy keeps this idempotent without
    // touching Arrange.cpp.
    arrangement::ArrangeParams est_params = params;
    update_arrange_params(est_params, m_plater->config(), m_selected);
    // Sequential print raises min_obj_distance to the extruder clearance; that happens inside
    // update_selected_items_inflation, so run it on throwaway copies of the items to learn the
    // real gap without disturbing the ones process() will pack.
    {
        ArrangePolygons probe = m_selected;
        update_selected_items_inflation(probe, m_plater->config(), est_params);
    }
    const Points est_bedpts = apply_user_bed_margins(get_shrink_bedpts(m_plater->config(), est_params), est_params);
    const double bed_area   = std::abs(Polygon{est_bedpts}.area()) / sc;

    // The area already taken by the objects staying put, plus the copies of the template that
    // are already on the plate.
    const double sel_area   = m_selected.front().poly.area() / sc;
    const double free_area  = std::max(0., bed_area - unsel_area - double(m_selected.size()) * sel_area);

    // The gap the packer will really use. update_selected_items_inflation() takes the
    // min_obj_distance/2 branch once a distance is set, dropping the brim entirely, so the fill
    // path widens the gap to the brim where the brim is wider - a 1 mm gap on a tree-support
    // object would otherwise mean colliding brims.
    double brim_max = 0.;
    for (const ArrangePolygon &ap : m_selected) brim_max = std::max(brim_max, double(ap.brim_width));
    const double eff_gap = std::max(unscaled<double>(est_params.min_obj_distance), brim_max);
    if (m_settings.gap > 0.)
        params.min_obj_distance = scaled(eff_gap);

    const BoundingBox tmpl_bb = m_selected.front().poly.contour.bounding_box();
    const double tmpl_w = unscaled<double>(tmpl_bb.size().x());
    const double tmpl_h = unscaled<double>(tmpl_bb.size().y());

    // A tiling estimate, not an area ratio: the gap is SHARED between neighbours, so a copy
    // occupies (w+gap)*(h+gap) rather than the area of its fully inflated outline. The overshoot
    // means a slightly optimistic pack is never starved of items; whatever lands on a virtual bed
    // is dropped by finalize().
    int needed_items = 0;
    if (m_settings.layout == fill_bed::Layout::Grid) {
        // Grid is deterministic and O(cells), so there is nothing to estimate and no cap to
        // respect: build the real grid now and ask for exactly that many clones.
        m_grid_cells = fill_bed::grid_pack(m_selected.front().poly,
                                           est_bedpts,
                                           scaled(eff_gap),
                                           m_settings.allow_rotation,
                                           grid_obstacles(est_params));
        // The copies already on the plate occupy cells of their own; the rest is what to clone.
        needed_items = std::max(0, int(m_grid_cells.size()) - int(m_selected.size()));
    } else {
        needed_items = fill_bed::estimate_count_with_overshoot(tmpl_w, tmpl_h, eff_gap, free_area);
        // The NFP packer is roughly O(n^2), so Compact is capped. Above the cap the fill falls
        // back to Grid - which the dialog warns about before the user presses Fill - rather than
        // taking minutes or, as the old code did, dropping onto a bounding-box branch that knew
        // nothing about the gap or the other objects.
        const int raw = fill_bed::estimate_count(tmpl_w, tmpl_h, eff_gap, free_area);
        if (raw > fill_bed::COUNT_CAP) {
            m_fallback_to_grid = true;
            m_grid_cells       = fill_bed::grid_pack(m_selected.front().poly,
                                                     est_bedpts,
                                                     scaled(eff_gap),
                                                     m_settings.allow_rotation,
                                                     grid_obstacles(est_params));
            needed_items = std::max(0, int(m_grid_cells.size()) - int(m_selected.size()));
        } else {
            needed_items = std::max(0, std::min(needed_items, fill_bed::COUNT_CAP - int(m_selected.size())));
        }
    }

    //int sel_id = m_plater->get_selection().get_instance_idx();
    // if the selection is not a single instance, choose the first as template
    //sel_id = std::max(sel_id, 0);
    ModelInstance *mi = model_object->instances[sel_id];
    ArrangePolygon template_ap = get_instance_arrange_poly(mi, global_config);

    // The probe only wants m_grid_cells; the clones it would build are thrown away with it.
    if (m_estimate_only)
        needed_items = 0;

    for (int i = 0; i < needed_items; ++i) {
        ArrangePolygon ap = template_ap;
        ap.poly = m_selected.front().poly;
        ap.bed_idx = PartPlateList::MAX_PLATES_COUNT;
        ap.itemid = -1;
        ap.setter = [this](const ArrangePolygon &p) {
            ModelObject *mo = m_plater->model().objects[m_object_idx];
            ModelObject* newObj = m_plater->model().add_object(*mo);
            newObj->name = mo->name +" "+ std::to_string(p.itemid);
            for (ModelInstance *newInst : newObj->instances) { newInst->apply_arrange_result(p.translation.cast<double>(), p.rotation); }
            //m_plater->sidebar().obj_list()->paste_objects_into_list({m_plater->model().objects.size()-1});
        };
        m_selected.emplace_back(ap);
    }

    m_status_range = m_selected.size();

    // The strides have to be removed from the fixed items. For the
    // arrangeable (selected) items bed_idx is ignored and the
    // translation is irrelevant.
    //BBS: remove logic for unselected object
    /*double stride = bed_stride(m_plater);
    for (auto &p : m_unselected)
        if (p.bed_idx > 0)
            p.translation(X) -= p.bed_idx * stride;*/
}

void FillBedJob::process(Ctl &ctl)
{
    auto statustxt = _u8L("Filling");
    ctl.call_on_main_thread([this] { prepare(); }).wait();
    ctl.update_status(0, statustxt);

    if (m_object_idx == -1 || m_selected.empty()) return;

    update_arrange_params(params, m_plater->config(), m_selected);
    // arrange's own shrink first, then the user's fill-only edge / front margins on top.
    m_bedpts = apply_user_bed_margins(get_shrink_bedpts(m_plater->config(), params), params);

    auto &partplate_list               = m_plater->get_partplate_list();
    auto &print                        = wxGetApp().plater()->get_partplate_list().get_current_fff_print();
    const Slic3r::DynamicPrintConfig& global_config = wxGetApp().preset_bundle->full_config();
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    const bool is_bbl = wxGetApp().preset_bundle->is_bbl_vendor();
    if (is_bbl && params.avoid_extrusion_cali_region && global_config.opt_bool("scan_first_layer"))
        partplate_list.preprocess_nonprefered_areas(m_unselected, MAX_NUM_PLATES);
    
    update_selected_items_inflation(m_selected, m_plater->config(), params);
    update_unselected_items_inflation(m_unselected, m_plater->config(), params);

    // NO early stop. The old on_packed/do_stop pair aborted the whole pack the moment one
    // clone landed on plate 1 - with a greedy TOP_RIGHT start that usually happened while
    // space remained at the far corner, leaving a whole strip of the bed empty. The surplus
    // clones are simply discarded: finalize() skips every item with bed_idx != 0, and the
    // cloning setter only fires inside the bed_idx == 0 branch.
    params.stopcondition = [&ctl]() { return ctl.was_canceled(); };

    params.progressind = [this, &ctl, &statustxt](unsigned st,std::string str="") {
         if (st > 0)
             ctl.update_status(st * 100 / status_range(), statustxt + " " + str);
    };

    // final align用的是凸包，在有fixed item的情况下可能找到的参考点位置是错的，这里就不做了。见STUDIO-3265
    params.do_final_align = !is_bbl;

    // One grid implementation. The old "more than 100 items" branch tiled the bounding box with
    // Plater::get_empty_cells(), which used brim rather than the gap, never rotated, and checked
    // cells only against the plate's exclusion areas - not against the wipe tower or any other
    // object. run_grid_layout() does the job properly, and it is now reached by an explicit
    // user choice or by the Compact cap, not by an item count nobody set.
    if (m_settings.layout == fill_bed::Layout::Grid || m_fallback_to_grid)
        run_grid_layout();
    else
        arrangement::arrange(m_selected, m_unselected, m_bedpts, params);

    // finalize just here.
    ctl.update_status(100, ctl.was_canceled() ?
                                       _u8L("Bed filling canceled.") :
                                       _u8L("Bed filling done."));
}

FillBedJob::FillBedJob() : m_plater{wxGetApp().plater()} {}

FillBedJob::FillBedJob(const FillBedSettings &settings) : m_settings(settings), m_plater{wxGetApp().plater()} {}

std::vector<fill_bed::GridObstacle> FillBedJob::grid_obstacles(const arrangement::ArrangeParams &p) const
{
    // The clearance each obstacle needs is what update_unselected_items_inflation() would give
    // it: the exclusion gap for a virtual region, half the copy gap for a real object. That runs
    // in process(), long after prepare() builds the grid, so run it here on a copy - the items
    // process() will pack are left exactly as they are.
    ArrangePolygons probe = m_unselected;
    arrangement::ArrangeParams pp = p;
    update_unselected_items_inflation(probe, m_plater->config(), pp);

    std::vector<fill_bed::GridObstacle> out;
    out.reserve(probe.size());
    for (const ArrangePolygon &ap : probe) {
        // Only what is actually on this plate. Everything the plate does not hold was pushed to
        // m_locked in prepare(), and virtual objects carry bed_idx 0 like the rest.
        if (ap.bed_idx != 0)
            continue;
        fill_bed::GridObstacle ob;
        ob.outline = ap.transformed_poly().contour;
        // Its own clearance, never the copy-to-copy gap - section 4 of the spec: an exclusion
        // region keeps the exclusion rule, an extrusion-cali region keeps 0.
        ob.inflation = ap.inflation;
        out.emplace_back(std::move(ob));
    }
    return out;
}

size_t FillBedJob::run_grid_layout()
{
    // Nothing to place is a legal outcome - the user may have asked for a gap no copy fits at.
    size_t placed = 0;
    for (ArrangePolygon &ap : m_selected) {
        if (placed < m_grid_cells.size()) {
            const fill_bed::GridCell &cell = m_grid_cells[placed];
            ap.translation = Vec2crd(cell.translation.x(), cell.translation.y());
            ap.rotation    = cell.rotation;
            ap.bed_idx     = 0;
            ++placed;
        } else {
            // finalize() skips anything not on bed 0, and the cloning setter only fires inside
            // the bed_idx == 0 branch, so a surplus clone simply never comes into existence.
            ap.bed_idx = arrangement::UNARRANGED;
        }
    }
    return placed;
}

// The user's edge margin on all four sides and the front override on the min-Y edge, applied
// to a bed outline that get_shrink_bedpts has already shrunk. Each side only moves by what
// arrange has not already taken off, so margins of 0 are bit-identical to the old behaviour
// and the dialog can never place a copy CLOSER to the edge than arrange would.
Points FillBedJob::apply_user_bed_margins(const Points &bedpts, const arrangement::ArrangeParams &p) const
{
    return fill_bed::shrink_bed_per_side(bedpts,
                                         m_settings.edge_margin,
                                         m_settings.front_margin,
                                         m_settings.front_enabled,
                                         p.bed_shrink_x,
                                         p.bed_shrink_y);
}

void FillBedJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    // Ignore the arrange result if aborted.
    if (canceled || eptr)
        return;

    if (m_object_idx == -1) return;

    ModelObject *model_object = m_plater->model().objects[m_object_idx];
    if (model_object->instances.empty()) return;

    //BBS: partplate
    PartPlateList& plate_list = m_plater->get_partplate_list();
    int plate_cols = plate_list.get_plate_cols();
    int cur_plate = plate_list.get_curr_plate_index();

    size_t inst_cnt = model_object->instances.size();

    int added_cnt = std::accumulate(m_selected.begin(), m_selected.end(), 0, [](int s, auto &ap) {
        return s + int(ap.priority == 0 && ap.bed_idx == 0);
    });

    Model& model = m_plater->model();
    const size_t oldObjectCount = model.objects.size();

    if (added_cnt > 0) {
        //BBS: adjust the selected instances
        for (ArrangePolygon& ap : m_selected) {
            if (ap.bed_idx != 0) {
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":skipped: bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y));
                /*if (ap.itemid == -1)*/
                    continue;
                ap.bed_idx = plate_list.get_plate_count();
            }
            else
                ap.bed_idx = cur_plate;

            // The stride back onto the real plate, always. This used to be skipped above 100
            // items, because the ad-hoc branch that caught those took its cells from
            // Plater::get_empty_cells(), whose coordinates are already absolute. Both the NFP
            // packer and the Grid layout work in plate-local coordinates - the shrunk bed
            // outline - so both need it, and the item count has nothing to do with it. Without
            // this, filling plate 2 or later dropped every copy onto plate 1.
            ap.row = ap.bed_idx / plate_cols;
            ap.col = ap.bed_idx % plate_cols;
            ap.translation(X) += bed_stride_x(m_plater) * ap.col;
            ap.translation(Y) -= bed_stride_y(m_plater) * ap.row;

            ap.apply();

            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":selected: bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y));
        }

        const size_t newObjectCount = model.objects.size();
        if (newObjectCount > oldObjectCount)
        {
            ModelObjectPtrs newObjects(model.objects.begin() + oldObjectCount, model.objects.end());
            model.InitializeAssemblyPositions(newObjects);
        }

        auto obj_list = m_plater->sidebar().obj_list();
        for (size_t i = oldObjectCount; i < newObjectCount; i++) {
            obj_list->add_object_to_list(i, true, true, false);
            obj_list->update_printable_state(i, 0);
        }

        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ": paste_objects_into_list";

        /*for (ArrangePolygon& ap : m_selected) {
            if (ap.bed_idx != arrangement::UNARRANGED && (ap.priority != 0 || ap.bed_idx == 0))
                ap.apply();
        }*/

        //model_object->ensure_on_bed();
        //BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ": model_object->ensure_on_bed()";

        m_plater->update();
    }
}

int FillBedJob::grid_copies_for(Plater *plater, const FillBedSettings &settings)
{
    if (plater == nullptr)
        return 0;
    // prepare() is what knows how to collect the template, the obstacles and the bed; run it on a
    // throwaway job so the live label and the fill itself can never disagree. It touches only the
    // job's own members - no model change, no snapshot - and the clone list it builds is
    // discarded with the job.
    FillBedJob probe(settings);
    probe.m_estimate_only = true;
    probe.prepare();
    if (probe.m_object_idx == -1 || probe.m_selected.empty())
        return 0;
    return int(probe.m_grid_cells.size());
}

}} // namespace Slic3r::GUI
