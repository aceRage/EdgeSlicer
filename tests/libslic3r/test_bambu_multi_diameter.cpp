// AdaptiveLayerMerge step 5: Bambu Studio 2.9.1's multi-diameter 3MF keys (Format/BambuMultiDiameter.hpp).
//  - Export: a sliced plate that prints with several nozzle sizes on one extruder's rack (the H3C) writes
//    multi_nozzle_diameter_mode / filament_diameter_map into project_settings.config and the mode plus Bambu's
//    nozzle_diameters rule into slice_info.config; a single-diameter plate, or a two-extruder plate, writes nothing new.
//  - Import: a Bambu Studio project in that mode sets up the rack, the extruder's size and the per-filament rack pins,
//    and its keys are not reported as unrecognized.
// Plates are stored the way PartPlateList::store_to_3mf_structure fills them. No printer or Bambu Studio was run.
#include <catch2/catch.hpp>

#include "libslic3r/Format/BambuMultiDiameter.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/NozzleOverride.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/ToolSlots.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
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

// A copy of `src` with project_settings.config replaced by `project_text`.
bool rewrite_project_settings(const std::string &src, const std::string &dst, const std::string &project_text)
{
    mz_zip_archive in;
    mz_zip_zero_struct(&in);
    if (!open_zip_reader(&in, src))
        return false;
    mz_zip_archive out;
    mz_zip_zero_struct(&out);
    if (!open_zip_writer(&out, dst)) {
        close_zip_reader(&in);
        return false;
    }
    bool ok = true;
    for (mz_uint i = 0; ok && i < mz_zip_reader_get_num_files(&in); ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&in, i, &stat))
            continue;
        const std::string name = stat.m_filename;
        if (name == "Metadata/project_settings.config") {
            ok = mz_zip_writer_add_mem(&out, name.c_str(), project_text.data(), project_text.size(), MZ_DEFAULT_COMPRESSION);
        } else {
            size_t size = 0;
            void  *data = mz_zip_reader_extract_to_heap(&in, i, &size, 0);
            if (data == nullptr)
                continue;
            ok = mz_zip_writer_add_mem(&out, name.c_str(), data, size, MZ_DEFAULT_COMPRESSION);
            mz_free(data);
        }
    }
    ok = ok && mz_zip_writer_finalize_archive(&out);
    close_zip_writer(&out);
    close_zip_reader(&in);
    return ok;
}

std::string temp_file(const std::string &name)
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() /
        ("snorca_tests_multi_diameter_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    return (tmp_root / name).string();
}

std::string plate_block(const std::string &xml)
{
    const size_t pos = xml.find("<plate>");
    if (pos == std::string::npos)
        return {};
    return xml.substr(pos, xml.find("</plate>", pos) - pos);
}

std::string metadata(const std::string &block, const std::string &key)
{
    const std::regex re("<metadata key=\"" + key + "\" value=\"([^\"]*)\"");
    std::smatch      m;
    return std::regex_search(block, m, re) ? m[1].str() : std::string("<absent>");
}

std::vector<std::string> json_strings(const json &j, const char *key)
{
    std::vector<std::string> out;
    if (j.contains(key))
        for (const auto &v : j[key])
            out.push_back(v.get<std::string>());
    return out;
}

const std::vector<std::string> COLOURS = { "#FF0000", "#00FF00", "#0000FF" };

DynamicPrintConfig base_config(const std::vector<double> &nozzles)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    // coEnums of a static config have no keys_map and the project writer serializes them (see test_3mf.cpp).
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_num_extruders(unsigned(nozzles.size()));
    cfg.set_num_filaments(unsigned(COLOURS.size()));
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values  = nozzles;
    cfg.option<ConfigOptionStrings>("filament_colour")->values = COLOURS;
    cfg.option<ConfigOptionStrings>("filament_ids", true)->values = { "GFA00", "GFA00", "GFA00" };
    cfg.set_key_value("filament_map", new ConfigOptionInts({ 1 }));
    return cfg;
}

// The H3C's shape: one 0.4 extruder with a rack of 0.4 / 0.8 / 0.4; filament 2 pinned to the 0.8.
DynamicPrintConfig h3c_config(bool pin_filament_2)
{
    DynamicPrintConfig cfg = base_config({ 0.4 });
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab H3C"));
    cfg.set_key_value("single_extruder_multi_material", new ConfigOptionBool(true));
    cfg.set_key_value("extruder_rack_nozzle_override", new ConfigOptionStrings({ "0.4,0.8,0.4" }));
    cfg.set_key_value("filament_rack_nozzle_diameter",
                      new ConfigOptionFloats(pin_filament_2 ? std::vector<double>{ 0., 0.8, 0. } : std::vector<double>{ 0. }));
    return cfg;
}

// A sliced plate; `used` holds (0-based filament, nozzle size) in the order the plate lists them.
PlateData *sliced_plate(bool uses_multiple, const std::vector<std::pair<int, double>> &used)
{
    PlateData *plate                      = new PlateData();
    plate->plate_index                    = 0;
    plate->is_sliced_valid                = true;
    plate->printer_model_id               = "O2D";
    plate->gcode_prediction               = "1000";
    plate->gcode_weight                   = "12.34";
    plate->first_layer_time               = "42";
    plate->uses_multiple_nozzle_diameters = uses_multiple;
    plate->objects_and_instances.emplace_back(0, 0);
    for (const auto &[fid, size] : used) {
        FilamentInfo info;
        info.id                 = fid;
        info.type               = "PLA";
        info.color              = COLOURS[size_t(fid)];
        info.filament_id        = "GFA00";
        info.used_m             = 1.5f;
        info.used_g             = 4.5f;
        info.group_id           = { 0 };
        info.nozzle_diameter    = size;
        info.nozzle_volume_type = "Standard";
        info.used_for_object    = true;
        plate->slice_filaments_info.push_back(info);
    }
    return plate;
}

PlateData *unsliced_plate()
{
    PlateData *plate   = new PlateData();
    plate->plate_index = 0;
    plate->objects_and_instances.emplace_back(0, 0);
    return plate;
}

void build_model(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(20., 20., 20.))->name = "cube";
    object->add_instance();
    object->ensure_on_bed();
}

const SaveStrategy CONNECT_EXPORT = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel |
                                    SaveStrategy::WithGcode | SaveStrategy::SkipModel | SaveStrategy::SkipAuxiliary;
const SaveStrategy PROJECT_SAVE   = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;

bool store(const std::string &path, Model &model, DynamicPrintConfig &cfg, const PlateDataPtrs &plates, SaveStrategy strategy)
{
    StoreParams sp;
    sp.path            = path.c_str();
    sp.model           = &model;
    sp.config          = &cfg;
    sp.plate_data_list = plates;
    sp.strategy        = strategy;
    return store_bbs_3mf(sp);
}

struct Exported
{
    std::string plate;   // slice_info.config's first <plate> block
    json        project; // project_settings.config
};

Exported export_plate(DynamicPrintConfig &cfg, PlateData *plate, SaveStrategy strategy, const std::string &name)
{
    Model model;
    build_model(model);
    PlateDataPtrs     plates = { plate };
    const std::string path   = temp_file(name);
    Exported          e;
    if (store(path, model, cfg, plates, strategy)) {
        e.plate   = plate_block(zip_entry(path, "Metadata/slice_info.config"));
        e.project = json::parse(zip_entry(path, "Metadata/project_settings.config"));
    }
    release_PlateData_list(plates);
    boost::filesystem::remove(path);
    return e;
}

} // namespace

TEST_CASE("Bambu multi-diameter mode: which jobs are in it", "[3mf][BambuMultiDiameter]")
{
    SECTION("an H3C rack of mixed sizes is Bambu's single-extruder multi-nozzle machine") {
        const DynamicPrintConfig cfg = h3c_config(true);
        CHECK(BambuMultiDiameter::single_extruder_rack(cfg));
        CHECK(BambuMultiDiameter::mode_for_job(true, cfg));
        CHECK_FALSE(BambuMultiDiameter::mode_for_job(false, cfg));
        CHECK(BambuMultiDiameter::filament_diameter_map(cfg) == std::vector<std::string>{ "0.4", "0.8", "0.4" });
        CHECK(BambuMultiDiameter::config_uses_multiple_diameters(cfg));
    }
    SECTION("the same rack without a pin prints every filament with the 0.4") {
        const DynamicPrintConfig cfg = h3c_config(false);
        CHECK(BambuMultiDiameter::filament_diameter_map(cfg) == std::vector<std::string>{ "0.4", "0.4", "0.4" });
        CHECK_FALSE(BambuMultiDiameter::config_uses_multiple_diameters(cfg));
    }
    SECTION("a two-extruder machine is never in the mode (Bambu's is_single_ext_multi_nozzle)") {
        DynamicPrintConfig cfg = base_config({ 0.2, 0.4 });
        CHECK_FALSE(BambuMultiDiameter::single_extruder_rack(cfg));
        CHECK_FALSE(BambuMultiDiameter::mode_for_job(true, cfg));
    }
    SECTION("a single extruder without a rack is not either") {
        DynamicPrintConfig cfg = base_config({ 0.4 });
        CHECK_FALSE(BambuMultiDiameter::single_extruder_rack(cfg));
    }
}

TEST_CASE("Bambu multi-diameter mode: slice_info nozzle_diameters rule", "[3mf][BambuMultiDiameter]")
{
    auto info = [](int id, double size) {
        FilamentInfo f;
        f.id              = id;
        f.nozzle_diameter = size;
        return f;
    };
    // The lowest slot wins, whatever order the list is in; once per extruder.
    CHECK(BambuMultiDiameter::slice_info_nozzle_diameters({ info(2, 0.4), info(1, 0.8) }, 1, "x") == "0.8");
    CHECK(BambuMultiDiameter::slice_info_nozzle_diameters({ info(0, 0.4), info(1, 0.8) }, 2, "x") == "0.4,0.4");
    // No size on the lowest slot: the plate's own string stays.
    CHECK(BambuMultiDiameter::slice_info_nozzle_diameters({ info(0, 0.), info(1, 0.8) }, 1, "x") == "x");
    CHECK(BambuMultiDiameter::slice_info_nozzle_diameters({}, 1, "x") == "x");
}

TEST_CASE("Bambu multi-diameter export of an H3C plate", "[3mf][BambuMultiDiameter][BambuConnect]")
{
    SECTION("a plate on the 0.4 and the 0.8 writes Bambu Studio's keys") {
        DynamicPrintConfig cfg = h3c_config(true);
        const Exported     e   = export_plate(cfg, sliced_plate(true, { { 0, 0.4 }, { 1, 0.8 } }), CONNECT_EXPORT, "h3c_mixed.gcode.3mf");
        REQUIRE_FALSE(e.plate.empty());
        CHECK(metadata(e.plate, "multi_nozzle_diameter_mode") == "1");
        CHECK(metadata(e.plate, "nozzle_diameters") == "0.4");
        REQUIRE(e.project.contains("multi_nozzle_diameter_mode"));
        CHECK(e.project["multi_nozzle_diameter_mode"].get<std::string>() == "1");
        CHECK(json_strings(e.project, "filament_diameter_map") == std::vector<std::string>{ "0.4", "0.8", "0.4" });
        // The caller's config is left alone.
        CHECK_FALSE(cfg.has("multi_nozzle_diameter_mode"));
        CHECK_FALSE(cfg.has("filament_diameter_map"));
    }
    SECTION("the lowest used slot's nozzle is the one slice_info reports") {
        DynamicPrintConfig cfg = h3c_config(true);
        const Exported     e   = export_plate(cfg, sliced_plate(true, { { 1, 0.8 }, { 2, 0.4 } }), CONNECT_EXPORT, "h3c_08first.gcode.3mf");
        CHECK(metadata(e.plate, "multi_nozzle_diameter_mode") == "1");
        CHECK(metadata(e.plate, "nozzle_diameters") == "0.8");
    }
    SECTION("the key sits after has_filament_switcher and before filament_maps, as Bambu Studio writes it") {
        DynamicPrintConfig cfg = h3c_config(true);
        const Exported     e   = export_plate(cfg, sliced_plate(true, { { 0, 0.4 }, { 1, 0.8 } }), CONNECT_EXPORT, "h3c_order.gcode.3mf");
        const size_t switcher = e.plate.find("key=\"has_filament_switcher\"");
        const size_t mode     = e.plate.find("key=\"multi_nozzle_diameter_mode\"");
        const size_t maps     = e.plate.find("key=\"filament_maps\"");
        REQUIRE(switcher != std::string::npos);
        REQUIRE(mode != std::string::npos);
        REQUIRE(maps != std::string::npos);
        CHECK(switcher < mode);
        CHECK(mode < maps);
    }
    SECTION("a single-diameter plate on the same machine writes no new key") {
        DynamicPrintConfig cfg = h3c_config(true);
        const Exported     e   = export_plate(cfg, sliced_plate(false, { { 0, 0.4 }, { 2, 0.4 } }), CONNECT_EXPORT, "h3c_single.gcode.3mf");
        REQUIRE_FALSE(e.plate.empty());
        CHECK(metadata(e.plate, "multi_nozzle_diameter_mode") == "<absent>");
        CHECK(metadata(e.plate, "nozzle_diameters") == "0.4");
        CHECK_FALSE(e.project.contains("multi_nozzle_diameter_mode"));
        CHECK_FALSE(e.project.contains("filament_diameter_map"));
    }
}

TEST_CASE("Bambu multi-diameter export leaves two-extruder plates alone", "[3mf][BambuMultiDiameter][BambuConnect]")
{
    // An H2D with a 0.2 left and a 0.4 right: Bambu Studio never turns the mode on there, and EdgeSlicer keeps reporting
    // each extruder's own size.
    DynamicPrintConfig cfg = base_config({ 0.2, 0.4 });
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab H2D"));
    cfg.set_key_value("filament_map", new ConfigOptionInts({ 1, 2, 2 }));
    const Exported e = export_plate(cfg, sliced_plate(true, { { 0, 0.2 }, { 1, 0.4 } }), CONNECT_EXPORT, "h2d_mixed.gcode.3mf");
    REQUIRE_FALSE(e.plate.empty());
    CHECK(metadata(e.plate, "multi_nozzle_diameter_mode") == "<absent>");
    CHECK(metadata(e.plate, "nozzle_diameters") == "0.2,0.4");
    CHECK_FALSE(e.project.contains("multi_nozzle_diameter_mode"));
    CHECK_FALSE(e.project.contains("filament_diameter_map"));
}

TEST_CASE("Bambu multi-diameter keys in a project save without a sliced plate", "[3mf][BambuMultiDiameter]")
{
    SECTION("filaments set to two sizes on the rack: the keys are written") {
        DynamicPrintConfig cfg = h3c_config(true);
        const Exported     e   = export_plate(cfg, unsliced_plate(), PROJECT_SAVE, "h3c_project_mixed.3mf");
        REQUIRE(e.project.contains("multi_nozzle_diameter_mode"));
        CHECK(e.project["multi_nozzle_diameter_mode"].get<std::string>() == "1");
        CHECK(json_strings(e.project, "filament_diameter_map") == std::vector<std::string>{ "0.4", "0.8", "0.4" });
    }
    SECTION("every filament on one size: no key") {
        DynamicPrintConfig cfg = h3c_config(false);
        const Exported     e   = export_plate(cfg, unsliced_plate(), PROJECT_SAVE, "h3c_project_single.3mf");
        CHECK_FALSE(e.project.contains("multi_nozzle_diameter_mode"));
        CHECK_FALSE(e.project.contains("filament_diameter_map"));
    }
}

TEST_CASE("Bambu multi-diameter import sets up the project's nozzles", "[3mf][BambuMultiDiameter]")
{
    // What Bambu Studio 2.9.1 saves for an H3C in multi-diameter mode: the printer on its canonical 0.2 preset, the
    // smallest installed nozzle (0.4) in nozzle_diameter, the rack per slot and a size per filament.
    const std::string bambu_project = R"({
        "multi_nozzle_diameter_mode": "1",
        "filament_diameter_map": ["0.4", "0.8", "0.4"],
        "extruder_nozzle_diameter": ["0.4", "0.8", "0.4"],
        "extruder_nozzle_volume_type": ["Standard", "Standard", "High Flow"],
        "printer_variant": "0.2"
    })";

    SECTION("read_keys") {
        const BambuMultiDiameter::ImportedKeys keys = BambuMultiDiameter::read_keys(bambu_project);
        CHECK(keys.has_mode);
        CHECK(keys.mode);
        CHECK(keys.has_map);
        CHECK(keys.diameter_map == std::vector<std::string>{ "0.4", "0.8", "0.4" });
        REQUIRE(keys.slot_diameters.size() == 3);
        CHECK(keys.slot_diameters[1] == Approx(0.8));
        CHECK(keys.slot_volume_types.size() == 3);
        CHECK_FALSE(BambuMultiDiameter::read_keys(R"({"layer_height": "0.2"})").any());
        CHECK_FALSE(BambuMultiDiameter::read_keys("not json, multi_nozzle_diameter_mode").any());
    }

    SECTION("apply_import: rack, extruder size and pins") {
        DynamicPrintConfig cfg = base_config({ 0.4 });
        cfg.set_key_value("printer_variant", new ConfigOptionString("0.2"));
        const BambuMultiDiameter::ImportResult r = BambuMultiDiameter::apply_import(cfg, BambuMultiDiameter::read_keys(bambu_project));
        CHECK(r.rack_set);
        CHECK(r.override_set);
        CHECK(r.pins_set == 1);
        CHECK(cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_override")->values == std::vector<std::string>{ "0.4,0.8,0.4:high_flow" });
        CHECK(cfg.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{ 0.2 });
        CHECK(effective_nozzle_diameters(cfg) == std::vector<double>{ 0.4 });
        const std::vector<double> pins = cfg.option<ConfigOptionFloats>("filament_rack_nozzle_diameter")->values;
        REQUIRE(pins.size() == 3);
        CHECK(pins[0] == 0.);
        CHECK(pins[1] == Approx(0.8));
        CHECK(pins[2] == 0.);
        // What slicing then sees per filament.
        CHECK(nozzle_diameter_for_filament(static_cast<const ConfigBase &>(cfg), 1) == Approx(0.4));
        CHECK(nozzle_diameter_for_filament(static_cast<const ConfigBase &>(cfg), 2) == Approx(0.8));
        CHECK(nozzle_diameter_for_filament(static_cast<const ConfigBase &>(cfg), 3) == Approx(0.4));
        CHECK(BambuMultiDiameter::filament_diameter_map(cfg) == std::vector<std::string>{ "0.4", "0.8", "0.4" });
    }

    SECTION("mode 0: nothing is set up, the keys are still consumed") {
        DynamicPrintConfig cfg = base_config({ 0.4 });
        cfg.set_key_value("printer_variant", new ConfigOptionString("0.2"));
        const DynamicPrintConfig before = cfg;
        const BambuMultiDiameter::ImportedKeys keys = BambuMultiDiameter::read_keys(
            R"({"multi_nozzle_diameter_mode": "0", "filament_diameter_map": ["", "", ""]})");
        const BambuMultiDiameter::ImportResult r = BambuMultiDiameter::apply_import(cfg, keys);
        CHECK_FALSE(r.rack_set);
        CHECK_FALSE(r.override_set);
        CHECK(r.pins_set == 0);
        CHECK(cfg == before);
        std::vector<std::string> unrecognized = { "multi_nozzle_diameter_mode", "some_future_key", "filament_diameter_map",
                                                  "extruder_nozzle_diameter" };
        BambuMultiDiameter::forget_consumed_keys(unrecognized, keys, r);
        CHECK(unrecognized == std::vector<std::string>{ "some_future_key", "extruder_nozzle_diameter" });
    }

    SECTION("a project that already has its own rack, override and pins is left as it is") {
        DynamicPrintConfig cfg = h3c_config(true);
        cfg.set_key_value("printer_variant", new ConfigOptionString("0.4"));
        const DynamicPrintConfig before = cfg;
        const BambuMultiDiameter::ImportResult r = BambuMultiDiameter::apply_import(cfg, BambuMultiDiameter::read_keys(bambu_project));
        CHECK_FALSE(r.rack_set);
        CHECK_FALSE(r.override_set);
        CHECK(r.pins_set == 0);
        CHECK(cfg == before);
    }
}

TEST_CASE("Loading a Bambu Studio multi-diameter project", "[3mf][BambuMultiDiameter]")
{
    // An EdgeSlicer project whose project_settings.config is then given Bambu Studio's multi-diameter keys and shape.
    Model model;
    build_model(model);
    DynamicPrintConfig cfg = base_config({ 0.4 });
    cfg.set_key_value("printer_variant", new ConfigOptionString("0.2"));
    PlateDataPtrs     plates = { unsliced_plate() };
    const std::string edge   = temp_file("edge_project.3mf");
    REQUIRE(store(edge, model, cfg, plates, PROJECT_SAVE));
    release_PlateData_list(plates);

    json project                           = json::parse(zip_entry(edge, "Metadata/project_settings.config"));
    project["multi_nozzle_diameter_mode"]  = "1";
    project["filament_diameter_map"]       = { "0.4", "0.8", "0.4" };
    project["extruder_nozzle_diameter"]    = { "0.4", "0.8", "0.4" };
    project["extruder_nozzle_volume_type"] = { "Standard", "Standard", "Standard" };
    const std::string bambu = temp_file("bambu_multi_diameter.3mf");
    REQUIRE(rewrite_project_settings(edge, bambu, project.dump(4)));

    Model                     loaded_model;
    DynamicPrintConfig        loaded;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             loaded_plates;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool ok = load_bbs_3mf(bambu.c_str(), &loaded, &ctxt, &loaded_model, &loaded_plates, &project_presets, &is_bbl_3mf,
                                 &file_version, nullptr,
                                 LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);
    release_PlateData_list(loaded_plates);
    boost::filesystem::remove(edge);
    boost::filesystem::remove(bambu);

    REQUIRE(ok);
    auto unrecognized = [&ctxt](const char *key) {
        return std::find(ctxt.unrecogized_keys.begin(), ctxt.unrecogized_keys.end(), std::string(key)) != ctxt.unrecogized_keys.end();
    };
    CHECK_FALSE(unrecognized("multi_nozzle_diameter_mode"));
    CHECK_FALSE(unrecognized("filament_diameter_map"));
    CHECK_FALSE(unrecognized("extruder_nozzle_diameter"));
    // Not kept as config keys: EdgeSlicer gates on the plate, not on a switch.
    CHECK_FALSE(loaded.has("multi_nozzle_diameter_mode"));
    CHECK_FALSE(loaded.has("filament_diameter_map"));
    REQUIRE(loaded.option<ConfigOptionStrings>("extruder_rack_nozzle_override") != nullptr);
    CHECK(loaded.option<ConfigOptionStrings>("extruder_rack_nozzle_override")->values == std::vector<std::string>{ "0.4,0.8,0.4" });
    CHECK(loaded.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{ 0.2 });
    CHECK(effective_nozzle_diameters(loaded) == std::vector<double>{ 0.4 });
    CHECK(BambuMultiDiameter::filament_diameter_map(loaded) == std::vector<std::string>{ "0.4", "0.8", "0.4" });
}
