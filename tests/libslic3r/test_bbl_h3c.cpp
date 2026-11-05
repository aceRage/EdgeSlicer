#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/NozzleStripModel.hpp"
#include "libslic3r/ToolSlots.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>

#include <string>

using namespace Slic3r;

// Bambu Lab H3C (N10, one extruder with a nine-nozzle rack), imported from Bambu Studio 2.9.1.
// Its start / filament-change templates read placeholders Bambu Studio 2.9.1 added:
// initial_no_support_nozzle_id, fins_extrude_safe_temp, initial_layer_travel_acceleration; and
// its 0.4 machine declares a TPU285 rack slot. These cases slice a cube on this tree's own BBL
// system profiles with the FULL start G-code, so any unresolved placeholder fails the slice.

namespace {

PresetBundle &h3c_bundle()
{
    static PresetBundle *bundle = [] {
        const std::string saved_data_dir = data_dir();
        const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("bbl_h3c_%%%%-%%%%");
        boost::filesystem::create_directories(scratch);
        set_data_dir(scratch.string());
        const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
        static PresetBundle library;
        library.load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
        auto *b = new PresetBundle();
        b->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, &library);
        set_data_dir(saved_data_dir);
        return b;
    }();
    return *bundle;
}

DynamicPrintConfig h3c_config(const char *printer, const char *process, const char *filament)
{
    PresetBundle &b = h3c_bundle();
    REQUIRE(b.printers.select_preset_by_name(printer, true));
    REQUIRE(b.prints.select_preset_by_name(process, true));
    REQUIRE(b.filaments.select_preset_by_name(filament, true));
    b.filament_presets = { filament };
    b.set_num_filaments(1, std::vector<std::string>{ "#E01919" });
    b.filament_presets = { filament };
    return b.full_config_secure();
}

std::string slice_cube(const DynamicPrintConfig &cfg, bool bbl = true)
{
    Print print;
    Model model;
    ModelObject *object = model.add_object();
    object->name        = "cube";
    object->add_volume(Test::mesh(Test::TestMesh::cube_20x20x20));
    object->add_instance();
    object->center_around_origin();
    object->instances.front()->set_offset(Vec3d(165., 160., 0.));
    object->ensure_on_bed();
    print.is_BBL_printer() = bbl;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

} // namespace

TEST_CASE("NozzleVolumeType TPU285 keeps Bambu's value and round-trips", "[BblH3C][NozzleVolumeType]")
{
    // Values saved in 3MFs, presets and slice_info: nothing existing may shift.
    STATIC_REQUIRE(int(nvtStandard) == 0);
    STATIC_REQUIRE(int(nvtHighFlow) == 1);
    STATIC_REQUIRE(int(nvtHybrid) == 2);
    STATIC_REQUIRE(int(nvtTPUHighFlow) == 3);
    STATIC_REQUIRE(int(nvtTPU285) == 4);
    STATIC_REQUIRE(int(nvtE3DHighFlow) == 5);

    CHECK(get_nozzle_volume_type_string(nvtTPU285) == "TPU285");
    CHECK(get_nozzle_volume_type_string(nvtE3DHighFlow) == "E3D High Flow");

    for (const char *key : { "nozzle_volume_type", "default_nozzle_volume_type", "extruder_nozzle_volume_type" }) {
        DYNAMIC_SECTION(key) {
            DynamicPrintConfig cfg;
            cfg.set_deserialize_strict({ { key, "Standard,TPU285,E3D High Flow,TPU High Flow" } });
            const auto *opt = cfg.option<ConfigOptionEnumsGeneric>(key);
            REQUIRE(opt != nullptr);
            CHECK(opt->values == std::vector<int>{ 0, 4, 5, 3 });
            CHECK(opt->serialize() == "Standard,TPU285,E3D High Flow,TPU High Flow");

            DynamicPrintConfig back;
            back.set_deserialize_strict({ { key, opt->serialize() } });
            CHECK(back.option<ConfigOptionEnumsGeneric>(key)->values == opt->values);
        }
    }
}

TEST_CASE("H3C machine preset declares the TPU285 rack slot", "[BblH3C]")
{
    PresetBundle &b = h3c_bundle();
    const Preset *printer = b.printers.find_preset("Bambu Lab H3C 0.4 nozzle", false);
    REQUIRE(printer != nullptr);
    const auto *dnvt = printer->config.option<ConfigOptionEnumsGeneric>("default_nozzle_volume_type");
    REQUIRE(dnvt != nullptr);
    REQUIRE(dnvt->values.size() == 9);
    for (size_t i = 0; i < dnvt->values.size(); ++i)
        CHECK(dnvt->values[i] == (i == 7 ? int(nvtTPU285) : int(nvtStandard)));
}

TEST_CASE("H3C cube slices with the full Bambu start G-code", "[BblH3C][PrintGCode]")
{
    struct Combo { const char *printer, *process, *filament; double nozzle; };
    const Combo combos[] = {
        { "Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Bambu PLA Basic @BBL H3C", 0.4 },
        { "Bambu Lab H3C 0.2 nozzle", "0.10mm Standard @BBL H3C 0.2 nozzle", "Bambu PLA Basic @BBL H3C 0.2 nozzle", 0.2 },
        { "Bambu Lab H3C 0.6 nozzle", "0.30mm Standard @BBL H3C 0.6 nozzle", "Bambu PLA Basic @BBL H3C 0.6 nozzle", 0.6 },
    };
    for (const Combo &c : combos) {
        DYNAMIC_SECTION(c.printer) {
            DynamicPrintConfig cfg = h3c_config(c.printer, c.process, c.filament);
            REQUIRE(cfg.option<ConfigOptionInts>("fins_extrude_safe_temp") != nullptr);
            CHECK(cfg.option<ConfigOptionInts>("fins_extrude_safe_temp")->get_at(0) == 57); // Bambu PLA Basic @base H3C

            std::string gcode;
            REQUIRE_NOTHROW(gcode = slice_cube(cfg));
            REQUIRE(gcode.find(";===== machine: H3C start") != std::string::npos);
            // The whole template ran (the M620.10 nozzle flush lines sit deep in it) and the cube printed.
            CHECK(gcode.find("\nM620.10 ") != std::string::npos);
            // ...and the end template ran after the layers. The config block (which quotes the template text)
            // sits in the header on BBL output, so look for the emitted line, not the quoted one.
            const size_t end_block = gcode.rfind("\n;===== H3C endgcode");
            REQUIRE(end_block != std::string::npos);
            CHECK(end_block > gcode.find("\nM620.10 "));
            // nozzle_diameter_at_nozzle_id[initial_no_support_nozzle_id] resolved to this machine's nozzle:
            // the M620.10 flush line carries it as H<diameter>.
            CHECK(gcode.find("Not a variable name") == std::string::npos);
            const size_t m620 = gcode.find("\nM620.10 A1 ");
            if (m620 != std::string::npos) {
                const std::string line = gcode.substr(m620 + 1, gcode.find('\n', m620 + 1) - m620 - 1);
                INFO(line);
                char buf[16];
                snprintf(buf, sizeof(buf), " H%g ", c.nozzle);
                CHECK(line.find(buf) != std::string::npos);
            }
        }
    }
}

// Owner hand test 2026-11-03: a new H3C project showed two rack nozzles ("0.4, 0.4"). With no rack text in the project
// or the printer preset, a rack machine has its full rack: the H3C nine slots (slot 8 TPU285 on the 0.4, as Bambu Studio
// 2.9.1 shows it), the H2C right extruder six. A saved project rack, a preset rack and Sync still win.
TEST_CASE("Rack machines default to their full rack", "[BblH3C][NozzleRackSlots]")
{
    PresetBundle &b = h3c_bundle();
    auto select = [&b](const char *printer) {
        REQUIRE(b.printers.select_preset_by_name(printer, true));
        b.update_compatible(PresetSelectCompatibleType::Never);
        b.set_rack_nozzle_override({});
        return b.printer_config_with_project_sizes();
    };

    SECTION("H3C 0.4: nine 0.4 slots, slot 8 TPU285")
    {
        const DynamicPrintConfig cfg = select("Bambu Lab H3C 0.4 nozzle");
        CHECK(default_rack_nozzle_text(cfg, 0) == "0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4:tpu285,0.4");
        const auto racks = extruder_rack_nozzles(cfg);
        REQUIRE(racks.size() == 1);
        REQUIRE(racks[0].size() == 9);
        for (size_t i = 0; i < 9; ++i) {
            INFO("slot " << i + 1);
            CHECK(racks[0][i].diameter == Approx(0.4));
            CHECK(racks[0][i].volume_type == (i == 7 ? nvtTPU285 : nvtStandard));
        }
        CHECK(rack_slot_count(cfg, 0) == 9);
        CHECK(rack_summary(racks[0]) == "9 x 0.4");
    }
    SECTION("H3C 0.2 / 0.6: nine slots of the machine's size; the 0.2 lists one Standard type for the extruder")
    {
        const DynamicPrintConfig c02 = select("Bambu Lab H3C 0.2 nozzle");
        CHECK(default_rack_nozzle_text(c02, 0) == "0.2,0.2,0.2,0.2,0.2,0.2,0.2,0.2,0.2");
        const DynamicPrintConfig c06 = select("Bambu Lab H3C 0.6 nozzle");
        CHECK(default_rack_nozzle_text(c06, 0) == "0.6,0.6,0.6,0.6,0.6,0.6,0.6,0.6:tpu285,0.6");
        CHECK(rack_slot_count(c06, 0) == 9);
    }
    SECTION("H2C: no rack on the left, six of its size on the right")
    {
        const DynamicPrintConfig cfg = select("Bambu Lab H2C 0.4 nozzle");
        CHECK(default_rack_nozzle_text(cfg, 0).empty());
        CHECK(default_rack_nozzle_text(cfg, 1) == "0.4,0.4,0.4,0.4,0.4,0.4");
        const auto racks = extruder_rack_nozzles(cfg);
        REQUIRE(racks.size() == 2);
        CHECK(racks[0].empty());
        CHECK(racks[1].size() == 6);
        CHECK(rack_slot_count(cfg, 1) == 6);
    }
    SECTION("X1C / H2D: no rack")
    {
        for (const char *printer : { "Bambu Lab X1 Carbon 0.4 nozzle", "Bambu Lab H2D 0.4 nozzle" }) {
            INFO(printer);
            const DynamicPrintConfig cfg = select(printer);
            for (const auto &rack : extruder_rack_nozzles(cfg))
                CHECK(rack.empty());
        }
    }
    SECTION("a saved project rack keeps its value")
    {
        select("Bambu Lab H3C 0.4 nozzle");
        REQUIRE(b.set_rack_nozzle_override(0, "0.4, 0.6, 0.2"));
        const auto racks = extruder_rack_nozzles(b.printer_config_with_project_sizes());
        REQUIRE(racks[0].size() == 3);
        CHECK(racks[0][1].diameter == Approx(0.6));
        CHECK(b.rack_nozzle_override(0) == "0.4, 0.6, 0.2");
        // The H2C keeps a saved right rack too.
        select("Bambu Lab H2C 0.4 nozzle");
        REQUIRE(b.set_rack_nozzle_override(1, "0.4,0.8"));
        CHECK(extruder_rack_nozzles(b.printer_config_with_project_sizes())[1].size() == 2);
        b.set_rack_nozzle_override({});
    }
    SECTION("a printer preset's rack text wins over the default")
    {
        DynamicPrintConfig cfg = select("Bambu Lab H3C 0.4 nozzle");
        cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_diameter", true)->values = { "0.4,0.2" };
        CHECK(extruder_rack_nozzles(cfg)[0].size() == 2);
    }
    SECTION("Sync replaces the default with the reported nozzles")
    {
        const std::vector<std::string> synced = rack_override_from_reported({ { 0.4, 0.4, 0.6, 0.2 } }, {});
        REQUIRE(synced.size() == 1);
        DynamicPrintConfig cfg = select("Bambu Lab H3C 0.4 nozzle");
        cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_override", true)->values = synced;
        const auto racks = extruder_rack_nozzles(cfg);
        REQUIRE(racks[0].size() == 4);
        CHECK(racks[0][2].diameter == Approx(0.6));
    }
}

// Owner hand test 2026-11-04: the strip and the Extruder Settings dialog showed slot 8 as "0.4 Std" / Standard, with no
// TPU285 in the type list. The default rack carries the type; the strip names it, offers it, and an edit keeps it.
TEST_CASE("The H3C's TPU285 slot reaches the nozzle strip and survives an edit", "[BblH3C][NozzleStrip]")
{
    PresetBundle &b = h3c_bundle();
    REQUIRE(b.printers.select_preset_by_name("Bambu Lab H3C 0.4 nozzle", true));
    b.update_compatible(PresetSelectCompatibleType::Never);
    b.set_rack_nozzle_override({});
    const DynamicPrintConfig cfg = b.printer_config_with_project_sizes();

    NozzleStripInput in = nozzle_strip_input(cfg, false);
    // As the sidebar does: the project's flow type (Standard) for the slots that do not name their own.
    nozzle_strip_set_volume_types(in, { nvtStandard });
    const auto tiles = nozzle_strip_tiles(in);
    REQUIRE(tiles.size() == 9);
    for (size_t i = 0; i < 9; ++i) {
        INFO("slot " << i + 1);
        CHECK(tiles[i].volume_type == (i == 7 ? nvtTPU285 : nvtStandard));
        CHECK(nozzle_flow_short(tiles[i].volume_type) == (i == 7 ? "TPU285" : "Std"));
    }
    CHECK(nozzle_chip_for(tiles, 0, 0.4).flow == "Std/TPU285");

    CHECK(nozzle_strip_printer_has_type(cfg, nvtTPU285));
    for (const char *printer : { "Bambu Lab X1 Carbon 0.4 nozzle", "Bambu Lab H2C 0.4 nozzle" }) {
        INFO(printer);
        REQUIRE(b.printers.select_preset_by_name(printer, true));
        CHECK_FALSE(nozzle_strip_printer_has_type(b.printer_config_with_project_sizes(), nvtTPU285));
    }

    // Editing slot 3 to 0.6 writes the rack with slot 8 still TPU285.
    NozzleStripEdit edit = nozzle_strip_edit_start(in);
    REQUIRE(nozzle_strip_edit_tile(edit, in, tiles[2], 0.6, nvtStandard).empty());
    const std::vector<RackNozzle> rack = parse_rack_nozzles(edit.rack_texts[0]);
    REQUIRE(rack.size() == 9);
    CHECK(rack[2].diameter == Approx(0.6));
    CHECK(rack[7].volume_type == nvtTPU285);
    CHECK(rack[7].volume_given);
    CHECK(validate_rack_nozzle_text(edit.rack_texts[0], 0.4).empty());
}

// Owner request 2026-11-04 (Bambu Studio 2.9.1 rule): a 2.85 mm filament prints only on a TPU285 nozzle, a 1.75 mm one
// never on it. The grouping puts each where it fits; a rack without the needed path refuses the slice.
TEST_CASE("H3C: 2.85 mm filament goes to the TPU285 nozzle, 1.75 mm stays off it", "[BblH3C][TPU285]")
{
    auto group_of = [](const DynamicPrintConfig &cfg) {
        Print print;
        Model model;
        ModelObject *object = model.add_object();
        object->add_volume(Test::mesh(Test::TestMesh::cube_20x20x20));
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(165., 160., 0.));
        object->ensure_on_bed();
        print.is_BBL_printer() = true;
        print.apply(model, cfg);
        print.validate();
        print.set_status_silent();
        Test::gcode(print);
        auto group = print.get_layered_nozzle_group_result();
        REQUIRE(group != nullptr);
        auto nozzle = group->get_first_nozzle_for_filament(0);
        REQUIRE(nozzle.has_value());
        return nozzle->volume_type;
    };

    SECTION("Generic TPU (2.85 mm) prints on slot 8, TPU285")
    {
        const DynamicPrintConfig cfg = h3c_config("Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Generic TPU(2.85mm) @BBL H3C");
        REQUIRE(cfg.option<ConfigOptionFloats>("filament_diameter")->get_at(0) == Approx(2.85));
        CHECK(group_of(cfg) == nvtTPU285);
    }
    SECTION("Bambu PLA Basic (1.75 mm) prints on a Standard slot")
    {
        const DynamicPrintConfig cfg = h3c_config("Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Bambu PLA Basic @BBL H3C");
        CHECK(group_of(cfg) == nvtStandard);
    }
    SECTION("validation: the needed nozzle missing from the rack refuses the slice, Bambu's wording")
    {
        DynamicPrintConfig cfg = h3c_config("Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Generic TPU(2.85mm) @BBL H3C");
        PrintConfig pc;
        pc.apply(cfg, true);
        CHECK(validate_tpu285_nozzles(pc, { 0 }).empty());
        // A project rack without the TPU285 slot.
        cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_override", true)->values = { "0.4,0.4,0.4" };
        pc.apply(cfg, true);
        const std::string error = validate_tpu285_nozzles(pc, { 0 });
        CHECK(error.find("The Standard nozzle can not print") == 0);
        CHECK(error.find("2.85") != std::string::npos);
        CHECK(validate_rack_nozzle_diameters(pc, { 0 }) == error);
        // A rack of only TPU285 nozzles cannot print a 1.75 mm filament.
        DynamicPrintConfig pla = h3c_config("Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Bambu PLA Basic @BBL H3C");
        pla.option<ConfigOptionStrings>("extruder_rack_nozzle_override", true)->values = { "0.4:tpu285,0.4:tpu285" };
        pc.apply(pla, true);
        CHECK(validate_tpu285_nozzles(pc, { 0 }).find("The TPU285 nozzle can not print") == 0);
    }
    SECTION("a printer without the TPU285 path keeps treating 2.85 mm as an ordinary filament")
    {
        DynamicPrintConfig cfg = h3c_config("Bambu Lab H2C 0.4 nozzle", "0.20mm Standard @BBL H2C", "Bambu PLA Basic @BBL H2C");
        cfg.option<ConfigOptionFloats>("filament_diameter")->values = { 2.85 };
        PrintConfig pc;
        pc.apply(cfg, true);
        CHECK(validate_tpu285_nozzles(pc, { 0 }).empty());
    }
}

// Owner hand test 2026-11-04: first-layer travels on the H3C were slower than Bambu Studio's. Bambu's processes set
// initial_layer_travel_acceleration (6000, one value per variant), a key we used to drop on load; first-layer travels
// then ran at the initial layer acceleration (M204 S500). The key is ported and wins over first_layer_travel_acceleration.
TEST_CASE("H3C first-layer travels use 6000 mm/s2 like Bambu Studio", "[BblH3C][Accel]")
{
    const DynamicPrintConfig cfg = h3c_config("Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Bambu PLA Basic @BBL H3C");
    const auto *opt = cfg.option<ConfigOptionFloats>("initial_layer_travel_acceleration");
    REQUIRE(opt != nullptr);
    REQUIRE(opt->values.size() == 3); // Standard, High Flow, TPU285 columns
    for (double v : opt->values)
        CHECK(v == Approx(6000.));
    CHECK(cfg.option<ConfigOptionFloatOrPercent>("first_layer_travel_acceleration")->value == Approx(0.));
    // The default is unset, so printers whose presets lack the key keep their old first-layer travels.
    CHECK(print_config_def.get("initial_layer_travel_acceleration")->get_default_value<ConfigOptionFloats>()->values == std::vector<double>{ 0. });
    const std::string gcode = slice_cube(cfg);
    const size_t layer1 = gcode.find("; CHANGE_LAYER");
    const size_t layer2 = gcode.find("; CHANGE_LAYER", layer1 + 1);
    REQUIRE(layer2 != std::string::npos);
    const std::string first = gcode.substr(layer1, layer2 - layer1);
    CHECK(first.find("\nM204 S6000") != std::string::npos);
    CHECK(first.find("\nM204 S500") != std::string::npos);
}

// Owner decision 2026-11-05 (#854): Bambu Studio writes every spiral lift as one G3 on its printers, also when the
// process turns arc fitting off (the H3C presets do). Ours wrote ~24 short Z-climbing G1 segments per lift there, which
// doubled the G1 count and slowed the time estimate. Non-BBL printers keep the segmented spiral without arc fitting.
TEST_CASE("H3C spiral lifts are G3 arcs with arc fitting off, as in Bambu Studio", "[BblH3C][SpiralLift]")
{
    const DynamicPrintConfig cfg = h3c_config("Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Bambu PLA Basic @BBL H3C");
    REQUIRE_FALSE(cfg.opt_bool("enable_arc_fitting"));
    auto count = [](const std::string &gcode, const char *what) {
        size_t n = 0;
        for (size_t pos = gcode.find(what); pos != std::string::npos; pos = gcode.find(what, pos + 1))
            ++n;
        return n;
    };
    const std::string bbl = slice_cube(cfg, true);
    const size_t      arcs = count(bbl, "\nG3 Z") + count(bbl, "\nG2 Z");
    CHECK(arcs > 50); // a lift on (nearly) every layer change of the 100 layers
    const std::string other = slice_cube(cfg, false);
    CHECK(count(other, "\nG3 Z") + count(other, "\nG2 Z") == 0);
}
