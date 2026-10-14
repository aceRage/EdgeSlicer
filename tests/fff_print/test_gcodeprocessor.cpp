#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "test_data.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

// Bambu firmware uses the " FEATURE: " style reserved tags, everything else the Slic3r-compatible
// "TYPE:" style, so which list applies depends on the printer kind passed in.
TEST_CASE("Reserved keyword detection follows the printer kind it is given", "[GCodeProcessor]")
{
    struct Case
    {
        const char *name;
        std::string gcode;
        bool        reserved_on_bbl;
        bool        reserved_on_non_bbl;
    };

    // Loop + DYNAMIC_SECTION rather than GENERATE(values<Case>): Catch2 v2 has no
    // StringMaker for the aggregate, and the project's test guide forbids reused SECTION names.
    const Case cases[] = {
        {"compatible feature tag", ";TYPE:Prime tower", false, true},
        {"compatible layer tag", ";LAYER_CHANGE", false, true},
        {"bbl feature tag", "; FEATURE: Outer wall", true, false},
        {"tag shared by both lists", ";_GP_FIRST_LINE_M73_PLACEHOLDER", true, true},
        {"bbl spells this one with a leading space", ";COLOR_CHANGE", false, true},
        {"ordinary comment", "; heat the bed", false, false},
        {"not a comment at all", "G1 X10 Y10 F3000", false, false},
        // A tag counts only as the whole comment's prefix, so neither a tag mentioned mid-comment
        // nor one trailing a real command is a reserved use.
        {"tag text later in the comment", "; the TYPE:Prime tower marker", false, false},
        {"tag trailing a command", "G1 X10 ;TYPE:Prime tower", false, false},
    };

    for (const auto &test_case : cases) {
        DYNAMIC_SECTION(test_case.name)
        {
            std::vector<std::string> tags;
            REQUIRE(GCodeProcessor::contains_reserved_tags(test_case.gcode, 5, tags, true) == test_case.reserved_on_bbl);

            tags.clear();
            REQUIRE(GCodeProcessor::contains_reserved_tags(test_case.gcode, 5, tags, false) == test_case.reserved_on_non_bbl);
        }
    }
}

TEST_CASE("Reserved keyword detection reports every offending line", "[GCodeProcessor]")
{
    const std::string gcode = ";TYPE:Prime tower\nG1 X10\n;LAYER_CHANGE\n";

    std::vector<std::string> tags;
    REQUIRE(GCodeProcessor::contains_reserved_tags(gcode, 5, tags, false));
    REQUIRE(tags.size() == 2);
    // Reported in the order they appear, which is what makes the max_count cut-off meaningful.
    CHECK(tags[0] == "TYPE:Prime tower");
    CHECK(tags[1] == "LAYER_CHANGE");

    SECTION("the reported count is capped at max_count")
    {
        tags.clear();
        REQUIRE(GCodeProcessor::contains_reserved_tags(gcode, 1, tags, false));
        CHECK(tags.size() == 1);
        CHECK(tags[0] == "TYPE:Prime tower");
    }

    SECTION("a max_count of zero still reports the first tag")
    {
        tags.clear();
        REQUIRE(GCodeProcessor::contains_reserved_tags(gcode, 0, tags, false));
        CHECK(tags.size() == 1);
    }

    SECTION("g-code with nothing reserved in it reports nothing")
    {
        tags.clear();
        CHECK_FALSE(GCodeProcessor::contains_reserved_tags("G28\n; home all axes\n", 5, tags, false));
        CHECK(tags.empty());
    }
}

TEST_CASE("GCodeProcessorResult move assignment transfers members the copy assignment used to miss", "[GCodeProcessor]")
{
    GCodeProcessorResult src;
    src.filename = "export.gcode";
    src.backtrace_enabled = true;
    src.support_traditional_timelapse = false;
    src.moves.emplace_back();
    GCodeProcessorResult dst;
    dst = std::move(src);
    REQUIRE(dst.filename == "export.gcode");
    REQUIRE(dst.backtrace_enabled);
    REQUIRE_FALSE(dst.support_traditional_timelapse);
    REQUIRE(dst.moves.size() == 1);
}

TEST_CASE("GCodeProcessorResult copy assignment keeps the source and carries the full member set", "[GCodeProcessor]")
{
    GCodeProcessorResult src;
    src.filename = "export.gcode";
    src.backtrace_enabled = true;
    src.nozzle_hrc = 55;
    src.required_nozzle_HRC = {0, 3};
    src.filament_vitrification_temperature = {45, 60};
    src.support_traditional_timelapse = false;
    src.moves.emplace_back();
    GCodeProcessorResult dst;
    dst = src;
    REQUIRE(dst.filename == "export.gcode");
    REQUIRE(dst.backtrace_enabled);
    REQUIRE(dst.nozzle_hrc == 55);
    REQUIRE(dst.required_nozzle_HRC == std::vector<int>({0, 3}));
    REQUIRE(dst.filament_vitrification_temperature == std::vector<int>({45, 60}));
    REQUIRE_FALSE(dst.support_traditional_timelapse);
    REQUIRE(dst.moves.size() == 1);
    // The copy leaves the source intact.
    REQUIRE(src.moves.size() == 1);
    REQUIRE(src.filename == "export.gcode");
}

TEST_CASE("post-process lines_ends match newline offsets", "[GCodeProcessor]")
{
    auto make_config = [](bool by_time) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_num_extruders(2);
        config.set_num_filaments(2);
        config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
        config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
        config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
        config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
        config.option<ConfigOptionFloat>("preheat_time")->value = by_time ? 30. : 0.;
        // Orca #11791: the processor's backtracked preheat also needs ooze prevention.
        config.option<ConfigOptionBool>("ooze_prevention")->value = true;
        config.option<ConfigOptionBool>("gcode_comments")->value = true;
        config.set_deserialize_strict({{"brim_type", "no_brim"},
                                       {"skirt_loops", "0"},
                                       {"layer_height", "0.2"},
                                       {"initial_layer_print_height", "0.2"},
                                       {"gcode_flavor", "marlin"}});
        return config;
    };

    auto run_case = [](DynamicPrintConfig config, bool by_time) {
        Print print;
        Model model;
        init_print({make_cube(40., 40., 20.), make_cube(40., 40., 20.)}, print, model, config, false);
        REQUIRE(model.objects.size() == 2);
        model.objects[0]->config.set("extruder", 1);
        model.objects[1]->config.set("extruder", 2);
        print.apply(model, config);
        print.is_BBL_printer() = false;

        GCodeProcessorResult result;
        const std::string    exported = Test::gcode(print, result);
        REQUIRE((exported.find("preheat T") != std::string::npos) == by_time);
        REQUIRE(exported.size() > GCodeProcessor::Output_Block_Size);

        std::vector<size_t> newline_ends;
        for (size_t i = exported.find('\n'); i != std::string::npos; i = exported.find('\n', i + 1))
            newline_ends.push_back(i + 1);
        REQUIRE(result.lines_ends.size() == newline_ends.size());
        const auto difference = std::mismatch(result.lines_ends.begin(), result.lines_ends.end(), newline_ends.begin());
        INFO("first difference at line " << (difference.first - result.lines_ends.begin() + 1));
        CHECK(difference.first == result.lines_ends.end());
    };

    SECTION("BySize") { run_case(make_config(false), false); }
    SECTION("ByTime") { run_case(make_config(true), true); }
}

TEST_CASE("Reloaded moves name their lines in G-code a script rewrote in place", "[GCodeProcessor]")
{
    Print print;
    Model model;
    Test::init_print({TestMesh::cube_20x20x20}, print, model);
    GCodeProcessorResult result;
    const std::string    gcode          = Test::gcode(print, result);
    const auto           exported_moves = result.moves;
    const auto           exported_lines_ends = result.lines_ends;

    // A script that prepends one comment and, writing in text mode on Windows, turns every LF into CRLF.
    const std::string prepended = ";EDITED\r\n";
    std::string       edited    = prepended;
    for (const char c : gcode) {
        if (c == '\n')
            edited += '\r';
        edited += c;
    }
    const boost::filesystem::path temp = Test::scratch_path();
    save_string_file(temp, edited);
    result.filename = temp.string();
    print.reload_gcode_moves(&result);

    std::vector<size_t> newline_ends;
    for (size_t i = edited.find('\n'); i != std::string::npos; i = edited.find('\n', i + 1))
        newline_ends.push_back(i + 1);
    CHECK(result.lines_ends == newline_ends);

    // A fresh re-parse of the file is not move-for-move identical to the export: it starts with its own
    // initial Noop move (gcode_id 0), and it splits some start-up moves such as the start G-code's
    // "G1 Z5 F5000 ; lift nozzle" into several travel segments. So the contract checked here is what the
    // Preview relies on: every exported move is still present, at the same position, and the line it names
    // in the rewritten file (one further down than before) holds the same G-code as the line it named at export.
    REQUIRE(result.moves.size() >= exported_moves.size());

    const auto line_text = [](const std::string &text, const std::vector<size_t> &ends, unsigned int id) {
        REQUIRE(id >= 1);
        REQUIRE(id <= ends.size());
        const size_t begin = id == 1 ? 0 : ends[id - 2];
        std::string  line  = text.substr(begin, ends[id - 1] - begin);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        return line;
    };

    std::unordered_multimap<unsigned int, const GCodeProcessorResult::MoveVertex *> reloaded_by_line;
    for (const auto &reloaded : result.moves)
        reloaded_by_line.emplace(reloaded.gcode_id, &reloaded);

    size_t                                      checked = 0;
    std::vector<std::pair<size_t, std::string>> failures;
    for (size_t i = 0; i < exported_moves.size(); ++i) {
        const auto &exported = exported_moves[i];
        if (exported.gcode_id == 0) // Moves not tied to a line (the initial Noop).
            continue;
        ++checked;
        const std::string wanted = line_text(gcode, exported_lines_ends, exported.gcode_id);
        bool              found  = false;
        const auto candidates = reloaded_by_line.equal_range(exported.gcode_id + 1);
        for (auto it = candidates.first; it != candidates.second && !found; ++it)
            found = (it->second->position - exported.position).norm() <= 1e-3f &&
                    line_text(edited, result.lines_ends, it->second->gcode_id) == wanted;
        if (!found && failures.size() < 5)
            failures.emplace_back(i, wanted);
    }
    for (const auto &failure : failures)
        INFO("exported move " << failure.first << " (" << failure.second << ") has no matching reloaded move");
    CHECK(checked > 100);
    CHECK(failures.empty());
    // Only the re-parse's extra start-up segments may be added, not a second copy of the print.
    CHECK(result.moves.size() < exported_moves.size() + 20);
}

TEST_CASE("Rewritten G-code that cannot be re-read keeps the moves and hides the G-code window", "[GCodeProcessor]")
{
    Print print;
    Model model;
    Test::init_print({TestMesh::cube_20x20x20}, print, model);
    GCodeProcessorResult result;
    const std::string    gcode          = Test::gcode(print, result);
    const auto           exported_moves = result.moves;

    // A script that strips the trailing config block, which the G-code reader needs.
    const size_t config_block = gcode.find("; CONFIG_BLOCK_START");
    REQUIRE(config_block != std::string::npos);
    const boost::filesystem::path temp = Test::scratch_path();
    save_string_file(temp, gcode.substr(0, config_block));
    result.filename = temp.string();
    print.reload_gcode_moves(&result);

    CHECK(result.lines_ends.empty());
    REQUIRE(result.moves.size() == exported_moves.size());
    CHECK(result.moves.back().gcode_id == exported_moves.back().gcode_id);
}

namespace {
// The trailing config block of an exported G-code, from a full config with two filaments plus the
// given values. ConfigBase::load_from_gcode_file() refuses a block of fewer than 80 values.
std::string two_filament_config_block(std::initializer_list<ConfigBase::SetDeserializeItem> items)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00"};
    config.set_deserialize_strict(items);
    std::string block = "; CONFIG_BLOCK_START\n";
    for (const std::string &key : config.keys())
        block += "; " + key + " = " + config.opt_serialize(key) + "\n";
    block += "; CONFIG_BLOCK_END\n";
    return block;
}
} // namespace

// Orca #15379: a G-code opened in the viewer gets its config from the file (the DynamicPrintConfig
// overload of apply_config), which never read manual_filament_change, so the MANUAL_TOOL_CHANGE
// comments of a manual-filament-change print did not switch the preview colour.
TEST_CASE("Loaded manual tool change comments switch subsequent moves when enabled", "[GCodeProcessor][Regression]")
{
    for (const bool manual_filament_change : { false, true }) {
        DYNAMIC_SECTION("manual_filament_change = " << manual_filament_change)
        {
            const bool was_bbl_printer = GCodeProcessor::s_IsBBLPrinter;
            const ScopeGuard restore_bbl_printer([was_bbl_printer] { GCodeProcessor::s_IsBBLPrinter = was_bbl_printer; });
            GCodeProcessor::s_IsBBLPrinter = false;

            const boost::filesystem::path temp = Test::scratch_path();
            const ScopeGuard remove_temp([&temp] { boost::filesystem::remove(temp); });
            std::string gcode = "; generated by OrcaSlicer\n"
                                "G90\nM83\nT0\n"
                                "G1 X10 Y10 Z0.2 F3000\n"
                                "G1 X20 E1\n"
                                "; MANUAL_TOOL_CHANGE T1\n"
                                "G1 X30\nG1 X40 E1\nG1 X50\nG1 X60 E1\n";
            // The block process_file() loads, as the standalone viewer does.
            gcode += two_filament_config_block({{"gcode_flavor", "marlin"},
                                                {"filament_type", "PLA;PETG"},
                                                {"manual_filament_change", manual_filament_change}});
            save_string_file(temp, gcode);

            GCodeProcessor processor;
            processor.process_file(temp.string());
            const GCodeProcessorResult &result = processor.get_result();
            REQUIRE(result.extruders_count == 2);

            constexpr unsigned int tool_change_line = 7;
            size_t before = 0;
            size_t after  = 0;
            for (const GCodeProcessorResult::MoveVertex &move : result.moves) {
                if (move.type != EMoveType::Extrude && move.type != EMoveType::Travel)
                    continue;
                CAPTURE(move.gcode_id);
                if (move.gcode_id < tool_change_line) {
                    ++before;
                    CHECK(move.extruder_id == 0);
                } else if (move.gcode_id > tool_change_line) {
                    ++after;
                    CHECK(move.extruder_id == (manual_filament_change ? 1 : 0));
                }
            }
            REQUIRE(before > 0);
            REQUIRE(after > 0);
        }
    }
}

// Orca #13741: the export pass picked the preheat temperature from m_layer_id, which by then holds
// the analysis pass's total layer count, so a tool change on the first layer was preheated to the
// other-layers temperature. It now follows the layer the export pass is in.
TEST_CASE("Preheat before a first-layer tool change uses the first-layer temperature", "[GCodeProcessor][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionInts>("nozzle_temperature")->values               = {200, 200};
    config.option<ConfigOptionInts>("nozzle_temperature_initial_layer")->values = {230, 230};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionFloat>("preheat_time")->value                  = 30.;
    config.option<ConfigOptionBool>("ooze_prevention")->value                = true;
    config.set_deserialize_strict({{"brim_type", "no_brim"},
                                   {"skirt_loops", "0"},
                                   {"layer_height", "0.2"},
                                   {"initial_layer_print_height", "0.2"},
                                   {"gcode_flavor", "marlin"}});

    Print print;
    Model model;
    init_print({make_cube(20., 20., 2.), make_cube(20., 20., 2.)}, print, model, config, false);
    REQUIRE(model.objects.size() == 2);
    model.objects[0]->config.set("extruder", 1);
    model.objects[1]->config.set("extruder", 2);
    print.apply(model, config);
    print.is_BBL_printer() = false;
    const std::string gcode = Test::gcode(print);

    // "M104 S<temp> T1 ; preheat T1 time: ...", in G-code order.
    std::vector<std::string> preheats;
    for (size_t pos = gcode.find("; preheat T1"); pos != std::string::npos; pos = gcode.find("; preheat T1", pos + 1)) {
        const size_t line_start = gcode.rfind('\n', pos) + 1;
        preheats.emplace_back(gcode.substr(line_start, pos - line_start));
    }
    REQUIRE(preheats.size() >= 2);
    CAPTURE(preheats);
    // The first change to T1 is on the first layer; the last one is on a later layer.
    CHECK(preheats.front().find("M104 S230 T1") == 0);
    CHECK(preheats.back().find("M104 S200 T1") == 0);
}

// Orca #14663: the Elegoo Centauri Carbon 2 changes filament with M6211 T<n> L<purge length> ...;
// the time estimate now books the firmware's own change-and-purge time on it, and the purged
// filament shows in the flush statistics. Other printers ignore the command.
TEST_CASE("Elegoo M6211 filament changes are timed and their purge is counted", "[GCodeProcessor][Elegoo]")
{
    const bool was_bbl_printer = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore_bbl_printer([was_bbl_printer] { GCodeProcessor::s_IsBBLPrinter = was_bbl_printer; });
    GCodeProcessor::s_IsBBLPrinter = false;

    // GCodeProcessorResult has a move assignment but no copy or move constructor.
    auto run = [](const std::string &printer_model, GCodeProcessorResult &out) {
        const boost::filesystem::path temp = Test::scratch_path();
        const ScopeGuard remove_temp([&temp] { boost::filesystem::remove(temp); });
        std::string gcode = "; generated by OrcaSlicer\n"
                            "G90\nM83\nM204 S1500\n"
                            // Start G-code: load the first filament (Centauri Carbon 2 profile form).
                            "M6211 A1 L200 T0 Q220 R220 S220\nT0\n"
                            "G1 X10 Y10 Z0.2 F3000\nG1 X60 E2 F1800\nG1 X60 Y60 E2\n"
                            // change_filament_gcode: swap to filament 2 and purge 100 mm of it.
                            "M6211 T1 L100 M300 N300 Q220 R240 S220\nT1\n"
                            "G1 X10 Y60 E2 F1800\nG1 X10 Y10 E2\n";
        gcode += two_filament_config_block({{"gcode_flavor", "klipper"},
                                            {"printer_model", printer_model},
                                            {"nozzle_volume", 10},
                                            {"machine_load_filament_time", 0},
                                            {"machine_unload_filament_time", 0},
                                            {"machine_tool_change_time", 0}});
        save_string_file(temp, gcode);
        GCodeProcessor processor;
        processor.process_file(temp.string());
        out = processor.extract_result();
    };

    GCodeProcessorResult elegoo;
    GCodeProcessorResult other;
    run("Elegoo Centauri Carbon 2", elegoo);
    run("Generic Klipper Printer", other);
    constexpr size_t           normal = static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal);

    // One change from filament 1 to 2 either way; the start G-code's load is not a change.
    CHECK(elegoo.print_statistics.total_filamentchanges == 1);
    CHECK(other.print_statistics.total_filamentchanges == 1);

    // Centauri Carbon 2 model: L200 at the start = 31 + 15 mm at F300 (3 s) + 2 intermediate purges
    // (22.7 s each) + the final one (25.883 s) = 105.283 s. L100 with R240 > S220 = 31 + 3 + 22.7 +
    // 25.883 + 20 / 1.36 (14.706 s) = 97.289 s.
    const float added = elegoo.print_statistics.modes[normal].time - other.print_statistics.modes[normal].time;
    CHECK_THAT(added, Catch::Matchers::WithinAbs(105.283 + 97.289, 0.5));

    // Purge: L200 of filament 1 at the start (the nozzle was empty), then L100 of filament 2, whose
    // first 10 mm^3 push out what was left of filament 1.
    const double area = PI * 0.875 * 0.875;
    REQUIRE(elegoo.print_statistics.flush_per_filament.count(0) == 1);
    REQUIRE(elegoo.print_statistics.flush_per_filament.count(1) == 1);
    CHECK_THAT(elegoo.print_statistics.flush_per_filament.at(0), Catch::Matchers::WithinAbs(200. * area + 10., 0.01));
    CHECK_THAT(elegoo.print_statistics.flush_per_filament.at(1), Catch::Matchers::WithinAbs(100. * area - 10., 0.01));
    CHECK(other.print_statistics.flush_per_filament.empty());
}
