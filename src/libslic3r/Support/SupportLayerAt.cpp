#include "SupportLayerAt.hpp"

#include "../ClipperUtils.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace support_layer_util {

SupportLayer *support_layer_find(PrintObject &object, const Layer &layer)
{
    SupportLayerPtrs &sls = object.support_layers();
    auto it = std::lower_bound(sls.begin(), sls.end(), layer.print_z - EPSILON,
                               [](const SupportLayer *l, double z) { return l->print_z < z; });
    return it != sls.end() && std::abs((*it)->print_z - layer.print_z) < EPSILON ? *it : nullptr;
}

SupportLayer *support_layer_at(PrintObject &object, const Layer &layer, bool &inserted)
{
    if (SupportLayer *sl = support_layer_find(object, layer))
        return sl;
    SupportLayerPtrs &sls = object.support_layers();
    auto it = std::lower_bound(sls.begin(), sls.end(), layer.print_z - EPSILON,
                               [](const SupportLayer *l, double z) { return l->print_z < z; });
    it = object.insert_support_layer(it, 0, 0, layer.height, layer.print_z, layer.slice_z);
    (*it)->support_type = stInnerTree;
    inserted = true;
    return *it;
}

void relink_support_layers(PrintObject &object)
{
    SupportLayerPtrs &sls = object.support_layers();
    for (size_t i = 0; i < sls.size(); ++i) {
        sls[i]->set_id(i);
        sls[i]->lower_layer = i > 0 ? sls[i - 1] : nullptr;
        sls[i]->upper_layer = i + 1 < sls.size() ? sls[i + 1] : nullptr;
    }
}

double support_layer_bottom(const SupportLayer &layer)
{
    const double h = std::max({ double(layer.height), layer.base_slab_height, layer.interface_slab_height });
    return layer.print_z - h;
}

ExPolygons support_coverage(const PrintObject &object, double bottom_z, double top_z)
{
    ExPolygons out;
    for (const SupportLayer *sl : object.support_layers()) {
        if (sl->print_z <= bottom_z + EPSILON)
            continue;
        // Sorted by print_z, but a later merged slab can reach further down: no early exit.
        if (support_layer_bottom(*sl) >= top_z - EPSILON)
            continue;
        expolygons_append(out, sl->support_islands);
        expolygons_append(out, sl->tree_support_areas());
    }
    return out.empty() ? out : union_ex(out);
}

} // namespace support_layer_util
} // namespace Slic3r
