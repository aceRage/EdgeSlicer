#include "FilamentPresetExport.hpp"

#include <map>

#include "Preset.hpp"
#include "PresetBundle.hpp"

namespace Slic3r {

std::string filament_group_name(const std::string &base_preset_name)
{
    const size_t at = base_preset_name.find_last_of('@');
    if (at == std::string::npos)
        return base_preset_name;
    return base_preset_name.substr(0, at == 0 ? 0 : at - 1);
}

UserPresetsByPrinter collect_user_filament_presets(PresetBundle &bundle)
{
    UserPresetsByPrinter out;
    for (const Preset &printer_preset : bundle.printers.get_presets()) {
        const std::string printer_name = printer_preset.name;
        if (!printer_preset.is_visible || printer_preset.is_default || printer_preset.is_project_embedded)
            continue;
        if (!bundle.printers.select_preset_by_name(printer_name, true))
            continue;
        bundle.update_compatible(PresetSelectCompatibleType::Always);
        for (const Preset &filament : bundle.filaments.get_presets()) {
            if (filament.is_system || filament.is_default || filament.is_project_embedded)
                continue;
            if (filament.is_compatible)
                out[printer_name].push_back(&filament);
        }
    }
    return out;
}

std::vector<const Preset *> collect_exportable_user_filaments(const PresetBundle &bundle)
{
    std::vector<const Preset *> out;
    for (const Preset &filament : bundle.filaments.get_presets()) {
        if (filament.is_system || filament.is_default)
            continue;
        if (bundle.filaments.get_preset_base(filament) == nullptr)
            continue;
        out.push_back(&filament);
    }
    return out;
}

namespace {

std::string first_string(const Preset &preset, const char *key)
{
    const auto *opt = preset.config.option<ConfigOptionStrings>(key);
    return (opt != nullptr && !opt->values.empty()) ? opt->values.front() : std::string();
}

} // namespace

PresetExportModel build_filament_export_model(const UserPresetsByPrinter &by_printer, const std::vector<const Preset *> &exportable,
                                              const PresetCollection &printers, const PresetCollection &filaments)
{
    const ExportPrinterIndex index(by_printer, printers);
    std::map<std::string, const Preset *> by_name;
    for (const Preset *filament : exportable)
        if (filament != nullptr && !filament->is_system)
            by_name.emplace(filament->name, filament);

    PresetExportModel model;
    for (const auto &[name, preset] : by_name) {
        PresetExportRow row;
        row.name = name;
        row.file = preset->file;
        if (const Preset *base = filaments.get_preset_base(*preset))
            row.group = filament_group_name(base->name);
        row.inherits = preset->inherits();
        row.vendor   = first_string(*preset, "filament_vendor");
        row.material = first_string(*preset, "filament_type");
        index.fill(name, row);
        row.mtime = export_file_mtime(row.file);
        model.rows.push_back(std::move(row));
    }
    finish_export_model(model);
    return model;
}

PresetExportModel build_filament_export_model(PresetBundle &scratch_bundle)
{
    const UserPresetsByPrinter by_printer = collect_user_filament_presets(scratch_bundle);
    const std::vector<const Preset *> exportable = collect_exportable_user_filaments(scratch_bundle);
    return build_filament_export_model(by_printer, exportable, scratch_bundle.printers, scratch_bundle.filaments);
}

} // namespace Slic3r
