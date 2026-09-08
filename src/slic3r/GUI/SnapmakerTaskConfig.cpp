#include "SnapmakerTaskConfig.hpp"

#include <algorithm>

namespace Slic3r {
namespace GUI {
namespace SnapmakerLan {

// The firmware zeroes the whole array first, so the toolheads this job does not use are told "no"
// explicitly rather than inheriting whatever the last job left behind.
std::string end_unload_parameter(const std::vector<int>& used_toolheads)
{
    std::string out = "END_UNLOAD_FILAMENT=[";
    for (int t = 0; t < TOOLHEAD_COUNT; ++t)
        out += std::string(t ? "," : "") +
               (std::find(used_toolheads.begin(), used_toolheads.end(), t) != used_toolheads.end() ? "1" : "0");
    return out + "]";
}

// The desktop's own send does not build the task-config script: the bundled Device page does, and
// hands the finished blob to the slicer over SSWCP (sw_SendGCodes) as one
// SET_PRINT_EXTRUDER_MAP.. / SET_PRINT_USED_EXTRUDERS.. / SET_PRINT_PREFERENCES.. text. So the
// desktop's answer to "which toolheads does this job use" is that script's own
// SET_PRINT_USED_EXTRUDERS line, and adding the flag means amending the line already in it.
std::string with_end_unload(const std::string& script)
{
    const std::string PREF = "SET_PRINT_PREFERENCES";
    const std::string USED = "SET_PRINT_USED_EXTRUDERS EXTRUDERS=";
    if (script.find("END_UNLOAD_FILAMENT") != std::string::npos)
        return script; // already asked for: never two of them on one line
    const size_t pref = script.find(PREF);
    if (pref == std::string::npos)
        return script; // not a task-config script at all
    // Which toolheads the page named. Without them there is nothing to unload and no list to send.
    std::vector<int> used;
    if (const size_t u = script.find(USED); u != std::string::npos) {
        const size_t start = u + USED.size();
        const size_t end   = script.find_first_of("\r\n", start);
        std::string  list  = script.substr(start, end == std::string::npos ? std::string::npos : end - start);
        std::string  num;
        list += ',';
        for (char c : list) {
            if (c >= '0' && c <= '9') {
                num += c;
            } else {
                if (!num.empty()) {
                    try {
                        const int t = std::stoi(num);
                        if (t >= 0 && std::find(used.begin(), used.end(), t) == used.end())
                            used.push_back(t);
                    } catch (...) {}
                    num.clear();
                }
            }
        }
    }
    if (used.empty())
        return script;
    // The end of the SET_PRINT_PREFERENCES line, wherever it is in the blob.
    const size_t eol = script.find_first_of("\r\n", pref);
    const size_t at  = eol == std::string::npos ? script.size() : eol;
    std::string  out = script.substr(0, at);
    if (!out.empty() && out.back() != ' ')
        out += ' ';
    out += end_unload_parameter(used);
    if (at < script.size())
        out += script.substr(at);
    return out;
}

} // namespace SnapmakerLan
} // namespace GUI
} // namespace Slic3r
