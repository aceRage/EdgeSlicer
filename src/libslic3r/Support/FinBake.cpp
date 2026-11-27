// The fin bake (see FinBake.hpp). The technique is Slant3D's designed-in supports, as specified by Support
// Fins (gittrahan/support-fins, printfins.com, MIT); this is an independent native implementation.

#include "FinBake.hpp"

#include "../ClipperUtils.hpp"
#include "../Model.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../TriangleSelector.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>

namespace Slic3r {

double fin_bake_gap(FinBakePlacement placement, double sliced_gap)
{
    const double gap = std::max(0., sliced_gap);
    return placement == FinBakePlacement::PartOfObject ? std::max(gap, FIN_BAKE_PART_MIN_GAP) : gap;
}

double fin_bake_tine_width(FinBakeTines tines, double line_width)
{
    return tines == FinBakeTines::Wide ? FIN_BAKE_WIDE_TINE_LINES * line_width : line_width;
}

FinBakeResult plan_fin_bake(const PrintObject &object, const FinBakeOptions &options)
{
    FinBakeResult            res;
    const PrintObjectConfig &cfg = object.config();
    if (object.layers().size() < 2) {
        res.error = "the object is not sliced";
        return res;
    }
    // The fins stand on the bed, which a raft is not (the live generator refuses a raft too).
    if (object.has_raft()) {
        res.error = "objects printed on a raft are not supported";
        return res;
    }
    const fins::FinSettings st = fins::settings_of(object);
    if (! st.painted_fins()) {
        res.error = "support fins are off for this object";
        return res;
    }

    // Exactly the settings of the slice, so the bake is the fins the preview showed. The one exception is
    // a part's gap, which never goes below FIN_BAKE_PART_MIN_GAP: the fins are planned with the slice's
    // settings and only sliced with the part's gap.
    fins::FinSettings mesh_st = st;
    mesh_st.gap          = fin_bake_gap(options.placement, st.gap);
    res.sliced_gap       = st.gap;
    res.gap              = mesh_st.gap;
    res.tine_width       = fin_bake_tine_width(options.tines, st.line_width);
    res.layer_height     = cfg.layer_height.value;
    res.support_filament = cfg.support_filament.value;

    const std::vector<fins::FinLayer> layers = fins::layers_of(object);
    res.plan = fins::plan_fins(layers, st, fins::painted_fin_spots(object), &res.report);
    if (res.plan.fins.empty()) {
        res.error = res.report.painted_without_paint ? "no fin spots are painted on this object" : "no fin fits this object with its settings";
        return res;
    }

    // What the live generator keeps out of, less the other support (see FinBake.hpp): support blocker
    // volumes, and painted blockers remove the fins they touch.
    const size_t            n = layers.size();
    std::vector<ExPolygons> obstacles(n), blockers(n);
    {
        const std::vector<Polygons> volumes = object.slice_support_blockers();
        for (size_t i = 0; i < n && i < volumes.size(); ++i)
            if (! volumes[i].empty())
                obstacles[i] = union_ex(volumes[i]);
        std::vector<Polygons> painted;
        object.project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER, painted);
        for (size_t i = 0; i < n && i < painted.size(); ++i)
            if (! painted[i].empty())
                blockers[i] = union_ex(painted[i]);
    }
    // The planner's counts (lean, painted spots) stay; the slicer adds its own.
    res.slices = fins::slice_fins(layers, mesh_st, res.plan, &obstacles, &blockers, &res.report, {});

    fins::FinTineOptions tines;
    tines.width        = res.tine_width;
    tines.stop_at_part = options.placement == FinBakePlacement::SeparateObject;
    res.bands          = fins::fin_mesh_layers(layers, res.slices, mesh_st, tines);
    for (size_t i = 0; i < n; ++i) {
        if (! res.slices[i].tines.empty())
            res.tines += res.slices[i].tines.size();
        if (! res.bands[i].sections.empty())
            res.fin_top = std::max(res.fin_top, res.bands[i].top_z);
    }
    if (res.fin_top <= 0.)
        res.error = "every fin was removed (support blockers, or no room beside the part)";
    return res;
}

FinBakeResult bake_fins(const PrintObject &object, const FinBakeOptions &options)
{
    FinBakeResult res = plan_fin_bake(object, options);
    if (! res.error.empty())
        return res;

    res.mesh = fins::fin_mesh(res.bands, {}, &res.mesh_report);
    if (res.mesh.indices.empty()) {
        res.error = "the fin mesh came out empty";
        return res;
    }

    // The mesh is in the sliced frame: XY around the object's centre (print coordinates less the instance
    // shift), Z above the bed. The same frames as the stabilizer bake (StabilizerBake.cpp).
    if (options.placement == FinBakePlacement::SeparateObject) {
        const Vec2d c = unscaled(object.center_offset());
        for (Vec3f &v : res.mesh.vertices)
            v = Vec3f(v.x() + float(c.x()), v.y() + float(c.y()), v.z());
        for (const PrintInstance &pi : object.instances()) {
            const Point off(pi.shift.x() - object.center_offset().x(), pi.shift.y() - object.center_offset().y());
            const Vec2d o = unscaled(off);
            res.instance_offsets.emplace_back(o.x(), o.y(), 0.);
        }
    } else
        // The source object's own coordinates: undo trafo_centered() (instance rotation, scale and Z).
        its_transform(res.mesh, object.trafo_centered().inverse(), true);

    BOOST_LOG_TRIVIAL(info) << "Fin bake: " << res.plan.fins.size() << " fins, " << res.tines << " tines, "
                            << res.mesh_report.prisms << " prisms, " << res.mesh_report.triangles << " triangles, "
                            << (res.mesh_report.unioned ? "unioned" : "overlapping prisms") << ", "
                            << (res.mesh_report.closed ? "closed" : "NOT closed") << ", gap " << res.gap << " mm, tine width "
                            << res.tine_width << " mm";
    return res;
}

std::string fin_bake_object_name(const ModelObject &source)
{
    return (source.name.empty() ? std::string("object") : source.name) + " fins";
}

ModelObject *apply_fin_bake(Model &model, ModelObject &source, const FinBakeResult &result, const FinBakeOptions &options)
{
    if (result.mesh.indices.empty())
        return nullptr;

    ModelObject *target = nullptr;
    if (options.placement == FinBakePlacement::SeparateObject) {
        // The filament: the source's support filament when it has one, else whatever the source prints with.
        const int source_extruder = source.config.has("extruder") ? source.config.opt_int("extruder") : 0;
        const int extruder        = result.support_filament > 0 ? result.support_filament : source_extruder;

        ModelObject *obj = model.add_object();
        obj->name        = fin_bake_object_name(source);
        ModelVolume *vol = obj->add_volume(TriangleMesh(result.mesh));
        vol->name        = obj->name;
        // Settings that make it print like a support wall - and that Orca and a Bambu-compatible export
        // keep. Bambu Studio drops the EdgeSlicer-only ones; the geometry prints there on defaults.
        obj->config.set_key_value("extruder", new ConfigOptionInt(extruder));
        obj->config.set_key_value("enable_support", new ConfigOptionBool(false));
        obj->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
        // Never fins on the fins, nor stabilizers.
        obj->config.set_key_value("support_fins", new ConfigOptionEnum<SupportFinsMode>(sfmOff));
        obj->config.set_key_value("stabilizer_supports", new ConfigOptionEnum<StabilizerMode>(smOff));
        // The tines are one layer of the source: slice the fins at the same height.
        if (result.layer_height > 0.)
            obj->config.set_key_value("layer_height", new ConfigOptionFloat(result.layer_height));
        if (result.instance_offsets.empty())
            obj->add_instance();
        for (const Vec3d &offset : result.instance_offsets) {
            ModelInstance *inst = obj->add_instance();
            // Rotation and scale are in the vertices already; only the position is the instance's.
            inst->set_offset(offset);
            inst->set_rotation(Vec3d::Zero());
            inst->set_scaling_factor(Vec3d::Ones());
            inst->set_mirror(Vec3d::Ones());
        }
        obj->invalidate_bounding_box();
        target = obj;
    } else {
        // No settings of its own: the fins share the object's region, so the tines are one with the part's
        // perimeters (a part with its own walls, infill or filament would be a region of its own).
        ModelVolume *vol = source.add_volume(TriangleMesh(result.mesh), ModelVolumeType::MODEL_PART);
        vol->name        = FIN_BAKE_PART_NAME;
        source.invalidate_bounding_box();
        target = &source;
    }

    // Not twice: the source would print its live fins beside the baked ones. Only the switch goes; the
    // other fin settings stay for a later bake.
    source.config.set_key_value("support_fins", new ConfigOptionEnum<SupportFinsMode>(sfmOff));
    return target;
}

} // namespace Slic3r
