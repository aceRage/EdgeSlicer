// Temperature × retraction stringing grid: column / band / temperature schedule and geometry.
#include <catch2/catch.hpp>

#include <algorithm>

#include "libslic3r/Model.hpp"
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
