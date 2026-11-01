// Temperature × retraction stringing grid: column / band / temperature schedule and geometry.
#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <string>

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/calib.hpp"

using namespace Slic3r;

namespace {

Calib_Params grid_params()
{
    Calib_Params p;
    p.mode        = CalibMode::Calib_Temp_Retract_Grid;
    p.start       = 230.;
    p.end         = 205.;
    p.step        = 5.;
    p.band_height = 8.;
    p.retract_start = 0.2;
    p.retract_end   = 1.6;
    p.retract_step  = 0.2;
    p.wait_for_temperature = true;
    p.print_labels         = false;
    return p;
}

bool has_label_vertices(const TriangleMesh &mesh)
{
    // Raised base labels sit on z = 1..1.6 at the front edge (y < 2.5). The pillars start at y = 3.
    for (const auto &v : mesh.its.vertices)
        if (v.z() > stringing_grid_base_height + 0.05f && v.z() < stringing_grid_base_height + stringing_grid_label_height + 0.05f &&
            v.y() < 2.5f)
            return true;
    return false;
}

} // namespace

TEST_CASE("Stringing grid temperature bands sit between Z boundaries and clamp at the end", "[Calib][TempRetract]")
{
    const Calib_Params params = grid_params();
    CHECK(stringing_grid_band_count(params) == 6);
    CHECK(temp_retract_band_index(params, 0.2) == 0);
    CHECK(temp_retract_band_index(params, stringing_grid_base_height) == 0);
    CHECK(temp_for_z(params, stringing_grid_base_height) == Approx(230.));

    const double boundary = stringing_grid_base_height + params.band_height;
    CHECK(temp_retract_band_index(params, boundary - 1e-6) == 0);
    CHECK(temp_for_z(params, boundary - 1e-6) == Approx(230.));
    CHECK(temp_retract_band_index(params, boundary) == 1);
    CHECK(temp_for_z(params, boundary) == Approx(225.));
    CHECK(temp_retract_band_index(params, boundary + 1e-6) == 1);

    const double last_boundary = stringing_grid_base_height + 5. * params.band_height;
    CHECK(temp_retract_band_index(params, last_boundary) == 5);
    CHECK(temp_for_z(params, last_boundary) == Approx(205.));
    CHECK(temp_retract_band_index(params, 200.) == 5);
    CHECK(temp_for_z(params, 200.) == Approx(205.));
}

TEST_CASE("Stringing grid retraction follows the column schedule", "[Calib][TempRetract]")
{
    Calib_Params params = grid_params();
    const std::vector<double> lens = stringing_grid_retract_schedule(params);
    REQUIRE(lens.size() == 8);
    CHECK(lens.front() == Approx(0.2));
    CHECK(lens.back() == Approx(1.6));
    CHECK(retract_for_column(params, 0) == Approx(0.2));
    CHECK(retract_for_column(params, 3) == Approx(0.8));
    CHECK(retract_for_column(params, 7) == Approx(1.6));
    CHECK(retract_for_column(params, 99) == Approx(1.6));

    params.retract_lengths = {0.2, 0.4, 0.6};
    CHECK(stringing_grid_retract_schedule(params).size() == 3);
    CHECK(retract_for_column(params, 1) == Approx(0.4));
    CHECK(stringing_grid_column_from_name("SG_col_03") == 3);
    CHECK(stringing_grid_column_from_name(stringing_grid_column_name(2)) == 2);
    CHECK(stringing_grid_column_from_name("SG_label") == -1);
    CHECK(stringing_grid_column_from_name("cube") == -1);
}

TEST_CASE("Stringing grid column count is limited by bed width", "[Calib][TempRetract]")
{
    CHECK(stringing_grid_max_columns(270.) == 17);
    CHECK(stringing_grid_columns_fit(17, 270.));
    CHECK_FALSE(stringing_grid_columns_fit(18, 270.));
    CHECK_FALSE(stringing_grid_columns_fit(1, 270.));
}

TEST_CASE("Stringing grid geometry is one pillar pair per column", "[Calib][TempRetract]")
{
    Calib_Params params = grid_params();
    params.retract_lengths = {0.2, 0.4, 0.6};
    params.band_count      = 2;
    params.print_labels    = false;

    const TriangleMesh mesh = make_stringing_grid_column_mesh(params, 0);
    const BoundingBoxf3 bb  = mesh.bounding_box();
    CHECK(bb.size().x() == Approx(stringing_grid_base_x));
    CHECK(bb.size().y() == Approx(stringing_grid_base_y));
    CHECK(bb.size().z() == Approx(stringing_grid_base_height + 2. * params.band_height));
    CHECK_FALSE(has_label_vertices(mesh));

    double pillar_min_y = 1e9, pillar_max_y = -1e9;
    for (const auto &v : mesh.its.vertices) {
        if (v.z() > stringing_grid_base_height + 1.f) {
            pillar_min_y = std::min(pillar_min_y, double(v.y()));
            pillar_max_y = std::max(pillar_max_y, double(v.y()));
        }
    }
    // 4 mm pillars, 20 mm centre-to-centre: outer faces are 24 mm apart.
    CHECK(pillar_max_y - pillar_min_y == Approx(stringing_grid_pillar_gap + stringing_grid_pillar_xy));

    Model model;
    make_stringing_grid(model, params);
    REQUIRE(model.objects.size() == 3);
    for (int col = 0; col < 3; ++col) {
        REQUIRE(model.objects[size_t(col)]->name == stringing_grid_column_name(col));
        REQUIRE_FALSE(model.objects[size_t(col)]->instances.empty());
        CHECK(model.objects[size_t(col)]->instances.front()->get_offset().x() == Approx(col * stringing_grid_column_pitch));
    }
}

TEST_CASE("Stringing grid raised labels are optional and default off", "[Calib][TempRetract]")
{
    Calib_Params params = grid_params();
    params.retract_lengths = {0.2, 0.4};
    params.band_count      = 2;

    CHECK_FALSE(params.print_labels);
    {
        Model model;
        make_stringing_grid(model, params);
        CHECK(model.objects.size() == 2);
        for (const ModelObject *obj : model.objects)
            CHECK(obj->name != stringing_grid_label_name);
        CHECK_FALSE(has_label_vertices(make_stringing_grid_column_mesh(params, 0)));
    }

    params.print_labels = true;
    {
        Calib_Params plain_params = params;
        plain_params.print_labels = false;
        const TriangleMesh labelled = make_stringing_grid_column_mesh(params, 0);
        const TriangleMesh plain    = make_stringing_grid_column_mesh(plain_params, 0);
        CHECK(has_label_vertices(labelled));
        CHECK(labelled.its.vertices.size() > plain.its.vertices.size());

        Model model;
        make_stringing_grid(model, params);
        REQUIRE(model.objects.size() == 3);
        CHECK(model.objects.back()->name == stringing_grid_label_name);
        CHECK(model.objects.back()->instances.front()->get_offset().x() == Approx(-stringing_grid_column_pitch));
    }
}

TEST_CASE("Stringing grid column names parse strictly", "[Calib][TempRetract]")
{
    // Review F5: digits only, to the end of the name.
    CHECK(stringing_grid_column_from_name("SG_col_00") == 0);
    CHECK(stringing_grid_column_from_name("SG_col_7") == 7);
    CHECK(stringing_grid_column_from_name("SG_col_16") == 16);
    CHECK(stringing_grid_column_from_name("SG_col_3abc") == -1);
    CHECK(stringing_grid_column_from_name("SG_col_") == -1);
    CHECK(stringing_grid_column_from_name("SG_col_-1") == -1);
    CHECK(stringing_grid_column_from_name("SG_col_+2") == -1);
    CHECK(stringing_grid_column_from_name("SG_col_ 3") == -1);
    CHECK(stringing_grid_column_from_name("SG_col_3 ") == -1);
    CHECK(stringing_grid_column_from_name("xSG_col_3") == -1);
}

TEST_CASE("Stringing grid band Z is relative to the first printed layer", "[Calib][TempRetract]")
{
    // F11: raft and z_offset shift absolute print_z. Band 0 still starts at the object's
    // first-layer height (1 mm base), measured from the first printed layer bottom.
    const Calib_Params params = grid_params();
    CHECK(temp_retract_relative_z(0.4, 0.) == Approx(0.4));
    CHECK(temp_retract_relative_z(1.9, 1.5) == Approx(0.4));
    CHECK(temp_retract_relative_z(0.8, 0.4) == Approx(0.4));

    const double raft_bottom = 1.5;
    CHECK(temp_retract_band_index(params, temp_retract_relative_z(raft_bottom + 0.3, raft_bottom)) == 0);
    CHECK(temp_retract_band_index(params, temp_retract_relative_z(raft_bottom + 9.0, raft_bottom)) == 1);
    CHECK(temp_for_z(params, temp_retract_relative_z(raft_bottom + 9.0, raft_bottom)) == Approx(225.));

    const double z_off_bottom = 0.4;
    CHECK(temp_retract_band_index(params, temp_retract_relative_z(z_off_bottom + 0.3, z_off_bottom)) == 0);
    CHECK(temp_retract_band_index(params, temp_retract_relative_z(z_off_bottom + 9.0, z_off_bottom)) == 1);
}

TEST_CASE("Stringing grid dialog defaults validate and 18 columns do not fit", "[Calib][TempRetract]")
{
    Calib_Params params = grid_params();
    CHECK(stringing_grid_column_count(params) == 8);
    CHECK(stringing_grid_params_valid(params, 270.));
    CHECK(calib_temp_retract_grid_mode_name == std::string("temp_retract_grid_calib_mode"));

    params.retract_end  = 3.6;
    params.retract_step = 0.2;
    CHECK(stringing_grid_column_count(params) == 18);
    std::string err;
    CHECK_FALSE(stringing_grid_params_valid(params, 270., &err));
    CHECK(err.find("max") != std::string::npos);
}

TEST_CASE("Stringing grid slicer config disables raft and the prime tower", "[Calib][TempRetract]")
{
    DynamicPrintConfig print    = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig filament = print;
    DynamicPrintConfig printer  = print;
    apply_stringing_grid_slicer_config(print, filament, printer, 0);
    CHECK(print.opt_int("raft_layers") == 0);
    CHECK_FALSE(print.opt_bool("enable_prime_tower"));
    CHECK(print.opt_enum<PrintSequence>("print_sequence") == PrintSequence::ByLayer);
    CHECK_FALSE(printer.opt_bool("use_firmware_retraction"));
}

TEST_CASE("Stringing grid band index is stable at the layer middle", "[Calib][TempRetract]")
{
    // Review F4/F6: the G-code asks for the band at the middle of a layer and the labels at the middle of
    // a band, so a layer top at 8.9999 or 9.0 (band boundary 9.0) maps the same way.
    Calib_Params p;
    p.mode        = CalibMode::Calib_Temp_Retract_Grid;
    p.start       = 230.;
    p.end         = 225.;
    p.step        = 5.;
    p.band_height = 8.;
    p.band_count  = 2;
    CHECK(Calib_Params().mode == CalibMode::Calib_None); // F8
    const double h = 0.2;
    CHECK(temp_retract_band_index(p, 0.5 * (8.9999 + 8.9999 - h)) == 0);
    CHECK(temp_retract_band_index(p, 0.5 * (9.0 + 9.0 - h)) == 0);
    CHECK(temp_retract_band_index(p, 0.5 * (9.2 + 9.0)) == 1);
    CHECK(temp_for_z(p, stringing_grid_base_height + 0.5 * p.band_height) == Approx(230.));
    CHECK(temp_for_z(p, stringing_grid_base_height + 1.5 * p.band_height) == Approx(225.));
}

TEST_CASE("Stringing grid defaults build outward from the other-layers temperature", "[Calib][TempRetract]")
{
    // Lance 2026-11-01: center +5, -5, +10, -10, ... within the filament range, at most 10 bands.
    struct Case { double center, low, high, start, end; int bands; };
    const Case cases[] = {
        {220., 190., 240., 240., 195., 10}, // 220,225,215,...,240,200 then 195 (245 is out of range)
        {220., 210., 230., 230., 210., 5},  // both sides reach the range limits
        {235., 190., 240., 240., 195., 10}, // high side stops at 240, low side fills
        {215., 190., 240., 240., 195., 10}, // 240/190 PLA profile, nozzle 215
    };
    for (const Case &c : cases) {
        CAPTURE(c.center, c.low, c.high);
        Calib_Params p;
        stringing_grid_default_params(p, c.center, c.low, c.high, 5.);
        CHECK(p.start == Approx(c.start));
        CHECK(p.end == Approx(c.end));
        CHECK(p.step == Approx(5.));
        CHECK(stringing_grid_band_count(p) == c.bands);
        CHECK(std::fmod(p.start - p.end, p.step) == Approx(0.).margin(1e-9));
        CHECK(p.mode == CalibMode::Calib_Temp_Retract_Grid);
        CHECK_FALSE(p.print_labels);
        CHECK(p.wait_for_temperature);
        std::string err;
        CHECK(stringing_grid_params_valid(p, 270., &err));
        CHECK(err.empty());
    }
}

TEST_CASE("Stringing grid defaults fall back to 230/205 on invalid filament temperatures", "[Calib][TempRetract]")
{
    struct Case { double center, low, high; };
    const Case cases[] = {
        {0., 190., 240.},   // missing nozzle temperature
        {220., 0., 0.},     // missing range
        {220., 240., 190.}, // low > high
        {250., 190., 240.}, // center above the range
        {180., 190., 240.}, // center below the range
    };
    for (const Case &c : cases) {
        CAPTURE(c.center, c.low, c.high);
        Calib_Params p;
        stringing_grid_default_params(p, c.center, c.low, c.high, 5.);
        CHECK(p.start == Approx(230.));
        CHECK(p.end == Approx(205.));
        CHECK(stringing_grid_params_valid(p, 270.));
    }
    // The raw 240/190 range is still over the cap when typed in by hand.
    Calib_Params ranged;
    stringing_grid_default_params(ranged, 220., 190., 240., 5.);
    ranged.start = 240.;
    ranged.end   = 190.;
    CHECK_FALSE(stringing_grid_params_valid(ranged, 270.));
}

TEST_CASE("Stringing grid rejects a temperature range that is not a whole number of steps", "[Calib][TempRetract]")
{
    // F5: temp_for_z must never land between steps.
    Calib_Params p = grid_params();
    p.end          = 207.;
    std::string err;
    CHECK_FALSE(stringing_grid_params_valid(p, 270., &err));
    CHECK(err.find("multiple") != std::string::npos);
    p.end = 205.;
    CHECK(stringing_grid_params_valid(p, 270.));
    for (int b = 0; b < stringing_grid_band_count(p); ++b) {
        const double t = temp_for_z(p, stringing_grid_base_height + (b + 0.5) * p.band_height);
        CHECK(std::fmod(p.start - t, p.step) == Approx(0.).margin(1e-9));
    }
}

TEST_CASE("Stringing grid slicer config neutralises filament retraction overrides", "[Calib][TempRetract]")
{
    // F6
    DynamicPrintConfig print    = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig filament = print;
    DynamicPrintConfig printer  = print;
    filament.set_deserialize_strict({{"filament_z_hop", "0.6"},
                                     {"filament_retract_when_changing_layer", "0"},
                                     {"filament_retraction_minimum_travel", "5"}});
    apply_stringing_grid_slicer_config(print, filament, printer, 0);
    const auto *zh = filament.option<ConfigOptionFloatsNullable>("filament_z_hop");
    const auto *rl = filament.option<ConfigOptionBoolsNullable>("filament_retract_when_changing_layer");
    const auto *mt = filament.option<ConfigOptionFloatsNullable>("filament_retraction_minimum_travel");
    REQUIRE(zh != nullptr);
    REQUIRE(rl != nullptr);
    REQUIRE(mt != nullptr);
    CHECK(zh->get_at(0) == Approx(0.));
    CHECK(rl->get_at(0));
    CHECK(mt->get_at(0) == Approx(1.));
}
