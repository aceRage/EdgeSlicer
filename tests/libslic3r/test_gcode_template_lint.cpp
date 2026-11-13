#include <catch2/catch.hpp>

#include "libslic3r/GCodeTemplateLint.hpp"
#include "libslic3r/Config.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GCodeLint;

namespace {

KnownFn known_set(std::set<std::string> names)
{
    return [names = std::move(names)](const std::string &n) { return names.count(n) > 0; };
}

const KnownFn &test_known()
{
    static const KnownFn fn = known_set({"temperature", "bed_temperature", "current_extruder", "layer_z", "layer_num",
                                         "filament_type", "first_layer_temperature", "nozzle_diameter"});
    return fn;
}

std::vector<Code> codes(const Result &r)
{
    std::vector<Code> out;
    for (const Issue &is : r.issues)
        out.push_back(is.code);
    return out;
}

// The style of the byte at `pos` (Default when no span covers it).
Style style_at(const Result &r, size_t pos)
{
    for (const Span &s : r.spans)
        if (pos >= s.start && pos < s.start + s.length)
            return s.style;
    return Style::Default;
}

Style style_of(const Result &r, const std::string &text, const std::string &needle, size_t from = 0)
{
    const size_t pos = text.find(needle, from);
    REQUIRE(pos != std::string::npos);
    return style_at(r, pos);
}

} // namespace

TEST_CASE("G-code template lint: clean templates have no issues", "[GCodeTemplateLint]")
{
    const std::string text =
        "G28 ; home all\n"
        "M104 S{temperature[current_extruder]} ; nozzle\n"
        "M140 S[bed_temperature]\n"
        "{if layer_num == 0}\n"
        "G1 Z{layer_z + 0.2} F600\n"
        "{elsif layer_num > 3}M117 later\n"
        "{else}\n"
        "M117 [first_layer_temperature_0]\n"
        "{endif}\n"
        "{local purge = 5.0; global g_count = 1}G1 E{purge} ; {g_count}\n"
        "{if one_of(filament_type[current_extruder], \"TPU\", \"PVA\") then 1 else 2 endif}\n"
        "M117 {temperature[current_extruder] > 200 ? \"hot\" : \"cold\"}\n"
        "T[current_extruder]\n";
    const Result r = analyze(text, test_known());
    INFO("first issue line " << (r.issues.empty() ? 0 : r.issues.front().line));
    CHECK(r.issues.empty());
    CHECK(r.errors() == 0);
    CHECK(r.warnings() == 0);
}

TEST_CASE("G-code template lint: highlighting spans", "[GCodeTemplateLint]")
{
    const std::string text = "G1 X10.5 Y-2 F{layer_z} ; move [layer_z]\nPRINT_START EXTRUDER=200\n{if true}M117{endif}";
    const Result r = analyze(text, test_known());
    CHECK(r.issues.empty());
    CHECK(style_of(r, text, "G1") == Style::Command);
    CHECK(style_of(r, text, "X10.5") == Style::Param);
    CHECK(style_at(r, text.find("10.5")) == Style::Param);
    CHECK(style_of(r, text, "Y-2") == Style::Param);
    CHECK(style_of(r, text, "{layer_z}") == Style::Brace);
    CHECK(style_of(r, text, "layer_z}") == Style::Variable);
    CHECK(style_of(r, text, "; move") == Style::Comment);
    CHECK(style_of(r, text, "move") == Style::Comment);
    // A placeholder inside a comment is still a placeholder (the parser expands it there too).
    CHECK(style_of(r, text, "[layer_z]") == Style::Brace);
    CHECK(style_of(r, text, "layer_z]") == Style::Variable);
    CHECK(style_of(r, text, "PRINT_START") == Style::Command);
    CHECK(style_of(r, text, "EXTRUDER") == Style::Param);
    CHECK(style_of(r, text, "200") == Style::Number);
    CHECK(style_of(r, text, "if") == Style::Keyword);
    CHECK(style_of(r, text, "true") == Style::Keyword);
    CHECK(style_of(r, text, "endif") == Style::Keyword);

    // Spans are sorted and do not overlap.
    for (size_t k = 1; k < r.spans.size(); ++k)
        CHECK(r.spans[k - 1].start + r.spans[k - 1].length <= r.spans[k].start);
}

TEST_CASE("G-code template lint: strings, regexes and numbers inside expressions", "[GCodeTemplateLint]")
{
    const std::string text = "{if filament_type[0] =~ /PLA.*/ and nozzle_diameter[0] / 2 >= 0.2}M117 \"x\"{endif}";
    const Result r = analyze(text, test_known());
    CHECK(r.issues.empty());
    CHECK(style_of(r, text, "/PLA") == Style::String);
    CHECK(style_of(r, text, "/ 2") == Style::Operator);
    CHECK(style_of(r, text, "0.2") == Style::Number);
    CHECK(style_of(r, text, "and") == Style::Keyword);
    // Words inside a string or a regex are not placeholder names.
    const Result r2 = analyze("{if filament_type[0] == \"not_a_key\" or filament_type[0] =~ /.*ALSO_NOT.*/}{endif}", test_known());
    CHECK(r2.issues.empty());
}

TEST_CASE("G-code template lint: unknown placeholders are warnings", "[GCodeTemplateLint]")
{
    const std::string text = "M104 S{temprature}\nM140 S[bed_temp]\nM117 [temperature_1] [temperature_x]\n";
    const Result r = analyze(text, test_known());
    REQUIRE(r.issues.size() == 3);
    CHECK(r.errors() == 0);
    CHECK(r.warnings() == 3);
    CHECK(r.issues[0].code == Code::UnknownPlaceholder);
    CHECK(r.issues[0].name == "temprature");
    CHECK(r.issues[0].line == 1);
    CHECK(r.issues[0].start == text.find("temprature"));
    CHECK(r.issues[0].length == std::string("temprature").size());
    CHECK(r.issues[1].name == "bed_temp");
    CHECK(r.issues[1].line == 2);
    // [temperature_1] is a legacy vector index and fine; [temperature_x] is not.
    CHECK(r.issues[2].name == "temperature_x");
    CHECK(r.issues[2].line == 3);

    // `local` / `global` declare names for the whole template.
    CHECK(analyze("{my_var}{local my_var = 1}", test_known()).issues.empty());
    // A null checker skips the name check.
    CHECK(analyze("{anything_goes} [whatever]", KnownFn()).issues.empty());
}

TEST_CASE("G-code template lint: braces and brackets", "[GCodeTemplateLint]")
{
    SECTION("unclosed brace") {
        const Result r = analyze("G1 Z{layer_z\nG1 X0\n", test_known());
        CHECK(codes(r) == std::vector<Code>{Code::UnclosedBrace});
        CHECK(r.issues[0].start == 4);
        CHECK(r.issues[0].severity == Severity::Error);
    }
    SECTION("unclosed brace before the next expression") {
        const Result r = analyze("{layer_z {layer_num}", test_known());
        CHECK(codes(r) == std::vector<Code>{Code::UnclosedBrace});
        CHECK(r.issues[0].start == 0);
    }
    SECTION("stray closing brace") {
        const Result r = analyze("G1 X0}\n", test_known());
        CHECK(codes(r) == std::vector<Code>{Code::StrayClosingBrace});
        CHECK(r.issues[0].start == 5);
    }
    SECTION("malformed legacy placeholder") {
        const Result r = analyze("M117 [1]\nM117 [layer_z\n", test_known());
        CHECK(codes(r) == std::vector<Code>{Code::MalformedLegacy, Code::MalformedLegacy});
        CHECK(r.issues[1].line == 2);
    }
    SECTION("legacy index form") {
        CHECK(analyze("M104 S[temperature[current_extruder]]", test_known()).issues.empty());
        const Result r = analyze("M104 S[temperature[extruder_idx]]", test_known());
        CHECK(codes(r) == std::vector<Code>{Code::UnknownPlaceholder});
        CHECK(r.issues[0].name == "extruder_idx");
    }
    SECTION("unbalanced parentheses inside an expression") {
        CHECK(codes(analyze("{max(layer_z, 1}", test_known())) == std::vector<Code>{Code::UnbalancedBracket});
        CHECK(codes(analyze("{layer_z)}", test_known())) == std::vector<Code>{Code::UnbalancedBracket});
        CHECK(codes(analyze("{temperature[0)]}", test_known())) == std::vector<Code>{Code::UnbalancedBracket});
    }
    SECTION("unterminated string") {
        const Result r = analyze("{if filament_type[0] == \"PLA}\nG1\n", test_known());
        const std::vector<Code> c = codes(r);
        CHECK(std::find(c.begin(), c.end(), Code::UnclosedString) != c.end());
        CHECK(r.errors() >= 1);
    }
}

TEST_CASE("G-code template lint: if / else / endif balance", "[GCodeTemplateLint]")
{
    SECTION("missing endif") {
        const Result r = analyze("G28\n{if layer_num == 0}\nG1 Z1\n", test_known());
        CHECK(codes(r) == std::vector<Code>{Code::IfWithoutEndif});
        CHECK(r.issues[0].line == 2);
        CHECK(r.issues[0].start == std::string("G28\n{").size());
    }
    SECTION("endif without if") {
        const Result r = analyze("G1\n{endif}\n", test_known());
        CHECK(codes(r) == std::vector<Code>{Code::EndifWithoutIf});
        CHECK(r.issues[0].line == 2);
    }
    SECTION("else without if") {
        CHECK(codes(analyze("{else}{elsif true}", test_known())) == std::vector<Code>{Code::ElseWithoutIf, Code::ElseWithoutIf});
    }
    SECTION("nested ifs") {
        CHECK(analyze("{if true}{if false}a{else}b{endif}{endif}", test_known()).issues.empty());
        CHECK(codes(analyze("{if true}{if false}a{endif}", test_known())) == std::vector<Code>{Code::IfWithoutEndif});
    }
    SECTION("inline if must end in the same expression") {
        CHECK(analyze("{if true then 1 else 2 endif}", test_known()).issues.empty());
        const Result r = analyze("{if true then 1 else 2}", test_known());
        CHECK(codes(r) == std::vector<Code>{Code::IfWithoutEndif});
    }
}

TEST_CASE("G-code template lint: known placeholder names", "[GCodeTemplateLint]")
{
    const auto &names = known_placeholder_names();
    // config keys, editor dialog groups, G-code specific placeholders and runtime values
    for (const char *name : {"nozzle_temperature", "bed_temperature_initial_layer_single", "layer_z", "layer_num",
                             "current_extruder", "next_extruder", "timestamp", "print_bed_max", "total_layer_count",
                             "first_layer_print_min", "outer_wall_volumetric_speed", "initial_no_support_extruder",
                             "machine_max_speed_z", "filament_type", "is_extruder_used"})
        CHECK(names.count(name) == 1);
    CHECK(names.count("temprature") == 0);
    CHECK(is_keyword("endif"));
    CHECK(is_keyword("one_of"));
    CHECK(!is_keyword("layer_z"));
}

// Every G-code template shipped in resources/profiles/*/machine parses without a structural error:
// the editor must never paint a shipped profile red.
TEST_CASE("G-code template lint: shipped machine G-code has no errors", "[GCodeTemplateLint]")
{
    namespace fs = boost::filesystem;
    const fs::path profiles = fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles";
    REQUIRE(fs::is_directory(profiles));
    const auto &known = known_placeholder_names();
    const KnownFn is_known = [&known](const std::string &n) { return known.count(n) > 0; };

    size_t templates = 0;
    std::set<std::string> unknown_names;
    for (fs::directory_iterator vit(profiles); vit != fs::directory_iterator(); ++vit) {
        const fs::path machine_dir = vit->path() / "machine";
        if (!fs::is_directory(machine_dir))
            continue;
        for (fs::recursive_directory_iterator it(machine_dir); it != fs::recursive_directory_iterator(); ++it) {
            if (!fs::is_regular_file(it->path()) || it->path().extension() != ".json")
                continue;
            boost::nowide::ifstream ifs(it->path().string());
            const nlohmann::json j = nlohmann::json::parse(ifs, nullptr, false);
            if (!j.is_object())
                continue;
            for (auto kv = j.begin(); kv != j.end(); ++kv) {
                if (kv.key().find("gcode") == std::string::npos || !kv.value().is_string())
                    continue;
                ++templates;
                // A string option is C-unescaped when a profile is loaded (ConfigOptionString::deserialize):
                // some profiles store \" and \n escapes inside the JSON string. Check what the editor shows.
                std::string gcode;
                if (!unescape_string_cstyle(kv.value().get<std::string>(), gcode))
                    gcode = kv.value().get<std::string>();
                const Result r = analyze(gcode, is_known);
                for (const Issue &is : r.issues) {
                    if (is.severity == Severity::Warning) {
                        unknown_names.insert(is.name);
                        continue;
                    }
                    INFO(it->path().filename().string() << " " << kv.key() << " line " << is.line << " code " << int(is.code));
                    CHECK(is.severity == Severity::Warning);
                }
            }
        }
    }
    CHECK(templates > 1000);
    // Shipped profiles do carry a few genuinely unknown names (typos, other slicers' keys); list them
    // in the test log rather than failing on them.
    std::string listed;
    for (const std::string &n : unknown_names)
        listed += n + " ";
    WARN("unknown placeholder names in shipped machine G-code (" << unknown_names.size() << "): " << listed);
    CHECK(unknown_names.size() < 40);
}
