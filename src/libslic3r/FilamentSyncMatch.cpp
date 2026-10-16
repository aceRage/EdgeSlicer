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

} // namespace FilamentSyncMatch
} // namespace Slic3r
