// The filament metadata of a sliced-plate 3MF (.gcode.3mf), the file "Send to Bambu Connect" hands
// to Bambu Connect. Connect builds its filament / colour / AMS-slot list from three files that must
// agree, index for index (owner-provided facts about Connect 2.5.0-beta.15, PR #565):
//  - Metadata/slice_info.config: per plate filament_maps, and one <filament> per USED filament
//    (1-based id, tray_info_idx, type, color);
//  - Metadata/model_settings.config: per plate filament_maps + filament_map_mode;
//  - Metadata/project_settings.config: filament_map next to the per-filament filament_colour /
//    filament_type / filament_ids arrays.
// These cases store plates the way the application fills them (Plater::export_3mf /
// PartPlateList::store_to_3mf_structure) and read the three files back. No Connect was run.
#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <regex>
#include <string>
#include <vector>

using namespace Slic3r;
using nlohmann::json;

namespace {

std::string zip_entry(const std::string &zip_path, const std::string &entry_name)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return {};
    size_t      size = 0;
    void       *data = mz_zip_reader_extract_file_to_heap(&archive, entry_name.c_str(), &size, 0);
    std::string result;
    if (data != nullptr) {
        result.assign(static_cast<const char *>(data), size);
        mz_free(data);
    }
    close_zip_reader(&archive);
    return result;
}

std::string temp_file(const std::string &name)
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() /
        ("snorca_tests_connect_maps_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    return (tmp_root / name).string();
}

// The text of every <plate>...</plate> block, in file order.
std::vector<std::string> plate_blocks(const std::string &xml)
{
    std::vector<std::string> blocks;
    size_t                   pos = 0;
    while ((pos = xml.find("<plate>", pos)) != std::string::npos) {
        const size_t end = xml.find("</plate>", pos);
        if (end == std::string::npos)
            break;
        blocks.push_back(xml.substr(pos, end - pos));
        pos = end;
    }
    return blocks;
}

// <metadata key="KEY" value="..."/> inside one block; "<absent>" when there is none.
std::string metadata(const std::string &block, const std::string &key)
{
    const std::regex re("<metadata key=\"" + key + "\" value=\"([^\"]*)\"");
    std::smatch      m;
    return std::regex_search(block, m, re) ? m[1].str() : std::string("<absent>");
}

struct SliceInfoFilament
{
    int         id; // 1-based, as written
    std::string tray_info_idx;
    std::string type;
    std::string color;
};

std::vector<SliceInfoFilament> slice_info_filaments(const std::string &block)
{
    std::vector<SliceInfoFilament> out;
    static const std::regex re("<filament id=\"(\\d+)\" tray_info_idx=\"([^\"]*)\" type=\"([^\"]*)\" color=\"([^\"]*)\"");
    for (auto it = std::sregex_iterator(block.begin(), block.end(), re); it != std::sregex_iterator(); ++it)
        out.push_back({ std::stoi((*it)[1].str()), (*it)[2].str(), (*it)[3].str(), (*it)[4].str() });
    return out;
}

std::vector<std::string> split_spaces(const std::string &s)
{
    std::vector<std::string> out;
    std::string              cur;
    for (char c : s) {
        if (c == ' ') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else
            cur += c;
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

const std::vector<std::string> COLOURS = { "#FF0000", "#00FF00", "#0000FF", "#FFFF00" };
const std::vector<std::string> TYPES   = { "PLA", "PETG", "PLA", "PLA" };
const std::vector<std::string> IDS     = { "GFSA00", "GFSG02_00", "GFSA00", "GFSA01" };

// A project config as the application's full_config() has it, four filaments, filament_map left at
// its one-entry default - exactly what a single-nozzle Bambu printer carries in this fork.
DynamicPrintConfig printer_config(size_t extruders, bool single_extruder_mm, const std::string &printer_model)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    // coEnums of a static config have no keys_map and the project writer serializes them (see the
    // support-group scenario in test_3mf.cpp); rebuild them from print_config_def.
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_num_extruders(unsigned(extruders));
    cfg.set_num_filaments(4);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values = std::vector<double>(extruders, 0.4);
    cfg.option<ConfigOptionStrings>("filament_colour")->values = COLOURS;
    cfg.option<ConfigOptionStrings>("filament_type")->values   = TYPES;
    cfg.option<ConfigOptionStrings>("filament_ids", true)->values = IDS;
    cfg.set_key_value("filament_map", new ConfigOptionInts({ 1 }));
    cfg.set_key_value("single_extruder_multi_material", new ConfigOptionBool(single_extruder_mm));
    cfg.set_key_value("printer_model", new ConfigOptionString(printer_model));
    return cfg;
}

void build_model(Model &model, size_t instances)
{
    ModelObject *object = model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(20., 20., 20.))->name = "cube";
    for (size_t i = 0; i < instances; ++i)
        object->add_instance()->set_offset({ 40. * double(i), 0., 0. });
    object->ensure_on_bed();
}

// A plate as PartPlateList::store_to_3mf_structure + Plater::export_3mf fill it after slicing:
// filament_maps is the sliced Print's filament_map, one FilamentInfo per USED filament (0-based
// id) with type / colour / tray id taken from the project config.
PlateData *sliced_plate(int index, int instance, const std::vector<int> &sliced_map, const std::vector<int> &used,
                        DynamicPrintConfig &cfg)
{
    PlateData *plate        = new PlateData();
    plate->plate_index      = index;
    plate->is_sliced_valid  = true;
    plate->printer_model_id = "BL-P001";
    plate->gcode_prediction = "1000";
    plate->gcode_weight     = "12.34";
    plate->first_layer_time = "42";
    plate->filament_maps    = sliced_map;
    plate->objects_and_instances.emplace_back(0, instance);
    const auto *colours = cfg.option<ConfigOptionStrings>("filament_colour");
    const auto *ids     = cfg.option<ConfigOptionStrings>("filament_ids");
    for (int fid : used) {
        FilamentInfo info;
        info.id = fid;
        std::string display_type;
        info.type        = cfg.get_filament_type(display_type, fid);
        info.color       = colours->get_at(fid);
        info.filament_id = ids->get_at(fid);
        info.used_m      = 1.5f;
        info.used_g      = 4.5f;
        info.group_id    = { 0 };
        info.nozzle_diameter    = 0.4;
        info.nozzle_volume_type = get_nozzle_volume_type_string(nvtStandard);
        info.used_for_object    = true;
        plate->slice_filaments_info.push_back(info);
    }
    return plate;
}

PlateData *unsliced_plate(int index, int instance)
{
    PlateData *plate = new PlateData();
    plate->plate_index = index;
    plate->objects_and_instances.emplace_back(0, instance);
    return plate;
}

// What "Send to Bambu Connect" / "Export plate sliced file" pass (Plater::send_to_bambu_connect).
const SaveStrategy CONNECT_EXPORT = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel |
                                    SaveStrategy::WithGcode | SaveStrategy::SkipModel | SaveStrategy::SkipAuxiliary;
// A project save ("Save project").
const SaveStrategy PROJECT_SAVE = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;

bool store(const std::string &path, Model &model, DynamicPrintConfig &cfg, const PlateDataPtrs &plates, SaveStrategy strategy)
{
    StoreParams sp;
    sp.path            = path.c_str();
    sp.model           = &model;
    sp.config          = &cfg;
    sp.plate_data_list = plates;
    sp.strategy        = strategy;
    const bool ok = store_bbs_3mf(sp);
    return ok;
}

struct Exported
{
    std::vector<std::string> slice_info_plates;
    std::vector<std::string> model_settings_plates;
    json                     project;
};

Exported read_back(const std::string &path)
{
    Exported e;
    e.slice_info_plates     = plate_blocks(zip_entry(path, "Metadata/slice_info.config"));
    e.model_settings_plates = plate_blocks(zip_entry(path, "Metadata/model_settings.config"));
    e.project               = json::parse(zip_entry(path, "Metadata/project_settings.config"));
    return e;
}

std::vector<std::string> json_strings(const json &j, const char *key)
{
    std::vector<std::string> out;
    if (j.contains(key))
        for (const auto &v : j[key])
            out.push_back(v.get<std::string>());
    return out;
}

// The cross-file check Connect relies on: every used filament's colour / type / tray id agrees with
// the project's per-filament arrays at the same index, and the three filament maps agree.
void check_consistent(const Exported &e, size_t plate)
{
    REQUIRE(plate < e.slice_info_plates.size());
    REQUIRE(plate < e.model_settings_plates.size());
    const std::string              si_map = metadata(e.slice_info_plates[plate], "filament_maps");
    const std::string              ms_map = metadata(e.model_settings_plates[plate], "filament_maps");
    const std::vector<std::string> proj_colour = json_strings(e.project, "filament_colour");
    const std::vector<std::string> proj_type   = json_strings(e.project, "filament_type");
    const std::vector<std::string> proj_ids    = json_strings(e.project, "filament_ids");
    const std::vector<std::string> proj_map    = json_strings(e.project, "filament_map");
    INFO("plate " << plate + 1 << ": slice_info map '" << si_map << "', model_settings map '" << ms_map << "'");
    CHECK(si_map == ms_map);
    CHECK(split_spaces(si_map).size() == proj_colour.size());
    CHECK(proj_map.size() == proj_colour.size());
    CHECK(proj_type.size() == proj_colour.size());
    CHECK(proj_ids.size() == proj_colour.size());
    for (const SliceInfoFilament &f : slice_info_filaments(e.slice_info_plates[plate])) {
        INFO("filament id " << f.id);
        REQUIRE(f.id >= 1);
        REQUIRE(size_t(f.id) <= proj_colour.size());
        CHECK(f.color == proj_colour[f.id - 1]);
        CHECK(f.type == proj_type[f.id - 1]);
        CHECK(f.tray_info_idx == proj_ids[f.id - 1]);
    }
}

} // namespace

TEST_CASE("Connect export of an X1C plate using filaments 2 and 4 of 4", "[3mf][BambuConnect]")
{
    Model model;
    build_model(model, 1);
    DynamicPrintConfig cfg = printer_config(1, true, "Bambu Lab X1 Carbon");
    // The sliced Print's filament_map on a single-nozzle machine: the one-entry default.
    PlateDataPtrs plates = { sliced_plate(0, 0, { 1 }, { 1, 3 }, cfg) };
    const std::string path = temp_file("x1c_2of4.gcode.3mf");
    REQUIRE(store(path, model, cfg, plates, CONNECT_EXPORT));
    const Exported e = read_back(path);

    REQUIRE(e.slice_info_plates.size() == 1);
    REQUIRE(e.model_settings_plates.size() == 1);

    SECTION("slice_info lists only the used filaments, 1-based, with their own colour, type and tray id") {
        const std::vector<SliceInfoFilament> f = slice_info_filaments(e.slice_info_plates[0]);
        REQUIRE(f.size() == 2);
        CHECK(f[0].id == 2);
        CHECK(f[0].color == "#00FF00");
        CHECK(f[0].type == "PETG");
        CHECK(f[0].tray_info_idx == "GFSG02_00");
        CHECK(f[1].id == 4);
        CHECK(f[1].color == "#FFFF00");
        CHECK(f[1].type == "PLA");
        CHECK(f[1].tray_info_idx == "GFSA01");
        CHECK(metadata(e.slice_info_plates[0], "filament_maps") == "1 1 1 1");
    }

    SECTION("model_settings carries the same map in Auto mode") {
        CHECK(metadata(e.model_settings_plates[0], "filament_map_mode") == "Auto For Flush");
        CHECK(metadata(e.model_settings_plates[0], "filament_maps") == "1 1 1 1");
    }

    SECTION("project_settings filament_map has one entry per filament, like the colour / type / id arrays") {
        // The defect: this used to be ["1"] next to four colours, so Connect found no map entry for
        // filaments 2 to 4.
        CHECK(json_strings(e.project, "filament_map") == std::vector<std::string>{ "1", "1", "1", "1" });
        CHECK(json_strings(e.project, "filament_colour") == COLOURS);
        CHECK(json_strings(e.project, "filament_type") == TYPES);
        CHECK(json_strings(e.project, "filament_ids") == IDS);
    }

    SECTION("the three files agree index for index") {
        check_consistent(e, 0);
    }

    SECTION("the caller's config is left alone") {
        CHECK(cfg.option<ConfigOptionInts>("filament_map")->values == std::vector<int>{ 1 });
    }

    release_PlateData_list(plates);
    boost::filesystem::remove(path);
}

TEST_CASE("Connect export of an H2D two-nozzle plate", "[3mf][BambuConnect]")
{
    Model model;
    build_model(model, 2);
    // A dual-nozzle Bambu: one AMS feeding two extruders, so not an identical-toolhead machine.
    DynamicPrintConfig cfg = printer_config(2, true, "Bambu Lab H2D");

    SECTION("auto-grouped plate: model_settings reports the map the plate was sliced with") {
        // Filament 2 on extruder 1, filament 4 on extruder 2, as ToolOrdering grouped them. The
        // project-level map is stale (the CLI never mirrors the Print's map into it; the GUI holds
        // the last sliced plate's): model_settings used to write it, "1 1 1 1", while slice_info
        // said "2 1 2 2".
        PlateDataPtrs     plates = { sliced_plate(0, 0, { 2, 1, 2, 2 }, { 1, 3 }, cfg) };
        const std::string path   = temp_file("h2d_auto.gcode.3mf");
        REQUIRE(store(path, model, cfg, plates, CONNECT_EXPORT));
        const Exported e = read_back(path);
        CHECK(metadata(e.slice_info_plates[0], "filament_maps") == "2 1 2 2");
        CHECK(metadata(e.model_settings_plates[0], "filament_maps") == "2 1 2 2");
        CHECK(metadata(e.model_settings_plates[0], "filament_map_mode") == "Auto For Flush");
        CHECK(json_strings(e.project, "filament_map").size() == 4);
        check_consistent(e, 0);
        release_PlateData_list(plates);
        boost::filesystem::remove(path);
    }

    SECTION("auto-grouped plate in the GUI: the project map already mirrors the sliced plate") {
        cfg.option<ConfigOptionInts>("filament_map")->values = { 2, 1, 2, 2 };
        PlateDataPtrs     plates = { sliced_plate(0, 0, { 2, 1, 2, 2 }, { 1, 3 }, cfg) };
        const std::string path   = temp_file("h2d_gui.gcode.3mf");
        REQUIRE(store(path, model, cfg, plates, CONNECT_EXPORT));
        const Exported e = read_back(path);
        CHECK(json_strings(e.project, "filament_map") == std::vector<std::string>{ "2", "1", "2", "2" });
        CHECK(metadata(e.model_settings_plates[0], "filament_maps") == "2 1 2 2");
        check_consistent(e, 0);
        release_PlateData_list(plates);
        boost::filesystem::remove(path);
    }

    SECTION("hand-grouped plate: Manual mode and its own map in both files") {
        PlateData *plate = sliced_plate(0, 0, { 1, 2, 1, 2 }, { 1, 3 }, cfg);
        plate->config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(FilamentMapMode::fmmManual));
        plate->config.set_key_value("filament_map", new ConfigOptionInts({ 1, 2, 1, 2 }));
        PlateDataPtrs     plates = { plate };
        const std::string path   = temp_file("h2d_manual.gcode.3mf");
        REQUIRE(store(path, model, cfg, plates, CONNECT_EXPORT));
        const Exported e = read_back(path);
        CHECK(metadata(e.model_settings_plates[0], "filament_map_mode") == "Manual");
        CHECK(metadata(e.model_settings_plates[0], "filament_maps") == "1 2 1 2");
        CHECK(metadata(e.slice_info_plates[0], "filament_maps") == "1 2 1 2");
        check_consistent(e, 0);
        release_PlateData_list(plates);
        boost::filesystem::remove(path);
    }

    SECTION("send all plates: each plate keeps its own map, not the last sliced plate's") {
        // The project map holds plate 2's grouping (sliced last). Connect reads plate[0].
        cfg.option<ConfigOptionInts>("filament_map")->values = { 2, 1, 2, 2 };
        PlateDataPtrs     plates = { sliced_plate(0, 0, { 1, 2, 1, 1 }, { 1, 3 }, cfg),
                                     sliced_plate(1, 1, { 2, 1, 2, 2 }, { 1, 3 }, cfg) };
        const std::string path   = temp_file("h2d_all.gcode.3mf");
        REQUIRE(store(path, model, cfg, plates, CONNECT_EXPORT));
        const Exported e = read_back(path);
        REQUIRE(e.slice_info_plates.size() == 2);
        REQUIRE(e.model_settings_plates.size() == 2);
        CHECK(metadata(e.model_settings_plates[0], "filament_maps") == "1 2 1 1");
        CHECK(metadata(e.model_settings_plates[1], "filament_maps") == "2 1 2 2");
        CHECK(metadata(e.slice_info_plates[0], "filament_maps") == "1 2 1 1");
        CHECK(metadata(e.slice_info_plates[1], "filament_maps") == "2 1 2 2");
        release_PlateData_list(plates);
        boost::filesystem::remove(path);
    }
}

TEST_CASE("A project save keeps its filament maps as before", "[3mf][BambuConnect]")
{
    Model model;
    build_model(model, 2);

    SECTION("X1C: project_settings filament_map is written as the project holds it") {
        DynamicPrintConfig cfg    = printer_config(1, true, "Bambu Lab X1 Carbon");
        PlateDataPtrs      plates = { unsliced_plate(0, 0), unsliced_plate(1, 1) };
        const std::string  path   = temp_file("x1c_project.3mf");
        REQUIRE(store(path, model, cfg, plates, PROJECT_SAVE));
        CHECK(zip_entry(path, "Metadata/slice_info.config").find("<filament ") == std::string::npos);
        const json project = json::parse(zip_entry(path, "Metadata/project_settings.config"));
        // Unpadded: a reloaded project re-slices with exactly the config it was saved with.
        CHECK(json_strings(project, "filament_map") == std::vector<std::string>{ "1" });
        const std::vector<std::string> ms = plate_blocks(zip_entry(path, "Metadata/model_settings.config"));
        REQUIRE(ms.size() == 2);
        for (const std::string &block : ms) {
            CHECK(metadata(block, "filament_map_mode") == "Auto For Flush");
            CHECK(metadata(block, "filament_maps") == "1 1 1 1");
        }
        release_PlateData_list(plates);
        boost::filesystem::remove(path);
    }

    SECTION("H2D: every unsliced plate writes the project map, as before") {
        DynamicPrintConfig cfg = printer_config(2, true, "Bambu Lab H2D");
        cfg.option<ConfigOptionInts>("filament_map")->values = { 2, 1, 2, 2 };
        PlateDataPtrs     plates = { unsliced_plate(0, 0), unsliced_plate(1, 1) };
        const std::string path   = temp_file("h2d_project.3mf");
        REQUIRE(store(path, model, cfg, plates, PROJECT_SAVE));
        const json project = json::parse(zip_entry(path, "Metadata/project_settings.config"));
        CHECK(json_strings(project, "filament_map") == std::vector<std::string>{ "2", "1", "2", "2" });
        const std::vector<std::string> ms = plate_blocks(zip_entry(path, "Metadata/model_settings.config"));
        REQUIRE(ms.size() == 2);
        for (const std::string &block : ms)
            CHECK(metadata(block, "filament_maps") == "2 1 2 2");
        release_PlateData_list(plates);
        boost::filesystem::remove(path);
    }

    SECTION("U1 (four identical toolheads): model_settings reports filament n on toolhead n, project map untouched") {
        DynamicPrintConfig cfg = printer_config(4, false, "Snapmaker U1");
        REQUIRE(identity_filament_map(cfg, 4) == std::vector<int>{ 1, 2, 3, 4 });
        PlateDataPtrs     plates = { unsliced_plate(0, 0) };
        const std::string path   = temp_file("u1_project.3mf");
        REQUIRE(store(path, model, cfg, plates, PROJECT_SAVE));
        const std::vector<std::string> ms = plate_blocks(zip_entry(path, "Metadata/model_settings.config"));
        REQUIRE(ms.size() == 1);
        CHECK(metadata(ms[0], "filament_map_mode") == "Auto For Flush");
        CHECK(metadata(ms[0], "filament_maps") == "1 2 3 4");
        const json project = json::parse(zip_entry(path, "Metadata/project_settings.config"));
        CHECK(json_strings(project, "filament_map") == std::vector<std::string>{ "1" });
        release_PlateData_list(plates);
        boost::filesystem::remove(path);
    }
}

TEST_CASE("A sliced U1 export reports the same toolhead map in slice_info and model_settings", "[3mf][BambuConnect]")
{
    Model model;
    build_model(model, 1);
    DynamicPrintConfig cfg    = printer_config(4, false, "Snapmaker U1");
    PlateDataPtrs      plates = { sliced_plate(0, 0, { 1 }, { 1, 3 }, cfg) };
    const std::string  path   = temp_file("u1_sliced.gcode.3mf");
    REQUIRE(store(path, model, cfg, plates, CONNECT_EXPORT));
    const Exported e = read_back(path);
    CHECK(metadata(e.slice_info_plates[0], "filament_maps") == "1 2 3 4");
    CHECK(metadata(e.model_settings_plates[0], "filament_maps") == "1 2 3 4");
    release_PlateData_list(plates);
    boost::filesystem::remove(path);
}
