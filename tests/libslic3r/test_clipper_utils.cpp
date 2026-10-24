#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <random>
#include <boost/filesystem.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/MultiPoint.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/SVG.hpp"

using namespace Slic3r;

SCENARIO("Various Clipper operations - xs/t/11_clipper.t", "[ClipperUtils]") {
	// CCW oriented contour
	Slic3r::Polygon   square{ { 200, 100 }, {200, 200}, {100, 200}, {100, 100} };
	// CW oriented contour
	Slic3r::Polygon   hole_in_square{ { 160, 140 }, { 140, 140 }, { 140, 160 }, { 160, 160 } };
	Slic3r::ExPolygon square_with_hole(square, hole_in_square);
	GIVEN("square_with_hole") {
        WHEN("offset") {
            Polygons result = Slic3r::offset(square_with_hole, 5.f);
            THEN("offset matches") {
                REQUIRE(result == Polygons { 
                    { { 205, 205 }, { 95, 205 }, { 95, 95 }, { 205, 95 }, },
                    { { 155, 145 }, { 145, 145 }, { 145, 155 }, { 155, 155 } } });
            }
        }
        WHEN("offset_ex") {
            ExPolygons result = Slic3r::offset_ex(square_with_hole, 5.f);
            THEN("offset matches") {
                REQUIRE(result == ExPolygons { { 
                    { { 205, 205 }, { 95, 205 }, { 95, 95 }, { 205, 95 }, },
                    { { 145, 145 }, { 145, 155 }, { 155, 155 }, { 155, 145 } } } } );
            }
        }
        WHEN("offset2_ex") {
            ExPolygons result = Slic3r::offset2_ex({ square_with_hole }, 5.f, -2.f);
            THEN("offset matches") {
                REQUIRE(result == ExPolygons { {
                    { { 203, 203 }, { 97, 203 }, { 97, 97 }, { 203, 97 } },
                    { { 143, 143 }, { 143, 157 }, { 157, 157 }, { 157, 143 } } } } );
            }
        }
    }
    GIVEN("square_with_hole 2") {
        Slic3r::ExPolygon square_with_hole(
            { { 20000000, 20000000 }, { 0, 20000000 }, { 0, 0 }, { 20000000, 0 } },
            { { 5000000, 15000000 }, { 15000000, 15000000 }, { 15000000, 5000000 }, { 5000000, 5000000 } });
        WHEN("offset2_ex") {
            Slic3r::ExPolygons result = Slic3r::offset2_ex(ExPolygons { square_with_hole }, -1.f, 1.f);
            THEN("offset matches") {
                REQUIRE(result.size() == 1);
                REQUIRE(square_with_hole.area() == result.front().area());
            }
        }
    }
    GIVEN("square and hole") {
        WHEN("diff_ex") {
            ExPolygons result = Slic3r::diff_ex(Polygons{ square }, Polygons{ hole_in_square });
            THEN("hole is created") {
                REQUIRE(result.size() == 1);
                REQUIRE(square_with_hole.area() == result.front().area());
            }
        }
    }
    GIVEN("polyline") {
        Polyline polyline { { 50, 150 }, { 300, 150 } };
        WHEN("intersection_pl") {
            Polylines result = Slic3r::intersection_pl(Polylines { polyline }, Polygons { square, hole_in_square });
            THEN("correct number of result lines") {
                REQUIRE(result.size() == 2);
            }
            THEN("result lines have correct length") {
                // results are in no particular order
                REQUIRE(result[0].length() == 40);
                REQUIRE(result[1].length() == 40);
            }
        }
        WHEN("diff_pl") {
            Polylines result = Slic3r::diff_pl({ polyline }, Polygons{ square, hole_in_square });
            THEN("correct number of result lines") {
                REQUIRE(result.size() == 3);
            }
            // results are in no particular order
            THEN("the left result line has correct length") {
                REQUIRE(std::count_if(result.begin(), result.end(), [](const Polyline &pl) { return pl.length() == 50; }) == 1);
            }
            THEN("the right result line has correct length") {
                REQUIRE(std::count_if(result.begin(), result.end(), [](const Polyline &pl) { return pl.length() == 100; }) == 1);
            }
            THEN("the central result line has correct length") {
                REQUIRE(std::count_if(result.begin(), result.end(), [](const Polyline &pl) { return pl.length() == 20; }) == 1);
            }
        }
    }
	GIVEN("Clipper bug #96 / Slic3r issue #2028") {
		Slic3r::Polyline subject{
			{ 44735000, 31936670 }, { 55270000, 31936670 }, { 55270000, 25270000 }, { 74730000, 25270000 }, { 74730000, 44730000 }, { 68063296, 44730000 }, { 68063296, 55270000 }, { 74730000, 55270000 },
			{ 74730000, 74730000 }, { 55270000, 74730000 }, { 55270000, 68063296 }, { 44730000, 68063296 }, { 44730000, 74730000 }, { 25270000, 74730000 }, { 25270000, 55270000 }, { 31936670, 55270000 },
			{ 31936670, 44730000 }, { 25270000, 44730000 }, { 25270000, 25270000 }, { 44730000, 25270000 }, { 44730000, 31936670 } };
		Slic3r::Polygon clip { {75200000, 45200000}, {54800000, 45200000}, {54800000, 24800000}, {75200000, 24800000} };
        Slic3r::Polylines result = Slic3r::intersection_pl(subject, Polygons { clip });
		THEN("intersection_pl - result is not empty") {
			REQUIRE(result.size() == 1);
		}
	}
	GIVEN("Clipper bug #122") {
		Slic3r::Polyline subject { { 1975, 1975 }, { 25, 1975 }, { 25, 25 }, { 1975, 25 }, { 1975, 1975 } };
		Slic3r::Polygons clip { { { 2025, 2025 }, { -25, 2025 } , { -25, -25 }, { 2025, -25 } },
								{ { 525, 525 }, { 525, 1475 }, { 1475, 1475 }, { 1475, 525 } } };
		Slic3r::Polylines result = Slic3r::intersection_pl({ subject }, clip);
		THEN("intersection_pl - result is not empty") {
			REQUIRE(result.size() == 1);
			REQUIRE(result.front().points.size() == 5);
		}
	}
	GIVEN("Clipper bug #126") {
		Slic3r::Polyline subject { { 200000, 19799999 }, { 200000, 200000 }, { 24304692, 200000 }, { 15102879, 17506106 }, { 13883200, 19799999 }, { 200000, 19799999 } };
		Slic3r::Polygon clip { { 15257205, 18493894 }, { 14350057, 20200000 }, { -200000, 20200000 }, { -200000, -200000 }, { 25196917, -200000 } };
        Slic3r::Polylines result = Slic3r::intersection_pl(subject, Polygons { clip });
		THEN("intersection_pl - result is not empty") {
			REQUIRE(result.size() == 1);
		}
		THEN("intersection_pl - result has same length as subject polyline") {
			REQUIRE(result.front().length() == Approx(subject.length()));
		}
	}

#if 0
	{
		# Clipper does not preserve polyline orientation
		my $polyline = Slic3r::Polyline->new([50, 150], [300, 150]);
		my $result = Slic3r::Geometry::Clipper::intersection_pl([$polyline], [$square]);
		is scalar(@$result), 1, 'intersection_pl - correct number of result lines';
		is_deeply $result->[0]->pp, [[100, 150], [200, 150]], 'clipped line orientation is preserved';
	}
	{
		# Clipper does not preserve polyline orientation
		my $polyline = Slic3r::Polyline->new([300, 150], [50, 150]);
		my $result = Slic3r::Geometry::Clipper::intersection_pl([$polyline], [$square]);
		is scalar(@$result), 1, 'intersection_pl - correct number of result lines';
		is_deeply $result->[0]->pp, [[200, 150], [100, 150]], 'clipped line orientation is preserved';
	}
	{
		# Disabled until Clipper bug #127 is fixed
		my $subject = [
			Slic3r::Polyline->new([-90000000, -100000000], [-90000000, 100000000]), # vertical
				Slic3r::Polyline->new([-100000000, -10000000], [100000000, -10000000]), # horizontal
				Slic3r::Polyline->new([-100000000, 0], [100000000, 0]), # horizontal
				Slic3r::Polyline->new([-100000000, 10000000], [100000000, 10000000]), # horizontal
		];
		my $clip = Slic3r::Polygon->new(# a circular, convex, polygon
			[99452190, 10452846], [97814760, 20791169], [95105652, 30901699], [91354546, 40673664], [86602540, 50000000],
			[80901699, 58778525], [74314483, 66913061], [66913061, 74314483], [58778525, 80901699], [50000000, 86602540],
			[40673664, 91354546], [30901699, 95105652], [20791169, 97814760], [10452846, 99452190], [0, 100000000],
			[-10452846, 99452190], [-20791169, 97814760], [-30901699, 95105652], [-40673664, 91354546],
			[-50000000, 86602540], [-58778525, 80901699], [-66913061, 74314483], [-74314483, 66913061],
			[-80901699, 58778525], [-86602540, 50000000], [-91354546, 40673664], [-95105652, 30901699],
			[-97814760, 20791169], [-99452190, 10452846], [-100000000, 0], [-99452190, -10452846],
			[-97814760, -20791169], [-95105652, -30901699], [-91354546, -40673664], [-86602540, -50000000],
			[-80901699, -58778525], [-74314483, -66913061], [-66913061, -74314483], [-58778525, -80901699],
			[-50000000, -86602540], [-40673664, -91354546], [-30901699, -95105652], [-20791169, -97814760],
			[-10452846, -99452190], [0, -100000000], [10452846, -99452190], [20791169, -97814760],
			[30901699, -95105652], [40673664, -91354546], [50000000, -86602540], [58778525, -80901699],
			[66913061, -74314483], [74314483, -66913061], [80901699, -58778525], [86602540, -50000000],
			[91354546, -40673664], [95105652, -30901699], [97814760, -20791169], [99452190, -10452846], [100000000, 0]
			);
		my $result = Slic3r::Geometry::Clipper::intersection_pl($subject, [$clip]);
		is scalar(@$result), scalar(@$subject), 'intersection_pl - expected number of polylines';
		is sum(map scalar(@$_), @$result), scalar(@$subject) * 2, 'intersection_pl - expected number of points in polylines';
	}
#endif
}

SCENARIO("Various Clipper operations - t/clipper.t", "[ClipperUtils]") {
    GIVEN("square with hole") {
        // CCW oriented contour
        Slic3r::Polygon   square { { 10, 10 }, { 20, 10 }, { 20, 20 }, { 10, 20 } };
        Slic3r::Polygon   square2 { { 5, 12 }, { 25, 12 }, { 25, 18 }, { 5, 18 } };
        // CW oriented contour
        Slic3r::Polygon   hole_in_square { { 14, 14 }, { 14, 16 }, { 16, 16 }, { 16, 14 } };
        WHEN("intersection_ex with another square") {
            ExPolygons intersection = Slic3r::intersection_ex(Polygons{ square, hole_in_square }, Polygons{ square2 });
            THEN("intersection area matches (hole is preserved)") {
                ExPolygon match({ { 20, 18 }, { 10, 18 }, { 10, 12 }, { 20, 12 } },
                                { { 14, 16 }, { 16, 16 }, { 16, 14 }, { 14, 14 } });
                REQUIRE(intersection.size() == 1);
                REQUIRE(intersection.front().area() == Approx(match.area()));
            }
        }
    }
    GIVEN("square with hole 2") {
        // CCW oriented contour
        Slic3r::Polygon   square { { 0, 0 }, { 40, 0 }, { 40, 40 }, { 0, 40 } };
        Slic3r::Polygon   square2 { { 10, 10 }, { 30, 10 }, { 30, 30 }, { 10, 30 } };
        // CW oriented contour
        Slic3r::Polygon   hole { { 15, 15 }, { 15, 25 }, { 25, 25 }, {25, 15 } };
        WHEN("union_ex with another square") {
            ExPolygons union_ = Slic3r::union_ex({ square, square2, hole });
            THEN("union of two ccw and one cw is a contour with no holes") {
                REQUIRE(union_.size() == 1);
                REQUIRE(union_.front() == ExPolygon { { 40, 40 }, { 0, 40 }, { 0, 0 }, { 40, 0 } } );
            }
        }
        WHEN("diff_ex with another square") {
			ExPolygons diff = Slic3r::diff_ex(Polygons{ square, square2 }, Polygons{ hole });
            THEN("difference of a cw from two ccw is a contour with one hole") {
                REQUIRE(diff.size() == 1);
                REQUIRE(diff.front().area() == Approx(ExPolygon({ {40, 40}, {0, 40}, {0, 0}, {40, 0} }, { {15, 25}, {25, 25}, {25, 15}, {15, 15} }).area()));
            }
        }
    }
    GIVEN("yet another square") {
        Slic3r::Polygon  square { { 10, 10 }, { 20, 10 }, { 20, 20 }, { 10, 20 } };
        Slic3r::Polyline square_pl = square.split_at_first_point();
        WHEN("no-op diff_pl") {
            Slic3r::Polylines res = Slic3r::diff_pl({ square_pl }, Polygons{});
            THEN("returns the right number of polylines") {
                REQUIRE(res.size() == 1);
            }
            THEN("returns the unmodified input polyline") {
                REQUIRE(res.front().points.size() == square_pl.points.size());
            }
        }
    }
}

template<e_ordering o = e_ordering::OFF, class P, class Tree, class Alloc> 
double polytree_area(const Tree &tree, std::vector<P, Alloc> *out)
{
    traverse_pt<o>(tree, out);
    
    return std::accumulate(out->begin(), out->end(), 0.0,
                           [](double a, const P &p) { return a + p.area(); });
}

size_t count_polys(const ExPolygons& expolys)
{
    size_t c = 0;
    for (auto &ep : expolys) c += ep.holes.size() + 1;
    
    return c;
}

TEST_CASE("Traversing Clipper PolyTree", "[ClipperUtils]") {
    // Create a polygon representing unit box
    Polygon unitbox;
    const auto UNIT = coord_t(1. / SCALING_FACTOR);
    unitbox.points = { Vec2crd{0, 0}, Vec2crd{UNIT, 0}, Vec2crd{UNIT, UNIT}, Vec2crd{0, UNIT}};
    
    Polygon box_frame = unitbox;
    box_frame.scale(20, 10);
    
    Polygon hole_left = unitbox;
    hole_left.scale(8);
    hole_left.translate(UNIT, UNIT);
    hole_left.reverse();
    
    Polygon hole_right = hole_left;
    hole_right.translate(UNIT * 10, 0);
    
    Polygon inner_left = unitbox;
    inner_left.scale(4);
    inner_left.translate(UNIT * 3, UNIT * 3);
    
    Polygon inner_right = inner_left;
    inner_right.translate(UNIT * 10, 0);
    
    Polygons reference = union_({box_frame, hole_left, hole_right, inner_left, inner_right});
    
    ClipperLib::PolyTree tree = union_pt(reference);
    double area_sum = box_frame.area() + hole_left.area() +
                      hole_right.area() + inner_left.area() +
                      inner_right.area();
    
    REQUIRE(area_sum > 0);

    SECTION("Traverse into Polygons WITHOUT spatial ordering") {
        Polygons output;
        REQUIRE(area_sum == Approx(polytree_area(tree.GetFirst(), &output)));
        REQUIRE(output.size() == reference.size());
    }
    
    SECTION("Traverse into ExPolygons WITHOUT spatial ordering") {
        ExPolygons output;
        REQUIRE(area_sum == Approx(polytree_area(tree.GetFirst(), &output)));
        REQUIRE(count_polys(output) == reference.size());
    }
    
    SECTION("Traverse into Polygons WITH spatial ordering") {
        Polygons output;
        REQUIRE(area_sum == Approx(polytree_area<e_ordering::ON>(tree.GetFirst(), &output)));
        REQUIRE(output.size() == reference.size());
    }
    
    SECTION("Traverse into ExPolygons WITH spatial ordering") {
        ExPolygons output;
        REQUIRE(area_sum == Approx(polytree_area<e_ordering::ON>(tree.GetFirst(), &output)));
        REQUIRE(count_polys(output) == reference.size());
    }
}

TEST_CASE("Subtracting a far expansion gives the same polygons as diff with expand", "[ClipperUtils]") {
    // As bridge_over_infill uses it: the lower layer's solids, each shrunk by the solid infill spacing on its own, expanded by
    // two or four spacings.
    const float            spacing = scaled<float>(0.45);
    const float            delta   = GENERATE(2.f, 4.f) * spacing;
    const auto             expanded_diff = [delta](const Polygons &subject, const Polygons &clip) { return diff(subject, expand(clip, delta)); };
    // Whether the smaller round expansion that diff_expanded tries first covers the subject on its own.
    const auto             coarse_covers = [delta](const Polygons &subject, const Polygons &coarse) {
        return diff(subject, expand(coarse, 0.7f * delta, jtRound, 0.01 * delta)).empty();
    };
    const auto rectangle = [](double x0, double y0, double x1, double y1) {
        return Polygon::new_scale({ { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } });
    };
    // A rectangle whose sides are sawteeth 12 µm deep, as the slices of a relief are.
    const auto sawtooth = [](double x0, double y0, double x1, double y1) {
        const Vec2d corners[] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
        Polygon     out;
        for (size_t i = 0; i < 4; ++i) {
            const Vec2d a = corners[i], b = corners[(i + 1) % 4], outward = Vec2d((b - a).y(), (a - b).x()).normalized();
            const int   teeth = int(std::round((b - a).norm() / 0.024));
            for (int t = 0; t < teeth; ++t) {
                out.points.emplace_back(Point::new_scale(a + (b - a) * (t / double(teeth))));
                out.points.emplace_back(Point::new_scale(a + (b - a) * ((t + 0.5) / teeth) + 0.012 * outward));
            }
        }
        return out;
    };

    SECTION("A crack between solids shrunk apart is covered") {
        const Polygons solids = shrink(Polygons{ rectangle(0., 0., 10., 10.), rectangle(10., 0., 20., 10.) }, spacing);
        const Polygons subject{ rectangle(2., 2., 18., 8.) };
        REQUIRE_FALSE(diff(subject, solids).empty());
        CHECK(coarse_covers(subject, polygons_simplify(solids, delta / 8.)));
        CHECK(expanded_diff(subject, solids).empty());
        CHECK(diff_expanded(subject, solids, delta).empty());
    }
    SECTION("A crack between detailed solids shrunk apart is covered") {
        const Polygons solids = shrink(Polygons{ sawtooth(0., 0., 10., 10.), sawtooth(10., 0., 20., 10.) }, spacing);
        const Polygons subject{ rectangle(2., 2., 18., 8.) };
        REQUIRE_FALSE(diff(subject, solids).empty());
        // The shrink's miters cut steep teeth about 1.4 spacings deep, which widens the crack to about what the coarse
        // expansion reaches at two spacings. So only the default reach, four spacings, is sure to take the early-out.
        if (delta > 3.f * spacing)
            CHECK(coarse_covers(subject, polygons_simplify(solids, delta / 8.)));
        CHECK(expanded_diff(subject, solids).empty());
        CHECK(diff_expanded(subject, solids, delta).empty());
    }
    SECTION("Strips too thin to survive the shrink leave the subject unsupported") {
        const Polygons solids = shrink(Polygons{ rectangle(0., 0., 10., 10.), rectangle(10., 0., 10.6, 10.), rectangle(11.2, 0., 11.8, 10.),
                                                 rectangle(12.4, 0., 13., 10.) },
                                       spacing);
        const Polygons subject{ rectangle(1., 1., 14., 9.) };
        const Polygons expected = expanded_diff(subject, solids);
        REQUIRE_FALSE(expected.empty());
        CHECK(diff_expanded(subject, solids, delta) == expected);
    }
    SECTION("A hole wider than the expansion leaves the subject over it") {
        Polygon hole = rectangle(5., 5., 15., 15.);
        hole.reverse();
        const Polygons solids{ rectangle(0., 0., 20., 20.), hole };
        const Polygons subject{ rectangle(2., 2., 18., 18.) };
        const Polygons expected = expanded_diff(subject, solids);
        REQUIRE_FALSE(expected.empty());
        CHECK(diff_expanded(subject, solids, delta) == expected);
    }
    SECTION("A hole that simplification removes is covered") {
        Polygon hole = rectangle(5., 9.975, 10., 10.025);
        hole.reverse();
        const Polygons solids{ rectangle(0., 0., 20., 20.), hole };
        const Polygons subject{ rectangle(2., 2., 18., 18.) };
        REQUIRE(polygons_simplify({ hole }, delta / 8.).empty());
        CHECK(expanded_diff(subject, solids).empty());
        CHECK(diff_expanded(subject, solids, delta).empty());
    }
    SECTION("A thin C that simplification would turn into a hole is kept as it is") {
        // A C 100 µm wide over a 10 mm radius at a tolerance of 0.5 mm, scaled with the tolerance. Arcs of 3,200 and 2,500
        // points leave 0.05 radians open at each end, and the path starts 4,500 points in.
        const double scale = delta / 8. / scaled<double>(0.5);
        const auto   arc   = [](double radius, int n, Points &out) {
            for (int i = 0; i < n; ++i) {
                const double a = 0.05 + (2. * PI - 0.1) * i / (n - 1);
                out.emplace_back(coord_t(std::round(radius * std::cos(a))), coord_t(std::round(radius * std::sin(a))));
            }
        };
        Points outer, inner;
        arc(std::round(scale * scaled<double>(10.)) + std::round(scale * scaled<double>(0.1)), 3200, outer);
        arc(std::round(scale * scaled<double>(10.)), 2500, inner);
        Polygon c(std::move(outer));
        c.points.insert(c.points.end(), inner.rbegin(), inner.rend());
        std::rotate(c.points.begin(), c.points.begin() + 4500, c.points.end());
        REQUIRE(c.is_counter_clockwise());
        REQUIRE(area(polygons_simplify({ c }, delta / 8.)) < 0.);
        // A square beside the C, halfway round, which its own round expansion covers.
        const coord_t  x = coord_t(-scale * scaled<double>(10.05)), half = coord_t(scale * scaled<double>(0.2));
        const Polygons subject{ Polygon({ { x - half, -half }, { x + half, -half }, { x + half, half }, { x - half, half } }) };
        CHECK(coarse_covers(subject, { c }));
        CHECK(expanded_diff(subject, { c }).empty());
        CHECK(diff_expanded(subject, { c }, delta).empty());
    }
    SECTION("A path that simplifies into several contours and holes keeps its sign") {
        // A band 1 mm wide winding once and a radian more, its end 20 µm outside its start, then on as a strand 40 µm wide for
        // two radians. Simplified, the end crosses the start, closing a ring around holes, and the strand's sides cross each other.
        const double radius = scaled<double>(5.), width = scaled<double>(1.), pitch = width + scaled<double>(0.02), band_end = 2. * PI + 1.,
                     end = band_end + 2.;
        const int    n      = 6000;
        Points       outer, inner;
        for (int i = 0; i < n; ++i) {
            const double t = end * i / (n - 1), centre = radius + pitch * t / (2. * PI), half = t <= band_end ? width / 2. : scaled<double>(0.02);
            outer.emplace_back(coord_t(std::round((centre + half) * std::cos(t))), coord_t(std::round((centre + half) * std::sin(t))));
            inner.emplace_back(coord_t(std::round((centre - half) * std::cos(t))), coord_t(std::round((centre - half) * std::sin(t))));
        }
        Polygon band(std::move(outer));
        band.points.insert(band.points.end(), inner.rbegin(), inner.rend());
        REQUIRE(band.is_counter_clockwise());
        Points path = MultiPoint::_douglas_peucker(to_polyline(band).points, delta / 8.);
        path.pop_back();
        const Polygons simplified = polygons_simplify({ band }, delta / 8.);
        const size_t   contours   = size_t(std::count_if(simplified.begin(), simplified.end(), [](const Polygon &p) { return p.is_counter_clockwise(); }));
        CHECK(contours >= 2);
        CHECK(simplified.size() - contours >= 1);
        CHECK((area(simplified) > 0.) == (Polygon(path).area() > 0.));
        const Polygons subject{ rectangle(-8., -8., 8., 8.) };
        CHECK(diff_expanded(subject, { band }, delta) == expanded_diff(subject, { band }));
    }
    SECTION("Random solids give the same polygons") {
        // Overlapping rectangles, some too thin to survive the shrink, and jittered rings, under the region they cover and on
        // every other seed a rectangle more.
        size_t empty = 0, unsupported = 0;
        for (unsigned seed = 1; seed <= 24; ++seed) {
            std::mt19937 rng(seed);
            const auto   uniform = [&rng](double from, double to) { return from + (to - from) * double(rng()) / double(std::mt19937::max()); };
            Polygons     solids;
            for (int i = 0; i < 6; ++i) {
                const double x = uniform(0., 40.), y = uniform(0., 40.);
                solids.emplace_back(rectangle(x, y, x + uniform(0.3, 12.), y + uniform(0.3, 12.)));
            }
            for (int i = 0; i < 2; ++i) {
                const Vec2d  centre(uniform(5., 45.), uniform(5., 45.));
                const double r = uniform(2., 8.), r_hole = r * uniform(0.3, 0.7);
                Polygon      ring, hole;
                for (int k = 0; k < 96; ++k) {
                    const Vec2d direction(std::cos(2. * PI * k / 96), std::sin(2. * PI * k / 96));
                    ring.points.emplace_back(Point::new_scale(centre + r * uniform(0.97, 1.03) * direction));
                    hole.points.emplace_back(Point::new_scale(centre + r_hole * uniform(0.97, 1.03) * direction));
                }
                hole.reverse();
                solids.emplace_back(std::move(ring));
                solids.emplace_back(std::move(hole));
            }
            Polygons covered = solids;
            if (seed % 2 == 0) {
                const double x = uniform(0., 40.), y = uniform(0., 40.);
                covered.emplace_back(rectangle(x, y, x + uniform(2., 10.), y + uniform(2., 10.)));
            }
            const Polygons subject  = shrink(union_(covered), spacing);
            const Polygons shrunk   = shrink(solids, spacing);
            const Polygons expected = expanded_diff(subject, shrunk);
            CHECK(diff_expanded(subject, shrunk, delta) == expected);
            ++(expected.empty() ? empty : unsupported);
        }
        // The seeds give both outcomes: subjects the solids support entirely, and subjects with unsupported parts left.
        CHECK(empty > 0);
        CHECK(unsupported > 0);
    }
}
