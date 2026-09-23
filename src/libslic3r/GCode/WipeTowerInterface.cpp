#include "WipeTowerInterface.hpp"

#include "../Config.hpp"
#include "../PrintConfig.hpp"

#include <boost/algorithm/string.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {
namespace TowerInterface {

namespace {

// A key the config does not hold takes its declared default.
const ConfigOption *option_of(const ConfigBase &config, const char *key)
{
    if (const ConfigOption *opt = config.option(key); opt != nullptr)
        return opt;
    if (const ConfigDef *def = config.def(); def != nullptr)
        if (const ConfigOptionDef *opt_def = def->get(key); opt_def != nullptr)
            return opt_def->default_value.get();
    return nullptr;
}

bool opt_bool(const ConfigBase &config, const char *key)
{
    const ConfigOption *opt = option_of(config, key);
    return opt != nullptr && opt->getBool();
}

bool starts_with_any(const std::string &s, std::initializer_list<const char *> prefixes)
{
    for (const char *p : prefixes)
        if (boost::starts_with(s, p))
            return true;
    return false;
}

} // namespace

std::string material_family(const std::string &filament_type, bool is_support)
{
    std::string t = boost::to_upper_copy(boost::trim_copy(filament_type));
    std::string family;
    if (t == "PVA" || t == "BVOH")
        family = "PVA";
    else if (t == "PCTG" || boost::starts_with(t, "PET"))
        family = "PETG";
    else if (boost::starts_with(t, "PLA"))
        family = "PLA";
    else if (starts_with_any(t, { "ABS", "ASA" }))
        family = "ABS";
    else if (starts_with_any(t, { "PA", "PPA" }))
        family = "PA";
    else if (t == "PC" || boost::starts_with(t, "PC-") || boost::starts_with(t, "PC/"))
        family = "PC";
    else if (starts_with_any(t, { "TPU", "TPE", "FLEX" }))
        family = "TPU";
    else {
        const size_t dash = t.find('-');
        family = dash == std::string::npos ? t : t.substr(0, dash);
    }
    // Soluble and breakaway support materials are their own family already.
    if (is_support && family != "PVA" && family != "HIPS")
        family = "SUPPORT-" + family;
    return family;
}

bool triggers(int trigger, const FilamentKind &from, const FilamentKind &to)
{
    switch (trigger) {
    case titEveryToolChange:
        return true;
    case titMaterialFamilyChange:
        return material_family(from.type, from.is_support) != material_family(to.type, to.is_support);
    case titMaterialChange:
    default:
        return from.is_support != to.is_support ||
               ! boost::iequals(boost::trim_copy(from.type), boost::trim_copy(to.type));
    }
}

int interface_temperature(int configured, int range_high, int normal)
{
    const int t = configured == -1 ? range_high : configured;
    return t > 0 ? t : normal;
}

Settings Settings::from_config(const ConfigBase &config)
{
    Settings s;
    s.temp        = opt_bool(config, "wipe_tower_interface_temp");
    s.run_in      = opt_bool(config, "wipe_tower_interface_run_in") && opt_bool(config, "wipe_tower_wall_gap");
    s.extra_prime = opt_bool(config, "wipe_tower_interface_extra_prime");
    if (const ConfigOption *opt = option_of(config, "wipe_tower_interface_trigger"); opt != nullptr)
        s.trigger = opt->getInt();
    return s;
}

FilamentKind filament_kind(const ConfigBase &config, unsigned int filament)
{
    FilamentKind kind;
    if (const auto *types = dynamic_cast<const ConfigOptionStrings *>(option_of(config, "filament_type")); types != nullptr && ! types->values.empty())
        kind.type = types->get_at(filament);
    if (const auto *support = dynamic_cast<const ConfigOptionBools *>(option_of(config, "filament_is_support")); support != nullptr && ! support->values.empty())
        kind.is_support = support->get_at(filament);
    return kind;
}

float run_in_distance(const ConfigBase &config, unsigned int filament)
{
    const auto *dist = dynamic_cast<const ConfigOptionFloats *>(option_of(config, "filament_tower_interface_pre_extrusion_dist"));
    return dist == nullptr || dist->values.empty() ? 0.f : std::max(0.f, float(dist->get_at(filament)));
}

double run_in_reserve(const Settings &settings, const std::vector<FilamentKind> &kinds, const std::vector<float> &distances,
                      const std::vector<unsigned int> &filaments, double line_width)
{
    if (! settings.run_in)
        return 0.;
    double longest = 0.;
    for (unsigned int from : filaments)
        for (unsigned int to : filaments)
            if (from != to && from < kinds.size() && to < kinds.size() && to < distances.size() &&
                triggers(settings.trigger, kinds[from], kinds[to]))
                longest = std::max(longest, double(distances[to]));
    return longest > 0. ? longest + line_width / 2. : 0.;
}

double run_in_reserve(const ConfigBase &config, const std::vector<unsigned int> &filaments, double line_width)
{
    const Settings settings = Settings::from_config(config);
    if (! settings.run_in || filaments.empty())
        return 0.;
    const unsigned int        count = *std::max_element(filaments.begin(), filaments.end()) + 1;
    std::vector<FilamentKind> kinds;
    std::vector<float>        distances;
    for (unsigned int f = 0; f < count; ++f) {
        kinds.push_back(filament_kind(config, f));
        distances.push_back(run_in_distance(config, f));
    }
    return run_in_reserve(settings, kinds, distances, filaments, line_width);
}

BoundingBoxf shared_printable_box(const ConfigBase &config)
{
    BoundingBoxf box;
    if (const auto *area = dynamic_cast<const ConfigOptionPoints *>(option_of(config, "printable_area")); area != nullptr && ! area->values.empty())
        box = BoundingBoxf(area->values);
    if (const auto *groups = dynamic_cast<const ConfigOptionPointsGroups *>(config.option("extruder_printable_area")); groups != nullptr && box.defined)
        for (const auto &group : groups->values) {
            if (group.size() < 3)
                continue;
            const BoundingBoxf g(group);
            box.min = box.min.cwiseMax(g.min);
            box.max = box.max.cwiseMin(g.max);
        }
    return box;
}

float clamp_to_bed(const Vec2f &from, const Vec2f &dir, float dist, const std::function<Vec2f(const Vec2f &)> &to_bed, const BoundingBoxf &bed)
{
    if (! bed.defined || dist <= 0.f)
        return std::max(0.f, dist);
    const Vec2d b0   = to_bed(from).cast<double>();
    const Vec2d step = to_bed(from + dir).cast<double>() - b0; // per unit of t
    double      t    = dist;
    for (int axis = 0; axis < 2; ++axis) {
        if (b0[axis] < bed.min[axis] - EPSILON || b0[axis] > bed.max[axis] + EPSILON)
            return 0.f; // the tower edge itself is off the bed: no run-in
        if (step[axis] > EPSILON)
            t = std::min(t, (bed.max[axis] - b0[axis]) / step[axis]);
        else if (step[axis] < -EPSILON)
            t = std::min(t, (bed.min[axis] - b0[axis]) / step[axis]);
    }
    return float(std::max(0., t));
}

} // namespace TowerInterface
} // namespace Slic3r
