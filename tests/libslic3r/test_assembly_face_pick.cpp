#include <catch2/catch.hpp>

#include <libslic3r/Measure.hpp>
#include <libslic3r/TriangleMesh.hpp>

using namespace Slic3r;
using namespace Slic3r::Measure;

// Assembly gizmo "Face and face" mode: hovering the middle of a flat face must yield a Plane.
// Regression guard for the Ultra view-scaled snap radius, which let a border Edge (or a corner
// vertex) win over the face for an ordinary hover, so the Face/Face hover filter (which accepts
// only Plane and Circle) discarded every pick and nothing highlighted.

namespace {

struct TopFace { size_t facet_idx; Vec3d centre; };

// A facet on the +Z face of an axis-aligned cube, plus the centre of that whole face.
static TopFace top_face_of(const indexed_triangle_set &its, double size)
{
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const auto &t = its.indices[i];
        const Vec3d a = its.vertices[t[0]].cast<double>();
        const Vec3d b = its.vertices[t[1]].cast<double>();
        const Vec3d c = its.vertices[t[2]].cast<double>();
        const Vec3d n = (b - a).cross(c - a).normalized();
        if (n.z() > 0.99 && std::abs(a.z() - size) < 1e-6)
            return TopFace{ i, Vec3d(size / 2.0, size / 2.0, size) };
    }
    return TopFace{ size_t(-1), Vec3d::Zero() };
}

} // namespace

TEST_CASE("Measure: hovering the centre of a flat face returns that Plane", "[Measure][Assembly]")
{
    const double         size = 20.0;
    indexed_triangle_set its  = its_make_cube(size, size, size);
    const TopFace        top  = top_face_of(its, size);
    REQUIRE(top.facet_idx != size_t(-1));

    Measuring m(its);

    SECTION("legacy fixed hover limit") {
        auto f = m.get_feature(top.facet_idx, top.centre, Transform3d::Identity(), false, -1.0, 0);
        REQUIRE(f.has_value());
        REQUIRE(f->get_type() == SurfaceFeatureType::Plane);
        CHECK(std::get<1>(f->get_plane()).z() == Approx(1.0).margin(1e-6));
    }

    SECTION("view-scaled snap radius, from a tight zoom-in to a wide zoom-out") {
        for (double snap : { 0.2, 0.5, 1.0, 5.0, 12.0, 30.0 }) {
            INFO("snap_radius = " << snap);
            auto f = m.get_feature(top.facet_idx, top.centre, Transform3d::Identity(), false, snap, 0);
            REQUIRE(f.has_value());
            REQUIRE(f->get_type() == SurfaceFeatureType::Plane);
            CHECK(std::get<1>(f->get_plane()).z() == Approx(1.0).margin(1e-6));
        }
    }

    SECTION("a hover genuinely on a corner still snaps to that vertex") {
        const auto &t      = its.indices[top.facet_idx];
        const Vec3d corner = its.vertices[t[0]].cast<double>();
        auto        f      = m.get_feature(top.facet_idx, corner, Transform3d::Identity(), false, 0.5, 0);
        REQUIRE(f.has_value());
        CHECK(f->get_type() == SurfaceFeatureType::Point);
    }

    SECTION("a hover near a border edge still snaps to that Edge") {
        // 0.1 mm inside the +Y border of the top face, far from both of its corners
        const Vec3d near_edge(size / 2.0, size - 0.1, size);
        auto        f = m.get_feature(top.facet_idx, near_edge, Transform3d::Identity(), false, 0.5, 0);
        REQUIRE(f.has_value());
        CHECK(f->get_type() == SurfaceFeatureType::Edge);
    }

    SECTION("Triangle pick (pick_kind 1) is unchanged") {
        auto f = m.get_feature(top.facet_idx, top.centre, Transform3d::Identity(), false, 0.5, 1);
        REQUIRE(f.has_value());
        REQUIRE(f->get_type() == SurfaceFeatureType::Triangle);
        CHECK(f->get_patch().first.z() == Approx(1.0).margin(1e-6));
        REQUIRE(f->plane_indices != nullptr);
        CHECK(f->plane_indices->size() == 1);
    }

    SECTION("Curve pick (pick_kind 2) is unchanged") {
        auto f = m.get_feature(top.facet_idx, top.centre, Transform3d::Identity(), false, 0.5, 2);
        REQUIRE(f.has_value());
        REQUIRE(f->get_type() == SurfaceFeatureType::Curve);
        CHECK(f->get_patch().first.z() == Approx(1.0).margin(1e-6));
        REQUIRE(f->plane_indices != nullptr);
        CHECK(f->plane_indices->size() >= 2);
    }
}
