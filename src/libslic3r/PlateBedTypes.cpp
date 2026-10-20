#include "PlateBedTypes.hpp"

#include <algorithm>

#include <boost/algorithm/string/predicate.hpp>

namespace Slic3r {
namespace PlateBedTypes {

bool is_snapmaker_u1_model(const std::string &printer_model)
{
    return boost::icontains(printer_model, "Snapmaker") && boost::icontains(printer_model, "U1");
}

static std::vector<BedType> u1_bed_types()
{
    // Single source: the U1 selector list in the curr_bed_type definition.
    std::vector<BedType>   out;
    const ConfigOptionDef *def = print_config_def.get("curr_bed_type");
    if (def == nullptr || def->enum_keys_map == nullptr)
        return out;
    for (const std::string &key : def->enum_values_u1) {
        auto it = def->enum_keys_map->find(key);
        if (it != def->enum_keys_map->end())
            out.push_back(BedType(it->second));
    }
    return out;
}

Allowed allowed_bed_types(const DynamicPrintConfig &printer_config, bool is_bbl_vendor)
{
    Allowed allowed;
    const auto *multi = printer_config.option<ConfigOptionBool>("support_multi_bed_types");
    if (is_bbl_vendor || (multi != nullptr && multi->value)) {
        allowed.any = true;
        return allowed;
    }
    const auto *model = printer_config.option<ConfigOptionString>("printer_model");
    if (model != nullptr && is_snapmaker_u1_model(model->value))
        allowed.types = u1_bed_types();
    return allowed;
}

bool is_allowed(const Allowed &allowed, BedType type)
{
    if (type == btDefault || allowed.any)
        return true;
    return std::find(allowed.types.begin(), allowed.types.end(), type) != allowed.types.end();
}

bool normalize(std::vector<BedType> &plate_bed_types, const Allowed &allowed)
{
    bool reset = false;
    for (BedType &type : plate_bed_types)
        if (!is_allowed(allowed, type)) {
            type  = btDefault;
            reset = true;
        }
    return reset;
}

} // namespace PlateBedTypes
} // namespace Slic3r
