#include "ModelArrange.hpp"

#include <libslic3r/Model.hpp>
#include <libslic3r/Geometry/ConvexHull.hpp>
#include <libslic3r/Print.hpp>
#include <libslic3r/MixedFilament.hpp>
#include <libslic3r/GCode/WipeTowerEstimate.hpp>
#include "MTUtils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

// Ultra: would support material be generated for this instance at all? The automatic kinds attach to
// facets that face down more steeply than the threshold (measured from the horizontal, as the setting
// is) and are not the object's own base on the bed; the manual kinds attach only where the user painted
// an enforcer. A flat print says no on both counts, and arrange then owes it no support clearance.
// Facet-level and in world space, so a rotated instance is judged as it will print.
bool instance_may_get_support(const ModelInstance &instance, SupportType support_type, int threshold_deg)
{
    const ModelObject *object = instance.get_object();
    if (object == nullptr)
        return false;
    const bool automatic = support_type == stNormalAuto || support_type == stTreeAuto;
    if (! automatic) {
        for (const ModelVolume *mv : object->volumes)
            if (mv->is_model_part() && ! mv->supported_facets.empty())
                return true;
        return false;
    }
    // A downward-facing facet whose slope from the horizontal is below the threshold is an overhang:
    // slope = acos(-nz), so overhang <=> -nz > cos(threshold). Threshold 0 means "auto" in the UI;
    // the slicer's own default of 30 deg stands in for it here.
    const double thr     = threshold_deg > 0 ? double(threshold_deg) : 30.0;
    const double cos_thr = std::cos(thr * M_PI / 180.0);
    const Transform3d inst_tr = instance.get_matrix();
    // The object's lowest point in world space: a facet lying on the bed is the base, not an overhang.
    double z_min = std::numeric_limits<double>::max();
    std::vector<std::pair<const ModelVolume *, Transform3d>> parts;
    for (const ModelVolume *mv : object->volumes) {
        if (! mv->is_model_part())
            continue;
        const Transform3d tr = inst_tr * mv->get_matrix();
        for (const Vec3f &v : mv->mesh().its.vertices)
            z_min = std::min(z_min, (tr * v.cast<double>()).z());
        parts.emplace_back(mv, tr);
    }
    const double bed_eps = 0.05;
    for (const auto &[mv, tr] : parts) {
        const indexed_triangle_set &its = mv->mesh().its;
        for (const Vec3i32 &f : its.indices) {
            const Vec3d a = tr * its.vertices[f(0)].cast<double>();
            const Vec3d b = tr * its.vertices[f(1)].cast<double>();
            const Vec3d c = tr * its.vertices[f(2)].cast<double>();
            const Vec3d n = (b - a).cross(c - a);
            const double l = n.norm();
            if (l <= 1e-12)
                continue;
            if (-n.z() / l <= cos_thr)
                continue; // not facing down steeply enough to need support
            if (std::max({a.z(), b.z(), c.z()}) <= z_min + bed_eps)
                continue; // the base on the bed
            return true;
        }
    }
    return false;
}

} // namespace Slic3r

namespace Slic3r {

arrangement::ArrangePolygons get_arrange_polys(const Model &model, ModelInstancePtrs &instances)
{
    size_t count = 0;
    for (auto obj : model.objects) count += obj->instances.size();

    ArrangePolygons input;
    input.reserve(count);
    instances.clear(); instances.reserve(count);
    ArrangePolygon ap;
    for (ModelObject *mo : model.objects)
        for (ModelInstance *minst : mo->instances) {
            minst->get_arrange_polygon(&ap);
            input.emplace_back(ap);
            instances.emplace_back(minst);
        }

    return input;
}

bool apply_arrange_polys(ArrangePolygons &input, ModelInstancePtrs &instances, VirtualBedFn vfn)
{
    bool ret = true;

    for(size_t i = 0; i < input.size(); ++i) {
        if (input[i].bed_idx != 0) { ret = false; if (vfn) vfn(input[i]); }
        if (input[i].bed_idx >= 0)
            instances[i]->apply_arrange_result(input[i].translation.cast<double>(),
                                               input[i].rotation);
    }

    return ret;
}

Slic3r::arrangement::ArrangePolygon get_arrange_poly(const Model &model)
{
    ArrangePolygon ap;
    Points &apts = ap.poly.contour.points;
    for (const ModelObject *mo : model.objects)
        for (const ModelInstance *minst : mo->instances) {
            ArrangePolygon obj_ap;
            minst->get_arrange_polygon(&obj_ap);
            ap.poly.contour.rotate(obj_ap.rotation);
            ap.poly.contour.translate(obj_ap.translation.x(), obj_ap.translation.y());
            const Points &pts = obj_ap.poly.contour.points;
            std::copy(pts.begin(), pts.end(), std::back_inserter(apts));
        }

    apts = std::move(Geometry::convex_hull(apts).points);
    return ap;
}

void duplicate(Model &model, Slic3r::arrangement::ArrangePolygons &copies, VirtualBedFn vfn)
{
    for (ModelObject *o : model.objects) {
        // make a copy of the pointers in order to avoid recursion when appending their copies
        ModelInstancePtrs instances = o->instances;
        o->instances.clear();
        for (const ModelInstance *i : instances) {
            for (arrangement::ArrangePolygon &ap : copies) {
                if (ap.bed_idx != 0) vfn(ap);
                ModelInstance *instance = o->add_instance(*i);
                Vec2d pos = unscale(ap.translation);
                instance->set_offset(instance->get_offset() + to_3d(pos, 0.));
            }
        }
        o->invalidate_bounding_box();
    }
}

void duplicate_objects(Model &model, size_t copies_num)
{
    for (ModelObject *o : model.objects) {
        // make a copy of the pointers in order to avoid recursion when appending their copies
        ModelInstancePtrs instances = o->instances;
        for (const ModelInstance *i : instances)
            for (size_t k = 2; k <= copies_num; ++ k)
                o->add_instance(*i);
    }
}

// Set up arrange polygon for a ModelInstance and Wipe tower
template<class T>
arrangement::ArrangePolygon get_arrange_poly(T obj, const Slic3r::DynamicPrintConfig& config)
{
    ArrangePolygon ap = obj.get_arrange_polygon(config);
    //BBS: always set bed_idx to 0 to use original transforms with no bed_idx
    //if this object is not arranged, it can keep the original transforms
    //ap.bed_idx        = ap.translation.x() / bed_stride_x(plater);
    ap.bed_idx = 0;
    ap.setter = [obj](const ArrangePolygon& p) {
        if (p.is_arranged()) {
            Vec2d t = p.translation.cast<double>();
            //BBS: change to sudoku-style computation, do it in partplate list
            //t.x() += p.bed_idx * bed_stride(plater);
            //t.x() += col * bed_stride_x(plater);
            //t.y() -= row * bed_stride_y(plater);
            T{ obj }.apply_arrange_result(t, p.rotation, p.itemid);
        }
    };

    return ap;
}

template<>
arrangement::ArrangePolygon get_arrange_poly(ModelInstance* inst, const Slic3r::DynamicPrintConfig& config)
{
    return get_arrange_poly(PtrWrapper{ inst },config);
}

ArrangePolygon get_instance_arrange_poly(ModelInstance* instance, const Slic3r::DynamicPrintConfig& config)
{
    ArrangePolygon ap = get_arrange_poly(PtrWrapper{ instance }, config);

    const unsigned int filament_id = ap.extrude_ids.empty() ? 0u : unsigned(std::max(0, ap.extrude_ids.front() - 1));

    //BBS: add temperature information
    if (config.has("curr_bed_type")) {
        ap.bed_temp = 0;
        ap.first_bed_temp = 0;
        BedType curr_bed_type = config.opt_enum<BedType>("curr_bed_type");

        const ConfigOptionInts* bed_opt = config.option<ConfigOptionInts>(get_bed_temp_key(curr_bed_type));
        if (bed_opt != nullptr)
            ap.bed_temp = bed_opt->get_at(filament_id);

        const ConfigOptionInts* bed_opt_1st_layer = config.option<ConfigOptionInts>(get_bed_temp_1st_layer_key(curr_bed_type));
        if (bed_opt_1st_layer != nullptr)
            ap.first_bed_temp = bed_opt_1st_layer->get_at(filament_id);
    }

    if (const auto *nozzle_temp = config.option<ConfigOptionInts>("nozzle_temperature"))
        ap.print_temp = get_value_at(config, *nozzle_temp, ConfigFlowDomain::Filament, filament_id);
    if (const auto *initial_temp = config.option<ConfigOptionInts>("nozzle_temperature_initial_layer"))
        ap.first_print_temp = get_value_at(config, *initial_temp, ConfigFlowDomain::Filament, filament_id);

    if (config.has("temperature_vitrification")) {
        ap.vitrify_temp = config.opt_int("temperature_vitrification", filament_id);
    }

    // get filament temp types
    auto* filament_types_opt = dynamic_cast<const ConfigOptionStrings*>(config.option("filament_type"));
    if (filament_types_opt) {
        std::set<int> filament_temp_types;
        for (auto i : ap.extrude_ids) {
            std::string type_str = filament_types_opt->get_at(i-1);
            int temp_type = Print::get_filament_temp_type(type_str);
            filament_temp_types.insert(temp_type);
        }
        ap.filament_temp_type = Print::get_compatible_filament_type(filament_temp_types);
    }

    // get brim width
    auto obj = instance->get_object();

    ap.brim_width = 1.0;
    // For by-layer printing, need to shrink bed a little, so the support won't go outside bed.
    // We set it to 5mm because that's how much a normal support will grow by default.
    // normal support 5mm, other support 22mm, no support 0mm
    auto supp_type_ptr = obj->get_config_value<ConfigOptionBool>(config, "enable_support");
    // Read the enum through the base option + virtual getInt(): DynamicConfig stores
    // enums as ConfigOptionEnumGeneric, so a typed downcast would be UB.
    const ConfigOption* support_type_ptr = obj->config.has("support_type") ?
        obj->config.option("support_type") : config.option("support_type");
    auto support_type = static_cast<SupportType>(support_type_ptr->getInt());
    auto enable_support = supp_type_ptr->getBool();
    int support_int = support_type_ptr->getInt();

    // Ultra: the clearance is for support material that will actually be printed. "Enable support"
    // is on in most people's global presets, and a flat print (nothing under the threshold angle,
    // nothing painted) generates none - yet every such object was still spaced 6 mm (24 mm with tree
    // support) apart, and Fill bed's gap floor blamed a brim for it (owner, 2026-09-16). So the
    // instance is scanned for what support would attach to first; with nothing to attach to it keeps
    // the flat 1 mm.
    const int threshold_deg = obj->get_config_value<ConfigOptionInt>(config, "support_threshold_angle")->getInt();
    if (enable_support && !instance_may_get_support(*instance, support_type, threshold_deg))
        enable_support = false;

    if (enable_support && (support_type == stNormalAuto || support_type == stNormal))
        ap.brim_width = 6.0;
    else if (enable_support) {
        ap.brim_width = 24.0; // 2*MAX_BRANCH_RADIUS_FIRST_LAYER
        ap.has_tree_support = true;
    }

    auto size = obj->instance_convex_hull_bounding_box(instance).size();
    ap.height = size.z();
    ap.name = obj->name;
    return ap;
}

// Per-object prime towers: the arrange item of a copy and its tower as one unit (see ModelArrange.hpp). Before, the item
// was grown on every side by the tower's whole size (on top of half the by-object clearance), so two towered copies
// already needed some 360 mm side by side on an X1C, and the arrange moved every copy but one onto new plates.
bool reserve_object_prime_tower(ArrangePolygon &ap, const ModelObject &object, const DynamicPrintConfig &config,
                                const MixedFilamentManager *mixed)
{
    auto global_bool = [&config](const char *key) {
        const ConfigOption *opt = config.option(key);
        return opt != nullptr && opt->getBool();
    };
    if (!global_bool("prime_tower_per_object") || !global_bool("enable_prime_tower") || ap.poly.contour.points.size() < 3)
        return false;
    // Design D13: an object taken out of the per-object towers needs no room for one.
    if (const ConfigOption *own = object.config.option("object_prime_tower"); own != nullptr && !own->getBool())
        return false;
    const auto  *diameters    = config.option<ConfigOptionFloats>("filament_diameter");
    const size_t num_physical = diameters != nullptr ? diameters->size() : 0;
    if (num_physical < 2)
        return false;

    // The object's own value of a key, else the global one.
    auto value_of = [&](const char *key) -> const ConfigOption * {
        const ConfigOption *opt = object.config.option(key);
        return opt != nullptr ? opt : config.option(key);
    };
    auto float_of = [&](const char *key, double fallback) {
        const auto *opt = dynamic_cast<const ConfigOptionFloat *>(value_of(key));
        return opt != nullptr ? opt->value : fallback;
    };

    // The filaments the copy prints with, from the model, as Print::plan_object_towers() takes them before slicing: the
    // parts (painting included), the height ranges, the feature filaments and the support's (1-based here).
    std::vector<int> ids;
    for (const ModelVolume *volume : object.volumes)
        if (volume->is_model_part())
            append(ids, volume->get_extruders());
    for (const auto &range : object.layer_config_ranges)
        if (const ConfigOption *opt = range.second.option("extruder"); opt != nullptr && opt->getInt() > 0)
            ids.push_back(opt->getInt());
    for (const char *key : { "wall_filament", "sparse_infill_filament", "solid_infill_filament" })
        if (const ConfigOption *opt = value_of(key); opt != nullptr && opt->getInt() > 0)
            ids.push_back(opt->getInt());
    if (const ConfigOption *support = value_of("enable_support"); support != nullptr && support->getBool())
        for (const char *key : { "support_filament", "support_interface_filament" })
            if (const ConfigOption *opt = value_of(key); opt != nullptr && opt->getInt() > 0)
                ids.push_back(opt->getInt());
    if (mixed != nullptr)
        mixed->expand_virtual_extruder_ids(ids, num_physical);
    std::vector<unsigned int> filaments;
    for (int id : ids)
        if (id > 0 && size_t(id) <= num_physical)
            filaments.push_back(unsigned(id - 1));
    sort_remove_duplicates(filaments);
    if (filaments.size() < 2)
        return false;

    const WipeTowerFootprint footprint = estimate_wipe_tower_footprint(config, resolve_wipe_tower_type(config), filaments,
                                                                       float_of("layer_height", 0.2), ap.height);
    if (footprint.depth <= EPSILON)
        return false;
    // The tower relative to its anchor (its front left body corner), brim included, as the planner lays it.
    const Polygon     local = placed_wipe_tower_footprint(config, footprint, Vec2d::Zero());
    const BoundingBox tb    = get_extents(local);
    // The copy as it stands on the plate (turned), relative to its position.
    Polygon hull = ap.poly.contour;
    hull.rotate(ap.rotation);
    const BoundingBox cb = get_extents(hull);

    // The room the planner keeps between a copy and its tower: the side scrape's reach and a little air, the copy's brim
    // and its own skirt; 1 mm more, so the side kept here is clear for the planner too.
    double scrape = 0.;
    if (global_bool("prime_tower_side_scrape"))
        scrape = std::max(0., (config.has("prime_tower_side_scrape_wall_gap") ? config.opt_float("prime_tower_side_scrape_wall_gap") : 0.) +
                                  (config.has("prime_tower_side_scrape_length") ? config.opt_float("prime_tower_side_scrape_length") : 0.));
    double brim = 0.;
    if (const ConfigOption *type = value_of("brim_type"); type != nullptr && type->getInt() != int(btNoBrim))
        brim = std::max(0., float_of("brim_width", 0.));
    double skirt = 0.;
    if (const ConfigOption *type = config.option("skirt_type"), *loops = config.option("skirt_loops");
        type != nullptr && loops != nullptr && type->getInt() == int(stPerObject) && loops->getInt() > 0)
        skirt = float_of("skirt_distance", 0.) + 0.5 * loops->getInt();
    const coord_t gap = scale_(std::max(3., scrape + 2.) + brim + skirt + 1.);

    // Where the tower goes: where the user dragged it (the offset from the copy's centre), else the middle of the right or
    // the back side, whichever makes the smaller unit (the planner tries both, and slides along every side).
    std::vector<Point> anchors;
    const Vec2d        drag(float_of("prime_tower_offset_x", 0.), float_of("prime_tower_offset_y", 0.));
    if (std::abs(drag.x()) > EPSILON || std::abs(drag.y()) > EPSILON)
        anchors.push_back(cb.center() + Point::new_scale(drag.x(), drag.y()));
    else {
        anchors.emplace_back(cb.max.x() + gap - tb.min.x(), cb.center().y() - tb.center().y());
        anchors.emplace_back(cb.center().x() - tb.center().x(), cb.max.y() + gap - tb.min.y());
    }
    Polygon best;
    double  best_area = std::numeric_limits<double>::max();
    for (const Point &anchor : anchors) {
        Points points = hull.points;
        for (const Point &p : local.points)
            points.push_back(p + anchor);
        Polygon      unit = Geometry::convex_hull(points);
        const double area = get_extents(unit).size().cast<double>().prod();
        if (area < best_area - EPSILON) {
            best_area = area;
            best      = std::move(unit);
        }
    }
    // A unit larger than the bed is never placed whole: the copy then prints without a tower, so it keeps its own size.
    const BoundingBox bed = get_extents(Polygon(get_bed_shape(config)));
    const Point       size = get_extents(best).size();
    if (best.points.size() < 3 || size.x() > bed.size().x() || size.y() > bed.size().y())
        return false;
    best.rotate(-ap.rotation);
    ap.poly.contour = std::move(best);
    ap.poly.holes.clear();
    return true;
}

} // namespace Slic3r
