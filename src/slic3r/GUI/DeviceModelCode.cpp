#include "DeviceModelCode.hpp"

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

} } // namespace Slic3r::GUI
