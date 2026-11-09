// Support interface settings (owner request 2026-11-09):
//   1. support_interface_top_layers is a plain number (0-20) rather than a 0-3 drop-down, and
//      support_interface_bottom_layers keeps "Same as top" (-1) with a 0-10 list and a 20 maximum.
//      Counts above the old 3 have to survive a project 3MF and an ini round trip.

#include <catch2/catch.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <string>

using namespace Slic3r;

TEST_CASE("Top interface layers is a plain number from 0 to 20", "[Config][SupportInterface]")
{
    const ConfigOptionDef *def = print_config_def.get("support_interface_top_layers");
    REQUIRE(def != nullptr);
    CHECK(def->type == coInt);
    // No drop-down: the Tab builds a SpinCtrl for a coInt without an enum GUI type.
    CHECK(def->gui_type == ConfigOptionDef::GUIType::undefined);
    CHECK(def->enum_values.empty());
    CHECK(def->enum_labels.empty());
    CHECK(def->min == 0);
    CHECK(def->max == 20);
    CHECK(def->default_value->getInt() == 3);

    for (int v : { 0, 1, 2, 3, 4, 5, 12, 20 })
        CHECK(def->is_value_valid(v));
    CHECK_FALSE(def->is_value_valid(-1));
    CHECK_FALSE(def->is_value_valid(21));
}

TEST_CASE("Bottom interface layers keeps Same as top and lists 0 to 10", "[Config][SupportInterface]")
{
    const ConfigOptionDef *def = print_config_def.get("support_interface_bottom_layers");
    REQUIRE(def != nullptr);
    CHECK(def->type == coInt);
    CHECK(def->gui_type == ConfigOptionDef::GUIType::i_enum_open);
    CHECK(def->min == -1);
    CHECK(def->max == 20);
    CHECK(def->default_value->getInt() == 0);
    REQUIRE(def->enum_values.size() == 12);
    REQUIRE(def->enum_labels.size() == 12);
    CHECK(def->enum_values.front() == "-1");
    CHECK(def->enum_labels.front() == "Same as top");
    for (int i = 0; i <= 10; ++ i) {
        CHECK(def->enum_values[size_t(i) + 1] == std::to_string(i));
        CHECK(def->enum_labels[size_t(i) + 1] == std::to_string(i));
    }
    for (int v : { -1, 0, 3, 10, 15, 20 })
        CHECK(def->is_value_valid(v));
    CHECK_FALSE(def->is_value_valid(-2));
    CHECK_FALSE(def->is_value_valid(21));
}

TEST_CASE("Interface layer counts of the old list and above it load and validate", "[Config][SupportInterface]")
{
    // The values the shipped profiles use (0-5 and -1) and counts past the old 0-3 list.
    for (const char *top : { "0", "1", "2", "3", "4", "5", "8", "20" })
        for (const char *bottom : { "-1", "0", "2", "4", "7", "20" }) {
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
            config.set_deserialize("support_interface_top_layers", top, ctxt);
            config.set_deserialize("support_interface_bottom_layers", bottom, ctxt);
            CHECK(ctxt.substitutions.empty());
            CHECK(config.opt_int("support_interface_top_layers") == std::stoi(top));
            CHECK(config.opt_int("support_interface_bottom_layers") == std::stoi(bottom));
            const std::map<std::string, std::string> errors = config.validate(true);
            CHECK(errors.find("support_interface_top_layers") == errors.end());
            CHECK(errors.find("support_interface_bottom_layers") == errors.end());
        }
}

TEST_CASE("Interface layer counts above the old maximum survive an ini round trip", "[Config][SupportInterface]")
{
    DynamicPrintConfig src = DynamicPrintConfig::full_print_config();
    src.set_key_value("support_interface_top_layers", new ConfigOptionInt(6));
    src.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(5));

    const std::string ini = "support_interface_top_layers = " + src.opt_serialize("support_interface_top_layers") + "\n" +
                            "support_interface_bottom_layers = " + src.opt_serialize("support_interface_bottom_layers") + "\n";
    DynamicPrintConfig dst = DynamicPrintConfig::full_print_config();
    dst.load_from_ini_string(ini, ForwardCompatibilitySubstitutionRule::Disable);
    CHECK(dst.opt_int("support_interface_top_layers") == 6);
    CHECK(dst.opt_int("support_interface_bottom_layers") == 5);
}

TEST_CASE("Interface layer counts above the old maximum survive a project 3MF round trip", "[3mf][Config][SupportInterface]")
{
    Model src_model;
    ModelObject *src_object = src_model.add_object();
    src_object->name = "cube";
    src_object->add_volume(make_cube(10., 10., 10.))->name = "cube";
    src_object->add_instance();
    src_object->ensure_on_bed();
    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    store_config.set_key_value("support_interface_top_layers", new ConfigOptionInt(7));
    store_config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(-1));
    // A per-object override above the old list as well.
    src_object->config.set_key_value("support_interface_top_layers", new ConfigOptionInt(5));
    src_object->config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(9));

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string test_file = (tmp_root / "support_interface_layers_project.3mf").string();

    StoreParams store_params;
    store_params.path     = test_file.c_str();
    store_params.model    = &src_model;
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    const bool stored = store_bbs_3mf(store_params);

    Model                     dst_model;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset*>      project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool loaded = stored && load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &plate_data, &project_presets,
                                               &is_bbl_3mf, &file_version, nullptr,
                                               LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                                                   LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    boost::filesystem::remove(test_file);

    REQUIRE(stored);
    REQUIRE(loaded);
    CHECK(dst_config.opt_int("support_interface_top_layers") == 7);
    CHECK(dst_config.opt_int("support_interface_bottom_layers") == -1);
    REQUIRE(dst_model.objects.size() == 1);
    const ModelConfigObject &obj_config = dst_model.objects.front()->config;
    REQUIRE(obj_config.has("support_interface_top_layers"));
    REQUIRE(obj_config.has("support_interface_bottom_layers"));
    CHECK(obj_config.opt_int("support_interface_top_layers") == 5);
    CHECK(obj_config.opt_int("support_interface_bottom_layers") == 9);
}
