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

ProcessPresetsByPrinter collect_user_process_presets(PresetBundle &bundle)
{
    ProcessPresetsByPrinter out;
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

namespace {

std::int64_t file_mtime(const std::string &path)
{
    if (path.empty())
        return 0;
    boost::system::error_code ec;
    const std::time_t t = boost::filesystem::last_write_time(boost::filesystem::path(path), ec);
    return ec ? 0 : std::int64_t(t);
}

struct Accumulated
{
    const Preset          *preset = nullptr;
    std::set<std::string>  system_printers;
    std::set<std::string>  user_printers;
};

} // namespace

ProcessExportModel build_process_export_model(const ProcessPresetsByPrinter &by_printer, const PresetCollection &printers)
{
    std::map<std::string, Accumulated> by_name;
    std::set<std::string>              system_printers; // printers with at least one preset, listed the way the old dialog did
    for (const auto &[printer_name, presets] : by_printer) {
        if (presets.empty())
            continue;
        const Preset *printer = printers.find_preset(printer_name, false);
        if (printer == nullptr)
            continue;
        // The old per-printer list showed system printer presets that are their own base only.
        const bool listed = printer->is_system && printers.get_preset_base(*printer) == printer;
        if (listed)
            system_printers.insert(printer_name);
        for (const Preset *process : presets) {
            if (process == nullptr || process->is_system)
                continue;
            Accumulated &acc = by_name[process->name];
            if (acc.preset == nullptr)
                acc.preset = process;
            (listed ? acc.system_printers : acc.user_printers).insert(printer_name);
        }
    }

    ProcessExportModel model;
    std::set<std::string> all_printers;
    for (auto &[name, acc] : by_name) {
        ProcessExportRow row;
        row.name = name;
        row.file = acc.preset->file;
        row.inherits = acc.preset->inherits();
        if (const auto *lh = acc.preset->config.option<ConfigOptionFloat>("layer_height"))
            row.layer_height = lh->value;
        const std::set<std::string> &listed = acc.system_printers.empty() ? acc.user_printers : acc.system_printers;
        row.printers.assign(listed.begin(), listed.end()); // a std::set: sorted, distinct
        row.all_printers = !acc.system_printers.empty() && system_printers.size() > 1 && acc.system_printers.size() == system_printers.size();
        row.mtime = file_mtime(row.file);
        all_printers.insert(listed.begin(), listed.end());
        model.rows.push_back(std::move(row));
    }
    std::stable_sort(model.rows.begin(), model.rows.end(), [](const ProcessExportRow &a, const ProcessExportRow &b) {
        const std::string la = boost::to_lower_copy(a.name), lb = boost::to_lower_copy(b.name);
        return la != lb ? la < lb : a.name < b.name;
    });
    for (size_t i = 0; i < model.rows.size(); ++i)
        model.rows[i].id = i;
    model.printers.assign(all_printers.begin(), all_printers.end());
    return model;
}

ProcessExportModel build_process_export_model(PresetBundle &scratch_bundle)
{
    const ProcessPresetsByPrinter by_printer = collect_user_process_presets(scratch_bundle);
    return build_process_export_model(by_printer, scratch_bundle.printers);
}

nlohmann::json process_export_rows_json(const ProcessExportModel &model, const std::vector<size_t> &selected)
{
    nlohmann::json rows = nlohmann::json::array();
    for (const ProcessExportRow &r : model.rows) {
        nlohmann::json o;
        o["id"]       = r.id;
        o["name"]     = r.name;
        o["printers"] = r.printers;
        o["all"]      = r.all_printers;
        o["inherits"] = r.inherits;
        o["lh"]       = r.layer_height > 0. ? nlohmann::json(r.layer_height) : nlohmann::json(nullptr);
        o["mtime"]    = r.mtime > 0 ? nlohmann::json(r.mtime) : nlohmann::json(nullptr);
        rows.push_back(std::move(o));
    }
    nlohmann::json out;
    out["rows"]     = std::move(rows);
    out["printers"] = model.printers;
    out["selected"] = selected;
    return out;
}

std::vector<PresetZipEntry> process_export_entries(const ProcessExportModel &model, const std::vector<size_t> &selected,
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
        const ProcessExportRow &row = model.rows[i];
        const std::string path = boost::filesystem::path(row.file).make_preferred().string();
        if (path.empty()) {
            BOOST_LOG_TRIVIAL(info) << "Export process preset: " << row.name << " skip because of the preset file path is empty.";
            if (skipped)
                skipped->push_back(row.name + ": no preset file");
            continue;
        }
        const std::string entry = row.name + ".json";
        if (!names.insert(entry).second) {
            BOOST_LOG_TRIVIAL(warning) << "Export process preset: " << row.name << " skipped, the zip already has an entry named " << entry;
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
        BOOST_LOG_TRIVIAL(info) << "Process preset json add successful: " << entry.name;
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
