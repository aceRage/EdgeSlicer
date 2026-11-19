#include "DeviceModelCode.hpp"

#include <algorithm>
#include <cctype>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "nlohmann/json.hpp"

namespace Slic3r { namespace GUI {

std::map<std::string, std::vector<std::string>> load_model_subseries(const std::string &printers_dir)
{
    namespace fs = boost::filesystem;
    std::map<std::string, std::vector<std::string>> table;
    boost::system::error_code ec;
    if (! fs::is_directory(printers_dir, ec) || ec)
        return table;
    for (fs::directory_iterator it(printers_dir, ec), end; ! ec && it != end; it.increment(ec)) {
        const fs::path &p = it->path();
        if (! fs::is_regular_file(p, ec) || p.extension() != ".json")
            continue;
        try {
            boost::nowide::ifstream f(p.string().c_str());
            if (! f.is_open())
                continue;
            nlohmann::json jj;
            f >> jj;
            if (! jj.contains("00.00.00.00"))
                continue;
            const nlohmann::json &printer = jj["00.00.00.00"];
            if (! printer.contains("subseries") || ! printer["subseries"].is_array() || ! printer.contains("model_id"))
                continue;
            std::vector<std::string> subs;
            for (const auto &s : printer["subseries"])
                if (s.is_string())
                    subs.push_back(s.get<std::string>());
            if (! subs.empty())
                table[printer["model_id"].get<std::string>()] = std::move(subs);
        } catch (...) {
            // filaments_blacklist.json and friends live in the same folder and have another shape;
            // a file that does not parse as a printer definition is simply not one.
        }
    }
    return table;
}

std::string resolve_model_subseries(const std::string &code, const std::map<std::string, std::vector<std::string>> &table)
{
    if (code.empty())
        return "";
    for (const auto &kv : table)
        for (const std::string &sub : kv.second)
            if (sub == code)
                return kv.first;
    return "";
}

std::string strip_model_revision(const std::string &code)
{
    const std::size_t dash = code.rfind('-');
    if (dash == std::string::npos || dash == 0 || dash + 2 >= code.size())
        return code;
    if (code[dash + 1] != 'V')
        return code;
    for (std::size_t i = dash + 2; i < code.size(); ++i)
        if (! std::isdigit(static_cast<unsigned char>(code[i])))
            return code;
    return code.substr(0, dash);
}

namespace {
// The "00.00.00.00" block of a printer definition, or a null json when the file is not one.
nlohmann::json read_definition(const boost::filesystem::path &p)
{
    try {
        boost::nowide::ifstream f(p.string().c_str());
        if (! f.is_open())
            return nlohmann::json();
        nlohmann::json jj;
        f >> jj;
        if (jj.is_object() && jj.contains("00.00.00.00") && jj["00.00.00.00"].is_object())
            return jj["00.00.00.00"];
    } catch (...) {
        // filaments_blacklist.json and friends share the folder and have another shape.
    }
    return nlohmann::json();
}

// A model code names a file; anything that could step out of the folder is not a code.
bool is_plain_code(const std::string &code)
{
    if (code.empty() || code == "." || code == "..")
        return false;
    for (char c : code)
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20)
            return false;
    return true;
}
} // namespace

std::string read_definition_printer_type(const std::string &printers_dir, const std::string &code)
{
    if (! is_plain_code(code))
        return "";
    const nlohmann::json printer = read_definition(boost::filesystem::path(printers_dir) / (code + ".json"));
    if (printer.is_object() && printer.contains("printer_type") && printer["printer_type"].is_string())
        return printer["printer_type"].get<std::string>();
    return "";
}

std::string resolve_model_code(const std::string &code, const std::string &printers_dir,
                               const std::map<std::string, std::vector<std::string>> &subseries)
{
    if (code.empty())
        return "";
    // The X1 family's definitions carry their old product names as printer_type
    // ("3DPrinter-X1-Carbon" in BL-P001.json); the code is the model id (MachineObject::parse_printer_type).
    if (code == "BL-P001" || code == "BL-P002")
        return code;
    if (code == "3DPrinter-X1-Carbon")
        return "BL-P001";
    if (code == "3DPrinter-X1")
        return "BL-P002";
    std::string type = read_definition_printer_type(printers_dir, code);
    if (! type.empty())
        return type;
    std::string parent = resolve_model_subseries(code, subseries);
    if (parent.empty()) {
        const std::string bare = strip_model_revision(code);
        if (bare != code)
            parent = bare;
    }
    return parent.empty() ? std::string() : read_definition_printer_type(printers_dir, parent);
}

std::map<std::string, std::string> load_model_sn_prefixes(const std::string &printers_dir)
{
    namespace fs = boost::filesystem;
    std::map<std::string, std::string> table;
    boost::system::error_code ec;
    if (! fs::is_directory(printers_dir, ec) || ec)
        return table;
    // Sorted, so a prefix two definitions share (Bambu ships O1C and O1C2 both on 31B) resolves the
    // same way on every run and platform.
    std::vector<fs::path> files;
    for (fs::directory_iterator it(printers_dir, ec), end; ! ec && it != end; it.increment(ec))
        if (it->path().extension() == ".json" && fs::is_regular_file(it->path(), ec))
            files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const fs::path &p : files) {
        const nlohmann::json printer = read_definition(p);
        if (! printer.is_object() || ! printer.contains("sn_prefix") || ! printer["sn_prefix"].is_string() ||
            ! printer.contains("model_id") || ! printer["model_id"].is_string())
            continue;
        const std::string prefix = printer["sn_prefix"].get<std::string>();
        if (! prefix.empty())
            table.emplace(prefix, printer["model_id"].get<std::string>());
    }
    return table;
}

std::string resolve_model_by_serial(const std::string &dev_id, const std::map<std::string, std::string> &sn_prefixes)
{
    for (const auto &kv : sn_prefixes)
        if (! kv.first.empty() && dev_id.size() > kv.first.size() && dev_id.compare(0, kv.first.size(), kv.first) == 0)
            return kv.second;
    return "";
}

std::string identify_device_model(const std::string &code, const std::string &dev_id, const std::string &printers_dir,
                                  const std::map<std::string, std::vector<std::string>> &subseries,
                                  const std::map<std::string, std::string>                &sn_prefixes)
{
    std::string type = resolve_model_code(code, printers_dir, subseries);
    if (! type.empty())
        return type;
    const std::string by_serial = resolve_model_by_serial(dev_id, sn_prefixes);
    return by_serial.empty() ? std::string() : resolve_model_code(by_serial, printers_dir, subseries);
}

bool device_matches_profile_model(const std::string &profile_model, const std::string &device_model,
                                  const std::vector<std::string> &device_compatible_machines)
{
    if (profile_model == device_model)
        return true;
    return std::find(device_compatible_machines.begin(), device_compatible_machines.end(), profile_model) !=
           device_compatible_machines.end();
}

} } // namespace Slic3r::GUI
