#include <catch2/catch.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <string>
#include <vector>

using namespace Slic3r;

// A Bambu Studio 2.9.1 H3C project (tests/h3c_cube_bs.gcode.3mf from the owner's hand test, with the
// plate G-code, the thumbnails and the account id removed). Its project settings carry the 2.9.1
// circle compensation, support ironing and prime tower keys this fork used to drop on load.

namespace {

const std::string H3C_BS291 = (boost::filesystem::path(TEST_DATA_DIR) / "bambu_compat_3mf" / "h3c_cube_bs291.3mf").string();

DynamicPrintConfig load_project_config()
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() / ("snorca_tests_bs29keys_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(root);
    Slic3r::set_temporary_dir(root.string());

    Model                     model;
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool ok = load_bbs_3mf(H3C_BS291.c_str(), &config, &ctxt, &model, &plate_data, &project_presets, &is_bbl_3mf, &file_version, nullptr,
                                 LoadStrategy::LoadConfig | LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    REQUIRE(ok);
    CHECK(is_bbl_3mf);
    return config;
}

std::vector<double> floats_of(const DynamicPrintConfig &cfg, const char *key)
{
    const auto *opt = cfg.option<ConfigOptionFloats>(key);
    REQUIRE(opt != nullptr);
    return opt->values;
}

} // namespace

TEST_CASE("A Bambu Studio 2.9.1 project keeps its circle compensation, support ironing and prime tower keys", "[BS29Keys][BambuCompat][3mf]")
{
    const DynamicPrintConfig config = load_project_config();

    SECTION("auto circle compensation")
    {
        REQUIRE(config.has("enable_circle_compensation"));
        CHECK_FALSE(config.opt_bool("enable_circle_compensation"));
        CHECK(config.opt_bool("apply_scarf_seam_on_circles"));
        CHECK(config.opt_float("circle_compensation_manual_offset") == Approx(0.));
        // Four filaments, Bambu PLA Basic / PETG Translucent @BBL H3C coefficients.
        CHECK(floats_of(config, "circle_compensation_speed") == std::vector<double>{ 200, 200, 200, 200 });
        CHECK(floats_of(config, "counter_coef_2") == std::vector<double>{ 0.008, 0.008, 0.008, 0.008 });
        CHECK(floats_of(config, "counter_coef_3") == std::vector<double>{ -0.041, -0.041, -0.041, -0.041 });
        CHECK(floats_of(config, "hole_coef_2") == std::vector<double>{ -0.008, -0.008, -0.008, -0.008 });
        CHECK(floats_of(config, "hole_coef_3") == std::vector<double>{ 0.23415, 0.23415, 0.23415, 0.23415 });
        CHECK(floats_of(config, "counter_limit_min") == std::vector<double>{ -0.035, -0.035, -0.035, -0.035 });
        CHECK(floats_of(config, "counter_limit_max") == std::vector<double>{ 0.033, 0.033, 0.033, 0.033 });
        CHECK(floats_of(config, "hole_limit_min") == std::vector<double>{ 0.088, 0.088, 0.088, 0.088 });
        CHECK(floats_of(config, "hole_limit_max") == std::vector<double>{ 0.22, 0.22, 0.22, 0.22 });
        CHECK(floats_of(config, "diameter_limit") == std::vector<double>{ 50, 50, 50, 50 });
        CHECK(floats_of(config, "counter_coef_1") == std::vector<double>{ 0, 0, 0, 0 });
        CHECK(floats_of(config, "hole_coef_1") == std::vector<double>{ 0, 0, 0, 0 });
    }

    SECTION("support ironing")
    {
        // enable_support_ironing is Bambu's name for support_ironing (BambuKeyAliases).
        CHECK_FALSE(config.opt_bool("support_ironing"));
        CHECK(config.opt_float("support_ironing_speed") == Approx(30.));
        CHECK(config.opt_float("support_ironing_inset") == Approx(0.));
        CHECK(config.opt_float("support_ironing_direction") == Approx(0.));
        CHECK(config.opt_float("support_ironing_spacing") == Approx(0.15));
    }

    SECTION("prime tower")
    {
        CHECK(config.opt_float("prime_tower_acceleration") == Approx(20000.));
        CHECK(config.opt_float("prime_tower_line_width") == Approx(0.8));
        CHECK(config.opt_float("prime_tower_wipe_speed") == Approx(0.));
        CHECK(config.opt_float("prime_tower_wipe_distance") == Approx(0.));
        // prime_tower_max_speed is Bambu's name for wipe_tower_max_purge_speed (BambuKeyAliases).
        CHECK(config.opt_float("wipe_tower_max_purge_speed") == Approx(90.));
    }
}

TEST_CASE("Bambu Studio 2.9.1 prime tower keys without a Type 1 tower counterpart stay unported", "[BS29Keys][BambuCompat]")
{
    // These drive parts of Bambu's newer tower generator (plan_tower_new / toolchange_wipe_new and
    // the hotend-as-extruder path) that this fork's Type 1 tower does not have. Declaring them
    // would only add settings that do nothing; see the PR for each one's reason.
    for (const char *key : { "prime_tower_enable_framework", "prime_tower_infill_gap", "prime_tower_ramming_line_width",
                             "prime_tower_feedrate_factor", "prime_tower_flat_ironing", "prime_tower_line_ironing",
                             "prime_tower_ironing_speed", "prime_tower_lift_height", "prime_tower_lift_speed",
                             "prime_tower_side_scrape", "prime_tower_side_scrape_face", "prime_tower_side_scrape_length",
                             "prime_tower_side_scrape_height", "prime_tower_side_scrape_speed", "prime_tower_side_scrape_wall_gap",
                             "prime_tower_side_scrape_point_spacing", "prime_tower_side_scrape_along_length",
                             "prime_tower_tail_slowdown_volume", "prime_tower_tail_speed_factor" }) {
        INFO(key);
        CHECK_FALSE(print_config_def.has(key));
    }
    // The ported ones are known to the loader.
    for (const char *key : { "enable_circle_compensation", "circle_compensation_manual_offset", "apply_scarf_seam_on_circles",
                             "circle_compensation_speed", "counter_coef_1", "counter_coef_2", "counter_coef_3", "hole_coef_1",
                             "hole_coef_2", "hole_coef_3", "counter_limit_min", "counter_limit_max", "hole_limit_min",
                             "hole_limit_max", "diameter_limit", "support_ironing_inset", "support_ironing_speed",
                             "support_ironing_direction", "prime_tower_acceleration", "prime_tower_line_width",
                             "prime_tower_wipe_speed", "prime_tower_wipe_distance" }) {
        INFO(key);
        CHECK(print_config_def.has(key));
    }
}

TEST_CASE("The new keys default to the behaviour before they existed", "[BS29Keys]")
{
    const DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    CHECK_FALSE(cfg.opt_bool("enable_circle_compensation"));
    CHECK(cfg.opt_float("circle_compensation_manual_offset") == 0.);
    CHECK(cfg.opt_float("support_ironing_inset") == 0.);
    CHECK(cfg.opt_float("support_ironing_speed") == 0.);
    CHECK(cfg.opt_float("support_ironing_direction") == 0.);
    CHECK(cfg.opt_float("prime_tower_acceleration") == 0.);
    CHECK(cfg.opt_float("prime_tower_line_width") == 0.);
    CHECK(cfg.opt_float("prime_tower_wipe_speed") == 0.);
    CHECK(cfg.opt_float("prime_tower_wipe_distance") == 0.);
}
