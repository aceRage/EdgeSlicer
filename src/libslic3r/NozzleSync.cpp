#include "NozzleSync.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Slic3r {
namespace NozzleSync {

namespace {
std::string trim(std::string s)
{
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

// One value of product_info.nozzle_diameter as one of the four known sizes, or empty.
std::string known_size(const nlohmann::json &value)
{
    static const char *sizes[] = { "0.2", "0.4", "0.6", "0.8" };
    if (value.is_number()) {
        const double d = value.get<double>();
        for (const char *s : sizes)
            if (std::fabs(d - std::atof(s)) < 1e-6)
                return s;
    } else if (value.is_string()) {
        const std::string s = value.get<std::string>();
        for (const char *known : sizes)
            if (s == known)
                return s;
    }
    return std::string();
}
} // namespace

std::vector<std::string> parse_reported_nozzles(const nlohmann::json &reported)
{
    std::vector<std::string> out;
    if (reported.is_array()) {
        for (const auto &value : reported)
            if (std::string s = known_size(value); !s.empty())
                out.push_back(std::move(s));
    } else if (std::string s = known_size(reported); !s.empty()) {
        out.push_back(std::move(s));
    }
    return out;
}

double parse_reported_diameter(const std::string &text)
{
    std::string s = trim(text);
    if (s.size() >= 2 && (s.compare(s.size() - 2, 2, "mm") == 0 || s.compare(s.size() - 2, 2, "MM") == 0))
        s = trim(s.substr(0, s.size() - 2));
    if (s.empty())
        return 0.;
    char *end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == nullptr || *end != '\0' || !(v > 0.) || v > 2.)
        return 0.;
    return v;
}

std::string variant_name(double diameter)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", diameter);
    std::string s = buf;
    while (!s.empty() && s.back() == '0')
        s.pop_back();
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    return s;
}

Plan plan(const std::vector<std::string> &reported)
{
    Plan p;
    p.per_head.reserve(reported.size());
    for (const std::string &r : reported)
        p.per_head.push_back(parse_reported_diameter(r));
    double first = 0.;
    bool   any = false, differs = false;
    for (double d : p.per_head) {
        if (d <= 0.)
            continue;
        if (!any) {
            first = d;
            any   = true;
        } else if (std::fabs(d - first) > 1e-6) {
            differs = true;
        }
    }
    if (any && !differs) {
        p.uniform = true;
        p.variant = variant_name(first);
    }
    return p;
}

std::vector<double> apply_per_head(const std::vector<double> &current, const std::vector<double> &reported_per_head, bool *changed)
{
    std::vector<double> out = current;
    bool                any = false;
    const size_t        count = std::min(out.size(), reported_per_head.size());
    for (size_t i = 0; i < count; ++i) {
        const double v = reported_per_head[i];
        if (v <= 0. || v > 2.)
            continue;
        if (std::fabs(out[i] - v) > 1e-6) {
            out[i] = v;
            any    = true;
        }
    }
    if (changed != nullptr)
        *changed = any;
    return out;
}

std::string match_machine_variant(const std::vector<double> &reported, const std::vector<MachineVariant> &machines)
{
    if (std::none_of(reported.begin(), reported.end(), [](double d) { return d > 0.; }))
        return std::string();
    for (const MachineVariant &m : machines) {
        if (m.nozzles.size() != reported.size())
            continue;
        bool ok = true;
        for (size_t i = 0; i < reported.size() && ok; ++i)
            ok = reported[i] <= 0. || std::fabs(reported[i] - m.nozzles[i]) < 1e-6;
        if (ok)
            return m.variant;
    }
    return std::string();
}

std::vector<std::string> families(const std::vector<FilamentChoice> &choices)
{
    std::vector<std::string> out;
    for (const FilamentChoice &c : choices)
        if (std::find(out.begin(), out.end(), c.family) == out.end())
            out.push_back(c.family);
    return out;
}

std::vector<SlotAssignment> assign_family_to_slots(const std::vector<FilamentChoice> &choices, const std::string &family,
                                                   const std::vector<double> &slot_nozzles)
{
    std::vector<SlotAssignment> out(slot_nozzles.size());
    for (size_t i = 0; i < slot_nozzles.size(); ++i) {
        const double          wanted = slot_nozzles[i];
        const FilamentChoice *exact = nullptr, *any = nullptr, *first = nullptr;
        for (const FilamentChoice &c : choices) {
            if (c.family != family)
                continue;
            if (first == nullptr)
                first = &c;
            if (wanted > 0. && c.nozzle > 0. && std::fabs(c.nozzle - wanted) < 1e-6 && exact == nullptr)
                exact = &c;
            if (c.nozzle <= 0. && any == nullptr)
                any = &c;
        }
        const FilamentChoice *pick = wanted <= 0. ? first : (exact != nullptr ? exact : any);
        if (pick != nullptr)
            out[i].preset = pick->name;
        else
            out[i].skipped = true;
    }
    return out;
}

std::string first_nozzle(const std::vector<std::string> &reported)
{
    return reported.empty() ? std::string() : reported.front();
}

std::string device_preset_name(const std::string &model, const std::vector<std::string> &reported, const std::string &current)
{
    if (reported.empty() || reported.front().empty())
        return current;
    return model + " (" + reported.front() + " nozzle)";
}

} // namespace NozzleSync
} // namespace Slic3r
