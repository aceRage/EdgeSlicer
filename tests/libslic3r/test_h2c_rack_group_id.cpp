// Owner report 2026-11-05 (tests/h2c_mixedslice.gcode.3mf): an H2C with the rack L 0.4, R1-R4 0.4, R5 0.8, R6 0.6 refused a plate
// printed with the 0.8 and the 0.6: "Hotend type mismatch ... replace the hotend in slot 5". The grouping numbered its nozzles over
// the nozzle groups sorted by size (build_nozzle_list), so the 0.6 got id 5 and the 0.8 id 6; the printer checks group 5 against
// its slot-5 hotend. Bambu Studio 2.9.x numbers them by physical slot (build_nozzle_list_from_extruder_nozzles): left 0, right rack
// slots 1..6. These tests pin the slot numbering (grouping_nozzle_list), that a rack of one kind numbers as before, the H3C's
// nine slots, slice_info in a stored 3MF and the get_auto_nozzle_mapping (V0) request.

#include <catch2/catch.hpp>

#include "libslic3r/BambuNozzleMappingRequest.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "nlohmann/json.hpp"

#include <boost/filesystem.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using MultiNozzleUtils::NozzleGroupInfo;
using MultiNozzleUtils::NozzleInfo;

namespace {

const char *OWNER_RACK = "0.4,0.4,0.4,0.4,0.8,0.6";

// An H2C: left one 0.4 nozzle, right a rack of six (the project's rack text, extruder_rack_nozzle_override), three filaments: 1 on the left, 2 and 3 on the right (with only two, each would own an extruder).
DynamicPrintConfig h2c(const std::string &right_rack)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(3);
    cfg.option<ConfigOptionFloats>("nozzle_diameter", true)->values             = { 0.4, 0.4 };
    cfg.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = false;
    cfg.option<ConfigOptionInts>("filament_map", true)->values                  = { 1, 2, 2 };
    cfg.set_key_value("extruder_max_nozzle_count", new ConfigOptionIntsNullable({ 1, 6 }));
    cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_override", true)->values = { "", right_rack };
    return cfg;
}

// What ToolOrdering's build_nozzle_groups gives for such a config: an extruder with a rack one group per kind of rack nozzle,
// any other its nozzle_diameter x extruder_max_nozzle_count.
std::vector<NozzleGroupInfo> nozzle_groups(const DynamicPrintConfig &cfg)
{
    std::vector<NozzleGroupInfo> groups;
    const auto                  &nozzles = cfg.option<ConfigOptionFloats>("nozzle_diameter")->values;
    const auto                  *counts  = cfg.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count");
    for (size_t e = 0; e < nozzles.size(); ++e) {
        std::vector<std::pair<std::string, std::pair<NozzleVolumeType, int>>> rack;
        if (rack_nozzle_groups(cfg, e, rack)) {
            for (const auto &g : rack)
                groups.emplace_back(g.first, g.second.first, int(e), g.second.second);
            continue;
        }
        const int count = counts != nullptr && e < counts->values.size() ? counts->values[e] : 1;
        groups.emplace_back(format_diameter_to_str(nozzles[e]), nvtStandard, int(e), count);
    }
    return groups;
}

void check_same_list(const std::vector<NozzleInfo> &a, const std::vector<NozzleInfo> &b)
{
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CAPTURE(i);
        CHECK(a[i].group_id == b[i].group_id);
        CHECK(a[i].extruder_id == b[i].extruder_id);
        CHECK(a[i].diameter == b[i].diameter);
        CHECK(a[i].volume_type == b[i].volume_type);
    }
}

// The owner's plate grouped: filament 2 pinned to the 0.8, filament 3 to the 0.6 (the owner's filaments 1 and 2) (filament_rack_nozzle_diameter), both on the
// right. Returns the grouping result over grouping_nozzle_list.
std::optional<MultiNozzleUtils::LayeredNozzleGroupResult> owner_plate_group(std::vector<NozzleInfo> *list_out = nullptr)
{
    DynamicPrintConfig cfg = h2c(OWNER_RACK);
    cfg.option<ConfigOptionFloats>("filament_rack_nozzle_diameter", true)->values = { 0., 0.8, 0.6 };
    PrintConfig print_config;
    print_config.apply(cfg, true);
    const std::vector<NozzleInfo>   list = grouping_nozzle_list(print_config, nozzle_groups(cfg));
    const std::vector<unsigned int> used{ 1, 2 };
    // The engine's first answer: both on the right's first nozzle; the pins move them to their sizes.
    std::vector<int> map{ 0, 1, 1 };
    pin_rack_nozzle_diameters(print_config, list, used, map);
    if (list_out)
        *list_out = list;
    return MultiNozzleUtils::LayeredNozzleGroupResult::create(map, list, used);
}

} // namespace

TEST_CASE("The owner's H2C rack is numbered by physical slot", "[H2CRackGroupId]")
{
    const DynamicPrintConfig cfg  = h2c(OWNER_RACK);
    const std::vector<NozzleInfo> list = grouping_nozzle_list(cfg, nozzle_groups(cfg));
    REQUIRE(list.size() == 7);
    const std::vector<std::string> sizes{ "0.4", "0.4", "0.4", "0.4", "0.4", "0.8", "0.6" };
    for (size_t i = 0; i < list.size(); ++i) {
        CAPTURE(i);
        CHECK(list[i].group_id == int(i));
        CHECK(list[i].extruder_id == (i == 0 ? 0 : 1));
        CHECK(list[i].diameter == sizes[i]);
    }
    // The old numbering (the bug): sorted by size, the 0.6 came before the 0.8.
    const std::vector<NozzleInfo> sorted = MultiNozzleUtils::build_nozzle_list(nozzle_groups(cfg));
    REQUIRE(sorted.size() == 7);
    CHECK(sorted[5].diameter == "0.6");
    CHECK(sorted[6].diameter == "0.8");
    // Rack slot k of the right extruder is global slot k.
    PrintConfig print_config;
    print_config.apply(cfg, true);
    CHECK(rack_slot_nozzle_index(print_config, list, 1, 5) == 5);
    CHECK(rack_slot_nozzle_index(print_config, list, 1, 6) == 6);
    CHECK(rack_slot_nozzle_index(print_config, list, 1, 1) == 1);
}

TEST_CASE("Filaments pinned to the 0.8 and the 0.6 get group ids 5 and 6", "[H2CRackGroupId]")
{
    std::vector<NozzleInfo> list;
    const auto              group = owner_plate_group(&list);
    REQUIRE(group);
    const std::vector<int> nozzle_map = group->get_nozzle_map();
    CHECK(nozzle_map[1] == 5);
    CHECK(nozzle_map[2] == 6);
    auto first = group->get_first_nozzle_for_filament(1);
    REQUIRE(first);
    CHECK(first->group_id == 5);
    CHECK(first->diameter == "0.8");
    first = group->get_first_nozzle_for_filament(2);
    REQUIRE(first);
    CHECK(first->group_id == 6);
    CHECK(first->diameter == "0.6");
    // nozzle_diameter_at_nozzle_id (GCode.cpp get_nozzle_diameters_by_nozzle_id) reads get_nozzle_from_id(id) for id 0, 1, ...
    REQUIRE(group->get_nozzle_from_id(5));
    CHECK(group->get_nozzle_from_id(5)->diameter == "0.8");
    REQUIRE(group->get_nozzle_from_id(6));
    CHECK(group->get_nozzle_from_id(6)->diameter == "0.6");
}

TEST_CASE("Filaments pinned to rack slots 5 and 6 print on global slots 5 and 6", "[H2CRackGroupId]")
{
    DynamicPrintConfig cfg = h2c(OWNER_RACK);
    cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values = { 0, 5, 6 };
    PrintConfig print_config;
    print_config.apply(cfg, true);
    const std::vector<NozzleInfo> list = grouping_nozzle_list(print_config, nozzle_groups(cfg));
    std::vector<int>              map{ 0, 1, 1 };
    pin_rack_nozzle_diameters(print_config, list, { 0, 1, 2 }, map);
    CHECK(map == std::vector<int>{ 0, 5, 6 });
    // The P9 slot a filament prints with names the same rack position.
    CHECK(filament_rack_slot(print_config, 2) == 5);
    CHECK(filament_rack_slot(print_config, 3) == 6);
}

TEST_CASE("A rack of one kind of nozzle numbers exactly as before", "[H2CRackGroupId][Identity]")
{
    SECTION("six 0.4 nozzles on the right")
    {
        const DynamicPrintConfig cfg = h2c("0.4,0.4,0.4,0.4,0.4,0.4");
        check_same_list(grouping_nozzle_list(cfg, nozzle_groups(cfg)), MultiNozzleUtils::build_nozzle_list(nozzle_groups(cfg)));
    }
    SECTION("six 0.6 nozzles on the right, a 0.6 left")
    {
        DynamicPrintConfig cfg = h2c("0.6,0.6,0.6,0.6,0.6,0.6");
        cfg.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.6, 0.6 };
        check_same_list(grouping_nozzle_list(cfg, nozzle_groups(cfg)), MultiNozzleUtils::build_nozzle_list(nozzle_groups(cfg)));
    }
    SECTION("no project rack: the stock H2C's default rack, six of its own size (#854)")
    {
        DynamicPrintConfig cfg = h2c("");
        cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_override")->values = {};
        const std::vector<NozzleInfo> list = grouping_nozzle_list(cfg, nozzle_groups(cfg));
        check_same_list(list, MultiNozzleUtils::build_nozzle_list(nozzle_groups(cfg)));
        CHECK(list.size() == 7);
    }
    SECTION("no rack at all (an H2D: one nozzle per extruder): build_nozzle_list itself")
    {
        DynamicPrintConfig cfg = h2c("");
        cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_override")->values = {};
        cfg.set_key_value("extruder_max_nozzle_count", new ConfigOptionIntsNullable({ 1, 1 }));
        const std::vector<NozzleInfo> list = grouping_nozzle_list(cfg, nozzle_groups(cfg));
        check_same_list(list, MultiNozzleUtils::build_nozzle_list(nozzle_groups(cfg)));
        CHECK(list.size() == 2);
    }
}

namespace {
// The H3C (#854): one extruder with a rack of nine, slot 8 (1-based) the 2.85 mm TPU nozzle.
DynamicPrintConfig h3c()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(2);
    cfg.option<ConfigOptionFloats>("nozzle_diameter", true)->values = { 0.4 };
    cfg.set_key_value("extruder_max_nozzle_count", new ConfigOptionIntsNullable({ 9 }));
    return cfg;
}
} // namespace

TEST_CASE("An H3C's nine rack slots are numbered 0..8 in rack order", "[H2CRackGroupId][H3C]")
{
    SECTION("the machine's default rack: TPU285 keeps slot id 7 (the size-sorted list made it 8)")
    {
        DynamicPrintConfig cfg = h3c();
        cfg.option<ConfigOptionEnumsGeneric>("default_nozzle_volume_type", true)->values = { nvtStandard, nvtStandard, nvtStandard, nvtStandard, nvtStandard,
                                                                                           nvtStandard, nvtStandard, nvtTPU285,   nvtStandard };
        REQUIRE(default_rack_nozzle_text(cfg, 0) == "0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4:tpu285,0.4");
        const std::vector<NozzleInfo> list = grouping_nozzle_list(cfg, nozzle_groups(cfg));
        REQUIRE(list.size() == 9);
        for (size_t i = 0; i < list.size(); ++i) {
            CAPTURE(i);
            CHECK(list[i].group_id == int(i));
            CHECK(list[i].extruder_id == 0);
            CHECK(list[i].diameter == "0.4");
            CHECK(list[i].volume_type == (i == 7 ? nvtTPU285 : nvtStandard));
        }
        const std::vector<NozzleInfo> sorted = MultiNozzleUtils::build_nozzle_list(nozzle_groups(cfg));
        REQUIRE(sorted.size() == 9);
        CHECK(sorted[8].volume_type == nvtTPU285);
        PrintConfig print_config;
        print_config.apply(cfg, true);
        CHECK(rack_slot_nozzle_index(print_config, list, 0, 8) == 7);
        CHECK(rack_slot_nozzle_index(print_config, list, 0, 9) == 8);
    }
    SECTION("a project rack of mixed sizes")
    {
        DynamicPrintConfig cfg = h3c();
        cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_override", true)->values = { "0.4,0.6,0.4,0.2,0.4,0.4,0.4,0.4:tpu285,0.8" };
        const std::vector<NozzleInfo>  list = grouping_nozzle_list(cfg, nozzle_groups(cfg));
        const std::vector<std::string> sizes{ "0.4", "0.6", "0.4", "0.2", "0.4", "0.4", "0.4", "0.4", "0.8" };
        REQUIRE(list.size() == 9);
        for (size_t i = 0; i < list.size(); ++i) {
            CAPTURE(i);
            CHECK(list[i].group_id == int(i));
            CHECK(list[i].diameter == sizes[i]);
        }
        CHECK(list[7].volume_type == nvtTPU285);
    }
}

TEST_CASE("The V0 nozzle mapping request carries the physical slot ids", "[H2CRackGroupId][NozzleMapping]")
{
    const auto group = owner_plate_group();
    REQUIRE(group);
    // As SelectMachineDialog builds it: each mapped filament with the grouping's nozzles for it.
    BambuNozzleMapping::RequestInput in;
    in.filament_change_sequence = { 2, 1 };
    in.mapped.push_back({ 1, "0", "0", -1, "GFA00", "000000FF" });
    in.mapped.push_back({ 2, "0", "1", -1, "GFA00", "FFFFFFFF" });
    for (const auto &fila : in.mapped)
        for (const NozzleInfo &nz : group->get_nozzles_for_filament(fila.id))
            in.filament_nozzles.push_back({ fila.id, nz.extruder_id, nz.group_id, nz.diameter, nz.volume_type });
    in.physical_extruder_map = { 1, 0 };
    in.preset_diameters      = { 0.4, 0.4 };
    const auto fila_info = nlohmann::json::parse(BambuNozzleMapping::build_v0_request(in))["print"]["fila_info"];
    REQUIRE(fila_info.size() == 2);
    CHECK(fila_info[0]["id"] == 2);
    CHECK(fila_info[0]["group"] == 5);
    CHECK(fila_info[0]["nozzle_d"] == "0.80");
    CHECK(fila_info[0]["direction"] == 2);
    CHECK(fila_info[1]["id"] == 3);
    CHECK(fila_info[1]["group"] == 6);
    CHECK(fila_info[1]["nozzle_d"] == "0.60");
}

SCENARIO("A stored 3MF's slice_info names the rack slots", "[H2CRackGroupId][3mf]")
{
    const auto group = owner_plate_group();
    REQUIRE(group);

    Model        src_model;
    ModelObject *src_object = src_model.add_object();
    src_object->name        = "h2c_rack_cube";
    src_object->add_volume(make_cube(10., 10., 10.))->name = "cube";
    src_object->add_instance();
    src_object->ensure_on_bed();

    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    // A coEnums option from a static config class has no keys_map and the project writer serialises it (see test_3mf.cpp).
    for (const std::string &key : store_config.keys())
        if (const ConfigOption *opt = store_config.option(key); opt != nullptr && opt->type() == coEnums) {
            store_config.erase(key);
            store_config.option(key, true);
        }
    store_config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.4 }));
    store_config.set_key_value("filament_colour", new ConfigOptionStrings({ "#808080", "#000000", "#FFFFFF" }));
    store_config.set_key_value("filament_map", new ConfigOptionInts({ 1, 2, 2 }));
    store_config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values      = { 0, 0 };
    store_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = { 0, 0 };

    PlateData *plate           = new PlateData();
    plate->plate_index         = 0;
    plate->is_sliced_valid     = true;
    plate->printer_model_id    = "O1C2";
    plate->nozzle_diameters    = "0.4,0.4";
    plate->gcode_prediction    = "1000";
    plate->gcode_weight        = "12.34";
    plate->first_layer_time    = "42";
    plate->filament_maps       = { 1, 2, 2 };
    plate->nozzle_group_result = *group;
    plate->objects_and_instances.emplace_back(0, 0);
    // As PartPlate fills slice_filaments_info from the grouping result.
    for (int fid = 1; fid < 3; ++fid) {
        FilamentInfo info;
        info.id                 = fid;
        info.type               = "PLA";
        info.color              = fid == 1 ? "#000000" : "#FFFFFF";
        info.filament_id        = "GFA00";
        info.used_m             = 1.5f;
        info.used_g             = 4.5f;
        info.used_for_object    = true;
        info.nozzle_volume_type = get_nozzle_volume_type_string(nvtStandard);
        for (const NozzleInfo &nozzle : group->get_nozzles_for_filament(fid)) {
            info.group_id.push_back(nozzle.group_id);
            info.nozzle_diameter = string_to_double_decimal_point(nozzle.diameter);
        }
        plate->slice_filaments_info.push_back(info);
    }

    const std::string             test_file = std::string(TEST_DATA_DIR) + "/test_3mf/h2c_rack_group_id.3mf";
    const boost::filesystem::path tmp_root  = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());

    StoreParams store_params;
    store_params.path            = test_file.c_str();
    store_params.model           = &src_model;
    store_params.config          = &store_config;
    store_params.plate_data_list = { plate };
    store_params.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store_params));

    Model                     dst_model;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &plate_data, &project_presets, &is_bbl_3mf, &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
    REQUIRE(plate_data.size() == 1);
    const PlateData &loaded = *plate_data.front();
    // <filament group_id="5" nozzle_diameter="0.80"> and <filament group_id="6" nozzle_diameter="0.60">.
    REQUIRE(loaded.slice_filaments_info.size() == 2);
    CHECK(loaded.slice_filaments_info[0].group_id == std::vector<int>{ 5 });
    CHECK(loaded.slice_filaments_info[0].nozzle_diameter == Approx(0.8));
    CHECK(loaded.slice_filaments_info[1].group_id == std::vector<int>{ 6 });
    CHECK(loaded.slice_filaments_info[1].nozzle_diameter == Approx(0.6));
    // The <nozzle> table: the used nozzles with their slot ids, on the right extruder (1-based 2).
    REQUIRE(loaded.nozzles_info.size() == 2);
    for (const NozzleInfo &nozzle : loaded.nozzles_info) {
        CAPTURE(nozzle.group_id);
        CHECK(nozzle.extruder_id == 1);
        if (nozzle.group_id == 5)
            CHECK(nozzle.diameter == "0.8");
        else {
            CHECK(nozzle.group_id == 6);
            CHECK(nozzle.diameter == "0.6");
        }
    }

    release_PlateData_list(plate_data);
    boost::filesystem::remove(test_file);
    delete plate;
}
