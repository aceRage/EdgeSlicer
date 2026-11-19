#include "GCodeTemplateLint.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "PrintConfig.hpp"

namespace Slic3r {
namespace GCodeLint {

namespace {

// The PlaceholderParser's keyword table (PlaceholderParser.cpp, `keywords.add`), plus `then`,
// which the inline `if ... then ... endif` form uses.
const std::array<const char *, 28> s_keywords = {
    "and", "digits", "zdigits", "empty", "if", "int", "is_nil", "local", "else", "elsif",
    "endif", "false", "global", "interpolate_table", "min", "max", "random", "regex_replace",
    "repeat", "round", "ceil", "floor", "not", "one_of", "or", "size", "true", "then",
};

// Values the G-code generator sets on the PlaceholderParser while it writes G-code
// (placeholder_parser().set("...") and friends in GCode.cpp, GCode/*.cpp, Print.cpp, PrintBase.cpp)
// that are not config keys and not in the editor dialog's placeholder groups. Collected 2026-11-12;
// a name missing here only produces a warning in the editor, never a blocked save.
const char *const s_runtime_names[] = {
    "adaptive_bed_mesh_max", "adaptive_bed_mesh_min", "additional_fan_full_speed_layer", "bed_mesh_algo",
    "bed_mesh_probe_count", "bed_temperature", "bed_temperature_initial_layer", "bed_temperature_initial_layer_single",
    "bed_temperature_initial_layer_vector", "chamber_cooling_mode", "chamber_minimal_temperature", "chamber_temperature",
    "clear_to_x0", "close_additional_fan_first_x_layers", "color_change_extruder", "cooling_filter_enabled",
    "curr_accumulated_mass", "curr_layer_mass", "curr_physical_extruder_id", "current_extruder", "current_filament_id",
    "current_hotend", "current_nozzle_id", "current_object_idx", "day", "deretraction_from_wipe_tower_generator",
    "during_print_exhaust_fan_speed_num", "e_position", "e_restart_extra", "e_retracted", "extruded_volume",
    "extruded_volume_total", "extruded_weight", "extruded_weight_total", "extruder", "extrusion_role", "fan_speed",
    "farthest_point_timelapse_enabled", "filament_cooling_before_tower", "filament_extruder_id", "filament_flush_temp",
    "filament_flush_temp_fast", "filament_flush_volumetric_speed", "filament_map", "filament_multitool_ramming",
    "filament_name", "filament_retract_length_nc", "filament_tower_interface_print_temp",
    "filament_tower_interface_purge_volume", "filament_volume_type_list", "first_filaments", "first_flush_volume",
    "first_layer_bed_temperature", "first_layer_center_no_wipe_tower", "first_layer_height",
    "first_layer_print_convex_hull", "first_layer_print_max", "first_layer_print_min", "first_layer_print_size",
    "first_layer_temperature", "first_non_support_filaments", "first_non_support_hotend", "first_object_name",
    "first_x_layer_fan_speed", "flush_length", "flush_temperatures", "flush_volumetric_speeds",
    "has_single_extruder_multi_material_priming", "has_timelapse_safe_pos", "has_tpu_in_first_layer", "has_wipe_tower",
    "hold_chamber_temp_for_flat_print", "hour", "in_head_wrap_detect_zone", "initial_extruder", "initial_filament_id",
    "initial_hotend", "initial_layer_travel_acceleration", "initial_no_support_extruder",
    "initial_no_support_filament_id", "initial_no_support_hotend", "initial_no_support_nozzle_id",
    "initial_no_support_physical_nozzle_id", "initial_no_support_tool", "initial_nozzle_id", "initial_tool",
    "input_filename", "input_filename_base", "is_all_bbl_filament", "is_extruder_used", "is_prime_tower_interface",
    "last_extrusion_role", "layer_num", "layer_z", "long_retraction_when_cut", "long_retraction_when_ec",
    "long_retractions_when_cut", "long_retractions_when_ec", "max_additional_fan", "max_layer_z", "max_print_height",
    "max_print_z", "min_vitrification_temperature", "minute", "model_name", "month", "most_used_physical_extruder_id",
    "new_extruder_retracted_length", "new_filament_e_feedrate", "new_filament_temp", "new_retract_length",
    "new_retract_length_toolchange", "next_extruder", "next_filament_id", "next_hotend", "next_nozzle_id",
    "next_wipe_x", "next_wipe_y", "normal_print_time", "nozzle_diameter_at_nozzle_id", "nozzle_temperature",
    "nozzle_temperature_initial_layer", "nozzle_volume_types", "num_extruders", "num_filaments", "num_instances",
    "num_objects", "old_filament_e_feedrate", "old_filament_temp", "old_retract_length", "old_retract_length_toolchange",
    "outer_wall_speed", "outer_wall_volumetric_speed", "overall_chamber_temperature", "physical_nozzle_properties",
    "plate_name", "plate_number", "position", "previous_extruder", "print_bed_max", "print_bed_min", "print_bed_size",
    "print_time", "relative_e_axis", "restore_layer_z_before_toolchange", "retract_after_outer_wall", "retract_length",
    "retraction_distance_when_cut", "retraction_distance_when_ec", "retraction_distances_when_cut",
    "retraction_distances_when_ec", "retraction_length", "scale", "scan_first_layer", "second", "second_flush_volume",
    "silent_print_time", "temperature", "timelapse_inline_photo", "timelapse_pos_x", "timelapse_pos_y", "timestamp",
    "toolchange_count", "toolchange_z", "total_cost", "total_layer_count", "total_toolchanges", "total_weight",
    "total_wipe_tower_cost", "total_wipe_tower_filament", "travel_point_1_x", "travel_point_1_y", "travel_point_2_x",
    "travel_point_2_y", "travel_point_3_x", "travel_point_3_y", "used_filament", "user", "version",
    "wipe_avoid_perimeter", "wipe_avoid_pos_x", "wipe_inward", "wipe_tower_center_pos_valid", "wipe_tower_center_pos_x",
    "wipe_tower_center_pos_y", "x_after_toolchange", "y_after_toolchange", "year", "z_after_toolchange", "z_offset",
    "zhop",
};

inline bool is_alpha_(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
inline bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
inline bool is_alnum_(unsigned char c) { return is_alpha_(c) || is_digit(c); }
inline bool is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

class Analyzer
{
public:
    Analyzer(const std::string &text, const KnownFn &known) : s(text), n(text.size()), known(known) {}

    Result run()
    {
        collect_declarations();
        while (i < n) {
            const unsigned char c = s[i];
            if (c == '\n') {
                in_comment = false;
                line_start = true;
                ++i;
            } else if (c == '{') {
                macro();
                line_start = false;
            } else if (c == '}') {
                add_span(i, 1, Style::Brace);
                add_issue(i, 1, Severity::Error, Code::StrayClosingBrace);
                ++i;
                line_start = false;
            } else if (c == '[') {
                legacy();
                line_start = false;
            } else if (in_comment || c == ';') {
                in_comment = true;
                size_t j = i;
                while (j < n && s[j] != '\n' && s[j] != '{' && s[j] != '}' && s[j] != '[')
                    ++j;
                add_span(i, j - i, Style::Comment);
                i = j;
            } else if (is_space(c)) {
                ++i;
            } else if (is_alpha_(c)) {
                gcode_word();
                line_start = false;
            } else if (is_digit(c) || ((c == '-' || c == '+' || c == '.') && i + 1 < n && is_digit(s[i + 1]))) {
                const bool boundary = i == 0 || !is_alnum_(s[i - 1]);
                const size_t j = scan_number(c == '-' || c == '+' ? i + 1 : i);
                if (boundary)
                    add_span(i, j - i, Style::Number);
                i = j;
                line_start = false;
            } else {
                ++i;
                line_start = false;
            }
        }
        for (const IfEntry &e : ifs)
            add_issue(e.pos, e.len, Severity::Error, Code::IfWithoutEndif, "if");
        finish();
        return std::move(r);
    }

private:
    struct IfEntry
    {
        size_t pos;
        size_t len;
        int    macro_id;
        bool   inline_if;
    };

    const std::string &s;
    const size_t       n;
    const KnownFn     &known;
    Result             r;
    size_t             i          = 0;
    bool               line_start = true;
    bool               in_comment = false;
    int                macro_count = 0;
    std::vector<IfEntry>            ifs;
    std::unordered_set<std::string> declared;

    void add_span(size_t start, size_t len, Style style)
    {
        if (len == 0 || style == Style::Default)
            return;
        if (!r.spans.empty()) {
            Span &last = r.spans.back();
            if (last.style == style && last.start + last.length == start) {
                last.length += len;
                return;
            }
        }
        r.spans.push_back({start, len, style});
    }

    void add_issue(size_t start, size_t len, Severity sev, Code code, std::string name = {})
    {
        r.issues.push_back({start, std::max<size_t>(len, 1), 0, sev, code, std::move(name)});
    }

    void finish()
    {
        std::stable_sort(r.issues.begin(), r.issues.end(), [](const Issue &a, const Issue &b) { return a.start < b.start; });
        // Line numbers.
        size_t pos = 0;
        int    line = 1;
        for (Issue &is : r.issues) {
            const size_t target = std::min(is.start, n);
            for (; pos < target; ++pos)
                if (s[pos] == '\n')
                    ++line;
            is.line = line;
        }
    }

    size_t scan_ident(size_t j) const
    {
        while (j < n && is_alnum_(s[j]))
            ++j;
        return j;
    }

    // Digits with an optional fraction and exponent, starting at j (after any sign).
    size_t scan_number(size_t j) const
    {
        while (j < n && is_digit(s[j]))
            ++j;
        if (j < n && s[j] == '.') {
            ++j;
            while (j < n && is_digit(s[j]))
                ++j;
        }
        if (j + 1 < n && (s[j] == 'e' || s[j] == 'E') &&
            (is_digit(s[j + 1]) || ((s[j + 1] == '-' || s[j + 1] == '+') && j + 2 < n && is_digit(s[j + 2])))) {
            j += 2;
            while (j < n && is_digit(s[j]))
                ++j;
        }
        return j;
    }

    bool name_known(const std::string &name) const
    {
        return !known || declared.count(name) > 0 || known(name);
    }

    // `local name` / `global name` anywhere in the template declares `name` for the whole template:
    // the order of declarations across {if} branches is not something a lexer can follow.
    void collect_declarations()
    {
        size_t j = 0;
        while (j < n) {
            if (is_alpha_(s[j]) && (j == 0 || !is_alnum_(s[j - 1]))) {
                const size_t e = scan_ident(j);
                const std::string word(s, j, e - j);
                j = e;
                if (word == "local" || word == "global") {
                    size_t k = j;
                    while (k < n && is_space(s[k]))
                        ++k;
                    if (k < n && is_alpha_(s[k])) {
                        const size_t e2 = scan_ident(k);
                        declared.emplace(s, k, e2 - k);
                        j = e2;
                    }
                }
            } else
                ++j;
        }
    }

    // A word in G-code text: a command at the start of a line, a parameter letter, or plain text.
    void gcode_word()
    {
        const size_t start    = i;
        const bool   boundary = i == 0 || !is_alnum_(s[i - 1]);
        const unsigned char c = s[i];
        const unsigned char next = i + 1 < n ? s[i + 1] : 0;
        if (line_start) {
            if ((c == 'G' || c == 'M' || c == 'T' || c == 'g' || c == 'm' || c == 't') && (is_digit(next) || next == '{' || next == '[')) {
                const size_t j = is_digit(next) ? scan_number(i + 1) : i + 1;
                add_span(start, j - start, Style::Command);
                i = j;
                return;
            }
            const size_t j = scan_ident(i);
            add_span(start, j - start, Style::Command);
            i = j;
            return;
        }
        if (boundary && !is_alpha_(next) && !is_digit(next) && c != '_' &&
            (next == '-' || next == '+' || next == '.' || next == '{' || next == '[')) {
            // X-1.5, E.4, S{temp}, S[temp]
            size_t j = i + 1;
            if (next == '-' || next == '+' || next == '.')
                j = (next == '.') ? scan_number(i + 1) : scan_number(i + 2);
            add_span(start, j - start, Style::Param);
            i = j;
            return;
        }
        if (boundary && is_digit(next) && c != '_') {
            // X10, S200, F3000.5: one letter followed by a number.
            const size_t j = scan_number(i + 1);
            if (j >= n || !is_alpha_(s[j])) {
                add_span(start, j - start, Style::Param);
                i = j;
                return;
            }
        }
        const size_t j = scan_ident(i);
        if (boundary && j < n && s[j] == '=')
            add_span(start, j - start, Style::Param); // Klipper style EXTRUDER=200
        i = j;
    }

    // [name] or [name[index_name]]
    void legacy()
    {
        const size_t open = i;
        size_t       j    = i + 1;
        if (j < n && is_alpha_(s[j])) {
            const size_t e = scan_ident(j);
            if (e < n && s[e] == ']') {
                add_span(open, 1, Style::Brace);
                add_span(j, e - j, Style::Variable);
                add_span(e, 1, Style::Brace);
                check_legacy_name(j, e);
                i = e + 1;
                return;
            }
            if (e < n && s[e] == '[' && e + 1 < n && is_alpha_(s[e + 1])) {
                const size_t e2 = scan_ident(e + 1);
                if (e2 + 1 < n && s[e2] == ']' && s[e2 + 1] == ']') {
                    add_span(open, 1, Style::Brace);
                    add_span(j, e - j, Style::Variable);
                    add_span(e, 1, Style::Operator);
                    add_span(e + 1, e2 - e - 1, Style::Variable);
                    add_span(e2, 1, Style::Operator);
                    add_span(e2 + 1, 1, Style::Brace);
                    check_legacy_name(j, e);
                    check_name(e + 1, e2);
                    i = e2 + 2;
                    return;
                }
            }
        }
        add_span(open, 1, Style::Brace);
        add_issue(open, 1, Severity::Error, Code::MalformedLegacy);
        i = open + 1;
    }

    void check_name(size_t b, size_t e)
    {
        std::string name(s, b, e - b);
        if (!name_known(name))
            add_issue(b, e - b, Severity::Warning, Code::UnknownPlaceholder, std::move(name));
    }

    // A legacy placeholder may index a vector as name_N.
    void check_legacy_name(size_t b, size_t e)
    {
        std::string name(s, b, e - b);
        if (name_known(name))
            return;
        const size_t us = name.rfind('_');
        if (us != std::string::npos && us + 1 < name.size() && us > 0 &&
            std::all_of(name.begin() + us + 1, name.end(), [](char ch) { return is_digit(ch); }) &&
            name_known(name.substr(0, us)))
            return;
        add_issue(b, e - b, Severity::Warning, Code::UnknownPlaceholder, std::move(name));
    }

    // { expression } - the macro language.
    void macro()
    {
        const size_t open = i;
        const int    id   = ++macro_count;
        add_span(i, 1, Style::Brace);
        ++i;
        // Rollback marks: an expression that is never closed is reported once and the text after its
        // '{' is lexed again as G-code, so one missing '}' does not flag the rest of the template.
        const size_t                 spans_mark  = r.spans.size();
        const size_t                 issues_mark = r.issues.size();
        const std::vector<IfEntry>   ifs_mark    = ifs;
        std::vector<std::pair<char, size_t>> brackets;
        std::string prev; // last significant token, for telling a regex from a division
        bool closed       = false;
        bool string_error = false;
        while (i < n) {
            const unsigned char c = s[i];
            if (c == '}') {
                for (const auto &b : brackets)
                    add_issue(b.second, 1, Severity::Error, Code::UnbalancedBracket);
                add_span(i, 1, Style::Brace);
                ++i;
                closed = true;
                break;
            }
            if (c == '{')
                break; // a new expression starts: this one was never closed
            if (is_space(c)) {
                ++i;
                continue;
            }
            if (c == '"') {
                const size_t b = i;
                size_t       j = i + 1;
                while (j < n && s[j] != '"') {
                    if (s[j] == '\\' && j + 1 < n)
                        ++j;
                    ++j;
                }
                if (j >= n) {
                    add_span(b, n - b, Style::String);
                    add_issue(b, 1, Severity::Error, Code::UnclosedString);
                    i            = n;
                    string_error = true;
                    break;
                }
                add_span(b, j + 1 - b, Style::String);
                i    = j + 1;
                prev = "\"";
                continue;
            }
            if (c == '/' && (prev == "=~" || prev == "!~" || prev == "," || prev == "(")) {
                const size_t b = i;
                size_t       j = i + 1;
                while (j < n && s[j] != '/' && s[j] != '\n') {
                    if (s[j] == '\\' && j + 1 < n)
                        ++j;
                    ++j;
                }
                if (j >= n || s[j] != '/') {
                    add_span(b, j - b, Style::String);
                    add_issue(b, 1, Severity::Error, Code::UnclosedString);
                    i = j;
                    continue;
                }
                add_span(b, j + 1 - b, Style::String);
                i    = j + 1;
                prev = "/re/";
                continue;
            }
            if (is_alpha_(c)) {
                const size_t e = scan_ident(i);
                std::string  word(s, i, e - i);
                if (is_keyword(word)) {
                    add_span(i, e - i, Style::Keyword);
                    keyword(word, i, e - i, id);
                } else {
                    add_span(i, e - i, Style::Variable);
                    check_name(i, e);
                }
                prev = std::move(word);
                i    = e;
                continue;
            }
            if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(s[i + 1]))) {
                const size_t e = scan_number(i);
                add_span(i, e - i, Style::Number);
                prev = "0";
                i    = e;
                continue;
            }
            if (c == '(' || c == '[') {
                brackets.emplace_back(char(c), i);
                add_span(i, 1, Style::Operator);
                prev.assign(1, char(c));
                ++i;
                continue;
            }
            if (c == ')' || c == ']') {
                const char want = c == ')' ? '(' : '[';
                if (brackets.empty() || brackets.back().first != want)
                    add_issue(i, 1, Severity::Error, Code::UnbalancedBracket);
                else
                    brackets.pop_back();
                add_span(i, 1, Style::Operator);
                prev.assign(1, char(c));
                ++i;
                continue;
            }
            if (c < 0x80 && std::strchr("+-*/%<>=!&|?:,;~", c) != nullptr) {
                size_t len = 1;
                if (i + 1 < n) {
                    const char two[3] = {char(c), s[i + 1], 0};
                    static const char *const ops2[] = {"==", "!=", "<>", "<=", ">=", "&&", "||", "=~", "!~"};
                    for (const char *op : ops2)
                        if (std::strcmp(two, op) == 0)
                            len = 2;
                }
                add_span(i, len, Style::Operator);
                prev.assign(s, i, len);
                i += len;
                continue;
            }
            ++i; // anything else (non-ASCII) is left unstyled; the parser reports it at slice time
        }
        if (!closed) {
            if (string_error) {
                // The string swallowed the closing '}': the string is the mistake to report.
                while (!ifs.empty() && ifs.back().macro_id == id && ifs.back().inline_if)
                    ifs.pop_back();
                return;
            }
            r.spans.resize(spans_mark);
            r.issues.resize(issues_mark);
            ifs = ifs_mark;
            add_issue(open, 1, Severity::Error, Code::UnclosedBrace);
            i = open + 1;
            return;
        }
        while (!ifs.empty() && ifs.back().macro_id == id && ifs.back().inline_if) {
            add_issue(ifs.back().pos, ifs.back().len, Severity::Error, Code::IfWithoutEndif, "if");
            ifs.pop_back();
        }
    }

    void keyword(const std::string &word, size_t pos, size_t len, int id)
    {
        if (word == "if") {
            ifs.push_back({pos, len, id, false});
        } else if (word == "then") {
            if (!ifs.empty() && ifs.back().macro_id == id)
                ifs.back().inline_if = true;
        } else if (word == "else" || word == "elsif") {
            if (ifs.empty())
                add_issue(pos, len, Severity::Error, Code::ElseWithoutIf, word);
        } else if (word == "endif") {
            if (ifs.empty())
                add_issue(pos, len, Severity::Error, Code::EndifWithoutIf, word);
            else
                ifs.pop_back();
        }
    }
};

template<class Def> void add_keys(std::unordered_set<std::string> &out, const Def &def)
{
    for (const auto &kv : def.options)
        out.insert(kv.first);
}

} // namespace

size_t Result::errors() const
{
    return size_t(std::count_if(issues.begin(), issues.end(), [](const Issue &is) { return is.severity == Severity::Error; }));
}

size_t Result::warnings() const
{
    return size_t(std::count_if(issues.begin(), issues.end(), [](const Issue &is) { return is.severity == Severity::Warning; }));
}

Result analyze(const std::string &text, const KnownFn &is_known)
{
    return Analyzer(text, is_known).run();
}

bool is_keyword(const std::string &word)
{
    for (const char *kw : s_keywords)
        if (word == kw)
            return true;
    return false;
}

const std::unordered_set<std::string> &known_placeholder_names()
{
    static const std::unordered_set<std::string> names = [] {
        std::unordered_set<std::string> out;
        add_keys(out, print_config_def);
        add_keys(out, custom_gcode_specific_config_def);
        add_keys(out, ReadOnlySlicingStatesConfigDef());
        add_keys(out, ReadWriteSlicingStatesConfigDef());
        add_keys(out, OtherSlicingStatesConfigDef());
        add_keys(out, PrintStatisticsConfigDef());
        add_keys(out, ObjectsInfoConfigDef());
        add_keys(out, DimensionsConfigDef());
        add_keys(out, TemperaturesConfigDef());
        add_keys(out, TimestampsConfigDef());
        add_keys(out, OtherPresetsConfigDef());
        for (const char *name : s_runtime_names)
            out.insert(name);
        return out;
    }();
    return names;
}

} // namespace GCodeLint
} // namespace Slic3r
