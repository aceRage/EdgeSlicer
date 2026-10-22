#include "ElegooCanvasMapping.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>

namespace Slic3r {
namespace ElegooCanvasMapping {

namespace {

std::string s_mapping_store_override;

using mapjson = nlohmann::json;

bool is_tray(const PrintHostDevices::Slot& s) { return s.unit >= 0 && s.tray >= 0; }

const PrintHostDevices::Slot* slot_at(const std::vector<PrintHostDevices::Slot>& slots, int pos)
{
    return (pos >= 0 && pos < (int) slots.size()) ? &slots[pos] : nullptr;
}

std::string mapping_str(const mapjson& j, const char* key)
{
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

} // namespace

std::string normalize_colour(const std::string& colour)
{
    std::string s = boost::trim_copy(colour);
    if (!s.empty() && s[0] == '#')
        s = s.substr(1);
    if (s.size() != 6 && s.size() != 8)
        return std::string();
    for (char& c : s) {
        if (!std::isxdigit((unsigned char) c))
            return std::string();
        c = (char) std::toupper((unsigned char) c);
    }
    return "#" + s.substr(0, 6);
}

std::vector<ElegooStartPrint::SlotMapEntry> slot_map_for(const std::vector<UsedFilament>& used,
                                                         const std::vector<int>& choice,
                                                         const std::vector<PrintHostDevices::Slot>& slots,
                                                         bool identity)
{
    std::vector<ElegooStartPrint::SlotMapEntry> out;
    for (size_t i = 0; i < used.size(); ++i) {
        const int pos = i < choice.size() ? choice[i] : -1;
        const PrintHostDevices::Slot* s = slot_at(slots, pos);
        if (s == nullptr && identity)
            s = slot_at(slots, used[i].index);
        if (s == nullptr || !is_tray(*s))
            continue;
        ElegooStartPrint::SlotMapEntry e;
        e.t         = used[i].index;
        e.canvas_id = s->unit;
        e.tray_id   = s->tray;
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.t < b.t; });
    return out;
}

std::vector<Warning> warnings_for(const std::vector<UsedFilament>& used, const std::vector<int>& choice,
                                  const std::vector<PrintHostDevices::Slot>& slots)
{
    std::vector<Warning> out;
    const int loaded = (int) std::count_if(slots.begin(), slots.end(), [](const auto& s) { return is_tray(s) && s.loaded; });
    if ((int) used.size() > loaded)
        out.push_back({ WarningKind::MoreFilamentsThanTrays, -1, -1 });

    std::map<int, int> first_on_slot;
    for (size_t i = 0; i < used.size(); ++i) {
        const int                     pos = i < choice.size() ? choice[i] : -1;
        const PrintHostDevices::Slot* s   = slot_at(slots, pos);
        if (s == nullptr || !is_tray(*s))
            continue;
        if (!s->loaded)
            out.push_back({ WarningKind::EmptyTray, used[i].index, s->index });
        auto ins = first_on_slot.emplace(pos, used[i].index);
        if (!ins.second)
            out.push_back({ WarningKind::SharedTray, used[i].index, s->index });
        const int t = used[i].nozzle_temp;
        if (s->loaded && t > 0 && ((s->min_temp > 0 && t < s->min_temp) || (s->max_temp > 0 && t > s->max_temp)))
            out.push_back({ WarningKind::TemperatureOutOfRange, used[i].index, s->index });
    }
    return out;
}

std::string validate(const std::vector<ElegooStartPrint::SlotMapEntry>& entries, const std::vector<PrintHostDevices::Slot>& fresh)
{
    for (const ElegooStartPrint::SlotMapEntry& e : entries) {
        const auto it = std::find_if(fresh.begin(), fresh.end(),
                                     [&e](const PrintHostDevices::Slot& s) { return s.unit == e.canvas_id && s.tray == e.tray_id; });
        if (it == fresh.end())
            return "filament " + std::to_string(e.t + 1) + ": its CANVAS tray (unit " + std::to_string(e.canvas_id) + ", tray " +
                   std::to_string(e.tray_id) + ") is no longer reported";
        if (!it->loaded)
            return "filament " + std::to_string(e.t + 1) + ": its CANVAS tray (unit " + std::to_string(e.canvas_id) + ", tray " +
                   std::to_string(e.tray_id) + ") is now empty";
    }
    return std::string();
}

std::string key_for(const UsedFilament& f)
{
    return boost::to_upper_copy(boost::trim_copy(f.type)) + "|" + boost::trim_copy(f.alias) + "|" + normalize_colour(f.colour);
}

std::string store_path()
{
    if (!s_mapping_store_override.empty())
        return s_mapping_store_override;
    return (boost::filesystem::path(data_dir()) / "hub" / "elegoo_canvas_mapping.json").string();
}

void set_store_path(const std::string& path) { s_mapping_store_override = path; }

std::map<std::string, Remembered> load()
{
    std::map<std::string, Remembered> out;
    try {
        boost::nowide::ifstream in(store_path());
        if (!in.good())
            return out;
        mapjson j;
        in >> j;
        const mapjson& m = j.is_object() && j.contains("mappings") ? j["mappings"] : mapjson::object();
        if (!m.is_object())
            return out;
        for (auto it = m.begin(); it != m.end(); ++it) {
            Remembered r;
            r.type   = mapping_str(*it, "type");
            r.name   = mapping_str(*it, "name");
            r.colour = normalize_colour(mapping_str(*it, "colour"));
            if (!r.type.empty())
                out[it.key()] = r;
        }
    } catch (...) {
        out.clear(); // unreadable is empty: a memory is never worth refusing a send over
    }
    return out;
}

void save(const std::map<std::string, Remembered>& memory)
{
    try {
        mapjson m = mapjson::object();
        for (const auto& kv : memory)
            m[kv.first] = { { "type", kv.second.type }, { "name", kv.second.name }, { "colour", kv.second.colour } };
        mapjson j;
        j["version"]  = 1;
        j["mappings"] = std::move(m);

        boost::system::error_code     ec;
        const boost::filesystem::path final(store_path());
        if (final.has_parent_path())
            boost::filesystem::create_directories(final.parent_path(), ec);
        const boost::filesystem::path temp = final.parent_path() / (final.filename().string() + ".tmp");
        {
            boost::nowide::ofstream out(temp.string(), std::ios::binary | std::ios::trunc);
            out << j.dump(1);
        }
        boost::filesystem::remove(final, ec);
        boost::filesystem::rename(temp, final, ec);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "CANVAS: could not write the remembered mappings: " << e.what();
    }
}

void remember(std::map<std::string, Remembered>& memory, const std::vector<UsedFilament>& used, const std::vector<int>& choice,
              const std::vector<PrintHostDevices::Slot>& slots)
{
    for (size_t i = 0; i < used.size(); ++i) {
        const PrintHostDevices::Slot* s = slot_at(slots, i < choice.size() ? choice[i] : -1);
        if (s == nullptr || !is_tray(*s) || !s->loaded)
            continue;
        Remembered r;
        r.type   = s->type;
        r.name   = s->name;
        r.colour = normalize_colour(s->color);
        memory[key_for(used[i])] = r;
    }
}

int recall(const std::map<std::string, Remembered>& memory, const UsedFilament& f, const std::vector<PrintHostDevices::Slot>& slots)
{
    const auto it = memory.find(key_for(f));
    if (it == memory.end())
        return -1;
    const Remembered& r     = it->second;
    int               found = -1;
    for (size_t i = 0; i < slots.size(); ++i) {
        const PrintHostDevices::Slot& s = slots[i];
        if (!is_tray(s) || !s.loaded)
            continue;
        if (!boost::iequals(s.type, r.type) || s.name != r.name || normalize_colour(s.color) != r.colour)
            continue;
        if (found >= 0)
            return -1; // two identical spools: nothing to tell them apart by, let the colour match pick
        found = (int) i;
    }
    return found;
}

} // namespace ElegooCanvasMapping
} // namespace Slic3r
