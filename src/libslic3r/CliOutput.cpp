#include "CliOutput.hpp"

#include "CostEstimate.hpp"
#include "GCode/GCodeProcessor.hpp"
#include "Utils.hpp"

#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace Slic3r {

// ---- Output paths --------------------------------------------------------------------------------

std::string cli_join_output_path(const std::string &outputdir, const std::string &name)
{
    if (outputdir.empty() || name.empty())
        return name;
    const boost::filesystem::path p(name);
    // has_root_name(): C:foo and \\server\share; has_root_directory(): /x and \x, which Windows reads as
    // "on the current drive" and which boost does not call absolute there.
    if (p.has_root_name() || p.has_root_directory())
        return name;
    const char last = outputdir.back();
    if (last == '/' || last == '\\')
        return outputdir + name;
    return outputdir + "/" + name;
}

std::string cli_gcode_file_name(const std::string &name, int plate_number, int plates_written)
{
    const std::string token  = "{plate}";
    const std::string number = std::to_string(plate_number);
    if (name.find(token) != std::string::npos) {
        std::string out = name;
        for (size_t at = out.find(token); at != std::string::npos; at = out.find(token, at + number.size()))
            out.replace(at, token.size(), number);
        return out;
    }
    if (plates_written <= 1)
        return name;
    // The extension is the last dot after the last separator.
    const size_t sep = name.find_last_of("/\\");
    const size_t dot = name.find_last_of('.');
    const bool   has_extension = dot != std::string::npos && (sep == std::string::npos || dot > sep) && dot > 0 &&
                                 dot + 1 < name.size();
    if (!has_extension)
        return name + "_" + number;
    return name.substr(0, dot) + "_" + number + name.substr(dot);
}

// ---- Exit codes ----------------------------------------------------------------------------------

const std::vector<CliExitCode> &cli_exit_code_names()
{
    static const std::vector<CliExitCode> table = {
        {CLI_SUCCESS, "CLI_SUCCESS"},
        {CLI_ENVIRONMENT_ERROR, "CLI_ENVIRONMENT_ERROR"},
        {CLI_INVALID_PARAMS, "CLI_INVALID_PARAMS"},
        {CLI_FILE_NOTFOUND, "CLI_FILE_NOTFOUND"},
        {CLI_FILELIST_INVALID_ORDER, "CLI_FILELIST_INVALID_ORDER"},
        {CLI_CONFIG_FILE_ERROR, "CLI_CONFIG_FILE_ERROR"},
        {CLI_DATA_FILE_ERROR, "CLI_DATA_FILE_ERROR"},
        {CLI_INVALID_PRINTER_TECH, "CLI_INVALID_PRINTER_TECH"},
        {CLI_UNSUPPORTED_OPERATION, "CLI_UNSUPPORTED_OPERATION"},
        {CLI_COPY_OBJECTS_ERROR, "CLI_COPY_OBJECTS_ERROR"},
        {CLI_SCALE_TO_FIT_ERROR, "CLI_SCALE_TO_FIT_ERROR"},
        {CLI_EXPORT_STL_ERROR, "CLI_EXPORT_STL_ERROR"},
        {CLI_EXPORT_OBJ_ERROR, "CLI_EXPORT_OBJ_ERROR"},
        {CLI_EXPORT_3MF_ERROR, "CLI_EXPORT_3MF_ERROR"},
        {CLI_OUT_OF_MEMORY, "CLI_OUT_OF_MEMORY"},
        {CLI_3MF_NOT_SUPPORT_MACHINE_CHANGE, "CLI_3MF_NOT_SUPPORT_MACHINE_CHANGE"},
        {CLI_3MF_NEW_MACHINE_NOT_SUPPORTED, "CLI_3MF_NEW_MACHINE_NOT_SUPPORTED"},
        {CLI_PROCESS_NOT_COMPATIBLE, "CLI_PROCESS_NOT_COMPATIBLE"},
        {CLI_INVALID_VALUES_IN_3MF, "CLI_INVALID_VALUES_IN_3MF"},
        {CLI_POSTPROCESS_NOT_SUPPORTED, "CLI_POSTPROCESS_NOT_SUPPORTED"},
        {CLI_PRINTABLE_SIZE_REDUCED, "CLI_PRINTABLE_SIZE_REDUCED"},
        {CLI_OBJECT_ARRANGE_FAILED, "CLI_OBJECT_ARRANGE_FAILED"},
        {CLI_OBJECT_ORIENT_FAILED, "CLI_OBJECT_ORIENT_FAILED"},
        {CLI_MODIFIED_PARAMS_TO_PRINTER, "CLI_MODIFIED_PARAMS_TO_PRINTER"},
        {CLI_FILE_VERSION_NOT_SUPPORTED, "CLI_FILE_VERSION_NOT_SUPPORTED"},
        {CLI_EXPORT_STEP_ERROR, "CLI_EXPORT_STEP_ERROR"},
        {CLI_NO_SUITABLE_OBJECTS, "CLI_NO_SUITABLE_OBJECTS"},
        {CLI_VALIDATE_ERROR, "CLI_VALIDATE_ERROR"},
        {CLI_OBJECTS_PARTLY_INSIDE, "CLI_OBJECTS_PARTLY_INSIDE"},
        {CLI_EXPORT_CACHE_DIRECTORY_CREATE_FAILED, "CLI_EXPORT_CACHE_DIRECTORY_CREATE_FAILED"},
        {CLI_EXPORT_CACHE_WRITE_FAILED, "CLI_EXPORT_CACHE_WRITE_FAILED"},
        {CLI_IMPORT_CACHE_NOT_FOUND, "CLI_IMPORT_CACHE_NOT_FOUND"},
        {CLI_IMPORT_CACHE_DATA_CAN_NOT_USE, "CLI_IMPORT_CACHE_DATA_CAN_NOT_USE"},
        {CLI_IMPORT_CACHE_LOAD_FAILED, "CLI_IMPORT_CACHE_LOAD_FAILED"},
        {CLI_SLICING_TIME_EXCEEDS_LIMIT, "CLI_SLICING_TIME_EXCEEDS_LIMIT"},
        {CLI_TRIANGLE_COUNT_EXCEEDS_LIMIT, "CLI_TRIANGLE_COUNT_EXCEEDS_LIMIT"},
        {CLI_NO_SUITABLE_OBJECTS_AFTER_SKIP, "CLI_NO_SUITABLE_OBJECTS_AFTER_SKIP"},
        {CLI_FILAMENT_NOT_MATCH_BED_TYPE, "CLI_FILAMENT_NOT_MATCH_BED_TYPE"},
        {CLI_FILAMENTS_DIFFERENT_TEMP, "CLI_FILAMENTS_DIFFERENT_TEMP"},
        {CLI_OBJECT_COLLISION_IN_SEQ_PRINT, "CLI_OBJECT_COLLISION_IN_SEQ_PRINT"},
        {CLI_OBJECT_COLLISION_IN_LAYER_PRINT, "CLI_OBJECT_COLLISION_IN_LAYER_PRINT"},
        {CLI_SPIRAL_MODE_INVALID_PARAMS, "CLI_SPIRAL_MODE_INVALID_PARAMS"},
        {CLI_MIXED_FILAMENT_INVALID, "CLI_MIXED_FILAMENT_INVALID"},
        {CLI_SLICING_ERROR, "CLI_SLICING_ERROR"},
        {CLI_GCODE_PATH_CONFLICTS, "CLI_GCODE_PATH_CONFLICTS"},
    };
    return table;
}

std::string cli_exit_code_name(int code)
{
    for (const CliExitCode &entry : cli_exit_code_names())
        if (entry.code == code)
            return entry.name;
    return std::string();
}

static std::string message_of(const std::map<int, std::string> &messages, int code)
{
    const auto it = messages.find(code);
    return it == messages.end() ? std::string() : it->second;
}

static std::string trimmed(std::string s)
{
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

nlohmann::json cli_exit_codes_json(const std::map<int, std::string> &messages)
{
    nlohmann::json list = nlohmann::json::array();
    for (const CliExitCode &entry : cli_exit_code_names()) {
        nlohmann::json item;
        item["code"]    = entry.code;
        item["name"]    = entry.name;
        item["message"] = trimmed(message_of(messages, entry.code));
        list.push_back(std::move(item));
    }
    nlohmann::json out;
    out["exit_codes"] = std::move(list);
    return out;
}

std::string cli_exit_codes_text(const std::map<int, std::string> &messages)
{
    size_t name_width = 4;
    for (const CliExitCode &entry : cli_exit_code_names())
        name_width = std::max(name_width, std::string(entry.name).size());

    std::string out;
    char        line[64];
    std::snprintf(line, sizeof(line), "%6s  ", "Code");
    out += line;
    out += "Name" + std::string(name_width - 4, ' ') + "  Meaning\n";
    for (const CliExitCode &entry : cli_exit_code_names()) {
        std::snprintf(line, sizeof(line), "%6d  ", entry.code);
        out += line;
        out += entry.name + std::string(name_width - std::string(entry.name).size(), ' ') + "  ";
        out += trimmed(message_of(messages, entry.code)) + "\n";
    }
    return out;
}

// ---- result.json usage numbers -------------------------------------------------------------------

static double round_to(double value, int decimals)
{
    const double scale = std::pow(10., decimals);
    return std::round(value * scale) / scale;
}

nlohmann::json cli_plate_usage_json(const GCodeProcessorResult &result, const std::vector<CliFilamentSlot> &slots)
{
    using ETimeMode = PrintEstimatedStatistics::ETimeMode;
    const PrintEstimatedStatistics &ps   = result.print_statistics;
    const CostBreakdown             cost = compute_cost(result);

    nlohmann::json j;
    j["layers"] = ps.modes[size_t(ETimeMode::Normal)].layers_times.size();

    auto total_volume = [&ps](size_t slot) {
        const auto it = ps.total_volumes_per_extruder.find(slot);
        return it == ps.total_volumes_per_extruder.end() ? 0. : it->second;
    };

    struct Sum
    {
        std::vector<size_t> filaments;
        double              g{0.}, mm{0.}, mm3{0.}, cost{0.};
    };
    std::map<int, Sum> per_extruder;

    j["filaments"] = nlohmann::json::array();
    for (const FilamentCostLine &line : cost.lines) {
        const CliFilamentSlot *slot = line.slot < slots.size() ? &slots[line.slot] : nullptr;
        double volume = total_volume(line.slot);
        if (volume <= 0. && line.slot < result.filament_densities.size() && result.filament_densities[line.slot] > 0.f)
            volume = line.total_g / (0.001 * double(result.filament_densities[line.slot]));
        const double diameter = line.slot < result.filament_diameters.size() ? double(result.filament_diameters[line.slot]) : 0.;
        const double mm       = diameter > 0. ? volume / (3.14159265358979323846 * 0.25 * diameter * diameter) : 0.;

        std::string colour = slot ? slot->colour : std::string();
        if (colour.empty() && line.slot < result.extruder_colors.size())
            colour = result.extruder_colors[line.slot];
        const int extruder = slot && slot->extruder > 0 ? slot->extruder : 1;

        nlohmann::json f;
        f["index"]        = line.slot;
        f["preset"]       = slot ? slot->preset : std::string();
        f["type"]         = slot ? slot->type : std::string();
        f["colour"]       = colour;
        f["extruder"]     = extruder;
        f["grams"]        = round_to(line.total_g, 4);
        f["mm"]           = round_to(mm, 2);
        f["mm3"]          = round_to(volume, 2);
        f["cost"]         = round_to(line.total_cost, 4);
        f["price_per_kg"] = round_to(line.price_per_kg, 4);
        f["model_g"]      = round_to(line.model_g, 4);
        f["support_g"]    = round_to(line.support_g, 4);
        f["flush_g"]      = round_to(line.flush_g, 4);
        f["tower_g"]      = round_to(line.tower_g, 4);
        j["filaments"].push_back(std::move(f));

        Sum &sum = per_extruder[extruder];
        sum.filaments.push_back(line.slot);
        sum.g += line.total_g;
        sum.mm += mm;
        sum.mm3 += volume;
        sum.cost += line.total_cost;
    }

    j["extruders"] = nlohmann::json::array();
    for (const auto &[extruder, sum] : per_extruder) {
        nlohmann::json e;
        e["index"]     = extruder;
        e["filaments"] = sum.filaments;
        e["grams"]     = round_to(sum.g, 4);
        e["mm"]        = round_to(sum.mm, 2);
        e["mm3"]       = round_to(sum.mm3, 2);
        e["cost"]      = round_to(sum.cost, 4);
        j["extruders"].push_back(std::move(e));
    }

    nlohmann::json c;
    c["prices_known"]       = cost.prices_known;
    c["machine_rate_known"] = cost.machine_rate_known;
    c["machine_rate_per_h"] = round_to(cost.machine_rate_per_h, 4);
    c["material"]           = round_to(cost.material, 4);
    c["machine"]            = round_to(cost.machine, 4);
    c["total"]              = round_to(cost.total, 4);
    c["display_total"]      = round_money(cost.display_total);
    j["cost"]               = std::move(c);
    return j;
}

nlohmann::json cli_result_totals(const nlohmann::json &sliced_plates)
{
    double time_s = 0., grams = 0., mm = 0., mm3 = 0., cost = 0.;
    size_t layers = 0, plates = 0;
    if (sliced_plates.is_array()) {
        for (const nlohmann::json &plate : sliced_plates) {
            if (!plate.is_object())
                continue;
            ++plates;
            time_s += plate.value("time_s", 0.);
            grams += plate.value("filament_g", 0.);
            mm3 += plate.value("filament_mm3", 0.);
            layers += plate.value("layers", size_t(0));
            if (plate.contains("filaments") && plate["filaments"].is_array())
                for (const nlohmann::json &f : plate["filaments"])
                    mm += f.value("mm", 0.);
            if (plate.contains("cost") && plate["cost"].is_object())
                cost += plate["cost"].value("display_total", 0.);
        }
    }
    nlohmann::json t;
    t["plates"]       = plates;
    t["time_s"]       = round_to(time_s, 1);
    t["layers"]       = layers;
    t["filament_g"]   = round_to(grams, 4);
    t["filament_mm"]  = round_to(mm, 2);
    t["filament_mm3"] = round_to(mm3, 2);
    t["cost"]         = round_money(cost);
    return t;
}

} // namespace Slic3r
