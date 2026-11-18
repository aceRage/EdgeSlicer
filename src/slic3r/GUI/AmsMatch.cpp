#include "AmsMatch.hpp"

#include "slic3r/Utils/ColorSpaceConvert.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <set>

#include <wx/colour.h>

namespace Slic3r { namespace GUI { namespace AmsMatch {

Rgba filament_colour(const std::string& colour)
{
    // wxColour's own parser, as the send dialog has always read it.
    const wxColour c(colour);
    return {c.Red(), c.Green(), c.Blue(), c.Alpha()};
}

Rgba tray_colour(const std::string& colour)
{
    // AmsTray::decode_color, unchanged: an invalid digit stops the parse where it is.
    auto hex = [](char c) {
        return (c >= '0' && c <= '9') ? int(c - '0') : (c >= 'A' && c <= 'F') ? int(c - 'A') + 10 : (c >= 'a' && c <= 'f') ? int(c - 'a') + 10 : -1;
    };
    if (colour.size() != 8)
        return {255, 255, 255, 255};
    int ret[4] = {0, 0, 0, 0};
    for (size_t j = 0; j < 4; ++j) {
        const int d1 = hex(colour[2 * j]);
        const int d2 = hex(colour[2 * j + 1]);
        if (d1 == -1 || d2 == -1)
            break;
        ret[j] = d1 * 16 + d2;
    }
    return {ret[0], ret[1], ret[2], ret[3]};
}

float colour_distance(const Rgba& a, const Rgba& b)
{
    float lab[2][3];
    RGB2Lab(a.r / 255.f, a.g / 255.f, a.b / 255.f, &lab[0][0], &lab[0][1], &lab[0][2]);
    RGB2Lab(b.r / 255.f, b.g / 255.f, b.b / 255.f, &lab[1][0], &lab[1][1], &lab[1][2]);
    return DeltaE76(lab[0][0], lab[0][1], lab[0][2], lab[1][0], lab[1][1], lab[1][2]);
}

std::vector<std::vector<Distance>> distances(const std::vector<FilamentInfo>& filaments, const std::map<int, FilamentInfo>& trays, DistanceFn fn)
{
    std::vector<std::vector<Distance>> out;
    out.reserve(filaments.size());
    for (const FilamentInfo& f : filaments) {
        const Rgba             c = filament_colour(f.color);
        std::vector<Distance> row;
        row.reserve(trays.size());
        for (const auto& [tray_id, tray] : trays) {
            const Rgba tc = tray_colour(tray.color);
            Distance   d;
            d.tray_id = tray_id;
            d.distance = fn(c, tc);
            if (f.type != tray.type) {
                d.distance      = FAR_DISTANCE;
                d.is_type_match = false;
            } else {
                if (c.a != tc.a)
                    d.distance = FAR_DISTANCE;
                d.is_type_match = true;
            }
            row.push_back(d);
        }
        out.push_back(std::move(row));
    }
    return out;
}

std::vector<FilamentInfo> nearest_colour(const std::vector<FilamentInfo>& filaments, const std::map<int, FilamentInfo>& trays,
                                         const std::vector<std::vector<Distance>>& distance_map)
{
    std::vector<FilamentInfo> result;
    result.reserve(filaments.size());
    for (const FilamentInfo& f : filaments) {
        FilamentInfo info;
        info.id       = f.id;
        info.tray_id  = -1;
        info.distance = FAR_DISTANCE;
        result.push_back(info);
    }

    // Column j of distance_map is the j-th tray of the map.
    std::vector<std::map<int, FilamentInfo>::const_iterator> column;
    column.reserve(trays.size());
    for (auto it = trays.begin(); it != trays.end(); ++it)
        column.push_back(it);

    std::set<int> picked_src;
    std::set<int> picked_tar;
    for (size_t k = 0; k < distance_map.size(); ++k) {
        float min_val        = INT_MAX;
        int   picked_src_idx = -1;
        int   picked_tar_idx = -1;
        for (int i = 0; i < int(distance_map.size()); ++i) {
            if (picked_src.count(i))
                continue;
            for (int j = 0; j < int(distance_map[i].size()) && j < int(column.size()); ++j) {
                const Distance& d = distance_map[i][j];
                if (picked_tar.count(j)) {
                    // A tray already taken serves another filament of exactly its colour.
                    if (d.is_type_match && d.distance < 0.0001f) {
                        min_val        = d.distance;
                        picked_src_idx = i;
                        picked_tar_idx = j;
                    }
                    continue;
                }
                if (!d.is_type_match)
                    continue;
                if (min_val > d.distance) {
                    min_val        = d.distance;
                    picked_src_idx = i;
                    picked_tar_idx = j;
                } else if (min_val == d.distance && picked_src_idx >= 0 &&
                           filaments[picked_src_idx].filament_id != column[picked_tar_idx]->second.filament_id &&
                           filaments[i].filament_id == column[j]->second.filament_id) {
                    // A tie goes to the tray holding the filament's own preset.
                    picked_src_idx = i;
                    picked_tar_idx = j;
                }
            }
        }
        if (picked_src_idx < 0 || picked_tar_idx < 0)
            continue;
        const FilamentInfo& tray = column[picked_tar_idx]->second;
        FilamentInfo&       r    = result[picked_src_idx];
        r.tray_id     = column[picked_tar_idx]->first;
        r.color       = tray.color;
        r.type        = tray.type;
        r.distance    = distance_map[picked_src_idx][picked_tar_idx].distance;
        r.filament_id = tray.filament_id;
        r.ctype       = tray.ctype;
        r.colors      = tray.colors;
        r.ams_id      = tray.ams_id;
        r.slot_id     = tray.slot_id;
        picked_src.insert(picked_src_idx);
        picked_tar.insert(picked_tar_idx);
    }
    return result;
}

void drop_far_picks(std::vector<FilamentInfo>& result)
{
    for (FilamentInfo& r : result)
        if (r.distance >= DROP_DISTANCE)
            r.tray_id = -1;
}

bool excluded(int tray_index, const std::vector<int>& exclude_id)
{
    return std::find(exclude_id.begin(), exclude_id.end(), tray_index) != exclude_id.end();
}

std::string vendor_key(const std::string& vendor)
{
    std::string key;
    key.reserve(vendor.size());
    for (unsigned char c : vendor)
        if (!std::isspace(c))
            key += char(std::tolower(c));
    return key;
}

BlacklistHit check_blacklist(const nlohmann::json& list, const std::string& vendor, const std::string& type, bool skip_check)
{
    BlacklistHit hit;
    if (!list.is_object() || !list.contains("blacklist") || !list["blacklist"].is_array())
        return hit;

    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    };
    const std::string tag_vendor = vendor_key(vendor);
    const std::string tag_type   = lower(type);

    for (const nlohmann::json& entry : list["blacklist"]) {
        if (!(entry.contains("vendor") && entry.contains("type") && entry.contains("action") && entry.contains("description")))
            return hit; // a malformed entry ends the check, as before
        const std::string entry_vendor = vendor_key(entry["vendor"].get<std::string>());
        const std::string entry_type   = lower(entry["type"].get<std::string>());
        if (entry_type != tag_type)
            continue;
        if (entry_vendor != "thirdparty" && entry_vendor != tag_vendor)
            continue;
        hit.in_blacklist = true;
        hit.action       = skip_check ? std::string("warning") : entry["action"].get<std::string>();
        hit.description  = entry["description"].get<std::string>();
        return hit;
    }
    return hit;
}

}}} // namespace Slic3r::GUI::AmsMatch
