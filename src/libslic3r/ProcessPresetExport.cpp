#include "ProcessPresetExport.hpp"

#include <algorithm>
#include <ctime>
#include <set>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <miniz.h>

#include "Preset.hpp"
#include "PresetBundle.hpp"
#include "Utils.hpp"

namespace Slic3r {

UserPresetsByPrinter collect_user_process_presets(PresetBundle &bundle)
{
    UserPresetsByPrinter out;
    for (const Preset &printer_preset : bundle.printers.get_presets()) {
        const std::string printer_name = printer_preset.name;
        if (!printer_preset.is_visible || printer_preset.is_default || printer_preset.is_project_embedded)
            continue;
        // Compatibility is a property of the selected printer, so select it and re-evaluate.
        if (!bundle.printers.select_preset_by_name(printer_name, true))
            continue;
        bundle.update_compatible(PresetSelectCompatibleType::Always);
        for (const Preset &process : bundle.prints.get_presets()) {
            if (process.is_system || process.is_default || process.is_project_embedded)
                continue;
            if (process.is_compatible)
                out[printer_name].push_back(&process);
        }
    }
    return out;
}

std::int64_t export_file_mtime(const std::string &path)
{
    if (path.empty())
        return 0;
    boost::system::error_code ec;
    const std::time_t t = boost::filesystem::last_write_time(boost::filesystem::path(path), ec);
    return ec ? 0 : std::int64_t(t);
}

ExportPrinterIndex::ExportPrinterIndex(const UserPresetsByPrinter &by_printer, const PresetCollection &printers)
{
    std::set<std::string> system_printers; // printers with at least one preset, listed the way the old dialog did
    for (const auto &[printer_name, presets] : by_printer) {
        if (presets.empty())
            continue;
        const Preset *printer = printers.find_preset(printer_name, false);
        if (printer == nullptr)
            continue;
        const bool listed = printer->is_system && printers.get_preset_base(*printer) == printer;
        if (listed)
            system_printers.insert(printer_name);
        for (const Preset *preset : presets) {
            if (preset == nullptr || preset->is_system)
                continue;
            Listed &l = m_by_preset[preset->name];
            (listed ? l.system_printers : l.user_printers).insert(printer_name);
        }
    }
    m_system_printers = system_printers.size();
}

void ExportPrinterIndex::fill(const std::string &preset_name, PresetExportRow &row) const
{
    row.printers.clear();
    row.all_printers = false;
    auto it = m_by_preset.find(preset_name);
    if (it == m_by_preset.end())
        return;
    const Listed &l = it->second;
    const std::set<std::string> &listed = l.system_printers.empty() ? l.user_printers : l.system_printers;
    row.printers.assign(listed.begin(), listed.end()); // a std::set: sorted, distinct
    row.all_printers = !l.system_printers.empty() && m_system_printers > 1 && l.system_printers.size() == m_system_printers;
}

void finish_export_model(PresetExportModel &model)
{
    std::stable_sort(model.rows.begin(), model.rows.end(), [](const PresetExportRow &a, const PresetExportRow &b) {
        const std::string la = boost::to_lower_copy(a.name), lb = boost::to_lower_copy(b.name);
        return la != lb ? la < lb : a.name < b.name;
    });
    std::set<std::string> printers, vendors, materials;
    for (size_t i = 0; i < model.rows.size(); ++i) {
        PresetExportRow &row = model.rows[i];
        row.id = i;
        printers.insert(row.printers.begin(), row.printers.end());
        if (!row.vendor.empty())
            vendors.insert(row.vendor);
        if (!row.material.empty())
            materials.insert(row.material);
    }
    model.printers.assign(printers.begin(), printers.end());
    model.vendors.assign(vendors.begin(), vendors.end());
    model.materials.assign(materials.begin(), materials.end());
}

PresetExportModel build_process_export_model(const UserPresetsByPrinter &by_printer, const PresetCollection &printers)
{
    const ExportPrinterIndex index(by_printer, printers);
    std::map<std::string, const Preset *> by_name;
    for (const auto &[printer_name, presets] : by_printer)
        for (const Preset *process : presets)
            if (process != nullptr && !process->is_system)
                by_name.emplace(process->name, process);

    PresetExportModel model;
    for (const auto &[name, preset] : by_name) {
        PresetExportRow row;
        row.name     = name;
        row.file     = preset->file;
        row.inherits = preset->inherits();
        if (const auto *lh = preset->config.option<ConfigOptionFloat>("layer_height"))
            row.layer_height = lh->value;
        index.fill(name, row);
        if (row.printers.empty())
            continue; // listed under no known printer
        row.mtime = export_file_mtime(row.file);
        model.rows.push_back(std::move(row));
    }
    finish_export_model(model);
    return model;
}

PresetExportModel build_process_export_model(PresetBundle &scratch_bundle)
{
    const UserPresetsByPrinter by_printer = collect_user_process_presets(scratch_bundle);
    return build_process_export_model(by_printer, scratch_bundle.printers);
}

nlohmann::json export_rows_json(const PresetExportModel &model, const std::vector<size_t> &selected)
{
    nlohmann::json rows = nlohmann::json::array();
    for (const PresetExportRow &r : model.rows) {
        nlohmann::json o;
        o["id"]       = r.id;
        o["name"]     = r.name;
        o["printers"] = r.printers;
        o["all"]      = r.all_printers;
        o["inherits"] = r.inherits;
        o["lh"]       = r.layer_height > 0. ? nlohmann::json(r.layer_height) : nlohmann::json(nullptr);
        o["mtime"]    = r.mtime > 0 ? nlohmann::json(r.mtime) : nlohmann::json(nullptr);
        o["vendor"]   = r.vendor;
        o["material"] = r.material;
        rows.push_back(std::move(o));
    }
    nlohmann::json out;
    out["rows"]      = std::move(rows);
    out["printers"]  = model.printers;
    out["vendors"]   = model.vendors;
    out["materials"] = model.materials;
    out["selected"]  = selected;
    return out;
}

std::vector<PresetZipEntry> preset_export_entries(const PresetExportModel &model, const std::vector<size_t> &selected,
                                                  std::vector<std::string> *skipped)
{
    std::vector<bool> chosen(model.rows.size(), false);
    for (const size_t id : selected)
        if (id < chosen.size())
            chosen[id] = true;

    std::vector<PresetZipEntry> out;
    std::set<std::string>       names;
    for (size_t i = 0; i < model.rows.size(); ++i) {
        if (!chosen[i])
            continue;
        const PresetExportRow &row = model.rows[i];
        const std::string path = boost::filesystem::path(row.file).make_preferred().string();
        if (path.empty()) {
            BOOST_LOG_TRIVIAL(info) << "Export preset: " << row.name << " skip because of the preset file path is empty.";
            if (skipped)
                skipped->push_back(row.name + ": no preset file");
            continue;
        }
        const std::string entry = row.name + ".json";
        if (!names.insert(entry).second) {
            BOOST_LOG_TRIVIAL(warning) << "Export preset: " << row.name << " skipped, the zip already has an entry named " << entry;
            if (skipped)
                skipped->push_back(row.name + ": duplicate zip entry " + entry);
            continue;
        }
        out.push_back({ entry, path });
    }
    return out;
}

PresetZipResult write_presets_zip(const std::string &zip_path_utf8, const std::vector<PresetZipEntry> &entries)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (mz_zip_writer_init_file(&zip, encode_path(zip_path_utf8.c_str()).c_str(), 0) == MZ_FALSE) {
        BOOST_LOG_TRIVIAL(info) << "Failed to initialize ZIP archive";
        return PresetZipResult::InitFailed;
    }
    for (const PresetZipEntry &entry : entries) {
        if (mz_zip_writer_add_file(&zip, entry.name.c_str(), encode_path(entry.path.c_str()).c_str(), nullptr, 0, MZ_DEFAULT_COMPRESSION) == MZ_FALSE) {
            BOOST_LOG_TRIVIAL(info) << entry.name << " Failed to add file to ZIP archive";
            mz_zip_writer_end(&zip);
            return PresetZipResult::AddFileFailed;
        }
        BOOST_LOG_TRIVIAL(info) << "Preset json add successful: " << entry.name;
    }
    if (mz_zip_writer_finalize_archive(&zip) == MZ_FALSE) {
        BOOST_LOG_TRIVIAL(info) << "Failed to finalize ZIP archive";
        mz_zip_writer_end(&zip);
        return PresetZipResult::FinalizeFailed;
    }
    mz_zip_writer_end(&zip);
    return PresetZipResult::Ok;
}

} // namespace Slic3r
