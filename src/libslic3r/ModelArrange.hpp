#ifndef MODELARRANGE_HPP
#define MODELARRANGE_HPP

#include <libslic3r/Arrange.hpp>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r {
using ModelInstancePtrs = std::vector<ModelInstance*>;

using arrangement::ArrangePolygon;
using arrangement::ArrangePolygons;
using arrangement::ArrangeParams;
using arrangement::InfiniteBed;
using arrangement::CircleBed;

// Do something with ArrangePolygons in virtual beds
using VirtualBedFn = std::function<void(arrangement::ArrangePolygon&)>;

[[noreturn]] inline void throw_if_out_of_bed(arrangement::ArrangePolygon&) 
{
    throw Slic3r::RuntimeError("Objects could not fit on the bed");
}

// Ultra: whether support material would be generated for the instance at all - the automatic kinds
// need an overhang steeper than the threshold that is not the base on the bed, the manual kinds a
// painted enforcer. Drives the arrange/fill-bed support clearance; a flat print gets none.
bool instance_may_get_support(const ModelInstance &instance, SupportType support_type, int threshold_deg);

ArrangePolygons get_arrange_polys(const Model &model, ModelInstancePtrs &instances);
ArrangePolygon  get_arrange_poly(const Model &model);
bool apply_arrange_polys(ArrangePolygons &polys, ModelInstancePtrs &instances, VirtualBedFn);

void duplicate(Model &model, ArrangePolygons &copies, VirtualBedFn);
void duplicate_objects(Model &model, size_t copies_num);

template<class TBed>
bool arrange_objects(Model &              model,
                     const TBed &         bed,
                     const ArrangeParams &params,
                     VirtualBedFn         vfn = throw_if_out_of_bed)
{
    ModelInstancePtrs instances;
    auto&& input = get_arrange_polys(model, instances);
    arrangement::arrange(input, bed, params);
    
    return apply_arrange_polys(input, instances, vfn);
}

template<class TBed>
void duplicate(Model &              model,
               size_t               copies_num,
               const TBed &         bed,
               const ArrangeParams &params,
               VirtualBedFn         vfn = throw_if_out_of_bed)
{
    ArrangePolygons copies(copies_num, get_arrange_poly(model));
    arrangement::arrange(copies, bed, params);
    duplicate(model, copies, vfn);
}

template<class TBed>
void duplicate_objects(Model &              model,
                       size_t               copies_num,
                       const TBed &         bed,
                       const ArrangeParams &params,
                       VirtualBedFn         vfn = throw_if_out_of_bed)
{
    duplicate_objects(model, copies_num);
    arrange_objects(model, bed, params, vfn);
}

template<class T> struct PtrWrapper
{
    T* ptr;

    explicit PtrWrapper(T* p) : ptr{ p } {}

    arrangement::ArrangePolygon get_arrange_polygon(const Slic3r::DynamicPrintConfig &config = Slic3r::DynamicPrintConfig()) const
    {
        arrangement::ArrangePolygon ap;
        ptr->get_arrange_polygon(&ap, config);
        return ap;
    }

    void apply_arrange_result(const Vec2d& t, double rot, int item_id)
    {
        ptr->apply_arrange_result(t, rot);
        ptr->arrange_order = item_id;
    }
};

template<class T>
arrangement::ArrangePolygon get_arrange_poly(T obj, const DynamicPrintConfig &config = DynamicPrintConfig());

template<>
arrangement::ArrangePolygon get_arrange_poly(ModelInstance* inst, const DynamicPrintConfig& config);

ArrangePolygon get_instance_arrange_poly(ModelInstance* instance, const DynamicPrintConfig& config);

class MixedFilamentManager;

// Per-object prime towers (tests/design_per_object_prime_towers.md 2.5 and 4): printing by object with
// prime_tower_per_object on, a multi-colour copy needs room beside it for its own tower. Grows the arrange item `ap` of a
// copy of `object` into its unit, the convex hull of the copy and its estimated tower (brim included) on one side, so
// arrange packs copy + tower as one item and keeps the clearance between units, as Print::plan_object_towers() needs.
// The tower stands where the object's prime_tower_offset_x/y put it, else at the middle of the side (right of, or
// behind, the copy) that makes the smaller unit. `config` is the full config; `mixed` expands mixed filament ids (GUI).
// Call only when the plate prints by object. False (ap unchanged) when the copy needs no tower: the setting or the
// prime tower off, the object opted out (object_prime_tower), one filament, or no unit that fits the bed.
bool reserve_object_prime_tower(ArrangePolygon &ap, const ModelObject &object, const DynamicPrintConfig &config,
                                const MixedFilamentManager *mixed = nullptr);
}

#endif // MODELARRANGE_HPP
