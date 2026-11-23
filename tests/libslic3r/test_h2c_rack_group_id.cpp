// Owner report 2026-11-05 (tests/h2c_mixedslice.gcode.3mf): an H2C with the rack L 0.4, R1-R4 0.4, R5 0.8, R6 0.6 refused a plate
// printed with the 0.8 and the 0.6: "Hotend type mismatch ... replace the hotend in slot 5". The grouping numbered its nozzles over
// the nozzle groups sorted by size (build_nozzle_list), so the 0.6 got id 5 and the 0.8 id 6; the printer checks group 5 against
// its slot-5 hotend. Bambu Studio 2.9.x numbers them by physical slot (build_nozzle_list_from_extruder_nozzles): left 0, right rack
// slots 1..6. These tests pin the slot numbering (grouping_nozzle_list), that a rack of one kind numbers as before, the H3C's
// nine slots, slice_info in a stored 3MF and the get_auto_nozzle_mapping (V0) request.

#include <catch2/catch.hpp>

#include "libslic3r/BambuNozzleMappingRequest.hpp"
#include "libslic3r/FilamentGroup.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "nlohmann/json.hpp"

#include <boost/filesystem.hpp>

#include <set>
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

namespace {
// A plate's grouping on `cfg`: filaments 2 and 3 (1-based) pinned to `pin2` and `pin3` (0 = no pin), both starting on the extruder-1 nozzle 1.
PrintConfig mixed_warning_config(DynamicPrintConfig &cfg, double pin2, double pin3)
{
    cfg.option<ConfigOptionFloats>("filament_rack_nozzle_diameter", true)->values = { 0., pin2, pin3 };
    PrintConfig print_config;
    print_config.apply(cfg, true);
    return print_config;
}

std::optional<MultiNozzleUtils::LayeredNozzleGroupResult> pinned_group(const PrintConfig &print_config, const DynamicPrintConfig &cfg)
{
    const std::vector<NozzleInfo>   list = grouping_nozzle_list(print_config, nozzle_groups(cfg));
    const std::vector<unsigned int> used{ 1, 2 };
    std::vector<int>                map{ 0, 1, 1 };
    pin_rack_nozzle_diameters(print_config, list, used, map);
    return MultiNozzleUtils::LayeredNozzleGroupResult::create(map, list, used);
}
} // namespace

TEST_CASE("The mixed rack diameter warning", "[H2CRackGroupId][MixedDiameterWarning]")
{
    SECTION("an H2C plate on the 0.8 and the 0.6 of one rack warns")
    {
        DynamicPrintConfig cfg   = h2c(OWNER_RACK);
        const PrintConfig  pc    = mixed_warning_config(cfg, 0.8, 0.6);
        const auto         group = pinned_group(pc, cfg);
        REQUIRE(group);
        CHECK(rack_group_mixes_nozzle_diameters(pc, *group));
    }
    SECTION("an H2C plate on one rack diameter does not warn")
    {
        DynamicPrintConfig cfg   = h2c(OWNER_RACK);
        const PrintConfig  pc    = mixed_warning_config(cfg, 0.4, 0.4);
        const auto         group = pinned_group(pc, cfg);
        REQUIRE(group);
        CHECK_FALSE(rack_group_mixes_nozzle_diameters(pc, *group));
    }
    SECTION("an H2C plate on the 0.8 only does not warn, though the rack holds other sizes")
    {
        DynamicPrintConfig cfg   = h2c(OWNER_RACK);
        const PrintConfig  pc    = mixed_warning_config(cfg, 0.8, 0.8);
        const auto         group = pinned_group(pc, cfg);
        REQUIRE(group);
        CHECK_FALSE(rack_group_mixes_nozzle_diameters(pc, *group));
    }
    SECTION("an H3C single-extruder rack with mixed sizes does not warn")
    {
        DynamicPrintConfig cfg = h3c();
        cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_override", true)->values = { "0.4,0.6,0.4,0.2,0.4,0.4,0.4,0.4:tpu285,0.8" };
        cfg.set_num_filaments(3);
        cfg.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = false;
        cfg.option<ConfigOptionInts>("filament_map", true)->values                  = { 1, 1, 1 };
        const PrintConfig pc = mixed_warning_config(cfg, 0.8, 0.6);
        const auto        group = pinned_group(pc, cfg);
        REQUIRE(group);
        // The two filaments really sit on two diameters of the one extruder's rack.
        const auto a = group->get_first_nozzle_for_filament(1);
        const auto c = group->get_first_nozzle_for_filament(2);
        REQUIRE(a);
        REQUIRE(c);
        CHECK(a->diameter != c->diameter);
        CHECK_FALSE(rack_group_mixes_nozzle_diameters(pc, *group));
    }
}

// ---- Rules S and P (owner 2026-11-22): pinned filaments spread over free nozzles of their size; a pin holds on an automatic map --

namespace {

// The rack of fixture h2c3_mix4_2211_pn: grouping nozzles 0 = L 0.4, 1 = R 0.4, 2 = R 0.6, 3 = R 0.4, 4 = R 0.6.
const char *TWO_SIXES = "0.4,0.6,0.4,0.6";

DynamicPrintConfig h2c_pins(const std::vector<int> &map, const std::vector<double> &sizes, const std::vector<int> &slots = { 0 },
                            const std::string &mode = "Manual")
{
    DynamicPrintConfig cfg = h2c(TWO_SIXES);
    cfg.set_num_filaments(unsigned(map.size()));
    cfg.option<ConfigOptionInts>("filament_map", true)->values                    = map;
    cfg.option<ConfigOptionFloats>("filament_rack_nozzle_diameter", true)->values = sizes;
    cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values       = slots;
    cfg.set_deserialize_strict("filament_map_mode", mode);
    return cfg;
}

std::vector<int> pinned_map(const DynamicPrintConfig &cfg, std::vector<int> map)
{
    PrintConfig print_config;
    print_config.apply(cfg, true);
    const std::vector<NozzleInfo> list = grouping_nozzle_list(print_config, nozzle_groups(cfg));
    REQUIRE(list.size() == 5);
    std::vector<unsigned int> used;
    for (size_t f = 0; f < map.size(); ++f)
        used.push_back(unsigned(f));
    pin_rack_nozzle_diameters(print_config, list, used, map);
    // Whatever the pass does, every filament ends on a nozzle of the size it was sliced for.
    const auto group = MultiNozzleUtils::LayeredNozzleGroupResult::create(map, list, used);
    REQUIRE(group);
    CHECK(check_rack_nozzle_diameters(print_config, *group).empty());
    return map;
}

} // namespace

TEST_CASE("Rule S: two filaments pinned to the 0.6 take the rack's two 0.6 nozzles", "[H2CRackGroupId][RackPins]")
{
    // Fixture h2c3_mix4_2211_pn: before the fix both ended on slot 2 and flushed into each other.
    const DynamicPrintConfig cfg = h2c_pins({ 2, 2, 1, 1 }, { 0.6, 0.6, 0., 0. });
    CHECK(pinned_map(cfg, { 1, 3, 0, 0 }) == std::vector<int>{ 2, 4, 0, 0 });
    CHECK(pinned_map(cfg, { 1, 1, 0, 0 }) == std::vector<int>{ 2, 4, 0, 0 });
    CHECK(pinned_map(cfg, { 3, 1, 0, 0 }) == std::vector<int>{ 2, 4, 0, 0 });
}

TEST_CASE("Rule S: three filaments pinned to two 0.6 nozzles share the first, the same way every time", "[H2CRackGroupId][RackPins]")
{
    const DynamicPrintConfig cfg = h2c_pins({ 2, 2, 2, 1 }, { 0.6, 0.6, 0.6, 0. });
    CHECK(pinned_map(cfg, { 1, 1, 3, 0 }) == std::vector<int>{ 2, 4, 2, 0 });
    CHECK(pinned_map(cfg, { 3, 1, 1, 0 }) == std::vector<int>{ 2, 4, 2, 0 });
    // One already on a 0.6 keeps it; the others fill the free one, then share the first.
    CHECK(pinned_map(cfg, { 1, 4, 3, 0 }) == std::vector<int>{ 2, 4, 2, 0 });
}

TEST_CASE("Rule S: a pinned filament already on a nozzle of its size stays there", "[H2CRackGroupId][RackPins]")
{
    const DynamicPrintConfig cfg = h2c_pins({ 2, 2, 1 }, { 0.6, 0.6, 0. });
    CHECK(pinned_map(cfg, { 4, 1, 0 }) == std::vector<int>{ 4, 2, 0 });
    CHECK(pinned_map(cfg, { 2, 1, 0 }) == std::vector<int>{ 2, 4, 0 });
    CHECK(pinned_map(cfg, { 2, 4, 0 }) == std::vector<int>{ 2, 4, 0 });
}

TEST_CASE("Rule S: two pinned filaments the grouping put on one nozzle of their size spread to a free one", "[H2CRackGroupId][RackPins]")
{
    // Fixture h2c4_onsize_2211: the grouping already put both on one 0.6; the second moves to the free 0.6.
    const DynamicPrintConfig two = h2c_pins({ 2, 2, 1 }, { 0.6, 0.6, 0. });
    CHECK(pinned_map(two, { 2, 2, 0 }) == std::vector<int>{ 2, 4, 0 });
    CHECK(pinned_map(two, { 4, 4, 0 }) == std::vector<int>{ 4, 2, 0 });
    // No free one left: the third keeps the nozzle it shares.
    const DynamicPrintConfig three = h2c_pins({ 2, 2, 2, 1 }, { 0.6, 0.6, 0.6, 0. });
    CHECK(pinned_map(three, { 4, 4, 4, 0 }) == std::vector<int>{ 4, 2, 4, 0 });
    // Sharing with an unpinned filament is the grouping's choice: kept.
    const DynamicPrintConfig one = h2c_pins({ 2, 2, 1 }, { 0.4, 0., 0. });
    CHECK(pinned_map(one, { 1, 1, 0 }) == std::vector<int>{ 1, 1, 0 });
}

TEST_CASE("Rule S: slot pins keep their slot; a size pin avoids a slot that is taken", "[H2CRackGroupId][RackPins]")
{
    SECTION("two filaments pinned to slot 2 share it although slot 4 is free")
    {
        const DynamicPrintConfig cfg = h2c_pins({ 2, 2, 1 }, { 0. }, { 2, 2, 0 });
        CHECK(pinned_map(cfg, { 1, 3, 0 }) == std::vector<int>{ 2, 2, 0 });
    }
    SECTION("a slot pin on slot 2 and a size pin on 0.6")
    {
        const DynamicPrintConfig cfg = h2c_pins({ 2, 2, 1 }, { 0., 0.6, 0. }, { 2, 0, 0 });
        CHECK(pinned_map(cfg, { 1, 1, 0 }) == std::vector<int>{ 2, 4, 0 });
    }
}

TEST_CASE("Rule S: filaments without pins move as before", "[H2CRackGroupId][RackPins][Identity]")
{
    // An unpinned filament the grouping put on a 0.6 goes to the first 0.4 of its extruder, shared or not (as before the rule).
    const DynamicPrintConfig cfg = h2c_pins({ 2, 2, 1 }, { 0. });
    CHECK(pinned_map(cfg, { 2, 4, 0 }) == std::vector<int>{ 1, 1, 0 });
    CHECK(pinned_map(cfg, { 1, 3, 0 }) == std::vector<int>{ 1, 3, 0 });
}

TEST_CASE("Rule P: on an automatic map a size pin is read on the extruder that has the size", "[H2CRackGroupId][RackPins][AutoMap]")
{
    // Fixture h2c3_mix2_auto_pn: saved map 1,2,1, filament 1 pinned to the 0.6, which only the right rack has.
    const DynamicPrintConfig automatic = h2c_pins({ 1, 2, 1 }, { 0.6, 0., 0. }, { 0 }, "Auto For Flush");
    CHECK(rack_pins_follow_grouping(automatic));
    CHECK_THAT(nozzle_diameter_for_filament(automatic, 1), Catch::Matchers::WithinAbs(0.6, 1e-6));
    CHECK(rack_pin_extruder(automatic, 1, 0) == 1);
    CHECK_THAT(nozzle_diameter_for_filament(automatic, 3), Catch::Matchers::WithinAbs(0.4, 1e-6)); // no pin: as before
    CHECK(rack_pin_extruders(automatic, 1) == std::vector<bool>{ false, true });
    CHECK(rack_pin_extruders(automatic, 3).empty());
    PrintConfig print_config;
    print_config.apply(automatic, true);
    CHECK(validate_rack_nozzle_diameters(print_config, { 0, 1, 2 }).empty());

    // A manual map is the user's: unchanged.
    const DynamicPrintConfig manual = h2c_pins({ 1, 2, 1 }, { 0.6, 0., 0. });
    CHECK_FALSE(rack_pins_follow_grouping(manual));
    CHECK(rack_pin_extruder(manual, 1, 0) == 0);
    CHECK_THAT(nozzle_diameter_for_filament(manual, 1), Catch::Matchers::WithinAbs(0.4, 1e-6));
}

TEST_CASE("Rule P: the automatic grouping is barred from extruders that cannot take a pin", "[H2CRackGroupId][RackPins][AutoMap]")
{
    std::vector<std::set<int>> unprintable(2);
    SECTION("a 0.6 pin bars the left (P) and keeps the unpinned filaments off the right (P2)")
    {
        const DynamicPrintConfig cfg = h2c_pins({ 1, 2, 1 }, { 0.6, 0., 0. }, { 0 }, "Auto For Flush");
        CHECK(bar_rack_pin_extruders(cfg, { 0, 1, 2 }, unprintable).empty());
        CHECK(unprintable[0] == std::set<int>{ 0 });
        CHECK(unprintable[1] == std::set<int>{ 1, 2 });
    }
    SECTION("a 0.4 pin bars nothing: both extruders have a 0.4")
    {
        const DynamicPrintConfig cfg = h2c_pins({ 1, 2, 1 }, { 0.4, 0., 0. }, { 0 }, "Auto For Match");
        CHECK(bar_rack_pin_extruders(cfg, { 0, 1, 2 }, unprintable).empty());
        CHECK(unprintable[0].empty());
        CHECK(unprintable[1].empty());
    }
    SECTION("a slot pin bars every extruder but the slot's")
    {
        // Slot 2 of the right rack is a 0.6; the saved map has filament 3 on the left, which has no rack slots.
        const DynamicPrintConfig cfg = h2c_pins({ 1, 2, 1 }, { 0. }, { 0, 0, 2 }, "Auto For Flush");
        CHECK(bar_rack_pin_extruders(cfg, { 0, 1, 2 }, unprintable).empty());
        CHECK(unprintable[0] == std::set<int>{ 2 });
        CHECK(unprintable[1] == std::set<int>{ 0, 1 }); // P2: the right is set to 0.6 by the slot
        CHECK_THAT(nozzle_diameter_for_filament(cfg, 3), Catch::Matchers::WithinAbs(0.6, 1e-6));
        CHECK_THAT(filament_rack_pin(cfg, 3), Catch::Matchers::WithinAbs(0.6, 1e-6));
    }
    SECTION("a manual map bars nothing")
    {
        const DynamicPrintConfig cfg = h2c_pins({ 1, 2, 1 }, { 0.6, 0., 0. });
        CHECK(bar_rack_pin_extruders(cfg, { 0, 1, 2 }, unprintable).empty());
        CHECK(unprintable[0].empty());
    }
    SECTION("a pin no extruder can take is an error, before and during the grouping")
    {
        const DynamicPrintConfig cfg = h2c_pins({ 1, 2, 1 }, { 0.8, 0., 0. }, { 0 }, "Auto For Flush");
        CHECK(bar_rack_pin_extruders(cfg, { 0, 1, 2 }, unprintable) ==
              "Filament 1 is pinned to a 0.8 mm rack nozzle, but no extruder that has one can print it.");
        PrintConfig print_config;
        print_config.apply(cfg, true);
        CHECK(validate_rack_nozzle_diameters(print_config, { 0, 1, 2 }) == "Filament 1 asks for a 0.8 mm rack nozzle, but no extruder has one.");
    }
    SECTION("the only extruder with the size cannot print the filament: an error")
    {
        const DynamicPrintConfig cfg = h2c_pins({ 1, 2, 1 }, { 0.6, 0., 0. }, { 0 }, "Auto For Flush");
        unprintable[1].insert(0);
        CHECK(bar_rack_pin_extruders(cfg, { 0, 1, 2 }, unprintable) ==
              "Filament 1 is pinned to a 0.6 mm rack nozzle, but no extruder that has one can print it.");
    }
}

TEST_CASE("Rule P: the flush grouping with the bar puts a 0.6 pin on the right, and the pass on a 0.6", "[H2CRackGroupId][RackPins][AutoMap]")
{
    const DynamicPrintConfig cfg = h2c_pins({ 1, 2, 1 }, { 0.6, 0., 0. }, { 0 }, "Auto For Flush");
    PrintConfig              print_config;
    print_config.apply(cfg, true);
    const std::vector<NozzleInfo>   list = grouping_nozzle_list(print_config, nozzle_groups(cfg));
    const std::vector<unsigned int> used{ 0, 1, 2 };

    FilamentGroupContext ctx;
    ctx.model_info.flush_matrix.assign(2, FlushMatrix(3, std::vector<float>{ 100.f, 100.f, 100.f }));
    for (FlushMatrix &m : ctx.model_info.flush_matrix)
        for (size_t i = 0; i < 3; ++i)
            m[i][i] = 0.f;
    ctx.model_info.layer_filaments = { { 0, 1, 2 }, { 0, 1, 2 } };
    for (const char *colour : { "#FFFFFF", "#202020", "#C12E1F" }) {
        FilamentGroupUtils::FilamentInfo info;
        info.color      = FilamentGroupUtils::Color(colour);
        info.type       = "PLA";
        info.is_support = false;
        info.usage_type = FilamentUsageType::ModelOnly;
        info.diameter   = 1.75f;
        ctx.model_info.filament_info.push_back(info);
    }
    ctx.model_info.filament_ids = { "GFA00", "GFA00", "GFA00" };
    ctx.model_info.unprintable_filaments.resize(2);
    ctx.group_info.total_filament_num  = 3;
    ctx.group_info.max_gap_threshold   = 0.01;
    ctx.group_info.mode                = FGMode::FlushMode;
    ctx.group_info.strategy            = FGStrategy::BestCost;
    ctx.group_info.ignore_ext_filament = false;
    ctx.group_info.filament_volume_map = std::vector<int>(3, int(nvtHybrid));
    ctx.machine_info.max_group_size    = { 16, 16 };
    ctx.machine_info.machine_filament_info.resize(2);
    ctx.machine_info.prefer_non_model_filament = { false, false };
    ctx.machine_info.master_extruder_id        = 1;
    ctx.speed_info.extruder_change_time        = 0.;
    ctx.speed_info.filament_change_time        = 0.;
    ctx.speed_info.group_with_time             = false;
    ctx.nozzle_info.nozzle_list                = list;
    ctx.nozzle_info.extruder_nozzle_list       = FilamentGroupUtils::build_extruder_nozzle_list(list);

    REQUIRE(bar_rack_pin_extruders(print_config, used, ctx.model_info.unprintable_filaments).empty());
    FilamentGroup     fg(ctx);
    std::vector<int>  map = fg.calc_filament_group();
    REQUIRE(map.size() == 3);
    REQUIRE(map[0] >= 0);
    REQUIRE(size_t(map[0]) < list.size());
    CHECK(list[size_t(map[0])].extruder_id == 1);
    // P2: the unpinned filaments stay off the right rack, so it holds one size.
    CHECK(list[size_t(map[1])].extruder_id == 0);
    CHECK(list[size_t(map[2])].extruder_id == 0);
    pin_rack_nozzle_diameters(print_config, list, used, map);
    CHECK(list[size_t(map[0])].diameter == "0.6");
    const auto group = MultiNozzleUtils::LayeredNozzleGroupResult::create(map, list, used);
    REQUIRE(group);
    CHECK(check_rack_nozzle_diameters(print_config, *group).empty());
}

TEST_CASE("Rule P2: an automatic map keeps other sizes off a rack the pins set to one size", "[H2CRackGroupId][RackPins][AutoMap]")
{
    std::vector<std::set<int>> unprintable(2);
    SECTION("an unpinned filament the left cannot print may still use the right")
    {
        const DynamicPrintConfig cfg = h2c_pins({ 1, 2, 1 }, { 0.6, 0., 0. }, { 0 }, "Auto For Flush");
        unprintable[0].insert(1);
        CHECK(bar_rack_pin_extruders(cfg, { 0, 1, 2 }, unprintable).empty());
        CHECK(unprintable[0] == std::set<int>{ 0, 1 });
        CHECK(unprintable[1] == std::set<int>{ 2 });
    }
    SECTION("forced pins putting two sizes on the right bar nothing more")
    {
        // f1 pinned 0.6 and f2 pinned to slot 1 (a 0.4): both can only go right.
        const DynamicPrintConfig cfg = h2c_pins({ 2, 2, 1 }, { 0.6, 0., 0. }, { 0, 1, 0 }, "Auto For Flush");
        CHECK(bar_rack_pin_extruders(cfg, { 0, 1, 2 }, unprintable).empty());
        CHECK(unprintable[0] == std::set<int>{ 0, 1 });
        CHECK(unprintable[1].empty());
    }
    SECTION("a pin to the extruder's own size bars no unpinned filament")
    {
        const DynamicPrintConfig cfg = h2c_pins({ 2, 2, 1 }, { 0.4, 0., 0. }, { 0 }, "Auto For Flush");
        CHECK(bar_rack_pin_extruders(cfg, { 0, 1, 2 }, unprintable).empty());
        CHECK(unprintable[0].empty());
        CHECK(unprintable[1].empty());
    }
    SECTION("a FREE pin to another size the right also has is kept off it")
    {
        // f1 pinned 0.6 (right only: forced), f2 pinned 0.4 (both extruders: free): the right is set to 0.6.
        const DynamicPrintConfig cfg = h2c_pins({ 2, 2, 1 }, { 0.6, 0.4, 0. }, { 0 }, "Auto For Flush");
        CHECK(bar_rack_pin_extruders(cfg, { 0, 1, 2 }, unprintable).empty());
        CHECK(unprintable[0] == std::set<int>{ 0 });
        CHECK(unprintable[1] == std::set<int>{ 1, 2 });
    }
    SECTION("a manual map is untouched")
    {
        const DynamicPrintConfig cfg = h2c_pins({ 1, 2, 1 }, { 0.6, 0., 0. });
        CHECK(bar_rack_pin_extruders(cfg, { 0, 1, 2 }, unprintable).empty());
        CHECK(unprintable[0].empty());
        CHECK(unprintable[1].empty());
    }
    SECTION("print by object: the plate map with the pin moves")
    {
        const DynamicPrintConfig cfg = h2c_pins({ 1, 1, 1 }, { 0., 0.6, 0. }, { 0 }, "Auto For Flush");
        CHECK(rack_pin_filament_map(cfg, { 0, 1, 2 }, { 1, 1, 1 }) == std::vector<int>{ 1, 2, 1 });
        CHECK(rack_pin_filament_map(cfg, { 0, 1, 2 }, { 2, 1, 2 }) == std::vector<int>{ 1, 2, 1 });
        // An unused filament keeps its entry.
        CHECK(rack_pin_filament_map(cfg, { 0, 1 }, { 2, 1, 2 }) == std::vector<int>{ 1, 2, 2 });
        // A hand-set map and a plate without pins are untouched.
        const DynamicPrintConfig manual = h2c_pins({ 1, 1, 1 }, { 0., 0.6, 0. });
        CHECK(rack_pin_filament_map(manual, { 0, 1, 2 }, { 2, 1, 2 }) == std::vector<int>{ 2, 1, 2 });
        const DynamicPrintConfig none = h2c_pins({ 1, 1, 1 }, { 0. }, { 0 }, "Auto For Flush");
        CHECK(rack_pin_filament_map(none, { 0, 1, 2 }, { 2, 1, 2 }) == std::vector<int>{ 2, 1, 2 });
    }
}

TEST_CASE("R2: on an automatic map the saved filament_map does not change the result", "[H2CRackGroupId][RackPins][AutoMap]")
{
    // Pins 0.6 (forced right) and 0.4 (free); saved maps 1,2,1,2 and 1,1,1,2 must give the same bars and the same moved map.
    std::vector<std::vector<std::set<int>>> bars;
    for (const std::vector<int> &saved : { std::vector<int>{ 1, 2, 1, 2 }, std::vector<int>{ 1, 1, 1, 2 } }) {
        const DynamicPrintConfig cfg = h2c_pins(saved, { 0.6, 0.4, 0., 0. }, { 0 }, "Auto For Flush");
        std::vector<std::set<int>> unprintable(2);
        CHECK(bar_rack_pin_extruders(cfg, { 0, 1, 2, 3 }, unprintable).empty());
        CHECK(rack_pin_filament_map(cfg, { 0, 1, 2, 3 }, saved) == std::vector<int>{ 2, 1, 1, 1 });
        CHECK(rack_pin_extruder(cfg, 2, saved[1] - 1) == 0); // a FREE pin is read on the lowest extruder that can take it
        bars.push_back(unprintable);
    }
    REQUIRE(bars.size() == 2);
    CHECK(bars[0] == bars[1]);
    CHECK(bars[0][0] == std::set<int>{ 0 });
    CHECK(bars[0][1] == std::set<int>{ 1, 2, 3 });
    // A slot pin belongs to the lowest extruder that shows the slot, whatever the saved map says.
    const DynamicPrintConfig slot = h2c_pins({ 2, 1, 1 }, { 0. }, { 0, 0, 2 }, "Auto For Flush");
    CHECK(rack_pin_extruders(slot, 3) == std::vector<bool>{ false, true });
}

TEST_CASE("R3: Manual and Nozzle Manual are set by hand; the Auto modes follow the grouping", "[H2CRackGroupId][RackPins]")
{
    CHECK(hand_set_filament_map(h2c_pins({ 1, 2, 1 }, { 0.6, 0., 0. })));
    CHECK(hand_set_filament_map(h2c_pins({ 1, 2, 1 }, { 0.6, 0., 0. }, { 0 }, "Nozzle Manual")));
    CHECK_FALSE(rack_pins_follow_grouping(h2c_pins({ 1, 2, 1 }, { 0.6, 0., 0. }, { 0 }, "Nozzle Manual")));
    for (const char *mode : { "Auto For Flush", "Auto For Match", "Auto For Quality" }) {
        CAPTURE(mode);
        const DynamicPrintConfig cfg = h2c_pins({ 1, 2, 1 }, { 0.6, 0., 0. }, { 0 }, mode);
        CHECK_FALSE(hand_set_filament_map(cfg));
        CHECK(rack_pins_follow_grouping(cfg));
    }
    // A Nozzle Manual map with a 0.6 pin on the left is not regrouped: it is refused (R4).
    const DynamicPrintConfig nozzle_manual = h2c_pins({ 1, 2, 1 }, { 0.6, 0., 0. }, { 0 }, "Nozzle Manual");
    CHECK(rack_pin_extruder(nozzle_manual, 1, 0) == 0);
    std::vector<std::set<int>> unprintable(2);
    CHECK(bar_rack_pin_extruders(nozzle_manual, { 0, 1, 2 }, unprintable).empty());
    CHECK(unprintable[0].empty());
    CHECK(unprintable[1].empty());
}

TEST_CASE("R4: a pin its hand-set extruder cannot take is refused", "[H2CRackGroupId][RackPins]")
{
    const auto validate = [](const DynamicPrintConfig &cfg) {
        PrintConfig print_config;
        print_config.apply(cfg, true);
        return validate_rack_nozzle_diameters(print_config, { 0, 1, 2 });
    };
    for (const char *mode : { "Manual", "Nozzle Manual" }) {
        CAPTURE(mode);
        // The left has no rack and a 0.4: a 0.6 pin there was sliced at 0.4 with no message.
        const std::string error = validate(h2c_pins({ 1, 2, 1 }, { 0.6, 0., 0. }, { 0 }, mode));
        CHECK_THAT(error, Catch::Matchers::StartsWith("Filament 1 asks for a 0.6 mm rack nozzle, but the "));
        CHECK_THAT(error, Catch::Matchers::EndsWith(" extruder has none."));
        // Its own size is fine.
        CHECK(validate(h2c_pins({ 1, 2, 1 }, { 0.4, 0., 0. }, { 0 }, mode)).empty());
    }
    // An extruder with a rack keeps the existing message; a slot pin the existing slot message.
    CHECK(validate(h2c_pins({ 2, 2, 1 }, { 0.8, 0., 0. })) == "Filament 1 asks for a 0.8 mm rack nozzle, but extruder 2's rack has none.");
    CHECK_THAT(validate(h2c_pins({ 1, 2, 1 }, { 0. }, { 2, 0, 0 })), Catch::Matchers::EndsWith("slot 2, but that rack has 0 nozzles."));
    // On the right with the size: fine.
    CHECK(validate(h2c_pins({ 2, 2, 1 }, { 0.6, 0., 0. })).empty());
}
