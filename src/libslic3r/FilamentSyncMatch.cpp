#include "FilamentSyncMatch.hpp"

#include <cctype>
#include <limits>

namespace Slic3r {
namespace FilamentSyncMatch {

bool parse_rgb(const std::string &text, unsigned int &rgb)
{
    size_t begin = 0;
    size_t end   = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    if (begin < end && text[begin] == '#')
        ++begin;
    const size_t len = end - begin;
    if (len != 6 && len != 8)
        return false;
    unsigned int value = 0;
    for (size_t i = begin; i < begin + 6; ++i) {
        const char c = text[i];
        unsigned int digit;
        if (c >= '0' && c <= '9')
            digit = unsigned(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = unsigned(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = unsigned(c - 'A' + 10);
        else
            return false;
        value = (value << 4) | digit;
    }
    // The alpha byte (RRGGBBAA, as the printer reports it) must still be hex, but is ignored.
    for (size_t i = begin + 6; i < end; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(text[i])))
            return false;
    rgb = value;
    return true;
}

unsigned int rgb_distance_sq(unsigned int a, unsigned int b)
{
    const int dr = int((a >> 16) & 0xff) - int((b >> 16) & 0xff);
    const int dg = int((a >> 8) & 0xff) - int((b >> 8) & 0xff);
    const int db = int(a & 0xff) - int(b & 0xff);
    return unsigned(dr * dr + dg * dg + db * db);
}

int closest_colour_index(const std::string &target, const std::vector<std::string> &candidate_colours)
{
    unsigned int target_rgb = 0;
    if (!parse_rgb(target, target_rgb))
        return -1;
    int          best          = -1;
    unsigned int best_distance = std::numeric_limits<unsigned int>::max();
    for (size_t i = 0; i < candidate_colours.size(); ++i) {
        unsigned int rgb = 0;
        if (!parse_rgb(candidate_colours[i], rgb))
            rgb = 0; // no default colour: treat as black
        const unsigned int distance = rgb_distance_sq(target_rgb, rgb);
        if (best < 0 || distance < best_distance) {
            best          = int(i);
            best_distance = distance;
        }
    }
    return best;
}

static std::string trimmed_lower(const std::string &text)
{
    size_t begin = 0;
    size_t end   = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    std::string out = text.substr(begin, end - begin);
    for (char &c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string base_name(const std::string &preset_name)
{
    std::string base = preset_name.substr(0, preset_name.find('@'));
    size_t      end  = base.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(base[end - 1])))
        --end;
    size_t begin = 0;
    while (begin < end && std::isspace(static_cast<unsigned char>(base[begin])))
        ++begin;
    return base.substr(begin, end - begin);
}

bool same_name(const std::string &a, const std::string &b) { return trimmed_lower(a) == trimmed_lower(b); }

bool is_stock_vendor(const std::string &vendor)
{
    const std::string v = trimmed_lower(vendor);
    return v.empty() || v == "generic" || v == "snapmaker" || v == "none";
}

Match match_preset(const std::vector<Candidate> &candidates, const std::string &reported_name, const std::string &reported_type,
                   const std::string &reported_vendor, const std::string &reported_colour)
{
    Match match;
    if (trimmed_lower(reported_name).empty())
        return match;

    // 1. The reported name, system presets first.
    for (int pass = 0; pass < 2 && match.index < 0; ++pass)
        for (size_t i = 0; i < candidates.size(); ++i) {
            const Candidate &c = candidates[i];
            if (!c.is_compatible || c.is_system != (pass == 0))
                continue;
            if (same_name(base_name(c.name), reported_name)) {
                match = { int(i), MatchKind::ExactName };
                break;
            }
        }
    if (match.index >= 0)
        return match;

    // 2. Same vendor and type, closest default colour.
    if (!trimmed_lower(reported_vendor).empty() && !reported_colour.empty()) {
        std::vector<int>         indices;
        std::vector<std::string> colours;
        for (size_t i = 0; i < candidates.size(); ++i) {
            const Candidate &c = candidates[i];
            if (!c.is_compatible || !same_name(c.vendor, reported_vendor) || !same_name(c.type, reported_type))
                continue;
            if (!c.is_base && is_stock_vendor(c.vendor))
                continue;
            indices.push_back(int(i));
            colours.push_back(c.colour);
        }
        const int best = closest_colour_index(reported_colour, colours);
        if (best >= 0)
            return { indices[size_t(best)], MatchKind::VendorType };
    }

    // 3. "Generic <type>".
    const std::string generic = "Generic " + reported_type;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const Candidate &c = candidates[i];
        if (c.is_compatible && c.is_system && same_name(c.type, reported_type) && same_name(base_name(c.name), generic))
            return { int(i), MatchKind::Generic };
    }
    return match;
}

std::string third_party_preset_name(const std::string &reported_name, const std::string &parent_name)
{
    std::string name = reported_name;
    for (char &c : name)
        if (c == '<' || c == '>' || c == ':' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' || c == '"' || c == '@')
            c = '-';
    name = base_name(name);
    if (name.empty())
        return {};
    const size_t at = parent_name.find(" @");
    return at == std::string::npos ? name : name + parent_name.substr(at);
}

} // namespace FilamentSyncMatch
} // namespace Slic3r
