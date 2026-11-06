#include "PresetManagerModel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <regex>
#include <unordered_map>

#include "Preset.hpp"
#include "PresetBundle.hpp"
#include "ProcessPresetExport.hpp"

namespace Slic3r {

namespace {

bool pm_is_nan(double v) { return std::isnan(v); }

// Case-insensitive, then exact (a stable order for names that differ only in case).
bool pm_less_name(const std::string &a, const std::string &b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int ca = std::tolower(static_cast<unsigned char>(a[i]));
        const int cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb)
            return ca < cb;
    }
    if (a.size() != b.size())
        return a.size() < b.size();
    return a < b;
}

std::string pm_first_string(const DynamicPrintConfig &config, const char *key)
{
    if (const auto *opt = config.option<ConfigOptionStrings>(key); opt != nullptr && !opt->values.empty())
        return opt->values.front();
    if (const auto *opt = config.option<ConfigOptionString>(key); opt != nullptr)
        return opt->value;
    return {};
}

// The first value of a numeric option (scalar or vector, nullable or not); NaN when absent or nil.
double pm_first_number(const DynamicPrintConfig &config, const std::string &key)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (key.empty())
        return nan;
    const ConfigOption *opt = config.option(key);
    if (opt == nullptr)
        return nan;
    if (const auto *v = dynamic_cast<const ConfigOptionVectorBase *>(opt)) {
        if (v->empty() || v->is_nil(0))
            return nan;
        if (const auto *ints = dynamic_cast<const ConfigOptionInts *>(opt))
            return double(ints->values.front());
        if (const auto *ints = dynamic_cast<const ConfigOptionIntsNullable *>(opt))
            return double(ints->values.front());
        if (const auto *floats = dynamic_cast<const ConfigOptionFloats *>(opt))
            return floats->values.front();
        if (const auto *floats = dynamic_cast<const ConfigOptionFloatsNullable *>(opt))
            return floats->values.front();
        return nan;
    }
    if (const auto *f = dynamic_cast<const ConfigOptionFloat *>(opt))
        return f->value;
    if (const auto *i = dynamic_cast<const ConfigOptionInt *>(opt))
        return double(i->value);
    return nan;
}

// 0.4 -> "0.4", 0.25 -> "0.25"
std::string pm_nozzle_text(double d)
{
    if (pm_is_nan(d) || d <= 0.)
        return {};
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", d);
    std::string s = buf;
    while (!s.empty() && s.back() == '0')
        s.pop_back();
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    return s;
}

void pm_sort_unique(std::vector<std::string> &v)
{
    std::sort(v.begin(), v.end(), pm_less_name);
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

double pm_first_known(double a, double b) { return pm_is_nan(a) ? b : a; }

nlohmann::json pm_number_or_null(double v)
{
    if (pm_is_nan(v))
        return nullptr;
    return v;
}

PresetManagerRow pm_row_of(const PresetManagerItem &item)
{
    PresetManagerRow row;
    row.favorite_key = item.favorite_key;
    row.name         = item.is_system && !item.alias.empty() ? item.alias : item.name;
    row.members      = {item.name};
    row.old_names    = item.renamed_from;
    row.vendor       = item.filament_vendor;
    row.type         = item.type;
    row.filament_id  = item.filament_id;
    row.is_system    = item.is_system;
    row.version      = item.version;
    row.printers     = item.printers;
    row.all_printers = item.all_printers;
    row.nozzles      = item.nozzles;
    row.nozzle_t     = item.nozzle_t;
    row.nozzle_lo    = item.nozzle_lo;
    row.nozzle_hi    = item.nozzle_hi;
    row.bed_t        = item.bed_t;
    row.layer_height = item.layer_height;
    row.inherits     = item.inherits;
    row.mtime        = item.is_system ? 0 : item.mtime;
    row.enabled      = item.is_system ? item.visible : true;
    row.in_use       = item.in_use;
    return row;
}

void pm_set_lock(PresetManagerRow &row)
{
    if (!row.is_system)
        row.lock = "user";
    else if (row.in_use && row.enabled)
        row.lock = "in_use";
    else
        row.lock.clear();
}

} // namespace

const PresetManagerRow *PresetManagerModel::find(const std::string &key) const
{
    for (const PresetManagerRow &row : rows)
        if (row.key == key)
            return &row;
    return nullptr;
}

std::string process_quality_tier(const std::string &alias)
{
    // "0.20mm Standard", "0.2mm Fine", ".12mm Fine @BBL X1C" (the printer part is cut first)
    static const std::regex re(R"(^\s*\d*\.?\d+\s*mm\s+(.*?)\s*$)", std::regex::icase);
    std::string base = alias;
    if (const size_t at = base.find(" @"); at != std::string::npos)
        base.erase(at);
    std::smatch m;
    if (!std::regex_match(base, m, re))
        return {};
    return m[1].str();
}

std::string preset_manager_favorite_key(const std::string &name, const std::string &alias, bool is_system)
{
    if (!is_system)
        return name;
    if (!alias.empty())
        return alias;
    std::string key = name;
    if (const size_t at = key.find(" @"); at != std::string::npos)
        key.erase(at);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
        key.pop_back();
    return key;
}

PresetManagerModel build_preset_manager_model(PresetManagerKind kind, const std::vector<PresetManagerItem> &items)
{
    PresetManagerModel model;
    model.kind = kind;

    std::vector<PresetManagerRow> system_rows, user_rows;
    if (kind == PresetManagerKind::Filament) {
        // System filaments: one row per vendor bundle + alias. Members in name order, so the values the
        // row shows come from the same member every time.
        std::vector<const PresetManagerItem *> sorted;
        for (const PresetManagerItem &item : items)
            sorted.push_back(&item);
        std::stable_sort(sorted.begin(), sorted.end(), [](const PresetManagerItem *a, const PresetManagerItem *b) { return pm_less_name(a->name, b->name); });
        std::map<std::string, size_t> group_index;
        for (const PresetManagerItem *item : sorted) {
            if (!item->is_system) {
                PresetManagerRow row = pm_row_of(*item);
                row.key = "u|" + item->name;
                user_rows.push_back(std::move(row));
                continue;
            }
            const std::string alias = preset_manager_favorite_key(item->name, item->alias, true);
            const std::string gkey  = "s|" + item->vendor_id + "|" + alias;
            auto              it    = group_index.find(gkey);
            if (it == group_index.end()) {
                PresetManagerRow row = pm_row_of(*item);
                row.key  = gkey;
                row.name = alias;
                group_index.emplace(gkey, system_rows.size());
                system_rows.push_back(std::move(row));
                continue;
            }
            PresetManagerRow &row = system_rows[it->second];
            row.members.push_back(item->name);
            row.old_names.insert(row.old_names.end(), item->renamed_from.begin(), item->renamed_from.end());
            row.printers.insert(row.printers.end(), item->printers.begin(), item->printers.end());
            row.all_printers = row.all_printers || item->all_printers;
            row.enabled      = row.enabled || item->visible;
            row.in_use       = row.in_use || item->in_use;
            if (row.vendor.empty()) row.vendor = item->filament_vendor;
            if (row.type.empty()) row.type = item->type;
            if (row.filament_id.empty()) row.filament_id = item->filament_id;
            row.nozzle_t  = pm_first_known(row.nozzle_t, item->nozzle_t);
            row.nozzle_lo = pm_first_known(row.nozzle_lo, item->nozzle_lo);
            row.nozzle_hi = pm_first_known(row.nozzle_hi, item->nozzle_hi);
            row.bed_t     = pm_first_known(row.bed_t, item->bed_t);
        }
        for (PresetManagerRow &row : system_rows) {
            pm_sort_unique(row.printers);
            pm_sort_unique(row.old_names);
        }
    } else {
        for (const PresetManagerItem &item : items) {
            PresetManagerRow row = pm_row_of(item);
            if (item.is_system) {
                row.key  = "s|" + item.name;
                row.tier = process_quality_tier(row.name);
                system_rows.push_back(std::move(row));
            } else {
                row.key  = "u|" + item.name;
                row.tier = item.inherits.empty() ? std::string("Detached") : preset_manager_favorite_key(item.inherits, std::string(), true);
                user_rows.push_back(std::move(row));
            }
        }
    }

    for (PresetManagerRow &row : system_rows)
        pm_set_lock(row);
    for (PresetManagerRow &row : user_rows)
        pm_set_lock(row);

    std::stable_sort(system_rows.begin(), system_rows.end(), [](const PresetManagerRow &a, const PresetManagerRow &b) {
        if (a.vendor != b.vendor)
            return pm_less_name(a.vendor, b.vendor);
        if (a.name != b.name)
            return pm_less_name(a.name, b.name);
        return pm_less_name(a.key, b.key);
    });
    std::stable_sort(user_rows.begin(), user_rows.end(), [](const PresetManagerRow &a, const PresetManagerRow &b) { return pm_less_name(a.name, b.name); });

    model.rows = std::move(system_rows);
    model.rows.insert(model.rows.end(), std::make_move_iterator(user_rows.begin()), std::make_move_iterator(user_rows.end()));
    for (const PresetManagerRow &row : model.rows)
        model.printers.insert(model.printers.end(), row.printers.begin(), row.printers.end());
    pm_sort_unique(model.printers);
    return model;
}

std::vector<PresetManagerItem> collect_preset_manager_items(const PresetBundle &bundle, PresetManagerKind kind, const std::string &bed_temp_key)
{
    // The printers a row can be listed under: every visible printer preset.
    std::vector<const Preset *>       printers;
    std::vector<DynamicPrintConfig>    extra;
    std::vector<std::string>           printer_nozzle;
    for (const Preset &printer : bundle.printers) {
        if (printer.is_default || !printer.is_visible)
            continue;
        printers.push_back(&printer);
        DynamicPrintConfig config;
        config.set_key_value("printer_preset", new ConfigOptionString(printer.name));
        if (const auto *noz = printer.config.option<ConfigOptionFloats>("nozzle_diameter"); noz != nullptr) {
            config.set_key_value("num_extruders", new ConfigOptionInt(int(noz->values.size())));
            printer_nozzle.push_back(noz->values.empty() ? std::string() : pm_nozzle_text(noz->values.front()));
        } else
            printer_nozzle.emplace_back();
        extra.push_back(std::move(config));
    }
    // A condition gives the same answer for the same printer, and thousands of presets share a handful
    // of conditions: evaluate each once per printer.
    std::vector<std::unordered_map<std::string, bool>> memo(printers.size());
    auto compatible = [&](const Preset &preset, size_t i) {
        const Preset &printer = *printers[i];
        const auto   *list    = preset.config.option<ConfigOptionStrings>("compatible_printers");
        const bool    has_list = list != nullptr && !list->values.empty();
        const std::string &condition = preset.compatible_printers_condition();
        const PresetWithVendorProfile p(preset, preset.vendor), pr(printer, printer.vendor);
        if (!has_list && !condition.empty() && preset.m_excluded_from.empty()) {
            auto it = memo[i].find(condition);
            if (it != memo[i].end())
                return it->second;
            const bool r = is_compatible_with_printer(p, pr, &extra[i]);
            memo[i].emplace(condition, r);
            return r;
        }
        return is_compatible_with_printer(p, pr, &extra[i]);
    };

    std::set<std::string> in_use;
    if (kind == PresetManagerKind::Filament) {
        in_use.insert(bundle.filament_presets.begin(), bundle.filament_presets.end());
    } else {
        in_use.insert(bundle.prints.get_selected_preset_name());
    }

    const PresetCollection        &collection = kind == PresetManagerKind::Filament ? static_cast<const PresetCollection &>(bundle.filaments) : bundle.prints;
    std::vector<PresetManagerItem> items;
    for (const Preset &preset : collection) {
        if (preset.is_default || preset.is_project_embedded || preset.is_external)
            continue;
        if (!preset.is_system && !preset.is_user())
            continue;
        if (preset.is_system && preset.vendor != nullptr && (preset.vendor->id == "Template" || preset.vendor->name == "Template"))
            continue;
        PresetManagerItem item;
        item.name         = preset.name;
        item.alias        = preset.alias;
        item.is_system    = preset.is_system;
        item.vendor_id    = preset.is_system && preset.vendor != nullptr ? preset.vendor->id : std::string();
        item.version      = preset.is_system && preset.vendor != nullptr ? preset.vendor->config_version.to_string() : std::string();
        item.visible      = preset.is_visible;
        item.in_use       = in_use.count(preset.name) > 0;
        item.renamed_from = preset.renamed_from;
        item.favorite_key = preset_manager_favorite_key(preset.name, preset.alias, preset.is_system);
        if (!preset.is_system)
            item.mtime = export_file_mtime(preset.file);
        const DynamicPrintConfig &config = preset.config;
        if (kind == PresetManagerKind::Filament) {
            item.filament_vendor = pm_first_string(config, "filament_vendor");
            item.type            = pm_first_string(config, "filament_type");
            item.filament_id     = preset.filament_id;
            item.nozzle_t        = pm_first_number(config, "nozzle_temperature");
            item.nozzle_lo       = pm_first_number(config, "nozzle_temperature_range_low");
            item.nozzle_hi       = pm_first_number(config, "nozzle_temperature_range_high");
            item.bed_t           = pm_first_number(config, bed_temp_key);
        } else {
            item.layer_height = pm_first_number(config, "layer_height");
            if (!preset.is_system)
                item.inherits = preset.inherits();
        }
        size_t n_compatible = 0;
        for (size_t i = 0; i < printers.size(); ++i) {
            if (!compatible(preset, i))
                continue;
            ++n_compatible;
            item.printers.push_back(printers[i]->name);
            if (kind == PresetManagerKind::Process && !printer_nozzle[i].empty())
                item.nozzles.push_back(printer_nozzle[i]);
        }
        item.all_printers = !printers.empty() && n_compatible == printers.size() && printers.size() > 1;
        pm_sort_unique(item.printers);
        std::sort(item.nozzles.begin(), item.nozzles.end(), [](const std::string &a, const std::string &b) { return std::atof(a.c_str()) < std::atof(b.c_str()); });
        item.nozzles.erase(std::unique(item.nozzles.begin(), item.nozzles.end()), item.nozzles.end());
        items.push_back(std::move(item));
    }
    return items;
}

nlohmann::json preset_manager_rows_json(const PresetManagerModel &model, const std::set<std::string> &favorites, const std::string &current_printer)
{
    nlohmann::json out;
    out["kind"]            = model.kind == PresetManagerKind::Filament ? "filament" : "process";
    out["printers"]        = model.printers;
    out["current_printer"] = current_printer;
    nlohmann::json rows    = nlohmann::json::array();
    for (const PresetManagerRow &row : model.rows) {
        nlohmann::json r;
        r["key"]         = row.key;
        r["fav"]         = row.favorite_key;
        r["name"]        = row.name;
        r["members"]     = row.members;
        r["vendor"]      = row.vendor;
        r["type"]        = row.type;
        r["source"]      = row.is_system ? "System" : "User";
        r["filament_id"] = row.filament_id;
        r["version"]     = row.version;
        r["printers"]    = row.printers;
        r["all"]         = row.all_printers;
        r["nozzles"]     = row.nozzles;
        r["nozzle_t"]    = pm_number_or_null(row.nozzle_t);
        r["nozzle_lo"]   = pm_number_or_null(row.nozzle_lo);
        r["nozzle_hi"]   = pm_number_or_null(row.nozzle_hi);
        r["bed_t"]       = pm_number_or_null(row.bed_t);
        r["lh"]          = pm_number_or_null(row.layer_height);
        r["tier"]        = row.tier;
        r["inherits"]    = row.inherits;
        r["mtime"]       = row.mtime > 0 ? nlohmann::json(row.mtime) : nlohmann::json(nullptr);
        r["enabled"]     = row.enabled;
        r["lock"]        = row.lock;
        r["in_use"]      = row.in_use;
        r["star"]        = !row.favorite_key.empty() && favorites.count(row.favorite_key) > 0;
        // Phase 2 (rename, delete) turns these on for user presets.
        r["can_rename"]  = false;
        r["can_delete"]  = false;
        rows.push_back(std::move(r));
    }
    out["rows"] = std::move(rows);
    return out;
}

std::map<std::string, std::string> filament_section_with(std::map<std::string, std::string> section, const PresetManagerRow &row, bool enabled)
{
    if (!row.is_system)
        return section; // user presets are always shown; nothing is stored for them
    if (enabled) {
        for (const std::string &name : row.members)
            section[name] = "true";
    } else {
        for (const std::string &name : row.members)
            section.erase(name);
        for (const std::string &name : row.old_names)
            section.erase(name);
    }
    return section;
}

std::map<std::string, std::string> hidden_process_section_with(std::map<std::string, std::string> section, const PresetManagerRow &row, bool enabled)
{
    if (!row.is_system)
        return section;
    if (enabled) {
        for (const std::string &name : row.members)
            section.erase(name);
        for (const std::string &name : row.old_names)
            section.erase(name);
    } else {
        for (const std::string &name : row.members)
            section[name] = "true";
    }
    return section;
}

} // namespace Slic3r
