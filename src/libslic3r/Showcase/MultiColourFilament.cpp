#include "MultiColourFilament.hpp"

#include "../Config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/trim.hpp>

namespace Slic3r {
namespace MultiColour {

namespace {

constexpr double PI = 3.14159265358979323846;

float wrap_turns(float t)
{
    t -= std::floor(t);
    // floor() of a value just below an integer can leave exactly 1.f after rounding.
    return t >= 1.f ? 0.f : t;
}

float smoothstep(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

RGB mix(const RGB &a, const RGB &b, float t)
{
    return { a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t };
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<std::string> split_bar(const std::string &value)
{
    std::vector<std::string> parts;
    boost::algorithm::split(parts, value, boost::algorithm::is_any_of("|"));
    for (std::string &p : parts)
        boost::algorithm::trim(p);
    return parts;
}

std::array<float, MAX_COLOURS> equal_split(int n)
{
    std::array<float, MAX_COLOURS> p{ 0.f, 0.f, 0.f, 0.f };
    n = std::clamp(n, 1, MAX_COLOURS);
    for (int i = 0; i < n; ++i)
        p[i] = 1.f / float(n);
    return p;
}

// The string at slot of a strings option, or nullptr when the key or the slot is missing.
const std::string *string_at(const ConfigBase &config, const char *key, size_t slot)
{
    const auto *opt = dynamic_cast<const ConfigOptionStrings*>(config.option(key));
    return opt != nullptr && slot < opt->values.size() ? &opt->values[slot] : nullptr;
}

} // namespace

float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float c)
{
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

RGB srgb_to_linear(const RGB &c) { return { srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2]) }; }
RGB linear_to_srgb(const RGB &c) { return { linear_to_srgb(c[0]), linear_to_srgb(c[1]), linear_to_srgb(c[2]) }; }

bool parse_hex(const std::string &hex, RGB &out_srgb)
{
    if ((hex.size() != 7 && hex.size() != 9) || hex[0] != '#')
        return false;
    int v[8];
    for (size_t i = 1; i < hex.size(); ++i)
        if ((v[i - 1] = hex_digit(hex[i])) < 0)
            return false;
    for (int c = 0; c < 3; ++c)
        out_srgb[c] = float(v[2 * c] * 16 + v[2 * c + 1]) / 255.f;
    return true;
}

float Filament::boundary(int i) const
{
    if (i <= 0)
        return 0.f;
    if (i >= n)
        return 1.f;
    float a = 0.f;
    for (int k = 0; k < i; ++k)
        a += proportion[k];
    return a;
}

float Filament::blend_turns() const
{
    float narrowest = 1.f;
    for (int i = 0; i < n; ++i)
        narrowest = std::min(narrowest, proportion[i]);
    return std::min(BLEND_TURNS, narrowest);
}

RGB Filament::average() const
{
    RGB sum{ 0.f, 0.f, 0.f };
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c)
            sum[c] += colour[i][c] * (n == 1 ? 1.f : proportion[i]);
    return sum;
}

BeadParams bead_params(bool wall)
{
    BeadParams p;
    p.blend_turns    = wall ? BLEND_TURNS_WALL : BLEND_TURNS_TOP;
    p.stack_up_k     = STACK_UP_K;
    p.visible_height = wall ? WALL_VISIBLE_HEIGHT : 1.f;
    return p;
}

RGB Filament::colour_at(float phi) const
{
    return colour_at(phi, BLEND_TURNS);
}

RGB Filament::colour_at(float phi, float blend) const
{
    if (n <= 1)
        return colour[0];
    phi = wrap_turns(phi);
    int i = n - 1;
    while (i > 0 && phi < boundary(i))
        --i;
    float narrowest = 1.f;
    for (int k = 0; k < n; ++k)
        narrowest = std::min(narrowest, proportion[k]);
    const float h  = 0.5f * std::min(blend, narrowest);
    const float ds = phi - boundary(i);     // past the start of slice i
    const float de = boundary(i + 1) - phi; // before its end
    if (h > 0.f && ds < h)
        return mix(colour[(i + n - 1) % n], colour[i], smoothstep(-h, h, ds));
    if (h > 0.f && de < h)
        return mix(colour[i], colour[(i + 1) % n], smoothstep(-h, h, -de));
    return colour[i];
}

float Filament::filament_angle(float dir_turns, float filament_length_m) const
{
    // Where the middle of colour 1 points in the printer's frame after the twist so far.
    const float colour1_dir = (rotation_deg + twist_deg_per_m * filament_length_m) / 360.f;
    return wrap_turns(dir_turns - colour1_dir + 0.5f * proportion[0]);
}

RGB Filament::bead_colour(float sx, float sy, float rx, float ry, float dx, float dy, float filament_length_m) const
{
    return bead_colour(sx, sy, rx, ry, dx, dy, filament_length_m, BeadParams());
}

RGB Filament::bead_colour(float sx, float sy, float rx, float ry, float dx, float dy, float filament_length_m,
                          const BeadParams &bead) const
{
    if (n <= 1)
        return colour[0];
    // The source direction in the printer's XY plane (design 2.2): sideways across the bead, tilted
    // towards the travel at the bottom (leading material) and against it at the top (trailing).
    sy *= bead.visible_height;
    float vx = sx * rx - bead.stack_up_k * sy * dx;
    float vy = sx * ry - bead.stack_up_k * sy * dy;
    if (vx * vx + vy * vy < 1e-12f) {
        // The bead's centre line: never visible, take the right side.
        vx = rx;
        vy = ry;
        if (vx * vx + vy * vy < 1e-12f)
            return average();
    }
    return colour_at(filament_angle(direction_turns(vx, vy), filament_length_m), bead.blend_turns);
}

float direction_turns(float x, float y)
{
    return wrap_turns(float(std::atan2(double(y), double(x)) / (2. * PI)) + 0.25f);
}

std::array<float, MAX_COLOURS> parse_split(const std::string &value, int n)
{
    n = std::clamp(n, 1, MAX_COLOURS);
    if (n == 1 || value.empty())
        return equal_split(n);
    const std::vector<std::string> parts = split_bar(value);
    if (int(parts.size()) != n)
        return equal_split(n);
    std::array<float, MAX_COLOURS> p{ 0.f, 0.f, 0.f, 0.f };
    double sum = 0.;
    for (int i = 0; i < n; ++i) {
        const char *s   = parts[i].c_str();
        char       *end = nullptr;
        const double v  = std::strtod(s, &end);
        if (parts[i].empty() || end == nullptr || *end != '\0' || !std::isfinite(v) || v <= 0.)
            return equal_split(n);
        p[i] = float(v);
        sum += v;
    }
    for (int i = 0; i < n; ++i)
        p[i] = float(p[i] / sum);
    return p;
}

std::string format_split(const std::array<float, MAX_COLOURS> &proportion, int n)
{
    n = std::clamp(n, 1, MAX_COLOURS);
    if (n == 1)
        return {};
    bool equal = true;
    for (int i = 0; i < n; ++i)
        equal &= std::abs(proportion[i] - 1.f / float(n)) < 0.005f;
    if (equal)
        return {};
    std::string out;
    for (int i = 0; i < n; ++i) {
        if (i > 0)
            out += '|';
        out += std::to_string(int(std::lround(proportion[i] * 100.f)));
    }
    return out;
}

std::array<int, MAX_COLOURS> quantize_percent(const std::array<float, MAX_COLOURS> &proportion, int n)
{
    n = std::clamp(n, 1, MAX_COLOURS);
    std::array<int, MAX_COLOURS> q{ 0, 0, 0, 0 };
    if (n == 1) {
        q[0] = 100;
        return q;
    }
    float total = 0.f;
    for (int i = 0; i < n; ++i)
        total += std::max(proportion[i], 0.f);
    int sum = 0;
    for (int i = 0; i < n; ++i) {
        const float pct = total > 0.f ? 100.f * std::max(proportion[i], 0.f) / total : 100.f / float(n);
        q[i] = std::max(MIN_PERCENT, int(std::lround(pct / float(STEP_PERCENT))) * STEP_PERCENT);
        sum += q[i];
    }
    // Make it sum to 100: take from the largest (first on a tie) that can give, give to the largest.
    while (sum > 100) {
        int k = -1;
        for (int i = 0; i < n; ++i)
            if (q[i] > MIN_PERCENT && (k < 0 || q[i] > q[k]))
                k = i;
        if (k < 0)
            break;
        q[k] -= STEP_PERCENT;
        sum -= STEP_PERCENT;
    }
    while (sum < 100) {
        int k = 0;
        for (int i = 1; i < n; ++i)
            if (q[i] > q[k])
                k = i;
        q[k] += STEP_PERCENT;
        sum += STEP_PERCENT;
    }
    return q;
}

Filament from_config(const ConfigBase &config, size_t slot)
{
    Filament f;
    RGB      primary{ 0.5f, 0.5f, 0.5f };
    bool     have_primary = false;
    if (const std::string *c = string_at(config, "filament_colour", slot))
        have_primary = parse_hex(*c, primary);

    std::vector<RGB> colours;
    if (const std::string *multi = string_at(config, "filament_multi_colors", slot); multi != nullptr && !multi->empty())
        for (const std::string &part : split_bar(*multi)) {
            RGB c;
            if (parse_hex(part, c) && int(colours.size()) < MAX_COLOURS)
                colours.push_back(c);
        }

    int mode = 0;
    if (const auto *modes = dynamic_cast<const ConfigOptionInts*>(config.option("filament_colour_mode"));
        modes != nullptr && slot < modes->values.size())
        mode = modes->values[slot];

    if (mode == 1 || colours.size() <= 1) {
        // Gradient (P6) and single-colour slots: exactly filament_colour, as today.
        f.n         = 1;
        f.colour[0] = srgb_to_linear(have_primary ? primary : (colours.empty() ? primary : colours.front()));
    } else {
        f.n = int(colours.size());
        for (int i = 0; i < f.n; ++i)
            f.colour[i] = srgb_to_linear(colours[i]);
    }

    const std::string *split = string_at(config, "filament_colour_split", slot);
    f.proportion = parse_split(split != nullptr ? *split : std::string(), f.n);
    if (const auto *angle = dynamic_cast<const ConfigOptionFloats*>(config.option("filament_colour_angle"));
        angle != nullptr && slot < angle->values.size() && std::isfinite(angle->values[slot]))
        f.rotation_deg = float(angle->values[slot]);
    if (const auto *twist = dynamic_cast<const ConfigOptionFloats*>(config.option("filament_colour_twist"));
        twist != nullptr && slot < twist->values.size() && std::isfinite(twist->values[slot]))
        f.twist_deg_per_m = float(twist->values[slot]);
    return f;
}

} // namespace MultiColour
} // namespace Slic3r
