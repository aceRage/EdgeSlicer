#include "GlowFilament.hpp"

#include "../Config.hpp"
#include "../PresetBundle.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Slic3r {
namespace Glow {

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool has_glow_word(const std::string &s) { return lower(s).find("glow") != std::string::npos; }

RGB clamp01(const RGB &c)
{
    return RGB{ std::clamp(c[0], 0.f, 1.f), std::clamp(c[1], 0.f, 1.f), std::clamp(c[2], 0.f, 1.f) };
}

std::string hex_of(const RGB &srgb)
{
    const RGB c = clamp01(srgb);
    return MultiColour::format_hex8(int(std::lround(c[0] * 255.f)), int(std::lround(c[1] * 255.f)), int(std::lround(c[2] * 255.f)));
}

} // namespace

bool suggests_glow(const std::string &preset_name, const std::string &filament_type)
{
    return has_glow_word(preset_name) || has_glow_word(filament_type);
}

bool resolve_glow(int state, const std::string &preset_name, const std::string &filament_type)
{
    if (state == GLOW_ON)
        return true;
    if (state == GLOW_OFF)
        return false;
    return suggests_glow(preset_name, filament_type);
}

RGB vibrant(const RGB &srgb)
{
    const RGB   c  = clamp01(srgb);
    const float mx = std::max({ c[0], c[1], c[2] });
    const float mn = std::min({ c[0], c[1], c[2] });
    const float s  = mx > 0.f ? (mx - mn) / mx : 0.f;
    if (s < GREY_SAT)
        return GREY_GLOW;
    // The hue is kept by keeping where the middle channel sits between the smallest and the largest:
    // t is 0 for the smallest, 1 for the largest. HSV puts the channels at v (1 - s) + v s t.
    const float s2 = s + (1.f - s) * SAT_LIFT;
    const float v2 = mx + (1.f - mx) * VAL_LIFT;
    RGB         out;
    for (int i = 0; i < 3; ++i)
        out[size_t(i)] = v2 * ((1.f - s2) + s2 * ((c[size_t(i)] - mn) / (mx - mn)));
    return clamp01(out);
}

bool vibrant_hex(const std::string &hex, std::string &out)
{
    RGB c;
    if (!MultiColour::parse_hex(hex, c))
        return false;
    out = hex_of(vibrant(c));
    return true;
}

RGB Slot::glow_srgb(const RGB &daylight_srgb) const
{
    RGB c;
    if (!colour.empty() && MultiColour::parse_hex(colour, c))
        return c;
    return vibrant(daylight_srgb);
}

Slot slot_from_config(const ConfigBase &config, size_t slot, const std::string &preset_name, const std::string &filament_type)
{
    Slot out;
    int  state = GLOW_AUTO;
    if (const auto *o = dynamic_cast<const ConfigOptionInts*>(config.option("filament_glow")); o != nullptr && slot < o->values.size())
        state = std::clamp(o->values[slot], GLOW_AUTO, GLOW_ON);
    out.on = resolve_glow(state, preset_name, filament_type);
    if (const auto *o = dynamic_cast<const ConfigOptionStrings*>(config.option("filament_glow_colour")); o != nullptr && slot < o->values.size()) {
        RGB c;
        if (MultiColour::parse_hex(o->values[slot], c))
            out.colour = hex_of(c);
    }
    return out;
}

float intensity_from_config(const ConfigBase &config)
{
    float percent = INTENSITY_DEFAULT_PERCENT;
    if (const auto *o = dynamic_cast<const ConfigOptionFloat*>(config.option("filament_glow_intensity")); o != nullptr && std::isfinite(o->value))
        percent = float(o->value);
    return std::clamp(percent, 0.f, INTENSITY_MAX_PERCENT) / 100.f;
}

namespace {

// The type of the filament preset of slot `i`, "" when it has none.
void preset_info(const PresetBundle &bundle, size_t i, std::string &name, std::string &type)
{
    name.clear();
    type.clear();
    if (i >= bundle.filament_presets.size())
        return;
    name = Preset::remove_suffix_modified(bundle.filament_presets[i]);
    if (const Preset *p = bundle.filaments.find_preset(name, false); p != nullptr) {
        if (const auto *t = dynamic_cast<const ConfigOptionStrings*>(p->config.option("filament_type")); t != nullptr && !t->values.empty())
            type = t->values.front();
    }
}

} // namespace

std::vector<Slot> slots_from_bundle(const PresetBundle &bundle)
{
    std::vector<Slot> slots;
    const size_t      n = bundle.filament_presets.size();
    for (size_t i = 0; i < n; ++i) {
        std::string name, type;
        preset_info(bundle, i, name, type);
        slots.push_back(slot_from_config(bundle.project_config, i, name, type));
    }
    return slots;
}

std::string signature(const PresetBundle &bundle)
{
    std::string sig;
    for (const char *key : { "filament_glow", "filament_glow_colour", "filament_glow_intensity" })
        if (const ConfigOption *o = bundle.project_config.option(key); o != nullptr) {
            sig += key;
            sig += '=';
            sig += o->serialize();
            sig += ';';
        }
    for (size_t i = 0; i < bundle.filament_presets.size(); ++i) {
        std::string name, type;
        preset_info(bundle, i, name, type);
        sig += (suggests_glow(name, type) ? "g" : "n");
    }
    return sig;
}

bool any_glow(const std::vector<Slot> &slots, const std::vector<unsigned char> &used)
{
    for (const unsigned char id : used)
        if (size_t(id) < slots.size() && slots[size_t(id)].on)
            return true;
    return false;
}

UniformTable pack_uniforms(const std::vector<Slot> &slots, float intensity)
{
    UniformTable t;
    t.intensity = std::max(intensity, 0.f);
    for (size_t i = 0; i < slots.size() && i < size_t(MultiColour::GLSL_SLOTS); ++i) {
        const Slot &s = slots[i];
        if (!s.on)
            continue;
        t.any = true;
        RGB c;
        if (!s.colour.empty() && MultiColour::parse_hex(s.colour, c)) {
            const RGB lin = MultiColour::srgb_to_linear(c);
            t.slot[i * 4 + 0] = 2.f;
            t.slot[i * 4 + 1] = lin[0];
            t.slot[i * 4 + 2] = lin[1];
            t.slot[i * 4 + 3] = lin[2];
        } else
            t.slot[i * 4 + 0] = 1.f;
    }
    return t;
}

std::string lights_off_hex(const std::string &hex, const Slot &slot, float intensity, float ambient)
{
    RGB c;
    if (!MultiColour::parse_hex(hex, c))
        return hex;
    RGB lin;
    if (slot.on) {
        lin = MultiColour::srgb_to_linear(slot.glow_srgb(c));
        for (float &v : lin)
            v = std::min(v * std::max(intensity, 0.f), 1.f);
    } else {
        lin = MultiColour::srgb_to_linear(c);
        for (float &v : lin)
            v *= std::max(ambient, 0.f);
    }
    return hex_of(MultiColour::linear_to_srgb(lin));
}

void apply_lights_off(MultiColour::UniformTable &table, const std::vector<Slot> &slots, float intensity, float ambient)
{
    for (size_t s = 0; s < size_t(MultiColour::GLSL_SLOTS); ++s) {
        const Slot slot = s < slots.size() ? slots[s] : Slot();
        for (size_t i = 0; i < size_t(MultiColour::MAX_COLOURS); ++i) {
            float *c = &table.colors[(s * size_t(MultiColour::MAX_COLOURS) + i) * 4];
            RGB    lin{ c[0], c[1], c[2] };
            if (slot.on) {
                lin = MultiColour::srgb_to_linear(slot.glow_srgb(MultiColour::linear_to_srgb(lin)));
                for (float &v : lin)
                    v = std::min(v * std::max(intensity, 0.f), 1.f);
            } else
                for (float &v : lin)
                    v *= std::max(ambient, 0.f);
            c[0] = lin[0];
            c[1] = lin[1];
            c[2] = lin[2];
        }
    }
}

} // namespace Glow
} // namespace Slic3r
