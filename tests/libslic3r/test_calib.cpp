// Ironing flow calibration mesh and mark-step tests, ported from OrcaSlicer #17026.
#include <catch2/catch.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/calib.hpp"

#include <boost/filesystem/path.hpp>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace Slic3r;

TEST_CASE("Ironing flow marks use the smallest step that keeps them 10 mm apart", "[Calib]")
{
    // min flow, max flow, band length, expected step (0 when even 20% marks are closer than 10 mm)
    const std::vector<std::tuple<double, double, double, double>> cases = {
        {10., 60., 100., 5.},
        {0., 10., 100., 1.},
        {0., 100., 100., 10.},
        {0., 100., 50., 20.},
        {0., 100., 40., 0.},
    };
    for (const auto &[min_flow, max_flow, length, step] : cases) {
        CAPTURE(min_flow, max_flow, length);
        CHECK(ironing_flow_calib_mark_step(min_flow, max_flow, length) == Approx(step));
    }
}

TEST_CASE("Ironing flow test mesh is a closed plate with two bands and side strips", "[Calib]")
{
    // The text needs the font in resources/fonts.
    const std::string previous = resources_dir();
    set_resources_dir((boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources").string());

    const double length = 100.;
    const double top_z  = 0.68;
    const indexed_triangle_set its = ironing_flow_calib_mesh(10., 60., length, "Generic PLA", 0.4, 0.32, 0.56, top_z);
    set_resources_dir(previous);

    const BoundingBoxf3 bb = bounding_box(its);
    // The band plus 5 mm at each end.
    CHECK(bb.size().x() == Approx(length + 10.).margin(1e-3));
    CHECK(bb.min.z() == Approx(0.).margin(1e-6));
    CHECK(bb.max.z() == Approx(top_z).margin(1e-6));
    // The marks and the name lie in front of the band.
    CHECK(bb.min.y() < -10.);
    // Two 15 mm bands plus a 4 mm valley sit behind y = 0.
    CHECK(bb.max.y() == Approx(34.).margin(1e-3));
    CHECK(its_num_open_edges(its) == 0);
}
