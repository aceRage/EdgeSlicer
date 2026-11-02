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
    // Raised base labels sit on z = 1..1.6 between the two pillars, y = 12.5..17.5 (the pillars cover
    // y = 3..7 and 23..27, and their fillet feet reach y = 10 and 20 at the plate).
    for (const auto &v : mesh.its.vertices)
        if (v.z() > stringing_grid_base_height + 0.05f && v.z() < stringing_grid_base_height + stringing_grid_label_height + 0.05f &&
            v.y() > 11.f && v.y() < 19.f)
            return true;
    return false;
}

// Pillar footprints of a column (in column-mesh coordinates), as add_pillar() places them.
BoundingBoxf pillar_footprint(int which)
{
    const double cx   = 0.5 * stringing_grid_base_x;
    const double cy   = 0.5 * (stringing_grid_base_y - stringing_grid_pillar_gap) + which * stringing_grid_pillar_gap;
    const double half = 0.5 * stringing_grid_pillar_xy;
    return BoundingBoxf(Vec2d(cx - half, cy - half), Vec2d(cx + half, cy + half));
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

TEST_CASE("Stringing grid band labels stand on the label tower's front face inside their band", "[Calib][TempRetract]")
{
    // Owner hand test: the digits hung off the back of the tower as horizontal slabs.
    Calib_Params params = grid_params();
    params.print_labels = true;
    const int    bands  = stringing_grid_band_count(params);
    const double band_h = params.band_height;
    const double eps    = 1e-4;
    REQUIRE(bands >= 2);
    for (int band = 0; band < bands; ++band) {
        CAPTURE(band);
        const TriangleMesh label = make_stringing_grid_band_label_mesh(params, band);
        REQUIRE_FALSE(label.empty());
        const BoundingBoxf3 bb = label.bounding_box();
        // Front face is y = 0: the label sticks out by the emboss height and reaches into the tower.
        CHECK(bb.min.y() == Approx(-stringing_grid_label_height).margin(eps));
        CHECK(bb.max.y() >= -eps);
        CHECK(bb.max.y() <= stringing_grid_label_embed + eps);
        // Inside the tower's width, and inside its band (not across a band boundary).
        CHECK(bb.min.x() >= -eps);
        CHECK(bb.max.x() <= stringing_grid_label_tower_x + eps);
        const double band_lo = stringing_grid_base_height + band * band_h;
        CHECK(bb.min.z() >= band_lo - eps);
        CHECK(bb.max.z() <= band_lo + band_h + eps);
        // Upright: as tall as a digit along Z, as thick as the emboss along Y.
        CHECK(bb.size().z() == Approx(stringing_grid_label_glyph_height).margin(eps));
    }
    // The tower with labels: nothing behind or beside the tower body, labels only in front of it.
    const BoundingBoxf3 tower = make_stringing_grid_label_tower_mesh(params).bounding_box();
    CHECK(tower.max.y() == Approx(stringing_grid_label_tower_y).margin(eps));
    CHECK(tower.min.y() == Approx(-stringing_grid_label_height).margin(eps));
    CHECK(tower.min.x() == Approx(0.).margin(eps));
    CHECK(tower.max.x() == Approx(stringing_grid_label_tower_x).margin(eps));
    // The widest temperature label fits the tower with room to spare.
    CHECK(3 * stringing_grid_label_glyph_width + 2 * 0.4 < stringing_grid_label_tower_x);
}

TEST_CASE("Stringing grid base labels sit between the pillars, clear of both footprints", "[Calib][TempRetract]")
{
    // Owner hand test: the base labels overlapped the front pillar and sliced to stray fragments.
    Calib_Params params = grid_params();
    params.print_labels    = true;
    params.retract_lengths = {0.2, 1.6, 9.8, 10.};
    const double eps    = 1e-4;
    const double margin = 1.0;
    CHECK(stringing_grid_label_stroke >= 0.9); // about two 0.45 mm lines
    CHECK(stringing_grid_label_height >= 0.6 - eps); // three layers at 0.2 mm
    CHECK(stringing_grid_retract_label(0.2) == "0.2");
    CHECK(stringing_grid_retract_label(9.8) == "9.8");
    CHECK(stringing_grid_retract_label(10.) == "10");
    for (int col = 0; col < int(params.retract_lengths.size()); ++col) {
        CAPTURE(col);
        const TriangleMesh label = make_stringing_grid_column_label_mesh(params, col);
        REQUIRE_FALSE(label.empty());
        const BoundingBoxf3 bb = label.bounding_box();
        // On the base top: reaches into the plate and rises the emboss height above it.
        CHECK(bb.min.z() == Approx(stringing_grid_base_height - stringing_grid_label_embed).margin(eps));
        CHECK(bb.max.z() == Approx(stringing_grid_base_height + stringing_grid_label_height).margin(eps));
        // Inside the plate.
        CHECK(bb.min.x() >= -eps);
        CHECK(bb.max.x() <= stringing_grid_base_x + eps);
        // Clear of both pillars by at least `margin`.
        for (int p = 0; p < 2; ++p) {
            const BoundingBoxf fp = pillar_footprint(p);
            const bool clear = bb.max.y() + margin <= fp.min.y() || bb.min.y() - margin >= fp.max.y();
            CHECK(clear);
            // ...and of the fillet foot around it (default radius, the largest that fits).
            const double r          = stringing_grid_fillet_radius(params);
            const bool   clear_foot = bb.max.y() + margin <= fp.min.y() - r || bb.min.y() - margin >= fp.max.y() + r;
            CHECK(r > 0.);
            CHECK(clear_foot);
        }
    }
}

TEST_CASE("Stringing grid pillars get a watertight fillet foot on the base plate", "[Calib][TempRetract]")
{
    // Owner (real print): the towers snapped where they meet the plate.
    const double eps = 1e-4;
    Calib_Params params = grid_params();
    CHECK(params.fillet_base);                                          // default on
    CHECK(stringing_grid_max_fillet_radius() == Approx(3.).margin(eps)); // 3 mm to the plate's edges
    CHECK(stringing_grid_fillet_radius(params) == Approx(3.).margin(eps));
    params.fillet_radius = 5.;
    CHECK(stringing_grid_fillet_radius(params) == Approx(3.).margin(eps)); // clamped: the foot stays on the plate
    params.fillet_radius = 1.5;
    CHECK(stringing_grid_fillet_radius(params) == Approx(1.5).margin(eps));
    params.fillet_base = false;
    CHECK(stringing_grid_fillet_radius(params) == 0.);

    const double       r      = 3.;
    const double       cx     = 0.5 * stringing_grid_base_x;
    const double       cy     = 0.5 * (stringing_grid_base_y - stringing_grid_pillar_gap);
    TriangleMesh       fillet = make_stringing_grid_pillar_fillet_mesh(cx, cy, r); // volume() is not const
    REQUIRE_FALSE(fillet.empty());
    // Watertight, outward facing.
    CHECK(its_num_open_edges(fillet.its) == 0);
    CHECK(fillet.volume() > 0.);
    // From inside the plate up to radius above it; pillar plus radius wide at the plate, pillar wide at the top.
    const BoundingBoxf3 bb = fillet.bounding_box();
    CHECK(bb.min.z() == Approx(stringing_grid_base_height - stringing_grid_label_embed).margin(eps));
    CHECK(bb.max.z() == Approx(stringing_grid_base_height + r).margin(eps));
    CHECK(bb.size().x() > stringing_grid_pillar_xy + 2. * r - 0.2);
    CHECK(bb.min.x() >= -eps);
    CHECK(bb.max.x() <= stringing_grid_base_x + eps);
    CHECK(bb.min.y() >= -eps);
    // Concave: half way up, the foot is much narrower than at the plate but still wider than the pillar.
    double half_way_max_x = 0.;
    for (const auto &v : fillet.its.vertices)
        if (std::abs(v.z() - (stringing_grid_base_height + 0.5 * r)) < 1e-3)
            half_way_max_x = std::max(half_way_max_x, double(v.x()) - cx);
    CHECK(half_way_max_x > 0.5 * stringing_grid_pillar_xy);
    CHECK(half_way_max_x < 0.5 * stringing_grid_pillar_xy + 0.5 * r);
    CHECK(make_stringing_grid_pillar_fillet_mesh(cx, cy, 0.).empty());

    // The column mesh carries both feet only when the option is on, and stays on its plate.
    Calib_Params on = grid_params(), off = grid_params();
    off.fillet_base = false;
    const TriangleMesh col_on  = make_stringing_grid_column_mesh(on, 0);
    const TriangleMesh col_off = make_stringing_grid_column_mesh(off, 0);
    CHECK(col_on.its.indices.size() == col_off.its.indices.size() + 2 * fillet.its.indices.size());
    CHECK(col_on.bounding_box().min.x() == Approx(col_off.bounding_box().min.x()).margin(eps));
    CHECK(col_on.bounding_box().max.x() == Approx(col_off.bounding_box().max.x()).margin(eps));
    CHECK(col_on.bounding_box().min.y() == Approx(col_off.bounding_box().min.y()).margin(eps));
    CHECK(col_on.bounding_box().max.y() == Approx(col_off.bounding_box().max.y()).margin(eps));
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
